#include "spa.h"

#include <cstdlib>

namespace spa {

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

static const struct {
  Button button;
  const char *name;
} BUTTONS[] = {
    {Button::BUBBLES, "bubbles"}, {Button::FILTER, "filter"}, {Button::UNIT, "unit"}, {Button::HEATER, "heater"},
    {Button::POWER, "power"},     {Button::UP, "up"},         {Button::DOWN, "down"}, {Button::SALT, "salt"},
};

const char *button_name(Button b) {
  for (const auto &entry : BUTTONS) {
    if (entry.button == b)
      return entry.name;
  }
  return "?";
}

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

// The temperature menu's blank phase keeps the unit: "   F"
static bool is_blank(const std::string &text) { return text.compare(0, 3, "   ") == 0; }

static std::optional<int> number_of(const std::string &text) {
  if (text.size() != 4)
    return std::nullopt;
  for (int i = 0; i < 3; i++) {
    if (text[i] < '0' || text[i] > '9')
      return std::nullopt;
  }
  return (text[0] - '0') * 100 + (text[1] - '0') * 10 + (text[2] - '0');
}

std::optional<int> temperature_of(const std::string &text) {
  auto n = number_of(text);
  if (!n || (text[3] != 'F' && text[3] != 'C'))
    return std::nullopt;
  return n;
}

std::optional<int> hours_of(const std::string &text) {
  if (text == "0FF ")
    return 0;
  auto n = number_of(text);
  if (!n || text[3] != 'H')
    return std::nullopt;
  return n;
}

void Decoder::feed(uint16_t raw) {
  uint16_t frame = raw & ~NO_BEEP;
  if (frame == 0) {
    after_cue_ = true;
    return;
  }
  bool after_cue = after_cue_;
  after_cue_ = false;
  if (!after_cue)
    return;  // button poll

  if (frame & led::FRAME) {
    // The first frame is a starting point, not a toggle
    uint16_t changed = raw_leds_ ? frame ^ raw_leds_ : 0;
    raw_leds_ = frame;
    blinking_ = 0;
    for (int bit = 0; bit < 16; bit++) {
      if (changed >> bit & 1) {
        toggled_before_[bit] = toggled_[bit];
        toggled_[bit] = displays_ + 1;
      }
      if (toggled_before_[bit] && displays_ + 1 - toggled_before_[bit] < BLINK_DISPLAYS)
        blinking_ |= 1 << bit;
    }
    leds_ = frame | (blinking_ & led::HEATER_IDLE);
    bool beeping = !(raw & NO_BEEP);
    if (beeping && !beeping_)
      beeps_++;
    if (beeping)
      last_beep_ = displays_;
    beeping_ = beeping;
    if (!(frame & (led::SALT | led::FILTER)))
      hours_left_.reset();
    return;
  }
  int pos = 0;
  while (pos < 4 && DIGIT_POS[pos] != (frame & DIGIT_POS_MASK))
    pos++;
  if (pos == 4)
    return;  // the first poll of a run, or a garbled frame
  if (pos == 0)
    next_pos_ = 0;
  if (pos != next_pos_)
    return;  // out of order, wait for the next display
  digits_[pos] = to_char(frame & SEGMENTS);
  if (++next_pos_ == 4) {
    next_pos_ = 0;
    on_display();
  }
}

void Decoder::on_display() {
  displays_++;
  std::string text(digits_.begin(), digits_.end());
  if (text == pending_) {
    pending_count_++;
  } else {
    pending_ = text;
    pending_count_ = 1;
  }
  if (pending_count_ == STABLE && text != stable_)
    new_run(text);
  if (is_blank(stable_))
    last_blank_ = displays_;
  if (!run_classified_ && displays_ - run_start_ >= STEADY) {
    run_classified_ = true;
    classify(stable_, true);
  }
}

void Decoder::new_run(const std::string &text) {
  bool blank = is_blank(text);
  // Runs that reached STEADY are classified already. A short value between two other values is
  // skipped: the water temperature flickering by a degree, or a menu value changing before its
  // first blank.
  if (!run_classified_) {
    if (run_after_blank_ || blank)
      classify(stable_, false);
    else if (displays_ - run_start_ >= MIN_STEADY)
      classify(stable_, true);
  }
  run_after_blank_ = is_blank(stable_);
  stable_ = text;
  run_start_ = displays_;
  run_classified_ = blank;
  if (blank)
    seen_blank_ = true;
}

void Decoder::classify(const std::string &text, bool steady) {
  if (text[0] == 'E') {
    error_ = text.substr(0, text.find(' '));
    return;
  }
  auto temperature = temperature_of(text);
  if (temperature)
    unit_ = text[3];
  if (!steady) {
    menu_text_ = text;
    menu_seq_++;
    if (temperature)
      target_ = temperature;
  } else if (temperature) {
    water_ = temperature;
    error_.clear();
  } else if (auto hours = hours_of(text); hours && *hours) {
    hours_left_ = hours;
  }
}

std::string parse_request(const std::string &key, const std::string &value, Request &out) {
  auto on_off = [&](Request::Kind kind) -> std::string {
    if (value != "on" && value != "off")
      return key + " must be on or off";
    out = {kind, value == "on"};
    return "";
  };
  if (key == "power")
    return on_off(Request::POWER);
  if (key == "heater")
    return on_off(Request::HEATER);
  if (key == "bubbles")
    return on_off(Request::BUBBLES);
  if (key == "temp") {
    char *end;
    long t = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || *end || t < 10 || t > 104)
      return "temp must be a whole number, 50-104 F or 10-40 C";
    out = {Request::TEMP, int(t)};
    return "";
  }
  if (key == "salt") {
    int hours = value == "off" ? 0 : value == "3" ? 3 : value == "5" ? 5 : value == "8" ? 8 : -1;
    if (hours < 0)
      return "salt must be off, 3, 5 or 8";
    out = {Request::SALT, hours};
    return "";
  }
  if (key == "press") {
    for (const auto &entry : BUTTONS) {
      if (value == entry.name) {
        out = {Request::PRESS, int(entry.button)};
        return "";
      }
    }
    return "press must be one of bubbles, filter, unit, heater, power, up, down, salt";
  }
  return "unknown setting '" + key + "'";
}

