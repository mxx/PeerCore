#include <gtest/gtest.h>
#include <peercore/connection_session.hpp>

#include <array>
#include <fcntl.h>
#include <sodium.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace peercore;

namespace {

void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("failed to set O_NONBLOCK");
    }
}

struct SocketPair {
    int local{-1};
    int peer{-1};

    SocketPair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            throw std::runtime_error("socketpair() failed");
        }
        local = fds[0];
        peer = fds[1];
        set_nonblocking(local);
        set_nonblocking(peer);
    }

    ~SocketPair() {
        if (local >= 0) ::close(local);
        if (peer >= 0) ::close(peer);
    }
};

Identity make_identity() {
    Identity identity;
    ::crypto_sign_keypair(identity.secret_key.data() + 32, identity.secret_key.data());
    identity.peer_id = PeerId::from_bytes(
        std::span<const uint8_t, 32>(identity.secret_key.data() + 32, 32));
    return identity;
}

std::vector<ConnectionEvent> drain_events(ConnectionSession& session) {
    std::vector<ConnectionEvent> events;
    while (auto ev = session.next_event()) {
        events.push_back(std::move(*ev));
    }
    return events;
}

void drive_sessions(ConnectionSession& a, ConnectionSession& b, int rounds = 8) {
    for (int i = 0; i < rounds; ++i) {
        a.on_socket_writable();
        b.on_socket_writable();
        a.on_socket_readable();
        b.on_socket_readable();
    }
}

}  // namespace

TEST(ConnectionSession, RunsNoiseHandshakeBeforeReady) {
    ASSERT_GE(::sodium_init(), 0);
    SocketPair sockets;
    auto outbound_identity = make_identity();
    auto inbound_identity = make_identity();
    auto outbound = make_outbound_connection_session(
        7, sockets.local, Multiaddr("/ip4/127.0.0.1/tcp/4001"), outbound_identity);
    auto inbound = make_inbound_connection_session(
        8, sockets.peer, Multiaddr("/ip4/127.0.0.1/tcp/4002"), inbound_identity);

    ASSERT_TRUE(outbound->begin_outbound_upgrade().is_ok());
    ASSERT_TRUE(inbound->begin_inbound_upgrade().is_ok());

    EXPECT_EQ(outbound->state(), ConnectionState::Securing);
    EXPECT_EQ(inbound->state(), ConnectionState::Securing);

    for (int i = 0; i < 32; ++i) {
        drive_sessions(*outbound, *inbound);
        if (outbound->state() == ConnectionState::Ready &&
            inbound->state() == ConnectionState::Ready) {
            break;
        }
    }

    EXPECT_EQ(outbound->state(), ConnectionState::Ready);
    EXPECT_EQ(inbound->state(), ConnectionState::Ready);
    ASSERT_TRUE(outbound->remote_peer().has_value());
    ASSERT_TRUE(inbound->remote_peer().has_value());
    EXPECT_EQ(*outbound->remote_peer(), inbound_identity.peer_id);
    EXPECT_EQ(*inbound->remote_peer(), outbound_identity.peer_id);

    auto outbound_events = drain_events(*outbound);
    ASSERT_GE(outbound_events.size(), 2u);
    EXPECT_EQ(outbound_events[0].type, ConnectionEvent::Type::Secured);
    EXPECT_EQ(outbound_events[1].type, ConnectionEvent::Type::MultiplexerReady);

    auto inbound_events = drain_events(*inbound);
    ASSERT_GE(inbound_events.size(), 3u);
    EXPECT_EQ(inbound_events[0].type, ConnectionEvent::Type::Secured);
    EXPECT_EQ(inbound_events[1].type, ConnectionEvent::Type::MultiplexerReady);
    EXPECT_EQ(inbound_events[2].type, ConnectionEvent::Type::StreamAccepted);

    sockets.local = -1;
    sockets.peer = -1;
}

TEST(ConnectionSession, EncryptsBidirectionalStreamIoAfterHandshake) {
    ASSERT_GE(::sodium_init(), 0);
    SocketPair sockets;
    auto outbound_identity = make_identity();
    auto inbound_identity = make_identity();
    auto outbound = make_outbound_connection_session(
        7, sockets.local, Multiaddr("/ip4/127.0.0.1/tcp/4001"), outbound_identity);
    auto inbound = make_inbound_connection_session(
        8, sockets.peer, Multiaddr("/ip4/127.0.0.1/tcp/4002"), inbound_identity);

    ASSERT_TRUE(outbound->begin_outbound_upgrade().is_ok());
    ASSERT_TRUE(inbound->begin_inbound_upgrade().is_ok());

    for (int i = 0; i < 32; ++i) {
        drive_sessions(*outbound, *inbound);
        if (outbound->state() == ConnectionState::Ready &&
            inbound->state() == ConnectionState::Ready) {
            break;
        }
    }

    ASSERT_EQ(outbound->state(), ConnectionState::Ready);
    ASSERT_EQ(inbound->state(), ConnectionState::Ready);

    drain_events(*outbound);
    drain_events(*inbound);

    auto outbound_stream_res = outbound->request_open_stream("/test/1.0.0");
    ASSERT_TRUE(outbound_stream_res.is_ok());
    auto outbound_stream = outbound_stream_res.value();

    auto outbound_events = drain_events(*outbound);
    ASSERT_FALSE(outbound_events.empty());
    EXPECT_EQ(outbound_events.front().type, ConnectionEvent::Type::StreamOpened);

    auto inbound_stream = inbound->accept_inbound_stream();
    ASSERT_TRUE(inbound_stream.has_value());

    const std::array<uint8_t, 4> ping{{'p', 'i', 'n', 'g'}};
    auto write_ping = outbound_stream->try_write(ping);
    ASSERT_TRUE(write_ping.is_ok());
    EXPECT_EQ(write_ping.value(), ping.size());

    drive_sessions(*outbound, *inbound);

    std::array<uint8_t, 16> read_buf{};
    auto read_ping = (*inbound_stream)->try_read(read_buf);
    ASSERT_TRUE(read_ping.is_ok());
    EXPECT_EQ(read_ping.value(), ping.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(read_buf.data()), read_ping.value()), "ping");

    const std::array<uint8_t, 4> pong{{'p', 'o', 'n', 'g'}};
    auto write_pong = (*inbound_stream)->try_write(pong);
    ASSERT_TRUE(write_pong.is_ok());
    EXPECT_EQ(write_pong.value(), pong.size());

    drive_sessions(*outbound, *inbound);

    read_buf.fill(0);
    auto read_pong = outbound_stream->try_read(read_buf);
    ASSERT_TRUE(read_pong.is_ok());
    EXPECT_EQ(read_pong.value(), pong.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(read_buf.data()), read_pong.value()), "pong");

    outbound->close();
    auto closed = outbound->next_event();
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->type, ConnectionEvent::Type::Closed);

    sockets.local = -1;
    sockets.peer = -1;
}

TEST(ConnectionSession, RejectsStreamOpenBeforeHandshakeCompletes) {
    ASSERT_GE(::sodium_init(), 0);
    SocketPair sockets;
    auto outbound_identity = make_identity();
    auto outbound = make_outbound_connection_session(
        7, sockets.local, Multiaddr("/ip4/127.0.0.1/tcp/4001"), outbound_identity);

    ASSERT_TRUE(outbound->begin_outbound_upgrade().is_ok());
    auto stream_res = outbound->request_open_stream("/test/1.0.0");
    ASSERT_TRUE(stream_res.is_err());
    EXPECT_EQ(stream_res.error().message, "connection is not ready");

    sockets.local = -1;
    sockets.peer = -1;
}
