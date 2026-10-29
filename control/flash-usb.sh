#!/bin/bash
# Tests, builds and flashes the firmware over USB, for the first time or when WiFi updates don't
# work. Only with the Pico off the spa: USB would join the computer's ground and 5 V to the spa's.
# Hold BOOTSEL while plugging in the USB cable, then run: control/flash-usb.sh
set -euo pipefail
cd "$(dirname "$0")"
source wifi.env
PICOTOOL=~/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool

read -r -p "Is the Pico disconnected from the spa? [y/N] " answer
if [ "$answer" != y ]; then
  echo "Stopped. Unplug the spa and take the Pico off it first."
  exit 1
fi
make -C tests
uvx platformio run
if ! "$PICOTOOL" info -d > /dev/null 2>&1; then
  echo "No Pico in BOOTSEL mode. Unplug the USB cable, hold BOOTSEL while plugging it back in, then run this again."
  exit 1
fi
"$PICOTOOL" load -x .pio/build/pico_w/firmware.uf2
echo "Flashed. The Pico restarts and joins WiFi in about 10 s: http://spa.local/"
