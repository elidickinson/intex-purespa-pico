#include "protocol.h"

#include <cmath>

namespace esphome {
namespace purespa {

static constexpr uint16_t DIGIT_POS[4] = {0x0040, 0x0020, 0x0800, 0x0004};
static constexpr uint16_t DIGIT_POS_MASK = 0x0864;

// Segment bits: A 0x2000, B 0x1000, C 0x0200, D 0x0400, E 0x0080, F 0x0008, G 0x0010
static constexpr uint16_t SEGMENTS = 0x3698;
static constexpr struct {
  char c;
  uint16_t segments;
} CHARS[] = {
    {'0', 0x3688}, {'1', 0x1200}, {'2', 0x3490}, {'3', 0x3610}, {'4', 0x1218}, {'5', 0x2618}, {'6', 0x2698},
    {'7', 0x3200}, {'8', 0x3698}, {'9', 0x3618}, {'A', 0x3298}, {'C', 0x2488}, {'D', 0x1690}, {'E', 0x2498},
    {'F', 0x2098}, {'H', 0x1298}, {'N', 0x3288}, {' ', 0x0000},
};

uint16_t digit_frame(int pos, char c) {
  for (const auto &ch : CHARS) {
    if (ch.c == c)
      return DIGIT_POS[pos] | ch.segments;
  }
  return DIGIT_POS[pos];
}

static char to_char(uint16_t segments) {
  for (const auto &ch : CHARS) {
    if (ch.segments == segments)
      return ch.c;
  }
  return '?';
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Temperature in °C if the display shows one ("038C" or "100F")
static std::optional<uint8_t> temperature(const Display &d) {
  if ((d[3] != 'C' && d[3] != 'F') || !is_digit(d[0]) || !is_digit(d[1]) || !is_digit(d[2]))
    return std::nullopt;
  int value = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
  if (d[3] == 'F')
    value = std::lround((value - 32) * 5 / 9.0);
  if (value < 0)
    value = 0;
  return value;
}

static std::optional<uint8_t> sanitizer_hours(const Display &d) {
  if (d[3] != 'H' || !is_digit(d[0]) || !is_digit(d[1]) || !is_digit(d[2]))
    return std::nullopt;
  uint8_t hours = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
  if (hours < 3 || hours > 8)
    return std::nullopt;
  return hours;
}

void Decoder::feed(uint16_t frame) {
  uint16_t pos_bits = frame & DIGIT_POS_MASK;
  if (pos_bits) {
    int pos = 0;
    while (pos < 4 && DIGIT_POS[pos] != pos_bits)
      pos++;
    if (pos == 0)
      next_pos_ = 0;
    if (pos != next_pos_)
      return;  // invalid or out of order, wait for the next display
    display_[pos] = to_char(frame & SEGMENTS);
    if (++next_pos_ == 4)
      on_display();
    return;
  }
  if (frame & led::FRAME) {
    status_.leds = frame;
    if (!(frame & led::SANITIZER)) {
      status_.sanitizer_hours = 0;
      sanitizer_candidate_.reset();
    }
  }
}

void Decoder::on_display() {
  bool blank = display_[0] == ' ' && display_[1] == ' ' && display_[2] == ' ';
  if (display_ != last_display_) {
    last_display_ = display_;
    count_ = 0;
    run_after_blank_ = prev_was_blank_;
  }
  if (count_ < STEADY) {
    count_++;
    if (count_ == CONFIRM) {
      if (blank) {
        // The blank phase confirms the candidate of the type that was blinking
        if (candidate_) {
          status_.target_temperature = *candidate_;
          candidate_.reset();
          sanitizer_candidate_.reset();  // a temp blink cannot also be an hours edit
        }
        if (sanitizer_candidate_) {
          status_.sanitizer_hours = *sanitizer_candidate_;
          sanitizer_candidate_.reset();
          candidate_.reset();
        }
        if (blink_count_ < UINT8_MAX)
          blink_count_++;
      } else if (display_[0] == 'E') {
        status_.error = {display_[0], display_[1], display_[2], 0};
      } else if (auto t = temperature(display_); t && run_after_blank_) {
        // Value after a blank is a blink candidate; a repeated value confirms it early
        sanitizer_candidate_.reset();  // the display shows a temperature, not hours
        if (candidate_ && *candidate_ == *t) {
          status_.target_temperature = *t;
          candidate_.reset();
        } else {
          candidate_ = t;
        }
      } else if (auto hours = sanitizer_hours(display_); hours && run_after_blank_) {
        candidate_.reset();  // the display shows hours, not a temperature
        if (sanitizer_candidate_ && *sanitizer_candidate_ == *hours) {
          status_.sanitizer_hours = *hours;
          sanitizer_candidate_.reset();
        } else {
          sanitizer_candidate_ = hours;
        }
      }
    } else if (count_ == STEADY) {
      // Persistent display is steady water temperature or sanitizer hours, not a blink value
      if (auto t = temperature(display_)) {
        status_.current_temperature = *t;
        status_.error = {};
        candidate_.reset();
        sanitizer_candidate_.reset();
        blink_count_ = 0;
      } else if (auto hours = sanitizer_hours(display_)) {
        status_.sanitizer_hours = *hours;
        sanitizer_candidate_.reset();
        candidate_.reset();
        blink_count_ = 0;
      }
    }
  }
  prev_was_blank_ = blank;
}

bool Controller::set_target(uint8_t temperature, uint32_t now) {
  if (temperature < MIN_TARGET || temperature > MAX_TARGET || !(decoder_.status().leds & led::POWER) ||
      adjusting_sanitizer())
    return false;
  if (!setpoint_)
    phase_ = Phase::WAKE;
  setpoint_ = temperature;
  setpoint_at_ = now;
  return true;
}

bool Controller::set_sanitizer_hours(uint8_t hours, uint32_t now) {
  if ((hours != 0 && hours != 3 && hours != 5 && hours != 8) ||
      !(decoder_.status().leds & led::POWER) || adjusting())
    return false;
  sanitizer_setpoint_ = hours;
  sanitizer_setpoint_at_ = now;
  sanitizer_waiting_ = false;
  unseen_presses_ = 0;
  return true;
}

void Controller::update(uint32_t now) {
  const Status &s = decoder_.status();
  // The target is only shown after pressing UP/DOWN; retry until known if a press was swallowed.
  if (!(s.leds & led::POWER)) {
    probed_ = false;
  } else if (!adjusting() && !adjusting_sanitizer() && !decoder_.blinking() && !s.target_temperature &&
             (!probed_ || now - probed_at_ >= PROBE_RETRY_MS)) {
    probed_ = true;
    probed_at_ = now;
    press(Button::DOWN);
  }

  if (short_press_) {
    if (now - pressed_at_ < PRESS_MS)
      return;
    release(now);
  }
  if (gap_) {
    if (now - released_at_ < GAP_MS)
      return;
    gap_ = false;
  }

  if (setpoint_) {
    adjust(now);
  } else if (sanitizer_setpoint_) {
    adjust_sanitizer(now);
  } else if (!presses_.empty()) {
    start_press(presses_.front(), now);
    presses_.pop_front();
  }
}

void Controller::start_press(uint16_t frame_code, uint32_t now) {
  held_ = frame_code;
  short_press_ = true;
  pressed_at_ = now;
}

void Controller::release(uint32_t now) {
  held_.reset();
  short_press_ = false;
  gap_ = true;
  released_at_ = now;
}

void Controller::adjust(uint32_t now) {
  const Status &s = decoder_.status();
  if (now - setpoint_at_ >= TARGET_TIMEOUT_MS || !(s.leds & led::POWER)) {
    finish(now);
    return;
  }
  if (decoder_.shown_target() == setpoint_) {
    finish(now);
    return;
  }
  // Stop on reaching/crossing the target, then let the blink settle before re-evaluating.
  if (phase_ == Phase::SETTLE) {
    if (decoder_.blinking())
      return;
    phase_ = Phase::WAKE;
    return;
  }
  bool crossed = direction_would_be_(s) == Button::UP
                     ? decoder_.shown_target() >= setpoint_
                     : decoder_.shown_target() <= setpoint_;
  if (crossed) {
    if (held_)
      release(now);
    phase_ = Phase::SETTLE;
    return;
  }

  Button direction = setpoint_ > s.target_temperature ? Button::UP : Button::DOWN;
  switch (phase_) {
    case Phase::WAKE:
      if (!decoder_.blinking()) {
        start_press(static_cast<uint16_t>(direction), now);
        woke_at_ = now;
        phase_ = Phase::WAIT_BLINK;
        return;
      }
      break;
    case Phase::WAIT_BLINK:
      if (s.target_temperature == 0 && decoder_.blinking() && now - woke_at_ < BLINK_WAIT_MS)
        return;
      if (s.target_temperature == 0) {
        release(now);
        phase_ = Phase::WAKE;
        woke_at_ = now;
        return;
      }
      break;
    case Phase::HOLD:
      if (decoder_.blinking()) {
        last_blink_at_ = now;
      } else if (now - last_blink_at_ >= STALL_MS) {
        release(now);
        phase_ = Phase::WAKE;
        woke_at_ = now;
        return;
      }
      break;
    case Phase::SETTLE:
      break;
  }
  phase_ = Phase::HOLD;
  if (held_ != static_cast<uint16_t>(direction))
    held_ = static_cast<uint16_t>(direction);
}

void Controller::adjust_sanitizer(uint32_t now) {
  const Status &s = decoder_.status();
  if (now - sanitizer_setpoint_at_ >= SANITIZER_TIMEOUT_MS || !(s.leds & led::POWER)) {
    finish_sanitizer(now);
    return;
  }
  bool on = s.leds & led::SANITIZER;
  uint8_t shown_hours = decoder_.shown_sanitizer_hours();
  bool reached = *sanitizer_setpoint_ == 0 ? !on : on && shown_hours == *sanitizer_setpoint_;
  if (reached) {
    finish_sanitizer(now);
    return;
  }

  // Each accepted press cycles off/3h/5h/8h/off; wait for the display or LED to prove it
  // advanced. A press that appears swallowed is retried; after MAX_UNSEEN unobserved presses
  // the flow aborts (the button code or LED mask is likely wrong - both are unverified).
  if (sanitizer_waiting_) {
    bool changed = on != sanitizer_before_on_ || shown_hours != sanitizer_before_hours_;
    if (changed) {
      sanitizer_waiting_ = false;
      unseen_presses_ = 0;
    } else if (now - sanitizer_pressed_at_ < STALL_MS) {
      return;
    } else {
      sanitizer_waiting_ = false;  // retry a swallowed press
      if (++unseen_presses_ >= MAX_UNSEEN_PRESSES) {
        finish_sanitizer(now);
        return;
      }
    }
  }
  if (on && shown_hours == 0) {
    // The panel does not show the selected hours steadily; wait for the blink to reveal them.
    // Bounded: after STALL_MS a press is sent anyway - cycling once cannot lose more than the
    // cycle length, and the LED state keeps the switch truthful.
    if (now - sanitizer_setpoint_at_ < STALL_MS)
      return;
  }

  sanitizer_before_on_ = on;
  sanitizer_before_hours_ = shown_hours;
  sanitizer_pressed_at_ = now;
  sanitizer_waiting_ = true;
  start_press(static_cast<uint16_t>(Button::SANITIZER), now);
}

void Controller::finish(uint32_t now) {
  setpoint_ = 0;
  phase_ = Phase::WAKE;
  if (held_)
    release(now);
}

void Controller::finish_sanitizer(uint32_t now) {
  sanitizer_setpoint_.reset();
  sanitizer_waiting_ = false;
  if (held_)
    release(now);
}

}  // namespace purespa
}  // namespace esphome
