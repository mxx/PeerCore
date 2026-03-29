#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
COMPOSE_FILE="$ROOT_DIR/tests/interop/docker-compose.yml"

if [[ $# -lt 1 ]]; then
  echo "usage: tests/interop/oracle.sh <msg1-hex> [payload-hex]" >&2
  exit 2
fi

MSG1_HEX="$1"
PAYLOAD_HEX="${2:-}"

docker compose -f "$COMPOSE_FILE" run --rm \
  -e MSG1_HEX="$MSG1_HEX" \
  -e PAYLOAD_HEX="$PAYLOAD_HEX" \
  go-noise-oracle
