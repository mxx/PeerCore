#include <gtest/gtest.h>
#include <peercore/connection_session.hpp>

#include <array>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

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

}  // namespace

TEST(ConnectionSession, OutboundUpgradeAndStreamIo) {
    SocketPair sockets;
    auto session = make_outbound_connection_session(
        7, sockets.local, Multiaddr("/ip4/127.0.0.1/tcp/4001"));

    ASSERT_TRUE(session->begin_outbound_upgrade().is_ok());

    auto ev = session->next_event();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, ConnectionEvent::Type::Secured);

    ev = session->next_event();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, ConnectionEvent::Type::MultiplexerReady);

    auto stream_res = session->request_open_stream("/test/1.0.0");
    ASSERT_TRUE(stream_res.is_ok());
    auto stream = stream_res.value();

    ev = session->next_event();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, ConnectionEvent::Type::StreamOpened);
    ASSERT_TRUE(ev->stream_id.has_value());
    EXPECT_EQ(stream->id(), *ev->stream_id);

    const char inbound[] = "ping";
    ASSERT_EQ(::write(sockets.peer, inbound, sizeof(inbound) - 1), sizeof(inbound) - 1);
    session->on_socket_readable();

    std::array<uint8_t, 16> read_buf{};
    auto read_res = stream->try_read(read_buf);
    ASSERT_TRUE(read_res.is_ok());
    EXPECT_EQ(read_res.value(), sizeof(inbound) - 1);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(read_buf.data()), read_res.value()), "ping");

    const std::array<uint8_t, 4> outbound{{'p', 'o', 'n', 'g'}};
    auto write_res = stream->try_write(outbound);
    ASSERT_TRUE(write_res.is_ok());
    EXPECT_EQ(write_res.value(), outbound.size());

    char peer_buf[16]{};
    const ssize_t n = ::read(sockets.peer, peer_buf, sizeof(peer_buf));
    ASSERT_EQ(n, static_cast<ssize_t>(outbound.size()));
    EXPECT_EQ(std::string(peer_buf, peer_buf + n), "pong");

    session->close();
    ev = session->next_event();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, ConnectionEvent::Type::Closed);

    sockets.local = -1;
}

TEST(ConnectionSession, InboundUpgradeCreatesAcceptableStream) {
    SocketPair sockets;
    auto session = make_inbound_connection_session(
        9, sockets.local, Multiaddr("/ip4/127.0.0.1/tcp/4002"));

    ASSERT_TRUE(session->begin_inbound_upgrade().is_ok());

    auto ev = session->next_event();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, ConnectionEvent::Type::Secured);

    ev = session->next_event();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, ConnectionEvent::Type::MultiplexerReady);

    ev = session->next_event();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, ConnectionEvent::Type::StreamAccepted);

    auto stream = session->accept_inbound_stream();
    ASSERT_TRUE(stream.has_value());

    const char inbound[] = "hello";
    ASSERT_EQ(::write(sockets.peer, inbound, sizeof(inbound) - 1), sizeof(inbound) - 1);
    session->on_socket_readable();

    std::array<uint8_t, 16> read_buf{};
    auto read_res = (*stream)->try_read(read_buf);
    ASSERT_TRUE(read_res.is_ok());
    EXPECT_EQ(read_res.value(), sizeof(inbound) - 1);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(read_buf.data()), read_res.value()), "hello");

    sockets.local = -1;
}
