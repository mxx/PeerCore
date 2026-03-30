# Official Interop Adapter

This directory contains the thin compatibility layer for running PeerCore under
the libp2p official interoperability framework.

It intentionally does not replace the internal runners under:

- `tests/interop/local/`
- `tests/interop/docker/`

Those remain optimized for fast in-repo diagnostics. The files here exist only
to expose `interop_peercore_probe` as a stable test application image with a
small environment-to-CLI adapter.

## Files

- `Dockerfile`
  Builds `interop_peercore_probe` into a container image suitable for external
  orchestration.
- `run-peercore.sh`
  Container entrypoint that translates environment variables to CLI arguments
  and executes the probe.
- `env-to-cli.sh`
  Emits one CLI argument per line for safe shell-array consumption.
- `schema.md`
  Documents the JSON event schema emitted by the probe.

## Environment Contract

The adapter currently understands these variables:

- `PEERCORE_PROBE_MODE`
- `TEST_MODE`
- `ROLE`
- `PEERCORE_PROBE_LISTEN_ADDRS`
- `LISTEN_ADDRS`
- `LISTEN_ADDR`
- `PEERCORE_PROBE_DIAL_ADDR`
- `DIAL_ADDR`
- `TARGET_ADDR`
- `PEERCORE_PROBE_MUXERS`
- `MUXERS`
- `MUXER`
- `PEERCORE_PROBE_RUNTIME_MS`
- `RUNTIME_MS`
- `PEERCORE_PROBE_OPEN_PROTOCOL`
- `OPEN_PROTOCOL`
- `APP_PROTOCOL`
- `PEERCORE_PROBE_SECURITY`
- `SECURE_CHANNEL`
- `SECURITY`
- `PEERCORE_PROBE_TRANSPORT`
- `TRANSPORT`
- `PEERCORE_IDENTITY_SECRET_HEX`
- `IDENTITY_SECRET_HEX`
- `PEERCORE_PROBE_WRITE_INPUTS_YAML`
- `WRITE_INPUTS_YAML`

Multi-value variables use comma-separated values.

The `PEERCORE_*` names are the native adapter interface. The shorter aliases are
compatibility shims so the adapter can be wired into external runners with less
glue while we converge on the official transport-app contract.

## Scope

This is the first integration layer only. It is meant to make PeerCore
launchable by the official framework's transport interoperability jobs while
keeping the richer internal diagnostics separate.

## Smoke Validation

The adapter also ships with `smoke.sh`, a small end-to-end self-check that runs
the adapter locally, verifies JSON event output, and checks that `inputs.yaml`
is written. Because it performs a real local `listen()`, it is not enabled in
the default CTest set. Enable it explicitly with:

```bash
cmake -S . -B build -DPEERCORE_ENABLE_NETWORK_SMOKE_TESTS=ON
ctest --test-dir build -R official_adapter_smoke --output-on-failure
```
