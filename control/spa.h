#pragma once

// Hardware-independent part of the controller: decoding SB-HS10-1 bus frames into spa state, and
// turning requests such as "heater=on" or "salt=5" into button presses that are checked against
// what the panel shows. No Pico SDK or Arduino code, so it runs in the host tests.
// Protocol details are in bus-protocol.md in the hottub repo. Frame values are as decoded: bus
// DATA inverted, MSB first.

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace spa {

// Clear in every frame while the panel beeps; the beep acknowledges a press
constexpr uint16_t NO_BEEP = 0x0100;

namespace led {
constexpr uint16_t FRAME = 0x4000;
constexpr uint16_t POWER = 0x0001;
constexpr uint16_t SALT = 0x0010;
constexpr uint16_t HEATING = 0x0080;
// Blinks while salt runs with the heater on (heater suspended). Steady: heater on with the water
// at temperature.
constexpr uint16_t HEATER_IDLE = 0x0200;
constexpr uint16_t BUBBLES = 0x0400;
constexpr uint16_t FILTER = 0x1000;
}  // namespace led

// The poll each button answers
enum class Button : uint16_t {
  BUBBLES = 0x0108,
  FILTER = 0x0102,
  UNIT = 0x2100,
  HEATER = 0x8100,
  POWER = 0x0500,
  UP = 0x1100,
  DOWN = 0x0180,
  SALT = 0x0110,
};

const char *button_name(Button b);

// Frame showing c at display position pos (0 = leftmost), with the beep bit clear; for tests
uint16_t digit_frame(int pos, char c);

// Temperature from "094F" or "038C"
std::optional<int> temperature_of(const std::string &text);
// Hours from "003H", 0 from "0FF "
std::optional<int> hours_of(const std::string &text);

/**
 * Decodes frames into spa state.
 *
 * Digit and LED frames are the ones right after a cue frame; button polls come in a run, which
 * keeps the 0x0140 poll from being read as a blank first digit. A display only counts once it
 * repeats STABLE times, which drops the half-updated displays seen when it changes.
 *
 * The panel flashes a value (alternating it with a blank display) while a menu is open: the
 * target temperature after Up/Down, or the salt hours after Salt. Any other value is shown
 * steadily: the water temperature, alternating with the hours left while salt or filter runs.
 * A value is steady if it lasts STEADY displays, or lasts MIN_STEADY and is not next to a blank.
 * A shorter value next to a blank is a menu value, known when it ends; one that isn't is skipped.
 * The value right after a menu closes follows a blank but lasts, so it is not mistaken for a menu
 * value.
 *
 * A light that toggles twice within BLINK_DISPLAYS is blinking. leds() counts the heater idle
 * light as on while it blinks, since the heater stays on while salt runs. The panel isn't settled
 * until no light has changed for LIGHTS_DISPLAYS, that blinking aside: a light switched on and off
 * quickly at the panel looks like blinking, and when salt ends the heater lights go out for a
 * moment before heating resumes.
 */
class Decoder {
 public:
  void feed(uint16_t raw);

  uint16_t leds() const { return leds_; }  // LED frame without the beep bit, 0 until seen
  bool on(uint16_t led_bit) const { return leds_ & led_bit; }
  uint16_t blinking() const { return blinking_; }
  bool beeping() const { return beeping_; }
  uint32_t beeps() const { return beeps_; }        // beeps started so far
  uint32_t displays() const { return displays_; }  // complete displays decoded, ~84/s
  const std::string &display() const { return stable_; }
  std::optional<int> water() const { return water_; }
  std::optional<int> target() const { return target_; }  // known once the panel has shown it
  char unit() const { return unit_; }                     // 'F' or 'C', 0 until known
  std::optional<int> hours_left() const { return hours_left_; }
  const std::string &error() const { return error_; }  // e.g. "E91", empty if none
  bool menu_open() const { return seen_blank_ && displays_ - last_blank_ < MENU_DISPLAYS; }
  // No menu open, no beep for MENU_DISPLAYS (a press just made on the panel shows no blank yet),
  // and no light change for LIGHTS_DISPLAYS
  bool settled() const {
    return !menu_open() && !beeping_ && displays_ - last_beep_ >= MENU_DISPLAYS &&
           displays_ - lights_changed_ >= LIGHTS_DISPLAYS;
  }
  uint32_t menu_seq() const { return menu_seq_; }  // counts flashing values shown
  const std::string &menu_text() const { return menu_text_; }

 private:
  static constexpr int STABLE = 3;
  static constexpr uint32_t STEADY = 60;          // ~0.7 s; flashing values last up to ~33
  static constexpr uint32_t MIN_STEADY = 30;      // water temperature flickers for as few as 5
  static constexpr uint32_t MENU_DISPLAYS = 100;  // ~1.2 s; blanks come every ~41 while flashing
  static constexpr uint32_t BLINK_DISPLAYS = 120;  // ~1.4 s; blinking lights toggle every ~42
  static constexpr uint32_t LIGHTS_DISPLAYS = 250;  // ~3 s; heating resumed within 1 s of salt ending

  void on_display();
  void new_run(const std::string &text);
  void classify(const std::string &text, bool steady);

  bool after_cue_ = false;
  std::array<char, 4> digits_{};
  int next_pos_ = 0;
  uint16_t leds_ = 0;
  uint16_t raw_leds_ = 0;
  uint16_t blinking_ = 0;
  uint32_t lights_changed_ = 0;  // display count at the last light change
  // Per light, the display count after its last two toggles; 0 if it hasn't toggled
  std::array<uint32_t, 16> toggled_{};
  std::array<uint32_t, 16> toggled_before_{};
  bool beeping_ = false;
  uint32_t beeps_ = 0;
  uint32_t last_beep_ = 0;
  uint32_t displays_ = 0;

  std::string pending_;
  int pending_count_ = 0;
  std::string stable_;
  uint32_t run_start_ = 0;
  bool run_classified_ = true;
  bool run_after_blank_ = false;
  bool seen_blank_ = false;
  uint32_t last_blank_ = 0;

  std::optional<int> water_;
  std::optional<int> target_;
  char unit_ = 0;
  std::optional<int> hours_left_;
  std::string error_;
  uint32_t menu_seq_ = 0;
  std::string menu_text_;
};

struct Request {
  enum Kind : uint8_t { POWER, HEATER, BUBBLES, TEMP, SALT, PRESS } kind;
  int value;  // 1/0 for on/off, a temperature, salt hours, or a Button for PRESS
};

// Parses one "key=value" pair, e.g. heater=on, temp=102, salt=5, press=bubbles. Returns an error
// message, empty on success.
std::string parse_request(const std::string &key, const std::string &value, Request &out);
std::string describe(const Request &r);

/**
 * Carries out requests one at a time by holding buttons, checking every step against the display
 * and LEDs. A request waits for the panel to settle first, so a menu opened there can't change the
 * state afterwards, and fails if no display has arrived for BUS_MS. A press holds the button until
 * the panel beeps or PRESS_MS passes, then waits for the beep to end. After a press without a
 * beep or a new menu value the request starts over, checking the state again; a toggle is never
 * pressed twice without that check, since the second press would undo the first. Never presses anything except
 * for a request. Never blocks: call update() regularly with the time in ms.
 *
 * salt=N while salt runs sets a new N-hour run starting now, as the manual describes. Heater
 * requests that would press Heater fail while salt runs: per the manual, the press stops the salt.
 */
class Controller {
 public:
  static constexpr uint32_t PRESS_MS = 500;
  static constexpr uint32_t GAP_MS = 300;
  static constexpr uint32_t LED_WAIT_MS = 3000;
  static constexpr uint32_t VALUE_WAIT_MS = 2000;
  static constexpr uint32_t MENU_CLOSE_MS = 20000;
  static constexpr uint32_t JOB_MS = 120000;
  static constexpr uint32_t BUS_MS = 1000;
  static constexpr uint32_t FRESH_DISPLAYS = 100;  // ~1.2 s
  static constexpr int MAX_UNACKED = 3;
  static constexpr int MAX_SALT_PRESSES = 6;
  static constexpr int MAX_TEMP_PRESSES = 60;
  static constexpr size_t MAX_QUEUE = 8;

  explicit Controller(const Decoder &decoder) : d_(decoder) {}

  bool queue(const Request &r);  // false if the queue is full
  void update(uint32_t now);
  // Frames were dropped, so the decoded state may be stale: fails the current request, and the
  // next one waits for FRESH_DISPLAYS new displays
  void lost_data();

  std::optional<Button> held() const { return held_; }
  const std::optional<Request> &current() const { return job_; }
  size_t waiting() const { return queue_.size(); }
  // Next finished request and its outcome, e.g. "heater=on: done"
  bool take_result(std::string &out);

 private:
  enum class Step { START, PRESSING, SETTLING, PRESSED, WAIT_VALUE, WAIT_MENU_CLOSED, WAIT_LED };

  void run(uint32_t now);
  void toggle(Button button, uint16_t led_bits, uint32_t now);
  void set_temperature(uint32_t now);
  void set_salt(uint32_t now);
  void press(Button button, uint32_t now);
  void to(Step step, uint32_t now);
  void finish(const std::string &outcome);

  const Decoder &d_;
  std::deque<Request> queue_;
  std::deque<std::string> results_;
  std::optional<Request> job_;
  uint32_t job_at_ = 0;
  Step step_ = Step::START;
  uint32_t step_at_ = 0;
  std::optional<Button> held_;
  uint32_t beeps_before_ = 0;
  int presses_ = 0;
  int unacked_ = 0;
  uint32_t seq_ = 0;
  uint32_t displays_seen_ = 0;
  uint32_t bus_at_ = 0;
  uint32_t fresh_after_ = 0;
};

}  // namespace spa
