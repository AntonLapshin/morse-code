# WiFi messaging plan — two ESP32s + Omarchy laptop relay

**Status:** proposed, not implemented  
**Date:** 2026-11-16  
**Scope:** LAN-only family messaging between two Morse-code ESP32 devices, with a
long-running Omarchy laptop as the relay. Phone/watch delivery is a later bridge
on top of the same protocol.

---

## 1. Goal

Two ESP32 Morse-code devices should be able to send short text messages to each
other over the home WiFi:

- Type a message with the existing single BOOT button (tap = dot, hold = dash).
- Send it explicitly from the device.
- Receive messages on the other device, with a readable notification screen and
  an inbox for older messages.
- Keep working through WiFi drops, server restarts, and power cycles without
  losing or duplicating messages.
- Stay usable as a Morse trainer when the network is down.

The laptop is the source of truth: it stores messages, tracks delivery order and
read state, and gives us one place to add a phone/watch bridge later.

> **MVP assumption:** both boards are on the same home LAN as the laptop. If one
> board is at your daughter's place, this LAN version is still the right
> prototype for the device UI and protocol, but remote delivery needs Phase 4
> (the ntfy bridge) or a VPN such as Tailscale.

---

## 2. What already exists

| Piece | Current state |
|---|---|
| `firmware/src/main.cpp` | Single-button Morse input state machine, text buffer, boot log |
| `firmware/src/ui.cpp` | 170×320 canvas renderer, tree, pattern row, result line, countdown |
| `firmware/src/morse.cpp` | A–Z tree table and child lookup |
| `firmware/src/config.h` | Pins and timing constants |
| `tools/preview/` | Host-side renderer using the real `ui.cpp`, outputs PPM scenes |
| `Makefile` | `build`, `upload`, `monitor`, `preview`, `check` |
| Network | None. No WiFi, no JSON, no server. |

The display layout is fixed and tight:

```
y=4    free (status line can go here)
y=18   "MORSE CODE" title
y=40   tree labels start
y=62..249  tree nodes
y=261  pattern row
y=270  commit countdown bar
y=276  decoded result / hints
```

---

## 3. Constraints and decisions

### 3.1 Hardware

- **Two IdeaSpark ESP32-WROOM boards**, each with the on-board 1.9″ ST7789
  (170×320) and two physical buttons: BOOT (GPIO0, readable) and RESET/EN
  (hardware reset, not readable).
- **One usable input button.** No keyboard, no touch, no speaker documented.
- The display is the only output. Incoming attention cue = backlight pulse and
  on-screen card (no buzzer).
- ESP32-WROOM: 2.4 GHz WiFi, 4 MB flash, 320 KB RAM. The existing image is about
  323 KB flash / 24 KB RAM; adding WiFi, HTTP and JSON still leaves plenty of
  room with the `huge_app.csv` partition.
- The current UI already allocates a 170×320×2 = **108,800-byte** `GFXcanvas16`.
  That is the largest single RAM consumer and must be kept.

### 3.2 Network

- Laptop: Omarchy (Arch-based), hostname `omarchy`, reserved address
  `192.168.4.44` on `wlo1` (DHCP reservation).
- The ESP32s and the laptop are on the same home WiFi (2.4 GHz; ESP32 cannot see
  5 GHz-only SSIDs). This plan assumes both boards are on that LAN; see the note
  in §1.
- **No port forwarding, no cloud dependency in the MVP.** The laptop listens on
  the LAN only.
- The laptop must stay awake and reachable. Keep the DHCP reservation; if the
  address changes, update `RELAY_HOST` in `.env` and regenerate `secrets.h`.
- Guest WiFi / AP client isolation must be off, or the ESP32s will see the
  network but not the laptop.

### 3.3 Phone / watch path (later)

The original goal was a watch notification. This plan keeps that door open:
the laptop can later mirror every message to **ntfy** (or Telegram), and the
phones/watch receive it through the normal notification path. No firmware change
is required for that bridge. A native Zepp OS app can also use the same HTTP API
later.

### 3.4 Decisions for the MVP

| Decision | Choice | Why |
|---|---|---|
| Transport | HTTP over LAN, POST + long-poll | `HTTPClient` is built into the ESP32 core; no broker or extra library; easy to test with `curl`; survives reconnects |
| Server | Python 3 + FastAPI + SQLite | `uv`/Python are available on this laptop; async long-poll is natural; SQLite is enough for a family |
| Message routing | One shared `family` channel | With two devices this is 1:1; `to` field is already there for later |
| Source of truth | Laptop SQLite DB | Survives ESP32 resets; one place for history and read state |
| Auth | Per-device bearer token | Simple, LAN-appropriate; prevents accidental cross-talk |
| TLS | None on LAN in MVP | Avoid certificate handling on the ESP32; document the trust boundary |
| Send action | Explicit action sheet | Safe; no accidental sends; does not collide with Morse tap patterns |
| Incoming UI | Modal message card + inbox | Legible; one button can navigate it |
| Time display | Relative age from server (`age_s`) | ESP32 has no RTC; no NTP dependency |
| Text | ASCII A–Z + space for now | Matches the current decoder; server accepts UTF-8 for the future |

### 3.5 Non-goals for the MVP

- No internet/remote messaging (the ntfy bridge is Phase 4).
- No native Zepp OS app yet.
- No OTA updates.
- No second hardware button (design for one; support a second later).
- No digits/punctuation in the Morse tree.
- No group chats beyond the shared family channel.
- No message recall/editing.

---

## 4. Architecture

```
        ┌──────────────────────────┐
        │ ESP32 A (DAD)           │
        │  Morse input + 170×320   │
        │  compose / inbox / send  │
        └────────────┬─────────────┘
                     │ WiFi 2.4 GHz
                     │ HTTP POST / long-poll
        ┌────────────▼─────────────┐
        │ Omarchy laptop           │
        │  FastAPI relay :8180     │
        │  SQLite message store    │
        │  auth + presence + ack   │
        └────────────┬─────────────┘
                     │ WiFi
        ┌────────────▼─────────────┐
        │ ESP32 B (EMMA)           │
        │  same firmware,           │
        │  different device ID/token│
        └──────────────────────────┘

  Optional later:
  laptop ──HTTPS──> ntfy.sh topic ──push──> phones ──BLE──> Zepp watches
  laptop <──HTTP stream── ntfy inbound topic <── phones
```

### Components

1. **Relay server** (`server/`): stores messages, authenticates devices,
   delivers new messages, tracks read state and online presence, serves a small
   web chat page.
2. **Firmware network layer** (`net.cpp/h`): WiFi lifecycle, HTTP POST for
   outbound, long-poll for inbound, retry/backoff, presence.
3. **Firmware message store** (`messages.cpp/h`): inbox ring buffer, outbox
   queue, unread count, read-ack tracking, NVS persistence of the last sequence.
4. **Firmware UI** (`ui.cpp/h`): compose screen plus three new screens —
   incoming message card, inbox browser, action sheet — and toasts/status.
5. **One-button input state machine** (`main.cpp`): modes and gesture handling.

### Data flow

**Send**

1. User composes text in Morse.
2. Hold BOOT ≥1.5 s → action sheet opens with `SEND` highlighted.
3. Hold BOOT again → message is copied to the outbox queue and the compose
   buffer clears immediately.
4. Network TX task POSTs it; server assigns a global `seq` and stores it.
5. UI shows `SENT` (green) or `QUEUED` (yellow) if offline.

**Receive**

