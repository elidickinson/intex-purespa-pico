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

class PureSpa : public Component {
 public:
  void setup() override;
  void loop() override;

  void set_climate(PureSpaClimate *climate);
  void set_switch_power(PureSpaSwitch *sw);
  void set_switch_filter(PureSpaSwitch *sw);
  void set_switch_bubble(PureSpaSwitch *sw);
  void set_error_text_sensor(text_sensor::TextSensor *error) { error_text_ = error; }

  // Presses button unless the LEDs in mask already match state
  void set_state(Button button, uint16_t mask, bool state);
  void set_heater(bool state) { set_state(Button::HEATER, led::HEATER | led::STANDBY, state); }
  void set_target(float temperature);

 private:
  void publish(const Status &status);

  PioBus bus_;
  Decoder decoder_;
  Controller controller_{decoder_};
  std::optional<Button> held_;
  uint32_t dropped_ = 0;
  uint8_t requested_target_ = 0;
  Status published_;
  bool force_publish_ = true;

  PureSpaClimate *climate_ = nullptr;
  PureSpaSwitch *switch_power_ = nullptr;
  PureSpaSwitch *switch_filter_ = nullptr;
  PureSpaSwitch *switch_bubble_ = nullptr;
  text_sensor::TextSensor *error_text_ = nullptr;
};

}  // namespace purespa
}  // namespace esphome
