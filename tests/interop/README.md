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
- stream-level multistream-select is not wired yet, so application protocol
  interoperability is not expected to pass.

## Layout

- `peercore_probe.cpp`
  Local binary that starts a `Swarm`, optionally listens, optionally dials, and
  prints JSON event lines.
- `docker-compose.yml`
  Container definitions for Go and Rust libp2p peers.
- `go-peer/`
  Minimal go-libp2p peer container.
- `rust-peer/`
  Minimal rust-libp2p peer container.
- `run.sh`
  Runner for the outbound/inbound diagnostic matrix plus summary generation.
- `report.sh`
  Aggregates per-case summaries into a single markdown report.

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

At the moment, failure during Noise handshake is an expected outcome.

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

Artifacts are written under `tests/interop/artifacts/<case>/`.
Each case also produces a `summary.txt` file with a coarse layer classification.
Running the full matrix also writes `tests/interop/artifacts/report.md`.

## Follow-up

Once full libp2p Noise XX and stream multistream-select are implemented, these
diagnostic cases should be upgraded into strict pass/fail interop checks.
