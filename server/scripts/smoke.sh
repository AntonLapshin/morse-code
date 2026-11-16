#!/usr/bin/env bash
# Phase 0 acceptance: send, long-poll, ack, and state over real HTTP.
#
#   ./scripts/smoke.sh
#   SMOKE_PORT=18081 ./scripts/smoke.sh
#
# Runs a throwaway relay in a temp dir; never touches server/config.toml.
set -euo pipefail

server_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/morse-smoke.XXXXXX")"
port="${SMOKE_PORT:-18080}"
base="http://127.0.0.1:${port}"
dad_token="$(openssl rand -hex 24)"
emma_token="$(openssl rand -hex 24)"
pid=""

cleanup() {
  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$tmp_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

pass() {
  echo "ok: $*"
}

cat >"$tmp_dir/config.toml" <<EOF
[server]
host = "127.0.0.1"
port = $port
db = "$tmp_dir/morse.db"
family_name = "MORSE"
retention = 1000

[[devices]]
id = "dad"
name = "DAD"
peer = true

[[devices]]
id = "emma"
name = "EMMA"
peer = true
EOF

echo "starting relay on port $port (db: $tmp_dir/morse.db)"
(
  cd "$server_dir"
  MORSE_CONFIG="$tmp_dir/config.toml" \
  MORSE_ENV_FILE="$tmp_dir/no-such.env" \
  MORSE_TOKEN_DAD="$dad_token" \
  MORSE_TOKEN_EMMA="$emma_token" \
  exec uv run uvicorn app:app \
    --host 127.0.0.1 --port "$port" --log-level warning
) &
pid=$!

for _ in $(seq 1 100); do
  if curl -fsS "$base/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    fail "relay exited during startup"
  fi
  sleep 0.2
done

health="$(curl -fsS "$base/health")" || fail "no /health"
[[ "$(jq -r .status <<<"$health")" == "ok" ]] || fail "/health not ok: $health"
pass "/health"

code="$(curl -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/json' \
  -d '{"client_msg_id":"dad-0-1","text":"HI"}' \
  "$base/api/v1/messages")"
[[ "$code" == "401" ]] || fail "unauthenticated send returned $code"
pass "auth required"

# Start the long-poll first; the insert must wake it.
poll_file="$tmp_dir/poll.json"
curl -fsS -G \
  -H "Authorization: Bearer $emma_token" \
  --data-urlencode since=0 \
  --data-urlencode wait=10 \
  "$base/api/v1/messages" >"$poll_file" &
poll_pid=$!

sent="$(curl -fsS -H "Authorization: Bearer $dad_token" \
  -H 'Content-Type: application/json' \
  -d '{"client_msg_id":"dad-0-1","text":"HELLO"}' \
  "$base/api/v1/messages")"
seq="$(jq -r .seq <<<"$sent")"
[[ "$(jq -r .from <<<"$sent")" == "dad" ]] || fail "wrong sender: $sent"
[[ "$(jq -r .duplicate <<<"$sent")" == "false" ]] || fail "unexpected duplicate"
pass "send seq=$seq"

wait "$poll_pid" || fail "long-poll failed"
polled="$(cat "$poll_file")"
[[ "$(jq -r '.messages | length' <<<"$polled")" == "1" ]] || fail "poll returned $polled"
[[ "$(jq -r '.messages[0].text' <<<"$polled")" == "HELLO" ]] || fail "wrong text: $polled"
[[ "$(jq -r '.messages[0].seq' <<<"$polled")" == "$seq" ]] || fail "wrong seq: $polled"
[[ "$(jq -r .unread <<<"$polled")" == "1" ]] || fail "unread not 1: $polled"
pass "long-poll woke with HELLO"

dup="$(curl -fsS -H "Authorization: Bearer $dad_token" \
  -H 'Content-Type: application/json' \
  -d '{"client_msg_id":"dad-0-1","text":"HELLO"}' \
  "$base/api/v1/messages")"
[[ "$(jq -r .duplicate <<<"$dup")" == "true" ]] || fail "retry not idempotent: $dup"
[[ "$(jq -r .seq <<<"$dup")" == "$seq" ]] || fail "retry changed seq: $dup"
pass "duplicate client_msg_id is idempotent"

acked="$(curl -fsS -H "Authorization: Bearer $emma_token" \
  -H 'Content-Type: application/json' \
  -d "{\"upto_seq\":$seq}" \
  "$base/api/v1/ack")"
[[ "$(jq -r .unread <<<"$acked")" == "0" ]] || fail "ack did not clear unread: $acked"
pass "ack clears unread"

state="$(curl -fsS -H "Authorization: Bearer $dad_token" "$base/api/v1/state")"
[[ "$(jq -r '[.devices[].id] | sort | join(",")' <<<"$state")" == "dad,emma" ]] \
  || fail "unexpected devices: $state"
[[ "$(jq -r '.devices[] | select(.id=="emma") | .online' <<<"$state")" == "true" ]] \
  || fail "emma not online: $state"
pass "state lists both devices"

empty_send="$(curl -sS -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer $dad_token" \
  -H 'Content-Type: application/json' \
  -d '{"client_msg_id":"dad-0-2","text":"   "}' \
  "$base/api/v1/messages")"
[[ "$empty_send" == "400" ]] || fail "empty text returned $empty_send"
pass "empty text rejected"

echo
echo "SMOKE OK — relay, auth, send, long-poll, dedupe, ack, state."
