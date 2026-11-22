#!/usr/bin/env bash
# Deploy the Morse-code Worker, attaching the Workers Custom Domain derived
# from MORSE_SERVER_URL in the repo-root .env — so the hostname lives in
# exactly one place (that .env value) and never in wrangler.toml or docs.
#
#   MORSE_SERVER_URL=https://<your-host>/  -> npx wrangler deploy --domain "<your-host>"
#   MORSE_SERVER_URL unset / localhost / *.workers.dev
#                                          -> plain deploy (workers.dev URL only)
#
# Why --domain and not --route: a Worker route only intercepts requests that
# already resolve to Cloudflare for that hostname. If the DNS record still
# points at another origin (a website builder, an old VPS, ...), the browser
# gets that origin's response (often a 302) and the Worker is never invoked.
# --domain creates/updates the proxied DNS record and the certificate itself,
# so a fresh checkout deploys to a working custom domain with no dashboard
# DNS step. The Durable Object namespace is created by the [[migrations]]
# entry in wrangler.toml on the first deploy that includes it.
#
# Device names come from .env too (MORSE_DAD_NAME / MORSE_EMMA_NAME) and are
# passed as --var on EVERY deploy (both branches below): the wrangler.toml
# [vars] defaults (DAD/EMMA) are only a fallback, and without --var a deploy
# would silently revert the names to them. Names are single words; a name
# containing ":" would confuse the KEY:VALUE split.
#
# Pass --dry-run to validate the deploy (bundling, bindings, domain) without
# deploying or needing a login: scripts/worker-deploy.sh --dry-run
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/worker"

if [ -f "$ROOT/.env" ]; then
  # shellcheck disable=SC1091
  set -a; . "$ROOT/.env"; set +a
fi

URL="${MORSE_SERVER_URL:-}"
HOST=""
if [[ "$URL" =~ ^https://([^/:]+) ]]; then
  HOST="${BASH_REMATCH[1]}"
fi

NAME_ARGS=()
if [ -n "${MORSE_DAD_NAME:-}" ]; then
  NAME_ARGS+=(--var "MORSE_DAD_NAME:${MORSE_DAD_NAME}")
fi
if [ -n "${MORSE_EMMA_NAME:-}" ]; then
  NAME_ARGS+=(--var "MORSE_EMMA_NAME:${MORSE_EMMA_NAME}")
fi

if [ -z "$HOST" ] || [[ "$HOST" == *.workers.dev ]] \
    || [[ "$HOST" == localhost || "$HOST" == 127.0.0.1 ]]; then
  echo "worker-deploy: no custom domain in MORSE_SERVER_URL ($URL) — plain deploy (workers.dev URL only)."
  exec npx wrangler deploy "${NAME_ARGS[@]}" "$@"
fi

echo "worker-deploy: deploying with custom domain $HOST"
exec npx wrangler deploy --domain "$HOST" "${NAME_ARGS[@]}" "$@"
