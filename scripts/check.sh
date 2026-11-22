#!/usr/bin/env bash
# morse-code readiness check — the FIRST diagnostic step when something is
# broken. Run from the repo root, after `make setup`:
#
#   make check
#
# Checks, in order: Arduino toolchain + ESP32 core + display libraries;
# host compilers (C++ for sim/preview, ImageMagick for preview); node +
# wrangler; .env (WiFi, Worker URL, both device tokens); generated
# firmware/src/secrets.h; serial port; live Worker health; wrangler login.
#
# Every item reports PASS / FAIL / WARN / SKIP. FAIL means a hard
# requirement is missing (exit 1). WARN means "works without this, but
# check it" — no board needs to be plugged in and no deploy is needed:
# a missing serial port, a missing secrets.h, an unreachable Worker, and
# a logged-out wrangler are all warnings. Nothing secret is ever printed:
# tokens and passwords are checked by length and placeholder only.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PASS=0
WARN=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf 'FAIL  %s\n      -> %s\n' "$1" "$2"; }
warn() { WARN=$((WARN + 1)); printf 'WARN  %s\n      -> %s\n' "$1" "$2"; }
skip() { printf 'SKIP  %s\n      -> %s\n' "$1" "$2"; }

# --- .env reading (parsed, never sourced: values are never executed) --------
# Prints the value of KEY in .env with surrounding quotes stripped.
env_val() {
  local key="$1" file="$ROOT/.env" v
  [ -f "$file" ] || return 0
  v="$(grep -E "^[[:space:]]*(export[[:space:]]+)?${key}=" "$file" | tail -n 1 | sed -E 's/^[^=]*=//')"
  v="$(printf '%s' "$v" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/^"\(.*\)"$/\1/' -e "s/^'\(.*\)'$/\1/")"
  printf '%s' "$v"
}

is_placeholder() { # is_placeholder VALUE PREFIX... — starts with REPLACE/CHANGEME/your-wifi?
  local v upper
  v="$1"
  shift
  upper="$(printf '%s' "$v" | tr '[:lower:]' '[:upper:]')"
  for p in "$@"; do
    case "$upper" in
      "$p"*) return 0 ;;
    esac
  done
  return 1
}

echo "morse-code check (repo: $ROOT)"
echo

# --- 1. Arduino toolchain ----------------------------------------------------
if command -v arduino-cli >/dev/null 2>&1; then
  ok "arduino-cli present ($(arduino-cli version 2>/dev/null | head -n 1))"
else
  fail "arduino-cli present" "install it: https://arduino.github.io/arduino-cli/latest/installation/"
fi

if command -v arduino-cli >/dev/null 2>&1; then
  if arduino-cli core list 2>/dev/null | grep -q 'esp32:esp32'; then
    ok "esp32 core installed"
  else
    fail "esp32 core installed" "arduino-cli core install esp32:esp32"
  fi
  if arduino-cli lib list 2>/dev/null | grep -qi 'Adafruit ST7735 and ST7789'; then
    ok "Adafruit ST7735/ST7789 library installed"
  else
    fail "Adafruit ST7735/ST7789 library installed" "arduino-cli lib install 'Adafruit ST7735 and ST7789 Library'"
  fi
  if arduino-cli lib list 2>/dev/null | grep -qi 'Adafruit GFX'; then
    ok "Adafruit GFX library installed"
  else
    fail "Adafruit GFX library installed" "arduino-cli lib install 'Adafruit GFX Library'"
  fi
fi
skip "ArduinoJson" "not required — firmware and sim use hand-rolled JSON (net.cpp, sim.cpp)"

# --- 2. Host compilers --------------------------------------------------------
if command -v c++ >/dev/null 2>&1; then
  ok "C++ compiler present ($(c++ --version 2>/dev/null | head -n 1))"
else
  fail "C++ compiler present" "install g++ or clang (needed for: make sim, make sim-dump, make preview)"
fi

