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

// Minimal spa model: sanitizer presses cycle off -> 3 -> 5 -> 8 -> off and blink the H display
// for 5 s; UP/DOWN first shows the target blinking for 5 s, then changes it and repeats while held.
struct FakeSpa {
  bool power = true;
  bool responsive = true;
  bool filter = false;
  bool sanitizer_on = false;
  uint8_t sanitizer_hours = 0;
  int water = 38;
  int target = 37;
  uint32_t blink_period = 500;
  std::vector<uint16_t> presses;

  std::optional<uint16_t> held;
  uint32_t held_at = 0, repeated_at = 0;
  uint32_t setting_at = 0, setting_until = 0;
  uint32_t sanitizer_setting_at = 0, sanitizer_setting_until = 0;

  void buttons(std::optional<uint16_t> button, uint32_t now) {
    bool temp_held = held == static_cast<uint16_t>(Button::UP) || held == static_cast<uint16_t>(Button::DOWN);
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

  void press(uint16_t button, uint32_t now) {
    presses.push_back(button);
    if (!responsive)
      return;
    if (button == static_cast<uint16_t>(Button::POWER)) {
      power = !power;
    } else if (power && button == static_cast<uint16_t>(Button::FILTER)) {
      filter = !filter;
    } else if (power && button == static_cast<uint16_t>(Button::SANITIZER)) {
      if (!sanitizer_on) {
        sanitizer_on = true;
        sanitizer_hours = 3;
      } else if (sanitizer_hours == 3) {
        sanitizer_hours = 5;
      } else if (sanitizer_hours == 5) {
        sanitizer_hours = 8;
      } else {
        sanitizer_on = false;
        sanitizer_hours = 0;
      }
      sanitizer_setting_at = now;
      sanitizer_setting_until = now + 5000;
    } else if (power && (button == static_cast<uint16_t>(Button::UP) ||
                         button == static_cast<uint16_t>(Button::DOWN))) {
      if (now < setting_until)
        target = std::clamp(target + (button == static_cast<uint16_t>(Button::UP) ? 1 : -1), 20, 40);
      else
        setting_at = now;
      setting_until = now + 5000;
    }
  }

  std::string display(uint32_t now) const {
    char text[5];
    if (!power || (now < setting_until && ((now - setting_at) / blink_period) % 2) ||
        (sanitizer_on && now < sanitizer_setting_until &&
         ((now - sanitizer_setting_at) / blink_period) % 2))
      return "    ";
    if (sanitizer_on) {
      std::snprintf(text, sizeof text, "%03uH", sanitizer_hours);
      return text;
    }
    std::snprintf(text, sizeof text, "%03dC", now < setting_until ? target : water);
    return text;
  }

  // Frames of one 21 ms cycle, without cue and button frames
  std::vector<uint16_t> frames(uint32_t now) const {
    std::string text = display(now);
    uint16_t leds = led::FRAME | (power ? led::POWER : 0) | (filter ? led::FILTER : 0) |
                    (sanitizer_on ? led::SANITIZER : 0);
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

static void test_sanitizer_hours_are_decoded_and_not_temperature_or_error() {
  Decoder d;
  show(d, "005H", 150);
  CHECK_EQ(d.status().sanitizer_hours, 5);
  CHECK_EQ(d.status().current_temperature, 0);
  CHECK_EQ(d.status().error[0], 0);
  show(d, "E91 ", 150);
  CHECK(d.status().error == (std::array<char, 4>{'E', '9', '1', 0}));
  show(d, "005H", 150);
  CHECK_EQ(d.status().current_temperature, 0);
  CHECK(d.status().error == (std::array<char, 4>{'E', '9', '1', 0}));
}

static void test_out_of_order_digit_frames_are_ignored() {
  Decoder d;
  // Valid temperature first, then stray out-of-order frames
  show(d, "038C", 150);
  d.feed(digit_frame(2, '5'));
  d.feed(digit_frame(1, '9'));
  show(d, "038C", 150);
  CHECK_EQ(d.status().current_temperature, 38);
  CHECK_EQ(d.status().target_temperature, 0);
}

static void test_slow_blink_is_not_mistaken_for_water_temperature() {
  Decoder d;
  // Blank for a long time, then a value that stays: that value is the water temperature
  // (a real blink value alternates with blanks)
  show(d, "    ", 200);
  CHECK_EQ(d.status().current_temperature, 0);
  show(d, "040C", 200);
  CHECK_EQ(d.status().current_temperature, 40);
  // The long-blank phase did not register as a blink, so no target was captured
  CHECK_EQ(d.status().target_temperature, 0);
}

static void test_target_is_read_during_blinks_only() {
  Decoder d;
  show(d, "038C", 150);  // learn water temp
  // Blink: blank, then value. The value becomes a candidate, confirmed as target by the next blank.
  show(d, "    ", 3);
  show(d, "040C", 3);
  CHECK_EQ(d.status().target_temperature, 0);
  show(d, "    ", 3);
  CHECK_EQ(d.status().target_temperature, 40);
  CHECK_EQ(d.status().current_temperature, 38);
  // The same value seen again in the next blink phase confirms it at once
  show(d, "040C", 3);
  CHECK_EQ(d.status().target_temperature, 40);
  // A changed value in a blink confirms on the next blank
  show(d, "    ", 3);
  show(d, "041C", 3);
  show(d, "    ", 3);
  CHECK_EQ(d.status().target_temperature, 41);
  // Blink ends with a steady temperature: water temp again, never the target
  // (the candidate 41 was confirmed by the blank; 42 persists = water temp)
  show(d, "042C", 150);
  CHECK_EQ(d.status().current_temperature, 42);
  CHECK_EQ(d.status().target_temperature, 41);
}

static void test_target_is_probed_once_and_water_temp_not_mistaken_for_it() {
  Rig r;
  CHECK_EQ(r.decoder.status().current_temperature, 38);
  CHECK_EQ(r.decoder.status().target_temperature, 37);
  // One DOWN probe at boot (possibly retried once), no target changes
  CHECK(r.spa.presses.size() <= 2);
  CHECK(r.spa.presses[0] == static_cast<uint16_t>(Button::DOWN));
  r.run(20000);
  CHECK_EQ(r.decoder.status().target_temperature, 37);
  CHECK(r.spa.presses.size() <= 2);
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
  r.run(Controller::TARGET_TIMEOUT_MS + 500);
  CHECK(!r.controller.adjusting());
  CHECK(!r.controller.held());
}

static void test_target_is_probed_again_after_swallowed_press() {
  Rig r;
  r.spa.power = false;  // no probe during construction
  r.run(2000);
  r.spa.presses.clear();
  // Wipe the learned state: a fresh power-up knows nothing
  r.controller.~Controller();
  new (&r.controller) Controller(r.decoder);
  r.decoder.~Decoder();
  new (&r.decoder) Decoder();
  r.spa.power = true;
  r.spa.responsive = false;  // presses are swallowed
  r.run(Controller::PROBE_RETRY_MS + 2000);
  CHECK(r.spa.presses.size() >= 2);  // repeated probe presses
  r.spa.responsive = true;
  r.run(8000);
  CHECK_EQ(r.decoder.status().target_temperature, 37);
}

static void test_slow_blink_does_not_confuse_controller() {
  // A slower blink (600 ms phases): still under the 630 ms steady threshold, the controller
  // must converge
  Rig r;
  r.spa.blink_period = 600;
  CHECK(r.controller.set_target(40, r.now));
  r.run(60000);
  CHECK_EQ(r.spa.target, 40);
  CHECK_EQ(r.decoder.status().target_temperature, 40);
  CHECK(r.decoder.status().current_temperature <= 40);  // water temp, not the target
}

static void test_single_sanitizer_press_turns_on_led_state() {
  Rig r;
  r.spa.presses.clear();
  r.controller.press(Button::SANITIZER);
  r.run(2000);
  CHECK(r.decoder.status().leds & led::SANITIZER);
  CHECK_EQ(r.decoder.status().sanitizer_hours, 3);
  CHECK_EQ(r.spa.presses.size(), 1);
}

static void test_sanitizer_hours_converge_without_overshoot() {
  Rig r;
  r.spa.presses.clear();
  CHECK(r.controller.set_sanitizer_hours(5, r.now));
  r.run(10000);
  CHECK(r.spa.sanitizer_on);
  CHECK_EQ(r.spa.sanitizer_hours, 5);
  CHECK_EQ(r.decoder.status().sanitizer_hours, 5);
  CHECK_EQ(r.spa.presses.size(), 2);
  CHECK(!r.controller.held());
  CHECK(!r.controller.adjusting_sanitizer());
}

static void test_setting_current_sanitizer_hours_issues_no_presses() {
  Rig r;
  r.spa.sanitizer_on = true;
  r.spa.sanitizer_hours = 5;
  r.run(2000);
  r.spa.presses.clear();
  CHECK(r.controller.set_sanitizer_hours(5, r.now));
  r.run(100);
  CHECK_EQ(r.spa.presses.size(), 0);
  CHECK(!r.controller.adjusting_sanitizer());
}

static void test_sanitizer_turns_off_after_cycle() {
  Rig r;
  r.spa.sanitizer_on = true;
  r.spa.sanitizer_hours = 3;
  r.run(2000);
  r.spa.presses.clear();
  CHECK(r.controller.set_sanitizer_hours(0, r.now));
  r.run(10000);
  CHECK(!r.spa.sanitizer_on);
  CHECK(!(r.decoder.status().leds & led::SANITIZER));
  CHECK_EQ(r.spa.presses.size(), 3);
  CHECK(!r.controller.held());
}

static void test_unresponsive_sanitizer_times_out_and_releases() {
  Rig r;
  r.spa.responsive = false;
  r.spa.presses.clear();
  CHECK(r.controller.set_sanitizer_hours(5, r.now));
  r.run(Controller::SANITIZER_TIMEOUT_MS + 500);
  CHECK(!r.controller.adjusting_sanitizer());
  CHECK(!r.controller.held());
  CHECK(std::count(r.spa.presses.begin(), r.spa.presses.end(),
                   (uint16_t) Button::SANITIZER) >= 2);  // sanitizer presses, not the boot probe
}

static void test_sanitizer_aborts_after_unseen_presses() {
  // A wrong sanitizer button code or LED mask makes every press look swallowed; the flow must
  // abort after MAX_UNSEEN_PRESSES instead of cycling the real spa around the whole ring.
  Rig r;
  r.spa.presses.clear();
  r.spa.responsive = false;
  CHECK(r.controller.set_sanitizer_hours(5, r.now));
  r.run(Controller::SANITIZER_TIMEOUT_MS + 500);
  CHECK(std::count(r.spa.presses.begin(), r.spa.presses.end(), (uint16_t) Button::SANITIZER) <=
        (int) Controller::MAX_UNSEEN_PRESSES);
}

static void test_sanitizer_on_at_unknown_hours_still_reaches_goal() {
  // Sanitizer is already on but the display has not revealed the hours; setting 5 must not
  // get stuck: after the bounded wait it presses through the cycle and lands on 5h.
  Rig r;
  r.spa.presses.clear();
  r.spa.sanitizer_on = true;   // on, hours unknown to the decoder (display steady water)
  r.spa.sanitizer_hours = 3;
  r.spa.presses.clear();
  CHECK(r.controller.set_sanitizer_hours(5, r.now));
  r.run(15000);
  CHECK_EQ(r.spa.sanitizer_hours, 5);
  CHECK(r.spa.sanitizer_on);
  CHECK(!r.controller.adjusting_sanitizer());
}

static void test_sanitizer_request_is_rejected_during_temperature_adjustment() {
  Rig r;
  CHECK(r.controller.set_target(40, r.now));
  CHECK(!r.controller.set_sanitizer_hours(5, r.now));
  r.run(30000);
  CHECK_EQ(r.spa.sanitizer_on, false);
  CHECK(!r.spa.presses.empty());
  CHECK(std::find(r.spa.presses.begin(), r.spa.presses.end(), static_cast<uint16_t>(Button::SANITIZER)) ==
        r.spa.presses.end());
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
  test_sanitizer_hours_are_decoded_and_not_temperature_or_error();
  test_out_of_order_digit_frames_are_ignored();
  test_slow_blink_is_not_mistaken_for_water_temperature();
  test_target_is_read_during_blinks_only();
  test_target_is_probed_once_and_water_temp_not_mistaken_for_it();
  test_spa_is_never_switched_on();
  test_target_is_raised_and_lowered();
  test_setting_current_target_finishes_at_once();
  test_setting_target_gives_up_when_spa_ignores_buttons();
  test_target_is_probed_again_after_swallowed_press();
  test_slow_blink_does_not_confuse_controller();
  test_single_sanitizer_press_turns_on_led_state();
  test_sanitizer_hours_converge_without_overshoot();
  test_setting_current_sanitizer_hours_issues_no_presses();
  test_sanitizer_turns_off_after_cycle();
  test_unresponsive_sanitizer_times_out_and_releases();
  test_sanitizer_aborts_after_unseen_presses();
  test_sanitizer_on_at_unknown_hours_still_reaches_goal();
  test_sanitizer_request_is_rejected_during_temperature_adjustment();
  test_queued_presses_are_separate();
  std::printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures != 0;
}
