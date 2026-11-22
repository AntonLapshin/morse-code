# Morse-code Worker

Cloudflare Worker + Durable Object backend for morse-code. Replaces the
0.1.x laptop relay (FastAPI + SQLite, deleted in 0.2.0). The API is
unchanged since the laptop relay — see the protocol section below — so
the ESP32 firmware and the web sim needed no protocol changes, only a
transport change (HTTPS to the Worker instead of plain HTTP to the LAN)
and a longer long-poll wait.

## Layout

```
worker/
  wrangler.toml          Worker + DO binding (custom domain comes from .env)
  test-config/
    wrangler.test.toml   test-only config (fake tokens, no route)
  .dev.vars.example      copy to .dev.vars for local dev (gitignored)
  src/index.js           Worker entry: config check, Bearer auth, routing, CORS
  src/morse-do.js        MorseRelay Durable Object: messages, long-poll, ack
  src/store.js           SQLite persistence (ctx.storage.sql)
  tests/                 vitest: unit (fake storage) + integration (workerd)
```

`wrangler` is a devDependency here (installed by `make setup` into
`worker/node_modules/.bin`). Every wrangler invocation goes through a
make target or `npx` from `worker/` — no global install needed:

- recommended: `make worker-dev` / `make worker-test` / `make worker-deploy` / `make worker-secrets` / `make worker-logs` from the repo root;
- direct: `cd worker && npx wrangler <cmd>`;
- optional: `npm install -g wrangler` if you prefer bare commands.

## How it works

One Durable Object instance (`MorseRelay`, id `"family"`) owns all
messages. It keeps the rate limiter and the long-poll waiters in memory;
SQLite (`ctx.storage.sql`) holds the durable copy: `devices` and
`messages` tables with the same table and column names as the deleted
FastAPI relay (`server/db.py`), plus a `meta` schema-version row.

Long-poll: `GET /api/v1/messages?since=&wait=` registers a waiter
(`{since, resolve, timer}`, `wait` clamped 0–25 s) on the DO; any insert
resolves all waiters whose `since < latest_seq`. `blockConcurrencyWhile`
is deliberately NOT used for the wait (30 s timeout, blocks the DO).
Retention pruning runs on a DO alarm (hourly, `setAlarm`), not on every
request.

CORS is for the browser sim only (the ESP32 does not use it):
`http://localhost:8077` is always allowed; set `MORSE_CORS_ORIGINS`
(comma-separated, as a Worker var / secret / `.dev.vars` entry) to allow
more origins, e.g. the deployed Worker URL when running `make sim`
against it.

## Protocol (unchanged from the laptop relay)

All `/api/v1/*` calls need `Authorization: Bearer <device token>`.
Error bodies keep the old relay's shape: `{error, detail}`.

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/v1/messages` | Send a message |
| `GET` | `/api/v1/messages?since=<seq>&wait=<s>&limit=<n>` | Long-poll for messages |
| `POST` | `/api/v1/ack` | Mark messages read up to `upto_seq` |
| `GET` | `/api/v1/state` | Devices, presence, unread count, latest seq |
| `GET` | `/health` (also `/api/v1/health`) | Liveness/version, no auth |

`POST /api/v1/messages` `{client_msg_id, to, text}` → `201`
(`200` + `"duplicate": true` when the same `(from, client_msg_id)` is
sent again). `to` defaults to `"family"`; `to` must be a known device
id or `"family"` (`400` otherwise, `400` for sending to yourself).
Text is trimmed; empty / whitespace-only or over 512 bytes is `400`.
The sender's own messages are never returned by the poll; the POST
response is its confirmation.

`GET /api/v1/messages` returns
`{messages, latest_seq, truncated, unread, peer_online, server_time}`.
Messages carry `{seq, from, from_name, to, text, ts, age_s}`. If more
than `limit` messages are new, the newest `limit` are returned with
`"truncated": true` and the caller jumps its `lastSeq` to `latest_seq`.
`peer_online` is true if any other device has been seen in the last
45 s. `POST /api/v1/ack` `{upto_seq}` sets
`last_read_seq = max(last_read_seq, upto_seq)` and returns the new
unread count. Sends are rate-limited to 20/minute per device (`429`).

One intentional difference from the laptop relay: `/health` returns
`{status: "ok", version}` only — no `latest_seq`. That number lives on
authenticated `GET /api/v1/state` now.

## Free-tier limits

- Requests: 100,000/day — a 20 s long-poll is ~4,320 requests/day per
  connected device, fine for two boards. (At the old `wait=0` polling
  it would have been ~43,200/day per device — that is why the firmware
  and sim poll with `wait=20` now.)
- **Duration: 13,000 GB-s/day is the binding constraint.** A pending
  long-poll keeps the DO non-hibernatable, so it bills duration while a
  device is polling. 24 h of polling ≈ 10,800 GB-s — most of the daily
  budget, but boards only poll while they are on, and two boards
  running a few hours a day are far under it. If duration ever becomes
  a problem, shorten the client `wait` or poll only while the board is
  awake; do not "fix" it by moving the wait into `blockConcurrencyWhile`.
- CPU: 10 ms/invocation on Free — the per-request path is single-row
  inserts and indexed selects, ~1 ms; there is no per-request replay
  or move generation to blow the budget.
- DO SQLite: 10 GB total; this relay is kilobytes (retention: newest
  1,000 messages).

## Setup

From the repo root:

```bash
make setup   # installs worker deps, copies .env.example -> .env
             # and worker/.dev.vars.example -> worker/.dev.vars
