#include "../include/peercore/swarm.hpp"

#include "protocol/multistream_select.hpp"
#include "runtime/event_loop.hpp"
#include "transport/tcp_transport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace peercore {

namespace {

std::string dial_failed_detail(const Multiaddr& addr, const std::string& detail) {
    return addr.to_string() + ": " + detail;
}

Result<uint64_t> decode_multistream_length(ConstBytes& buf) {
    uint64_t value = 0;
    uint32_t shift = 0;
    size_t index = 0;
    while (index < buf.size()) {
        const uint8_t byte = buf[index++];
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            buf = buf.subspan(index);
            return Result<uint64_t>::ok(value);
        }
        shift += 7;
        if (shift >= 64) {
            return Result<uint64_t>::err("invalid multistream length");
        }
    }
    return Result<uint64_t>::err("incomplete multistream length");
}

bool is_incomplete_multistream_error(std::string_view error) {
    return error == "incomplete multistream length" ||
           error == "incomplete multistream message";
}

Result<size_t> multistream_message_span(ConstBytes buf) {
    auto remaining = buf;
    auto len = decode_multistream_length(remaining);
    if (len.is_err()) return Result<size_t>::err(len.error().message);
    if (remaining.size() < len.value()) {
        return Result<size_t>::err("incomplete multistream message");
    }
    return Result<size_t>::ok(buf.size() - remaining.size() + len.value());
}

Result<size_t> multistream_exchange_span(ConstBytes buf) {
    auto first = multistream_message_span(buf);
    if (first.is_err()) return first;
    buf = buf.subspan(first.value());

    auto second = multistream_message_span(buf);
    if (second.is_err()) return second;
    return Result<size_t>::ok(first.value() + second.value());
}

class ProtocolNegotiatingStream final : public MuxedStream {
public:
    ProtocolNegotiatingStream(ConnectionId conn_id,
                              StreamHandle inner,
                              ProtocolId desired_protocol)
        : conn_id_(conn_id)
        , inner_(std::move(inner))
        , desired_protocol_(std::move(desired_protocol))
        , inbound_(false)
        , stage_(Stage::SendingOutboundProposal) {}

    ProtocolNegotiatingStream(ConnectionId conn_id,
                              StreamHandle inner,
                              std::vector<ProtocolId> supported_protocols)
        : conn_id_(conn_id)
        , inner_(std::move(inner))
        , supported_protocols_(std::move(supported_protocols))
        , inbound_(true)
        , stage_(Stage::AwaitingInboundProposal) {}

    StreamId id() const override { return inner_->id(); }
    ConnectionId connection_id() const override { return conn_id_; }

    Result<size_t> try_read(MutableBytes buf) override {
        auto progressed = poll();
        if (progressed.is_err()) {
            return Result<size_t>::err(progressed.error_message());
        }
        if (!is_negotiated()) {
            return Result<size_t>::err("stream protocol negotiation not complete");
        }

        if (!app_rx_buffer_.empty()) {
            const size_t n = std::min(buf.size(), app_rx_buffer_.size());
            std::copy_n(app_rx_buffer_.begin(), n, buf.begin());
            app_rx_buffer_.erase(app_rx_buffer_.begin(),
                                 app_rx_buffer_.begin() + static_cast<std::ptrdiff_t>(n));
            return Result<size_t>::ok(n);
        }

        return inner_->try_read(buf);
    }

    Result<size_t> try_write(ConstBytes data) override {
        auto progressed = poll();
        if (progressed.is_err()) {
            return Result<size_t>::err(progressed.error_message());
        }
        if (!is_negotiated()) {
            return Result<size_t>::err("stream protocol negotiation not complete");
        }
        return inner_->try_write(data);
    }

    Result<void> close_write() override {
        auto progressed = poll();
        if (progressed.is_err()) return progressed;
        if (!is_negotiated()) {
            return Result<void>::err("stream protocol negotiation not complete");
        }
        return inner_->close_write();
    }

    Result<void> reset() override {
        stage_ = Stage::Failed;
        if (!failure_detail_.has_value()) {
            failure_detail_ = "stream reset locally";
        }
        return inner_->reset();
    }

    bool is_open() const override {
        return inner_->is_open();
    }

    std::optional<ProtocolId> negotiated_protocol() const override {
        return negotiated_protocol_;
    }

