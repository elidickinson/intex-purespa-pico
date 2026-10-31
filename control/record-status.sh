#!/bin/sh
# Adds the spa's status, with the date and time, as one line of JSON to a file. Meant for cron
# once a minute on a computer that's always on; the Pico keeps no history itself:
#   * * * * * SPA_PASS=... /path/to/record-status.sh spa.local /path/to/spa-history.jsonl
# Each line is about 350 bytes, so about 0.5 MB a day.
set -eu
[ $# -eq 2 ] || { echo "Usage: SPA_PASS=... $0 <spa host> <output file>" >&2; exit 2; }
status=$(curl -sSf --max-time 20 -u "spa:$SPA_PASS" "http://$1/status")
printf '{"at":"%s",%s\n' "$(date +%FT%T%z)" "${status#\{}" >> "$2"
