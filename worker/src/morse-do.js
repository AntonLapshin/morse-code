// MorseRelay Durable Object — the single source of truth for messages.
// Cloudflare Durable Object with SQLite storage (ctx.storage.sql).
//
// This is a direct port of the deleted FastAPI relay (server/app.py +
// server/db.py): same wire format, same validation, same routing,
// same presence window, same retention. The only intentional changes:
//   - /health is answered by the Worker entry (status + version only);
//     latest_seq lives on authenticated GET /api/v1/state instead.
//   - Long-poll waiters are a per-DO list of resolvers + setTimeout.
//     Do NOT use blockConcurrencyWhile for the wait (30 s timeout + it
//     blocks the DO). A pending long-poll keeps the DO non-hibernatable,
//     so it bills duration while a device page is open — expected; see
//     worker/README.md for the free-tier numbers.
//   - Retention pruning runs on a DO alarm (hourly), not on every request.
import * as store from "./store.js";

export const VERSION = "0.2.0";
const ONLINE_WINDOW_S = 45;
const MAX_TEXT_BYTES = 512;
const MAX_CLIENT_MSG_ID = 64;
const SEND_RATE_LIMIT = 20;
const SEND_RATE_WINDOW_S = 60;
const RETENTION = 1000;
const PRUNE_INTERVAL_S = 3600;
const FAMILY = "family";

const DEVICE_IDS = ["dad", "emma"];

