# Build

The adapter is a small waterproof box on a short cable. The cable's plug goes into the pump's panel socket, and the original panel plugs into a socket on the box. Inside, the five wires pass straight through, and each one also branches to the level shifter and the Pico.

```text
Pump's socket ← printed plug ── cable ── box ── printed socket ← panel's plug
                                          │
                                 level shifter + Pico W
```

## Parts

- Raspberry Pi Pico W, without headers
- Adafruit 757 level shifter (4-channel BSS138; three channels used)
- 10 µF 25 V electrolytic capacitor
- Plug and socket printed from [Elektroarzt's Printables model](https://www.printables.com/model/509289-intex-purespa-whirlpool-controller-electromechanic), which also has assembly instructions
- TE 350669-1 male and 926882-3 female contacts, 5 of each plus spares. Shorter pins don't fit the printed parts.
- About 50 cm of five-core 18 AWG cable (Lapp Ölflex 190 601805) and an M20 cable gland
- An M686-style three-way T junction box
- Heat shrink, thin hookup wire, an open-barrel crimper (Knipex 97 22 240) and a multimeter

## Pin numbers

The cable is straight through: plug pin N joins socket pin N. Hold each part with its row of three at the top:

```text
          Mating face      Wiring side
Plug      1   2   3        3   2   1
            5   4            4   5

Socket    3   2   1        1   2   3
            4   5            5   4
```

This matches [jnsbyr's schematic](https://github.com/jnsbyr/esp8266-intexsbh20/blob/master/assets/Schematic.png). Contacts are hard to remove once they click in, so check the position before pushing.

## Wiring

| Spa pin | Level shifter | Pico W |
| --- | --- | --- |
| 1: +5 V | HV | VSYS (pin 39) |
| 2: GND | GND | GND (pin 3) |
| 3: DATA | B1 ↔ A1 | GP3 (pin 5) |
| 4: CLOCK | B2 ↔ A2 | GP4 (pin 6) |
| 5: HOLD | B3 ↔ A3 | GP5 (pin 7) |
| | LV | 3V3 OUT (pin 36) |

- Solder the capacitor across pins 39 (+) and 38 (−). Leave VBUS (pin 40) empty.
- The shifter fits flat under the USB end of the Pico, on foam tape, with short jumpers to the pads above. Every pad used is in the first seven rows.
- Each spa wire is joined to its socket pigtail and its shifter lead in one soldered, heat-shrunk joint (or a WAGO 221-413 lever connector). Stagger the joints so they can't touch.

## Before attaching the Pico

1. With the spa unplugged, check with the meter's continuity beep that plug pin N reaches socket pin N and its shifter post, and that no two pins are shorted.
2. Leave the Pico off and check polarity on the live spa with a battery-powered meter: black on the shifter's GND jumper, red on its HV jumper. Expect a steady +5 V (no more than 5.5 V). A negative reading means pins 1 and 2 are swapped.
3. Unplug the spa and solder the jumpers to the Pico.

Flash the Pico over USB before it goes on the spa (see the README), and never connect USB while it's on the spa.
