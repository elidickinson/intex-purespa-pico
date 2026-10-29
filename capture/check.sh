#!/bin/bash
# Syntax-check capture/capture.cpp with the real cross-compiler (offline).
# Run from the repo root: bash capture/check.sh
# Requires the PlatformIO packages under /tmp/picow (reinstall with a short
# 'esphome compile' if missing, or point FW/TC at another arduino-pico install):
# Regenerate the PIO header first if capture.pio changed:
#   /tmp/picow/pio/packages/tool-pioasm-rp2040-earlephilhower/pioasm  # adjust if your PlatformIO packages live elsewhere capture/capture.pio capture/capture.pio.h
TC=/tmp/picow/pio/packages/toolchain-rp2040-earlephilhower/bin
FW=/tmp/picow/pio/packages/framework-arduinopico
cd "$(dirname "$0")"
LIBINC="-I$FW/libraries/SPI/src -I$FW/libraries/WiFi/src -I$FW/libraries/lwIP_CYW43/src -I$FW/libraries/lwIP_Ethernet/src -I$FW/libraries/lwIP_ESPHost/src -I$FW/libraries/cyw43_driver/src -I$FW/libraries/Updater/src -I$FW/libraries/Adafruit_TinyUSB_Arduino/src -I$FW/libraries/LEAmDNS/src -I$FW/libraries/MD5Builder/src"
INC="-I. -I$FW/variants/rpipicow -I$FW/cores/rp2040 -I$FW/cores/rp2040/api/deprecated -I$FW/cores/rp2040/api/deprecated-avr-comp -I$FW/include $LIBINC"
DEF="-DPICO_RP2040=1 -DUSE_ARDUINO -DUSE_RP2040 -DUSE_RP2 -DARDUINO_RASPBERRY_PI_PICO_W -DUSE_UTF8_LONG_NAMES=1 -DDISABLE_FS_H_WARNING=1 -DPICO_CYW43_ARCH_THREADSAFE_BACKGROUND=1 -DCYW43_LWIP=1 -DCYW43_PIO_CLOCK_DIV_DYNAMIC=1 -DLWIP_IPV4=1 -DLWIP_IPV6=0 -DLWIP_IGMP=1 -DLWIP_CHECKSUM_CTRL_PER_NETIF=1 -DCYW43_USE_OTP_MAC=1 -DUSBD_MAX_POWER_MA=500"
CFG="-iprefix$FW/ @$FW/lib/rp2040/platform_inc.txt @$FW/lib/core_inc.txt -std=gnu++17 -fsyntax-only"
$TC/arm-none-eabi-c++ $DEF $INC $CFG capture.cpp 2>&1 | grep -v '^In file\|^ *from' || true
echo "syntax-check done"
