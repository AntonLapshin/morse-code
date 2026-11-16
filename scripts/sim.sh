#!/usr/bin/env bash
# Build + run the morse-code UI simulator (host webserver with the real renderer).
#   ./scripts/sim.sh [port]
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-8077}"
BUILD=tools/sim/build
mkdir -p "$BUILD"
echo "Compiling sim ..."
c++ -std=c++17 -O2 -Wall -Wextra \
  -Itools/preview -Ifirmware/src \
  tools/sim/sim.cpp firmware/src/ui.cpp firmware/src/morse.cpp \
  -o "$BUILD/sim"
echo "Serving at http://localhost:$PORT/ ..."
exec "$BUILD/sim" --port "$PORT" --index tools/sim/index.html
