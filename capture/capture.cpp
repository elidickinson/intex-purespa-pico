// Read-only PureSpa bus capture for the Pico W.
//
// Watches the panel bus and logs every raw 16-bit frame with a microsecond
// timestamp to the USB serial console and as batched UDP packets:
// "<us> <frame>". Button-poll frames also get the six words of DATA, CLOCK and
// HOLD samples taken after them (see the sampler in capture.pio), where the
// panel answers a pressed button. A status line every 5 s adds WiFi signal,
// chip temperature and the range of HOLD high and low times since the
// previous status line.
//
// Passivity: capture.pio has no instruction that writes pins or pindirs, and
// no set, out or side-set pins are configured, so DATA is never driven and the
// panel and base behave exactly as if the Pico were not on the bus.
//
// Logging over WiFi is the safe path: a USB cable would tie the laptop ground
// to the spa ground (and feed USB 5 V into the spa rail). If you must use
// serial, unplug the laptop charger and expect Serial.write to block for up
// to 1 s per call when the host is connected but not reading.
//
// Build: see platformio.ini; the build runs pioasm on capture.pio.
// Pins (repo build-plan.md): GP3 = DATA, GP4 = CLOCK, GP5 = HOLD/LATCH.
// The spa's panel 5 V feeds VSYS through a fuse; no USB power needed.

#include <Arduino.h>
#include <pico/time.h>

#include "capture.pio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include <WiFi.h>
#include <WiFiUdp.h>

// LOG_HOST is the computer running `python3 -I listen.py <log>`; unicast, not
// broadcast. Avoid `nc -lu`: it locks onto the first sender and silently
// ignores the Pico after a restart, which sends from a new port.
// A wrong host fails silently and looks like a wiring fault, so the build
// refuses to compile until it is set.
static_assert(sizeof(WIFI_SSID) > 1 && sizeof(WIFI_PASS) > 1 && sizeof(LOG_HOST) > 1,
              "run `source wifi.env` before building; see platformio.ini");
#ifndef LOG_PORT
#define LOG_PORT 1514
#endif

static const unsigned DATA_PIN = 3;
static const unsigned CLK_PIN = 4;
static const unsigned CS_PIN = 5;

static const PIO pio = pio0;
static uint offset;
static uint sm;
static uint sampler_offset;
static uint sampler_sm;
// pio0 has no room left for hold_timing
static const PIO hold_pio = pio1;
static uint hold_offset;
static uint hold_sm;
// HOLD high and low times since the last status line, in 62.5 ns steps
static uint32_t high_min = UINT32_MAX;
static uint32_t high_max = 0;
static uint32_t low_min = UINT32_MAX;
static uint32_t low_max = 0;

static constexpr unsigned SAMPLE_WORDS = 6;

struct Frame {
  uint32_t t;                      // time_us_32() at capture
  uint32_t samples[SAMPLE_WORDS];  // pins after the frame, see the sampler in capture.pio
  uint16_t frame;                  // decoded (inverted) frame
};

// ~1 s of bus traffic at ~2800 frames/s (spa off; ~2 s at ~1600 when on)
static constexpr uint32_t RING_SIZE = 3072;

