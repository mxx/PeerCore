#include "../include/peercore/swarm.hpp"

#include "runtime/event_loop.hpp"
#include "transport/tcp_transport.hpp"

#include <algorithm>
#include <utility>

namespace peercore {

namespace {

std::string dial_failed_detail(const Multiaddr& addr, const std::string& detail) {
    return addr.to_string() + ": " + detail;
}

std::string join_protocols(const std::vector<ProtocolId>& protocols) {
    std::string out;
    for (size_t i = 0; i < protocols.size(); ++i) {
        if (i > 0) out += ", ";
        out += protocols[i];
    }
    return out;
}

}  // namespace

Swarm::Swarm(PeerStore& peer_store,
             std::optional<Identity> local_identity,
             std::vector<ProtocolId> local_stream_muxers)
    : peer_store_(peer_store)
    , local_identity_(std::move(local_identity))
    , local_stream_muxers_(std::move(local_stream_muxers))
    , controller_(std::make_shared<DefaultController>())
    , event_loop_(std::make_unique<runtime::EventLoop>())
    , transport_(std::make_unique<transport::TcpTransport>()) {}

Swarm::~Swarm() { stop(); }

Result<void> Swarm::start() {
    sync_transport_fds();
    return Result<void>::ok();
}

Result<void> Swarm::stop() {
    for (const auto fd : listener_fds_) {
        event_loop_->unregister_fd(fd);
    }
    listener_fds_.clear();

    for (const auto fd : dialing_fds_) {
        event_loop_->unregister_fd(fd);
    }
    dialing_fds_.clear();

    for (const auto& [id, fd] : conn_fds_) {
        (void)id;
        event_loop_->unregister_fd(fd);
    }
    fd_to_conn_.clear();
    conn_fds_.clear();
    peer_connections_.clear();
    connection_peers_.clear();

    for (auto& [id, conn] : connections_) {
        (void)id;
        conn->close();
    }
    connections_.clear();

    transport_->close_all();
    event_loop_ = std::make_unique<runtime::EventLoop>();
    transport_ = std::make_unique<transport::TcpTransport>();
    return Result<void>::ok();
}

Result<void> Swarm::listen_on(const Multiaddr& addr) {
    const auto before = transport_->listener_fds();
    auto res = transport_->listen(
        addr,
        transport::TcpTransportCallbacks{
            .on_accepted = [this](transport::TcpSocket socket) {
                const ConnectionId id = next_conn_id_++;
                auto session = make_inbound_connection_session(
                    id, socket.fd, socket.remote_addr, local_identity_, local_stream_muxers_);
                connections_[id] = std::move(session);
                register_connection(id, socket.fd);

                dispatch_event(SwarmEvent{
                    .type = SwarmEvent::Type::IncomingConnection,
                    .connection_id = id,
                    .stream_id = std::nullopt,
                    .peer_id = std::nullopt,
                    .detail = socket.remote_addr.to_string(),
                });

                auto upgrade = connections_[id]->begin_inbound_upgrade();
                if (upgrade.is_err()) {
                    dispatch_event(SwarmEvent{
                        .type = SwarmEvent::Type::ProtocolError,
                        .connection_id = id,
                        .stream_id = std::nullopt,
                        .peer_id = std::nullopt,
                        .detail = upgrade.error_message(),
                    });
                    connections_[id]->close();
                }

                std::vector<ConnectionId> closed_connections;
                drain_connection_events_for(id, closed_connections);
                for (const auto closed_id : closed_connections) {
                    remove_connection(closed_id);
                }
            },
        });
    if (res.is_err()) return res;

    sync_transport_fds();

    const auto after = transport_->listener_fds();
    for (const auto fd : after) {
        if (std::find(before.begin(), before.end(), fd) != before.end()) continue;

        std::string detail = addr.to_string();
        auto local_addr = transport_->local_addr(fd);
        if (local_addr.is_ok()) detail = local_addr.value().to_string();

        dispatch_event(SwarmEvent{
            .type = SwarmEvent::Type::ListenerStarted,
            .connection_id = 0,
            .stream_id = std::nullopt,
            .peer_id = std::nullopt,
            .detail = std::move(detail),
        });
    }

    return Result<void>::ok();
}

Result<void> Swarm::dial_addr(const Multiaddr& addr) {
    auto res = transport_->dial(
        addr,
        transport::TcpTransportCallbacks{
            .on_connected = [this](transport::TcpSocket socket) {
                const ConnectionId id = next_conn_id_++;
                auto session = make_outbound_connection_session(
                    id, socket.fd, socket.remote_addr, local_identity_, local_stream_muxers_);
                connections_[id] = std::move(session);
                register_connection(id, socket.fd);

                auto upgrade = connections_[id]->begin_outbound_upgrade();
                if (upgrade.is_err()) {
                    dispatch_event(SwarmEvent{
                        .type = SwarmEvent::Type::ProtocolError,
                        .connection_id = id,
                        .stream_id = std::nullopt,
                        .peer_id = std::nullopt,
                        .detail = upgrade.error_message(),
                    });
                    connections_[id]->close();
                }

                std::vector<ConnectionId> closed_connections;
                drain_connection_events_for(id, closed_connections);
                for (const auto closed_id : closed_connections) {
                    remove_connection(closed_id);
                }
            },
            .on_dial_failed = [this](const Multiaddr& failed_addr, std::string detail) {
                dispatch_event(SwarmEvent{
                    .type = SwarmEvent::Type::DialFailed,
                    .connection_id = 0,
                    .stream_id = std::nullopt,
                    .peer_id = std::nullopt,
                    .detail = dial_failed_detail(failed_addr, detail),
                });
            },
        });
    if (res.is_err()) return res;

    sync_transport_fds();
    return Result<void>::ok();
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

    auto stream = it->second->request_open_stream(proto);
    if (stream.is_err()) return stream;

    std::vector<ConnectionId> closed_connections;
    drain_connection_events_for(conn_id, closed_connections);
    for (const auto closed_id : closed_connections) {
        remove_connection(closed_id);
    }
    return stream;
}

Result<StreamHandle> Swarm::open_stream(const PeerId& peer, ProtocolId proto) {
    const auto it = peer_connections_.find(peer.to_string());
    if (it == peer_connections_.end()) {
        return Result<StreamHandle>::err("no active connection for peer");
    }
    return open_stream(it->second, std::move(proto));
}

void Swarm::register_handler(std::shared_ptr<ProtocolHandler> handler) {
    handlers_.push_back(std::move(handler));
}

void Swarm::set_controller(std::shared_ptr<Controller> ctrl) {
    controller_ = std::move(ctrl);
}

void Swarm::poll_once() {
    sync_transport_fds();
    event_loop_->poll_once();
    sync_transport_fds();
    drain_connection_events();

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
        .stream_count = 0,
        .extra = {},
    };
}