1. Network RX task holds `GET /api/v1/messages?since=<lastSeq>&wait=20`.
2. Server returns as soon as a message for this device exists, or after 20 s
   with an empty list.
3. Firmware appends to the inbox ring, increments unread, and pushes an event
   to the UI.
4. If the device is idle and not composing, open the message card automatically;
   otherwise show a toast + unread badge and let the user open the inbox.
5. When the card has been visible for ≥1 s, mark it read locally and POST an ack
   (debounced).

---

## 5. Protocol

### 5.1 Identity

- Each ESP32 has a stable `device_id` (`dad`, `emma`) and a 48-hex-char bearer
  token stored in the repo-root `.env`, compiled into `secrets.h` by
  `scripts/gen-secrets.py` on the machine that flashes the board.
- The server maps the token to the device; **the client never supplies `from`**.
- Device display names (`DAD`, `EMMA`) live in the server config, not in the
  firmware, so they can be changed without reflashing.

### 5.2 Message envelope

```json
{
  "seq": 42,
  "from": "dad",
  "from_name": "DAD",
  "to": "family",
  "text": "HELLO",
  "ts": 1763312345,
  "age_s": 12,
  "client_msg_id": "dad-000042-7"
}
```

- `seq`: server-assigned, monotonically increasing, global.
- `from`: server-derived device ID.
- `to`: `"family"` (default) or a specific device ID (future 1:1).
- `ts`: server Unix seconds.
- `age_s`: server-computed age at response time; the device adds elapsed millis
  while the message is on screen, so no RTC is needed.
- `client_msg_id`: idempotency key generated by the sender.

### 5.3 Endpoints

All endpoints except `/health` and the web UI require
`Authorization: Bearer <token>`.

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/v1/messages` | Send a message |
| `GET` | `/api/v1/messages?since=<seq>&wait=<s>&limit=<n>` | Long-poll for messages |
| `POST` | `/api/v1/ack` | Mark messages read up to `upto_seq` |
| `GET` | `/api/v1/state` | Devices, presence, unread count, latest seq |
| `GET` | `/health` | Liveness/version |
| `GET` | `/` | Small web chat UI (optional but recommended) |

#### `POST /api/v1/messages`

Request:

```json
{
  "client_msg_id": "dad-000042-7",
  "to": "family",
  "text": "HELLO"
}
```

Response `201`:

```json
{
  "seq": 42,
  "from": "dad",
  "from_name": "DAD",
  "to": "family",
  "text": "HELLO",
  "ts": 1763312345,
  "duplicate": false
}
```

- Repeating the same `(from, client_msg_id)` returns the existing row with
  `"duplicate": true` and `200` instead of inserting.
- `to` defaults to `"family"`; `to` must be a known device ID or `"family"`.
- Text is trimmed; empty/whitespace-only is rejected with `400`.
- Maximum text length: 512 bytes (firmware sends at most 96).

#### `GET /api/v1/messages`

Query:

- `since` (required, int): last sequence the device has processed; `0` = from
  the beginning.
- `wait` (optional, 0–25 s): long-poll timeout. Default `0` (return
  immediately).
- `limit` (optional, 1–50): maximum messages to return. Default `20`.

Response `200`:

```json
{
  "messages": [
    {
      "seq": 42,
      "from": "dad",
      "from_name": "DAD",
      "to": "family",
      "text": "HELLO",
      "ts": 1763312345,
      "age_s": 3
    }
  ],
  "latest_seq": 42,
  "truncated": false,
  "unread": 1,
  "peer_online": true,
  "server_time": 1763312348
}
```

Semantics:

- Only messages addressed to this device (`to == "family"` or `to == device_id`)
  are returned.
- The sender's own messages are **not** returned; the POST response is its
  confirmation.
- If more than `limit` messages are new, return the **newest** `limit` and set
  `"truncated": true`. The device jumps its `lastSeq` to `latest_seq` and shows
  "N NEW"; older messages remain available in the web UI.
- While waiting, the server wakes as soon as a matching message is inserted.
- Every request updates the device's `last_seen` (presence).
- `peer_online` is true if any **other** ESP32 has been seen in the last 45 s.
  The optional `web` device is excluded so an open browser tab does not fake
  presence.
- The long-poll must not hold a database transaction open.

#### `POST /api/v1/ack`

```json
{ "upto_seq": 42 }
```

The server sets `last_read_seq = max(last_read_seq, upto_seq)` for the device.
Return `200` with the new unread count. Acks are idempotent.

#### `GET /api/v1/state`

```json
{
  "devices": [
    { "id": "dad", "name": "DAD", "online": true,  "last_seen_s": 2 },
    { "id": "emma", "name": "EMMA", "online": false, "last_seen_s": 830 }
  ],
  "unread": 2,
  "latest_seq": 42
}
```

Presence rule: `online = now - last_seen < 45 s` (polls are every ~20 s).

### 5.4 Sequence, delivery, ack

- `seq` is global and monotonic. It is the only ordering the clients need.
- The device persists `lastSeq` in NVS. On boot it polls with that value, so
  messages received while powered off are delivered.
- `last_read_seq` is server-side per device and drives the unread count.
- The device acks the highest **displayed** message, not the highest received.
  Messages that arrive while the user is away stay unread.
- If a message is truncated out of the poll window, it is not auto-acked; the
  web UI remains the place to read older history.

### 5.5 Retry and idempotency

- The sender keeps an outbox queue in RAM (and optionally NVS for the last few
  messages).
- `client_msg_id` format: `<device_id>-<boot_id>-<counter>`, where `boot_id` is
  a random 16-bit value generated at boot. This avoids collisions after a
  reboot and makes retries safe.
- On network failure: keep the message queued, retry with exponential backoff
  (1 s, 2 s, 5 s, 10 s, 30 s max).
- On duplicate response: mark the queued item sent, do not re-insert.
- On `401`: stop retrying, show `AUTH ERR`, retry slowly (60 s); a wrong token
  will not fix itself.
- On `5xx`/timeout: normal backoff.

### 5.6 Limits

| Limit | Value | Reason |
|---|---|---|
| Text | 512 bytes server, 96 chars firmware | Fits screen and JSON buffer |
| Messages/min per device | 20 | Accidental loop protection |
| Poll wait | 25 s max | Keeps proxies/clients happy |
| Returned messages | 50 max | Bounded JSON payload |
| DB retention | last 1000 messages | Small, predictable |

### 5.7 Errors

Use a consistent JSON error body:

```json
{ "error": "invalid_token", "detail": "..." }
```

Status codes: `400` validation, `401` auth, `403` wrong device, `404` unknown
route, `429` rate limit, `500` server error.

---

## 6. Server design

### 6.1 Stack

- **Python 3.14** + **FastAPI** + **uvicorn**.
- **SQLite** via `aiosqlite` (WAL mode).
- `uv` for the environment (already installed: 0.12.13).
- No Redis, no MQTT broker, no Docker required.
- One process; one SQLite file. Two devices and a handful of web clients are
  far below any scaling concern. Run **one uvicorn worker**: the long-poll
  condition lives in-process, so multiple workers would each have their own
  waiters.

Why not MQTT/WebSocket first: a broker or persistent socket adds another
dependency and more reconnect state. HTTP long-poll is boring, debuggable with
`curl`, and good enough for two devices. The message model is transport-neutral;
a WebSocket transport can be added later behind the same API.

`server/pyproject.toml`:

```toml
[project]
name = "morse-relay"
version = "0.1.0"
requires-python = ">=3.11"
dependencies = [
  "fastapi>=0.115",
  "uvicorn[standard]>=0.30",
  "aiosqlite>=0.20",
]

