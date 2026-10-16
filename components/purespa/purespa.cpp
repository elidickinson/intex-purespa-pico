#include "purespa.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "purespa_climate.h"
#include "purespa_switch.h"

namespace esphome {
namespace purespa {

static const char *const TAG = "purespa";
static constexpr unsigned BASE_PIN = 3;

void PureSpa::set_climate(PureSpaClimate *climate) {
  climate_ = climate;
  climate->set_parent(this);
}
void PureSpa::set_switch_power(PureSpaSwitch *sw) {
  switch_power_ = sw;
  sw->init(this, Button::POWER, led::POWER);
}
void PureSpa::set_switch_filter(PureSpaSwitch *sw) {
  switch_filter_ = sw;
  sw->init(this, Button::FILTER, led::FILTER);
}
void PureSpa::set_switch_bubble(PureSpaSwitch *sw) {
  switch_bubble_ = sw;
  sw->init(this, Button::BUBBLE, led::BUBBLE);
}

void PureSpa::setup() { bus_.setup(BASE_PIN); }

void PureSpa::loop() {
  uint16_t frame;
  while (bus_.read(frame))
    decoder_.feed(frame);

  uint32_t dropped = bus_.dropped();
  if (dropped != dropped_) {
    ESP_LOGW(TAG, "Dropped %u frames", (unsigned) (dropped - dropped_));
    dropped_ = dropped;
  }

  controller_.update(millis());
  if (controller_.held() != held_) {
    held_ = controller_.held();
    if (held_)
      bus_.hold((uint16_t) *held_);
    else
      bus_.release();
  }

  // The displayed target is transient while adjusting; switches, errors and temperatures
  // are published as usual
  if (controller_.adjusting()) {
    const Status &adjusting_status = decoder_.status();
    if (adjusting_status != published_)
      publish(adjusting_status);
    return;
  }

  const Status &status = decoder_.status();
  if (requested_target_) {
    if (status.target_temperature == requested_target_) {
      ESP_LOGI(TAG, "Target %u reached", requested_target_);
    } else {
      ESP_LOGW(TAG, "Could not set target %u, spa shows %u", requested_target_, status.target_temperature);
    }
    requested_target_ = 0;
    force_publish_ = true;
  }
  if (force_publish_ || status != published_)
    publish(status);
}

void PureSpa::publish(const Status &status) {
  ESP_LOGI(TAG, "Water %u, target %u, error '%s', LEDs %04x", status.current_temperature, status.target_temperature,
           status.error.data(), status.leds);
  if (switch_power_)
    switch_power_->publish_state(status.leds & led::POWER);
  if (switch_filter_)
    switch_filter_->publish_state(status.leds & led::FILTER);
  if (switch_bubble_)
    switch_bubble_->publish_state(status.leds & led::BUBBLE);
  if (error_text_)
    error_text_->publish_state(status.error.data());
  if (climate_) {
    climate_->current_temperature = status.current_temperature ? status.current_temperature : NAN;
    climate_->target_temperature = status.target_temperature ? status.target_temperature : NAN;
    if (status.leds & led::HEATER) {
      climate_->action = climate::CLIMATE_ACTION_HEATING;
      climate_->mode = climate::CLIMATE_MODE_HEAT;
    } else if (status.leds & led::STANDBY) {
      climate_->action = climate::CLIMATE_ACTION_IDLE;
      climate_->mode = climate::CLIMATE_MODE_HEAT;
    } else {
      climate_->action = climate::CLIMATE_ACTION_OFF;
      climate_->mode = climate::CLIMATE_MODE_OFF;
    }
    climate_->publish_state();
  }
  published_ = status;
  force_publish_ = false;
}

void PureSpa::set_state(Button button, uint16_t mask, bool state) {
  ESP_LOGI(TAG, "Set %04x %s", (unsigned) button, state ? "on" : "off");
  if (bool(decoder_.status().leds & mask) != state) {
    controller_.press(button);
  } else {
    // Already in that state; resend it in case Home Assistant shows something else
    force_publish_ = true;
  }
}

void PureSpa::set_target(float temperature) {
  if (!std::isfinite(temperature)) {
    ESP_LOGW(TAG, "Invalid target temperature");
    force_publish_ = true;
    return;
  }
  uint8_t target = std::lround(temperature);
  if (controller_.set_target(target, millis())) {
    ESP_LOGI(TAG, "Set target %u", target);
    requested_target_ = target;
  } else {
    ESP_LOGW(TAG, "Cannot set target %u: spa is off or target out of range", target);
    force_publish_ = true;
  }
}

}  // namespace purespa
}  // namespace esphome
