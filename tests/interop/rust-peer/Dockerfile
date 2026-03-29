FROM rust:1.85-bookworm AS build

WORKDIR /app
COPY tests/interop/rust-peer/Cargo.toml /app/Cargo.toml
COPY tests/interop/rust-peer/src /app/src
RUN cargo build --release

FROM debian:bookworm-slim
COPY --from=build /app/target/release/peercore-interop-rust /usr/local/bin/rust-libp2p-peer
ENTRYPOINT ["/usr/local/bin/rust-libp2p-peer"]
