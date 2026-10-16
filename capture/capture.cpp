// Read-only PureSpa bus capture for the Pico W.
//
// Watches the panel bus and logs every raw 16-bit frame with microsecond
// timestamps to the USB serial console and as batched UDP packets.
//
// Passivity: the PIO program's match register X is armed with `mov x, ~null`
// (0xFFFFFFFF, which can never equal a 16-bit frame) while the state machine
// is still halted, and sm_config_set_set_pins() is not called, so the
// program's `set pindirs, 1` can never reach a pin: DATA is never driven and
// the panel and base behave exactly as if the Pico were not on the bus.
//
// Logging over WiFi is the safe path: a USB cable would tie the laptop ground
// to the spa ground (and feed USB 5 V into the spa rail). If you must use
// serial, unplug the laptop charger and expect Serial.write to block for up
// to 1 s per call when the host is connected but not reading.
//
// Build: pioasm sbh20.pio sbh20.pio.h, then compile with the arduino-pico core
// (PlatformIO: platform = raspberrypi, board = pico_w, framework = arduino).
// Pins (repo build-plan.md): GP3 = DATA, GP4 = CLOCK, GP5 = HOLD/LATCH.
// The spa's panel 5 V feeds VSYS through a fuse; no USB power needed.

#include <Arduino.h>
#include <pico/time.h>

#include "hardware/irq.h"
#include "hardware/pio.h"
#include "sbh20.pio.h"

#include <WiFi.h>
#include <WiFiUdp.h>

#ifndef WIFI_SSID
#define WIFI_SSID "net"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "password"
#endif
// SET THIS to the computer running `nc -lu 1514`; unicast, not broadcast.
// A wrong host fails silently and looks like a wiring fault, so the build
// refuses to compile until this is set.
#ifndef LOG_HOST
#error "Set LOG_HOST to your computer's IP, e.g. -DLOG_HOST=\"192.168.1.100\""
#endif
#ifndef LOG_PORT
#define LOG_PORT 1514
#endif

static const unsigned DATA_PIN = 3;
static const unsigned CLK_PIN = 4;
static const unsigned CS_PIN = 5;

static const PIO pio = pio0;
static uint offset;
static uint sm;

struct Frame {
  uint32_t t;      // time_us_32() at capture
  uint16_t frame;  // decoded (inverted) frame
};

// ~700 ms of bus traffic at ~1500 frames/s
static constexpr uint32_t RING_SIZE = 1024;
static Frame ring[RING_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static volatile uint32_t dropped = 0;
static volatile uint32_t zero_frames = 0;  // raw 0: DATA low all 16 clocks

static void __not_in_flash_func(irq_handler)() {
  while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
    uint32_t raw = pio_sm_get(pio, sm);
    uint32_t h = head;
    uint32_t next = (h + 1) % RING_SIZE;
    if (next == tail) {
      dropped++;
      continue;
    }
    ring[h].t = time_us_32();
    ring[h].frame = ~raw & 0xFFFF;
    // GOTCHA: raw 0 (DATA stuck low: floating pin, wrong pin, unpowered
    // shifter) is exactly the miswiring clue, so it is counted and logged,
    // never silently dropped.
    if (raw == 0)
      zero_frames++;
    head = next;
  }
  uint32_t rxstall = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm);
  if (pio->fdebug & rxstall) {
    pio->fdebug = rxstall;
    dropped++;
  }
  pio_interrupt_clear(pio, 0);
}

static WiFiUDP udp;
static uint32_t frames = 0;
static uint32_t last_report = 0;
static uint32_t last_flush = 0;
static uint32_t last_wifi_poll = 0;
static bool wifi_ok = false;

static char line[64];
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
  // GOTCHA: at ~1500 frames/s a 200 ms flush window fills the 1200-byte packet
  // about 4x over, so the buffer must flush on overflow, not only on the timer.
  if (pkt_len + len > sizeof(pkt))
    flush_udp(millis());
  // Lines are bounded by line[64], so len always fits an empty packet.
  memcpy(pkt + pkt_len, data, len);
  pkt_len += len;
}

static void flush_udp(uint32_t now) {
  // Frames are batched into ~1 KB packets flushed every 200 ms (~5 packets/s).
  // One packet per frame would be ~1500 packets/s of broadcast airtime, which
  // the AP re-sends at basic rate and the radio cannot sustain.
  if (!wifi_ok || pkt_len == 0)
    return;
  udp.beginPacket(LOG_HOST, LOG_PORT);
  udp.write((const uint8_t *) pkt, pkt_len);
  udp.endPacket();
  pkt_len = 0;
  (void) now;
}

static void report(uint32_t now) {
  int n = snprintf(line, sizeof(line), "%10lu ms  %8lu frames  %3lu dropped  %4lu zero\n",
                   (unsigned long) millis(), (unsigned long) frames, (unsigned long) dropped,
                   (unsigned long) zero_frames);
  send(line, n);
  flush_udp(now);
  last_flush = now;
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

  offset = pio_add_program(pio, &sbh20_program);
  sm = pio_claim_unused_sm(pio, true);

  pio_sm_config c = sbh20_program_get_default_config(offset);
  sm_config_set_in_pins(&c, DATA_PIN);
  sm_config_set_in_shift(&c, false, false, 16);
  sm_config_set_out_shift(&c, true, false, 16);
  // No sm_config_set_set_pins(): the default set-pin count is 0, so the
  // program's `set pindirs, 1` can never switch DATA to output. This is the
  // strongest passivity guarantee here; keep it if the program is updated.

  // Direction first: once the pads belong to PIO, their output enable must
  // already read input - the only window where this file could drive a pin.
  pio_sm_set_consecutive_pindirs(pio, sm, DATA_PIN, 3, false);
  pio_gpio_init(pio, DATA_PIN);
  pio_gpio_init(pio, CLK_PIN);
  pio_gpio_init(pio, CS_PIN);

  // RX-only: the FIFO join is safe here because nothing ever writes TX.
  // GOTCHA (from the production component): joining RX disables TX, whose
  // status then reads permanently full - any pio_sm_put_blocking() would hang
  // forever. If this sketch ever needs TX, remove the join first.
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  pio_sm_clear_fifos(pio, sm);
  pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
  irq_set_exclusive_handler(PIO0_IRQ_0, irq_handler);
  irq_set_enabled(PIO0_IRQ_0, true);

  pio_sm_init(pio, sm, offset, &c);
  // Arm X while the SM is halted: X = ~null = 0xFFFFFFFF, which no 16-bit
  // frame equals, so the reply block never runs and DATA stays an input.
  // GOTCHA: X's reset value is 0 and a raw 0 (DATA low all 16 clocks, e.g.
  // miswired or floating DATA) would match it; SM restart does NOT clear X.
  pio_sm_exec(pio, sm, pio_encode_mov_not(pio_x, pio_null));
  pio_sm_set_enabled(pio, sm, true);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("\ncapture: X=0xffffffff, set-pins=0, DATA is never driven\n");
}

void loop() {
  // wifi_up() is polled every loop, not only when frames arrive: with no bus
  // traffic (miswired pins, no latch) frames never drain and the UDP status
  // line would otherwise never start flowing - "miswired" would look like
  // "WiFi down".
  uint32_t now = millis();
  wifi_up(now);

  uint32_t t = tail;
  while (t != head) {
    const Frame &f = ring[t];
    tail = (t + 1) % RING_SIZE;
    int n = snprintf(line, sizeof(line), "%10lu %04x\n", (unsigned long) f.t, f.frame);
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
