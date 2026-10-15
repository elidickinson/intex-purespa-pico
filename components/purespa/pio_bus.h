#pragma once

#include <cstdint>

namespace esphome {
namespace purespa {

/**
 * Receives frames and replies to button frames using the sbh20 PIO program on pio0.
 * DATA, CLK and CS must be on consecutive pins starting at base_pin.
 *
 * The IRQ handler only copies frames into a ring buffer; they are decoded from the main loop.
 */
class PioBus {
 public:
  void setup(unsigned base_pin);
  // Next received frame (inverted to decoded form), false if none is waiting
  bool read(uint16_t &frame);
  // Reply to every button frame equal to code until release()
  void hold(uint16_t code);
  void release();
  // Frames lost because the main loop fell behind
  uint32_t dropped() const;
};

}  // namespace purespa
}  // namespace esphome
