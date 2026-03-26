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
#include <vector>

namespace peercore {

class Swarm {
public:
    explicit Swarm(PeerStore& peer_store);
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
    std::shared_ptr<Controller> controller_;

    std::unordered_map<ConnectionId, std::unique_ptr<ConnectionSession>> connections_;
    std::vector<std::shared_ptr<ProtocolHandler>> handlers_;
    std::vector<SwarmEvent> event_queue_;

    [[maybe_unused]] ConnectionId next_conn_id_{1};

    void dispatch_event(SwarmEvent event);
    void apply_action(const Action& action);
    ProtocolHandler* find_handler(const ProtocolId& proto);
};

}  // namespace peercore