```

Generate two *different* tokens with `openssl rand -hex 24`. The SAME
tokens go in the repo-root `.env` (firmware build), `worker/.dev.vars`
(local dev), and the Worker secrets — and `make worker-secrets` pushes
the `.env` values straight to Cloudflare on stdin, so there is no
copy-paste step that can make them equal or stale. Put the device names
in `.env` and `worker/.dev.vars` too (`MORSE_DAD_NAME` /
`MORSE_EMMA_NAME`) — the deploy passes them to the Worker, and local dev
reads them from `.dev.vars`.

## Local dev

```bash
make worker-dev   # or: cd worker && npm run dev — http://localhost:8787
```

Then `make sim` in the repo root (the sim talks to
`http://localhost:8787` by default — see `MORSE_SERVER_URL` in `.env`).

Smoke check against the dev server:

```bash
curl -s http://localhost:8787/api/v1/health
# {"status":"ok","version":"0.2.0"}

TOKEN_DAD=...   # MORSE_TOKEN_DAD from .env
TOKEN_EMMA=...  # MORSE_TOKEN_EMMA from .env
curl -s -X POST http://localhost:8787/api/v1/messages \
  -H "Authorization: Bearer $TOKEN_DAD" \
  -H 'Content-Type: application/json' \
  -d '{"client_msg_id":"dad-1-1","text":"HELLO"}'
curl -s -G http://localhost:8787/api/v1/messages \
  -H "Authorization: Bearer $TOKEN_EMMA" \
  --data-urlencode since=0 --data-urlencode wait=20
curl -s http://localhost:8787/api/v1/state \
  -H "Authorization: Bearer $TOKEN_DAD"
```

The sim cannot talk to the production Worker: its HTTP client is plain
HTTP over raw sockets, with no TLS stack, so an `https://` URL reports
"sim cannot do TLS" on the link dot. Production is tested with `curl`
as above, not with the sim.

## Deploy + cutover

```bash
make worker-login     # first time only: authorizes wrangler (opens a browser)
make worker-secrets   # pushes MORSE_TOKEN_DAD / MORSE_TOKEN_EMMA from .env
make worker-deploy    # deploys; attaches the custom domain from MORSE_SERVER_URL
```

`make worker-deploy` runs `scripts/worker-deploy.sh`, which reads
`MORSE_SERVER_URL` from the repo-root `.env` and passes its host to
`npx wrangler deploy --domain "<host>"`. No route or domain is declared
in `wrangler.toml` — the hostname lives only in that `.env` value.
Cloudflare creates/updates the proxied DNS record and certificate for
the custom domain, so a fresh deploy works even if the host previously
pointed at another origin (an old site, a website builder, etc.). Do not
add a route or DNS entry by hand; routing through `--route` would keep
the old origin in front and never reach the Worker.

The first deploy that includes `wrangler.toml` also runs the
`[[migrations]]` entry and creates the `MorseRelay` Durable Object
namespace. A dashboard / drag-and-drop upload does **not** have the
migration, so it creates no namespace and the Worker cannot serve
messages.

