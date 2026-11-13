#!/usr/bin/env sh
# Builds the firmware bridge and runs the end-to-end chain for N seconds.
set -e
cd "$(dirname "$0")"
[ -d node_modules ] || npm install --silent
TMP=$(mktemp -d)
cp ../../firmware/sketch.ino "$TMP/sketch.ino"
g++ -std=c++17 -O1 -I../../firmware/test -DSKETCH_PATH="\"$TMP/sketch.ino\"" \
    -x c++ ../../firmware/test/e2e_bridge.cpp -o "$TMP/e2e_bridge"
node run.js "$TMP/e2e_bridge" "${1:-120}" "${2:-$(pwd)/e2e-log.txt}"
