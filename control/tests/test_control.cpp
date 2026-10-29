// Host tests for spa.cpp and schedule.cpp: make -C tests

#include <cstdio>
#include <string>
#include <vector>

#include "schedule.h"
#include "spa.h"

using namespace spa;

static int failures = 0;

static std::string show(const std::string &s) { return "'" + s + "'"; }
static std::string show(const char *s) { return show(std::string(s)); }
static std::string show(long v) { return std::to_string(v); }
static std::string show(std::optional<int> v) { return v ? std::to_string(*v) : "none"; }
static std::string show(std::nullopt_t) { return "none"; }

static void check(bool ok, const char *expr, int line) {
  if (ok)
    return;
  std::printf("line %d: %s\n", line, expr);
  failures++;
}

// Evaluates actual once: it is often a request that presses buttons
template <typename A, typename B>
static void check_eq(const A &actual, const B &expected, const char *expr, int line) {
  if (actual == expected)
    return;
  std::printf("line %d: %s was %s, expected %s\n", line, expr, show(actual).c_str(), show(expected).c_str());
  failures++;
}
#define CHECK(cond) check(cond, #cond, __LINE__)
#define CHECK_EQ(actual, expected) check_eq(actual, expected, #actual, __LINE__)

// Frames from spa-capture-5.log, 30-92 s: spa off at 093F; power on; salt menu stepped 3H, 5H,
// 8H, OFF; bubbles on and off; salt menu OFF, 3H, after which salt starts and the display
// alternates the water temperature with 003H. Nine beeps in all.
static void test_real_capture() {
  FILE *in = popen("gzip -dc power-salt-bubbles.txt.gz", "r");
  Decoder d;
  std::vector<std::string> menu;
  std::vector<uint16_t> leds;
  uint32_t seq = 0;
  bool water_out_of_range = false;
  unsigned frame;
  while (std::fscanf(in, "%x", &frame) == 1) {
    d.feed(frame);
    if (d.menu_seq() != seq) {
      seq = d.menu_seq();
      if (menu.empty() || menu.back() != d.menu_text())
        menu.push_back(d.menu_text());
    }
    if (d.leds() && (leds.empty() || leds.back() != d.leds()))
      leds.push_back(d.leds());
    if (d.water() && (*d.water() < 85 || *d.water() > 95))
      water_out_of_range = true;
  }
  pclose(in);
  CHECK(d.displays() > 5000);
  CHECK((menu == std::vector<std::string>{"003H", "005H", "008H", "0FF ", "003H"}));
  CHECK((leds == std::vector<uint16_t>{0x4008, 0x4009, 0x4409, 0x4009, 0x5011}));
  CHECK_EQ(long(d.beeps()), 9L);
  CHECK(!water_out_of_range);
  CHECK(d.water().has_value());
  CHECK_EQ(long(d.unit()), long('F'));
  CHECK_EQ(d.target(), std::nullopt);
  CHECK_EQ(d.hours_left(), 3);
  CHECK(!d.menu_open());
  CHECK_EQ(d.error(), "");
}

// Spa model, one display cycle per call (~12 ms). A button counts as pressed after the bus has
// replied to 4 polls in a row, and only again after a poll without a reply; each press beeps for
// 12 cycles. Each Up/Down press changes the target and flashes it. Salt opens a flashing menu
// showing the current setting; further presses change it, and it takes effect when the menu
// closes.
struct FakeSpa {
  bool responsive = true;
  int beep_delay = 0;  // cycles between a press registering and its beep
  int beep_in = -1;
  bool power = false, heater = false, bubbles = false, salt_on = false;
  int water = 94, target = 100, salt_setting = 3, salt_left = 0;
  enum { NONE, TEMP, SALT } menu = NONE;
  uint32_t menu_at = 0;
  uint32_t cycle = 0;
  int beep = 0;
  int replies = 0;
  bool released = true;
  std::vector<Button> presses;

  std::string text() const {
    char s[8];
    uint32_t since = cycle - menu_at;
    if (menu == TEMP)
      std::snprintf(s, sizeof(s), "%03dF", target);
    else if (menu == SALT && salt_setting == 0)
      return since % 40 < 16 ? "0FF " : "    ";
    else if (menu == SALT)
      std::snprintf(s, sizeof(s), "%03dH", salt_setting);
    else if (salt_on && cycle / 170 % 2)
      std::snprintf(s, sizeof(s), "%03dH", salt_left);
    else
      std::snprintf(s, sizeof(s), "%03dF", water);
    if (menu != NONE && since % 40 >= 16)
      return menu == TEMP ? "   F" : "    ";
    return s;
  }