// Button polls seen on SB-HS10-1, with the beep bit 0x0100 set
static bool is_poll(uint16_t frame) {
  switch (frame | 0x0100) {
    case 0x0102: case 0x0108: case 0x0110: case 0x0140: case 0x0180:
    case 0x0500: case 0x1100: case 0x2100: case 0x8100:
      return true;
  }
  return false;
}
static Frame ring[RING_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static volatile uint32_t dropped = 0;
static volatile uint32_t zero_frames = 0;  // raw 0: DATA low all 16 clocks

// Starts capture and sampler in step at their first instruction, so each frame
// word pairs with the sampler's words for that frame.
static void start_capture() {
  uint32_t mask = (1u << sm) | (1u << sampler_sm);
  pio_set_sm_mask_enabled(pio, mask, false);
  pio_sm_clear_fifos(pio, sm);
  pio_sm_clear_fifos(pio, sampler_sm);
  pio_interrupt_clear(pio, 1);
  pio_restart_sm_mask(pio, mask);
  pio_sm_exec(pio, sm, pio_encode_jmp(offset));
  pio_sm_exec(pio, sampler_sm, pio_encode_jmp(sampler_offset));
  pio_set_sm_mask_enabled(pio, mask, true);
}

static void __not_in_flash_func(irq_handler)() {
  // Clear first, so a frame pushed while draining raises the IRQ again.
  pio_interrupt_clear(pio, 0);
  while (!pio_sm_is_rx_fifo_empty(pio, sm) && pio_sm_get_rx_fifo_level(pio, sampler_sm) >= SAMPLE_WORDS) {
    uint32_t raw = pio_sm_get(pio, sm);
    uint32_t s[SAMPLE_WORDS];
    for (auto &w : s)
      w = pio_sm_get(pio, sampler_sm);
    uint32_t h = head;
    uint32_t next = (h + 1) % RING_SIZE;
    if (next == tail) {
      dropped++;
      continue;
    }
    ring[h].t = time_us_32();
    memcpy(ring[h].samples, s, sizeof(s));
    ring[h].frame = ~raw & 0xFFFF;
    // GOTCHA: raw 0 (DATA stuck low: floating pin, wrong pin, unpowered
    // shifter) is exactly the miswiring clue, so it is counted and logged,
    // never silently dropped.
    if (raw == 0)
      zero_frames++;
    head = next;
  }
  // A push that stalled on a full FIFO may have missed the start of a frame or
  // of the sampler, which would pair later frames with the wrong samples.
  uint32_t rxstall = (1u << (PIO_FDEBUG_RXSTALL_LSB + sm)) | (1u << (PIO_FDEBUG_RXSTALL_LSB + sampler_sm));
  if (pio->fdebug & rxstall) {
    pio->fdebug = rxstall;
    dropped++;
    start_capture();
  }
}

static void widen(uint32_t &lo, uint32_t &hi, uint32_t v) {
  if (v < lo)
    lo = v;
  if (v > hi)
    hi = v;
}

static WiFiUDP udp;
static uint32_t frames = 0;
static uint32_t last_report = 0;
static uint32_t last_flush = 0;
static uint32_t last_wifi_poll = 0;
static bool wifi_ok = false;

static char line[160];
static char pkt[1200];
static size_t pkt_len = 0;

static void send(const char *data, size_t len);
static void flush_udp(uint32_t now);

static void send(const char *data, size_t len) {
  // GOTCHA: with a USB host connected but not reading, Serial.write blocks for up
  // to 1 s per call (arduino-pico SerialUSB), which overflows the ring. Prefer
  // the UDP path; USB is for a bench setup only.
  Serial.write(data, len);
  if (!wifi_ok)
    return;
  // GOTCHA: the bus fills the 1200-byte packet in well under 200 ms, so the
  // buffer must flush on overflow, not only on the timer.
  if (pkt_len + len > sizeof(pkt))
    flush_udp(millis());
  // Lines are bounded by line[160], so len always fits an empty packet.
  memcpy(pkt + pkt_len, data, len);
  pkt_len += len;
}

static void flush_udp(uint32_t now) {
  // Lines are batched into packets sent when full or 200 ms after the last one.
  // One packet per frame would be thousands of packets/s, which the radio
  // cannot sustain.
  if (!wifi_ok || pkt_len == 0)
    return;
  udp.beginPacket(LOG_HOST, LOG_PORT);
  udp.write((const uint8_t *) pkt, pkt_len);
  udp.endPacket();
  pkt_len = 0;
  last_flush = now;
}

static void report(uint32_t now) {
  // HOLD steps are 62.5 ns: x125/2 for ns, /16 for us
  int n = snprintf(line, sizeof(line),
                   "%10lu ms  %8lu frames  %3lu dropped  %4lu zero  %4d dBm  %3ld C  "
                   "HOLD high %lu-%lu ns  low %lu-%lu us\n",
                   (unsigned long) millis(), (unsigned long) frames, (unsigned long) dropped,
                   (unsigned long) zero_frames, (int) WiFi.RSSI(), lround(analogReadTemp()),
                   (unsigned long) (high_max ? high_min * 125 / 2 : 0), (unsigned long) (high_max * 125 / 2),
                   (unsigned long) (low_max ? low_min / 16 : 0), (unsigned long) (low_max / 16));
  high_min = low_min = UINT32_MAX;
  high_max = low_max = 0;
  send(line, n);
  flush_udp(now);
}

static bool wifi_up(uint32_t now) {
  if (wifi_ok)
    return true;
  if (now - last_wifi_poll < 1000)
    return false;
  last_wifi_poll = now;
  return (wifi_ok = WiFi.status() == WL_CONNECTED);
}

void setup() {
  Serial.begin(115200);

  offset = pio_add_program(pio, &capture_program);
  sm = pio_claim_unused_sm(pio, true);
  sampler_offset = pio_add_program(pio, &sampler_program);
  sampler_sm = pio_claim_unused_sm(pio, true);
  hold_offset = pio_add_program(hold_pio, &hold_timing_program);
  hold_sm = pio_claim_unused_sm(hold_pio, true);
  float clkdiv = clock_get_hz(clk_sys) / 32e6f;  // capture.pio timing assumes 32 MHz

  // RX-only: the FIFO joins are safe here because nothing ever writes TX.
  // GOTCHA (from the production component): joining RX disables TX, whose
  // status then reads permanently full - any pio_sm_put_blocking() would hang
  // forever. If this sketch ever needs TX, remove the join first.
  pio_sm_config c = capture_program_get_default_config(offset);
  sm_config_set_in_pins(&c, DATA_PIN);
  sm_config_set_in_shift(&c, false, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&c, clkdiv);

  pio_sm_config s = sampler_program_get_default_config(sampler_offset);
  sm_config_set_in_pins(&s, DATA_PIN);
  sm_config_set_in_shift(&s, false, true, 30);
  sm_config_set_fifo_join(&s, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&s, clkdiv);

  pio_sm_config h = hold_timing_program_get_default_config(hold_offset);
  sm_config_set_in_pins(&h, DATA_PIN);
  sm_config_set_jmp_pin(&h, CS_PIN);
  sm_config_set_fifo_join(&h, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&h, clkdiv);

  // Direction first: once the pads belong to PIO, their output enable must
  // already read input.
  pio_sm_set_consecutive_pindirs(pio, sm, DATA_PIN, 3, false);
  pio_gpio_init(pio, DATA_PIN);
  pio_gpio_init(pio, CLK_PIN);
  pio_gpio_init(pio, CS_PIN);

  pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
  irq_set_exclusive_handler(PIO0_IRQ_0, irq_handler);
  irq_set_enabled(PIO0_IRQ_0, true);

  pio_sm_init(pio, sm, offset, &c);
  pio_sm_init(pio, sampler_sm, sampler_offset, &s);
  pio_sm_init(hold_pio, hold_sm, hold_offset, &h);
  start_capture();
  pio_sm_set_enabled(hold_pio, hold_sm, true);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("\ncapture: listen-only, DATA is never driven\n");
}

void loop() {
  // wifi_up() is polled every loop, not only when frames arrive: with no bus
  // traffic (miswired pins, no latch) frames never drain and the UDP status
  // line would otherwise never start flowing - "miswired" would look like
  // "WiFi down".
  uint32_t now = millis();
  wifi_up(now);

  // Only ranges are reported and each word says whether it is a high or a low
  // time, so overflowing this FIFO between loops is harmless
  while (!pio_sm_is_rx_fifo_empty(hold_pio, hold_sm)) {
    uint32_t w = pio_sm_get(hold_pio, hold_sm);
    if (w >> 31)
      widen(high_min, high_max, ~w);
    else
      widen(low_min, low_max, w);
  }

  uint32_t t = tail;
  while (t != head) {
    const Frame &f = ring[t];
    tail = (t + 1) % RING_SIZE;
    int n = snprintf(line, sizeof(line), "%10lu %04x", (unsigned long) f.t, f.frame);
    if (is_poll(f.frame)) {
      for (uint32_t w : f.samples)
        n += snprintf(line + n, sizeof(line) - n, " %08lx", (unsigned long) w);
    }
    line[n++] = '\n';
    send(line, n);
    frames++;
    t = tail;
  }

  if (now - last_flush > 200)
    flush_udp(now);
  if (now - last_report > 5000) {
    last_report = now;
    report(now);
  }
}
