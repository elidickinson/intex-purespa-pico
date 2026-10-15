#include "purespa_climate.h"

namespace esphome {
namespace purespa {

void PureSpaClimate::control(const climate::ClimateCall &call) {
  if (call.get_target_temperature().has_value())
    parent_->set_target(*call.get_target_temperature());
  if (call.get_mode().has_value())
    parent_->set_heater(*call.get_mode() == climate::CLIMATE_MODE_HEAT);
}

}  // namespace purespa
}  // namespace esphome