  uint16_t leds() const {
    return led::FRAME | (power ? led::POWER : 0) | (heater ? led::HEATING | led::FILTER : 0) |
           (bubbles ? led::BUBBLES : 0) | (salt_on ? led::SALT | led::FILTER : 0x0008);
  }

  void press(Button b) {
    presses.push_back(b);
    beep_in = beep_delay;
    if (b == Button::POWER) {
      power = !power;
      heater = bubbles = salt_on = false;
      menu = NONE;
    } else if (!power) {
      return;
    } else if (b == Button::HEATER) {
      heater = !heater;
      salt_on = salt_on && !heater;
    } else if (b == Button::BUBBLES) {
      bubbles = !bubbles;
    } else if (b == Button::UP || b == Button::DOWN) {
      target = std::min(104, std::max(50, target + (b == Button::UP ? 1 : -1)));
      menu = TEMP;
      menu_at = cycle;
    } else if (b == Button::SALT) {
      if (menu == SALT)
        salt_setting = salt_setting == 0 ? 3 : salt_setting == 3 ? 5 : salt_setting == 5 ? 8 : 0;
      menu = SALT;
      menu_at = cycle;
    }
  }

  void step(Decoder &d, const Controller &c) {
    cycle++;
    if (menu != NONE && cycle - menu_at > (menu == TEMP ? 420u : 840u)) {
      if (menu == SALT) {
        salt_on = salt_setting > 0;
        salt_left = salt_setting;
        heater = heater && !salt_on;
      }
      menu = NONE;
    }
    if (beep_in >= 0 && beep_in-- == 0)
      beep = 12;
    uint16_t nb = beep > 0 ? 0 : NO_BEEP;
    if (beep > 0)
      beep--;
    std::string t = text();
    for (int pos = 0; pos < 4; pos++) {
      d.feed(nb);
      d.feed(digit_frame(pos, t[pos]) | nb);
    }
    d.feed(nb);
    d.feed(leds() | nb);
    d.feed(nb);
    // The bus only replies to a poll that matches exactly, so not while the beep bit is clear
    std::optional<Button> replied;
    for (Button b : {Button::BUBBLES, Button::FILTER, Button::UNIT, Button::HEATER, Button::POWER, Button::UP,
                     Button::DOWN, Button::SALT}) {
      d.feed((uint16_t(b) & ~NO_BEEP) | nb);
      if (nb && c.held() == b)
        replied = b;
    }
    d.feed((0x0140 & ~NO_BEEP) | nb);
    if (!replied) {
      replies = 0;
      released = true;
    } else if (++replies >= 4 && released && responsive) {
      released = false;
      press(*replied);
    }
  }
};

struct Rig {
  Decoder d;
  Controller c{d};
  FakeSpa spa;
  uint32_t now = 0;
  int held_while_beeping = 0;

  void run_ms(uint32_t ms) {
    for (uint32_t end = now + ms; now < end; now += 12)
      step();
  }

  void step() {
    spa.step(d, c);
    c.update(now);
    held_while_beeping = d.beeping() && c.held() ? held_while_beeping + 1 : 0;
    CHECK(held_while_beeping <= 1);
  }

  std::string request(const std::string &key, const std::string &value) {
    Request r;
    std::string error = parse_request(key, value, r);
    if (!error.empty())
      return error;
    CHECK(c.queue(r));
    std::string result;
    for (uint32_t end = now + 200000; now < end; now += 12) {
      step();
      if (c.take_result(result))
        return result;
    }
    return "no result";
  }
};

static Rig powered_on() {
  Rig rig;
  rig.spa.power = true;
  rig.run_ms(2000);
  return rig;
}

static void test_nothing_is_pressed_without_a_request() {
  Rig rig = powered_on();
  rig.run_ms(10000);
  CHECK(rig.spa.presses.empty());
  CHECK_EQ(rig.d.water(), 94);
}

