#pragma once

#include "esphome/components/select/select.h"
#include "purespa.h"

namespace esphome {
namespace purespa {

class PureSpaSelect : public select::Select, public Parented<PureSpa> {
 public:
  void init(PureSpa *parent) { parent_ = parent; }

 protected:
  void control(size_t index) override {
    static constexpr uint8_t HOURS[] = {0, 3, 5, 8};
    if (index < sizeof(HOURS) / sizeof(HOURS[0]))
      parent_->set_sanitizer_hours(HOURS[index]);
  }
};

}  // namespace purespa
}  // namespace esphome
