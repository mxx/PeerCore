#include "../include/peercore/swarm.hpp"

namespace peercore {

Swarm::Swarm(PeerStore& peer_store)
    : peer_store_(peer_store)
    , controller_(std::make_shared<DefaultController>()) {}

Swarm::~Swarm() { stop(); }

Result<void> Swarm::start() {
    // TODO: start event loop thread / integrate with runtime::EventLoop
    return Result<void>::ok();
}

Result<void> Swarm::stop() {
    for (auto& [id, conn] : connections_) {
        conn->close();
    }
    connections_.clear();
    return Result<void>::ok();
}

Result<void> Swarm::listen_on(const Multiaddr& /*addr*/) {
    return Result<void>::err("Swarm::listen_on not implemented");
}

Result<void> Swarm::dial_addr(const Multiaddr& /*addr*/) {
    return Result<void>::err("Swarm::dial_addr not implemented");
}

Result<void> Swarm::dial_peer(const PeerId& peer) {
    auto addrs = peer_store_.get_addrs(peer);
    if (addrs.empty()) {
        return Result<void>::err("no known addresses for peer");
    }
    return dial_addr(addrs.front());
}

Result<StreamHandle> Swarm::open_stream(ConnectionId conn_id, ProtocolId proto) {
    auto it = connections_.find(conn_id);
    if (it == connections_.end()) {
        return Result<StreamHandle>::err("unknown connection");
    }
    return it->second->request_open_stream(proto);
}

void Swarm::register_handler(std::shared_ptr<ProtocolHandler> handler) {
    handlers_.push_back(std::move(handler));
}

void Swarm::set_controller(std::shared_ptr<Controller> ctrl) {
    controller_ = std::move(ctrl);
}

void Swarm::poll_once() {
    // 1. Drive each connection's I/O state machine
    for (auto& [id, conn] : connections_) {
        while (auto ev = conn->next_event()) {
            // Translate ConnectionEvent → SwarmEvent
            (void)ev;
            // TODO: map and dispatch
        }
    }

    // 2. Let controller react and collect actions
    controller_->on_timer_tick();
    for (const auto& action : controller_->drain_actions()) {
        apply_action(action);
    }
}

std::optional<SwarmEvent> Swarm::next_event() {
    if (event_queue_.empty()) return std::nullopt;
    auto ev = std::move(event_queue_.front());
    event_queue_.erase(event_queue_.begin());
    return ev;
}

DebugSnapshot Swarm::snapshot() const {
    return DebugSnapshot{
        .connection_count = static_cast<uint32_t>(connections_.size()),
        .stream_count     = 0,  // TODO: sum streams across connections
        .extra            = {},
    };
}

void Swarm::dispatch_event(SwarmEvent event) {
    // Notify protocol handlers
    for (auto& h : handlers_) {
        h->on_tick();
    }
    // Notify controller
    controller_->on_swarm_event(event);
    // Enqueue for user polling
    event_queue_.push_back(std::move(event));
}

void Swarm::apply_action(const Action& action) {
    switch (action.type) {
        case Action::Type::DialAddr:
            if (action.addr) dial_addr(*action.addr);
            break;
        case Action::Type::DialPeer:
            if (action.peer_id) dial_peer(*action.peer_id);
            break;
        case Action::Type::CloseConnection:
            if (action.connection_id) {
                auto it = connections_.find(*action.connection_id);
                if (it != connections_.end()) it->second->close();
            }
            break;
        case Action::Type::OpenStream:
            if (action.connection_id && action.protocol_id) {
                open_stream(*action.connection_id, *action.protocol_id);
            }
            break;
        default:
            break;
    }
}

ProtocolHandler* Swarm::find_handler(const ProtocolId& proto) {
    for (auto& h : handlers_) {
        if (h->protocol_id() == proto) return h.get();
    }
    return nullptr;
}

}  // namespace peercore