function jsonResponse(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function textByteLength(s) {
  return new TextEncoder().encode(s).length;
}

export class MorseRelay {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env || {};
    this.sql = ctx.storage.sql;
    this.waiters = []; // { since, resolve, timer }
    this.limiter = new Map(); // device id -> [epoch seconds of sends]
    this.loaded = false;
  }

  // -- config -------------------------------------------------------------
  nameFor(deviceId) {
    const name =
      deviceId === "dad" ? this.env.MORSE_DAD_NAME : this.env.MORSE_EMMA_NAME;
    return name || deviceId.toUpperCase();
  }

  // -- persistence --------------------------------------------------------
  ensureLoaded(now) {
    if (this.loaded) return;
    store.ensureSchema(this.sql);
    store.syncDevices(
      this.sql,
      DEVICE_IDS.map((id) => [id, this.nameFor(id)]),
    );
    this.loaded = true;
    this.armPruneAlarm(now);
  }

  armPruneAlarm(now) {
    try {
      const setAlarm = this.ctx && this.ctx.storage && this.ctx.storage.setAlarm;
      if (typeof setAlarm === "function") {
        setAlarm.call(this.ctx.storage, Math.floor((now + PRUNE_INTERVAL_S) * 1000));
      }
    } catch (_) {
      // Alarms are best-effort: pruning also happens lazily in alarm()
      // only, so a missed alarm just delays retention, it loses nothing.
    }
  }

  async alarm() {
    const now = Date.now() / 1000;
    this.ensureLoaded(now);
    store.prune(this.sql, RETENTION);
    this.armPruneAlarm(now);
  }

  // -- long-poll waiters ----------------------------------------------------
  wakeWaiters(newSeq) {
    const ready = this.waiters.filter((w) => w.since < newSeq);
    this.waiters = this.waiters.filter((w) => w.since >= newSeq);
    for (const w of ready) {
      clearTimeout(w.timer);
      w.resolve(true);
    }
  }

  waitForChange(since, waitMs) {
    if (store.latestSeq(this.sql) > since) return Promise.resolve(true);
    return new Promise((resolve) => {
      const entry = { since, resolve, timer: null };
      entry.timer = setTimeout(() => {
        this.waiters = this.waiters.filter((w) => w !== entry);
        resolve(false);
      }, waitMs);
      this.waiters.push(entry);
    });
  }

  // -- rate limit -----------------------------------------------------------
  allowSend(deviceId, now) {
    const events = this.limiter.get(deviceId) || [];
    const cutoff = now - SEND_RATE_WINDOW_S;
    const kept = events.filter((t) => t > cutoff);
    if (kept.length >= SEND_RATE_LIMIT) {
      this.limiter.set(deviceId, kept);
      return false;
    }
    kept.push(now);
    this.limiter.set(deviceId, kept);
    return true;
  }

  // -- helpers --------------------------------------------------------------
  err(error, detail, status) {
    return jsonResponse({ error, detail }, status);
  }

  messageView(row, names, now) {
    return {
      seq: Number(row.seq),
      from: row.from_device,
      from_name: names[row.from_device] || String(row.from_device).toUpperCase(),
      to: row.to_device,
      text: row.text,
      ts: Math.floor(Number(row.created_at)),
      age_s: Math.max(0, Math.floor(now - Number(row.created_at))),
    };
  }

  names() {
    const out = {};
    for (const id of DEVICE_IDS) out[id] = this.nameFor(id);
    return out;
  }

  // -- request routing -------------------------------------------------------
  async fetch(request) {
    const url = new URL(request.url);
    const role = request.headers.get("x-morse-role");
    if (role !== "dad" && role !== "emma") {
      return this.err("invalid_token", "missing or invalid bearer token", 401);
    }
    const now = Date.now() / 1000;
    this.ensureLoaded(now);
    // Every authenticated request proves the device is alive, as the
    // FastAPI auth dependency did (it touched last_seen on every call).
    store.touch(this.sql, role, now);
    if (url.pathname === "/api/v1/messages" && request.method === "POST") {
      return this.handleSend(request, role, now);
    }
    if (url.pathname === "/api/v1/messages" && request.method === "GET") {
      return this.handlePoll(url, role, now);
    }
    if (url.pathname === "/api/v1/ack" && request.method === "POST") {
      return this.handleAck(request, role);
    }
    if (url.pathname === "/api/v1/state" && request.method === "GET") {
      return this.handleState(role, now);
    }
    return this.err("not_found", "unknown endpoint", 404);
  }

  async handleSend(request, device, now) {
    let body;
    try {
      body = await request.json();
    } catch (_) {
      return this.err("validation_error", "request body must be JSON", 400);
    }
    if (!body || typeof body !== "object") {
      return this.err("validation_error", "request body must be a JSON object", 400);
    }
    const clientMsgId = typeof body.client_msg_id === "string" ? body.client_msg_id.trim() : "";
    if (
      !clientMsgId ||
      clientMsgId.length > MAX_CLIENT_MSG_ID ||
      ![...clientMsgId].every((ch) => ch.charCodeAt(0) >= 0x21 && ch.charCodeAt(0) <= 0x7e)
    ) {
      return this.err(
        "validation_error",
        "client_msg_id: must not be empty; must be printable ASCII without spaces",
        400,
      );
    }
    const to = body.to === undefined || body.to === null ? FAMILY : String(body.to).trim().toLowerCase();
    const text = typeof body.text === "string" ? body.text.trim() : "";
    if (!text) {
      return this.err("validation_error", "text: must not be empty", 400);
    }
    if (textByteLength(text) > MAX_TEXT_BYTES) {
      return this.err("validation_error", `text: must be at most ${MAX_TEXT_BYTES} bytes`, 400);
    }

    if (!this.allowSend(device, now)) {
      return this.err(
        "rate_limited",
        `at most ${SEND_RATE_LIMIT} messages per minute`,
        429,
      );
    }
    if (to !== FAMILY && !DEVICE_IDS.includes(to)) {
      return this.err(
        "unknown_recipient",
        `to must be 'family' or one of ${[...DEVICE_IDS].sort().join(", ")}`,
        400,
      );
    }
    if (to === device) {
      return this.err("self_recipient", "cannot send to yourself", 400);
    }

    const { row, duplicate } = store.insertMessage(this.sql, {
      fromDevice: device,
      clientMsgId,
      toDevice: to,
      text,
      createdAt: now,
    });
    if (!duplicate) this.wakeWaiters(Number(row.seq));
    return jsonResponse(
      {
        seq: Number(row.seq),
        from: row.from_device,
        from_name: this.nameFor(device),
        to: row.to_device,
        text: row.text,
        ts: Math.floor(Number(row.created_at)),
        duplicate,
      },
      duplicate ? 200 : 201,
    );
  }

  async handlePoll(url, device, now) {
    const sinceRaw = url.searchParams.get("since");
    const since = sinceRaw === null || sinceRaw === "" ? 0 : Number(sinceRaw);
    if (!Number.isInteger(since) || since < 0) {
      return this.err("validation_error", "since: must be an integer >= 0", 400);
    }
    const waitRaw = url.searchParams.get("wait");
    const wait = waitRaw === null || waitRaw === "" ? 0 : Number(waitRaw);
    if (!Number.isFinite(wait) || wait < 0 || wait > 25) {
      return this.err("validation_error", "wait: must be between 0 and 25", 400);
    }
    const limitRaw = url.searchParams.get("limit");
    const limit = limitRaw === null || limitRaw === "" ? 20 : Number(limitRaw);
    if (!Number.isInteger(limit) || limit < 1 || limit > 50) {
      return this.err("validation_error", "limit: must be an integer between 1 and 50", 400);
    }

    const deadline = Date.now() + wait * 1000;
    let fetched = { rows: [], truncated: false };
    // Same no-lost-wakeup shape as the FastAPI notifier: query, then
    // block only if empty, and re-query after every wake/timeout.
    // `seen` is captured BEFORE each fetch: an insert between the snapshot
    // and the fetch is in the fetch result, and an insert after it makes
    // the waiter's immediate check (latest > seen) fire at once — so no
    // wakeup is ever lost. (Passing the caller's `since` here would be
    // wrong: the device's own sends make latest > since with an empty
    // fetch, which would spin the loop until the deadline.)
    // A wait that times out (no insert) cannot have changed anything, so
    // re-querying after it would return the same empty result — wait again
    // instead. Only a wake (waitForChange true) re-queries, and the
    // in-loop touch is dropped too: fetch() already touched at request
    // start and the post-loop touch covers the 45 s presence window for a
    // poll that lasts at most 25 s.
    let changed = true; // first pass must query
    let seen = 0;
    for (;;) {
      if (changed) {
        seen = store.latestSeq(this.sql);
        fetched = store.fetchAfter(this.sql, device, since, limit);
        if (fetched.rows.length > 0 || wait <= 0) break;
      }
      const remaining = deadline - Date.now();
      if (remaining <= 0) break;
      changed = await this.waitForChange(seen, Math.min(remaining, 5000));
    }

    store.touch(this.sql, device, Date.now() / 1000);
    const states = store.deviceStates(this.sql);
    const now2 = Date.now() / 1000;
    const names = this.names();
    const peerOnline = states.some(
      (s) => s.id !== device && now2 - s.last_seen < ONLINE_WINDOW_S,
    );
    return jsonResponse({
      messages: fetched.rows.map((row) => this.messageView(row, names, now2)),
      latest_seq: store.latestSeq(this.sql),
      truncated: fetched.truncated,
      unread: store.unread(this.sql, device),
      peer_online: peerOnline,
      server_time: Math.floor(now2),
    });
  }

  async handleAck(request, device) {
    let body;
    try {
      body = await request.json();
    } catch (_) {
      return this.err("validation_error", "request body must be JSON", 400);
    }
    const upto = body && typeof body.upto_seq === "number" ? body.upto_seq : NaN;
    if (!Number.isInteger(upto) || upto < 0) {
      return this.err("validation_error", "upto_seq: must be an integer >= 0", 400);
    }
    const unread = store.ack(this.sql, device, upto);
    return jsonResponse({ ok: true, unread, upto_seq: upto });
  }

  handleState(device, now) {
    const rows = store.deviceStates(this.sql);
    return jsonResponse({
      devices: rows.map((row) => ({
        id: row.id,
        name: row.name,
        online: now - row.last_seen < ONLINE_WINDOW_S,
        last_seen_s: row.last_seen ? Math.max(0, Math.floor(now - row.last_seen)) : null,
      })),
      unread: store.unread(this.sql, device),
      latest_seq: store.latestSeq(this.sql),
      server_time: Math.floor(now),
    });
  }
}
