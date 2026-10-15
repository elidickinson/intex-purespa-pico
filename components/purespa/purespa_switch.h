#pragma once

#include "esphome/components/switch/switch.h"
#include "purespa.h"

namespace esphome {
namespace purespa {

// State is published from the spa's LEDs, not from the request, so a press the spa ignores is visible
class PureSpaSwitch : public switch_::Switch, public Parented<PureSpa> {
 public:
  void init(PureSpa *parent, Button button, uint16_t led) {
    parent_ = parent;
    button_ = button;
    led_ = led;
  }

 protected:
  void write_state(bool state) override { parent_->set_state(button_, led_, state); }

  Button button_{};
  uint16_t led_ = 0;
};

}  // namespace purespa
}  // namespace esphome
