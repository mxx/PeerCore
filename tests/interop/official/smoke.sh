#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: tests/interop/official/smoke.sh <repo-root> <probe-bin>" >&2
  exit 2
fi

ROOT_DIR="$1"
PROBE_BIN="$2"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/peercore-official-smoke.XXXXXX")"
LOG_FILE="${WORK_DIR}/events.jsonl"
INPUTS_FILE="${WORK_DIR}/inputs.yaml"
ERR_FILE="${WORK_DIR}/stderr.log"
cleanup() {
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

export PEERCORE_ENV_TO_CLI_BIN="${ROOT_DIR}/tests/interop/official/env-to-cli.sh"
export PEERCORE_PROBE_BIN="${PROBE_BIN}"
export PEERCORE_PROBE_MODE="echo-server"
export PEERCORE_PROBE_LISTEN_ADDRS="/ip4/127.0.0.1/tcp/0"
export PEERCORE_PROBE_MUXERS="/yamux/1.0.0"
export PEERCORE_PROBE_RUNTIME_MS="25"
export PEERCORE_PROBE_SECURITY="/noise"
export PEERCORE_PROBE_TRANSPORT="tcp"
export PEERCORE_PROBE_WRITE_INPUTS_YAML="${INPUTS_FILE}"
export PEERCORE_IDENTITY_SECRET_HEX="1111111111111111111111111111111111111111111111111111111111111111"

if ! "${ROOT_DIR}/tests/interop/official/run-peercore.sh" >"${LOG_FILE}" 2>"${ERR_FILE}"; then
  if grep -q "Operation not permitted" "${ERR_FILE}"; then
    echo "official adapter smoke skipped: sandbox denied listen()" >&2
    exit 77
  fi
  cat "${ERR_FILE}" >&2
  exit 1
fi

test -s "${LOG_FILE}"
test -s "${INPUTS_FILE}"

grep -q '"type":"probe_started"' "${LOG_FILE}"
grep -q '"type":"probe_config"' "${LOG_FILE}"
grep -q '"type":"probe_finished"' "${LOG_FILE}"
grep -q '"phase":"lifecycle"' "${LOG_FILE}"
grep -q '"timestamp_ms":' "${LOG_FILE}"

grep -q "^mode: 'echo-server'$" "${INPUTS_FILE}"
grep -q "^transport: 'tcp'$" "${INPUTS_FILE}"
grep -q "^security: '/noise'$" "${INPUTS_FILE}"
grep -q "^  - '/yamux/1.0.0'$" "${INPUTS_FILE}"
