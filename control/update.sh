#!/bin/bash
# Tests, builds and sends the firmware to the Pico over WiFi, then checks that it restarted.
# Usage: control/update.sh
set -euo pipefail
cd "$(dirname "$0")"
source wifi.env

make -C tests
if ! uvx platformio run -e ota -t upload; then
  echo "Update failed. The Pico keeps running its old firmware; it is safe to try again."
  exit 1
fi

echo "Waiting for the Pico to restart..."
for _ in $(seq 30); do
  sleep 2
  status=$(curl -sf --max-time 3 -u "spa:$WEB_PASS" http://spa.local/status) || continue
  uptime=$(grep -o '"uptime":[0-9]*' <<< "$status" | cut -d: -f2)
  if [ "$uptime" -lt 60 ]; then
    echo "Restarted. Latest log lines:"
    curl -sf --max-time 3 -u "spa:$WEB_PASS" http://spa.local/log | head -4
    exit 0
  fi
done
echo "The Pico didn't come back within a minute. Check http://spa.local/ by hand."
exit 1
