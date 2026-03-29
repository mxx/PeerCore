#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CASE_NAME="${1:-}"
ARTIFACT_DIR="$ROOT_DIR/tests/interop/artifacts/${CASE_NAME}"
PROBE_BIN="$ROOT_DIR/build/tests/interop_peercore_probe"
COMPOSE_FILE="$ROOT_DIR/tests/interop/docker-compose.yml"
REPORT_BIN="$ROOT_DIR/tests/interop/report.sh"

if [[ -z "$CASE_NAME" ]]; then
  echo "usage: tests/interop/run.sh <go-outbound|rust-outbound|go-inbound|rust-inbound>" >&2
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

if [[ ! -x "$PROBE_BIN" ]]; then
  echo "missing $PROBE_BIN; build it with: cmake --build build --target interop_peercore_probe" >&2
  exit 2
fi

cleanup() {
  docker compose -f "$COMPOSE_FILE" down --remove-orphans >/dev/null 2>&1 || true
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

has_event() {
  local event_name="$1"
  [[ -f "$ARTIFACT_DIR/peercore.jsonl" ]] &&
    grep -q "\"type\":\"${event_name}\"" "$ARTIFACT_DIR/peercore.jsonl"
}

last_detail_for() {
  local event_name="$1"
  if [[ ! -f "$ARTIFACT_DIR/peercore.jsonl" ]]; then
    return 0
  fi
  sed -n "s/.*\"type\":\"${event_name}\".*\"detail\":\"\\([^\"]*\\)\".*/\\1/p" \
    "$ARTIFACT_DIR/peercore.jsonl" | tail -n1
}

write_summary() {
  local classification=""
  local stage=""
  local detail=""

  if has_event connection_established; then
    classification="PASS"
    stage="connection_established"
    detail="$(last_detail_for connection_established)"
  elif has_event protocol_error; then
    classification="EXPECTED_FAIL"
    stage="protocol_error"
    detail="$(last_detail_for protocol_error)"
  elif has_event dial_failed; then
    classification="UNEXPECTED_FAIL"
    stage="dial_failed"
    detail="$(last_detail_for dial_failed)"
  elif has_event listen_error; then
    classification="UNEXPECTED_FAIL"
    stage="listen_error"
    detail="$(last_detail_for listen_error)"
  else
    classification="UNEXPECTED_FAIL"
    stage="unknown"
    detail="no terminal diagnostic event recorded"
  fi

  cat >"$ARTIFACT_DIR/summary.txt" <<EOF
case: $CASE_NAME
classification: $classification
stage: $stage
detail: $detail
probe_started: $(has_event probe_started && echo yes || echo no)
listener_started: $(has_event listener_started && echo yes || echo no)
dial_requested: $(has_event dial_requested && echo yes || echo no)
incoming_connection: $(has_event incoming_connection && echo yes || echo no)
peer_identified: $(has_event peer_identified && echo yes || echo no)
protocol_negotiated: $(has_event protocol_negotiated && echo yes || echo no)
connection_established: $(has_event connection_established && echo yes || echo no)
stream_opened: $(has_event stream_opened && echo yes || echo no)
protocol_error: $(has_event protocol_error && echo yes || echo no)
dial_failed: $(has_event dial_failed && echo yes || echo no)
EOF
}

case "$CASE_NAME" in
  go-outbound)
    docker compose -f "$COMPOSE_FILE" up -d go-peer
    sleep 3
    run_probe --dial /ip4/127.0.0.1/tcp/41001 --muxer /yamux/1.0.0 --runtime-ms 6000
    docker compose -f "$COMPOSE_FILE" logs --no-color go-peer > "$ARTIFACT_DIR/go-peer.log"
    ;;
  rust-outbound)
    docker compose -f "$COMPOSE_FILE" up -d rust-peer
    sleep 3
    run_probe --dial /ip4/127.0.0.1/tcp/41002 --muxer /yamux/1.0.0 --runtime-ms 6000
    docker compose -f "$COMPOSE_FILE" logs --no-color rust-peer > "$ARTIFACT_DIR/rust-peer.log"
    ;;
  go-inbound)
    run_probe_background --listen /ip4/0.0.0.0/tcp/41003 --muxer /yamux/1.0.0 --runtime-ms 6000
    TARGET_ADDR="$(wait_for_probe_addr)"
    TARGET_ADDR="$TARGET_ADDR" docker compose -f "$COMPOSE_FILE" up -d go-peer
    wait "$PROBE_PID"
    docker compose -f "$COMPOSE_FILE" logs --no-color go-peer > "$ARTIFACT_DIR/go-peer.log"
    ;;
  rust-inbound)
    run_probe_background --listen /ip4/0.0.0.0/tcp/41004 --muxer /yamux/1.0.0 --runtime-ms 6000
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

write_summary
cat "$ARTIFACT_DIR/summary.txt"