static void test_power_on() {
  Rig rig;
  rig.run_ms(2000);
  CHECK_EQ(rig.request("power", "on"), "power=on: done");
  CHECK(rig.spa.power);
  CHECK_EQ(long(rig.spa.presses.size()), 1L);
}

static void test_heater_on_then_already_on() {
  Rig rig = powered_on();
  CHECK_EQ(rig.request("heater", "on"), "heater=on: done");
  CHECK_EQ(rig.request("heater", "on"), "heater=on: already on");
  CHECK_EQ(long(rig.spa.presses.size()), 1L);
}

static void test_bubbles_refused_while_spa_is_off() {
  Rig rig;
  rig.run_ms(2000);
  CHECK_EQ(rig.request("bubbles", "on"), "bubbles=on: failed: the spa is off");
  CHECK(rig.spa.presses.empty());
}

static void test_salt_on_steps_from_current_setting() {
  Rig rig = powered_on();
  CHECK_EQ(rig.request("salt", "5"), "salt=5: done");
  CHECK(rig.spa.salt_on);
  CHECK_EQ(rig.spa.salt_left, 5);
  CHECK_EQ(long(rig.spa.presses.size()), 2L);  // open showing 3H, then 5H
}

static void test_salt_off_while_running() {
  Rig rig = powered_on();
  rig.spa.salt_on = true;
  rig.spa.salt_left = 3;
  rig.run_ms(5000);
  CHECK_EQ(rig.d.hours_left(), 3);
  CHECK_EQ(rig.request("salt", "off"), "salt=off: done");
  CHECK(!rig.spa.salt_on);
  CHECK_EQ(long(rig.spa.presses.size()), 4L);  // open showing 3H, 5H, 8H, OFF
}

static void test_temperature_is_read_back_and_set() {
  Rig rig = powered_on();
  CHECK_EQ(rig.d.target(), std::nullopt);
  CHECK_EQ(rig.request("temp", "102"), "temp=102: done");
  CHECK_EQ(rig.spa.target, 102);
  CHECK_EQ(rig.d.target(), 102);
  CHECK_EQ(long(rig.spa.presses.size()), 4L);  // target unknown, so Down first: 99, 100, 101, 102
  CHECK_EQ(rig.request("temp", "102"), "temp=102: already set");
  CHECK_EQ(long(rig.spa.presses.size()), 4L);
  CHECK_EQ(rig.request("temp", "99"), "temp=99: done");
  CHECK_EQ(rig.spa.target, 99);
}

static void test_temperature_limits() {
  Rig rig = powered_on();
  CHECK_EQ(rig.request("temp", "105"), "temp must be a whole number, 50-104 F or 10-40 C");
  CHECK_EQ(rig.request("temp", "40"), "temp=40: failed: must be 50-104 F");
  CHECK(rig.spa.presses.empty());
}

static void test_unanswered_press_is_retried_then_given_up() {
  Rig rig = powered_on();
  rig.spa.responsive = false;
  CHECK_EQ(rig.request("heater", "on"), "heater=on: failed: no beep, the spa didn't take the press");
  CHECK(!rig.c.held());
  rig.spa.responsive = true;
  CHECK_EQ(rig.request("heater", "on"), "heater=on: done");
}

static void test_late_beep_is_not_retried() {
  Rig rig = powered_on();
  rig.spa.beep_delay = 45;  // ~540 ms, after the controller has let go
  CHECK_EQ(rig.request("bubbles", "on"), "bubbles=on: done");
  CHECK_EQ(long(rig.spa.presses.size()), 1L);
}

static void test_waits_for_a_menu_opened_on_the_panel() {
  Rig rig = powered_on();
  rig.spa.press(Button::SALT);  // someone opens the salt menu, showing 3H
  CHECK_EQ(rig.request("salt", "off"), "salt=off: done");
  CHECK(!rig.spa.salt_on);
}

static void test_salt_while_running_sets_a_new_duration() {
  Rig rig = powered_on();
  rig.spa.salt_on = true;
  rig.spa.salt_left = 2;
  CHECK_EQ(rig.request("salt", "8"), "salt=8: done");
  CHECK_EQ(rig.spa.salt_left, 8);
}

