# Morse Code

A pocket Morse-code trainer for the **IdeaSpark ESP32-WROOM** board with its
on-board **1.9″ ST7789 170×320 LCD**. Tap the BOOT button for a dot, hold it for
a dash, and the display walks the A–Z Morse tree while the decoded text builds
up at the bottom.

![UI preview](docs/preview.png)

The look follows [`ui.jpeg`](ui.jpeg): dark navy background, pale-yellow title,
slate circuit-tree with circular (dot) and rectangular (dash) nodes, and a
bright green glow on the node you are currently on.

> **Messaging is live over the internet:** two ESP32s exchange messages
> through a Cloudflare Worker + Durable Object at the HTTPS URL in
> `MORSE_SERVER_URL` (repo-root `.env`) — no laptop server, no open port.
> `make worker-deploy` attaches that host as a Workers Custom Domain and
> creates the Durable Object on the first deploy. See
> [`worker/README.md`](worker/README.md) and the cutover checklist in
> [`worker/MIGRATION.md`](worker/MIGRATION.md). The original LAN plan is
> kept as history in [`docs/messaging-plan.md`](docs/messaging-plan.md)
> (marked superseded there).

## Controls

The board has two physical buttons: **BOOT** (GPIO0) and **RESET/EN**.

| Action | Result |
|---|---|
| Tap **BOOT** (< 220 ms) | dot (`·`) |
| Hold **BOOT** (220 ms – 1.5 s) | dash (`−`) |
| Pause 0.7 s after a symbol | commit the current letter |
| Pause 4.0 s after a letter | insert a word space |
| Hold **BOOT** ≥ 1.5 s | open the action menu: `SEND` / `CLEAR` / `READ n` / `CLOSE` (tap cycles, hold picks; `SEND` clears the buffer, `READ n` opens the newest message) |
| In a message: tap | read this message (it is removed) and show the next one |
| In a message: hold ≥ 1.5 s | open the `NEXT` / `CLOSE` menu (tap cycles, hold picks; both remove the shown message) |
| Press **RESET/EN** | clear all decoded text (the board reboots) |
| Serial `MSG <text>` | simulate an incoming message (unique `#id`, `N NEW` badge top-right) |

