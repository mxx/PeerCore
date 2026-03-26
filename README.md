# PeerCore

A minimal C++20 libp2p subset library designed for interoperability with rust-libp2p nodes.

## Architecture

```
Node → Swarm → ConnectionSession → MuxedStream
```

Full architecture design: [doc/architecture.md](doc/architecture.md)

## Dependencies

| Dependency | Purpose | Required |
|------------|---------|----------|
| [libsodium](https://libsodium.org) | Noise handshake (DH, AEAD) | Yes |
| [GoogleTest](https://github.com/google/googletest) | Unit tests | Auto-fetched |

CMake 3.20+ and a C++20-capable compiler are required.

## Build

### Install dependencies

**macOS**
```sh
brew install libsodium cmake
```

**Ubuntu / Debian**
```sh
apt install libsodium-dev cmake
```

### Configure and build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Release build:
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Disable tests:
```sh
cmake -B build -DPEERCORE_BUILD_TESTS=OFF
cmake --build build
```

## Test

```sh
make -C build test
# 或
cmake --build build --target test
```

Expected output:
```
Test project .../PeerCore/build
    Start 1: test_types
1/4 Test #1: test_types ...............   Passed
    Start 2: test_peer_store
2/4 Test #2: test_peer_store ..........   Passed
    Start 3: test_routing_table
3/4 Test #3: test_routing_table .......   Passed
    Start 4: test_swarm
4/4 Test #4: test_swarm ...............   Passed

100% tests passed, 0 tests failed out of 4
```

## Project Structure

```
PeerCore/
├── CMakeLists.txt
├── cmake/
│   └── FindLibsodium.cmake
├── doc/
│   └── architecture.md
├── include/peercore/          # Public API headers
│   ├── types.hpp              # PeerId, Multiaddr, Result<T>, ...
│   ├── events.hpp             # SwarmEvent, ConnectionEvent, Action
│   ├── node.hpp
│   ├── swarm.hpp
│   ├── connection_session.hpp
│   ├── muxed_stream.hpp
│   ├── protocol_handler.hpp
│   ├── peer_store.hpp
│   ├── routing_table.hpp
│   ├── controller.hpp
│   └── services/
│       ├── dht_service.hpp
│       ├── ping_service.hpp
│       └── identify_service.hpp
├── src/                       # Implementations
│   ├── runtime/               # kqueue / epoll event loop
│   ├── transport/             # TCP transport
│   ├── protocol/
│   │   ├── multistream_select # Protocol negotiation
│   │   ├── noise/             # Noise_XX handshake (libsodium)
│   │   └── yamux/             # Stream multiplexer
│   └── services/              # Identify / Ping / DHT
└── tests/
    ├── test_types.cpp
    ├── test_peer_store.cpp
    ├── test_routing_table.cpp
    └── test_swarm.cpp
```

## Coverage

Requires `lcov` and `genhtml`:

```sh
brew install lcov        # macOS
apt install lcov         # Ubuntu
```

Configure and build with coverage instrumentation:

```sh
cmake -B build-cov -DCMAKE_BUILD_TYPE=Debug -DPEERCORE_COVERAGE=ON
cmake --build build-cov
```

Generate report:

```sh
make -C build-cov coverage
# 或
cmake --build build-cov --target coverage
```

HTML report: `build-cov/coverage/html/index.html`

## Implementation Status

| Module | Status |
|--------|--------|
| Types / Result | Done |
| PeerStore | Done |
| RoutingTable (Kademlia) | Done |
| Swarm (scaffold) | Done |
| Event loop (kqueue/epoll) | Done |
| TCP transport | Stub |
| multistream-select | Stub |
| Noise handshake | Stub |
| Yamux multiplexer | Stub |
| Identify service | Stub |
| Ping service | Stub |
| DHT service | Stub |

## Implementation Order

Per the architecture design, modules are implemented in this sequence:

1. Runtime (event loop) ✓
2. TCP transport
3. multistream-select
4. Noise
5. Yamux
6. Identify
7. Ping
8. Kad (minimal)
