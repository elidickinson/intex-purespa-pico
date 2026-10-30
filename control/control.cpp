// PureSpa SB-HS10-1 controller for the Pico W: decodes the panel bus, serves a web page and an
// HTTP API at http://spa.local/, and runs a weekly schedule. Over-the-air updates via
// `uvx platformio run -e ota -t upload`.
//
// Presses: the reply program in bus.pio, on pio1, is the only code that can drive DATA, and only
// low. It answers the poll for controller.held() and stops by itself at the spa's beep; the PIO
// IRQ stops it after MAX_REPLIES answers. Neither depends on the main loop, which a web request
// or an update can stall for seconds.
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

static_assert(sizeof(WIFI_SSID) > 1 && sizeof(WIFI_PASS) > 1 && sizeof(WEB_PASS) > 1 && sizeof(TIMEZONE) > 1,
              "run `source wifi.env` before building; see platformio.ini");

static const char *const HOSTNAME = "spa";
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

// The WiFi driver's program also lives on pio1; with it, reply still fits
static const PIO reply_pio = pio1;
static uint reply_sm;
static constexpr uint32_t NEVER = 0xFFFFFFFF;  // an X no 16-bit frame can equal
// Answers per press at most, even if the spa never beeps; the panel's own presses take 7-13
static constexpr uint32_t MAX_REPLIES = 20;
static volatile uint16_t answering = 0;  // poll the reply program is answering, 0 if none
static volatile uint32_t replies = 0;    // polls answered since it started

static void set_reply_x(uint32_t x) {
  // Only the newest value matters, so drop any the program hasn't taken yet
  pio_sm_clear_fifos(reply_pio, reply_sm);
  pio_sm_put(reply_pio, reply_sm, x);
}

static void __not_in_flash_func(irq_handler)() {
  // Clear first, so a frame pushed while draining raises the IRQ again
  pio_interrupt_clear(pio, 0);
  while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
    uint16_t frame = ~pio_sm_get(pio, sm) & 0xFFFF;
    if (answering) {
      if (!(frame & spa::NO_BEEP)) {
        answering = 0;  // the spa beeps, so the reply program has stopped by itself
      } else if (frame == answering && ++replies >= MAX_REPLIES) {
        answering = 0;
        set_reply_x(NEVER);
      }
    }
    uint32_t h = head;
    uint32_t next = (h + 1) % RING_SIZE;
    if (next == tail) {
      dropped++;
      continue;
    }
    ring[h] = frame;
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
  // Highest, since the handler enforces MAX_REPLIES
  irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
  irq_set_enabled(PIO0_IRQ_0, true);
  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_enabled(pio, sm, true);
}

// Call after start_bus(), which gives DATA to pio0
static void start_reply() {
  uint offset = pio_add_program(reply_pio, &reply_program);
  reply_sm = pio_claim_unused_sm(reply_pio, true);
  pio_sm_config c = reply_program_get_default_config(offset);
  sm_config_set_in_pins(&c, DATA_PIN);
  sm_config_set_set_pins(&c, DATA_PIN, 1);
  sm_config_set_in_shift(&c, false, false, 32);
  sm_config_set_out_shift(&c, true, false, 32);  // right, so `out` takes bit 0 first
  sm_config_set_clkdiv(&c, clock_get_hz(clk_sys) / 32e6f);
  pio_sm_init(reply_pio, reply_sm, offset, &c);
  // Level 0 and input before DATA is handed over; nothing sets the level again
  pio_sm_set_pins_with_mask(reply_pio, reply_sm, 0, 1u << DATA_PIN);
  pio_sm_set_pindirs_with_mask(reply_pio, reply_sm, 0, 1u << DATA_PIN);
  // X resets to 0, the frame that DATA stuck low gives
  pio_sm_exec(reply_pio, reply_sm, pio_encode_mov_not(pio_x, pio_null));
  pio_gpio_init(reply_pio, DATA_PIN);  // DATA's output now comes from pio1; pio0 still reads it
  pio_sm_set_enabled(reply_pio, reply_sm, true);
}

