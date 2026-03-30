# Interop Harness

This directory contains interoperability and replay-test tooling for checking
PeerCore against external libp2p implementations.

Current scope:

- classify failures by protocol layer
- exercise outbound and inbound dial directions
- capture machine-readable JSON logs from the PeerCore side
- provide both Docker-based and host-local external endpoints
- keep a small replay-style regression layer for protocol behavior

Current limitation:

- PeerCore is not yet expected to fully interoperate with go-libp2p or
  rust-libp2p Noise XX.
- Rust-side application stream interoperability is still limited by the current
  external peer scaffold.

## Layout

- `peercore_probe.cpp`
  Local binary that starts a `Swarm`, optionally listens, optionally dials, and
  prints JSON event lines.
- `noise_trace.cpp`
  Local binary for deterministic PeerCore Noise XX tracing with fixed initiator
  secrets, useful alongside the Go oracle.
- `go-peer/`
  Minimal go-libp2p peer program used by both Docker and local runners.
- `go-noise-oracle/`
  Deterministic Go helper that uses `flynn/noise` to generate authoritative
  Noise XX handshake messages for offline comparison.
- `rust-peer/`
  Minimal rust-libp2p peer program used by the Docker runner.
- `docker/`
  Docker-only interop runner, compose file, oracle wrapper, and Docker
  artifact output.
- `local/`
  Host-local interop runner, local Go build helper, and local artifact output.
- `official/`
  Thin adapter layer for exposing PeerCore to the libp2p official
  interoperability framework without coupling it to the internal runners.
- `report.sh`
  Shared summary-to-markdown helper used by both runners.
- `common.sh`
  Shared shell helpers for probe execution and summary generation.

## Docker Cases

- `go-outbound`
  PeerCore dials the Go peer in Docker.
- `rust-outbound`
  PeerCore dials the Rust peer in Docker.
- `go-inbound`
  Go peer in Docker dials PeerCore.
- `rust-inbound`
  Rust peer in Docker dials PeerCore.

## Local Cases

- `go-outbound`
  PeerCore dials a host-local `go-peer` process.
- `go-inbound`
  A host-local `go-peer` process dials PeerCore.

The local runner is intentionally small and deterministic. It is meant for
fast host-local iteration without Docker, while the Docker runner remains the
broader cross-runtime diagnostic matrix.

## Expected Result Today

These cases are primarily diagnostic. A useful run should tell us whether the
failure happens at:

1. TCP connectivity
2. multistream-select
3. `/noise` negotiation
4. Noise handshake completion
5. yamux selection
6. stream-level protocol negotiation

At the moment, failure during Noise handshake is an expected outcome.
When the secure channel does complete, the Go-based cases also exercise a
`/test/echo/1.0.0` stream to help distinguish transport success from
application-stream success.

## Usage

Build the local probe:

```bash
cmake --build build --target interop_peercore_probe
```

Build the deterministic Noise trace helper:

```bash
cmake --build build --target interop_noise_trace
```

Run one Docker case:

```bash
tests/interop/docker/run.sh go-outbound
```

Run the whole Docker matrix:

```bash
tests/interop/docker/run.sh all
```

Run the deterministic Go Noise oracle against a captured `msg1`:

```bash
tests/interop/docker/oracle.sh <msg1-hex>
```

Build local Go test binaries for host-local interop:

```bash
tests/interop/local/setup-go-env.sh
```

This writes local binaries to `tests/interop/bin/`:

- `go-peer`
- `go-noise-oracle`
- `go-noise-trace`

By default the setup script uses `/tmp/peercore-go` for `GOPATH`,
`GOMODCACHE`, and `GOCACHE` to avoid machine-global cache conflicts.
You can override these paths with `GO_CACHE_ROOT`, `GOPATH`, `GOMODCACHE`,
or `GOCACHE` before running the script.

Run one local case:

```bash
tests/interop/local/run.sh go-outbound
```

Run the local matrix:

```bash
tests/interop/local/run.sh all
```

Artifacts are split by runner:

- Docker: `tests/interop/docker/artifacts/<case>/`
- Local: `tests/interop/local/artifacts/<case>/`

Each case also produces a `summary.txt` file with a coarse layer
classification. Running a full matrix also writes `report.md` under that
runner's artifact root. The summary includes both PeerCore-side flags and
external peer readiness / connection flags, including whether the external
peer observed or completed the test echo stream.

For compatibility, the legacy top-level wrappers still forward to the new
paths:

- `tests/interop/run.sh` -> Docker runner
- `tests/interop/oracle.sh` -> Docker oracle wrapper
- `tests/interop/setup-local-go-env.sh` -> local Go builder

## Official Framework Adapter

The `tests/interop/official/` directory is intentionally separate from both the
internal local and internal Docker runners. It packages
`interop_peercore_probe` as a small adapter image and documents a stable event
schema, so the libp2p official interoperability framework can launch PeerCore
without inheriting the repo's internal summary/reporting conventions.

## Replay Regressions

Fast protocol replay fixtures live under `tests/fixtures/yamux/` and are
exercised by `test_yamux_replay`. These tests do not require sockets, Docker,
or external processes; they feed fixed frame hex into `YamuxSession` and
assert the exact outgoing response, accepted stream IDs, and readable payload
bytes.

## Follow-up

Once full libp2p Noise XX and stream multistream-select are implemented, these
diagnostic cases should be upgraded into strict pass/fail interop checks.
