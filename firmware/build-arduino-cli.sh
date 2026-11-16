#!/usr/bin/env bash
# Compile (and optionally flash) morse-code with arduino-cli.
#
#   ./build-arduino-cli.sh                      # compile only
#   ./build-arduino-cli.sh --upload             # compile + flash auto-detected port
#   ./build-arduino-cli.sh --upload /dev/ttyUSB0
#   ./build-arduino-cli.sh --device dad         # regenerate secrets.h from ../.env
#
# Adafruit_ST7789 + Adafruit_GFX only; no library config flags required.
set -euo pipefail
cd "$(dirname "$0")"

FQBN=esp32:esp32:esp32
SKETCH=/tmp/morse-code-sketch/morse-code
BUILD=/tmp/morse-code-sketch/build

UPLOAD=0
PORT="${UPLOAD_PORT:-}"
DEVICE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device)
      [[ $# -ge 2 ]] || { echo "missing device id after $1" >&2; exit 2; }
      DEVICE="$2"
      shift 2
      ;;
    --device=*)
      DEVICE="${1#*=}"
      shift
      ;;
    --upload)
      UPLOAD=1
      shift
      if [[ $# -gt 0 && "$1" != --* ]]; then PORT="$1"; shift; fi
      ;;
    --upload=*)
      UPLOAD=1
      PORT="${1#*=}"
      shift
      ;;
    -p|--port)
      [[ $# -ge 2 ]] || { echo "missing port after $1" >&2; exit 2; }
      PORT="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -n "$DEVICE" ]]; then
  ../scripts/gen-secrets.py "$DEVICE" --force
fi

if [[ -z "$PORT" ]]; then
  for p in /dev/ttyUSB* /dev/ttyACM*; do
    if [[ -e "$p" ]]; then PORT="$p"; break; fi
  done
fi

export PATH="$HOME/.local/bin:$PATH"

rm -rf /tmp/morse-code-sketch
mkdir -p "$SKETCH"
cp src/main.cpp src/ui.cpp src/ui.h src/morse.cpp src/morse.h src/config.h "$SKETCH/"
if [[ -f src/secrets.h ]]; then cp src/secrets.h "$SKETCH/"; fi
printf '// morse-code sketch - real sources live in main.cpp / ui.cpp / morse.cpp\n' \
  > "$SKETCH/morse-code.ino"

echo "Compiling morse-code for $FQBN ..."
arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD" "$SKETCH"

if [[ "$UPLOAD" -eq 1 ]]; then
  if [[ -z "$PORT" ]]; then
    echo "No serial port found. Plug in the board (USB data cable) or pass a port." >&2
    exit 1
  fi
  echo "Uploading to $PORT ..."
  arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD" "$SKETCH"
  echo "Done. Watch the boot log with: arduino-cli monitor -p $PORT -c baudrate=115200"
else
  echo "Compile OK. Flash with: $0 --upload ${PORT:-/dev/ttyUSB0}"
fi
