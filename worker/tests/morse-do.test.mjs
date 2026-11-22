// Durable Object unit tests over FakeSql. Every case mirrors a case in
// the deleted server/tests/test_api.py (same names where possible), so
// behaviour parity with the FastAPI relay is checkable, not asserted.
// Run with: npm test
import { describe, it, expect } from "vitest";
import { MorseRelay } from "../src/morse-do.js";
import * as store from "../src/store.js";
import { makeRelay, call, send, poll, ack, state } from "./helpers.mjs";

describe("auth at the DO boundary", () => {
  it("requires a device role header", async () => {
    const { relay } = makeRelay();
    const noRole = await relay.fetch(new Request("http://test/api/v1/state"));
    expect(noRole.status).toBe(401);
    const badRole = await call(relay, "/api/v1/state", "web");
    expect(badRole.status).toBe(401);
    expect(badRole.data.error).toBe("invalid_token");
  });
});

describe("send + poll (mirrors test_api.py)", () => {
  it("test_send_and_poll", async () => {
    const { relay } = makeRelay();
    const sent = await send(relay, "dad", "HELLO");
    expect(sent.status).toBe(201);
    expect(sent.data.from).toBe("dad");
    expect(sent.data.from_name).toBe("DAD");
    expect(sent.data.duplicate).toBe(false);
    const seq = sent.data.seq;

    const polled = await poll(relay, "emma", { since: 0, wait: 0 });
    expect(polled.status).toBe(200);
    expect(polled.data.messages.map((m) => m.seq)).toEqual([seq]);
    expect(polled.data.messages[0].text).toBe("HELLO");
    expect(polled.data.unread).toBe(1);
    expect(polled.data.latest_seq).toBe(seq);
    expect(polled.data.peer_online).toBe(true);
  });

  it("test_sender_does_not_receive_own_message", async () => {
    const { relay } = makeRelay();
    await send(relay, "dad", "MINE");
    const polled = await poll(relay, "dad", { since: 0 });
    expect(polled.data.messages).toEqual([]);
  });

  it("test_duplicate_client_msg_id_is_idempotent", async () => {
    const { relay } = makeRelay();
    const first = await send(relay, "dad", "ONCE", "dad-1-7");
    const second = await send(relay, "dad", "ONCE", "dad-1-7");
    expect(first.status).toBe(201);
    expect(second.status).toBe(200);
    expect(second.data.duplicate).toBe(true);
    expect(second.data.seq).toBe(first.data.seq);
  });

  it("test_long_poll_wakes_on_insert", async () => {
    const { relay } = makeRelay();
    const polling = poll(relay, "emma", { since: 0, wait: 5 });
    await new Promise((r) => setTimeout(r, 200));
    await send(relay, "dad", "PING");
    const response = await polling;
    expect(response.status).toBe(200);
    expect(response.data.messages[0].text).toBe("PING");
  });

  it("test_long_poll_times_out_empty", async () => {
    const { relay } = makeRelay();
    const response = await poll(relay, "emma", { since: 0, wait: 0.2 });
    expect(response.status).toBe(200);
    expect(response.data.messages).toEqual([]);
    expect(response.data.truncated).toBe(false);
  });

  it("test_since_skips_old_messages", async () => {
    const { relay } = makeRelay();
    const first = (await send(relay, "dad", "ONE", "dad-1-1")).data;
    const second = (await send(relay, "dad", "TWO", "dad-1-2")).data;
    const response = await poll(relay, "emma", { since: first.seq });
    expect(response.data.messages.map((m) => m.seq)).toEqual([second.seq]);
  });

  it("test_truncation_returns_newest", async () => {
    const { relay } = makeRelay();
    const seqs = [];
    for (let i = 0; i < 5; i++) {
      seqs.push((await send(relay, "dad", `M${i}`, `dad-1-${i}`)).data.seq);
    }
    const response = await poll(relay, "emma", { since: 0, limit: 3 });
    expect(response.data.truncated).toBe(true);
    expect(response.data.messages.map((m) => m.seq)).toEqual(seqs.slice(-3));
    expect(response.data.latest_seq).toBe(seqs[seqs.length - 1]);
  });

  it("test_ack_clears_unread", async () => {
    const { relay } = makeRelay();
    const seq = (await send(relay, "dad", "READ ME")).data.seq;
    const acked = await ack(relay, "emma", seq);
    expect(acked.status).toBe(200);
    expect(acked.data.unread).toBe(0);
    // Older acks must not move last_read_seq backwards.
    const older = await ack(relay, "emma", 0);
    expect(older.data.unread).toBe(0);
  });

  it("test_state_lists_devices_and_presence", async () => {
    const { relay } = makeRelay();
    await poll(relay, "emma", { since: 0 });
    await send(relay, "dad", "HI");
    const s = await state(relay, "dad");
    const byId = Object.fromEntries(s.devices.map((d) => [d.id, d]));
    expect(Object.keys(byId).sort()).toEqual(["dad", "emma"]);
    expect(byId.dad.online).toBe(true);
    expect(byId.emma.online).toBe(true);
    expect(s.latest_seq).toBe(1);
  });

  it("test_validation_and_recipients", async () => {
    const { relay } = makeRelay();
    const empty = await send(relay, "dad", "   ", "dad-1-9");
    expect(empty.status).toBe(400);
    expect(empty.data.error).toBe("validation_error");

    const unknown = await send(relay, "dad", "HI", "dad-1-10", "nobody");
    expect(unknown.status).toBe(400);
    expect(unknown.data.error).toBe("unknown_recipient");

    const toSelf = await send(relay, "dad", "HI", "dad-1-11", "dad");
    expect(toSelf.status).toBe(400);
    expect(toSelf.data.error).toBe("self_recipient");

    const oversized = await send(relay, "dad", "X".repeat(513), "dad-1-12");
    expect(oversized.status).toBe(400);
  });

  it("test_direct_message_only_reaches_recipient", async () => {
    const { relay } = makeRelay();
    await send(relay, "dad", "PSST", "dad-1-dm", "emma");
    const emma = await poll(relay, "emma", { since: 0 });
    expect(emma.data.messages[0].to).toBe("emma");

    await send(relay, "emma", "HI", "e-1", "family");
    const dad = await poll(relay, "dad", { since: 0 });
    expect(dad.data.messages.map((m) => m.text)).toEqual(["HI"]);
  });
});

