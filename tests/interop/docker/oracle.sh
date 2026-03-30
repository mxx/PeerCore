#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
COMPOSE_FILE="$ROOT_DIR/tests/interop/docker/docker-compose.yml"

if [[ $# -lt 1 ]]; then
  echo "usage: tests/interop/docker/oracle.sh <msg1-hex> [payload-hex]" >&2
  exit 2
fi

MSG1_HEX="$1"
PAYLOAD_HEX="${2:-}"

docker compose -f "$COMPOSE_FILE" run --rm \
  -e MSG1_HEX="$MSG1_HEX" \
  -e PAYLOAD_HEX="$PAYLOAD_HEX" \
  -e STATIC_SECRET_HEX="${STATIC_SECRET_HEX:-}" \
  -e EPHEMERAL_SECRET_HEX="${EPHEMERAL_SECRET_HEX:-}" \
  -e PROLOGUE_HEX="${PROLOGUE_HEX:-}" \
  go-noise-oracle