    Result<void> poll() {
        if (stage_ == Stage::Ready) return Result<void>::ok();
        if (stage_ == Stage::Failed) {
            return Result<void>::err(failure_detail_.value_or("stream protocol negotiation failed"));
        }

        if (stage_ == Stage::SendingOutboundProposal) {
            auto proposal = protocol::MultistreamSelect::prepare_outbound({desired_protocol_});
            if (proposal.is_err()) {
                return fail(proposal.error().message);
            }
            auto wrote = inner_->try_write(proposal.value());
            if (wrote.is_err()) {
                return fail(wrote.error().message);
            }
            stage_ = Stage::AwaitingOutboundResponse;
        }

        auto incoming = read_into_negotiation_buffer();
        if (incoming.is_err()) {
            return fail(incoming.error_message());
        }

        if (stage_ == Stage::AwaitingOutboundResponse) {
            auto span = multistream_exchange_span(
                ConstBytes(negotiation_buffer_.data(), negotiation_buffer_.size()));
            if (span.is_err()) {
                if (is_incomplete_multistream_error(span.error().message)) {
                    return Result<void>::ok();
                }
                return fail(span.error().message);
            }

            ConstBytes response(negotiation_buffer_.data(), span.value());
            auto selected = protocol::MultistreamSelect::read_outbound_response(
                response, desired_protocol_);
            if (selected.is_err()) {
                return fail(selected.error().message);
            }

            negotiated_protocol_ = selected.value();
            finish_negotiation(span.value());
        } else if (stage_ == Stage::AwaitingInboundProposal) {
            auto span = multistream_exchange_span(
                ConstBytes(negotiation_buffer_.data(), negotiation_buffer_.size()));
            if (span.is_err()) {
                if (is_incomplete_multistream_error(span.error().message)) {
                    return Result<void>::ok();
                }
                return fail(span.error().message);
            }

            ConstBytes request(negotiation_buffer_.data(), span.value());
            auto negotiation = protocol::MultistreamSelect::negotiate_inbound(
                request, supported_protocols_);
            if (negotiation.is_err()) {
                return fail(negotiation.error().message);
            }

            auto wrote = inner_->try_write(negotiation.value().outbound);
            if (wrote.is_err()) {
                return fail(wrote.error().message);
            }

            if (!negotiation.value().protocol.has_value()) {
                (void)inner_->reset();
                return fail("protocol not supported");
            }

            negotiated_protocol_ = *negotiation.value().protocol;
            finish_negotiation(span.value());
        }

        return Result<void>::ok();
    }

    bool is_negotiated() const {
        return stage_ == Stage::Ready && negotiated_protocol_.has_value();
    }

    bool has_failed() const {
        return stage_ == Stage::Failed;
    }

    bool is_inbound() const {
        return inbound_;
    }

    std::string failure_detail() const {
        return failure_detail_.value_or("stream protocol negotiation failed");
    }

private:
    enum class Stage {
        SendingOutboundProposal,
        AwaitingOutboundResponse,
        AwaitingInboundProposal,
        Ready,
        Failed,
    };

    Result<void> read_into_negotiation_buffer() {
        std::array<uint8_t, 1024> buf{};
        while (true) {
            auto read = inner_->try_read(buf);
            if (read.is_err()) {
                if (read.error().message == "EAGAIN") return Result<void>::ok();
                return Result<void>::err(read.error().message);
            }
            if (read.value() == 0) {
                if (!inner_->is_open()) {
                    return Result<void>::err("stream closed during protocol negotiation");
                }
                return Result<void>::ok();
            }
            negotiation_buffer_.insert(negotiation_buffer_.end(),
                                       buf.begin(),
                                       buf.begin() + static_cast<std::ptrdiff_t>(read.value()));
        }
    }

    void finish_negotiation(size_t consumed) {
        if (negotiation_buffer_.size() > consumed) {
            app_rx_buffer_.insert(app_rx_buffer_.end(),
                                  negotiation_buffer_.begin() +
                                      static_cast<std::ptrdiff_t>(consumed),
                                  negotiation_buffer_.end());
        }
        negotiation_buffer_.clear();
        stage_ = Stage::Ready;
    }

    Result<void> fail(std::string detail) {
        stage_ = Stage::Failed;
        failure_detail_ = std::move(detail);
        return Result<void>::err(*failure_detail_);
    }

