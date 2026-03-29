#pragma once

#include "events.hpp"
#include "muxed_stream.hpp"
#include "types.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace peercore {

enum class ConnectionState {
    TcpConnecting,
    TcpAccepted,
    Securing,
    SecureReady,
    Multiplexing,
    Ready,
    Closing,
    Closed,
    Failed,
};

class ConnectionSession {
public:
    virtual ~ConnectionSession() = default;

    virtual ConnectionId    id()          const = 0;
    virtual ConnectionState state()       const = 0;
    virtual std::optional<PeerId> remote_peer() const = 0;
    virtual std::vector<ProtocolId> remote_stream_muxers() const = 0;
    virtual std::optional<ProtocolId> negotiated_stream_muxer() const = 0;

    // Socket readiness callbacks (called by the event loop)
    virtual void on_socket_readable() = 0;
    virtual void on_socket_writable() = 0;
    virtual void on_timeout()         = 0;

    // Upgrade to secure + multiplexed channel
    virtual Result<void> begin_outbound_upgrade() = 0;
    virtual Result<void> begin_inbound_upgrade()  = 0;

    // Stream operations
    virtual Result<StreamHandle>         request_open_stream(const ProtocolId&) = 0;
    virtual std::optional<StreamHandle>  accept_inbound_stream()                = 0;

    // Event queue drained by Swarm
    virtual std::optional<ConnectionEvent> next_event() = 0;

    virtual void close() = 0;
};

std::unique_ptr<ConnectionSession> make_outbound_connection_session(
    ConnectionId id,
    int socket_fd,
    Multiaddr remote_addr,
    std::optional<Identity> local_identity = std::nullopt,
    std::vector<ProtocolId> local_stream_muxers = {});

std::unique_ptr<ConnectionSession> make_inbound_connection_session(
    ConnectionId id,
    int socket_fd,
    Multiaddr remote_addr,
    std::optional<Identity> local_identity = std::nullopt,
    std::vector<ProtocolId> local_stream_muxers = {});

}  // namespace peercore