describe("validation of poll / ack parameters", () => {
  it("rejects out-of-range since, wait, limit and upto_seq", async () => {
    const { relay } = makeRelay();
    expect((await poll(relay, "emma", { since: -1 })).status).toBe(400);
    expect((await poll(relay, "emma", { since: 0, wait: 26 })).status).toBe(400);
    expect((await poll(relay, "emma", { since: 0, wait: -1 })).status).toBe(400);
    expect((await poll(relay, "emma", { since: 0, limit: 0 })).status).toBe(400);
    expect((await poll(relay, "emma", { since: 0, limit: 51 })).status).toBe(400);
    expect((await ack(relay, "emma", -1)).status).toBe(400);
    expect((await call(relay, "/api/v1/nope", "dad")).status).toBe(404);
  });

  it("rejects a non-JSON send body", async () => {
    const { relay } = makeRelay();
    const res = await relay.fetch(
      new Request("http://test/api/v1/messages", {
        method: "POST",
        headers: { "x-morse-role": "dad", "Content-Type": "application/json" },
        body: "{not json",
      }),
    );
    expect(res.status).toBe(400);
  });
});

describe("rate limit", () => {
  it("21st message in a minute is 429", async () => {
    const { relay } = makeRelay();
    for (let i = 0; i < 20; i++) {
      const r = await send(relay, "dad", `M${i}`, `dad-rate-${i}`);
      expect(r.status).toBe(201);
    }
    const limited = await send(relay, "dad", "TOO MANY", "dad-rate-20");
    expect(limited.status).toBe(429);
    expect(limited.data.error).toBe("rate_limited");
    // The other device is unaffected.
    expect((await send(relay, "emma", "OK", "emma-rate-0")).status).toBe(201);
  });
});

describe("names and retention", () => {
  it("device names come from Worker vars, defaulting to DAD / EMMA", async () => {
    const { relay } = makeRelay({ MORSE_DAD_NAME: "Papa", MORSE_EMMA_NAME: "Em" });
    const sent = await send(relay, "dad", "HI");
    expect(sent.data.from_name).toBe("Papa");
    const s = await state(relay, "dad");
    expect(s.devices.find((d) => d.id === "emma").name).toBe("Em");
  });

  it("alarm prunes to the newest retention and re-arms", async () => {
    const { relay, sql, alarms } = makeRelay();
    for (let i = 0; i < 5; i++) {
      await send(relay, "dad", `M${i}`, `dad-prune-${i}`);
    }
    expect(store.latestSeq(sql)).toBe(5);
    store.prune(sql, 2);
    expect(store.latestSeq(sql)).toBe(5);
    expect(store.fetchAfter(sql, "emma", 0, 50).rows.map((m) => m.seq)).toEqual([4, 5]);
    const before = alarms.length;
    await relay.alarm();
    expect(alarms.length).toBeGreaterThan(before);
    // alarm() itself prunes to the full retention (1000), so nothing is lost.
    expect(store.latestSeq(sql)).toBe(5);
  });

  it("a fresh DO starts at seq 0 — the proto_gen reset target", async () => {
    // The firmware's proto_gen=2 reset (see firmware/src/net.cpp) exists
    // because of this: a brand-new DO has latest_seq 0, so a board that
    // kept polling since=42 would never see seq 1..42. The Worker side
    // of the contract is simply that a fresh instance starts at 0.
    const { relay } = makeRelay();
    const s = await state(relay, "dad");
    expect(s.latest_seq).toBe(0);
    expect(s.unread).toBe(0);
  });
});

describe("store unit tests", () => {
  it("insertMessage duplicate returns the stored row", () => {
    const { sql } = makeRelay();
    store.ensureSchema(sql);
    store.syncDevices(sql, [["dad", "DAD"], ["emma", "EMMA"]]);
    const first = store.insertMessage(sql, {
      fromDevice: "dad",
      clientMsgId: "x-1",
      toDevice: "family",
      text: "HI",
      createdAt: 1000,
    });
    expect(first.duplicate).toBe(false);
    expect(first.row.seq).toBe(1);
    const second = store.insertMessage(sql, {
      fromDevice: "dad",
      clientMsgId: "x-1",
      toDevice: "family",
      text: "HI",
      createdAt: 1001,
    });
    expect(second.duplicate).toBe(true);
    expect(second.row.seq).toBe(1);
    expect(store.latestSeq(sql)).toBe(1);
  });
});