if command -v magick >/dev/null 2>&1; then
  ok "ImageMagick present (magick)"
elif command -v convert >/dev/null 2>&1; then
  ok "ImageMagick present (convert)"
else
  fail "ImageMagick present" "install imagemagick (needed for: make preview)"
fi

# --- 3. Node + wrangler -------------------------------------------------------
if command -v node >/dev/null 2>&1; then
  ok "node present ($(node --version 2>/dev/null))"
else
  fail "node present" "install node 20+ (needed for: make worker-*)"
fi
if command -v npm >/dev/null 2>&1; then
  ok "npm present ($(npm --version 2>/dev/null))"
else
  fail "npm present" "install npm (comes with node)"
fi

WRANGLER=""
if [ -x "$ROOT/worker/node_modules/.bin/wrangler" ]; then
  WRANGLER="$ROOT/worker/node_modules/.bin/wrangler"
elif command -v wrangler >/dev/null 2>&1; then
  WRANGLER="$(command -v wrangler)"
fi
if [ -n "$WRANGLER" ]; then
  ok "wrangler present ($("$WRANGLER" --version 2>/dev/null | head -n 1))"
else
  fail "wrangler present" "run: make setup   (installs it into worker/node_modules)"
fi

# --- 4. .env ------------------------------------------------------------------
if [ ! -f "$ROOT/.env" ]; then
  fail ".env exists" "run: make setup   (copies .env.example) or: ./scripts/init-env.sh   (fresh tokens)"
  skip ".env values (WIFI_*, MORSE_SERVER_URL, MORSE_TOKEN_*)" ".env is missing — see above"
else
  ok ".env exists"
  SSID="$(env_val WIFI_SSID)"
  PASSWD="$(env_val WIFI_PASS)"
  if [ -n "$SSID" ] && ! is_placeholder "$SSID" "YOUR-WIFI"; then
    ok "WIFI_SSID set"
  else
    fail "WIFI_SSID set (non-placeholder)" "edit WIFI_SSID in .env (2.4 GHz SSID; the ESP32 cannot join 5 GHz-only)"
  fi
  if [ -n "$PASSWD" ] && ! is_placeholder "$PASSWD" "YOUR-WIFI"; then
    ok "WIFI_PASS set"
  else
    fail "WIFI_PASS set (non-placeholder)" "edit WIFI_PASS in .env"
  fi
  SERVER_URL="$(env_val MORSE_SERVER_URL)"
  if [ -n "$SERVER_URL" ]; then
    case "$SERVER_URL" in
      http://*|https://*) ok "MORSE_SERVER_URL set ($SERVER_URL)" ;;
      *) fail "MORSE_SERVER_URL set (a full URL)" "set MORSE_SERVER_URL in .env, e.g. https://<your-domain> (local dev: http://localhost:8787)" ;;
    esac
  else
    fail "MORSE_SERVER_URL set" "set MORSE_SERVER_URL in .env, e.g. https://<your-domain> (local dev: http://localhost:8787)"
  fi
  TOKEN_DAD="$(env_val MORSE_TOKEN_DAD)"
  TOKEN_EMMA="$(env_val MORSE_TOKEN_EMMA)"
  for spec in "MORSE_TOKEN_DAD:$TOKEN_DAD" "MORSE_TOKEN_EMMA:$TOKEN_EMMA"; do
    key="${spec%%:*}"
    val="${spec#*:}"
    if [ -z "$val" ]; then
      fail "$key present" "generate one: openssl rand -hex 24   (or run ./scripts/init-env.sh for a fresh .env)"
    elif is_placeholder "$val" "REPLACE" "CHANGEME"; then
      fail "$key present (non-placeholder)" "replace the placeholder in .env — generate one: openssl rand -hex 24"
    elif [ "${#val}" -lt 16 ]; then
      fail "$key at least 16 characters (is ${#val})" "regenerate: openssl rand -hex 24"
    else
      ok "$key present (${#val} chars)"
    fi
  done
  if [ -n "$TOKEN_DAD" ] && [ -n "$TOKEN_EMMA" ] && [ "$TOKEN_DAD" = "$TOKEN_EMMA" ]; then
    fail "MORSE_TOKEN_DAD and MORSE_TOKEN_EMMA differ" "the two tokens must differ — regenerate one: openssl rand -hex 24"
  elif [ -n "$TOKEN_DAD" ] && [ -n "$TOKEN_EMMA" ]; then
    ok "MORSE_TOKEN_DAD and MORSE_TOKEN_EMMA differ"
  fi
