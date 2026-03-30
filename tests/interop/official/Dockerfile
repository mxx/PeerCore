FROM debian:bookworm AS build

RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential cmake pkg-config libsodium-dev ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src
RUN cmake -S . -B build -DPEERCORE_BUILD_TESTS=ON && \
    cmake --build build --target interop_peercore_probe

FROM debian:bookworm-slim

RUN apt-get update && \
    apt-get install -y --no-install-recommends libsodium23 ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/tests/interop_peercore_probe /usr/local/bin/interop_peercore_probe
COPY tests/interop/official/run-peercore.sh /usr/local/bin/run-peercore.sh
COPY tests/interop/official/env-to-cli.sh /usr/local/bin/env-to-cli.sh

RUN chmod +x /usr/local/bin/run-peercore.sh /usr/local/bin/env-to-cli.sh

ENTRYPOINT ["/usr/local/bin/run-peercore.sh"]
