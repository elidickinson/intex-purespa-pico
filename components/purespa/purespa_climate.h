#pragma once

#include "esphome/components/climate/climate.h"
#include "purespa.h"

namespace esphome {
namespace purespa {

class PureSpaClimate : public climate::Climate, public Parented<PureSpa> {
 protected:
  void control(const climate::ClimateCall &call) override;

  climate::ClimateTraits traits() override {
    climate::ClimateTraits traits;
    traits.set_visual_min_temperature(Controller::MIN_TARGET);
    traits.set_visual_max_temperature(Controller::MAX_TARGET);
    traits.set_visual_temperature_step(1);
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
    traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT});
    return traits;
  }
};

}  // namespace purespa
}  // namespace esphome
