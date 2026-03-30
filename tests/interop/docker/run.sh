#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
CASE_NAME="${1:-}"
ARTIFACT_ROOT="$ROOT_DIR/tests/interop/docker/artifacts"
ARTIFACT_DIR="$ARTIFACT_ROOT/${CASE_NAME}"
PROBE_BIN="$ROOT_DIR/build/tests/interop_peercore_probe"
COMPOSE_FILE="$ROOT_DIR/tests/interop/docker/docker-compose.yml"
REPORT_BIN="$ROOT_DIR/tests/interop/docker/report.sh"

# shellcheck source=tests/interop/common.sh
source "$ROOT_DIR/tests/interop/common.sh"

if [[ -z "$CASE_NAME" ]]; then
  echo "usage: tests/interop/docker/run.sh <go-outbound|rust-outbound|go-inbound|rust-inbound>" >&2
  exit 2
fi

if [[ "$CASE_NAME" == "all" ]]; then
  "$0" go-outbound
  "$0" rust-outbound
  "$0" go-inbound
  "$0" rust-inbound
  "$REPORT_BIN"
  exit 0
fi

mkdir -p "$ARTIFACT_DIR"

ensure_probe_bin

cleanup() {
  docker compose -f "$COMPOSE_FILE" down --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

case "$CASE_NAME" in
  go-outbound)
    docker compose -f "$COMPOSE_FILE" up -d go-peer
    sleep 3
    run_probe --mode echo-client --transport tcp --security /noise \
      --dial /ip4/127.0.0.1/tcp/41001 --muxer /yamux/1.0.0 \
      --open-protocol /test/echo/1.0.0 --runtime-ms 6000
    docker compose -f "$COMPOSE_FILE" logs --no-color go-peer > "$ARTIFACT_DIR/go-peer.log"
    ;;
  rust-outbound)
    docker compose -f "$COMPOSE_FILE" up -d rust-peer
    sleep 3
    run_probe --mode dial --transport tcp --security /noise \
      --dial /ip4/127.0.0.1/tcp/41002 --muxer /yamux/1.0.0 --runtime-ms 6000
    docker compose -f "$COMPOSE_FILE" logs --no-color rust-peer > "$ARTIFACT_DIR/rust-peer.log"
    ;;
  go-inbound)
    run_probe_background --mode echo-server --transport tcp --security /noise \
      --listen /ip4/0.0.0.0/tcp/41003 --muxer /yamux/1.0.0 --runtime-ms 6000
    TARGET_ADDR="$(wait_for_probe_addr)"
    TARGET_ADDR="$TARGET_ADDR" docker compose -f "$COMPOSE_FILE" up -d go-peer
    wait "$PROBE_PID"
    docker compose -f "$COMPOSE_FILE" logs --no-color go-peer > "$ARTIFACT_DIR/go-peer.log"
    ;;
  rust-inbound)
    run_probe_background --mode listen --transport tcp --security /noise \
      --listen /ip4/0.0.0.0/tcp/41004 --muxer /yamux/1.0.0 --runtime-ms 6000
    TARGET_ADDR="$(wait_for_probe_addr)"
    TARGET_ADDR="$TARGET_ADDR" docker compose -f "$COMPOSE_FILE" up -d rust-peer
    wait "$PROBE_PID"
    docker compose -f "$COMPOSE_FILE" logs --no-color rust-peer > "$ARTIFACT_DIR/rust-peer.log"
    ;;
  *)
    echo "unknown case: $CASE_NAME" >&2
    exit 2
    ;;
esac

write_summary "$CASE_NAME"
cat "$ARTIFACT_DIR/summary.txt"
