#include "../include/peercore/connection_session.hpp"

#include "protocol/multistream_select.hpp"
#include "protocol/noise/noise.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace peercore {

namespace {

using protocol::noise::NoiseHandshake;
using protocol::noise::NoiseSession;
using protocol::MultistreamSelect;

enum class HandshakeState {
    Idle,
    AwaitingMsg1,
    AwaitingMsg2,
    AwaitingMsg3,
    Complete,
};

enum class SecurityStage {
    NegotiatingProtocol,
    NoiseHandshake,
    Ready,
};

struct SessionIoState {
    explicit SessionIoState(int raw_fd) : fd(raw_fd) {}

    int                  fd{-1};
    bool                 socket_open{true};
    bool                 write_shutdown{false};
    std::vector<uint8_t> wire_rx_buffer;
    std::vector<uint8_t> wire_tx_buffer;
};

struct SecureChannelState {
    NoiseSession   noise;
    SecurityStage  stage{SecurityStage::NegotiatingProtocol};
    HandshakeState handshake_state{HandshakeState::Idle};
    bool            secure_ready{false};
};

struct StreamState {
    StreamId                         id{0};
    ConnectionId                     connection_id{0};
    std::optional<ProtocolId>        protocol;
    std::shared_ptr<SessionIoState>  io;
    std::shared_ptr<SecureChannelState> secure;
    std::vector<uint8_t>             rx_buffer;
    bool                             open{true};
    bool                             write_open{true};
    bool                             close_write_pending{false};
};

void close_socket(const std::shared_ptr<SessionIoState>& io) {
    if (!io || !io->socket_open || io->fd < 0) return;
    ::close(io->fd);
    io->fd = -1;
    io->socket_open = false;
    io->write_shutdown = true;
    io->wire_rx_buffer.clear();
    io->wire_tx_buffer.clear();
}

Result<void> queue_wire_message(SessionIoState& io, ConstBytes message) {
    auto framed = NoiseHandshake::encode_frame(message);
    if (framed.is_err()) return Result<void>::err(framed.error().message);
    io.wire_tx_buffer.insert(io.wire_tx_buffer.end(),
                             framed.value().begin(),
                             framed.value().end());
    return Result<void>::ok();
}

Result<void> queue_raw_bytes(SessionIoState& io, ConstBytes bytes) {
    io.wire_tx_buffer.insert(io.wire_tx_buffer.end(), bytes.begin(), bytes.end());
    return Result<void>::ok();
}

Result<void> flush_wire(SessionIoState& io) {
    if (!io.socket_open || io.fd < 0) {
        return Result<void>::err("socket is closed");
    }

    while (!io.wire_tx_buffer.empty()) {
        ssize_t n = ::send(io.fd, io.wire_tx_buffer.data(), io.wire_tx_buffer.size(), 0);
        if (n > 0) {
            io.wire_tx_buffer.erase(io.wire_tx_buffer.begin(),
                                    io.wire_tx_buffer.begin() + n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        return Result<void>::err(std::strerror(errno));
    }

    return Result<void>::ok();
}

Result<void> flush_stream(StreamState& state) {
    if (!state.io || !state.io->socket_open || state.io->fd < 0) {
        state.open = false;
        return Result<void>::err("socket is closed");
    }

    auto flush = flush_wire(*state.io);
    if (flush.is_err()) {
        state.open = false;
        return flush;
    }

    if (state.close_write_pending &&
        state.io->socket_open &&
        !state.io->write_shutdown &&
        state.io->wire_tx_buffer.empty()) {
        if (::shutdown(state.io->fd, SHUT_WR) < 0 && errno != ENOTCONN) {
            state.open = false;
            return Result<void>::err(std::strerror(errno));
        }
        state.io->write_shutdown = true;
    }

    return Result<void>::ok();
}

bool try_pop_wire_frame(std::vector<uint8_t>& buffer,
                        std::vector<uint8_t>& frame,
                        std::string& error) {
    if (buffer.size() < 2) return false;

    const uint16_t len = static_cast<uint16_t>((static_cast<uint16_t>(buffer[0]) << 8) | buffer[1]);
    const size_t total = static_cast<size_t>(len) + 2;
    if (buffer.size() < total) return false;

    ConstBytes framed(buffer.data(), total);
    auto decoded = NoiseHandshake::decode_frame(framed);
    if (decoded.is_err()) {
        error = decoded.error().message;
        return false;
    }

    frame = std::move(decoded.value());
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(total));
    return true;
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

Result<size_t> queue_encrypted_write(StreamState& state, ConstBytes data) {
    if (!state.secure || !state.secure->secure_ready) {
        return Result<size_t>::err("secure channel is not ready");
    }

    auto ciphertext = NoiseHandshake::encrypt(state.secure->noise.cs_send, data);
    if (ciphertext.is_err()) {
        state.open = false;
        return Result<size_t>::err(ciphertext.error().message);
    }

    auto queued = queue_wire_message(*state.io, ciphertext.value());
    if (queued.is_err()) {
        state.open = false;
        return Result<size_t>::err(queued.error_message());
    }

    auto flush = flush_stream(state);
    if (flush.is_err()) {
        return Result<size_t>::err(flush.error_message());
    }
    return Result<size_t>::ok(data.size());
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

        return queue_encrypted_write(*state_, data);
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
    BasicConnectionSession(ConnectionId id,
                           int socket_fd,
                           Multiaddr remote_addr,
                           bool is_initiator,
                           std::optional<Identity> local_identity)
        : id_(id)
        , state_(ConnectionState::TcpAccepted)
        , remote_addr_(std::move(remote_addr))
        , io_(std::make_shared<SessionIoState>(socket_fd))
        , secure_(std::make_shared<SecureChannelState>())
        , is_initiator_(is_initiator) {
        secure_->noise.local_identity = std::move(local_identity);
    }

    ~BasicConnectionSession() override { close(); }

    ConnectionId id() const override { return id_; }
    ConnectionState state() const override { return state_; }
    std::optional<PeerId> remote_peer() const override { return secure_->noise.remote_peer_id; }

    void on_socket_readable() override {
        if (!is_active()) return;

        std::array<uint8_t, 4096> buf{};
        while (true) {
            ssize_t n = ::recv(io_->fd, buf.data(), buf.size(), 0);
            if (n > 0) {
                io_->wire_rx_buffer.insert(io_->wire_rx_buffer.end(), buf.begin(), buf.begin() + n);
                continue;
            }
            if (n == 0) {
                close();
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            emit_error(std::string("recv failed: ") + std::strerror(errno));
            close();
            return;
        }

        auto drain = process_pending_input();
        if (drain.is_err()) {
            emit_error(drain.error_message());
            close();
        }
    }

    void on_socket_writable() override {
        if (!is_active()) return;

        auto flush = flush_pending_writes();
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
        if (state_ == ConnectionState::Ready || secure_->secure_ready) return Result<void>::ok();
        if (!is_initiator_) return Result<void>::err("connection is not outbound");

        state_ = ConnectionState::Securing;
        create_inbound_stream_on_ready_ = false;
        secure_->stage = SecurityStage::NegotiatingProtocol;
        secure_->handshake_state = HandshakeState::Idle;

        auto proposal = MultistreamSelect::prepare_outbound({"/noise"});
        if (proposal.is_err()) return Result<void>::err(proposal.error().message);
        auto queued = queue_raw_bytes(*io_, proposal.value());
        if (queued.is_err()) return queued;
        return flush_pending_writes();
    }

    Result<void> begin_inbound_upgrade() override {
        if (!is_active()) return Result<void>::err("connection is closed");
        if (state_ == ConnectionState::Ready || secure_->secure_ready) return Result<void>::ok();
        if (is_initiator_) return Result<void>::err("connection is not inbound");

        state_ = ConnectionState::Securing;
        create_inbound_stream_on_ready_ = true;
        secure_->stage = SecurityStage::NegotiatingProtocol;
        secure_->handshake_state = HandshakeState::Idle;
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
            stream_state_->rx_buffer.clear();
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

    Result<void> flush_pending_writes() {
        auto flush = flush_wire(*io_);
        if (flush.is_err()) return flush;
        if (stream_state_ && stream_state_->close_write_pending) {
            return flush_stream(*stream_state_);
        }
        return Result<void>::ok();
    }

    Result<void> process_pending_input() {
        if (secure_->stage == SecurityStage::NegotiatingProtocol) {
            auto protocol = process_protocol_negotiation();
            if (protocol.is_err()) return protocol;
        }

        if (!secure_->secure_ready && secure_->stage == SecurityStage::NoiseHandshake) {
            while (!secure_->secure_ready) {
                std::vector<uint8_t> frame;
                std::string error;
                if (!try_pop_wire_frame(io_->wire_rx_buffer, frame, error)) {
                    if (!error.empty()) return Result<void>::err(error);
                    break;
                }

                auto step = process_handshake_frame(frame);
                if (step.is_err()) return step;
            }
        }

        if (!secure_->secure_ready) return Result<void>::ok();

        while (true) {
            std::vector<uint8_t> frame;
            std::string error;
            if (!try_pop_wire_frame(io_->wire_rx_buffer, frame, error)) {
                if (!error.empty()) return Result<void>::err(error);
                break;
            }

            auto plaintext = NoiseHandshake::decrypt(secure_->noise.cs_recv, frame);
            if (plaintext.is_err()) return Result<void>::err(plaintext.error().message);

            auto stream = ensure_inbound_stream();
            stream->rx_buffer.insert(stream->rx_buffer.end(),
                                     plaintext.value().begin(),
                                     plaintext.value().end());
        }

        return Result<void>::ok();
    }

    Result<void> process_protocol_negotiation() {
        if (is_initiator_) {
            auto span = multistream_exchange_span(io_->wire_rx_buffer);
            if (span.is_err()) {
                if (is_incomplete_multistream_error(span.error().message)) {
                    return Result<void>::ok();
                }
                return Result<void>::err(span.error().message);
            }

            ConstBytes incoming(io_->wire_rx_buffer.data(), span.value());
            auto selected = MultistreamSelect::read_outbound_response(incoming, "/noise");
            if (selected.is_err()) return Result<void>::err(selected.error().message);

            io_->wire_rx_buffer.erase(
                io_->wire_rx_buffer.begin(),
                io_->wire_rx_buffer.begin() + static_cast<std::ptrdiff_t>(span.value()));
            return start_noise_handshake();
        }

        auto span = multistream_exchange_span(io_->wire_rx_buffer);
        if (span.is_err()) {
            if (is_incomplete_multistream_error(span.error().message)) {
                return Result<void>::ok();
            }
            return Result<void>::err(span.error().message);
        }

        ConstBytes incoming(io_->wire_rx_buffer.data(), span.value());
        auto negotiated = MultistreamSelect::negotiate_inbound(incoming, {"/noise"});
        if (negotiated.is_err()) return Result<void>::err(negotiated.error().message);
        if (!negotiated.value().protocol.has_value() ||
            *negotiated.value().protocol != "/noise") {
            return Result<void>::err("noise protocol negotiation failed");
        }

        auto queued = queue_raw_bytes(*io_, negotiated.value().outbound);
        if (queued.is_err()) return queued;
        auto flush = flush_pending_writes();
        if (flush.is_err()) return flush;

        io_->wire_rx_buffer.erase(
            io_->wire_rx_buffer.begin(),
            io_->wire_rx_buffer.begin() + static_cast<std::ptrdiff_t>(span.value()));
        secure_->stage = SecurityStage::NoiseHandshake;
        secure_->handshake_state = HandshakeState::AwaitingMsg1;
        return Result<void>::ok();
    }

    Result<void> start_noise_handshake() {
        auto msg1 = NoiseHandshake::write_msg1(secure_->noise);
        if (msg1.empty()) return Result<void>::err("failed to start noise handshake");

        secure_->stage = SecurityStage::NoiseHandshake;
        secure_->handshake_state = HandshakeState::AwaitingMsg2;
        auto queued = queue_wire_message(*io_, msg1);
        if (queued.is_err()) return queued;
        return flush_pending_writes();
    }

    Result<void> validate_expected_remote_peer() const {
        if (!is_initiator_) return Result<void>::ok();

        auto endpoint = remote_addr_.parse_ip4_tcp();
        if (endpoint.is_err()) return Result<void>::ok();
        if (!endpoint.value().peer_id.has_value()) return Result<void>::ok();
        if (!secure_->noise.remote_peer_id.has_value()) {
            return Result<void>::err("missing authenticated remote peer id");
        }

        auto expected = PeerId::from_string(*endpoint.value().peer_id);
        if (expected.is_err()) {
            return Result<void>::err("invalid expected remote peer id");
        }
        if (expected.value() != *secure_->noise.remote_peer_id) {
            return Result<void>::err("authenticated peer id does not match dial target");
        }
        return Result<void>::ok();
    }

    Result<void> process_handshake_frame(ConstBytes frame) {
        switch (secure_->handshake_state) {
            case HandshakeState::AwaitingMsg1: {
                auto msg2 = NoiseHandshake::process_msg1(secure_->noise, frame);
                if (msg2.is_err()) return Result<void>::err(msg2.error().message);

                secure_->handshake_state = HandshakeState::AwaitingMsg3;
                auto queued = queue_wire_message(*io_, msg2.value());
                if (queued.is_err()) return queued;
                return flush_pending_writes();
            }
            case HandshakeState::AwaitingMsg2: {
                auto msg3 = NoiseHandshake::process_msg2(secure_->noise, frame);
                if (msg3.is_err()) return Result<void>::err(msg3.error().message);

                secure_->handshake_state = HandshakeState::Complete;
                auto queued = queue_wire_message(*io_, msg3.value());
                if (queued.is_err()) return queued;
                auto flush = flush_pending_writes();
                if (flush.is_err()) return flush;
                return finish_secure_upgrade();
            }
            case HandshakeState::AwaitingMsg3: {
                auto done = NoiseHandshake::process_msg3(secure_->noise, frame);
                if (done.is_err()) return Result<void>::err(done.error_message());

                secure_->handshake_state = HandshakeState::Complete;
                return finish_secure_upgrade();
            }
            case HandshakeState::Idle:
                return Result<void>::err("noise handshake has not started");
            case HandshakeState::Complete:
                return Result<void>::err("unexpected extra noise handshake frame");
        }

        return Result<void>::err("invalid handshake state");
    }

    Result<void> finish_secure_upgrade() {
        if (secure_->secure_ready) return Result<void>::ok();

        auto expected_peer = validate_expected_remote_peer();
        if (expected_peer.is_err()) return expected_peer;

        secure_->stage = SecurityStage::Ready;
        secure_->secure_ready = true;
        state_ = ConnectionState::SecureReady;
        event_queue_.push_back(ConnectionEvent{
            .type = ConnectionEvent::Type::Secured,
            .stream_id = std::nullopt,
            .detail = "noise handshake complete",
        });

        state_ = ConnectionState::Multiplexing;
        event_queue_.push_back(ConnectionEvent{
            .type = ConnectionEvent::Type::MultiplexerReady,
            .stream_id = std::nullopt,
            .detail = "noise secure channel ready (single-stream fallback)",
        });

        state_ = ConnectionState::Ready;
        if (create_inbound_stream_on_ready_ && pending_inbound_streams_.empty()) {
            create_stream(std::nullopt, /*inbound_event=*/true);
            create_inbound_stream_on_ready_ = false;
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
            .secure = secure_,
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

    ConnectionId                     id_{0};
    ConnectionState                  state_{ConnectionState::TcpAccepted};
    Multiaddr                        remote_addr_;
    std::shared_ptr<SessionIoState>  io_;
    std::shared_ptr<SecureChannelState> secure_;
    std::shared_ptr<StreamState>     stream_state_;
    std::vector<ConnectionEvent>     event_queue_;
    std::vector<StreamHandle>        pending_inbound_streams_;
    StreamId                         next_stream_id_{1};
    bool                             closed_emitted_{false};
    bool                             is_initiator_{false};
    bool                             create_inbound_stream_on_ready_{false};
};

}  // namespace

std::unique_ptr<ConnectionSession> make_outbound_connection_session(
    ConnectionId id,
    int socket_fd,
    Multiaddr remote_addr,
    std::optional<Identity> local_identity) {
    return std::make_unique<BasicConnectionSession>(
        id, socket_fd, std::move(remote_addr), true, std::move(local_identity));
}

std::unique_ptr<ConnectionSession> make_inbound_connection_session(
    ConnectionId id,
    int socket_fd,
    Multiaddr remote_addr,
    std::optional<Identity> local_identity) {
    return std::make_unique<BasicConnectionSession>(
        id, socket_fd, std::move(remote_addr), false, std::move(local_identity));
}

}  // namespace peercore
