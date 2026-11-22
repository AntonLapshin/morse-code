// Integration test: runs the real Worker + Durable Object (real workerd
// SQLite) through @cloudflare/vitest-plugin. Run with:
//   npm run test:workers
// Test-only bearer tokens come from test-config/wrangler.test.toml (fake values,
// never real secrets).
import { describe, it, expect } from "vitest";
import { SELF } from "cloudflare:test";

const DAD = { Authorization: "Bearer test-token-dad-0000000000000001" };
const EMMA = { Authorization: "Bearer test-token-emma-0000000000000002" };

async function state(headers, params = "") {
  const res = await SELF.fetch(`https://morse.test/api/v1/state${params}`, { headers });
  expect(res.status).toBe(200);
  return res.json();
}

async function send(headers, text, cid, to = "family") {
  const res = await SELF.fetch("https://morse.test/api/v1/messages", {
    method: "POST",
    headers: { ...headers, "Content-Type": "application/json" },
    body: JSON.stringify({ client_msg_id: cid, to, text }),
  });
  return { status: res.status, data: await res.json() };
}

async function poll(headers, params = "") {
  const res = await SELF.fetch(`https://morse.test/api/v1/messages${params}`, { headers });
  expect(res.status).toBe(200);
  return res.json();
}

// Storage is shared across the tests in this file (one DO instance per
// file), so the tests below are ordered to build on a single relay:
// shape, auth, then the full flow (which also covers duplicate ids).
describe("worker + DO integration (real workerd SQLite)", () => {
  it("health, no auth", async () => {
    const res = await SELF.fetch("https://morse.test/api/v1/health");
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ status: "ok", version: "0.2.0" });
  });

  it("state shape and names come from Worker vars", async () => {
    const s = await state(DAD);
    expect(s.latest_seq).toBe(0);
    expect(s.devices.map((d) => d.id)).toEqual(["dad", "emma"]);
    expect(s.devices.find((d) => d.id === "dad").name).toBe("DAD");
    expect(s.devices.find((d) => d.id === "emma").name).toBe("EMMA");
  });

  it("unauthenticated state is 401", async () => {
    const res = await SELF.fetch("https://morse.test/api/v1/state");
    expect(res.status).toBe(401);
    expect((await res.json()).error).toBe("invalid_token");
  });

  it("full flow: HELLO, duplicate id, long-poll wake, ack, direct message", async () => {
    const polling = poll(EMMA, "?since=0&wait=10");
    await new Promise((r) => setTimeout(r, 200));
    const m = await send(DAD, "HELLO", "int-1");
    expect(m.status).toBe(201);
    expect(m.data.seq).toBe(1);
    expect(m.data.from_name).toBe("DAD");
    // Same client_msg_id again: idempotent, current row, no new message.
    const dup = await send(DAD, "HELLO", "int-1");
    expect(dup.status).toBe(200);
    expect(dup.data.duplicate).toBe(true);
    expect(dup.data.seq).toBe(1);
    const woken = await polling;
    expect(woken.messages[0].text).toBe("HELLO");
    expect(woken.unread).toBe(1);

    const acked = await SELF.fetch("https://morse.test/api/v1/ack", {
      method: "POST",
      headers: { ...EMMA, "Content-Type": "application/json" },
      body: JSON.stringify({ upto_seq: 1 }),
    });
    expect(acked.status).toBe(200);
    expect((await acked.json()).unread).toBe(0);

    // Direct message only reaches its recipient.
    const dm = await send(DAD, "PSST", "int-2", "emma");
    expect(dm.status).toBe(201);
    const emma = await poll(EMMA, "?since=1");
    expect(emma.messages.map((x) => x.text)).toEqual(["PSST"]);
    const dad = await poll(DAD, "?since=1");
    expect(dad.messages).toEqual([]);

    const s = await state(DAD);
    expect(s.latest_seq).toBe(2);
    expect(s.devices.every((d) => d.online)).toBe(true);
  });

  it("validation: empty text and unknown recipient are 400", async () => {
    const empty = await send(DAD, "   ", "int-3");
    expect(empty.status).toBe(400);
    expect(empty.data.error).toBe("validation_error");
    const unknown = await send(DAD, "HI", "int-4", "nobody");
    expect(unknown.status).toBe(400);
    expect(unknown.data.error).toBe("unknown_recipient");
  });
});
