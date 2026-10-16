#include "purespa.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "purespa_climate.h"
#include "purespa_select.h"
#include "purespa_switch.h"

namespace esphome {
namespace purespa {

static const char *const TAG = "purespa";
static constexpr unsigned BASE_PIN = 3;

static const char *error_message(const std::array<char, 4> &error) {
  if (!error[0])
    return "ok";
  if (error == std::array<char, 4>{'E', '9', '0', 0})
    return "No water flow";
  if (error == std::array<char, 4>{'E', '9', '1', 0})
    return "Salt level too low";
  if (error == std::array<char, 4>{'E', '9', '2', 0})
    return "Salt level too high";
  if (error == std::array<char, 4>{'E', '9', '4', 0})
    return "Water temperature too low";
  if (error == std::array<char, 4>{'E', '9', '5', 0})
    return "Water temperature too high";
  if (error == std::array<char, 4>{'E', '9', '6', 0})
    return "System error";
  if (error == std::array<char, 4>{'E', '9', '7', 0})
    return "Dry fire protection";
  if (error == std::array<char, 4>{'E', '9', '9', 0})
    return "Water temperature sensor error";
  if (error == std::array<char, 4>{'E', 'N', 'D', 0})
    return "Heating aborted after 72h";
  return error.data();
}

void PureSpa::set_climate(PureSpaClimate *climate) {
  climate_ = climate;
  climate->set_parent(this);
}
void PureSpa::set_switch_power(PureSpaSwitch *sw) {
  switch_power_ = sw;
  sw->init(this, static_cast<uint16_t>(Button::POWER), led::POWER);
}
void PureSpa::set_switch_filter(PureSpaSwitch *sw) {
  switch_filter_ = sw;
  sw->init(this, static_cast<uint16_t>(Button::FILTER), led::FILTER);
}
void PureSpa::set_switch_bubble(PureSpaSwitch *sw) {
  switch_bubble_ = sw;
  sw->init(this, static_cast<uint16_t>(Button::BUBBLE), led::BUBBLE);
}
void PureSpa::set_switch_sanitizer(PureSpaSwitch *sw) {
  switch_sanitizer_ = sw;
  sw->init(this, 0, led::SANITIZER, /*toggles=*/false);
}
void PureSpa::set_sanitizer_hours_select(PureSpaSelect *hours) {
  sanitizer_hours_select_ = hours;
  hours->init(this);
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
      bus_.hold(*held_);
    else
      bus_.release();
  }

  const Status &status = decoder_.status();
  if (controller_.adjusting() || controller_.adjusting_sanitizer()) {
    if (status != published_)
      publish(status);
    return;
  }
  if (requested_target_) {
    if (status.target_temperature == requested_target_) {
      ESP_LOGI(TAG, "Target %u reached", requested_target_);
    } else {
      ESP_LOGW(TAG, "Could not set target %u, spa shows %u", requested_target_, status.target_temperature);
    }
    requested_target_ = 0;
    force_publish_ = true;
  }
  if (requested_sanitizer_) {
    uint8_t hours = *requested_sanitizer_;
    bool reached = hours == 0 ? !(status.leds & led::SANITIZER)
                              : (status.leds & led::SANITIZER) &&
                                    decoder_.shown_sanitizer_hours() == hours;
    if (reached) {
      ESP_LOGI(TAG, "Sanitizer %u hours reached", hours);
    } else {
      ESP_LOGW(TAG, "Could not set sanitizer to %u hours (LEDs %04x, shown %u)", hours, status.leds,
             decoder_.shown_sanitizer_hours());
    }
    requested_sanitizer_.reset();
    force_publish_ = true;
  }
  if (force_publish_ || status != published_)
    publish(status);
}

void PureSpa::publish(const Status &status) {
  ESP_LOGI(TAG, "Water %u, target %u, sanitizer %u h, error '%s', LEDs %04x", status.current_temperature,
           status.target_temperature, status.sanitizer_hours, error_message(status.error), status.leds);
  if (switch_power_)
    switch_power_->publish_state(status.leds & led::POWER);
  if (switch_filter_)
    switch_filter_->publish_state(status.leds & led::FILTER);
  if (switch_bubble_)
    switch_bubble_->publish_state(status.leds & led::BUBBLE);
  if (switch_sanitizer_)
    switch_sanitizer_->publish_state(status.leds & led::SANITIZER);
  if (sanitizer_hours_select_) {
    // With the sanitizer on but its hours not yet decoded, the last state is kept: there is
    // no honest option to show and the display catches up within one blink cycle.
    if (!(status.leds & led::SANITIZER))
      sanitizer_hours_select_->publish_state("off");
    else if (status.sanitizer_hours == 3)
      sanitizer_hours_select_->publish_state("3h");
    else if (status.sanitizer_hours == 5)
      sanitizer_hours_select_->publish_state("5h");
    else if (status.sanitizer_hours == 8)
      sanitizer_hours_select_->publish_state("8h");
  }
  if (error_text_)
    error_text_->publish_state(error_message(status.error));
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

void PureSpa::set_state(uint16_t button_code, uint16_t mask, bool state) {
  ESP_LOGI(TAG, "Set %04x %s", button_code, state ? "on" : "off");
  if (bool(decoder_.status().leds & mask) != state) {
    controller_.press(button_code);
  } else {
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
    ESP_LOGW(TAG, "Cannot set target %u: spa is off, target out of range or sanitizer is adjusting", target);
    force_publish_ = true;
  }
}

// The sanitizer button cycles off/3h/5h/8h/off, so the switch never presses it directly:
// on turns the sanitizer on (keeping/starting the default 3 h), off cycles to off.
void PureSpa::set_sanitizer(bool on) {
  if (on) {
    if (decoder_.status().leds & led::SANITIZER) {
      force_publish_ = true;
      return;
    }
    set_sanitizer_hours(decoder_.status().sanitizer_hours ? decoder_.status().sanitizer_hours : 3);
  } else {
    set_sanitizer_hours(0);
  }
}

void PureSpa::set_sanitizer_hours(uint8_t hours) {
  if (controller_.set_sanitizer_hours(hours, millis())) {
    ESP_LOGI(TAG, "Set sanitizer hours %u", hours);
    requested_sanitizer_ = hours;
  } else {
    ESP_LOGW(TAG, "Cannot set sanitizer hours %u: spa is off, hours are invalid or temperature is adjusting", hours);
    force_publish_ = true;
  }
}

}  // namespace purespa
}  // namespace esphome
