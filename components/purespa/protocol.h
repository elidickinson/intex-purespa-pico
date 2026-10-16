#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>

/**
 * Hardware-independent part of the SB-H20 protocol: decoding received frames and deciding which
 * button the bus should reply to. Contains no Pico SDK or ESPHome code so it can be tested on a host.
 *
 * The Intex serial protocol between the mainboard of the SB-H20 model and its
 * control panel uses a 16-bit data frame where each bit is sampled by the
 * rising clock signal and the frame is sampled by the rising slave select
 * signal, similar to SPI mode 3 but unidirectional. The clock speed is 100 kHz,
 * the data bits are inverted and the bit order is big endian (MSB).
 *
 * There are 4 telegram types, one for the 7-segment display, one for the LEDs,
 * one for the buttons and the cue telegram that is send each time the telegram
 * type changes. Only the button telegram is used for sending commands to the
 * SB-H20. All other telegrams are receive telegrams.
 *
 * To signal when a button is pressed the data line must be pulled low for
 * 2 microseconds after the slave select signal turns high if a switch telegram
 * has been received that matches the button function.
 *
 *        |-------------------------------------------------------------------------------|
 * BIT    | 15 | 14 | 13 | 12 | 11 | 10 |  9 |  8 |  7 |  6 |  5 |  4 |  3 |  2 |  1 |  0 |
 *        |-------------------------------------------------------------------------------|
 * CUE    |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  1 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |
 * DIGIT  | DP |  x |  A |  B | S3 |  D |  C |  x |  E | S1 | S2 |  G |  F | S4 |  x |  x |
 * LED    |  0 |  1 |  0 |  x |  0 |  x |  x |  x |  x |  0 |  0 |  0 |  0 |  0 |  0 |  x |
 * BUTTON |  x |  0 |  x |  x |  0 |  x |  0 |  1 |  0 |  0 |  0 |  0 |  x |  0 |  0 |  0 |
 *        |-------------------------------------------------------------------------------|
 *
 * display:
 *
 * bits S1, S2, S3 and S4 each select a 7-segment display, digits are from left to right
 *
 * DP .    A
 *       -----
 *      |     |
 *    F |     | B
 *      |     |
 *       --G--
 *      |     |
 *    E |     | C
 *      |     |
 *       -----
 *         D
 *
 * telegram order:
 *
 * 32 frames, repeating every 21 ms (5x digit 1-4, 5x LEDs, 1x buttons 1-7)
 *
 * C D1 C D2 C D3 C D4 C L
 * C D1 C D2 C D3 C D4 C L
 * C D1 C D2 C D3 C D4 C L
 * C D1 C D2 C D3 C D4 C L
 * C D1 C D2 C D3 C D4 C L
 * C B B B B B B B L
 *
 * All frame values below are as decoded, i.e. after inverting the bus data.
 */

namespace esphome {
namespace purespa {

namespace led {
constexpr uint16_t FRAME = 0x4000;
constexpr uint16_t POWER = 0x0001;
constexpr uint16_t HEATER = 0x0080;
constexpr uint16_t NO_BEEP = 0x0100;
constexpr uint16_t STANDBY = 0x0200;
constexpr uint16_t BUBBLE = 0x0400;
constexpr uint16_t FILTER = 0x1000;
}  // namespace led

// Value of the button frame the bus replies to in order to press the button
enum class Button : uint16_t {
  FILTER = 0x0102,
  HEATER = 0x8100,
  UP = 0x1100,
  DOWN = 0x0180,
  BUBBLE = 0x0108,
  POWER = 0x0500,
  UNIT = 0x2100,
};

// Display contents as characters, e.g. "038C", "E90 ", "003H" or "    "
using Display = std::array<char, 4>;

// Frame that shows character c at display position pos (0 = leftmost)
uint16_t digit_frame(int pos, char c);

struct Status {
  uint8_t current_temperature = 0;  // °C, 0 until known
  uint8_t target_temperature = 0;   // °C, 0 until known
  std::array<char, 4> error{};      // error code shown on the display, e.g. "E90"; empty if none
  uint16_t leds = 0;