std::string describe(const Request &r) {
  switch (r.kind) {
    case Request::POWER:
      return r.value ? "power=on" : "power=off";
    case Request::HEATER:
      return r.value ? "heater=on" : "heater=off";
    case Request::BUBBLES:
      return r.value ? "bubbles=on" : "bubbles=off";
    case Request::TEMP:
      return "temp=" + std::to_string(r.value);
    case Request::SALT:
      return r.value ? "salt=" + std::to_string(r.value) : "salt=off";
    case Request::PRESS:
      return std::string("press=") + button_name(Button(r.value));
  }
  return "?";
}

bool Controller::queue(const Request &r) {
  if (queue_.size() >= MAX_QUEUE)
    return false;
  queue_.push_back(r);
  return true;
}

bool Controller::take_result(std::string &out) {
  if (results_.empty())
    return false;
  out = results_.front();
  results_.pop_front();
  return true;
}

void Controller::update(uint32_t now) {
  if (d_.displays() != displays_seen_) {
    displays_seen_ = d_.displays();
    bus_at_ = now;
  }
  if (!job_) {
    if (queue_.empty())
      return;
    job_ = queue_.front();
    queue_.pop_front();
    job_at_ = now;
    presses_ = unacked_ = 0;
    to(Step::START, now);
  }
  if (now - job_at_ >= JOB_MS) {
    finish("failed: timed out");
    return;
  }
  // The state it would act on could be stale, and presses could land once the bus returns
  if (now - bus_at_ >= BUS_MS) {
    finish("failed: no data from the spa bus");
    return;
  }
  if (step_ == Step::PRESSING) {
    bool beeped = d_.beeps() != beeps_before_;
    if (!beeped && now - step_at_ < PRESS_MS)
      return;
    held_.reset();
    to(Step::SETTLING, now);
    return;
  }
  if (step_ == Step::SETTLING) {
    if (d_.beeping() || now - step_at_ < GAP_MS)
      return;
    // A beep that comes after the release still acknowledges the press, as does a new flashing
    // menu value. Without either, start over: the beep may only have been missed, so the state is
    // checked again before any new press.
    if (d_.beeps() == beeps_before_ && d_.menu_seq() == seq_) {
      if (job_->kind == Request::PRESS || ++unacked_ >= MAX_UNACKED)
        finish("failed: no beep, the spa didn't take the press");
      else
        to(Step::START, now);
      return;
    }
    unacked_ = 0;
    presses_++;
    seq_ = d_.menu_seq();
    to(Step::PRESSED, now);
  }
  run(now);
}

void Controller::run(uint32_t now) {
  const Request &r = *job_;
  if (step_ == Step::START) {
    // A menu someone opened on the panel may still change the state; let it settle first
    if (!d_.settled() || d_.displays() < fresh_after_) {
      if (now - step_at_ >= MENU_CLOSE_MS)
        finish("failed: a menu or light on the panel kept flashing");
      return;
    }
    if (r.kind != Request::POWER && r.kind != Request::PRESS && !d_.on(led::POWER)) {
      finish("failed: the spa is off");
      return;
    }
  }
  switch (r.kind) {
    case Request::POWER:
      toggle(Button::POWER, led::POWER, now);
      break;
    case Request::HEATER:
      toggle(Button::HEATER, led::HEATING | led::HEATER_IDLE, now);
      break;
    case Request::BUBBLES:
      toggle(Button::BUBBLES, led::BUBBLES, now);
      break;
    case Request::TEMP:
      set_temperature(now);
      break;
    case Request::SALT:
      set_salt(now);
      break;
    case Request::PRESS:
      if (step_ == Step::START)
        press(Button(r.value), now);
      else
        finish("done, the spa beeped");
      break;
  }
}

