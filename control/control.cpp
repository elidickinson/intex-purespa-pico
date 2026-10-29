// PureSpa SB-HS10-1 controller for the Pico W: decodes the panel bus, serves a web page and an
// HTTP API at http://spa.local/, and runs a weekly schedule. Over-the-air updates via
// `uvx platformio run -e ota -t upload`.
//
// Read-only build: bus.pio cannot drive any pin. Requests run through the controller as usual,
// but its presses are only logged ("would press ..."), so they fail with "no beep".
//
// Pins (hottub build-plan.md): GP3 = DATA, GP4 = CLOCK, GP5 = HOLD. The spa's panel 5 V feeds
// VSYS; never connect USB while attached to the spa.
//
// HTTP API, all with basic auth (user spa, password WEB_PASS):
//   GET  /status           JSON spa state
//   POST /set?heater=on    queue requests: power, heater, bubbles = on|off; temp = degrees as
//                          shown; salt = off|3|5|8; press = a button name (test only)
//   GET  /schedule         schedule text; POST it back (Content-Type: text/plain) to replace it
//   GET  /log              recent events, newest first

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <lwip/apps/sntp.h>
#include <time.h>

#include <string>
#include <vector>

#include "bus.pio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "page.h"
#include "schedule.h"
#include "spa.h"

static_assert(sizeof(WIFI_SSID) > 1 && sizeof(WIFI_PASS) > 1 && sizeof(WEB_PASS) > 1,
              "run `source wifi.env` before building; see platformio.ini");

static constexpr bool READ_ONLY = true;
static const char *const HOSTNAME = "spa";
static const char *const TIMEZONE = "EST5EDT,M3.2.0,M11.1.0";  // New York
static const char *const SCHEDULE_FILE = "/schedule.txt";

static const unsigned DATA_PIN = 3;  // CLOCK and HOLD follow on GP4 and GP5

static const PIO pio = pio0;
static uint sm;

// ~0.6 s of frames, so they survive a slow loop (a web request or a flash write)
static constexpr uint32_t RING_SIZE = 2048;
static uint16_t ring[RING_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static volatile uint32_t dropped = 0;

static void __not_in_flash_func(irq_handler)() {
  // Clear first, so a frame pushed while draining raises the IRQ again
  pio_interrupt_clear(pio, 0);
  while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
    uint32_t raw = pio_sm_get(pio, sm);
    uint32_t h = head;
    uint32_t next = (h + 1) % RING_SIZE;
    if (next == tail) {
      dropped++;
      continue;
    }
    ring[h] = ~raw & 0xFFFF;
    head = next;
  }
}

static void start_bus() {
  uint offset = pio_add_program(pio, &frames_program);
  sm = pio_claim_unused_sm(pio, true);
  pio_sm_config c = frames_program_get_default_config(offset);
  sm_config_set_in_pins(&c, DATA_PIN);
  sm_config_set_in_shift(&c, false, false, 32);
  // RX-only: joining disables TX, which this program never uses
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&c, clock_get_hz(clk_sys) / 32e6f);  // bus.pio assumes 32 MHz

  // Direction first: once the pads belong to PIO, their output enable must already read input
  pio_sm_set_consecutive_pindirs(pio, sm, DATA_PIN, 3, false);
  for (unsigned pin = DATA_PIN; pin < DATA_PIN + 3; pin++)
    pio_gpio_init(pio, pin);

  pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
  irq_set_exclusive_handler(PIO0_IRQ_0, irq_handler);
  irq_set_enabled(PIO0_IRQ_0, true);
  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_enabled(pio, sm, true);
}

static spa::Decoder decoder;
static spa::Controller controller(decoder);
static std::vector<spa::Entry> schedule;
static WebServer server(80);

static uint32_t frames = 0;
static uint32_t stalls = 0;  // times the PIO FIFO filled and frames were missed
static uint32_t last_displays = 0;
static uint32_t last_display_ms = 0;

static bool local_time(tm &out) {
  time_t t = time(nullptr);
  if (t < 1700000000)
    return false;  // not synced yet
  localtime_r(&t, &out);
  return true;
}

static constexpr int LOG_LINES = 200;
static std::string log_lines[LOG_LINES];
static int log_next = 0;

static void log(const std::string &msg) {
  char stamp[24];
  tm t;
  if (local_time(t))
    strftime(stamp, sizeof(stamp), "%a %H:%M:%S", &t);
  else
    snprintf(stamp, sizeof(stamp), "+%lus", (unsigned long) (millis() / 1000));
  std::string line = std::string(stamp) + "  " + msg;
  Serial.println(line.c_str());
  log_lines[log_next] = line;
  log_next = (log_next + 1) % LOG_LINES;
}

static void queue(const spa::Request &r, const char *source) {
  if (controller.queue(r))
    log(std::string(source) + ": " + spa::describe(r));
  else
    log(std::string(source) + ": " + spa::describe(r) + " dropped, too many requests waiting");
}

