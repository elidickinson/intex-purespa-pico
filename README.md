# PicoW-Intex-PureSpa

WiFi control for an Intex PureSpa with the **SB-HS10-1** control panel, on a Raspberry Pi Pico W. It sits inline between the pump base and the original panel, which keeps working.

Tested on one 28431EH with the 28530 salt cell. Other models may differ: the listen-only firmware in `capture/` logs the bus over WiFi so you can check yours (see the top of `capture/capture.cpp`).

- Web page and HTTP API: water and target temperature, lights, salt hours left, error codes.
- Settings: power, heater and bubbles on/off, target temperature, salt for 3, 5 or 8 hours. Each checks the panel first and only presses when something needs to change, so repeating one is harmless.
- Weekly schedule stored on the Pico, e.g. `fri 15:00 power=on heater=on temp=102`.
- Updates over WiFi.

## Hardware

A Pico W, a 4-channel BSS138 level shifter (Adafruit 757), a 10 µF capacitor, and an inline plug and socket for the panel cable.

| Spa wire | Level shifter | Pico W |
| --- | --- | --- |
| 1: +5 V | HV | VSYS (pin 39) |
| 2: GND | GND | GND (pin 3) |
| 3: DATA | B1 ↔ A1 | GP3 (pin 5) |
| 4: CLOCK | B2 ↔ A2 | GP4 (pin 6) |
| 5: HOLD | B3 ↔ A3 | GP5 (pin 7) |
| | LV | 3V3 OUT (pin 36) |

Wire numbers follow [jnsbyr's schematic](https://github.com/jnsbyr/esp8266-intexsbh20/blob/master/assets/Schematic.png). Put the capacitor across pins 39 and 38. Before connecting the Pico, check that wire 1 reads a steady 5 V against wire 2.

**Never connect USB while the Pico is on the spa:** it joins the computer's ground and 5 V to the spa's.

## Install

Needs [uv](https://docs.astral.sh/uv/) (PlatformIO runs through `uvx`) and a C++ compiler for the tests.

1. Create `control/wifi.env` (gitignored). The Pico W only joins 2.4 GHz networks. `WEB_PASS` protects the page and updates. `TIMEZONE` is a POSIX TZ string; this one is New York.
   ```sh
   export WIFI_SSID=...
   export WIFI_PASS=...
   export WEB_PASS=...
   export TIMEZONE=EST5EDT,M3.2.0,M11.1.0
   ```
2. First flash, with the Pico off the spa: hold BOOTSEL while plugging in USB, then run `control/flash-usb.sh`.
3. After that, update over WiFi with `control/update.sh`.

## Use

Open `http://spa.local/` and enter `WEB_PASS`; the browser remembers it. Or use the same settings from any computer with basic auth (user `spa`), for example from cron:

```sh
curl -u spa:PASSWORD http://spa.local/status
curl -u spa:PASSWORD -X POST 'http://spa.local/set?heater=on&temp=102'
```

`power`, `heater`, `bubbles` = `on`/`off`; `temp` = degrees as the panel shows them; `salt` = `off`, `3`, `5` or `8`. The API is listed at the top of `control/control.cpp`, the schedule format in `control/schedule.h`.

## How it works

The pump base polls the panel over a three-wire bus at about 330 kHz, and the panel answers a poll by pulling DATA low. Two PIO programs handle the timing: one reads every frame, the other answers polls the way the panel does and stops by itself when the spa beeps. Codes and timings are in [docs/protocol.md](docs/protocol.md).

## Compared with RealByron's original

This is a fork of [RealByron/PicoW-Intex-PureSpa](https://github.com/RealByron/PicoW-Intex-PureSpa), which showed that a Pico W with PIO can handle the bus, as an ESPHome component for the SB-H20. This version is a rewrite:

- Plain Arduino firmware with its own web page and schedule, instead of ESPHome and Home Assistant.
- Supports the SB-HS10-1, including salt. Its bus runs about 3 times faster than the SB-H20's.
- Button replies copy the real panel's timing and stop at the spa's beep, so a press can't repeat.
- Never presses anything at startup. On/off state comes from the panel's lights, not from what was requested.
- Tests against a real capture and in a PIO emulator.

## Other projects

For PureSpa models without built-in WiFi:

- [jnsbyr/esp8266-intexsbh20](https://github.com/jnsbyr/esp8266-intexsbh20): ESP8266 and MQTT, for the SB-H20, SB-B20, SSP-H-20-1 and SJB-HS (including salt).
- [YorffoeG/diyscip](https://github.com/YorffoeG/diyscip): ESP8266 on a custom PCB with MQTT, for SSP and SJB models.

## Disclaimer

Not affiliated with Intex, and tested on one tub. It may void your warranty; use at your own risk.

## License

MIT, see [LICENSE](LICENSE).