The **top-left dot** is the link indicator on every screen: red = no WiFi
connection, orange = WiFi OK but no connection to the relay, green = both OK.
(The dot is live on both the board and the sim: red while WiFi connects,
orange when WiFi is up but the relay is unreachable, green once the relay
answers. An `AUTH ERR` toast after `SEND` means the device token in `.env`
does not match the relay's.)

**About the RESET/EN button.** On the IdeaSpark board the button labelled
`RESET` (the left-hand button) is wired to the ESP32 `EN` pin, not a normal
GPIO. It cannot be read by software, so pressing it reboots the ESP32 — which
also clears the text. The firmware re-initialises the panel without a reset
pulse or `SWRESET`, keeps the last frame in the panel's GRAM, and turns the
backlight on immediately, so the reboot no longer blanks the screen. For a
dedicated clear button that does *not* reboot, wire a momentary switch between
GND and a free GPIO and set `BTN_CLEAR_PIN` in `firmware/src/config.h` (for
example `27`). See [`docs/hardware.md`](docs/hardware.md).

> GPIO0 is also the ESP32 boot-strap pin. Do not hold BOOT while resetting or
> flashing, or the board will enter the serial bootloader instead of the app.

## Relay: Cloudflare Worker + Durable Object

The relay is a Cloudflare Worker + Durable Object in [`worker/`](worker/README.md).
The firmware joins the WiFi, POSTs outbound messages to it over HTTPS, and
long-polls it (20 s wait) for link status and inbound messages; the sim
exercises the same path from the host against the *local* Worker dev server.
No laptop needs to run, no port is open, and the boards work from anywhere
with internet — including Emma's place, not just the home LAN.

```bash
./scripts/init-env.sh   # first run: creates .env with fresh tokens, chmod 600
$EDITOR .env            # set WIFI_SSID + WIFI_PASS, check MORSE_SERVER_URL
make setup              # installs worker deps, copies worker/.dev.vars.example
make worker-dev         # local Worker at http://localhost:8787
make worker-test        # worker unit + integration tests
```

Deploying (see [`worker/MIGRATION.md`](worker/MIGRATION.md) for the full
cutover checklist, including deleting the old laptop relay and its `ufw`
8180 rule — history does NOT migrate, and both boards must be re-flashed):

```bash
make worker-login       # first time only
make worker-secrets     # pushes MORSE_TOKEN_DAD / MORSE_TOKEN_EMMA from .env
make worker-deploy      # attaches the custom domain from MORSE_SERVER_URL
```

`make worker-deploy` runs `wrangler deploy --domain <host>`, so Cloudflare
creates/updates the proxied DNS record and certificate itself — do not add a
route or DNS entry by hand. The first deploy also creates the `MorseRelay`
Durable Object namespace from the migration in `worker/wrangler.toml`.
Smoke-test the URL from `.env` without hardcoding the hostname:

```bash
set -a; . ./.env; set +a
curl -s "$MORSE_SERVER_URL/api/v1/health"    # {"status":"ok","version":"0.2.0"}
```

A `302 Found` from `openresty` (or any other origin) means the host still
resolves to an old website: re-run `make worker-deploy` and wait for the
custom-domain certificate, then retry. A `500 config_error` saying the tokens
must differ means the deployed secrets were pasted incorrectly — run
`make worker-secrets` (it reads the tokens straight from `.env`).

Secrets live in `.env` (gitignored); see
[`worker/README.md`](worker/README.md) for setup, the API, free-tier
limits, and the curl smoke test. One deliberate trade-off to know about:
the ESP32 uses `WiFiClientSecure` with `setInsecure()` in 0.2.0 — the
certificate chain is not verified or pinned (see the comment in
`firmware/src/net.cpp`); pinning the Cloudflare CA is a TODO.

Flashing a board copies its slice of `.env` into the firmware:

```bash
./scripts/gen-secrets.py dad    # then make upload, or:
./firmware/build-arduino-cli.sh --device dad --upload /dev/ttyUSB0
```

Re-flashing after the 0.2.0 cutover is required, not optional: the DO
starts with an empty database and `seq` restarts at 1, so a board that
kept its LAN-era `last_seq` would ignore every new message. The firmware
resets `last_seq` to 0 on first boot after re-flash via the `proto_gen`
NVS key (`MORSE_PROTO_GEN = 3` in `firmware/src/config.h`; 3 also recovers
boards that ran the 0.2.0 TX bug which skipped peer messages after a send).

## How it works

The screen is a stylised A–Z Morse tree:

- **Dot branches go right/down, dash branches go left/down.**
- A **circle** node is a letter that ends in a dot; a **rectangle** ends in a dash.
- The **green halo** marks the current node, and the root-to-node path lights up.
- The **flash ring** confirms every accepted symbol or committed letter.
- The **pattern row** shows the dots/dashes typed for the
  current letter (yellow pulse = the symbol you are currently holding).
- The **thin bar** under the pattern row is the letter-commit countdown.
- The **bottom line** is the decoded text; the newest character flashes green.
  Up to 96 characters are stored, and the last 26 (two rows of 13) are visible.

The renderer composes each frame in a 170×320 `GFXcanvas16` held in RAM and
blits it to the panel in one SPI write. Refreshes never clear the physical
screen first, so there is no black flash when a symbol is entered: the panel
only ever receives complete strips or complete frames.

Idle hints (`TAP DOT` / `HOLD DASH`) are shown until the first character is
decoded.

## Build and flash

### arduino-cli (recommended, no Python needed)

Install the ESP32 core and the two Adafruit libraries once:

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit ST7735 and ST7789 Library" "Adafruit GFX Library"
```

Then:

```bash
make check                        # toolchain + /dev/ttyUSB0 sanity check
make build DEVICE=dad             # compile only (needs secrets.h, see below)
make upload-dad PORT=/dev/ttyUSB0 # regen dad secrets.h, compile + flash
make upload-emma PORT=/dev/ttyUSB0
make monitor PORT=/dev/ttyUSB0    # boot log + live key log @ 115200
```

Flashing always regenerates `firmware/src/secrets.h` from `.env` for
`DEVICE` (default `dad`), so one checkout flashes both boards:
`make upload-dad`, swap the USB cable, `make upload-emma`. `make build`
does not regenerate — run `make secrets DEVICE=emma` first if `secrets.h`
is missing or stale.

`make` stages the sources into `/tmp/morse-code-sketch` because `arduino-cli`
wants a sketch folder. You can also use the standalone script:

```bash
./firmware/build-arduino-cli.sh
./firmware/build-arduino-cli.sh --upload /dev/ttyUSB0
```

### PlatformIO

```bash
cd firmware
pio run                 # compile
pio run -t upload       # flash
pio device monitor      # serial log
```

The PlatformIO environment uses `board = esp32dev` and
`board_build.partitions = huge_app.csv`.

### Arduino IDE

Open `firmware/src/main.cpp` as a sketch (the three `.cpp`/`.h` pairs must sit
in the same sketch folder), select **ESP32 Dev Module**, and upload. Install
the Adafruit ST7789 and GFX libraries from the Library Manager.

## Host-side UI preview

The renderer (`firmware/src/ui.cpp`) can be compiled on a normal computer
against small Adafruit GFX/ST7789 shims, so you can iterate on the layout
without a camera pointed at the panel:

```bash
make preview
# -> tools/preview/out/preview.png
```

It renders twelve scenes (idle, first dot, `HELLO`, `SOS HELP`, a mid-letter `S`,
wrapped text, a held dash, the armed menu hold, the action sheet, the unread
badge, a message card, and the message `NEXT`/`CLOSE` menu), scales them 2×, and
montages them with ImageMagick. The preview uses the *real* tree table and
renderer, so a coordinate fix there is a coordinate fix on the device.
Requirements: a C++17 compiler and ImageMagick (`magick`).

## Display simulator (live web UI, 2 devices + local Worker)

`tools/sim/` is a two-ESP32-device simulation with the display: it links the
*real* `ui.cpp` + `morse.cpp` against host shims and serves two pixel-exact
`170×320` panels (Dad + Emma) in the browser, calling the same API the
firmware uses (`uiBegin / uiAttach / uiFull / uiPattern / uiResult /
uiProgress`) with the same input state machine as `main.cpp`.

Each virtual device runs its own relay poller against the **local Worker
dev server**, so a Morse `SEND` on Dad's panel appears on Emma's panel. The
**top-left dot** shows the link state on every screen: red = no WiFi, orange
= WiFi OK but no relay connection, green = both OK.

```bash
make worker-dev          # first: local Worker at http://localhost:8787
make sim                 # http://localhost:8077/ (PORT_SIM=8080 to change)
make sim-dump            # headless screenshots -> tools/sim/out/*.ppm
./scripts/sim.sh 8077
```

The sim is plain HTTP and cannot do TLS, so it talks to the local Worker
only — never to the production `https://` URL in `MORSE_SERVER_URL` (an
`https://` URL in the relay bar reports "sim cannot do TLS"). Test
production with `curl` (see `worker/README.md`).

