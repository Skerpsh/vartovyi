#!/bin/bash
# Builds and installs the firmware over USB; saved networks and settings stay. --erase also wipes them and everything else on the flash.
# Without src/secrets.h the stick opens phone setup on first start to get a Wi-Fi network.
set -eu
cd "$(dirname "$0")"

g++ -std=c++17 -I src test/test_alert.cpp -o /tmp/vartovyi-test && /tmp/vartovyi-test
pio run                # build first: a failed build must not leave the stick empty
[ "${1:-}" = --erase ] && pio run -t erase
pio run -t upload
echo 'Done. Settings > Test alarm checks the sound.'
