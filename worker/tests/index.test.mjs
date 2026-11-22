// Worker entry tests: config validation + Bearer auth + routing to the DO stub.
// The DO itself is stubbed here; its behaviour is covered in
// tests/morse-do.test.mjs and tests/integration.test.mjs.
import { describe, it, expect } from "vitest";
import worker, { validateConfig, resolveDevice } from "../src/index.js";

const DAD = "test-token-dad-0000000000000001";
const EMMA = "test-token-emma-0000000000000002";
const GOOD_ENV = { MORSE_TOKEN_DAD: DAD, MORSE_TOKEN_EMMA: EMMA };

function stubEnv(overrides, doBody = { ok: true }, doStatus = 200) {
  return {
    ...GOOD_ENV,
    MORSE_RELAY: {
      idFromName: (name) => `id:${name}`,
      get: () => ({
        fetch: async (req) =>
          new Response(JSON.stringify({ ...doBody, device: req.headers.get("x-morse-role") }), {
            status: doStatus,
            headers: { "Content-Type": "application/json" },
          }),
      }),
    },
    ...(overrides || {}),
  };
}

describe("validateConfig", () => {
  it("missing tokens refused", () => {
    expect(validateConfig({})).toMatch(/MORSE_TOKEN_DAD/);
    expect(validateConfig({ MORSE_TOKEN_DAD: DAD })).toMatch(/MORSE_TOKEN_EMMA/);
  });
  it("equal tokens refused", () => {
    expect(validateConfig({ MORSE_TOKEN_DAD: "a".repeat(32), MORSE_TOKEN_EMMA: "a".repeat(32) }))
      .toMatch(/differ/);
  });
  it("short token refused", () => {
    expect(validateConfig({ MORSE_TOKEN_DAD: "short", MORSE_TOKEN_EMMA: "b".repeat(32) }))
      .toMatch(/at least 16/);
  });
  it("REPLACE / CHANGEME placeholders refused", () => {
    expect(validateConfig({
      MORSE_TOKEN_DAD: "REPLACE_WITH_openssl_rand_hex_24",
      MORSE_TOKEN_EMMA: EMMA,
    })).toMatch(/placeholder/);
    expect(validateConfig({
      MORSE_TOKEN_DAD: "CHANGEME-dad-token-at-least-16-chars-long-000001",
      MORSE_TOKEN_EMMA: EMMA,
    })).toMatch(/placeholder/);
  });
  it("good config passes", () => {
    expect(validateConfig(GOOD_ENV)).toBe(null);
  });
});

describe("resolveDevice", () => {
  it("maps each token to its device", () => {
    expect(resolveDevice(`Bearer ${DAD}`, GOOD_ENV)).toBe("dad");
    expect(resolveDevice(`Bearer ${EMMA}`, GOOD_ENV)).toBe("emma");
  });
  it("rejects missing, malformed and wrong tokens", () => {
    expect(resolveDevice(null, GOOD_ENV)).toBe(null);
    expect(resolveDevice("Basic abc", GOOD_ENV)).toBe(null);
    expect(resolveDevice("Bearer nope-nope-nope-nope", GOOD_ENV)).toBe(null);
  });
});

describe("worker fetch", () => {
  it("health needs no auth and returns ok", async () => {
    const res = await worker.fetch(new Request("http://test/api/v1/health"), GOOD_ENV, {});
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ status: "ok", version: "0.2.0" });
  });

  it("health with a broken config is 500 config_error", async () => {
    const res = await worker.fetch(new Request("http://test/api/v1/health"), {}, {});
    expect(res.status).toBe(500);
    expect((await res.json()).error).toBe("config_error");
  });

  it("state without auth is 401", async () => {
    const res = await worker.fetch(
      new Request("http://test/api/v1/state"), stubEnv(), {},
    );
    expect(res.status).toBe(401);
    expect((await res.json()).error).toBe("invalid_token");
  });

  it("state with a bad token is 401", async () => {
    const res = await worker.fetch(
      new Request("http://test/api/v1/state", {
        headers: { Authorization: "Bearer nope-nope-nope-nope" },
      }),
      stubEnv(), {},
    );
    expect(res.status).toBe(401);
  });

  it("state with a good token reaches the DO with the device header", async () => {
    const res = await worker.fetch(
      new Request("http://test/api/v1/state", {
        headers: { Authorization: `Bearer ${DAD}` },
      }),
      stubEnv(), {},
    );
    expect(res.status).toBe(200);
    expect(await res.json()).toMatchObject({ ok: true, device: "dad" });
  });

  it("broken config fails API requests with 500, before auth", async () => {
    const res = await worker.fetch(
      new Request("http://test/api/v1/state", {
        headers: { Authorization: `Bearer ${DAD}` },
      }),
      stubEnv({ MORSE_TOKEN_DAD: "", MORSE_TOKEN_EMMA: "" }), {},
    );
    expect(res.status).toBe(500);
    expect((await res.json()).error).toBe("config_error");
  });

  it("unknown path is 404", async () => {
    const res = await worker.fetch(new Request("http://test/nope"), stubEnv(), {});
    expect(res.status).toBe(404);
  });

  it("OPTIONS preflight answers 204 for an allowed origin", async () => {
    const res = await worker.fetch(
      new Request("http://test/api/v1/state", {
        method: "OPTIONS",
        headers: { Origin: "http://localhost:8077" },
      }),
      stubEnv(), {},
    );
    expect(res.status).toBe(204);
    expect(res.headers.get("Access-Control-Allow-Origin")).toBe("http://localhost:8077");
  });

  it("OPTIONS preflight is 204 without CORS headers for a disallowed origin", async () => {
    const res = await worker.fetch(
      new Request("http://test/api/v1/state", {
        method: "OPTIONS",
        headers: { Origin: "https://example.com" },
      }),
      stubEnv(), {},
    );
    expect(res.status).toBe(204);
    expect(res.headers.get("Access-Control-Allow-Origin")).toBeNull();
  });

  it("MORSE_CORS_ORIGINS (comma-separated) adds allowed origins", async () => {
    const res = await worker.fetch(
      new Request("http://test/api/v1/state", {
        method: "OPTIONS",
        headers: { Origin: "https://example.com" },
      }),
      stubEnv({ MORSE_CORS_ORIGINS: "https://example.com, https://other.example" }), {},
    );
    expect(res.status).toBe(204);
    expect(res.headers.get("Access-Control-Allow-Origin")).toBe("https://example.com");
  });
});