Tokens come from the environment (`MORSE_TOKEN_DAD` / `MORSE_TOKEN_EMMA`) or
the repo-root `.env`, `--dad-token` / `--emma-token` /
`--relay-url=http://localhost:8787` flags, or the relay bar in the page
(`POST /api/relay`). If both dots stay orange with `auth rejected (401)`,
the sim's tokens do not match the local Worker's `.dev.vars` — paste the
local Worker's tokens into the relay bar and press save.

The web UI shows both framebuffers on `pixelated` canvases, a virtual BOOT
button per device (pointer + `Space` key on the focused device; tap = dot,
hold = dash, hold 1.5 s = action menu), a **send via relay** box (`⇧`) that
POSTs through the real server, a **receive (local)** button (`R`) that queues
an incoming message with a unique `#id` and lights the `N NEW` badge, the
twelve preview scenes, a text setter, and a BMP screenshot button. The inbox
is newest-first; opening a message and moving on (`TAP`/ `NEXT`) or leaving
it (`CLOSE`) removes it, so the badge always counts what is left. Per-device
endpoints live under `/api/dad/*` and `/api/emma/*` (legacy `/api/*` maps to
dad): `GET state`, `GET frame.raw`, `GET frame.bmp`, `POST press`
(`down`/`up`), `POST scene`, `POST text`, `POST receive` (`{"text":"..."}`),
`POST send` (`{"text":"..."}` → relay), `POST wifi` (`{"on":true}`), `GET
messages`, `POST clear`; `GET /api/relay` shows link state and `POST
/api/relay` reconfigures host/port/tokens at runtime.

