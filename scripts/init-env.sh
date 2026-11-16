#!/usr/bin/env bash
# Create .env from .env.example with a fresh random token per device.
#
#   ./scripts/init-env.sh
#   ./scripts/init-env.sh /path/to/.env
#
# WiFi credentials are left as placeholders for you to edit.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${1:-$repo_dir/.env}"

if [[ -e "$target" ]]; then
  echo "refusing to overwrite existing $target" >&2
  exit 1
fi

cp "$repo_dir/.env.example" "$target"

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
while IFS= read -r line; do
  while [[ "$line" == *"REPLACE_WITH_openssl_rand_hex_24"* ]]; do
    line="${line/REPLACE_WITH_openssl_rand_hex_24/$(openssl rand -hex 24)}"
  done
  printf '%s\n' "$line"
done <"$target" >"$tmp"

mv "$tmp" "$target"
trap - EXIT
chmod 600 "$target"

echo "wrote $target with fresh device tokens (chmod 600)"
echo "next: edit WIFI_SSID and WIFI_PASS, then run"
echo "  ./scripts/gen-secrets.py dad"
echo "  ./scripts/gen-secrets.py emma"
