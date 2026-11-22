#!/usr/bin/env bash
# Build + run the morse-code UI simulator (host webserver with the real renderer).
#   ./scripts/sim.sh [port]
# The sim talks to the local Cloudflare Worker dev server by default
# (MORSE_SERVER_URL=http://localhost:8787) — run `make worker-dev` in
# another terminal first. Pass --relay-url=<url> to point it elsewhere;
# the sim is plain HTTP, so no https:// URL works (see worker/README.md).
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-8077}"
if [[ $# -ge 1 ]]; then shift; fi
BUILD=tools/sim/build
mkdir -p "$BUILD"
echo "Compiling sim ..."
c++ -std=c++17 -O2 -Wall -Wextra -pthread \
  -Itools/preview -Ifirmware/src \
  tools/sim/sim.cpp firmware/src/ui.cpp firmware/src/morse.cpp \
  -o "$BUILD/sim"
echo "Serving at http://localhost:$PORT/ ..."
exec "$BUILD/sim" --port "$PORT" --index tools/sim/index.html "$@"