static std::string lights(uint16_t leds) {
  static const struct {
    uint16_t bit;
    const char *name;
  } NAMES[] = {{spa::led::POWER, "power"},      {spa::led::HEATING, "heating"}, {spa::led::HEATER_IDLE, "heater idle"},
               {spa::led::BUBBLES, "bubbles"}, {spa::led::FILTER, "filter"},   {spa::led::SALT, "salt"}};
  char hex[8];
  snprintf(hex, sizeof(hex), "%04x", leds);
  std::string out = std::string("lights ") + hex + ":";
  for (const auto &n : NAMES) {
    if (leds & n.bit)
      out += std::string(" ") + n.name;
  }
  return out;
}

// Logs changes, so the log can be compared with what the panel showed
static void log_changes(uint32_t now) {
  static uint16_t leds = 0;
  static std::string error;
  static std::optional<spa::Button> held;
  static uint32_t beeps = 0;
  static std::string display;
  static uint32_t display_at = 0;
  static std::optional<int> target;
  if (decoder.beeps() != beeps) {
    beeps = decoder.beeps();
    log("beep");
  }
  // Every display change while the panel is in use, to show how its menus behave
  if (decoder.display() != display) {
    if (!decoder.settled())
      log("display '" + decoder.display() + "', '" + display + "' lasted " + std::to_string(now - display_at) + " ms");
    display = decoder.display();
    display_at = now;
  }
  if (decoder.target() != target) {
    target = decoder.target();
    log("target " + std::to_string(*target) + decoder.unit());
  }
  if (decoder.leds() != leds) {
    leds = decoder.leds();
    log(lights(leds));
  }
  if (decoder.error() != error) {
    error = decoder.error();
    log(error.empty() ? "error cleared" : "error " + error);
  }
  if (controller.held() != held) {
    held = controller.held();
    if (held && READ_ONLY)
      log(std::string("would press ") + spa::button_name(*held) + " (read-only build)");
  }
  std::string result;
  while (controller.take_result(result))
    log(result);
}

static void run_schedule() {
  static int last_minute = -1;
  tm t;
  if (!local_time(t))
    return;
  int minute = t.tm_hour * 60 + t.tm_min;
  if (minute == last_minute)
    return;
  last_minute = minute;
  for (const auto &entry : schedule) {
    if (due(entry, t.tm_wday, t.tm_hour, t.tm_min)) {
      for (const auto &r : entry.requests)
        queue(r, "schedule");
    }
  }
}

static std::string read_schedule() {
  File f = LittleFS.open(SCHEDULE_FILE, "r");
  if (!f)
    return "";
  std::string text = f.readString().c_str();
  f.close();
  return text;
}

static std::string json_int(std::optional<int> v) { return v ? std::to_string(*v) : "null"; }
static const char *json_bool(bool b) { return b ? "true" : "false"; }

static std::string status_json() {
  char time_text[24] = "not synced";
  tm t;
  if (local_time(t))
    strftime(time_text, sizeof(time_text), "%a %H:%M", &t);
  const auto &job = controller.current();
  char unit[2] = {decoder.unit(), 0};
  std::string s = "{";
  s += std::string("\"time\":\"") + time_text + "\"";
  s += std::string(",\"bus\":") + json_bool(millis() - last_display_ms < 2000);
  s += ",\"display\":\"" + decoder.display() + "\"";
  s += ",\"water\":" + json_int(decoder.water());
  s += ",\"target\":" + json_int(decoder.target());
  s += std::string(",\"unit\":\"") + unit + "\"";
  s += std::string(",\"power\":") + json_bool(decoder.on(spa::led::POWER));
  s += std::string(",\"heater\":") + json_bool(decoder.on(spa::led::HEATING | spa::led::HEATER_IDLE));
  s += std::string(",\"heating\":") + json_bool(decoder.on(spa::led::HEATING));
  s += std::string(",\"bubbles\":") + json_bool(decoder.on(spa::led::BUBBLES));
  s += std::string(",\"filter\":") + json_bool(decoder.on(spa::led::FILTER));
  s += std::string(",\"salt\":") + json_bool(decoder.on(spa::led::SALT));
  s += ",\"hours_left\":" + json_int(decoder.hours_left());
  s += ",\"error\":\"" + decoder.error() + "\"";
  s += ",\"job\":\"" + (job ? spa::describe(*job) : "") + "\"";
  s += ",\"waiting\":" + std::to_string(controller.waiting());
  s += std::string(",\"read_only\":") + json_bool(READ_ONLY);
  s += ",\"frames\":" + std::to_string(frames);
  s += ",\"dropped\":" + std::to_string(dropped + stalls);
  s += ",\"rssi\":" + std::to_string(WiFi.RSSI());
  s += ",\"uptime\":" + std::to_string(millis() / 1000);
  return s + "}";
}

static bool authorized() {
  if (server.authenticate("spa", WEB_PASS))
    return true;
  server.requestAuthentication();
  return false;
}

static void reply(int code, const std::string &text) { server.send(code, "text/plain", (text + "\n").c_str()); }

