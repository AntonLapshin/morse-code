// Morse-code Worker entry: auth + routing to the MorseRelay Durable Object.
// /health is answered by the Worker directly; everything else goes to the DO.
//
// Tokens come from Worker secrets (MORSE_TOKEN_DAD / MORSE_TOKEN_EMMA) or
// .dev.vars for local dev. Missing, equal, or <16-char tokens — or the
// REPLACE/CHANGEME placeholders — fail EVERY /api/v1/* request with 500
// config_error (there is no deploy-time startup to refuse in, so the
// failure surfaces on first request instead). Names are Worker vars
// MORSE_DAD_NAME / MORSE_EMMA_NAME (defaults DAD / EMMA).
//
// Error bodies keep the old FastAPI relay's shape ({error, detail}), not
// chess-watch's {code, message}, so the firmware and sim — which only ever
// checked status codes and these error names — need no change.
import { MorseRelay, VERSION } from "./morse-do.js";

export { MorseRelay };

const MIN_TOKEN_LEN = 16;
// CORS allow-list for the browser sim only (the ESP32 does not use
// CORS). Default is the sim dev server; set MORSE_CORS_ORIGINS as a
// Worker var / secret / .dev.vars entry, comma-separated, to allow more
// origins (e.g. the deployed Worker URL for `make sim` against it).
function corsOrigins(env) {
  const extra = (env.MORSE_CORS_ORIGINS || "")
    .split(",")
    .map((o) => o.trim())
    .filter(Boolean);
  return new Set(["http://localhost:8077", ...extra]);
}

function jsonResponse(body, status = 200, extraHeaders) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", ...(extraHeaders || {}) },
  });
}

/** Constant-time string compare (no crypto.subtle needed). */
function safeEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string") return false;
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

export function validateConfig(env) {
  const dad = env.MORSE_TOKEN_DAD || "";
  const emma = env.MORSE_TOKEN_EMMA || "";
  if (!dad) return "missing MORSE_TOKEN_DAD: run `make worker-secrets`";
  if (!emma) return "missing MORSE_TOKEN_EMMA: run `make worker-secrets`";
  if (dad === emma) return "MORSE_TOKEN_DAD and MORSE_TOKEN_EMMA must differ";
  if (dad.length < MIN_TOKEN_LEN) return `MORSE_TOKEN_DAD must be at least ${MIN_TOKEN_LEN} characters`;
  if (emma.length < MIN_TOKEN_LEN) return `MORSE_TOKEN_EMMA must be at least ${MIN_TOKEN_LEN} characters`;
  if (
    dad.toUpperCase().startsWith("REPLACE") ||
    dad.toUpperCase().startsWith("CHANGEME") ||
    emma.toUpperCase().startsWith("REPLACE") ||
    emma.toUpperCase().startsWith("CHANGEME")
  ) {
    return "MORSE_TOKEN_DAD/MORSE_TOKEN_EMMA are still the placeholders; set real secrets";
  }
  return null;
}

export function resolveDevice(authorization, env) {
  if (!authorization) return null;
  const parts = authorization.split(/\s+/, 2);
  if (parts.length !== 2 || parts[0].toLowerCase() !== "bearer") return null;
  const token = parts[1].trim();
  // Compare against both tokens every time so timing leaks nothing.
  const dad = safeEqual(token, env.MORSE_TOKEN_DAD || "");
  const emma = safeEqual(token, env.MORSE_TOKEN_EMMA || "");
  if (dad) return "dad";
  if (emma) return "emma";
  return null;
}

function corsHeaders(request, env) {
  const origin = request.headers.get("Origin");
  if (origin && corsOrigins(env).has(origin)) {
    return {
      "Access-Control-Allow-Origin": origin,
      "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
      "Access-Control-Allow-Headers": "Authorization, Content-Type",
    };
  }
  return {};
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const cors = corsHeaders(request, env);
    if (request.method === "OPTIONS" && url.pathname.startsWith("/api/v1/")) {
      return new Response(null, { status: 204, headers: cors });
    }
    if (url.pathname === "/api/v1/health" || url.pathname === "/health") {
      if (request.method !== "GET") {
        return jsonResponse({ error: "method_not_allowed", detail: "GET only" }, 405, cors);
      }
      const configError = validateConfig(env);
      if (configError) {
        return jsonResponse({ error: "config_error", detail: configError }, 500, cors);
      }
      return jsonResponse({ status: "ok", version: VERSION }, 200, cors);
    }
    if (!url.pathname.startsWith("/api/v1/")) {
      return jsonResponse({ error: "not_found", detail: "unknown endpoint" }, 404, cors);
    }
    const configError = validateConfig(env);
    if (configError) {
      return jsonResponse({ error: "config_error", detail: configError }, 500, cors);
    }
    const device = resolveDevice(request.headers.get("Authorization"), env);
    if (device === null) {
      return jsonResponse(
        { error: "invalid_token", detail: "missing or invalid bearer token" },
        401,
        cors,
      );
    }
    const id = env.MORSE_RELAY.idFromName("family");
    const stub = env.MORSE_RELAY.get(id);
    const headers = new Headers(request.headers);
    headers.set("x-morse-role", device);
    const forwarded = new Request(request, { headers });
    const response = await stub.fetch(forwarded);
    // Pass CORS through on DO responses.
    if (Object.keys(cors).length > 0) {
      const out = new Headers(response.headers);
      for (const [k, v] of Object.entries(cors)) out.set(k, v);
      return new Response(response.body, { status: response.status, headers: out });
    }
    return response;
  },
};
