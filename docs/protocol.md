# SB-HS10-1 panel bus

What read-only captures on one tub (28431EH, base 13437B, panel SB-HS10-1, 28530 salt cell) showed, using the listen-only firmware in `capture/`. Frame values are as decoded: bus DATA inverted, MSB first.

## How the bus works

The main board drives everything. It sends 16-bit frames to the panel on three lines:

- **CLOCK:** one rising edge per bit; DATA is read on the rising edge.
- **HOLD:** low while a frame is sent, then a short high strobe that latches it.
- **DATA:** the frame bits. The panel's only output is a button reply: it pulls DATA low in the pause after the main board polls a button that is being held.

Each cycle the main board sends the four display digits, the LED frame and a run of nine button polls, with cue frames (`0x0100`) between groups.

## Timing

The bus is about 3 times faster than the SB-H20 documented by other projects (100 kHz there, about 140 kHz on nalditopr's 28431E).

- **Bit period:** about 2.9 µs (shortest HOLD-low per frame 47 µs for 16 bits).
- **CLOCK** is low between frames; each high pulse is about 0.5 µs.
- **HOLD strobe:** goes high about 1.7 µs after the 16th CLOCK rise and stays high 437–500 ns. Occasional strobes on any frame type stretch to about 4.9 µs; this is main-board jitter.
- **Frame spacing:** 48 µs within a group; 64–74 µs between button polls, leaving a pause of about 15–25 µs after each poll.
- **Rates:** about 1,580 frames/s with the spa on, 2,780 frames/s in standby. Each button is polled every 6–12 ms.
- **Start-up:** the bus starts about 7 s after power is applied, and the spa comes up switched off.

## Button reply

The panel answers a poll for a held button by pulling DATA low:

- **Start:** about 4 µs after the poll's 16th CLOCK rise (about 2.5 µs after HOLD rises).
- **Length:** about 10 µs.
- **Ends** about 15 µs before the next frame's first CLOCK edge.

All ±0.5 µs. Polls for buttons not being pressed show no dip. One salt reply, one character per 0.5 µs from the 16th CLOCK rise (`#` = high):

```text
         0    5    10   15   20   25   30 µs
DATA  ########....................################################
CLOCK #...........................................................
HOLD  ...##.......................................................
```

A press is answered for 7–13 polls in a row (about 70–110 ms). The panel stops about one poll after the main board starts beeping.

RealByron's `sbh20.pio` pulls DATA low about 4 µs after HOLD rises, for about 2 µs. `control/bus.pio` copies the panel instead: 2.4 µs after HOLD rises, for 10 µs, stopping when the beep starts.

## Button polls

Nine polls per cycle, in this order:

| Poll | Button |
| --- | --- |
| `0x0108` | Bubbles |
| `0x0102` | Filter |
| `0x2100` | °C/°F |
| `0x8100` | Heater |
| `0x0500` | Power |
| `0x1100` | Up |
| `0x0180` | Down |
| `0x0110` | Salt (sanitizer) |
| `0x0140` | none (never answered) |

The seven known buttons use the SB-H20 bit layout. The SJB-HS sanitizer code `0x0101` is never polled here.

`0x0140` is also what a blank first digit looks like (digit select `0x0040` plus cue bit), so tell that poll apart by its position after `0x0110`, not by its value.

## Beep

While the panel beeps, bit `0x0100` is clear in every frame, so a poll like `0x0108` arrives as `0x0008`. Decoders must mask this bit, and the beep is the main board's acknowledgement of a press.

## LED frame

Seen values:

| Frame | State |
| --- | --- |
| `0x4008` | spa off |
| `0x4009` | on |
| `0x4409` | on, bubbles |
| `0x5011` | on, salt running (with filter) |
| `0x5089` | on, heating (with filter) |
| `0x5009` | on, filter only |
| `0x5011` / `0x5211`, alternating | on, salt running, heater suspended |
| `0x5691` | power-up lamp test, about 0.9 s |

Known bits: `0x0001` power, `0x0080` heating, `0x0400` bubbles, `0x1000` filter, `0x0100` no beep.

- **`0x0010`:** on while salt runs; probably the salt LED. The SJB-HS value `0x2000` never appears.
- **`0x0200`:** blinks (about 0.5 s on, 0.5 s off) while salt runs with the heater on: the heater is suspended, as the manual describes, and resumes heating by itself when salt stops. Steady, it is probably heater on with the water at temperature (the SB-H20 value); not yet seen.
- **`0x0008`:** set in every frame except while salt runs and in the lamp test; not a light, meaning unknown.
- **Heater:** turning it on also turns on the filter. Turning it off leaves the filter running (per the manual, for 1 minute).

## Display

About 84 complete displays per second. Temperature shows in °F as three digits plus `F` (`094F`); digit 4 also carries the decimal-point bit `0x8000`. The water temperature reading often flips by 1 °F for a second at a time. When the display changes, one or two half-updated displays (such as `004F` between `094F` and `003H`) appear, so a decoder should wait for a display to repeat.

The salt menu:

- The first salt press shows the current setting flashing; each further press steps OFF → 3H → 5H → 8H → OFF.
- Shown as `0FF ` and `003H`, `005H`, `008H`.
- Flashing is about 190 ms on and 290 ms off. The blank phase is digit frames `0x0140`, `0x0120`, `0x0900` and `0x0104` (digit select plus cue bit, no segments); these are not buttons.
- About 10 s after the last press the flashing stops on the value (seen lasting about 380 ms), and the setting takes effect: the salt light comes on at that moment.
- With salt running, the display alternates the water temperature (about 2.1 s) and the remaining hours (`003H`, about 1.9 s).

The temperature menu (Up/Down):

- Flashes the target with the same timing as the salt menu, but **the blank phase keeps the unit: `   F`**, not an all-blank display.
- **Every press changes the target by 1 °F, including the first.** The first press shows the old target for a few tens of ms (`103F` for 36 ms), then flashes the new one (`104F`). Each press beeps.
- Flashing lasts about 5 s after the last press; then the water temperature returns straight after a blank phase, with no steady display of the new target.

The filter menu works like the salt menu: the first press shows the current setting (`0FF `), further presses step 2H, 4H, 6H, OFF (`002H`, `004H`, `006H`, `0FF `), and the setting takes effect about 10 s after the last press.

At power-up the panel shows `8888` with every light on for about 0.9 s.