    ConnectionId conn_id_{0};
    StreamHandle inner_;
    ProtocolId desired_protocol_;
    std::vector<ProtocolId> supported_protocols_;
    bool inbound_{false};
    Stage stage_{Stage::SendingOutboundProposal};
    std::vector<uint8_t> negotiation_buffer_;
    std::vector<uint8_t> app_rx_buffer_;
    std::optional<ProtocolId> negotiated_protocol_;
    std::optional<std::string> failure_detail_;
};

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

    auto negotiated_stream = std::make_shared<ProtocolNegotiatingStream>(
        conn_id, stream.value(), proto);
    pending_streams_[stream_key(conn_id, negotiated_stream->id())] = negotiated_stream;

    std::vector<ConnectionId> closed_connections;
    drain_connection_events_for(conn_id, closed_connections);
    for (const auto closed_id : closed_connections) {
        remove_connection(closed_id);
    }
    poll_pending_streams();
    return Result<StreamHandle>::ok(std::move(negotiated_stream));
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
    poll_pending_streams();

    for (auto& h : handlers_) {
        h->on_tick();
    }

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

std::vector<ProtocolId> Swarm::supported_protocols() const {
    std::vector<ProtocolId> protocols;
    protocols.reserve(handlers_.size());
    for (const auto& handler : handlers_) {
        protocols.push_back(handler->protocol_id());
    }
    return protocols;
}

uint64_t Swarm::stream_key(ConnectionId conn_id, StreamId stream_id) const {
    return (static_cast<uint64_t>(conn_id) << 32) | stream_id;
}

void Swarm::poll_pending_streams() {
    std::vector<uint64_t> completed;
    std::vector<uint64_t> failed;

    for (auto& [key, handle] : pending_streams_) {
        auto negotiating = std::dynamic_pointer_cast<ProtocolNegotiatingStream>(handle);
        if (!negotiating) {
            completed.push_back(key);
            continue;
        }

        auto progress = negotiating->poll();
        if (progress.is_err() && !negotiating->has_failed()) {
            continue;
        }

        if (negotiating->has_failed()) {
            std::optional<PeerId> peer_id;
            if (auto peer_it = connection_peers_.find(negotiating->connection_id());
                peer_it != connection_peers_.end()) {
                auto parsed = PeerId::from_string(peer_it->second);
                if (parsed.is_ok()) peer_id = parsed.value();
            }
            dispatch_event(SwarmEvent{
                .type = SwarmEvent::Type::ProtocolError,
                .connection_id = negotiating->connection_id(),
                .stream_id = negotiating->id(),
                .peer_id = peer_id,
                .detail = negotiating->failure_detail(),
            });
            failed.push_back(key);
            continue;
        }

        if (!negotiating->is_negotiated()) continue;

        std::optional<PeerId> peer_id;
        if (auto peer_it = connection_peers_.find(negotiating->connection_id());
            peer_it != connection_peers_.end()) {
            auto parsed = PeerId::from_string(peer_it->second);
            if (parsed.is_ok()) peer_id = parsed.value();
        }

        dispatch_event(SwarmEvent{
            .type = SwarmEvent::Type::ProtocolNegotiated,
            .connection_id = negotiating->connection_id(),
            .stream_id = negotiating->id(),
            .peer_id = peer_id,
            .detail = *negotiating->negotiated_protocol(),
        });

        if (auto* handler = find_handler(*negotiating->negotiated_protocol())) {
            if (negotiating->is_inbound()) {
                handler->on_inbound_stream(handle);
            } else {
                handler->on_outbound_stream_ready(handle);
            }
        }

        completed.push_back(key);
    }

    for (const auto key : completed) {
        pending_streams_.erase(key);
    }
    for (const auto key : failed) {
        pending_streams_.erase(key);
    }
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
                const auto negotiated_muxer = it->second->negotiated_stream_muxer();
                if (negotiated_muxer.has_value()) {
                    dispatch_event(SwarmEvent{
                        .type = SwarmEvent::Type::ProtocolNegotiated,
                        .connection_id = id,
                        .stream_id = std::nullopt,
                        .peer_id = it->second->remote_peer(),
                        .detail = *negotiated_muxer,
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
            if (event.stream_id.has_value()) {
                auto conn_it = connections_.find(id);
                if (conn_it != connections_.end()) {
                    auto stream = conn_it->second->accept_inbound_stream();
                    if (stream.has_value()) {
                        auto negotiated_stream = std::make_shared<ProtocolNegotiatingStream>(
                            id, *stream, supported_protocols());
                        pending_streams_[stream_key(id, negotiated_stream->id())] =
                            negotiated_stream;
                    }
                }
            }
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
    for (auto it = pending_streams_.begin(); it != pending_streams_.end();) {
        const auto conn_id = static_cast<ConnectionId>(it->first >> 32);
        if (conn_id == id) {
            it = pending_streams_.erase(it);
            continue;
        }
        ++it;
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
