# Probe Event Schema

`interop_peercore_probe` emits one JSON object per line to stdout.

## Stable Fields

- `type`
  Event kind such as `probe_started`, `connection_established`,
  `protocol_negotiated`, or `stream_opened`.
- `phase`
  Coarse bucket for the event:
  `lifecycle`, `transport`, `connection`, `negotiation`, or `stream`.
- `timestamp_ms`
  Milliseconds since Unix epoch.
- `detail`
  Human-readable description. This remains diagnostic text and should not be the
  sole machine-parse contract.

## Optional Fields

- `peer_id`
- `connection_id`
- `stream_id`
- `protocol`

## Notes

- Existing internal shell diagnostics continue to key primarily off `type` and
  `detail`.
- The official adapter should treat `type` as the primary machine-readable
  signal and use the other fields opportunistically.
- When `--write-inputs-yaml` or `PEERCORE_PROBE_WRITE_INPUTS_YAML` is provided,
  the probe also writes a small `inputs.yaml` file capturing the resolved test
  configuration for local reproduction.