void Controller::toggle(Button button, uint16_t led_bits, uint32_t now) {
  bool want = job_->value;
  bool on = d_.leds() & led_bits;
  switch (step_) {
    case Step::START:
      if (on == want)
        finish(want ? "already on" : "already off");
      else if (button == Button::HEATER && d_.on(led::SALT))
        finish("failed: salt is running, and pressing Heater would stop it");
      else
        press(button, now);
      break;
    case Step::PRESSED:
      to(Step::WAIT_LED, now);
      break;
    case Step::WAIT_LED:
      if (on == want)
        finish("done");
      else if (now - step_at_ >= LED_WAIT_MS)
        finish(std::string("failed: pressed, but the ") + button_name(button) + " light didn't change");
      break;
    default:
      break;
  }
}

void Controller::set_temperature(uint32_t now) {
  int want = job_->value;
  auto target = d_.target();
  switch (step_) {
    case Step::START: {
      char unit = d_.unit();
      if (!unit) {
        finish("failed: temperature unit not known yet");
        return;
      }
      bool in_range = unit == 'F' ? want >= 50 && want <= 104 : want >= 10 && want <= 40;
      if (!in_range) {
        finish(unit == 'F' ? "failed: must be 50-104 F" : "failed: must be 10-40 C");
        return;
      }
      if (target == want) {
        finish("already set");
        return;
      }
      // Every press changes the target by one and flashes it; each step reads the value back.
      // With the target unknown, lower is the safer direction.
      press(want > target.value_or(want) ? Button::UP : Button::DOWN, now);
      break;
    }
    case Step::PRESSED:
      to(Step::WAIT_VALUE, now);
      break;
    case Step::WAIT_VALUE: {
      auto shown = d_.menu_seq() != seq_ ? temperature_of(d_.menu_text()) : std::nullopt;
      if (!shown) {
        if (now - step_at_ >= VALUE_WAIT_MS)
          finish("failed: pressed, but no flashing temperature was shown");
      } else if (*shown == want) {
        to(Step::WAIT_MENU_CLOSED, now);
      } else if (presses_ >= MAX_TEMP_PRESSES) {
        finish("failed: too many presses");
      } else {
        press(*shown < want ? Button::UP : Button::DOWN, now);
      }
      break;
    }
    case Step::WAIT_MENU_CLOSED:
      // The panel confirms the new target by showing it for a few seconds
      if (!d_.menu_open())
        finish(target == want ? "done" : "failed: the target changed after it was set");
      else if (now - step_at_ >= MENU_CLOSE_MS)
        finish("failed: the display kept flashing");
      break;
    default:
      break;
  }
}

void Controller::set_salt(uint32_t now) {
  int want = job_->value;
  bool on = d_.on(led::SALT);
  switch (step_) {
    case Step::START:
      // While salt runs, the menu sets a new duration starting now
      if (!want && !on)
        finish("already off");
      else
        press(Button::SALT, now);
      break;
    case Step::PRESSED:
      to(Step::WAIT_VALUE, now);
      break;
    case Step::WAIT_VALUE: {
      // The first press shows the current setting; each further press steps OFF/3H/5H/8H
      auto shown = d_.menu_seq() != seq_ ? hours_of(d_.menu_text()) : std::nullopt;
      if (!shown) {
        if (now - step_at_ >= VALUE_WAIT_MS)
          finish("failed: pressed, but no salt hours were shown");
      } else if (*shown == want) {
        to(Step::WAIT_MENU_CLOSED, now);
      } else if (presses_ >= MAX_SALT_PRESSES) {
        finish("failed: too many presses");
      } else {
        press(Button::SALT, now);
      }
      break;
    }
    case Step::WAIT_MENU_CLOSED:
      // The setting takes effect about 10 s after the last press
      if (!d_.menu_open())
        to(Step::WAIT_LED, now);
      else if (now - step_at_ >= MENU_CLOSE_MS)
        finish("failed: the display kept flashing");
      break;
    case Step::WAIT_LED:
      if (on == (want > 0))
        finish("done");
      else if (now - step_at_ >= LED_WAIT_MS)
        finish("failed: the salt light didn't change");
      break;
    default:
      break;
  }
}

void Controller::press(Button button, uint32_t now) {
  held_ = button;
  beeps_before_ = d_.beeps();
  seq_ = d_.menu_seq();
  to(Step::PRESSING, now);
}

void Controller::lost_data() {
  fresh_after_ = d_.displays() + FRESH_DISPLAYS;
  if (job_)
    finish("failed: the Pico fell behind reading the bus");
}

void Controller::to(Step step, uint32_t now) {
  step_ = step;
  step_at_ = now;
}

void Controller::finish(const std::string &outcome) {
  results_.push_back(describe(*job_) + ": " + outcome);
  job_.reset();
  held_.reset();
}

}  // namespace spa
