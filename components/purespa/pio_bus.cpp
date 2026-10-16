#include "pio_bus.h"

#include <atomic>

#include "hardware/irq.h"
#include "hardware/pio.h"

namespace esphome {
namespace purespa {

#include "sbh20.pio.h"

// #define DEBUG_LOW_LEVEL_SPA
#define DEBUG_PIN 6

static const PIO pio = pio0;
static uint offset;
static uint sm;

// ~700 ms of bus traffic, so frames survive a slow main loop iteration
static constexpr uint32_t RING_SIZE = 1024;
static uint16_t ring[RING_SIZE];
static std::atomic<uint32_t> head{0};  // written by the IRQ handler only
static std::atomic<uint32_t> tail{0};  // written by read() only
static std::atomic<uint32_t> dropped_frames{0};

static void __not_in_flash_func(irq_handler)() {
  while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
    uint32_t raw = pio_sm_get(pio, sm);
    if (raw == 0)
      continue;  // pushed by set_x when the reply code changes
    uint32_t h = head.load(std::memory_order_relaxed);
    uint32_t next = (h + 1) % RING_SIZE;
    if (next == tail.load(std::memory_order_acquire)) {
      dropped_frames.store(dropped_frames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      continue;
    }
    ring[h] = ~raw & 0xFFFF;
    head.store(next, std::memory_order_release);
  }
  // RXSTALL is write-one-to-clear and covers all pushes blocked since the last clear
  uint32_t rxstall = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm);
  if (pio->fdebug & rxstall) {
    pio->fdebug = rxstall;
    dropped_frames.fetch_add(1, std::memory_order_relaxed);
  }
  pio_interrupt_clear(pio, 0);
}

// The program replies to a received frame equal to X; set_x loads X from the TX FIFO
static void set_x(uint32_t x) {
  pio_sm_put_blocking(pio, sm, x);
  pio_sm_exec_wait_blocking(pio, sm, pio_encode_jmp(offset + sbh20_offset_set_x));
}

void PioBus::setup(unsigned base_pin) {
  uint data_pin = base_pin;
  uint clk_pin = base_pin + 1;
  uint cs_pin = base_pin + 2;

  offset = pio_add_program(pio, &sbh20_program);
  sm = pio_claim_unused_sm(pio, true);

  pio_sm_config c = sbh20_program_get_default_config(offset);
  sm_config_set_in_pins(&c, data_pin);
  sm_config_set_set_pins(&c, data_pin, 1);
  sm_config_set_in_shift(&c, false, false, 16);
  sm_config_set_out_shift(&c, true, false, 16);

  pio_gpio_init(pio, data_pin);
  pio_gpio_init(pio, clk_pin);
  pio_gpio_init(pio, cs_pin);

  gpio_put(data_pin, false);

  pio_sm_set_consecutive_pindirs(pio, sm, data_pin, 3, false);
  pio_sm_clear_fifos(pio, sm);
  // No FIFO join: joining RX would disable TX, and hold()/release() need it.
  // Burst tolerance comes from the 1024-frame ring drained by the IRQ.

  pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
  irq_set_exclusive_handler(PIO0_IRQ_0, irq_handler);
  irq_set_enabled(PIO0_IRQ_0, true);

#ifdef DEBUG_LOW_LEVEL_SPA
  gpio_init(DEBUG_PIN);
  gpio_set_dir(DEBUG_PIN, true);
  pio_gpio_init(pio, DEBUG_PIN);
  gpio_put(DEBUG_PIN, false);
  sm_config_set_sideset_pins(&c, DEBUG_PIN);
  pio_sm_set_consecutive_pindirs(pio, sm, DEBUG_PIN, 1, true);
#endif

  pio_sm_init(pio, sm, offset, &c);
  release();  // arm X before the SM runs: its reset value 0 must never match a frame
  pio_sm_set_enabled(pio, sm, true);
}

bool PioBus::read(uint16_t &frame) {
  uint32_t t = tail.load(std::memory_order_relaxed);
  if (t == head.load(std::memory_order_acquire))
    return false;
  frame = ring[t];
  tail.store((t + 1) % RING_SIZE, std::memory_order_release);
  return true;
}

void PioBus::hold(uint16_t code) { set_x((uint16_t) ~code); }

// X is compared with a 16-bit frame, so 0xFFFFFFFF never matches
void PioBus::release() { set_x(0xFFFFFFFF); }

uint32_t PioBus::dropped() const { return dropped_frames.load(std::memory_order_relaxed); }

}  // namespace purespa
}  // namespace esphome