[dependency-groups]
dev = ["pytest>=8", "pytest-asyncio>=0.24", "httpx>=0.27"]
```

### 6.2 Server layout

```
server/
  pyproject.toml
  app.py                 FastAPI app, routes, startup/shutdown
  config.py              TOML + .env loading, env overrides
  db.py                  schema, queries, retention
  auth.py                token -> device, constant-time compare
  models.py              Pydantic request/response models
  static/index.html      optional web chat UI
  data/morse.db          created at runtime (gitignored)
  config.toml            committed: server settings + device list, no secrets
  tests/
  scripts/smoke.sh       end-to-end curl acceptance
  morse-relay.service    systemd unit example
.env                     gitignored: WiFi, relay address, device tokens
.env.example             committed template
scripts/init-env.sh      creates .env with fresh tokens
scripts/gen-secrets.py   writes firmware/src/secrets.h from .env
```

### 6.3 Database schema

```sql
PRAGMA journal_mode = WAL;
PRAGMA busy_timeout = 5000;

CREATE TABLE IF NOT EXISTS devices (
  id            TEXT PRIMARY KEY,
  name          TEXT NOT NULL,
  last_seen     REAL NOT NULL DEFAULT 0,
  last_read_seq INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS messages (
  seq           INTEGER PRIMARY KEY AUTOINCREMENT,
  client_msg_id TEXT NOT NULL,
  from_device   TEXT NOT NULL REFERENCES devices(id),
  to_device     TEXT NOT NULL DEFAULT 'family',
  text          TEXT NOT NULL,
  created_at    REAL NOT NULL,
  UNIQUE(from_device, client_msg_id)
);

CREATE INDEX IF NOT EXISTS idx_messages_seq ON messages(seq);
CREATE INDEX IF NOT EXISTS idx_messages_to  ON messages(to_device, seq);
```

- Tokens live in `.env`, not the DB, so the DB can be copied without leaking
  secrets.
- Retention job: delete messages older than the newest 1000, every hour.
- Backups: copy the SQLite file while the server is stopped, or use
  `sqlite3 data/morse.db ".backup 'backup.db'"`.

### 6.4 Long-poll implementation

- A single `asyncio.Condition` is notified after every successful insert.
- `GET /messages`:
  1. Update `last_seen`.
  2. Query for matching messages after `since`.
  3. If empty and `wait > 0`, loop:
     - `async with condition: await asyncio.wait_for(condition.wait(), timeout=remaining)`
     - re-query; return when non-empty or timeout.
  4. Compute `unread`, `peer_online`, `latest_seq`, `age_s` and return.
- Handle client disconnect (`await request.is_disconnected()`) so abandoned
  long-polls do not keep tasks alive.
- Use `asyncio.to_thread`/`aiosqlite` for DB calls; never block the event loop.

### 6.5 Configuration

One rule: **`server/config.toml` is committed and contains no secrets; the
repo-root `.env` is gitignored and holds every secret.**

`server/config.toml` (tracked) — server settings and the device list:

```toml
[server]
host = "0.0.0.0"
port = 8180
db = "data/morse.db"
family_name = "MORSE"
retention = 1000

[ntfy]
enabled = false
publish_topic = ""
subscribe_topic = ""

[[devices]]
id = "dad"
name = "DAD"
peer = true

[[devices]]
id = "emma"
name = "EMMA"
peer = true
```

`.env` (repo root, gitignored, `chmod 600`) — every secret:

```bash
WIFI_SSID="home-2.4"
WIFI_PASS="..."

RELAY_HOST="192.168.4.44"
RELAY_PORT="8180"

MORSE_TOKEN_DAD="<openssl rand -hex 24>"
MORSE_TOKEN_EMMA="<openssl rand -hex 24>"
```

- `scripts/init-env.sh` creates `.env` from `.env.example` with a fresh token
  per device. `scripts/gen-secrets.py dad|emma` turns `.env` into
  `firmware/src/secrets.h` for the board being flashed.
- The server maps `device.id` to `MORSE_TOKEN_<ID>` (uppercase, `-` → `_`). A
  token left in `config.toml` is a startup error, not a silent secret.
- Real environment variables win over `.env`: `MORSE_ENV_FILE`,
  `MORSE_CONFIG`, `MORSE_HOST`, `MORSE_PORT`, `MORSE_DB`, and the token
  variables. `NTFY_TOKEN` is reserved for Phase 4.
- `peer = false` marks an identity such as the web chat that must not satisfy
  the ESP32 `peer_online` indicator.
- `.env.example` is the only environment file in version control. `.env` is
  ignored by `.env` and `.env.*` rules with an explicit `!.env.example`.

### 6.6 Auth and security

- Bearer token per device; compare with `hmac.compare_digest`.
- The server sets `from` from the token, never from the request body.
- `.env` holds WiFi credentials and tokens; it is `chmod 600`, gitignored, and
  never logged. Tokens are not stored in SQLite either.
- Bind to the LAN; do **not** port-forward. If the laptop has a public
  interface, bind to the LAN IP instead of `0.0.0.0`, or firewall port 8180.
- No TLS in the MVP: the token and message text are visible on the local WiFi.
  Acceptable for a trusted WPA2/3 home network; document it. If the network is
  shared, add HTTPS or Tailscale later.
- Rate-limit sends per device (simple in-memory token bucket, 20/min).
- Limit request body size (e.g., 8 KB) and text length.
- The web UI uses the same bearer token in `localStorage`; it renders message
  text with `textContent` (no HTML injection).

### 6.7 Web chat UI (recommended)

A single static page served at `/`:

- Shows the message history, sender name, age, and device online dots.
- A text box + Send button; posts to `/api/v1/messages` as the special
  `web` device (add the `[[devices]]` block to `config.toml` and
  `MORSE_TOKEN_WEB` to `.env`) so messages show as `WEB`; do not reuse a
  person's token.
- Polls `GET /messages?since=...` every 2 s; no build step.
- Login prompt for the family token; stored in `localStorage`.
- This is the fastest way to test the server without hardware, and it gives
  phones/laptops on the LAN a usable chat view.

Do not add a framework; one `index.html` with vanilla JS is enough.

### 6.8 Deploying on Omarchy

Create the environment and run:

```bash
cd ~/ws/morse-code
./scripts/init-env.sh       # first run: creates .env with fresh tokens
$EDITOR .env                # WiFi credentials + RELAY_HOST
cd server
uv sync
uv run uvicorn app:app --host 0.0.0.0 --port 8180
```

The server reads the repo-root `.env` by absolute path, so the systemd unit
below needs no `EnvironmentFile=`. Real environment variables override it.

Systemd **user** unit (`~/.config/systemd/user/morse-relay.service`):

```ini
[Unit]
Description=Morse family message relay
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=%h/ws/morse-code/server
ExecStart=%h/ws/morse-code/server/.venv/bin/uvicorn app:app --host 0.0.0.0 --port 8180 --log-level info
Restart=always
RestartSec=2
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=default.target
```

Enable it and make it survive logout:

```bash
systemctl --user daemon-reload
systemctl --user enable --now morse-relay
loginctl enable-linger "$USER"     # currently Linger=no
systemctl --user status morse-relay
journalctl --user -u morse-relay -f
```

Open the firewall for the LAN only (example for `ufw`; adapt if Omarchy uses
`firewalld`/nftables directly). Find the subnet with
`ip -o -f inet addr show wlo1`, then allow only that CIDR (the example
assumes `192.168.4.0/22`; adjust):

```bash
sudo ufw allow from 192.168.4.0/22 to any port 8180 proto tcp
```

Keep the laptop awake with the lid closed if it is the dedicated relay. A
logind drop-in is the least invasive:

```ini
# /etc/systemd/logind.conf.d/90-morse-relay.conf
[Login]
HandleLidSwitch=ignore
HandleLidSwitchExternalPower=ignore
```

Then `sudo systemctl restart systemd-logind`. This affects the whole laptop;
use `systemd-inhibit --what=handle-lid-switch ...` around the server if that is
too broad.

### 6.9 Operations

- **Stable address:** the laptop's WiFi MAC has a DHCP reservation for
  `192.168.4.44`. If it ever changes, update `RELAY_HOST` in `.env` and
  regenerate `secrets.h` on both ESP32s.
- **Health check:** `curl http://192.168.4.44:8180/health`.
- **Logs:** `journalctl --user -u morse-relay -f`; message sends are logged as
  `tx dad -> family: HELLO`.
- **Backups:** nightly `sqlite3 ... ".backup"` to a local folder.
- **Time:** the laptop is NTP-synced; server timestamps are authoritative.
- **WiFi power saving:** if latency is poor, test with
  `iw dev wlo1 set power_save off` (temporary) before changing
  NetworkManager config.

---

## 7. Firmware design

### 7.1 New modules

```
firmware/src/
  config.h            add network/UI timing constants
  secrets.h           NEW, generated from .env, gitignored
  net.h / net.cpp     NEW: WiFi lifecycle, TX, RX, presence, ack
  messages.h/.cpp     NEW: inbox ring, outbox queue, unread/read state
  ui.h / ui.cpp       extend: status, message card, inbox, action sheet, toast
  main.cpp            extend: app modes, one-button state machine
```

`secrets.h` is generated from `.env` by `scripts/gen-secrets.py` and is
gitignored. Do not add a hand-written secrets template under `firmware/src`;
`.env.example` is the template.

### 7.2 Tasks and queues

The current `loop()` runs every 5 ms and must stay responsive. Do not perform
HTTP requests from it.

| Task | Core | Responsibility |
|---|---|---|
| `uiTask` (existing `loop`) | 1 | Button polling, Morse state machine, all rendering/SPI |
| `netTxTask` | 0 | Wait on the outbox queue, POST messages, retry/backoff |
| `netRxTask` | 0 | WiFi reconnect, long-poll, parse JSON, push inbound events |

Shared data:

- `QueueHandle_t outboxQueue` — `OutMessage` (text + client ID), depth 8.
- `QueueHandle_t inboundQueue` — `InboundEvent` (message + seq), depth 8.
- `QueueHandle_t ackQueue` — highest seq to ack, depth 1 (coalesced).
- The UI task owns the inbox ring and display state. Network tasks never call
  `ui*` or touch SPI.

Use a mutex only if the network task needs to read shared status (e.g.,
`lastSeq`). Prefer passing copies through queues.

Task stacks: 6 KB each for TX/RX (JSON + HTTP), 8 KB for the UI task if
needed. Monitor high-water marks in the boot log.

### 7.3 WiFi lifecycle

- `WiFi.mode(WIFI_STA)`, `WiFi.persistent(false)`,
  `WiFi.setAutoReconnect(true)`, `WiFi.setSleep(false)` for low latency.
- Connect with a 15 s timeout; on failure retry every 10 s with a short
  backoff. Never block the UI task.
- Statuses exposed to the UI: `WIFI_OFF`, `WIFI_CONNECTING`, `WIFI_OK`,
  `SERVER_OK`, `AUTH_ERROR`.
- On reconnect, reset the HTTP client and resume polling from `lastSeq`.
- `WiFi.setHostname("morse-dad")` for router visibility.
- Log the obtained IP once per connection.

### 7.4 Outbound (TX)

```
UI action SEND
  -> copy text + client_msg_id to outboxQueue
  -> clear compose buffer, toast "QUEUED"
netTxTask
  -> pop message
  -> HTTPClient POST /api/v1/messages
  -> 201/200: toast "SENT" (or update status), continue
  -> 401: status AUTH_ERROR, keep item, backoff 60 s
  -> 4xx (not 401): drop item, toast "REJECTED", log body
  -> 5xx/timeout/WiFi down: keep item, backoff 1/2/5/10/30 s
```

- `HTTPClient` per request; `setConnectTimeout(5000)`,
  `setTimeout(10000)`, `setReuse(true)`.
- Set `Content-Type: application/json` and the bearer token.
- Keep the JSON body small; use `JsonDocument` or `snprintf` with escaped text.
  Text is A–Z + space, so escaping is trivial; still validate.
- `client_msg_id`: `dad-<bootId>-<counter>`; `bootId` random at boot.
- Optional NVS persistence: store up to 4 pending outbox items under the
  `morse` namespace; rewrite on enqueue/dequeue. This is low-write and worth it
  for a device that may be unplugged mid-send.

### 7.5 Inbound (RX)

- Long-poll:
  `GET /api/v1/messages?since=<lastSeq>&wait=20&limit=20`
- `HTTPClient` timeout 25 s (server returns at 20 s).
- Parse the response with **ArduinoJson** (add dependency). A 4–8 KB JSON
  document is enough for 20 short messages; enforce a payload cap.
- For each message:
  - Ignore if `seq <= lastSeq` (duplicate/backlog overlap).
  - Copy `from_name`, `text`, `age_s`, `seq` into an `InboundEvent`.
  - Push to `inboundQueue`; if full, drop the oldest? No — drop the newest and
    log, because the server will re-deliver after `lastSeq` is advanced. Better:
    advance `lastSeq` only after the UI accepts the event; if the queue is full,
    stop polling until space is available. Simpler for 2 devices: queue depth 8
    and backpressure by not updating `lastSeq` until enqueued.
- Update `lastSeq` after enqueue; persist to NVS after each accepted message,
  coalescing bursts over a 2 s window (at family message rates, NVS wear is
  negligible). Do not write on every poll. If a message arrives that is longer
  than `TEXT_MAX`, truncate it to 96 characters plus `...` for the device; the
  web UI keeps the full text.
- On `truncated: true`: set `lastSeq = latest_seq`, show `N NEW`, and let the
  web UI hold the older history.
- Empty response (`messages: []`): update `peer_online`, `unread`, and loop.
- On `401`: status `AUTH_ERROR`; retry every 60 s.
- On malformed JSON: log, skip, advance `lastSeq` to `latest_seq` if present to
  avoid a poison loop.
- `age_s` is stored per message; the UI displays
  `age_s + (millis() - receivedAt)/1000`.

### 7.6 Ack

- When a message card has been visible for ≥1 s, or the inbox selection lands
  on a message, mark it read locally and set `ackUpto = max(ackUpto, seq)`.
- A 1 s debounce pushes `ackUpto` to `ackQueue`; `netTxTask` (or a small
  `netAckTask`) POSTs `/api/v1/ack`.
- On failure, keep the ack pending and retry with the next poll; acks are
  idempotent.
- Do not ack messages that were only shown in a 3 s toast/banner.

### 7.7 Persistence (NVS)

Use `Preferences` (`morse` namespace):

| Key | Type | When written |
|---|---|---|
| `last_seq` | `uint32` | After each accepted message, coalesced over 2 s |
| `outbox` | `string` (JSON) | On enqueue/dequeue, max 4 items |
| `boot_id` | `uint16` | Once at first boot |

Avoid writing on every poll; flash wear matters. Writing `last_seq` once per
accepted message (coalesced over 2 s) is fine at family message rates and avoids
replaying messages after an unclean power loss.

### 7.8 Memory budget

| Item | Approx. |
|---|---|
| `GFXcanvas16` 170×320×2 | 108.8 KB (existing) |
| WiFi + lwIP | 40–50 KB |
| Two network task stacks | 12 KB |
| ArduinoJson document | 4–8 KB |
| Inbox ring (16 × ~130 B) | 2 KB |
| Outbox queue (8 × ~160 B) | 1.3 KB |
| HTTPClient buffers | 2–4 KB |
| **Total added** | **~65–80 KB** |

The existing image has ~200 KB free heap at boot; the result should still be
comfortable. Log `ESP.getFreeHeap()` every 60 s and after each long-poll
timeout during development.

### 7.9 Logging

Keep the existing boot banner and add:

```
net: wifi connecting to "home-2.4"
net: wifi ok ip=192.178.4.42
net: server ok (dad)
tx: client=dad-1a2b-7 text="HELLO"
tx: ok seq=42
rx: seq=42 from=emma len=5
rx: poll timeout (20s) unread=0
ack: upto=42
heap: 182344
```

On errors, log the HTTP status and a short body; never log the token.

---

## 8. One-button UX

This is the hardest part. The design goal is: **a single button can type Morse,
send, read messages, and clear — without accidental destructive actions.**

### 8.1 Principles

1. **Modes are explicit.** The screen always says which mode you are in and what
   the button will do.
2. **Short press = dot/navigation; long press = commit/intent.** Morse dashes
   stay below 1.5 s; the long-press threshold is separate.
3. **Destructive actions live inside a menu.** No single long hold clears text.
4. **Incoming messages never steal a press that is in progress.** Queue the
   event until the button is released and the current letter is committed.
5. **Every modal has a visible escape:** `HOLD: CLOSE`.
6. **The action sheet is the only place that sends or clears**, so the user
   cannot send by accident while typing.

### 8.2 Modes

| Mode | Purpose | Enter | Exit |
|---|---|---|---|
| `COMPOSE` | Morse typing (default) | Boot, close modal | Long-press → `ACTIONS` |
| `ACTIONS` | Send / clear / inbox / close | Long-press in `COMPOSE` | Select an item, or select `CLOSE` |
| `MESSAGE` | Read one message (incoming or inbox) | Incoming while idle, or `INBOX` action | Tap through, or long-press → `COMPOSE` |

Toasts and the unread badge are overlays, not modes.

### 8.3 Gestures

| Mode | Gesture | Duration | Result |
|---|---|---|---|
| `COMPOSE` | tap | < 220 ms | dot |
| `COMPOSE` | hold | 220 ms – 1.49 s | dash |
| `COMPOSE` | long hold | ≥ 1.5 s | open `ACTIONS` on release |
| `ACTIONS` | tap | < 600 ms | move selection down (wrap) |
| `ACTIONS` | long hold | ≥ 1.5 s | activate selected item |
| `MESSAGE` | tap | < 600 ms | next message (wrap) |
| `MESSAGE` | long hold | ≥ 1.5 s | close → `COMPOSE` |

Notes:

- `1.5 s` reuses `CLEAR_HOLD_MS`, renamed conceptually to `ACTION_HOLD_MS`.
- The old "hold 1.5 s = clear" gesture is removed; clearing is now
  `HOLD → CLEAR → HOLD`. This is intentional: clear is destructive and rarer
  than send.
- A press that starts in `COMPOSE` and crosses 1.5 s shows `ACTIONS` in the
  pattern row (instead of `CLEAR`) and opens the sheet on release.
- Before opening `ACTIONS`, commit any pending letter (`commitLetter(now)`) so
  `SEND` includes the letter that was being typed. The long hold itself is not a
  Morse symbol; the user should finish the last symbol, then hold to open the
  sheet.
- Modal taps do not type Morse because the mode is explicit.

**Quick reference:**

| Action | Gestures |
|---|---|
| Send | `HOLD` (opens sheet) → `HOLD` (`SEND`) |
| Clear | `HOLD` → `TAP` (to `CLEAR`) → `HOLD` |
| Read inbox | `HOLD` → `TAP` to `INBOX` → `HOLD` |
| Next message | `TAP` |
| Close message | `HOLD` |
| Cancel sheet | `TAP` to `CLOSE` → `HOLD` |

### 8.4 Screen mockups

#### COMPOSE (idle, online, unread)

```
┌────────────────────────────────┐
│ ● DAD              2 NEW      │  y=4, 1× status/badge
│          MORSE CODE            │  y=18, 2× title
│                                │
│        (Morse tree)            │  y=40..249
│                                │
│   ·−·−                         │  y=261 pattern
│   ▁▁▁▁▁▁▁▁                     │  y=270 countdown
│          TAP DOT               │  y=283
│          HOLD DASH             │  y=301
│       HOLD 1.5s: ACTIONS       │  y=310, 1× hint
└────────────────────────────────┘
```

- Status dot: green = server ok, yellow = WiFi connecting, red = offline,
  red `AUTH` = token rejected.
- `2 NEW` appears when unread > 0; otherwise the top-right shows the peer name
  or `OFFLINE`.
- When text is non-empty, the result area shows the decoded text as today.
- When the button is held ≥1.5 s, the pattern row shows `ACTIONS` with a
  progress underline.

#### ACTIONS (bottom sheet)

```
┌────────────────────────────────┐
│          MORSE CODE            │
│        (tree, still visible)   │
│                                │
│  ┌──────────────────────────┐  │
│  │ ACTIONS                  │  │  y=176
│  │ ▸ SEND                   │  │  selected: green bar + green text
│  │   CLEAR                  │  │
│  │   INBOX  2               │  │
│  │   CLOSE                  │  │
│  │ TAP NEXT · HOLD PICK     │  │  y=304
│  └──────────────────────────┘  │
└────────────────────────────────┘
```

- Items are built dynamically:
  - `SEND` and `CLEAR` only when `st.len > 0`.
  - `INBOX` always; shows unread count if > 0.
  - `CLOSE` always.
- Default selection: `SEND` if text exists, else `INBOX` if unread > 0, else
  `CLOSE`.
- Selecting `SEND` enqueues the message, clears the compose buffer, closes the
  sheet, and shows a toast.
- Selecting `CLEAR` clears text and closes; toast `CLEARED`.
- Selecting `INBOX` enters `MESSAGE` at the newest unread (or the newest
  message if all are read); if empty, show `MESSAGE` with `NO MESSAGES`.
- Selecting `CLOSE` returns to `COMPOSE`.

#### MESSAGE (incoming or inbox)

```
┌────────────────────────────────┐
│ NEW MESSAGE            1/2     │  y=8, 1×
│ FROM DAD              2M      │  y=26, 2× sender, 1× age
│ ─────────────────────────────  │  y=48 divider
│                                │
│ HELLO, ARE YOU OK?             │  body 1×, 26 chars/line
│ MEET ME AT THE PARK            │
│ AT SIX                         │
│                                │
│                                │
│ TAP NEXT · HOLD CLOSE          │  y=304, 1×
└────────────────────────────────┘
```

- For an auto-opened incoming message: title `NEW MESSAGE`.
- For inbox browsing: title `INBOX` and `x/y`, where `1/N` is the newest
  message and `N/N` is the oldest.
- Sender in `COL_GREEN_IN`; body in `COL_WHITE`; meta in `COL_HINT`.
- A 3 px green accent bar down the left edge makes the card feel intentional.
- Mark read after the card has been visible for 1 s; ack in the background.
- If more messages arrive while reading, the counter updates (for example
  `1/3` → `2/3`) but the displayed message does not jump.

#### Toast

```
        ┌──────────────────┐
        │  SENT            │   green border/text
        └──────────────────┘
```

- Position: centered around y=230, above the pattern row.
- Kinds: `SENT` (green), `QUEUED` (yellow), `CLEARED` (hint), `OFFLINE` (red),
  `AUTH ERR` (red), `NEW: DAD` (green).
- Duration: 1.5 s for status, 3 s for `NEW: DAD`.

### 8.5 Incoming-message policy

| Situation | Behavior |
|---|---|
| `COMPOSE`, buffer empty, no press, idle ≥1.5 s | Auto-open `MESSAGE` with the newest message; pulse backlight; mark read after 1 s |
| `COMPOSE`, buffer empty, button held or letter in progress | Queue; show toast `NEW: DAD`; open after release + commit if still idle |
| `COMPOSE`, buffer non-empty | Do **not** auto-open; toast + unread badge; user opens `INBOX` from `ACTIONS` |
| `ACTIONS` | Toast; update `INBOX n` count; do not switch |
| `MESSAGE` | Append to inbox; update `x/y`; do not jump |
| Multiple arrivals | Show newest first in `MESSAGE`; `1/N` = newest, `N/N` = oldest; `TAP` moves to older messages |
| Backlog after reconnect | If idle, auto-open newest; badge shows total unread |

Rationale: auto-opening is delightful when the device is sitting on a desk, but
rude when the user is mid-word. The buffer check is the simplest reliable proxy
for "busy".

### 8.6 Alternatives considered

| Alternative | Why not (as the default) |
|---|---|
| Auto-send after a silence gap | Sends half-finished thoughts; useful as an opt-in later |
| Double/triple tap to send | Collides with Morse `I` (`..`), `S` (`...`), `H` (`....`) |
| Very long hold (≥3 s) to send/clear | Slow, error-prone, and conflicts with the dash/clear timing |
| Second physical button | Best ergonomics, but requires a hardware mod; keep as an optional upgrade |
| Swipe/touch | The panel is not a touchscreen |
| Send every committed letter | Too chatty, no message boundaries |

### 8.7 New timing constants

Add to `config.h`:

```cpp
#define ACTION_HOLD_MS        1500  // long-press opens ACTIONS / selects in a modal
#define NAV_TAP_MAX_MS         600  // tap vs. hold inside modals
#define INCOMING_IDLE_MS      1500  // idle before auto-opening a message
#define TOAST_MS              1500  // normal status toast
#define TOAST_NEW_MS          3000  // "NEW: DAD" toast
#define ACK_DEBOUNCE_MS       1000  // read-ack batching
#define BACKLIGHT_PULSE_MS     400  // incoming attention pulse
#define NET_POLL_WAIT_S         20  // server long-poll wait
#define NET_RETRY_MIN_MS      1000
#define NET_RETRY_MAX_MS     30000
```

`CLEAR_HOLD_MS` can remain as an alias for `ACTION_HOLD_MS` during the
transition, then be removed.

---

## 9. UI implementation plan

### 9.1 A view model for the renderer

Do not let `ui.cpp` call networking code. Extend `ui.h` with a small view model:

```cpp
enum UiMode : uint8_t { UI_COMPOSE, UI_ACTIONS, UI_MESSAGE };
enum UiLink : uint8_t { LINK_OFF, LINK_WIFI, LINK_OK, LINK_AUTH };
enum ToastKind : uint8_t { TOAST_NONE, TOAST_INFO, TOAST_OK, TOAST_WARN, TOAST_ERR };

struct UiMessage {
  uint32_t seq;
  char     from[12];
  char     text[TEXT_MAX + 1];
  uint32_t ageAtReceipt;   // seconds from server
  uint32_t receivedAt;     // millis
  bool     read;
};

struct UiModel {
  MorseState st;
  UiMode     mode;
  const UiMessage* msg;    // current message in UI_MESSAGE
  uint8_t    msgIndex;     // 1-based for display
  uint8_t    msgCount;
  uint8_t    actionIndex;
  uint8_t    actionCount;
  const char* actionLabels[5];
  bool       hasSend;      // SEND/CLEAR available
  uint8_t    unread;
  UiLink     link;
  const char* peerName;
  const char* toast;
  ToastKind  toastKind;
  uint32_t   toastUntil;
  bool       progressHold; // ≥1.5 s hold: show ACTIONS instead of CLEAR
  uint32_t   holdStartedAt;
};
```

The UI task builds the model from the app state and passes it to the renderer.
The preview tool can build the same model with fake data.

Implementation notes:

- The 5×7 font is ASCII-only. Draw status dots with `gfx->fillCircle`, not
  Unicode `●`; use `...` for ellipsis.
- `actionLabels` must point at storage inside the app state (for example,
  `char actionBuf[5][16]` in `UiModel`), not at a short-lived stack buffer.
- Truncate `from_name` to 11 characters for the `from[12]` field.

### 9.2 Renderer functions

Keep the existing partial-refresh approach; add:

| Function | Flushes | Purpose |
|---|---|---|
| `uiCompose(const UiModel&, now)` | full | Compose screen + status + badge |
| `uiStatus(const UiModel&, now)` | top strip (y=0..16) | Link dot, peer/unread |
| `uiActions(const UiModel&, now)` | bottom sheet + dim area | Action sheet |
| `uiMessage(const UiModel&, now)` | full | Incoming/inbox card |
| `uiToast(const UiModel&, now)` | toast rect | Transient feedback |
| `uiHold(const UiModel&, now)` | pattern row + underline | `ACTIONS` / `CLEAR` preview while holding |

Add `PREVIEW_ACTIONS` to `MorsePreview`; `drawPattern` renders the word
`ACTIONS` (green, with a progress underline) when the hold crosses 1.5 s.
`uiFull(MorseState, now)` can remain as a thin wrapper for the preview and for
`uiCompose` with default status, to minimize churn.

Toasts draw over the tree, so when a toast expires, repaint the covered strip or
the full frame; the canvas is composed off-screen, so there is no black flash.

### 9.3 Layout and wrapping

- 1× font: 6×7 px cell; 2×: 12×14 px.
- Body text area: x=8, y=64, width 154 px → 25 chars/line at 1×.
- Line height: 12 px; 5 lines fit in 60 px. A 96-char message uses 4 lines.
- Word-wrap helper:
  - Break at spaces; hard-break words longer than 25 chars.
  - Maximum 5 lines; append `...` if truncated (firmware only sends 96, but
    incoming web/phone messages can be longer).
  - Preserve explicit spaces as word gaps (the Morse decoder inserts them).
- Sender/meta: 2× sender at y=26; age right-aligned at y=30 in 1×.
- Footer hint at y=304, 1×, centered, `COL_HINT`.
- Reuse the existing palette; add:
  - `COL_CARD` = `C(14, 22, 40)`
  - `COL_CARD_EDGE` = `C(60, 80, 110)`
  - `COL_DIM` = `C(70, 84, 106)`

### 9.4 Preview scenes

Extend `tools/preview/main.cpp` with fake `UiModel` scenes and render them with
the real `ui.cpp`. Update `scripts/preview.sh` only if the montage needs a wider
tile; it already globs all frames.

Add scenes:

1. Compose idle + online + `2 NEW`.
2. Compose with text + `QUEUED 1`.
3. Holding ≥1.5 s: pattern row shows `ACTIONS`.
4. Action sheet with `SEND` selected (text present).
5. Action sheet with no text (only `INBOX`/`CLOSE`).
6. Incoming message card, short text, `NEW MESSAGE 1/2`.
7. Incoming message card, 96-char wrapped text.
8. Inbox `2/3` with a read message.
9. Toast `SENT`.
10. Offline + `AUTH ERR` status.

This is the main way to iterate on the one-button UX without flashing two
boards for every layout change.

---

## 10. Configuration

### 10.1 Secrets: `.env` and generated `secrets.h`

Single source of truth: the repo-root `.env` (gitignored, `chmod 600`).

```bash
WIFI_SSID="home-2.4"
WIFI_PASS="..."

RELAY_HOST="192.168.4.44"
RELAY_PORT="8180"

MORSE_TOKEN_DAD="<openssl rand -hex 24>"
MORSE_TOKEN_EMMA="<openssl rand -hex 24>"
```

Run `./scripts/init-env.sh` once to create it from `.env.example` with fresh
tokens, then edit the WiFi credentials.

`scripts/gen-secrets.py <device>` writes `firmware/src/secrets.h`:

```cpp
// Generated by scripts/gen-secrets.py from .env - do not edit or commit.
#pragma once

#define WIFI_SSID    "home-2.4"
#define WIFI_PASS    "..."
#define SERVER_HOST  "192.168.4.44"
#define SERVER_PORT  8180
#define DEVICE_ID    "dad"
#define DEVICE_NAME  "DAD"          // logs; the server name wins on screen
#define DEVICE_TOKEN "<generated>"
```

- Run it on the machine that flashes the board:
  `./scripts/gen-secrets.py dad`, then `./scripts/gen-secrets.py emma`.
- `make secrets DEVICE=dad` and
  `firmware/build-arduino-cli.sh --device dad` are the same operation.
- The generator refuses an existing `secrets.h` unless `--force`, rejects
  placeholder WiFi values or tokens, and writes the file `chmod 600`.
- `.env` holds both boards' tokens, so one checkout configures both boards.

### 10.2 Server config

See §6.5. `server/config.toml` is committed and contains only non-secret
settings plus the device list. If it contains a `token` key the server refuses
to start, which keeps secrets from drifting back into version control.

### 10.3 Build changes

- `platformio.ini`: add `bblanchon/ArduinoJson@^7.2.0`.
- `Makefile`:
  - Add `firmware/src/net.cpp`, `net.h`, `messages.cpp`, `messages.h`,
    `secrets.h` to `SOURCES` and the `stage` copy list.
  - Keep the `secrets` target (`make secrets DEVICE=dad|emma`).
  - Add a `check` for ArduinoJson (`arduino-cli lib list | grep -qi ArduinoJson`).
  - Keep `make preview` host-only; do not compile `net.cpp`/`messages.cpp` into
    the preview binary.
- `firmware/build-arduino-cli.sh`: copy the new source files; `--device <id>`
  regenerates `secrets.h` first.
- Document `arduino-cli lib install ArduinoJson` in the README.

---

## 11. Implementation phases

Estimates assume focused evenings; hardware iteration usually adds calendar
time.

### Phase 0 — Foundations (0.5 day)

- [x] Choose device IDs/names (`dad`, `emma`) and generate tokens.
- [x] Create `server/` skeleton with `/health`, config loading, SQLite schema.
- [x] Add `.env.example`, `server/config.toml`, and the secret generator.
- [x] Smoke test with `curl`: send, poll, ack, state.
- [x] Add the plan link to the README.

**Acceptance:** `curl` can send a message and another `curl` long-poll receives
it; `/health` is green.

### Phase 1 — Send-only firmware (1 day)

- [ ] WiFi connect/reconnect in `net.cpp`; status shown in the top strip.
- [ ] Outbox queue + `netTxTask` + POST.
- [ ] Action sheet with `SEND`/`CLEAR`/`CLOSE`.
- [ ] Toast feedback (`SENT`, `QUEUED`, `OFFLINE`).
- [ ] Server logs the message; web UI optional.
- [ ] Update the README **Controls** and troubleshooting sections for the new
      `HOLD → action sheet` gesture (the old 1.5 s hold-to-clear is gone).
- [ ] Update `Makefile`, `platformio.ini`, `build-arduino-cli.sh` for the new
      files and ArduinoJson.

**Acceptance:** Type `HELLO`, long-press → `SEND` → long-press; the server log
and web UI show it; the compose buffer clears; no Morse input is disturbed.

### Phase 2 — Receive + inbox (2 days)

- [ ] Server long-poll, `ack`, presence, `unread`, `age_s`.
- [ ] `netRxTask` + JSON parse + inbound queue + `lastSeq` NVS.
- [ ] Inbox ring, unread badge, auto-open-when-idle policy.
- [ ] `MESSAGE` card + inbox navigation + read acks.
- [ ] `NEW: DAD` toast while composing.
- [ ] Preview scenes for all new screens.

**Acceptance:** A sends to B; B shows the card in <1 s, acks it; A sees
`peer_online`; B can browse the inbox; long messages wrap; incoming during
compose does not lose text.

### Phase 3 — Robustness and polish (1.5 days)

- [ ] Retry/backoff, offline status, outbox persistence, duplicate handling.
- [ ] Backlog/truncated handling (`N NEW`).
- [ ] Backlight pulse on incoming.
- [ ] Relative age display; no RTC.
- [ ] Heap logging; task high-water marks.
- [ ] Server retention job and rate limit.

**Acceptance:** WiFi drop/reconnect, server restart, duplicate POST, power
cycle, and a 5-message burst all behave; no duplicates or lost sends.

### Phase 4 — Web chat + phone/watch bridge (1 day)

- [ ] Static web chat at `/` (history, send, presence).
- [ ] Optional ntfy outbound bridge: every family message → phone push →
      Zepp watch mirror.
- [ ] Optional ntfy inbound topic → server for phone replies.
- [ ] Document topic/token setup and the two-topic echo rule.

**Acceptance:** A message from the ESP32 reaches both phones and both watches;
a reply from the phone reaches the ESP32.

### Phase 5 — Hardening and docs (1 day)

- [ ] Pytest suite for server.
- [ ] 24 h soak with two boards.
- [ ] Systemd unit, linger, firewall, lid/suspend notes.
- [ ] Update README controls and troubleshooting.
- [ ] Record a short demo video/GIF (optional).

**Acceptance:** 24 h with no reboot, no memory decline, no lost messages;
`journalctl` clean; docs match reality.

**Total:** roughly 6–7 focused days, spread over 2–3 weeks of evenings.

---

## 12. Test plan

### 12.1 Server unit/integration (pytest + httpx)

- Auth: missing/bad token → 401; token maps to the right device.
- Send: valid text → 201; empty/too long → 400; `to` validation.
- Idempotency: same `client_msg_id` twice → one row, `duplicate: true`.
- Long-poll: insert from another task wakes the waiter in <1 s; timeout returns
  empty after `wait`.
- Routing: sender does not receive its own message; `family` and direct `to`
  work; other devices do not see direct messages.
- Ack: unread decreases; acks are idempotent; ack > latest is clamped.
- Truncation: > limit new messages returns the newest and `truncated: true`.
- Retention: only the newest 1000 remain.
- Rate limit: 21st message in a minute → 429.
- Disconnect: cancelling a long-poll does not leak tasks.

### 12.2 Firmware host preview

- Render all scenes from §9.4; check text wrap, status colors, selection
  highlight, footer hints.
- Build with `make preview`; inspect `tools/preview/out/preview.png`.
- Add a tiny host test for the wrap function if it is separated from drawing.

### 12.3 Hardware integration matrix

| Case | Expected |
|---|---|
| A sends to B | B card <1 s; B acks; A shows `SENT` |
| B is composing | B keeps text; toast + badge; no mode steal |
| B is idle | B auto-opens newest |
| B is off | A queues; server stores; B fetches on boot |
| Server restart | Both reconnect; no duplicates |
| WiFi drop | Status `OFFLINE`; sends queue; reconnect resumes |
| Wrong token | `AUTH ERR`; slow retry; log |
| Duplicate POST | One message; `duplicate: true` |
| 5-message burst | Ordered; inbox count correct; no drops |
| 96-char message | 4 lines; no overflow |
| Power cycle | `last_seq` survives; no replay; unread correct |
| Button held when message arrives | No mode change mid-press |
| Long-press in compose | Action sheet, not clear |
| Select CLEAR | Text gone; no send |
| Select SEND with empty text | SEND not shown |
| Server DB locked/backup | 5xx/backoff; recovers |
| 24 h soak | Heap stable; no watchdog resets |

### 12.4 Latency targets

- LAN send→receive: <300 ms typical, <1 s worst case.
- Long-poll reconnect: <100 ms after a 20 s timeout on LAN.
- UI input latency: unchanged (5 ms loop); network tasks must not block SPI.

### 12.5 Failure injection

- Stop the server mid-poll; confirm timeout/backoff and UI stays responsive.
- Change the token to an invalid one; confirm no retry storm.
- Fill the outbox (send 9 messages offline); confirm `BUSY`/queue cap behavior.
- Reboot the router; confirm reconnect.
- Move out of WiFi range; confirm `OFFLINE` and queued sends.

---

## 13. Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| One-button UX feels confusing | Medium | High | Explicit modes, on-screen hints, preview scenes, tunable timings; consider optional second button |
| Accidental clear/send | Medium | Medium | Destructive actions only in the sheet; no single-hold clear; send confirmation |
| Long-poll blocks or leaks | Medium | Medium | Separate tasks, timeouts, disconnect handling, backoff, heap logs |
| Laptop sleeps / IP changes | High | High | logind lid drop-in, DHCP reservation, health check, static IP fallback |
| AP client isolation | Medium | High | Use the main SSID; test from both boards; document |
| WiFi 5 GHz-only SSID | Low | High | Use a 2.4 GHz SSID or mixed band |
| Flash/RAM exhaustion | Low | Medium | Budget in §7.8; monitor heap; reduce JSON limit if needed |
| NVS wear | Low | Low | Debounce writes; no per-poll writes |
| Token leakage on LAN | Low | Medium | Trusted WPA2/3; no port forward; HTTPS/Tailscale later |
| Server DB corruption | Low | High | WAL, backups, small retention |
| Message ordering/duplicates | Low | Medium | Global `seq`, `client_msg_id`, idempotent ack |
| Text too small on 1.9″ | Medium | Medium | 1× body with 25 chars/line; test on hardware; can switch to paged 2× |
| Incoming auto-open interrupts | Medium | Medium | Only when buffer empty and idle; toast otherwise |

---

## 14. Future extensions

1. **ntfy / Telegram bridge** — phones and Zepp watches receive mirrored
   notifications without a native app. This is the shortest path to the original
   watch goal.
2. **Native Zepp OS mini-app** — inbox, native `@zos/notification.notify`,
   canned replies and a Morse keypad. Uses the same HTTP API via the side
   service.
3. **Optional second button** — GPIO27 momentary switch: short = send, long =
   inbox, double = clear. Best ergonomics; no firmware protocol change.
4. **Web PWA** — installable chat page with Web Push on the LAN (or via ntfy).
5. **Digits and punctuation** — expand the Morse tree and decoder.
6. **Message recall / delete** — server-side soft delete + client sync.
7. **Read receipts** — show `READ` on the sender's device.
8. **BLE direct mode** — watch/phone talks to the ESP32 without WiFi, using the
   Zepp `easy-ble` ESP32 example. Good for same-room use.
9. **OTA updates** — ArduinoOTA on the LAN for two boards.
10. **Encryption** — if the LAN is not trusted, add HTTPS with a small CA or a
    Tailscale/WireGuard path.

---

## 15. Open questions

Resolved on 2026-11-16:

1. **Same LAN.** Both boards are on the laptop's network, so this is the real
   deployment, not a local prototype. Phase 4 stays optional.
2. **Names.** Device IDs `dad` and `emma`; display names `DAD` and `EMMA`.
3. **Send gesture.** Long-press opens ACTIONS; a second long-press on `SEND`
   sends. The old 1.5 s hold-to-clear is removed — `CLEAR` lives only inside
   ACTIONS, so it cannot fire while typing.
4. **LAN-only first milestone.** The phone/watch bridge remains Phase 4.
5. **Relay address.** The laptop keeps `192.168.4.44` via a DHCP reservation.

Still open:

1. Confirm both boards are the same IdeaSpark/ST7789 revision and both have a
   working BOOT button.
2. Is the WiFi network a mixed 2.4/5 GHz SSID with client isolation off? What is
   the SSID?
3. Is it acceptable for the laptop to ignore the lid switch while it acts as the
   relay?
4. Is the current A–Z + space alphabet enough for the first version?

---

## 16. File checklist

```
docs/messaging-plan.md            this document
.env.example                      committed secret template
.env                              local only, gitignored
scripts/init-env.sh               create .env with random tokens
scripts/gen-secrets.py            generate firmware/src/secrets.h from .env
server/
  pyproject.toml
  app.py
  config.py
  db.py
  auth.py
  models.py
  README.md
  config.toml                     committed, no secrets
  static/index.html
  tests/test_api.py
  tests/test_config.py
  scripts/smoke.sh
  morse-relay.service
firmware/src/
  secrets.h                       generated, gitignored
  net.h
  net.cpp
  messages.h
  messages.cpp
  config.h                        new timing constants
  main.cpp                        modes + queues + tasks
  ui.h                            UiModel, new render functions
  ui.cpp                          status/card/inbox/action sheet/toast
tools/preview/main.cpp            new scenes
Makefile                          new sources, secrets target, ArduinoJson check
firmware/platformio.ini           ArduinoJson dependency
firmware/build-arduino-cli.sh     copy new sources, --device
README.md                         link to this plan + new controls
.gitignore                        .env, secrets.h, server data
```

---

## 17. Definition of done (MVP)

- Two ESP32s boot, join WiFi, and show `ONLINE`/peer status.
- Typing Morse is unchanged: tap dot, hold dash, pause commits, long pause
  spaces.
- A long-press opens the action sheet; `SEND` delivers the message to the other
  board in under a second on the LAN.
- The other board shows a readable message card, pulses the backlight, and marks
  it read; the sender sees `SENT`.
- An inbox holds at least 16 messages; tap cycles, long-press closes.
- Incoming messages while composing never lose the compose buffer.
- WiFi drops, server restarts, and power cycles do not lose or duplicate
  messages.
- `make preview` renders all new screens, and the README documents the controls.
- The server runs as a systemd user service on the Omarchy laptop, survives
  logout, and is reachable only on the LAN.
