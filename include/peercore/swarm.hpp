#pragma once

#include "connection_session.hpp"
#include "controller.hpp"
#include "events.hpp"
#include "peer_store.hpp"
#include "protocol_handler.hpp"
#include "types.hpp"

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace peercore {

namespace runtime {
class EventLoop;
}

namespace transport {
class TcpTransport;
}

class Swarm {
public:
    explicit Swarm(PeerStore& peer_store,
                   std::optional<Identity> local_identity = std::nullopt,
                   std::vector<ProtocolId> local_stream_muxers = {});
    ~Swarm();

    // Lifecycle
    Result<void> start();
    Result<void> stop();

    // Transport
    Result<void> listen_on(const Multiaddr& addr);
    Result<void> dial_addr(const Multiaddr& addr);
    Result<void> dial_peer(const PeerId& peer);

    // Streams
    Result<StreamHandle> open_stream(ConnectionId conn_id, ProtocolId proto);
    Result<StreamHandle> open_stream(const PeerId& peer, ProtocolId proto);

    // Protocol registration
    void register_handler(std::shared_ptr<ProtocolHandler> handler);

    // Controller (optional strategy override)
    void set_controller(std::shared_ptr<Controller> ctrl);

    // Event loop
    void poll_once();
    std::optional<SwarmEvent> next_event();

    DebugSnapshot snapshot() const;

private:
    PeerStore& peer_store_;
    std::optional<Identity> local_identity_;
    std::vector<ProtocolId> local_stream_muxers_;
    std::shared_ptr<Controller> controller_;
    std::unique_ptr<runtime::EventLoop> event_loop_;
    std::unique_ptr<transport::TcpTransport> transport_;

    std::unordered_map<ConnectionId, std::unique_ptr<ConnectionSession>> connections_;
    std::unordered_map<ConnectionId, int> conn_fds_;
    std::unordered_map<int, ConnectionId> fd_to_conn_;
    std::unordered_map<std::string, ConnectionId> peer_connections_;
    std::unordered_map<ConnectionId, std::string> connection_peers_;
    std::unordered_map<uint64_t, StreamHandle> pending_streams_;
    std::unordered_set<int> listener_fds_;
    std::unordered_set<int> dialing_fds_;
    std::vector<std::shared_ptr<ProtocolHandler>> handlers_;
    std::vector<SwarmEvent> event_queue_;

    ConnectionId next_conn_id_{1};

    void dispatch_event(SwarmEvent event);
    void apply_action(const Action& action);
    ProtocolHandler* find_handler(const ProtocolId& proto);
    std::vector<ProtocolId> supported_protocols() const;
    uint64_t stream_key(ConnectionId conn_id, StreamId stream_id) const;
    void poll_pending_streams();
    void sync_transport_fds();
    void register_connection(ConnectionId id, int fd);
    void drain_connection_events();
    void drain_connection_events_for(ConnectionId id,
                                     std::vector<ConnectionId>& closed_connections);
    void handle_connection_event(ConnectionId id,
                                 ConnectionEvent event,
                                 std::vector<ConnectionId>& closed_connections);
    void remove_connection(ConnectionId id);
};

}  // namespace peercore
