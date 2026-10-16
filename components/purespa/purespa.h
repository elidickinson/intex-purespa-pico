#pragma once

#include <optional>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "pio_bus.h"
#include "protocol.h"

namespace esphome {
namespace purespa {

class PureSpaClimate;
class PureSpaSwitch;
class PureSpaSelect;

class PureSpa : public Component {
 public:
  void setup() override;
  void loop() override;

  void set_climate(PureSpaClimate *climate);
  void set_switch_power(PureSpaSwitch *sw);
  void set_switch_filter(PureSpaSwitch *sw);
  void set_switch_bubble(PureSpaSwitch *sw);
  void set_switch_sanitizer(PureSpaSwitch *sw);
  void set_sanitizer_hours_select(PureSpaSelect *hours);
  void set_error_text_sensor(text_sensor::TextSensor *error) { error_text_ = error; }

  void set_state(Button button, uint16_t mask, bool state) { set_state(static_cast<uint16_t>(button), mask, state); }
  void set_state(uint16_t button_code, uint16_t mask, bool state);
  void set_heater(bool state) { set_state(Button::HEATER, led::HEATER | led::STANDBY, state); }
  void set_target(float temperature);
  void set_sanitizer_hours(uint8_t hours);
  void set_sanitizer(bool on);

 private:
  void publish(const Status &status);

  PioBus bus_;
  Decoder decoder_;
  Controller controller_{decoder_};
  std::optional<uint16_t> held_;
  uint32_t dropped_ = 0;
  uint8_t requested_target_ = 0;
  std::optional<uint8_t> requested_sanitizer_;
  Status published_;
  bool force_publish_ = true;

  PureSpaClimate *climate_ = nullptr;
  PureSpaSwitch *switch_power_ = nullptr;
  PureSpaSwitch *switch_filter_ = nullptr;
  PureSpaSwitch *switch_bubble_ = nullptr;
  PureSpaSwitch *switch_sanitizer_ = nullptr;
  PureSpaSelect *sanitizer_hours_select_ = nullptr;
  text_sensor::TextSensor *error_text_ = nullptr;
};

}  // namespace purespa
}  // namespace esphome