  bool operator==(const Status &o) const {
    return current_temperature == o.current_temperature && target_temperature == o.target_temperature &&
           error == o.error && leds == o.leds;
  }
  bool operator!=(const Status &o) const { return !(*this == o); }
};

/**
 * Decodes received frames into a Status.
 *
 * The display shows the water temperature when steady. After UP or DOWN is pressed it blinks the
 * target temperature: alternating blank and value, ending on either. A value run that starts
 * after a blank is therefore a blink's value phase, or - if the blink ended on a blank - the
 * returning water temperature. Such a value becomes a candidate; it is confirmed as the target
 * when the next blank appears (a real blink value never lasts that long) and rejected when the
 * run persists for STEADY displays (~630 ms, the water temperature). shown_target() returns the
 * candidate if one is pending, so a held button's changes are visible immediately; status()
 * carries the confirmed value only. Error codes are recognized when the first character is 'E'
 * and cleared once a temperature is steady.
 */
class Decoder {
 public:
  void feed(uint16_t frame);
  const Status &status() const { return status_; }
  // The value currently being shown as the target, confirmed or pending
  uint8_t shown_target() const { return candidate_.value_or(status_.target_temperature); }
  // True while the display alternates blank/value, i.e. the target is being shown.
  // Cleared once a steady temperature has been displayed for STEADY displays (~630 ms).
  bool blinking() const { return blink_count_ != 0; }

 private:
  static constexpr uint16_t CONFIRM = 3;   // identical displays before a value is used
  static constexpr uint16_t STEADY = 150;  // identical displays (~630 ms) before the display is steady

  void on_display();

  Status status_;
  Display display_{};
  int next_pos_ = 0;
  Display last_display_{};
  uint16_t count_ = 0;
  uint8_t blink_count_ = 0;
  bool prev_was_blank_ = false;   // previous display was blank
  bool run_after_blank_ = false;  // the current identical-display run started right after a blank
  std::optional<uint8_t> candidate_;  // value seen after a blank, target if confirmed
};

/**
 * Turns requests (button presses, target temperature) into the button the bus should currently
 * reply to. Never blocks; call update() regularly with the current time in ms.
 *
 * The target is changed the way a person would: press UP/DOWN once to show the blinking target,
 * then hold the button until the displayed target matches, reversing on overshoot. If the display
 * stops blinking while held (press not registering), the button is released and pressed again.
 */
class Controller {
 public:
  static constexpr uint32_t PRESS_MS = 100;
  static constexpr uint32_t GAP_MS = 100;  // released time between presses
  static constexpr uint32_t BLINK_WAIT_MS = 400;
  static constexpr uint32_t STALL_MS = 1500;  // hold re-pressed when the display stops blinking
  static constexpr uint32_t TARGET_TIMEOUT_MS = 30000;
  static constexpr uint32_t PROBE_RETRY_MS = 3000;
  static constexpr uint8_t MIN_TARGET = 20;
  static constexpr uint8_t MAX_TARGET = 40;

  explicit Controller(Decoder &decoder) : decoder_(decoder) {}

  void press(Button button) { presses_.push_back(button); }
  // Returns false if the spa is off or the temperature is out of range
  bool set_target(uint8_t temperature, uint32_t now);
  void update(uint32_t now);

  std::optional<Button> held() const { return held_; }
  // While adjusting, the displayed target is transient
  bool adjusting() const { return setpoint_ != 0; }

 private:
  enum class Phase { WAKE, WAIT_BLINK, HOLD, SETTLE };

  void start_press(Button button, uint32_t now);
  void release(uint32_t now);
  void adjust(uint32_t now);
  void finish(uint32_t now);
  Button direction_would_be_(const Status &s) const { return setpoint_ > s.target_temperature ? Button::UP : Button::DOWN; }

  Decoder &decoder_;
  std::deque<Button> presses_;
  std::optional<Button> held_;
  bool short_press_ = false;
  uint32_t pressed_at_ = 0;
  bool gap_ = false;
  uint32_t released_at_ = 0;

  uint8_t setpoint_ = 0;
  uint32_t setpoint_at_ = 0;
  Phase phase_ = Phase::WAKE;
  uint32_t woke_at_ = 0;
  uint32_t last_blink_at_ = 0;
  bool probed_ = false;
  uint32_t probed_at_ = 0;
};

}  // namespace purespa
}  // namespace esphome