fi

# --- 5. Generated firmware secrets (warn only — `make secrets` makes it) -----
if [ -f "$ROOT/firmware/src/secrets.h" ]; then
  DEV="$(grep -E '^#define DEVICE_ID' "$ROOT/firmware/src/secrets.h" 2>/dev/null | sed -E 's/.*"(.*)".*/\1/')"
  ok "firmware/src/secrets.h present${DEV:+ (DEVICE_ID=$DEV)}"
else
  warn "firmware/src/secrets.h present" "run: make secrets DEVICE=dad   (or emma) — needed before make build / make upload"
fi

# --- 6. Serial port (warn only — the check must run without a board) ----------
PORT="$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n 1 || true)"
if [ -n "$PORT" ]; then
  ok "serial port present ($PORT)"
else
  warn "serial port present" "no /dev/ttyUSB* or /dev/ttyACM* — plug a board in with a data USB cable before make upload (not needed for this check)"
fi

# --- 7. Worker health (warn only — the Worker may simply not be running) -----
if command -v curl >/dev/null 2>&1; then
  SERVER_URL="${SERVER_URL:-$(env_val MORSE_SERVER_URL)}"
  health() { # health URL -> prints "ok <version>" or nothing
    local body
    body="$(curl -sS -m 10 "$1/api/v1/health" 2>/dev/null || true)"
    [ -n "$body" ] || return 1
    printf '%s' "$body" | grep -q '"status"[[:space:]]*:[[:space:]]*"ok"' || return 1
    local ver
    ver="$(printf '%s' "$body" | grep -o '"version"[[:space:]]*:[[:space:]]*"[^"]*"' | head -n 1 | sed -E 's/.*"([^"]*)"[[:space:]]*$/\1/')"
    printf 'ok %s' "${ver:-unknown version}"
  }
  if [ -n "$SERVER_URL" ]; then
    if out="$(health "$SERVER_URL")"; then
      ok "Worker health at $SERVER_URL ($out)"
    elif [ "$SERVER_URL" != "http://localhost:8787" ] && out="$(health "http://localhost:8787")"; then
      warn "Worker health at $SERVER_URL" "configured URL is unreachable, but a local dev server answers ($out) — run: make worker-dev   (or deploy: make worker-deploy)"
    else
      warn "Worker health at ${SERVER_URL:-<unset>}" "no Worker answers — start one: make worker-dev   (local) or make worker-deploy   (production)"
    fi
  else
    skip "Worker health" "MORSE_SERVER_URL is not set in .env"
  fi
else
  skip "Worker health" "curl is not installed"
fi

# --- 8. Wrangler login (warn only — login is needed to deploy, not to check) --
# NOTE: `wrangler whoami` exits 0 even when logged OUT (it just prints "You
# are not authenticated"), so the output — not the exit code — decides.
if [ -n "$WRANGLER" ]; then
  if (cd "$ROOT/worker" && "$WRANGLER" whoami 2>&1 | grep -qi 'not authenticated'); then
    warn "wrangler logged in" "not logged in — run: make worker-login   (needed only for make worker-deploy / make worker-secrets)"
  else
    ok "wrangler logged in"
  fi
fi

echo
printf 'check: %s passed, %s warnings, %s failed\n' "$PASS" "$WARN" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
  echo "fix the FAIL items above, then re-run: make check"
  exit 1
fi
exit 0