static void start_pressing(spa::Button b) {
  uint16_t poll = uint16_t(b);
  uint32_t save = save_and_disable_interrupts();
  answering = poll;
  replies = 0;
  set_reply_x(~poll & 0xFFFF);  // the program compares raw bus bits
  restore_interrupts(save);
}

static void stop_pressing() {
  uint32_t save = save_and_disable_interrupts();
  answering = 0;
  set_reply_x(NEVER);
  restore_interrupts(save);
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

static std::string lights(uint16_t leds, uint16_t blinking) {
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
      out += std::string(" ") + n.name + (blinking & n.bit ? " (blinking)" : "");
  }
  return out;
}

// Logs changes, so the log can be compared with what the panel showed
static void log_changes(uint32_t now) {
  static uint16_t leds = 0;
  static uint16_t blinking = 0;
  static std::string error;
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
  if (decoder.leds() != leds || decoder.blinking() != blinking) {
    leds = decoder.leds();
    blinking = decoder.blinking();
    log(lights(leds, blinking));
  }
  if (decoder.error() != error) {
    error = decoder.error();
    log(error.empty() ? "error cleared" : "error " + error);
  }
  std::string result;
  while (controller.take_result(result))
    log(result);
}

// Makes the reply program follow controller.held()
static void drive_presses() {
  static std::optional<spa::Button> pressing;
  auto held = controller.held();
  if (held == pressing)
    return;
  if (pressing) {
    uint32_t n = replies;
    stop_pressing();
    log(std::string("let go of ") + spa::button_name(*pressing) + ", " + std::to_string(n) + " polls answered");
  }
  pressing = held;
  if (held) {
    start_pressing(*held);
    log(std::string("pressing ") + spa::button_name(*held));
  }
}

static void run_schedule() {
  static long last_minute = -1;
  tm t;
  if (!local_time(t))
    return;
  // Local minutes only ever count up, so the hour repeated when daylight saving ends runs once
  long minute = ((t.tm_year * 366L + t.tm_yday) * 24 + t.tm_hour) * 60 + t.tm_min;
  if (minute <= last_minute)
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
  s += std::string(",\"heater_suspended\":") + json_bool(decoder.blinking() & spa::led::HEATER_IDLE);
  s += std::string(",\"bubbles\":") + json_bool(decoder.on(spa::led::BUBBLES));
  s += std::string(",\"filter\":") + json_bool(decoder.on(spa::led::FILTER));
  s += std::string(",\"salt\":") + json_bool(decoder.on(spa::led::SALT));
  s += ",\"hours_left\":" + json_int(decoder.hours_left());
  s += ",\"error\":\"" + decoder.error() + "\"";
  s += ",\"job\":\"" + (job ? spa::describe(*job) : "") + "\"";
  s += ",\"waiting\":" + std::to_string(controller.waiting());
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
  // Writing flash masks interrupts, which pauses the MAX_REPLIES limit
  if (controller.held())
    return reply(503, "busy pressing a button, try again");
  // A form-encoded body arrives as arguments, not as "plain", and would save as empty
  if (!server.hasArg("plain") && server.args() > 0)
    return reply(400, "send the schedule as plain text: curl -H 'Content-Type: text/plain' --data-binary @file");
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
    // An update writes flash, which masks interrupts and so pauses the MAX_REPLIES limit
    if (!controller.held())
      ArduinoOTA.handle();
  }
}

void setup() {
  Serial.begin(115200);
  start_bus();
  start_reply();
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
  // After a stall, or when a full ring dropped the newest frames, what was just decoded may be
  // seconds old
  static uint32_t last_dropped = 0;
  static uint32_t last_loop = now;
  if (dropped != last_dropped || now - last_loop > 500) {
    last_dropped = dropped;
    controller.lost_data();
    log("fell behind reading the bus");
  }
  last_loop = now;
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
  drive_presses();
  log_changes(now);
  run_schedule();
  check_wifi(now);
}
