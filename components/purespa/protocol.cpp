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
  if (frame & led::FRAME)
    status_.leds = frame;
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
        // The blank phase of a blink: the previous value phase is confirmed as the target
        if (candidate_) {
          status_.target_temperature = *candidate_;
          candidate_.reset();
        }
        if (blink_count_ < UINT8_MAX)
          blink_count_++;
      } else if (display_[0] == 'E') {
        status_.error = {display_[0], display_[1], display_[2], 0};
      } else if (auto t = temperature(display_); t && run_after_blank_) {
        // First value run after a blank: a blink's value phase, or the water temperature
        // returning after a blink that ended on a blank. The same value appearing again after
        // the next blank confirms it early; otherwise it only confirms if another blank follows
        // before STEADY displays. A blink value is shown ~630 ms at most, well under STEADY.
        if (candidate_ && *candidate_ == *t) {
          status_.target_temperature = *t;
          candidate_.reset();
        } else {
          candidate_ = t;
        }
      }
    } else if (count_ == STEADY) {
      // A value that persists this long is the water temperature, blink or not: the blink value
      // run keeps being interrupted by blank phases, so it never reaches STEADY
      if (auto t = temperature(display_)) {
        status_.current_temperature = *t;
        status_.error = {};
        candidate_.reset();
        blink_count_ = 0;
      }
    }
  }
  prev_was_blank_ = blank;
}

bool Controller::set_target(uint8_t temperature, uint32_t now) {
  if (temperature < MIN_TARGET || temperature > MAX_TARGET || !(decoder_.status().leds & led::POWER))
    return false;
  if (!setpoint_)
    phase_ = Phase::WAKE;
  setpoint_ = temperature;
  setpoint_at_ = now;
  return true;
}

void Controller::update(uint32_t now) {
  const Status &s = decoder_.status();
  // The target is only shown after pressing UP/DOWN; the first press does not change it. Retried
  // until the target is known, in case a press is swallowed.
  if (!(s.leds & led::POWER)) {
    probed_ = false;
  } else if (!adjusting() && !decoder_.blinking() && !s.target_temperature &&
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
  } else if (!presses_.empty()) {
    start_press(presses_.front(), now);
    presses_.pop_front();
  }
}

void Controller::start_press(Button button, uint32_t now) {
  held_ = button;
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
  // The value shown as the target lags a held button by up to one blank phase. As soon as it
  // reaches or crosses the setpoint, stop pressing and let the blink settle; the final value is
  // then confirmed (or rejected as the water temperature) and re-evaluated here.
  if (phase_ == Phase::SETTLE) {
    if (decoder_.blinking())
      return;
    phase_ = Phase::WAKE;
    return;  // settled: re-evaluate on the next update with the confirmed value
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
        start_press(direction, now);
        woke_at_ = now;
        phase_ = Phase::WAIT_BLINK;
        return;
      }
      break;
    case Phase::WAIT_BLINK:
      if (s.target_temperature == 0 && decoder_.blinking() && now - woke_at_ < BLINK_WAIT_MS)
        return;  // blink started, waiting for the target to be captured
      if (s.target_temperature == 0) {
        // Blink never started or no target captured; press again
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
        // The press is not registering or the blink ended; press again
        release(now);
        phase_ = Phase::WAKE;
        woke_at_ = now;
        return;
      }
      break;
    case Phase::SETTLE:
      break;  // handled above
    }
  phase_ = Phase::HOLD;
  if (held_ != direction)
    held_ = direction;
}

void Controller::finish(uint32_t now) {
  setpoint_ = 0;
  phase_ = Phase::WAKE;
  if (held_)
    release(now);
}

}  // namespace purespa
}  // namespace esphome