static void handle_set() {
  if (!authorized())
    return;
  std::vector<spa::Request> requests;
  for (int i = 0; i < server.args(); i++) {
    std::string key = server.argName(i).c_str();
    if (key == "plain")
      continue;  // a request body, not a setting
    spa::Request r;
    std::string error = spa::parse_request(key, server.arg(i).c_str(), r);
    if (!error.empty())
      return reply(400, error);
    requests.push_back(r);
  }
  if (requests.empty())
    return reply(400, "nothing to set, e.g. /set?heater=on");
  if (controller.waiting() + requests.size() > spa::Controller::MAX_QUEUE)
    return reply(503, "busy, too many requests waiting");
  std::string queued = "queued:";
  for (const auto &r : requests) {
    queue(r, "web");
    queued += " " + spa::describe(r);
  }
  reply(202, queued);
}

static void handle_save_schedule() {
  if (!authorized())
    return;
  std::string text = server.arg("plain").c_str();
  std::vector<spa::Entry> entries;
  std::string error = spa::parse_schedule(text, entries);
  if (!error.empty())
    return reply(400, error);
  // Written whole, then renamed over the old file, so a failed write leaves the old one intact
  File f = LittleFS.open("/schedule.tmp", "w");
  bool saved = f && f.write(reinterpret_cast<const uint8_t *>(text.data()), text.size()) == text.size();
  if (f)
    f.close();
  if (!saved || !LittleFS.rename("/schedule.tmp", SCHEDULE_FILE))
    return reply(500, "could not save the schedule to flash");
  schedule = entries;
  log("schedule saved, " + std::to_string(entries.size()) + " entries");
  reply(200, "saved, " + std::to_string(entries.size()) + " entries");
}

static void handle_log() {
  if (!authorized())
    return;
  std::string text;
  for (int i = 1; i <= LOG_LINES; i++) {
    const std::string &line = log_lines[(log_next - i + LOG_LINES) % LOG_LINES];
    if (!line.empty())
      text += line + "\n";
  }
  server.send(200, "text/plain", text.c_str());
}

static void setup_server() {
  server.on("/", HTTP_GET, [] {
    if (authorized())
      server.send(200, "text/html", PAGE);
  });
  server.on("/status", HTTP_GET, [] {
    if (authorized())
      server.send(200, "application/json", status_json().c_str());
  });
  server.on("/set", HTTP_POST, handle_set);
  server.on("/schedule", HTTP_GET, [] {
    if (authorized())
      server.send(200, "text/plain", read_schedule().c_str());
  });
  server.on("/schedule", HTTP_POST, handle_save_schedule);
  server.on("/log", HTTP_GET, handle_log);
}

static void start_network_services() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(WEB_PASS);
  ArduinoOTA.onStart([] { log("firmware update started"); });
  ArduinoOTA.begin();
  server.begin();
}

static void check_wifi(uint32_t now) {
  static bool connected = false;
  static bool services_started = false;
  static uint32_t attempt_at = 0;  // setup() makes the first attempt
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != connected) {
    connected = up;
    log(up ? std::string("WiFi connected: http://spa.local/ or http://") + WiFi.localIP().toString().c_str() + "/"
           : std::string("WiFi lost"));
    if (up && !services_started) {
      services_started = true;
      start_network_services();
    }
  }
  if (!up && now - attempt_at >= 30000) {
    attempt_at = now;
    WiFi.beginNoBlock(WIFI_SSID, WIFI_PASS);
  }
  if (services_started) {
    server.handleClient();
    ArduinoOTA.handle();
  }
}

void setup() {
  Serial.begin(115200);
  start_bus();
  setenv("TZ", TIMEZONE, 1);
  tzset();
  // The core restarts SNTP whenever WiFi connects; lwIP looks these names up itself and keeps
  // retrying. Calling NTP.begin() or sntp_init() while it runs trips an lwIP assert.
  sntp_setservername(0, "pool.ntp.org");
  sntp_setservername(1, "time.nist.gov");

  if (!LittleFS.begin()) {
    log("started, but the flash file system failed: no schedule");
  } else {
    std::string error = spa::parse_schedule(read_schedule(), schedule);
    log(error.empty() ? "started, schedule has " + std::to_string(schedule.size()) + " entries"
                      : "started, schedule not loaded: " + error);
  }

  setup_server();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.beginNoBlock(WIFI_SSID, WIFI_PASS);
}

void loop() {
  uint32_t now = millis();
  uint32_t t = tail;
  while (t != head) {
    decoder.feed(ring[t]);
    t = (t + 1) % RING_SIZE;
    frames++;
  }
  tail = t;
  uint32_t rxstall = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm);
  if (pio->fdebug & rxstall) {
    pio->fdebug = rxstall;
    stalls++;
  }
  if (decoder.displays() != last_displays) {
    last_displays = decoder.displays();
    last_display_ms = now;
  }

  controller.update(now);
  log_changes(now);
  run_schedule();
  check_wifi(now);
}
