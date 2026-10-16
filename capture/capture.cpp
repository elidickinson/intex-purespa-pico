// Read-only PureSpa bus capture for the Pico W.
//
// Watches the panel bus and logs every raw 16-bit frame to the USB serial
// console and as UDP packets. The PIO program's match value X is loaded once
// with 0xFFFFFFFF, which can never equal a 16-bit frame, so DATA is never
// driven: the panel and base behave exactly as if the Pico were not on the bus.
//
// Build: pioasm sbh20.pio sbh20.pio.h, then compile with the arduino-pico core
// (PlatformIO: platform = raspberrypi, board = pico_w, framework = arduino).
// Pins (repo build-plan.md): GP3 = DATA, GP4 = CLOCK, GP5 = HOLD/LATCH.
// The spa's panel 5 V feeds VSYS; no USB power needed.

#include <Arduino.h>

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
#ifndef LOG_HOST
#define LOG_HOST "255.255.255.255"  // LAN broadcast; set to your computer's IP to unicast
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

// ~700 ms of bus traffic at ~1500 frames/s
static constexpr uint32_t RING_SIZE = 1024;
static uint16_t ring[RING_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static volatile uint32_t dropped = 0;

static void __not_in_flash_func(irq_handler)() {
  while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
    uint32_t raw = pio_sm_get(pio, sm);
    if (raw == 0)
      continue;  // pushed by set_x; never runs here, but cheap to skip
    uint32_t h = head;
    uint32_t next = (h + 1) % RING_SIZE;
    if (next == tail) {
      dropped++;
      continue;
    }
    ring[h] = ~raw & 0xFFFF;
    head = next;
  }
  uint32_t rxstall = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm);
  if (pio->fdebug & rxstall) {
    pio->fdebug = rxstall;
    dropped++;
  }
  pio_interrupt_clear(pio, 0);
}

// The program replies to a received frame equal to X; set_x loads X from the TX FIFO.
// It is called once in setup(): 0xFFFFFFFF is 32 bits and can never equal a 16-bit
// raw frame, so the reply block never runs and DATA stays an input forever.
static void set_x(uint32_t x) {
  pio_sm_put_blocking(pio, sm, x);
  pio_sm_exec_wait_blocking(pio, sm, pio_encode_jmp(offset + sbh20_offset_set_x));
}

static WiFiUDP udp;
static uint32_t frames = 0;
static uint32_t last_report = 0;
static uint32_t last_blink_at = 0;
static bool wifi_ok = false;
static char line[96];

static void report(uint32_t now) {
  int n = snprintf(line, sizeof(line), "%10lu ms  %8lu frames  %3lu dropped\n", (unsigned long) now,
                   (unsigned long) frames, (unsigned long) dropped);
  Serial.write(line, n);
  if (wifi_ok) {
    udp.beginPacket(LOG_HOST, LOG_PORT);
    udp.write((const uint8_t *) line, n);
    udp.endPacket();
  }
}

static bool wifi_up(uint32_t now) {
  if (wifi_ok)
    return true;
  if (now - last_blink_at < 1000)
    return false;
  last_blink_at = now;
  return (wifi_ok = WiFi.status() == WL_CONNECTED);
}

void setup() {
  Serial.begin(115200);

  offset = pio_add_program(pio, &sbh20_program);
  sm = pio_claim_unused_sm(pio, true);

  pio_sm_config c = sbh20_program_get_default_config(offset);
  sm_config_set_in_pins(&c, DATA_PIN);
  sm_config_set_set_pins(&c, DATA_PIN, 1);
  sm_config_set_in_shift(&c, false, false, 16);
  sm_config_set_out_shift(&c, true, false, 16);

  pio_gpio_init(pio, DATA_PIN);
  pio_gpio_init(pio, CLK_PIN);
  pio_gpio_init(pio, CS_PIN);
  // Inputs only. The program's reply block (set pindirs, 1) can never run
  // because X is never loaded with a frame value.
  pio_sm_set_consecutive_pindirs(pio, sm, DATA_PIN, 3, false);

  pio_sm_clear_fifos(pio, sm);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
  irq_set_exclusive_handler(PIO0_IRQ_0, irq_handler);
  irq_set_enabled(PIO0_IRQ_0, true);
  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_enabled(pio, sm, true);
  set_x(0xFFFFFFFF);  // matches nothing: the PIO never drives DATA

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("\ncapture: waiting for bus frames (X=0, reply never matches)\n");
}

void loop() {
  uint32_t now = millis();
  uint32_t t = tail;
  while (t != head) {
    uint16_t frame = ring[t];
    tail = (t + 1) % RING_SIZE;
    int n = snprintf(line, sizeof(line), "%08lu %04x\n", (unsigned long) frames++, frame);
    Serial.write(line, n);
    wifi_up(now);
    if (wifi_ok) {
      udp.beginPacket(LOG_HOST, LOG_PORT);
      udp.write((const uint8_t *) line, n);
      udp.endPacket();
    }
    t = tail;
  }
  if (now - last_report > 5000) {
    last_report = now;
    report(now);
  }
}
