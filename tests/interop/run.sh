#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CASE_NAME="${1:-}"
ARTIFACT_DIR="$ROOT_DIR/tests/interop/artifacts/${CASE_NAME}"
PROBE_BIN="$ROOT_DIR/build/tests/interop_peercore_probe"

if [[ -z "$CASE_NAME" ]]; then
  echo "usage: tests/interop/run.sh <go-outbound|rust-outbound|go-inbound|rust-inbound>" >&2
  exit 2
fi

mkdir -p "$ARTIFACT_DIR"

if [[ ! -x "$PROBE_BIN" ]]; then
  echo "missing $PROBE_BIN; build it with: cmake --build build --target interop_peercore_probe" >&2
  exit 2
fi

cleanup() {
  docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" down --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

run_probe() {
  "$PROBE_BIN" "$@" | tee "$ARTIFACT_DIR/peercore.jsonl"
}

run_probe_background() {
  "$PROBE_BIN" "$@" >"$ARTIFACT_DIR/peercore.jsonl" 2>&1 &
  PROBE_PID=$!
}

wait_for_probe_addr() {
  local timeout_secs=10
  local peer_id=""
  local listen_addr=""
  for _ in $(seq 1 $((timeout_secs * 10))); do
    if [[ -f "$ARTIFACT_DIR/peercore.jsonl" ]]; then
      peer_id="$(sed -n 's/.*"type":"probe_started".*"detail":"\([^"]*\)".*/\1/p' "$ARTIFACT_DIR/peercore.jsonl" | tail -n1)"
      listen_addr="$(sed -n 's/.*"type":"listener_started".*"detail":"\([^"]*\)".*/\1/p' "$ARTIFACT_DIR/peercore.jsonl" | tail -n1)"
      if [[ -n "$peer_id" && -n "$listen_addr" ]]; then
        echo "${listen_addr}/p2p/${peer_id}"
        return 0
      fi
    fi
    sleep 0.1
  done
  echo "failed to discover probe listen address" >&2
  return 1
}

case "$CASE_NAME" in
  go-outbound)
    docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" up -d go-peer
    sleep 3
    run_probe --dial /ip4/127.0.0.1/tcp/41001 --muxer /yamux/1.0.0 --runtime-ms 6000
    docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" logs --no-color go-peer > "$ARTIFACT_DIR/go-peer.log"
    ;;
  rust-outbound)
    docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" up -d rust-peer
    sleep 3
    run_probe --dial /ip4/127.0.0.1/tcp/41002 --muxer /yamux/1.0.0 --runtime-ms 6000
    docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" logs --no-color rust-peer > "$ARTIFACT_DIR/rust-peer.log"
    ;;
  go-inbound)
    run_probe_background --listen /ip4/0.0.0.0/tcp/41003 --muxer /yamux/1.0.0 --runtime-ms 6000
    TARGET_ADDR="$(wait_for_probe_addr)"
    TARGET_ADDR="$TARGET_ADDR" docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" up -d go-peer
    wait "$PROBE_PID"
    docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" logs --no-color go-peer > "$ARTIFACT_DIR/go-peer.log"
    ;;
  rust-inbound)
    run_probe_background --listen /ip4/0.0.0.0/tcp/41004 --muxer /yamux/1.0.0 --runtime-ms 6000
    TARGET_ADDR="$(wait_for_probe_addr)"
    TARGET_ADDR="$TARGET_ADDR" docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" up -d rust-peer
    wait "$PROBE_PID"
    docker compose -f "$ROOT_DIR/tests/interop/docker-compose.yml" logs --no-color rust-peer > "$ARTIFACT_DIR/rust-peer.log"
    ;;
  *)
    echo "unknown case: $CASE_NAME" >&2
    exit 2
    ;;
esac