// Two menu values before the first blank, as when Up is pressed twice quickly
static void test_quickly_changed_value_is_not_water() {
  Decoder d;
  auto display = [&](const char *text, int n) {
    for (int i = 0; i < n; i++) {
      for (int pos = 0; pos < 4; pos++) {
        d.feed(NO_BEEP);
        d.feed(digit_frame(pos, text[pos]) | NO_BEEP);
      }
    }
  };
  display("094F", 100);
  display("100F", 10);
  display("101F", 16);
  display("    ", 25);
  CHECK_EQ(d.water(), 94);
  CHECK_EQ(d.target(), 101);
}

// As seen on the tub: the blank phase keeps the F, and the water returns straight after a blank
static void test_temperature_menu_is_read() {
  Decoder d;
  auto display = [&](const char *text, int n) {
    for (int i = 0; i < n; i++) {
      for (int pos = 0; pos < 4; pos++) {
        d.feed(NO_BEEP);
        d.feed(digit_frame(pos, text[pos]) | NO_BEEP);
      }
    }
  };
  display("094F", 100);
  for (const char *value : {"100F", "101F", "101F"}) {
    display(value, 17);
    display("   F", 25);
  }
  CHECK(d.menu_open());
  CHECK_EQ(d.target(), 101);
  display("093F", 150);
  CHECK_EQ(d.water(), 93);
  CHECK_EQ(d.target(), 101);
  CHECK(!d.menu_open());
}

static void test_requests_run_in_order() {
  Rig rig;
  rig.run_ms(2000);
  Request r;
  parse_request("power", "on", r);
  rig.c.queue(r);
  parse_request("bubbles", "on", r);
  rig.c.queue(r);
  rig.run_ms(10000);
  std::string a, b;
  CHECK(rig.c.take_result(a) && rig.c.take_result(b));
  CHECK_EQ(a, "power=on: done");
  CHECK_EQ(b, "bubbles=on: done");
  CHECK(rig.spa.bubbles);
}

static void test_schedule() {
  std::vector<Entry> entries;
  CHECK_EQ(parse_schedule("# comment\n\nfri 15:00 power=on heater=on  # warm up\nDaily 02:00 salt=3\n"
                          "sat,sun 09:30 temp=102\n",
                          entries),
           "");
  CHECK_EQ(long(entries.size()), 3L);
  CHECK_EQ(long(entries[0].requests.size()), 2L);
  CHECK(due(entries[0], 5, 15, 0));
  CHECK(!due(entries[0], 4, 15, 0));
  CHECK(!due(entries[0], 5, 15, 1));
  CHECK(due(entries[1], 3, 2, 0));
  CHECK(due(entries[2], 0, 9, 30) && due(entries[2], 6, 9, 30) && !due(entries[2], 1, 9, 30));
  CHECK_EQ(describe(entries[1].requests[0]), "salt=3");

  CHECK_EQ(parse_schedule("daily 02:00 salt=3\nfry 15:00 heater=on\n", entries),
           "line 2: unknown day 'fry', use daily, weekdays, weekends or e.g. mon,wed,fri");
  CHECK_EQ(long(entries.size()), 3L);  // unchanged on error
  CHECK_EQ(parse_schedule("daily 25:00 heater=on", entries), "line 1: time must be HH:MM, 24-hour, e.g. 07:30 or 19:00");
  CHECK_EQ(parse_schedule("daily 7:00 heater=on", entries), "line 1: time must be HH:MM, 24-hour, e.g. 07:30 or 19:00");
  CHECK_EQ(parse_schedule("daily 07:00", entries), "line 1: expected at least one setting, e.g. heater=on");
  CHECK_EQ(parse_schedule("daily 07:00 heater=maybe", entries), "line 1: heater must be on or off");
}

int main() {
  test_real_capture();
  test_nothing_is_pressed_without_a_request();
  test_power_on();
  test_heater_on_then_already_on();
  test_bubbles_refused_while_spa_is_off();
  test_salt_on_steps_from_current_setting();
  test_salt_off_while_running();
  test_temperature_is_read_back_and_set();
  test_temperature_limits();
  test_unanswered_press_is_retried_then_given_up();
  test_late_beep_is_not_retried();
  test_waits_for_a_menu_opened_on_the_panel();
  test_salt_while_running_sets_a_new_duration();
  test_quickly_changed_value_is_not_water();
  test_temperature_menu_is_read();
  test_requests_run_in_order();
  test_schedule();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
