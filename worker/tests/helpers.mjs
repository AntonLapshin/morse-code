// Shared helpers for the Durable Object unit tests: build a fresh
// MorseRelay over FakeSql and drive it through fetch().
import { MorseRelay } from "../src/morse-do.js";
import { fakeCtx } from "./fake-sql.mjs";

export const DAD_TOKEN = "test-token-dad-0000000000000001";
export const EMMA_TOKEN = "test-token-emma-0000000000000002";

export function makeRelay(envOverrides) {
  const { ctx, sql, alarms } = fakeCtx();
  const env = {
    MORSE_DAD_NAME: "DAD",
    MORSE_EMMA_NAME: "EMMA",
    ...(envOverrides || {}),
  };
  const relay = new MorseRelay(ctx, env);
  return { relay, sql, alarms };
}

function req(path, device, { method = "GET", body } = {}) {
  return new Request(`http://test${path}`, {
    method,
    headers: {
      "x-morse-role": device,
      ...(body ? { "Content-Type": "application/json" } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
  });
}

export async function call(relay, path, device, opts) {
  const res = await relay.fetch(req(path, device, opts));
  const data = await res.json().catch(() => null);
  return { status: res.status, data };
}

export async function send(relay, device, text, cid = `${device}-1-1`, to = "family") {
  return call(relay, "/api/v1/messages", device, {
    method: "POST",
    body: { client_msg_id: cid, to, text },
  });
}

export async function poll(relay, device, params = {}) {
  const qs = new URLSearchParams(params).toString();
  return call(relay, `/api/v1/messages${qs ? `?${qs}` : ""}`, device);
}

export async function ack(relay, device, uptoSeq) {
  return call(relay, "/api/v1/ack", device, {
    method: "POST",
    body: { upto_seq: uptoSeq },
  });
}

export async function state(relay, device) {
  const r = await call(relay, "/api/v1/state", device);
  if (r.status !== 200) throw new Error(`state failed: ${r.status} ${JSON.stringify(r.data)}`);
  return r.data;
}
