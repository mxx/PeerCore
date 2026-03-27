#include "../src/transport/tcp_transport.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <poll.h>
#include <string>
#include <unistd.h>

using namespace peercore;
using namespace peercore::transport;

namespace {

bool wait_fd(int fd, short events, int timeout_ms = 1000) {
    pollfd pfd{
        .fd = fd,
        .events = events,
        .revents = 0,
    };
    const int rc = ::poll(&pfd, 1, timeout_ms);
    return rc == 1 && (pfd.revents & events) != 0;
}

}  // namespace

TEST(TcpTransport, ListenDialAndAccept) {
    TcpTransport server;
    TcpTransport client;
    std::optional<TcpSocket> accepted;
    std::optional<TcpSocket> connected;

    auto listen_res = server.listen(
        Multiaddr("/ip4/127.0.0.1/tcp/0"),
        TcpTransportCallbacks{
            .on_accepted = [&](TcpSocket socket) { accepted = std::move(socket); },
        });
    if (listen_res.is_err() &&
        listen_res.error_message().find("Operation not permitted") != std::string::npos) {
        GTEST_SKIP() << listen_res.error_message();
    }
    ASSERT_TRUE(listen_res.is_ok()) << listen_res.error_message();

    auto listeners = server.listener_fds();
    ASSERT_EQ(listeners.size(), 1u);

    auto listen_addr = server.local_addr(listeners.front());
    ASSERT_TRUE(listen_addr.is_ok());

    ASSERT_TRUE(client.dial(
        listen_addr.value(),
        TcpTransportCallbacks{
            .on_connected = [&](TcpSocket socket) { connected = std::move(socket); },
        }).is_ok());

    if (!client.dialing_fds().empty()) {
        const auto dial_fd = client.dialing_fds().front();
        ASSERT_TRUE(wait_fd(dial_fd, POLLOUT));
        client.on_connect_ready(dial_fd);
    }

    ASSERT_TRUE(wait_fd(listeners.front(), POLLIN));
    server.on_accept_ready(listeners.front());

    ASSERT_TRUE(connected.has_value());
    ASSERT_TRUE(accepted.has_value());

    const char payload[] = "x";
    ASSERT_EQ(::write(connected->fd, payload, 1), 1);

    char received = '\0';
    ASSERT_EQ(::read(accepted->fd, &received, 1), 1);
    EXPECT_EQ(received, 'x');

    ::close(connected->fd);
    ::close(accepted->fd);
}

TEST(TcpTransport, RejectsUnsupportedMultiaddr) {
    TcpTransport transport;
    auto res = transport.listen(Multiaddr("/dns4/example.com/tcp/80"), {});
    EXPECT_TRUE(res.is_err());
}