## Timing and configuration

Everything tunable lives in [`firmware/src/config.h`](firmware/src/config.h):

| Constant | Default | Meaning |
|---|---|---|
| `DOT_MAX_MS` | 220 | release before this → dot, after → dash |
| `DEBOUNCE_MS` | 20 | button debounce |
| `CLEAR_HOLD_MS` | 1500 | hold longer → clear text |
| `LETTER_GAP_MS` | 700 | silence before a letter is committed |
| `WORD_GAP_MS` | 4000 | silence before a space is inserted |
| `BURST_MS` | 240 | node flash-ring duration |
| `RESULT_FLASH_MS` | 350 | new character stays green |
| `PREVIEW_PULSE_MS` | 110 | held-symbol pulse period |
| `PROGRESS_REDRAW_MS` | 40 | countdown-bar refresh period |
| `TEXT_MAX` | 96 | decoded-text buffer size |
| `LCD_BRIGHTNESS` | 160 | backlight PWM (0–255) |
| `DISPLAY_ROTATION` | 2 | 0 / 2 = portrait, flipped 180° |
| `BTN_CLEAR_PIN` | -1 | optional dedicated clear button |

Set `DISPLAY_ROTATION` to `0` if your panel is mounted the other way up. The
firmware sets `invertDisplay(true)` like the reference `esp-status` project; if
your board revision has different colours, that is the first thing to flip.

Tree geometry and label positions are in
[`firmware/src/morse.cpp`](firmware/src/morse.cpp) — each row is
`letter, x, y, labelX, labelY, parent, dotChild, dashChild, shape`.

## Boot log

After flashing, the firmware prints a short banner and every accepted symbol:

