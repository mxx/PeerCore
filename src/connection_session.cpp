#include "../include/peercore/connection_session.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace peercore {

namespace {

struct SessionIoState {
    explicit SessionIoState(int raw_fd) : fd(raw_fd) {}

    int  fd{-1};
    bool socket_open{true};
    bool write_shutdown{false};
};

struct StreamState {
    StreamId                   id{0};
    ConnectionId               connection_id{0};
    std::optional<ProtocolId>  protocol;
    std::shared_ptr<SessionIoState> io;
    std::vector<uint8_t>       rx_buffer;
    std::vector<uint8_t>       tx_buffer;
    bool                       open{true};
    bool                       write_open{true};
    bool                       close_write_pending{false};
};

void close_socket(const std::shared_ptr<SessionIoState>& io) {
    if (!io || !io->socket_open || io->fd < 0) return;
    ::close(io->fd);
    io->fd = -1;
    io->socket_open = false;
    io->write_shutdown = true;
}

Result<void> flush_stream(StreamState& state) {
    if (!state.io || !state.io->socket_open || state.io->fd < 0) {
        state.open = false;
        return Result<void>::err("socket is closed");
    }

    while (!state.tx_buffer.empty()) {
        ssize_t n = ::send(state.io->fd,
                           state.tx_buffer.data(),
                           state.tx_buffer.size(),
                           0);
        if (n > 0) {
            state.tx_buffer.erase(state.tx_buffer.begin(),
                                  state.tx_buffer.begin() + n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        state.open = false;
        return Result<void>::err(std::strerror(errno));
    }

    if (state.close_write_pending &&
        state.io->socket_open &&
        !state.io->write_shutdown &&
        state.tx_buffer.empty()) {
        if (::shutdown(state.io->fd, SHUT_WR) < 0 && errno != ENOTCONN) {
            state.open = false;
            return Result<void>::err(std::strerror(errno));
        }
        state.io->write_shutdown = true;
    }

    return Result<void>::ok();
}

class DirectMuxedStream final : public MuxedStream {
public:
    explicit DirectMuxedStream(std::shared_ptr<StreamState> state)
        : state_(std::move(state)) {}

    StreamId id() const override { return state_->id; }
    ConnectionId connection_id() const override { return state_->connection_id; }

    Result<size_t> try_read(MutableBytes buf) override {
        if (!state_->open && state_->rx_buffer.empty()) {
            return Result<size_t>::ok(0);
        }

        const size_t n = std::min(buf.size(), state_->rx_buffer.size());
        std::copy_n(state_->rx_buffer.begin(), n, buf.begin());
        state_->rx_buffer.erase(state_->rx_buffer.begin(),
                                state_->rx_buffer.begin() + static_cast<std::ptrdiff_t>(n));
        return Result<size_t>::ok(n);
    }

    Result<size_t> try_write(ConstBytes data) override {
        if (!state_->open || !state_->write_open) {
            return Result<size_t>::err("stream is closed for writing");
        }

        state_->tx_buffer.insert(state_->tx_buffer.end(), data.begin(), data.end());
        auto flush = flush_stream(*state_);
        if (flush.is_err()) {
            return Result<size_t>::err(flush.error_message());
        }
        return Result<size_t>::ok(data.size());
    }

    Result<void> close_write() override {
        if (!state_->open) return Result<void>::err("stream is closed");
        state_->write_open = false;
        state_->close_write_pending = true;
        return flush_stream(*state_);
    }

    Result<void> reset() override {
        state_->open = false;
        state_->write_open = false;
        state_->close_write_pending = false;
        state_->rx_buffer.clear();
        state_->tx_buffer.clear();
        close_socket(state_->io);
        return Result<void>::ok();
    }

    bool is_open() const override {
        return state_->open && state_->io && state_->io->socket_open;
    }

    std::optional<ProtocolId> negotiated_protocol() const override {
        return state_->protocol;
    }

private:
    std::shared_ptr<StreamState> state_;
};

class BasicConnectionSession final : public ConnectionSession {
public:
    BasicConnectionSession(ConnectionId id, int socket_fd, Multiaddr remote_addr)
        : id_(id)
        , state_(ConnectionState::TcpAccepted)
        , remote_addr_(std::move(remote_addr))
        , io_(std::make_shared<SessionIoState>(socket_fd)) {}

    ~BasicConnectionSession() override { close(); }

    ConnectionId id() const override { return id_; }
    ConnectionState state() const override { return state_; }
    std::optional<PeerId> remote_peer() const override { return std::nullopt; }

    void on_socket_readable() override {
        if (!is_active()) return;

        std::array<uint8_t, 4096> buf{};
        while (true) {
            ssize_t n = ::recv(io_->fd, buf.data(), buf.size(), 0);
            if (n > 0) {
                auto stream = ensure_inbound_stream();
                stream->rx_buffer.insert(stream->rx_buffer.end(), buf.begin(), buf.begin() + n);
                continue;
            }
            if (n == 0) {
                close();
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            emit_error(std::string("recv failed: ") + std::strerror(errno));
            close();
            return;
        }
    }

    void on_socket_writable() override {
        if (!is_active() || !stream_state_) return;

        auto flush = flush_stream(*stream_state_);
        if (flush.is_err()) {
            emit_error(std::string("send failed: ") + flush.error_message());
            close();
        }
    }

    void on_timeout() override {
        if (!is_active()) return;
        if (state_ == ConnectionState::Ready) return;
        emit_error("connection timed out");
        close();
    }

    Result<void> begin_outbound_upgrade() override {
        if (!is_active()) return Result<void>::err("connection is closed");
        if (state_ == ConnectionState::Ready) return Result<void>::ok();
        return complete_upgrade_pipeline(/*create_inbound_stream=*/false);
    }

    Result<void> begin_inbound_upgrade() override {
        if (!is_active()) return Result<void>::err("connection is closed");
        if (state_ == ConnectionState::Ready) return Result<void>::ok();
        auto res = complete_upgrade_pipeline(/*create_inbound_stream=*/true);
        if (res.is_err()) return res;
        return Result<void>::ok();
    }

    Result<StreamHandle> request_open_stream(const ProtocolId& protocol) override {
        if (!is_active()) return Result<StreamHandle>::err("connection is closed");
        if (state_ != ConnectionState::Ready) {
            return Result<StreamHandle>::err("connection is not ready");
        }
        if (stream_state_ && stream_state_->open) {
            return Result<StreamHandle>::err("direct connection only supports one open stream");
        }

        auto handle = create_stream(protocol, /*inbound_event=*/false);
        return Result<StreamHandle>::ok(std::move(handle));
    }

    std::optional<StreamHandle> accept_inbound_stream() override {
        if (pending_inbound_streams_.empty()) return std::nullopt;
        auto stream = std::move(pending_inbound_streams_.front());
        pending_inbound_streams_.erase(pending_inbound_streams_.begin());
        return stream;
    }

    std::optional<ConnectionEvent> next_event() override {
        if (event_queue_.empty()) return std::nullopt;
        auto ev = std::move(event_queue_.front());
        event_queue_.erase(event_queue_.begin());
        return ev;
    }

    void close() override {
        if (state_ == ConnectionState::Closed) return;

        close_socket(io_);
        if (stream_state_) {
            stream_state_->open = false;
            stream_state_->write_open = false;
            stream_state_->close_write_pending = false;
        }

        state_ = ConnectionState::Closed;
        if (!closed_emitted_) {
            event_queue_.push_back(ConnectionEvent{
                .type = ConnectionEvent::Type::Closed,
                .stream_id = stream_state_ ? std::optional<StreamId>(stream_state_->id) : std::nullopt,
                .detail = "connection closed",
            });
            closed_emitted_ = true;
        }
    }

private:
    bool is_active() const {
        return io_ && io_->socket_open && state_ != ConnectionState::Closed &&
               state_ != ConnectionState::Failed;
    }

    Result<void> complete_upgrade_pipeline(bool create_inbound_stream) {
        state_ = ConnectionState::Securing;
        event_queue_.push_back(ConnectionEvent{
            .type = ConnectionEvent::Type::Secured,
            .stream_id = std::nullopt,
            .detail = "secure channel ready",
        });

        state_ = ConnectionState::Multiplexing;
        event_queue_.push_back(ConnectionEvent{
            .type = ConnectionEvent::Type::MultiplexerReady,
            .stream_id = std::nullopt,
            .detail = "single-stream fallback ready",
        });

        state_ = ConnectionState::Ready;
        if (create_inbound_stream && pending_inbound_streams_.empty()) {
            create_stream(std::nullopt, /*inbound_event=*/true);
        }
        return Result<void>::ok();
    }

    void emit_error(std::string detail) {
        if (state_ == ConnectionState::Failed || state_ == ConnectionState::Closed) return;
        state_ = ConnectionState::Failed;
        event_queue_.push_back(ConnectionEvent{
            .type = ConnectionEvent::Type::Error,
            .stream_id = stream_state_ ? std::optional<StreamId>(stream_state_->id) : std::nullopt,
            .detail = std::move(detail),
        });
    }

    std::shared_ptr<StreamState> ensure_inbound_stream() {
        if (stream_state_ && stream_state_->open) return stream_state_;
        create_stream(std::nullopt, /*inbound_event=*/true);
        return stream_state_;
    }

    StreamHandle create_stream(std::optional<ProtocolId> protocol, bool inbound_event) {
        stream_state_ = std::make_shared<StreamState>(StreamState{
            .id = next_stream_id_++,
            .connection_id = id_,
            .protocol = std::move(protocol),
            .io = io_,
        });

        auto handle = std::make_shared<DirectMuxedStream>(stream_state_);
        event_queue_.push_back(ConnectionEvent{
            .type = inbound_event ? ConnectionEvent::Type::StreamAccepted
                                  : ConnectionEvent::Type::StreamOpened,
            .stream_id = stream_state_->id,
            .detail = inbound_event ? "inbound stream ready"
                                    : "outbound stream opened",
        });
        if (inbound_event) {
            pending_inbound_streams_.push_back(handle);
        }
        return handle;
    }

    ConnectionId                id_{0};
    ConnectionState             state_{ConnectionState::TcpAccepted};
    Multiaddr                   remote_addr_;
    std::shared_ptr<SessionIoState> io_;
    std::shared_ptr<StreamState>    stream_state_;
    std::vector<ConnectionEvent>    event_queue_;
    std::vector<StreamHandle>       pending_inbound_streams_;
    StreamId                    next_stream_id_{1};
    bool                        closed_emitted_{false};
};

}  // namespace

std::unique_ptr<ConnectionSession> make_outbound_connection_session(
    ConnectionId id,
    int socket_fd,
    Multiaddr remote_addr) {
    return std::make_unique<BasicConnectionSession>(id, socket_fd, std::move(remote_addr));
}

std::unique_ptr<ConnectionSession> make_inbound_connection_session(
    ConnectionId id,
    int socket_fd,
    Multiaddr remote_addr) {
    return std::make_unique<BasicConnectionSession>(id, socket_fd, std::move(remote_addr));
}

}  // namespace peercore
