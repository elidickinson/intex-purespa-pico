#pragma once

#include "esphome/components/switch/switch.h"
#include "purespa.h"

namespace esphome {
namespace purespa {

// State is published from the spa's LEDs, not from the request, so a press the spa ignores is visible
class PureSpaSwitch : public switch_::Switch, public Parented<PureSpa> {
 public:
  // toggles = true: write_state presses the cycle button (power/filter/bubble).
  // toggles = false: write_state routes through set_sanitizer(), whose button cycles
  // off/3h/5h/8h/off and must not be pressed blindly.
  void init(PureSpa *parent, uint16_t button_code, uint16_t led, bool toggles = true) {
    parent_ = parent;
    button_code_ = button_code;
    led_ = led;
    toggles_ = toggles;
  }

 protected:
  void write_state(bool state) override {
    if (toggles_)
      parent_->set_state(button_code_, led_, state);
    else
      parent_->set_sanitizer(state);
  }

  uint16_t button_code_ = 0;
  uint16_t led_ = 0;
  bool toggles_ = true;
};

}  // namespace purespa
}  // namespace esphome
