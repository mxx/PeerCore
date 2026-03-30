#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
exec "${ROOT_DIR}/tests/interop/report.sh" \
  "${1:-${ROOT_DIR}/tests/interop/local/artifacts}" \
  "${2:-${ROOT_DIR}/tests/interop/local/artifacts/report.md}"