void Swarm::dispatch_event(SwarmEvent event) {
    for (auto& h : handlers_) {
        h->on_tick();
    }
    controller_->on_swarm_event(event);
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
                if (it != connections_.end()) {
                    it->second->close();
                    drain_connection_events();
                }
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

void Swarm::sync_transport_fds() {
    const auto listener_list = transport_->listener_fds();
    std::unordered_set<int> current_listeners(listener_list.begin(), listener_list.end());
    for (const auto fd : current_listeners) {
        if (listener_fds_.contains(fd)) continue;
        event_loop_->register_fd(fd, [this, fd](runtime::IoEvent event) {
            if (event == runtime::IoEvent::Readable || event == runtime::IoEvent::Error) {
                transport_->on_accept_ready(fd);
            }
        });
        listener_fds_.insert(fd);
    }

    for (auto it = listener_fds_.begin(); it != listener_fds_.end();) {
        if (current_listeners.contains(*it)) {
            ++it;
            continue;
        }
        event_loop_->unregister_fd(*it);
        it = listener_fds_.erase(it);
    }

    const auto dialing_list = transport_->dialing_fds();
    std::unordered_set<int> current_dials(dialing_list.begin(), dialing_list.end());
    for (const auto fd : current_dials) {
        if (dialing_fds_.contains(fd) || fd_to_conn_.contains(fd)) continue;
        event_loop_->register_fd(fd, [this, fd](runtime::IoEvent event) {
            if (event == runtime::IoEvent::Writable || event == runtime::IoEvent::Error) {
                transport_->on_connect_ready(fd);
            }
        });
        dialing_fds_.insert(fd);
    }

    for (auto it = dialing_fds_.begin(); it != dialing_fds_.end();) {
        if (current_dials.contains(*it)) {
            ++it;
            continue;
        }
        event_loop_->unregister_fd(*it);
        it = dialing_fds_.erase(it);
    }
}

void Swarm::register_connection(ConnectionId id, int fd) {
    if (dialing_fds_.contains(fd)) {
        event_loop_->unregister_fd(fd);
        dialing_fds_.erase(fd);
    }

    conn_fds_[id] = fd;
    fd_to_conn_[fd] = id;
    event_loop_->register_fd(fd, [this, fd](runtime::IoEvent event) {
        auto fd_it = fd_to_conn_.find(fd);
        if (fd_it == fd_to_conn_.end()) return;

        auto conn_it = connections_.find(fd_it->second);
        if (conn_it == connections_.end()) return;

        switch (event) {
            case runtime::IoEvent::Readable:
                conn_it->second->on_socket_readable();
                break;
            case runtime::IoEvent::Writable:
                conn_it->second->on_socket_writable();
                break;
            case runtime::IoEvent::Error:
                conn_it->second->close();
                break;
        }
    });
}

void Swarm::drain_connection_events() {
    std::vector<ConnectionId> closed_connections;
    for (const auto& [id, conn] : connections_) {
        (void)conn;
        drain_connection_events_for(id, closed_connections);
    }
    for (const auto id : closed_connections) {
        remove_connection(id);
    }
}

void Swarm::drain_connection_events_for(ConnectionId id,
                                        std::vector<ConnectionId>& closed_connections) {
    auto it = connections_.find(id);
    if (it == connections_.end()) return;

    while (auto ev = it->second->next_event()) {
        handle_connection_event(id, std::move(*ev), closed_connections);
    }
}

void Swarm::handle_connection_event(ConnectionId id,
                                    ConnectionEvent event,
                                    std::vector<ConnectionId>& closed_connections) {
    switch (event.type) {
        case ConnectionEvent::Type::Secured:
            if (auto it = connections_.find(id);
                it != connections_.end() && it->second->remote_peer().has_value()) {
                const auto peer_key = it->second->remote_peer()->to_string();
                peer_connections_[peer_key] = id;
                connection_peers_[id] = peer_key;
                dispatch_event(SwarmEvent{
                    .type = SwarmEvent::Type::PeerIdentified,
                    .connection_id = id,
                    .stream_id = std::nullopt,
                    .peer_id = it->second->remote_peer(),
                    .detail = event.detail,
                });
                const auto remote_muxers = it->second->remote_stream_muxers();
                if (!remote_muxers.empty()) {
                    dispatch_event(SwarmEvent{
                        .type = SwarmEvent::Type::ProtocolNegotiated,
                        .connection_id = id,
                        .stream_id = std::nullopt,
                        .peer_id = it->second->remote_peer(),
                        .detail = join_protocols(remote_muxers),
                    });
                }
            }
            break;
        case ConnectionEvent::Type::MultiplexerReady:
            {
                std::optional<PeerId> peer_id;
                if (auto it = connections_.find(id); it != connections_.end()) {
                    peer_id = it->second->remote_peer();
                    if (peer_id.has_value()) {
                        peer_store_.record_dial_success(*peer_id);
                    }
                }
            dispatch_event(SwarmEvent{
                .type = SwarmEvent::Type::ConnectionEstablished,
                .connection_id = id,
                .stream_id = std::nullopt,
                .peer_id = peer_id,
                .detail = event.detail,
            });
            }
            break;
        case ConnectionEvent::Type::StreamOpened:
            dispatch_event(SwarmEvent{
                .type = SwarmEvent::Type::StreamOpened,
                .connection_id = id,
                .stream_id = event.stream_id,
                .peer_id = std::nullopt,
                .detail = event.detail,
            });
            break;
        case ConnectionEvent::Type::StreamAccepted:
            dispatch_event(SwarmEvent{
                .type = SwarmEvent::Type::StreamAccepted,
                .connection_id = id,
                .stream_id = event.stream_id,
                .peer_id = std::nullopt,
                .detail = event.detail,
            });
            break;
        case ConnectionEvent::Type::StreamClosed:
            dispatch_event(SwarmEvent{
                .type = SwarmEvent::Type::StreamClosed,
                .connection_id = id,
                .stream_id = event.stream_id,
                .peer_id = std::nullopt,
                .detail = event.detail,
            });
            break;
        case ConnectionEvent::Type::Error:
            dispatch_event(SwarmEvent{
                .type = SwarmEvent::Type::ProtocolError,
                .connection_id = id,
                .stream_id = event.stream_id,
                .peer_id = std::nullopt,
                .detail = event.detail,
            });
            break;
        case ConnectionEvent::Type::Closed:
            dispatch_event(SwarmEvent{
                .type = SwarmEvent::Type::ConnectionClosed,
                .connection_id = id,
                .stream_id = event.stream_id,
                .peer_id = std::nullopt,
                .detail = event.detail,
            });
            closed_connections.push_back(id);
            break;
    }
}

void Swarm::remove_connection(ConnectionId id) {
    if (auto peer_it = connection_peers_.find(id); peer_it != connection_peers_.end()) {
        peer_connections_.erase(peer_it->second);
        connection_peers_.erase(peer_it);
    }
    auto fd_it = conn_fds_.find(id);
    if (fd_it != conn_fds_.end()) {
        event_loop_->unregister_fd(fd_it->second);
        fd_to_conn_.erase(fd_it->second);
        conn_fds_.erase(fd_it);
    }
    connections_.erase(id);
}

}  // namespace peercore
