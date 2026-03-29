# Interop Harness

This directory contains a diagnostic interoperability harness for checking
PeerCore against external libp2p implementations.

Current scope:

- classify failures by protocol layer
- exercise outbound and inbound dial directions
- capture machine-readable JSON logs from the PeerCore side
- provide Go and Rust libp2p peer containers as external endpoints

Current limitation:

- PeerCore is not yet expected to fully interoperate with go-libp2p or
  rust-libp2p Noise XX.
- Rust-side application stream interoperability is still limited by the current
  external peer scaffold.

## Layout

- `peercore_probe.cpp`
  Local binary that starts a `Swarm`, optionally listens, optionally dials, and
  prints JSON event lines.
- `docker-compose.yml`
  Container definitions for Go and Rust libp2p peers.
- `go-peer/`
  Minimal go-libp2p peer container.
- `go-noise-oracle/`
  Deterministic Go helper that uses `flynn/noise` to generate authoritative
  Noise XX handshake messages for offline comparison.
- `rust-peer/`
  Minimal rust-libp2p peer container.
- `run.sh`
  Runner for the outbound/inbound diagnostic matrix plus summary generation.
- `report.sh`
  Aggregates per-case summaries into a single markdown report.
- `oracle.sh`
  Convenience wrapper for running the Go Noise oracle in Docker.

## Cases

- `go-outbound`
  PeerCore dials the Go peer.
- `rust-outbound`
  PeerCore dials the Rust peer.
- `go-inbound`
  Go peer dials PeerCore.
- `rust-inbound`
  Rust peer dials PeerCore.

## Expected result today

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

Run one case:

```bash
tests/interop/run.sh go-outbound
```

Run the whole matrix:

```bash
tests/interop/run.sh all
```

Run the deterministic Go Noise oracle against a captured `msg1`:

```bash
tests/interop/oracle.sh <msg1-hex>
```

The oracle prints a single JSON object containing the responder's `msg2`,
its fixed local public keys, and the echoed remote ephemeral key. This is
useful when narrowing a Noise mismatch to a specific handshake token.

Artifacts are written under `tests/interop/artifacts/<case>/`.
Each case also produces a `summary.txt` file with a coarse layer classification.
Running the full matrix also writes `tests/interop/artifacts/report.md`.
The summary includes both PeerCore-side flags and external peer readiness /
connection flags, including whether the external peer observed or completed the
test echo stream.

## Follow-up

Once full libp2p Noise XX and stream multistream-select are implemented, these
diagnostic cases should be upgraded into strict pass/fail interop checks.
