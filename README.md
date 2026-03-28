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
1/7 Test #1: test_types ...............   Passed
    Start 2: test_peer_store
2/7 Test #2: test_peer_store ..........   Passed
    Start 3: test_routing_table
3/7 Test #3: test_routing_table .......   Passed
    Start 4: test_swarm
4/7 Test #4: test_swarm ...............   Passed
    Start 5: test_event_loop
5/7 Test #5: test_event_loop ..........   Passed
    Start 6: test_tcp_transport
6/7 Test #6: test_tcp_transport .......   Passed
    Start 7: test_connection_session
7/7 Test #7: test_connection_session .. Passed

100% tests passed, 0 tests failed out of 7
```

## Project Structure

```
PeerCore/
├── CMakeLists.txt
├── cmake/
│   └── FindLibsodium.cmake
├── doc/
│   ├── architecture.md
│   ├── types.md
│   ├── swarm.md
│   ├── connection_session.md
│   ├── transport/
│   ├── runtime/
│   └── services/
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
    ├── test_swarm.cpp
    ├── test_event_loop.cpp
    ├── test_tcp_transport.cpp
    └── test_connection_session.cpp
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
| Types / Result | Partial (minimal `PeerId`, `Multiaddr`, `Result`) |
| PeerStore | Basic implementation |
| RoutingTable | Basic XOR nearest-peer table (not full Kademlia) |
| Node | Thin orchestration scaffold |
| Swarm | Scaffold only |
| ConnectionSession | Minimal direct-stream implementation |
| Event loop (kqueue/epoll) | Minimal implementation |
| TCP transport | Minimal implementation |
| multistream-select | Stub |
| Noise handshake | Stub |
| Yamux multiplexer | Stub |
| Identify service | Stub |
| Ping service | Stub |
| DHT service | Stub |

## Implementation Order

Per the architecture design, modules are implemented in this sequence:

1. Runtime (event loop) - minimal implementation present
2. TCP transport - minimal implementation present
3. ConnectionSession - minimal implementation present
4. multistream-select - pending
5. Noise - pending
6. Yamux - pending
7. Identify - pending
8. Ping - pending
9. Kad / DHT - pending
