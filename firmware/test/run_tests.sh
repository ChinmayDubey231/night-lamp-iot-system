#!/usr/bin/env sh
# Host-side firmware tests. The sketch is copied to a temp folder first so
# it always picks up test/secrets.h (fake values), never a developer's real
# firmware/secrets.h. Both watchdog API generations are compiled:
#   Arduino-ESP32 2.x (ESP-IDF 4) and 3.x (ESP-IDF 5).
set -e
cd "$(dirname "$0")"
TMP=$(mktemp -d)
cp ../sketch.ino "$TMP/sketch.ino"
FLAGS="-std=c++17 -Wall -Wextra -I. -DSKETCH_PATH=\"$TMP/sketch.ino\""

echo "== Arduino-ESP32 2.x watchdog API =="
g++ $FLAGS -x c++ test_sketch.cpp -o "$TMP/test_v2"
"$TMP/test_v2"

echo "== Arduino-ESP32 3.x watchdog API =="
g++ $FLAGS -DESP_ARDUINO_VERSION_MAJOR=3 -x c++ test_sketch.cpp -o "$TMP/test_v3"
"$TMP/test_v3" | tail -1

rm -rf "$TMP"
