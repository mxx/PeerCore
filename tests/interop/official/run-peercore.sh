#!/usr/bin/env bash
set -euo pipefail

env_to_cli_bin="${PEERCORE_ENV_TO_CLI_BIN:-/usr/local/bin/env-to-cli.sh}"
probe_bin="${PEERCORE_PROBE_BIN:-/usr/local/bin/interop_peercore_probe}"

args=()
while IFS= read -r line; do
  args+=("${line}")
done < <("${env_to_cli_bin}")

exec "${probe_bin}" "$@" "${args[@]}"
