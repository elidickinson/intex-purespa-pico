// Host tests for protocol.cpp: make -C tests

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "protocol.h"

using namespace esphome::purespa;

static int failures = 0;

static void check_eq(long actual, long expected, const char *expr, int line) {
  if (actual == expected)
    return;
  std::printf("line %d: %s was %ld, expected %ld\n", line, expr, actual, expected);
  failures++;
}
#define CHECK_EQ(actual, expected) check_eq(actual, expected, #actual, __LINE__)
#define CHECK(cond) check_eq(bool(cond), true, #cond, __LINE__)

// Feeds the same display contents n times
static void show(Decoder &decoder, const char *text, int n) {
  for (int i = 0; i < n; i++) {
    for (int pos = 0; pos < 4; pos++)
      decoder.feed(digit_frame(pos, text[pos]));
  }
}

// Minimal model of the spa mainboard: emits display and LED frames and reacts to held buttons.
// UP/DOWN first shows the target blinking for 5 s; further presses change it and holding repeats.
struct FakeSpa {
  bool power = true;
  bool responsive = true;
  bool filter = false;
  int water = 38;
  int target = 37;
  std::vector<Button> presses;

  std::optional<Button> held;
  uint32_t held_at = 0, repeated_at = 0;
  uint32_t setting_at = 0, setting_until = 0;

  void buttons(std::optional<Button> button, uint32_t now) {
    bool temp_held = held == Button::UP || held == Button::DOWN;
    if (button != held) {
      held = button;
      held_at = repeated_at = now;
      if (button)
        press(*button, now);
    } else if (temp_held && now - held_at >= 500 && now - repeated_at >= 200) {
      repeated_at = now;
      press(*held, now);
    }
  }

  void press(Button button, uint32_t now) {
    presses.push_back(button);
    if (!responsive)
      return;
    if (button == Button::POWER) {
      power = !power;
    } else if (power && button == Button::FILTER) {
      filter = !filter;
    } else if (power && (button == Button::UP || button == Button::DOWN)) {
      if (now < setting_until)
        target = std::clamp(target + (button == Button::UP ? 1 : -1), 20, 40);
      else
        setting_at = now;
      setting_until = now + 5000;
    }
  }

  std::string display(uint32_t now) const {
    char text[5];
    if (!power || (now < setting_until && (now - setting_at) / 500 % 2))
      return "    ";
    std::snprintf(text, sizeof text, "%03dC", now < setting_until ? target : water);
    return text;
  }

  // Frames of one 21 ms cycle, without cue and button frames
  std::vector<uint16_t> frames(uint32_t now) const {
    std::string text = display(now);
    uint16_t leds = led::FRAME | (power ? led::POWER : 0) | (filter ? led::FILTER : 0);
    std::vector<uint16_t> out;
    for (int i = 0; i < 5; i++) {
      for (int pos = 0; pos < 4; pos++)
        out.push_back(digit_frame(pos, text[pos]));
      out.push_back(leds);
    }
    return out;
  }
};

struct Rig {
  FakeSpa spa;
  Decoder decoder;
  Controller controller{decoder};
  uint32_t now = 0;

  void run(uint32_t ms) {
    for (uint32_t end = now + ms; now < end; now += 21) {
      spa.buttons(controller.held(), now);
      for (uint16_t frame : spa.frames(now))
        decoder.feed(frame);
      controller.update(now);
    }
  }

  // Lets the controller learn the target after power-on
  Rig() { run(8000); }
};

static void test_water_temperature_needs_steady_display() {
  Decoder d;
  show(d, "038C", 149);
  CHECK_EQ(d.status().current_temperature, 0);
  show(d, "038C", 1);
  CHECK_EQ(d.status().current_temperature, 38);
}

static void test_fahrenheit_is_rounded() {
  Decoder d;
  show(d, "100F", 150);
  CHECK_EQ(d.status().current_temperature, 38);
}

static void test_error_is_reported_and_cleared() {
  Decoder d;
  show(d, "E90 ", 150);
  CHECK(d.status().error == (std::array<char, 4>{'E', '9', '0', 0}));
  CHECK_EQ(d.status().current_temperature, 0);
  show(d, "038C", 150);
  CHECK_EQ(d.status().error[0], 0);
  CHECK_EQ(d.status().current_temperature, 38);
}

static void test_sanitizer_hours_are_not_a_temperature() {
  Decoder d;
  show(d, "003H", 150);
  CHECK_EQ(d.status().current_temperature, 0);
}

static void test_target_is_probed_once_and_water_temp_not_mistaken_for_it() {
  Rig r;
  CHECK_EQ(r.decoder.status().current_temperature, 38);
  CHECK_EQ(r.decoder.status().target_temperature, 37);
  CHECK_EQ(r.spa.presses.size(), 1);
  CHECK(r.spa.presses[0] == Button::DOWN);
  r.run(20000);
  CHECK_EQ(r.decoder.status().target_temperature, 37);
  CHECK_EQ(r.spa.presses.size(), 1);
}

static void test_spa_is_never_switched_on() {
  Rig r;
  r.spa.power = false;
  r.spa.presses.clear();
  r.run(20000);
  CHECK_EQ(r.spa.presses.size(), 0);
  CHECK(!r.controller.set_target(30, r.now));
}

static void test_target_is_raised_and_lowered() {
  Rig r;
  CHECK(r.controller.set_target(40, r.now));
  r.run(30000);
  CHECK(!r.controller.adjusting());
  CHECK_EQ(r.spa.target, 40);
  CHECK_EQ(r.decoder.status().target_temperature, 40);

  CHECK(r.controller.set_target(25, r.now));
  r.run(30000);
  CHECK(!r.controller.adjusting());
  CHECK_EQ(r.spa.target, 25);
  CHECK_EQ(r.decoder.status().target_temperature, 25);
  CHECK(!r.spa.held);
}

static void test_setting_current_target_finishes_at_once() {
  Rig r;
  r.spa.presses.clear();
  CHECK(r.controller.set_target(37, r.now));
  r.run(100);
  CHECK(!r.controller.adjusting());
  CHECK_EQ(r.spa.presses.size(), 0);
}

static void test_setting_target_gives_up_when_spa_ignores_buttons() {
  Rig r;
  r.spa.responsive = false;
  CHECK(r.controller.set_target(40, r.now));
  r.run(Controller::TARGET_TIMEOUT_MS + 100);
  CHECK(!r.controller.adjusting());
  CHECK(!r.controller.held());
}

static void test_queued_presses_are_separate() {
  Rig r;
  r.spa.presses.clear();
  r.controller.press(Button::FILTER);
  r.controller.press(Button::FILTER);
  r.controller.press(Button::FILTER);
  r.run(1000);
  CHECK_EQ(r.spa.presses.size(), 3);
  CHECK(r.spa.filter);
  CHECK(r.decoder.status().leds & led::FILTER);
}

int main() {
  test_water_temperature_needs_steady_display();
  test_fahrenheit_is_rounded();
  test_error_is_reported_and_cleared();
  test_sanitizer_hours_are_not_a_temperature();
  test_target_is_probed_once_and_water_temp_not_mistaken_for_it();
  test_spa_is_never_switched_on();
  test_target_is_raised_and_lowered();
  test_setting_current_target_finishes_at_once();
  test_setting_target_gives_up_when_spa_ignores_buttons();
  test_queued_presses_are_separate();
  std::printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures != 0;
}
