# Migration: laptop relay → Cloudflare Worker + DO (0.2.0)

This is the cutover checklist for moving from the old LAN relay
(FastAPI + SQLite on the Omarchy laptop, port 8180) to the Cloudflare
Worker + Durable Object at `https://<your-domain>`.
The laptop relay is already deleted from this repo (`server/` is gone);
this file is about cleaning up the laptop itself and re-flashing the
boards. Do the steps in order — in particular, verify on the
`workers.dev` URL **before** attaching the custom domain, and
finish or abandon any pending LAN exchange first: **message history
does NOT migrate**. The DO starts with an empty database and `seq`
restarts at 1.

## 0. Before you start

- Finish or abandon any in-progress LAN exchange. Anything unread on
  a board, or queued in an outbox, will not appear on the Worker.
- Confirm the Cloudflare zone for the custom domain (the host in
  `MORSE_SERVER_URL`) is DNS-managed by Cloudflare in the same account as
  the Worker. `wrangler deploy --domain` needs that zone to create the
  proxied DNS record and certificate.

## 1. Deploy the Worker and verify it

Follow the one flow in [`worker/README.md`](README.md#setup) — setup,
local test, `make worker-secrets`, `make worker-deploy` — then verify on
the `workers.dev` URL **first**, then on the custom domain, and send one
test message as dad and poll it as emma. The commands are the smoke test
in `worker/README.md` ("Local dev" and "Deploy + cutover"), run against
the deployed URLs instead of `localhost:8787`: the health check must
answer `{"status":"ok","version":"0.2.0"}` on both URLs, and the emma
poll must return the HELLO message with `latest_seq` 1.

If any of that fails, stop here — do not touch the laptop relay yet.

## 2. Re-flash both boards (required)

The firmware now talks HTTPS to `MORSE_SERVER_URL` and polls with
`wait=5`. More importantly, the DO's `seq` restarts at 1, so a board
that kept its LAN-era `last_seq` (e.g. 42) would poll `since=42` and
ignore every new message forever. The firmware handles this with a
`proto_gen` NVS key (`MORSE_PROTO_GEN = 3` in `firmware/src/config.h`):
on first boot after the re-flash it sees the generation mismatch and
resets `last_seq` to 0. That reset only happens if you actually
re-flash — a board still running the LAN firmware keeps talking to a
server that no longer exists.

```bash
./scripts/gen-secrets.py dad --force    # writes firmware/src/secrets.h
make upload-dad                          # or: make upload DEVICE=dad
./scripts/gen-secrets.py emma --force
make upload-emma
make monitor PORT=/dev/ttyUSB0          # expect: net: device=dad server=https://<your-domain> ... proto_gen=3
```

Check the boot log on each board: the link dot should go green, and a
test SEND from one board should appear on the other within ~5 s
(the long-poll wait; usually much faster — the DO wakes the poll the
moment a message lands).

## 3. Stop the laptop relay

On the Omarchy laptop, in the repo checkout:

```bash
# If the systemd user unit was enabled:
systemctl --user stop --now morse-relay
systemctl --user disable morse-relay
rm -f ~/.config/systemd/user/morse-relay.service
systemctl --user daemon-reload
systemctl --user status morse-relay   # expect: not found / inactive
```

If the relay was only ever run with `make start` (background uvicorn,
no systemd unit), stop that instead:

```bash
# In the OLD checkout (before this patch) — server/ is gone now, so if
# you already applied the patch, find the pid by port instead:
ss -ltnp | grep 8180
kill <pid>   # the uvicorn process on port 8180; verify it is gone with ss
```

## 4. Delete the laptop leftovers

```bash
rm -rf server/.venv server/data   # the SQLite history — it does NOT migrate
# (server/ itself is already gone from the repo in 0.2.0.)
```

If you added the lid-switch drop-in so the laptop would stay awake as
a relay, remove it — the laptop no longer needs to stay awake for
anyone:

```bash
sudo rm -f /etc/systemd/logind.conf.d/90-morse-relay.conf
sudo systemctl restart systemd-logind
```

The DHCP reservation for `192.168.4.44` can stay or go: nothing in
0.2.0 uses it. Remove it from the router only if it existed solely
for the relay.

## 5. Close the firewall port (Omarchy / ufw)

The relay used to listen on TCP 8180, allowed from the LAN only.
Close it:

```bash
sudo ufw status numbered
# find the rule that allows 8180/tcp (it was added as e.g.
#   sudo ufw allow from 192.168.4.0/22 to any port 8180 proto tcp)
sudo ufw delete <n>     # the number from the listing above, e.g. sudo ufw delete 3
sudo ufw status         # confirm no 8180 rule remains
ss -ltnp | grep 8180    # expect: nothing listening on 8180
```

If the laptop uses `firewalld` or nftables directly instead of `ufw`,
remove the equivalent 8180 allow rule there. No port needs to be open
for the Worker setup — the boards make outbound HTTPS connections,
and Cloudflare terminates them.

## 6. Verify the cutover

- [ ] `curl https://<your-domain>/api/v1/health` →
      `{"status":"ok","version":"0.2.0"}`
- [ ] Both boards show a green link dot (not orange/red).
- [ ] Dad SEND → appears on Emma; Emma SEND → appears on Dad.
- [ ] `sudo ufw status` shows no 8180 rule; `ss -ltnp | grep 8180`
      is empty; `systemctl --user status morse-relay` shows no unit.
- [ ] The laptop can sleep / suspend / leave the house: messages keep
      flowing, because no laptop is involved any more.

## What changed, in one paragraph

The laptop is out of the loop entirely. Messages live in a Cloudflare
Durable Object (`MorseRelay`, id `"family"`) with the same API, auth,
routing, presence, and retention as the old relay; the ESP32s reach it
over HTTPS from any internet connection, so the boards work at
Emma's place as well as at home, with no laptop running, no open port,
and no DHCP reservation to maintain. The price is the Workers Free
limits (see `worker/README.md`: requests are fine, DO duration while
polling is the one to watch) and the firmware's `setInsecure()` TLS
trade-off in 0.2.0 (see `firmware/src/net.cpp` — pin the CA later).

## Future: the ntfy / phone-watch bridge (Phase 4)

The old plan's Phase 4 (mirror every family message to ntfy for phone
push → Zepp watch) was server-side code that died with `server/`.
Nothing about it is implemented in 0.2.0. If it is wanted later, it
belongs on the Worker: the DO can publish to an ntfy topic on insert
(and a second Worker route can accept inbound phone replies). That is
a separate patch, not part of this cutover.
