#!/usr/bin/env bash
# Deploy the Morse-code Worker, attaching the custom-domain route derived
# from MORSE_SERVER_URL in the repo-root .env — so the hostname lives in
# exactly one place (that .env value) and never in wrangler.toml or docs.
#
#   MORSE_SERVER_URL=https://<your-host>/  -> npx wrangler deploy --route "<host>/*"
#   MORSE_SERVER_URL unset / localhost / *.workers.dev
#                                            -> plain deploy (workers.dev URL only)
#
# MORSE_WORKER_ZONE (optional, in .env or the environment) is passed as
# --zone if route attachment needs an explicit zone (it is experimental in
# wrangler 4.x and needs --x-route-zones). Dashboard alternative:
# Workers & Pages -> morse-code -> Settings -> Triggers -> Add route.
#
# Device names come from .env too (MORSE_DAD_NAME / MORSE_EMMA_NAME) and are
# passed as --var on EVERY deploy (both branches below): the wrangler.toml
# [vars] defaults (DAD/EMMA) are only a fallback, and without --var a deploy
# would silently revert the names to them. Names are single words; a name
# containing ":" would confuse the KEY:VALUE split.
#
# Pass --dry-run to validate the deploy (bundling, bindings, route) without
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

ARGS=(--route "$HOST/*" "${NAME_ARGS[@]}")
if [ -n "${MORSE_WORKER_ZONE:-}" ]; then
  ARGS+=(--x-route-zones --zone "$MORSE_WORKER_ZONE")
fi
if [ "${1:-}" = "--dry-run" ]; then
  ARGS+=(--dry-run)
fi
echo "worker-deploy: deploying with route $HOST/*"
exec npx wrangler deploy "${ARGS[@]}"