If `MORSE_SERVER_URL` is unset, localhost, or a `*.workers.dev` URL, the
deploy is plain (workers.dev URL only). To validate the deploy without
deploying or logging in: `bash scripts/worker-deploy.sh --dry-run`.

Device names come from `.env` too: the script passes `MORSE_DAD_NAME` /
`MORSE_EMMA_NAME` as `--var` flags on every deploy, overriding the
DAD/EMMA defaults in `wrangler.toml` `[vars]`. Without `--var` a deploy
would silently revert the names to those defaults. Dashboard alternative:
Workers & Pages → morse-code → Settings → Variables → add
`MORSE_DAD_NAME` / `MORSE_EMMA_NAME` as plain Variables (not Secrets).

Dashboard half of the deploy (dash.cloudflare.com; also summarized in
the root README): the custom domain must be a Cloudflare zone (DNS
managed by Cloudflare) in the same account as the Worker. After
deploying, check Workers & Pages → morse-code for the deployment and
version 0.2.0, and Workers & Pages → morse-code → Settings → Domains &
Routes for the custom domain (Cloudflare also shows the DNS record and
certificate it created). Settings → Variables → Secrets is the dashboard
alternative to `make worker-secrets`; keep both tokens distinct.
Workers & Pages → Durable Objects lists the `MorseRelay` namespace after
the first real deploy — a redeploy keeps it, so messages survive
deploys. Observability → Logs (or `make worker-logs`) is where runtime
errors surface. If wrangler reports an auth error, run `make worker-login`
again (or `cd worker && npx wrangler whoami` to check who is logged in).

### Deploy troubleshooting

| Symptom | Cause / fix |
|---|---|
| `curl https://<host>/api/v1/health` returns `302 Found` from `openresty` (or another server) | The DNS record still points at an old origin, so Cloudflare never routes to the Worker. Re-run `make worker-deploy` (it uses `--domain`, which rewrites the record) and wait for the custom-domain certificate to become active. |
| `{"error":"config_error","detail":"...must differ"}` | The two deployed secrets are equal (usually a copy-paste slip). `make worker-secrets` pushes the distinct `.env` values. |
| `{"error":"config_error","detail":"...must be at least 16 characters"}` | Regenerate both tokens with `openssl rand -hex 24`, update `.env` (and `.dev.vars`), then `make worker-secrets`. |
| Worker exists but no Durable Object namespace | The script was not deployed with `wrangler.toml` (e.g. dashboard upload). Run `make worker-deploy` from `worker/` with the repo config. |
| Custom-domain deploy fails with a DNS conflict | Delete the conflicting DNS record for the host in the Cloudflare dashboard (DNS → Records), then re-run `make worker-deploy`; the custom domain recreates it as a proxied record. |

Cutover from the old laptop relay — including deleting the laptop
server, the systemd unit, and the `ufw` 8180 rule — is a separate
checklist in [`MIGRATION.md`](MIGRATION.md). Read it before deploying:
the old message history does NOT migrate, and both boards must be
re-flashed (the firmware's `proto_gen` reset depends on the re-flash).

## Firmware TLS note

The ESP32 talks to the Worker over HTTPS with `WiFiClientSecure` and
`setInsecure()` — the certificate chain is NOT verified or pinned in
0.2.0 (see the prominent comment in `firmware/src/net.cpp`). That is
the same trust level as the LAN MVP it replaces (plain HTTP on a
trusted home WiFi), but it is not as safe as a pinned chain: a network
attacker who can intercept the board's traffic could present any
certificate and read the bearer token and message text. The TODO in
`net.cpp` is to bundle the Cloudflare CA and switch to `setCACert()`.

## Tests

```bash
npm test              # unit: fake-storage DO + Worker entry
npm run test:workers  # integration: real workerd + SQLite via SELF
```

The integration run uses `test-config/wrangler.test.toml` (fake tokens,
no route); never deploy with it. It is independent of `worker/.dev.vars`
— see the comment at the top of that file for why it lives in a
subdirectory. `make test` at the repo root runs both suites.

Every case in the deleted `server/tests/test_api.py` has a Worker
equivalent in `tests/morse-do.test.mjs` (same test names where
possible), plus rate-limit, poll-parameter validation, name-var, and
retention/alarm coverage.
