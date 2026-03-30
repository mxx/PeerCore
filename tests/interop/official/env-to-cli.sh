#!/usr/bin/env bash
set -euo pipefail

first_nonempty() {
  local name=""
  for name in "$@"; do
    if [[ -n "${!name:-}" ]]; then
      printf '%s' "${!name}"
      return 0
    fi
  done
  return 1
}

append_csv_args() {
  local flag="$1"
  local csv="${2:-}"
  local item=""
  local old_ifs="$IFS"
  IFS=','
  for item in $csv; do
    [[ -n "$item" ]] || continue
    printf '%s\n' "$flag"
    printf '%s\n' "$item"
  done
  IFS="$old_ifs"
}

if mode_value="$(first_nonempty PEERCORE_PROBE_MODE TEST_MODE ROLE)"; then
  printf '%s\n%s\n' "--mode" "$mode_value"
fi

if listen_csv="$(first_nonempty PEERCORE_PROBE_LISTEN_ADDRS LISTEN_ADDRS LISTEN_ADDR)"; then
  append_csv_args "--listen" "$listen_csv"
fi

if dial_value="$(first_nonempty PEERCORE_PROBE_DIAL_ADDR DIAL_ADDR TARGET_ADDR)"; then
  printf '%s\n%s\n' "--dial" "$dial_value"
fi

if muxer_csv="$(first_nonempty PEERCORE_PROBE_MUXERS MUXERS MUXER)"; then
  append_csv_args "--muxer" "$muxer_csv"
fi

if runtime_value="$(first_nonempty PEERCORE_PROBE_RUNTIME_MS RUNTIME_MS)"; then
  printf '%s\n%s\n' "--runtime-ms" "$runtime_value"
fi

if protocol_value="$(first_nonempty PEERCORE_PROBE_OPEN_PROTOCOL OPEN_PROTOCOL APP_PROTOCOL)"; then
  printf '%s\n%s\n' "--open-protocol" "$protocol_value"
fi

if security_value="$(first_nonempty PEERCORE_PROBE_SECURITY SECURE_CHANNEL SECURITY)"; then
  printf '%s\n%s\n' "--security" "$security_value"
fi

if transport_value="$(first_nonempty PEERCORE_PROBE_TRANSPORT TRANSPORT)"; then
  printf '%s\n%s\n' "--transport" "$transport_value"
fi

if identity_value="$(first_nonempty PEERCORE_IDENTITY_SECRET_HEX IDENTITY_SECRET_HEX)"; then
  printf '%s\n%s\n' "--identity-secret-hex" "$identity_value"
fi

if inputs_path="$(first_nonempty PEERCORE_PROBE_WRITE_INPUTS_YAML WRITE_INPUTS_YAML)"; then
  printf '%s\n%s\n' "--write-inputs-yaml" "$inputs_path"
fi