```
morse-code v2
reset reason: 1
first frame at 111 ms
input: GPIO0 (BOOT)  clear: RESET/EN button
free heap: 216704
tap = dot, hold = dash, pause = letter, long pause = space
  dot -> E  (len 1)
letter 'E'  text="E"
space
  dot -> E  (len 1)
  dot -> I  (len 2)
letter 'I'  text="E I"
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| No `/dev/ttyUSB0` | Use a **data** USB-C cable; check `dmesg | tail` for `ch341-uart`; install the CH340 driver. |
| `Permission denied` on the port | `sudo usermod -aG uucp $USER` (or `dialout`), then log out/in. |
| Screen stays black | Backlight is GPIO32; check `LCD_BRIGHTNESS`, and that `bringUp()` ran. Try `DISPLAY_ROTATION 0`. |
| Colours look inverted / weird | Flip `invertDisplay(true)` in `firmware/src/main.cpp` or the panel revision differs. |
| Board won't boot after a reset | BOOT may be held: release GPIO0 and press RESET again. |
| Upload can't connect | Hold BOOT, tap RESET, release BOOT, then upload; most boards auto-reset correctly. |
| Text clears when I press the RESET button | Expected: that button is RESET/EN. Use a `BTN_CLEAR_PIN` button for a software-only clear. |
| Long dash clears everything | The old ≥ 1.5 s hold-to-clear is now the action menu (`SEND`/`CLEAR`/`READ`); pick `CLEAR` inside it. Shorten `CLEAR_HOLD_MS`/`ACTION_HOLD_MS` only if you really need slower dashes. |
| Preview fails: `magick: unable to read font` | Install a fontconfig font (`fc-match sans`) or edit `scripts/preview.sh`; the script picks the font file automatically when available. |
| Custom domain returns `302 Found` / `openresty` | The host still points at the old origin. `make worker-deploy` uses `--domain`, which rewrites the proxied DNS record; re-run it and wait for the certificate. |
| `/api/v1/health` → `500 config_error` "must differ" | Deployed tokens are wrong/identical. `make worker-secrets` pushes the distinct values from `.env`; re-check with `curl "$MORSE_SERVER_URL/api/v1/health"`. |
| No Durable Object in the dashboard | Deploy with the repo's `wrangler.toml` (`make worker-deploy`). A dashboard/drag-and-drop upload has no `[[migrations]]`, so no `MorseRelay` namespace is created. |
| Dot stays orange ~20 s after WiFi connects | Old firmware: the first poll used the full 20 s long-poll. Current firmware sends the first poll with `wait=0`, so the dot turns green within a few seconds of WiFi. Re-flash both boards. |
| `QUEUED` → `SENT` takes up to ~20 s | By design on the ESP32: one network task shares the 20 s long-poll (two TLS tasks do not fit next to the 108 KB canvas in 320 KB RAM), so a SEND queued mid-poll waits for that poll. Random 1–20 s is the wait remainder, not a failure. |
| Peer messages never arrive (badge stays empty) | Old firmware advanced `last_seq` to its own send's `seq`, skipping lower-numbered peer messages. Fixed: sends never move `last_seq`; the RX poll advances past them via `latest_seq`. Re-flash **both** boards — `MORSE_PROTO_GEN = 3` resets `last_seq` to 0 once, recovering anything skipped. |
| Inbox empty after a reboot | Fixed: the inbox (up to 8 messages) is now persisted in NVS and restored on boot; new arrivals auto-open when the board is idle in COMPOSE. |

## Repository layout

```
firmware/
  platformio.ini              PlatformIO environment
  build-arduino-cli.sh        compile / flash helper
  src/
    config.h                  pins, timing, brightness
    morse.h / morse.cpp       A–Z tree table and child lookup
    ui.h / ui.cpp             framebuffer-style renderer
    main.cpp                  input state machine + Arduino entry points
docs/
  hardware.md                 board, pins, buttons, timing notes
  messaging-plan.md           original LAN plan (SUPERSEDED in 0.2.0 — history only)
  preview.png                 rendered UI scenes
worker/                       Cloudflare Worker + Durable Object relay
  README.md                   setup, API, free-tier limits, deploy
  MIGRATION.md                cutover checklist + old laptop-relay cleanup
  src/index.js                Worker entry: auth, routing, CORS
  src/morse-do.js             MorseRelay DO: messages, long-poll, ack
  src/store.js                DO SQLite persistence
  tests/                      vitest unit + workerd integration tests
  wrangler.toml               Worker + DO binding (custom domain comes from .env)
scripts/
  init-env.sh                 create .env with random tokens
  gen-secrets.py              generate firmware/src/secrets.h from .env
  worker-deploy.sh            deploy the Worker (custom domain from MORSE_SERVER_URL)
.env.example                  secret template (copy to .env, gitignored)
tools/preview/                host shims + scene renderer
scripts/preview.sh            compile + render + montage
Makefile                      build / upload / monitor / check / preview / sim / worker-*
ui.jpeg                       visual reference
```
