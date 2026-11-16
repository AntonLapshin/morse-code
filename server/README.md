# Morse relay

LAN HTTP relay for the two ESP32 Morse boards. One process, one SQLite file,
no cloud. The design and protocol live in
[`../docs/messaging-plan.md`](../docs/messaging-plan.md).

Status: **Phase 0** — server scaffold, auth, storage, send, long-poll, ack,
state, tests, and a curl smoke test.

## Setup

One rule: **`.env` (repo root) holds every secret; `server/config.toml` is
committed and holds none.** Copy the template once, then fill in WiFi:

```bash
./scripts/init-env.sh   # creates .env with a fresh token per device, chmod 600
$EDITOR .env            # set WIFI_SSID + WIFI_PASS, check RELAY_HOST
cd server
uv sync
```

`.env` is gitignored. The database is created at `server/data/morse.db` on
first start (also gitignored). Re-running `init-env.sh` refuses to overwrite
an existing `.env`.

Device IDs and names are `dad`/`DAD` and `emma`/`EMMA` (see `config.toml`).
The firmware sends only its token; the server derives `from` from it.

## Run

Bind to the LAN so the ESP32s can reach it. Do **not** port-forward.

```bash
cd server
uv run uvicorn app:app --host 0.0.0.0 --port 8080
```

Use exactly **one** worker: long-poll waiters are in-process state. Check it:

The laptop keeps the reserved address `192.178.4.44` (DHCP reservation).
Check the relay from the LAN with:

```bash
curl -s http://192.178.4.44:8080/health | jq
```

On the laptop itself, clone the repo, create `.env` (see Setup above), and
run `uvicorn` there. The firmware gets its copy of the secrets via
`./scripts/gen-secrets.py dad` / `... emma` (see
[`../docs/messaging-plan.md`](../docs/messaging-plan.md) §10) — one checkout
configures both boards.

## Smoke test

Runs a throwaway relay with generated tokens on port 18080; does not touch
your `.env` or the real database (tokens are passed as environment variables).

```bash
cd server
./scripts/smoke.sh
```

Covers `/health`, auth rejection, send, long-poll wake-up, idempotent retry,
ack, state/presence, and empty-text rejection.

## Tests

```bash
cd server
uv run pytest -q
```

## API quick reference

All `/api/v1/*` calls need `Authorization: Bearer <device token>`.

```bash
TOKEN=...   # MORSE_TOKEN_DAD or MORSE_TOKEN_EMMA from .env

# Send
curl -s -X POST http://127.0.0.1:8080/api/v1/messages \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"client_msg_id":"dad-1-1","text":"HELLO"}'

# Long-poll (since = last seq processed, wait <= 25 s)
curl -s -G http://127.0.0.1:8080/api/v1/messages \
  -H "Authorization: Bearer $TOKEN" \
  --data-urlencode since=0 --data-urlencode wait=20

# Ack displayed messages
curl -s -X POST http://127.0.0.1:8080/api/v1/ack \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"upto_seq":1}'

# Device presence / unread / latest seq
curl -s http://127.0.0.1:8080/api/v1/state \
  -H "Authorization: Bearer $TOKEN"
```

## Files

```
app.py                 routes, long-poll notifier, lifespan
config.py              TOML + .env loading, env overrides
config.toml            committed: server settings + device list, no secrets
db.py                  schema, queries, retention
auth.py                bearer token -> device
models.py              request validation
tests/test_api.py      API tests (fake tokens, not real secrets)
tests/test_config.py   .env loading + token validation
scripts/smoke.sh       end-to-end curl acceptance (throwaway tokens)
```

## Not done yet

- systemd user unit and lid-switch handling (Phase 5)
- web chat UI at `/` (Phase 4)
- ntfy / phone-watch bridge (Phase 4)
- TLS: the MVP is LAN-only, so tokens and text are visible on the local WiFi.
  Acceptable on a trusted home network; see the plan's security section.

## Secrets hygiene

- Never commit `.env` or `firmware/src/secrets.h` (both gitignored).
- `.env.example` and `server/config.toml` contain no secrets and are safe to
  commit. If a real token ever lands in a tracked file, rotate it:
  replace the value in `.env`, regenerate `secrets.h`, restart the relay.
