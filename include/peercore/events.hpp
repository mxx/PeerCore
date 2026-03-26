#pragma once

#include "types.hpp"

#include <optional>
#include <string>

namespace peercore {

// ── SwarmEvent ────────────────────────────────────────────────────────────────

struct SwarmEvent {
    enum class Type {
        ListenerStarted,
        IncomingConnection,
        ConnectionEstablished,
        ConnectionClosed,
        DialFailed,
        StreamOpened,
        StreamAccepted,
        StreamClosed,
        ProtocolNegotiated,
        ProtocolError,
        PeerIdentified,
    };

    Type                    type;
    ConnectionId            connection_id{0};
    std::optional<StreamId> stream_id;
    std::optional<PeerId>   peer_id;
    std::string             detail;
};

// ── ConnectionEvent ───────────────────────────────────────────────────────────

struct ConnectionEvent {
    enum class Type {
        Secured,
        MultiplexerReady,
        StreamOpened,
        StreamAccepted,
        StreamClosed,
        Error,
        Closed,
    };

    Type                    type;
    std::optional<StreamId> stream_id;
    std::string             detail;
};

// ── Action (Controller → Swarm) ───────────────────────────────────────────────

struct Action {
    enum class Type {
        DialAddr,
        DialPeer,
        CloseConnection,
        OpenStream,
        SendPing,
    };

    Type                     type;
    std::optional<Multiaddr> addr;
    std::optional<PeerId>    peer_id;
    std::optional<ConnectionId> connection_id;
    std::optional<ProtocolId>   protocol_id;
};

}  // namespace peercore
