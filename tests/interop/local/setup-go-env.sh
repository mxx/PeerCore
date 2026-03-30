#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BIN_DIR="${ROOT_DIR}/tests/interop/bin"

# Keep local Go caches in writable, disposable paths by default.
GO_CACHE_ROOT="${GO_CACHE_ROOT:-/tmp/peercore-go}"
GOCACHE="${GOCACHE:-${GO_CACHE_ROOT}/cache}"
GOPATH="${GOPATH:-${GO_CACHE_ROOT}}"
GOMODCACHE="${GOMODCACHE:-${GOPATH}/pkg/mod}"

mkdir -p "${BIN_DIR}" "${GOCACHE}" "${GOMODCACHE}"

echo "[interop] using BIN_DIR=${BIN_DIR}"
echo "[interop] using GOPATH=${GOPATH}"
echo "[interop] using GOMODCACHE=${GOMODCACHE}"
echo "[interop] using GOCACHE=${GOCACHE}"

prepare_tmp_module() {
  local source_dir="$1"
  local tmp_dir
  tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/peercore-go-build.XXXXXX")"
  cp "${source_dir}/go.mod" "${tmp_dir}/go.mod"
  if [[ -f "${source_dir}/go.sum" ]]; then
    cp "${source_dir}/go.sum" "${tmp_dir}/go.sum"
  fi
  echo "${tmp_dir}"
}

go_env_build() {
  GOCACHE="${GOCACHE}" GOPATH="${GOPATH}" GOMODCACHE="${GOMODCACHE}" "$@"
}

build_go_peer() {
  local source_dir="${ROOT_DIR}/tests/interop/go-peer"
  local tmp_dir
  tmp_dir="$(prepare_tmp_module "${source_dir}")"
  echo "[interop] building go-peer from ${source_dir}"
  (
    cp "${source_dir}/main.go" "${tmp_dir}/main.go"
    cd "${tmp_dir}"
    go_env_build go mod tidy
    go_env_build go build -o "${BIN_DIR}/go-peer" main.go
  )
  rm -rf "${tmp_dir}"
}

build_go_oracles() {
  local source_dir="${ROOT_DIR}/tests/interop/go-noise-oracle"
  local tmp_dir
  tmp_dir="$(prepare_tmp_module "${source_dir}")"
  echo "[interop] building go-noise-oracle tools from ${source_dir}"
  (
    cp "${source_dir}/main.go" "${tmp_dir}/main.go"
    cp "${source_dir}/trace_main.go" "${tmp_dir}/trace_main.go"
    cd "${tmp_dir}"
    go_env_build go mod tidy
    go_env_build go build -o "${BIN_DIR}/go-noise-oracle" main.go
    go_env_build go build -o "${BIN_DIR}/go-noise-trace" trace_main.go
  )
  rm -rf "${tmp_dir}"
}

build_go_peer
build_go_oracles

echo "[interop] local go binaries ready:"
ls -lh "${BIN_DIR}/go-peer" "${BIN_DIR}/go-noise-oracle" "${BIN_DIR}/go-noise-trace"

cat <<'EOF'

[interop] quick smoke commands:
  RUNTIME_MS=1000 LISTEN_ADDR=/ip4/127.0.0.1/tcp/41111 tests/interop/bin/go-peer
  tests/interop/bin/go-noise-trace
  MSG1_HEX=<hex> tests/interop/bin/go-noise-oracle

EOF
