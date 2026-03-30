#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
CASE_NAME="${1:-}"
ARTIFACT_ROOT="$ROOT_DIR/tests/interop/local/artifacts"
ARTIFACT_DIR="$ARTIFACT_ROOT/${CASE_NAME}"
PROBE_BIN="$ROOT_DIR/build/tests/interop_peercore_probe"
GO_PEER_BIN="$ROOT_DIR/tests/interop/bin/go-peer"
REPORT_BIN="$ROOT_DIR/tests/interop/local/report.sh"
EXTERNAL_PID=""
PROBE_PID=""

# shellcheck source=tests/interop/common.sh
source "$ROOT_DIR/tests/interop/common.sh"

if [[ -z "$CASE_NAME" ]]; then
  echo "usage: tests/interop/local/run.sh <go-outbound|go-inbound|all>" >&2
  exit 2
fi

if [[ "$CASE_NAME" == "all" ]]; then
  "$0" go-outbound
  "$0" go-inbound
  "$REPORT_BIN"
  exit 0
fi

mkdir -p "$ARTIFACT_DIR"
ensure_probe_bin

if [[ ! -x "$GO_PEER_BIN" ]]; then
  echo "missing $GO_PEER_BIN; build it with: tests/interop/local/setup-go-env.sh" >&2
  exit 2
fi

cleanup() {
  if [[ -n "$PROBE_PID" ]]; then
    wait "$PROBE_PID" 2>/dev/null || true
  fi
  if [[ -n "$EXTERNAL_PID" ]]; then
    kill "$EXTERNAL_PID" 2>/dev/null || true
    wait "$EXTERNAL_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

case "$CASE_NAME" in
  go-outbound)
    LISTEN_ADDR=/ip4/127.0.0.1/tcp/41101 RUNTIME_MS=6000 \
      "$GO_PEER_BIN" >"$ARTIFACT_DIR/go-peer.log" 2>&1 &
    EXTERNAL_PID=$!
    if ! wait_for_json_event "$ARTIFACT_DIR/go-peer.log" ready 10 "$EXTERNAL_PID"; then
      write_summary "$CASE_NAME"
      cat "$ARTIFACT_DIR/summary.txt"
      exit 1
    fi
    run_probe --mode echo-client --transport tcp --security /noise \
      --dial /ip4/127.0.0.1/tcp/41101 --muxer /yamux/1.0.0 \
      --open-protocol /test/echo/1.0.0 --runtime-ms 6000
    wait "$EXTERNAL_PID" || true
    EXTERNAL_PID=""
    ;;
  go-inbound)
    run_probe_background --mode echo-server --transport tcp --security /noise \
      --listen /ip4/127.0.0.1/tcp/41103 --muxer /yamux/1.0.0 --runtime-ms 6000
    TARGET_ADDR="$(wait_for_probe_addr 10)"
    TARGET_ADDR="$TARGET_ADDR" LISTEN_ADDR=/ip4/127.0.0.1/tcp/41102 RUNTIME_MS=6000 \
      "$GO_PEER_BIN" >"$ARTIFACT_DIR/go-peer.log" 2>&1 &
    EXTERNAL_PID=$!
    wait "$PROBE_PID" || true
    PROBE_PID=""
    wait "$EXTERNAL_PID" || true
    EXTERNAL_PID=""
    ;;
  *)
    echo "unknown case: $CASE_NAME" >&2
    exit 2
    ;;
esac

write_summary "$CASE_NAME"
cat "$ARTIFACT_DIR/summary.txt"
