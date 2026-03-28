#include "../src/transport/tcp_transport.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace peercore;
using namespace peercore::transport;

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace {

// Block until fd has the requested events or timeout_ms elapses.
bool wait_fd(int fd, short events, int timeout_ms = 1000) {
    pollfd pfd{.fd = fd, .events = events, .revents = 0};
    const int rc = ::poll(&pfd, 1, timeout_ms);
    return rc == 1 && (pfd.revents & events) != 0;
}

// Listen on 127.0.0.1:0, return the OS-assigned port (0 on failure).
uint16_t listen_loopback(TcpTransport& t, TcpTransportCallbacks cbs = {}) {
    if (!t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0"), std::move(cbs)).is_ok())
        return 0;
    auto fds = t.listener_fds();
    if (fds.empty()) return 0;
    auto addr = t.local_addr(fds[0]);
    if (!addr.is_ok()) return 0;
    const auto s = addr.value().to_string();
    const auto pos = s.rfind('/');
    if (pos == std::string::npos) return 0;
    return static_cast<uint16_t>(std::stoi(s.substr(pos + 1)));
}

// Blocking raw connect to 127.0.0.1:port. Returns fd or -1.
int raw_connect(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

// ── Existing baseline tests (kept) ───────────────────────────────────────────

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

    // accepted socket is non-blocking; wait for the byte to arrive
    ASSERT_TRUE(wait_fd(accepted->fd, POLLIN));
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

// ── Address parsing (exercised via listen/dial) ───────────────────────────────

// Valid addresses ─────────────────────────────────────────────────────────────

TEST(TcpTransportAddr, Port0IsValid) {
    TcpTransport t;
    auto r = t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0"), {});
    EXPECT_TRUE(r.is_ok()) << r.error_message();
}

TEST(TcpTransportAddr, Port65535IsValid) {
    // Syntactically valid; may fail at bind if port is in use—that is not a
    // parse error, so we only assert the message doesn't mention the format.
    TcpTransport t;
    auto r = t.listen(Multiaddr("/ip4/127.0.0.1/tcp/65535"), {});
    if (r.is_err()) {
        EXPECT_EQ(r.error_message().find("only /ip4"), std::string::npos)
            << "port 65535 must not fail with a parse error: " << r.error_message();
    }
}

TEST(TcpTransportAddr, WildcardAddress) {
    TcpTransport t;
    auto r = t.listen(Multiaddr("/ip4/0.0.0.0/tcp/0"), {});
    EXPECT_TRUE(r.is_ok()) << r.error_message();
}

// Invalid protocol ────────────────────────────────────────────────────────────

TEST(TcpTransportAddr, IPv6Rejected) {
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip6/::1/tcp/8080"), {}).is_err());
}

TEST(TcpTransportAddr, UdpRejected) {
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/udp/8080"), {}).is_err());
}

TEST(TcpTransportAddr, MissingTcpComponent) {
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1"), {}).is_err());
}

TEST(TcpTransportAddr, ExtraPathComponent) {
    // /ip4/.../tcp/<port>/noise → 5 segments → index != 4 → parse error
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/8080/noise"), {}).is_err());
}

// Invalid IP ──────────────────────────────────────────────────────────────────

TEST(TcpTransportAddr, BadIPOctet) {
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/999.999.999.999/tcp/8080"), {}).is_err());
}

TEST(TcpTransportAddr, IPAsHostname) {
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/localhost/tcp/8080"), {}).is_err());
}

// Invalid port ────────────────────────────────────────────────────────────────

TEST(TcpTransportAddr, PortTooHigh) {
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/65536"), {}).is_err());
}

TEST(TcpTransportAddr, NegativePort) {
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/-1"), {}).is_err());
}

TEST(TcpTransportAddr, NonNumericPort) {
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/abc"), {}).is_err());
}

TEST(TcpTransportAddr, PortWithTrailingJunk) {
    // "0abc" → strtol returns 0 but *end != '\0' → invalid
    TcpTransport t;
    EXPECT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0abc"), {}).is_err());
}

// ── listen() ─────────────────────────────────────────────────────────────────

TEST(TcpTransportListen, InitiallyNoListeners) {
    TcpTransport t;
    EXPECT_TRUE(t.listener_fds().empty());
}

TEST(TcpTransportListen, AddsEntryToListenerFds) {
    TcpTransport t;
    ASSERT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0"), {}).is_ok());
    EXPECT_EQ(t.listener_fds().size(), 1u);
}

TEST(TcpTransportListen, MultipleListeners) {
    TcpTransport t;
    ASSERT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0"), {}).is_ok());
    ASSERT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0"), {}).is_ok());
    EXPECT_EQ(t.listener_fds().size(), 2u);
}

TEST(TcpTransportListen, PortZeroGetsDynamicPort) {
    TcpTransport t;
    const uint16_t port = listen_loopback(t);
    ASSERT_GT(port, 0u);
}

TEST(TcpTransportListen, DuplicatePortFails) {
    TcpTransport t1;
    const uint16_t port = listen_loopback(t1);
    ASSERT_GT(port, 0u);

    TcpTransport t2;
    auto r = t2.listen(
        Multiaddr("/ip4/127.0.0.1/tcp/" + std::to_string(port)), {});
    EXPECT_TRUE(r.is_err());
}

TEST(TcpTransportListen, ParseFailureDoesNotAddListener) {
    TcpTransport t;
    t.listen(Multiaddr("/ip4/127.0.0.1/udp/9999"), {});
    EXPECT_TRUE(t.listener_fds().empty());
}

// ── local_addr() ─────────────────────────────────────────────────────────────

TEST(TcpTransportLocalAddr, InvalidFdReturnsError) {
    TcpTransport t;
    EXPECT_TRUE(t.local_addr(-1).is_err());
}

TEST(TcpTransportLocalAddr, ReturnsAssignedPort) {
    TcpTransport t;
    const uint16_t port = listen_loopback(t);
    ASSERT_GT(port, 0u);

    auto addr = t.local_addr(t.listener_fds()[0]);
    ASSERT_TRUE(addr.is_ok());
    EXPECT_NE(addr.value().to_string().find("/tcp/" + std::to_string(port)),
              std::string::npos);
}

// ── on_accept_ready() ─────────────────────────────────────────────────────────

TEST(TcpTransportAccept, UnknownFdIsNoOp) {
    TcpTransport t;
    EXPECT_NO_FATAL_FAILURE(t.on_accept_ready(99));
}

TEST(TcpTransportAccept, NoConnectionsPendingReturnsQuietly) {
    TcpTransport t;
    int call_count = 0;
    listen_loopback(t, TcpTransportCallbacks{
        .on_accepted = [&](TcpSocket s) { ++call_count; ::close(s.fd); },
    });
    // No client has connected yet → EAGAIN on first accept() → returns
    t.on_accept_ready(t.listener_fds()[0]);
    EXPECT_EQ(call_count, 0);
}

TEST(TcpTransportAccept, SingleClientFiresCallback) {
    TcpTransport t;
    int accept_count = 0;
    std::optional<TcpSocket> accepted_sock;

    const uint16_t port = listen_loopback(t, TcpTransportCallbacks{
        .on_accepted = [&](TcpSocket s) { accepted_sock = std::move(s); ++accept_count; },
    });
    ASSERT_GT(port, 0u);

    int client_fd = raw_connect(port);
    ASSERT_GE(client_fd, 0);

    ASSERT_TRUE(wait_fd(t.listener_fds()[0], POLLIN));
    t.on_accept_ready(t.listener_fds()[0]);

    EXPECT_EQ(accept_count, 1);
    ASSERT_TRUE(accepted_sock.has_value());
    EXPECT_GE(accepted_sock->fd, 0);

    ::close(accepted_sock->fd);
    ::close(client_fd);
}

TEST(TcpTransportAccept, MultipleClientsDrainedInOneCall) {
    TcpTransport t;
    std::vector<int> accepted_fds;

    const uint16_t port = listen_loopback(t, TcpTransportCallbacks{
        .on_accepted = [&](TcpSocket s) { accepted_fds.push_back(s.fd); },
    });
    ASSERT_GT(port, 0u);

    // Queue 3 simultaneous clients
    std::vector<int> client_fds;
    for (int i = 0; i < 3; ++i) {
        int fd = raw_connect(port);
        ASSERT_GE(fd, 0) << "raw_connect failed for client " << i;
        client_fds.push_back(fd);
    }

    // A single on_accept_ready call must drain all three (ET-style loop)
    ASSERT_TRUE(wait_fd(t.listener_fds()[0], POLLIN));
    t.on_accept_ready(t.listener_fds()[0]);

    EXPECT_EQ(static_cast<int>(accepted_fds.size()), 3);

    for (int fd : accepted_fds) ::close(fd);
    for (int fd : client_fds) ::close(fd);
}

TEST(TcpTransportAccept, NullCallbackClosesFdWithoutLeak) {
    TcpTransport t;
    // Listen with no on_accepted — accepted fds must be closed by the transport.
    const uint16_t port = listen_loopback(t, TcpTransportCallbacks{});
    ASSERT_GT(port, 0u);

    int client_fd = raw_connect(port);
    ASSERT_GE(client_fd, 0);

    ASSERT_TRUE(wait_fd(t.listener_fds()[0], POLLIN));
    EXPECT_NO_FATAL_FAILURE(t.on_accept_ready(t.listener_fds()[0]));

    ::close(client_fd);
}

TEST(TcpTransportAccept, RemoteAddrIsLoopback) {
    TcpTransport t;
    std::string remote;

    const uint16_t port = listen_loopback(t, TcpTransportCallbacks{
        .on_accepted = [&](TcpSocket s) {
            remote = s.remote_addr.to_string();
            ::close(s.fd);
        },
    });
    ASSERT_GT(port, 0u);

    int client_fd = raw_connect(port);
    ASSERT_GE(client_fd, 0);

    ASSERT_TRUE(wait_fd(t.listener_fds()[0], POLLIN));
    t.on_accept_ready(t.listener_fds()[0]);

    // Remote address must be /ip4/127.0.0.1/tcp/<ephemeral>
    EXPECT_NE(remote.find("/ip4/127.0.0.1/tcp/"), std::string::npos) << remote;

    ::close(client_fd);
}

// ── dial() ────────────────────────────────────────────────────────────────────

TEST(TcpTransportDial, InitiallyNoPendingDials) {
    TcpTransport t;
    EXPECT_TRUE(t.dialing_fds().empty());
}

TEST(TcpTransportDial, InvalidMultiaddrReturnsError) {
    TcpTransport t;
    EXPECT_TRUE(t.dial(Multiaddr("/ip4/127.0.0.1/udp/8080"), {}).is_err());
}

TEST(TcpTransportDial, ParseFailureDoesNotAddPendingDial) {
    TcpTransport t;
    t.dial(Multiaddr("/ip4/127.0.0.1/udp/8080"), {});
    EXPECT_TRUE(t.dialing_fds().empty());
}

TEST(TcpTransportDial, RefusedConnectionFiresOnDialFailed) {
    // Port 1 is almost certainly closed on loopback; produces ECONNREFUSED.
    TcpTransport t;
    int fail_count  = 0;
    int conn_count  = 0;

    auto r = t.dial(Multiaddr("/ip4/127.0.0.1/tcp/1"), TcpTransportCallbacks{
        .on_connected  = [&](TcpSocket s) { ++conn_count; ::close(s.fd); },
        .on_dial_failed = [&](Multiaddr, std::string) { ++fail_count; },
    });
    ASSERT_TRUE(r.is_ok());

    // If EINPROGRESS path: drive on_connect_ready
    if (!t.dialing_fds().empty()) {
        const int dfd = t.dialing_fds().front();
        // Give up to 1s for the OS to report the error
        wait_fd(dfd, POLLOUT, 1000);
        t.on_connect_ready(dfd);
    }

    EXPECT_EQ(conn_count,  0);
    EXPECT_EQ(fail_count,  1);
    EXPECT_TRUE(t.dialing_fds().empty());
}

TEST(TcpTransportDial, NullOnDialFailedDoesNotCrash) {
    TcpTransport t;
    // No on_dial_failed — must not crash on refused connection
    EXPECT_NO_FATAL_FAILURE(
        t.dial(Multiaddr("/ip4/127.0.0.1/tcp/1"), TcpTransportCallbacks{})
    );
    if (!t.dialing_fds().empty()) {
        int dfd = t.dialing_fds().front();
        wait_fd(dfd, POLLOUT, 1000);
        EXPECT_NO_FATAL_FAILURE(t.on_connect_ready(dfd));
    }
}

TEST(TcpTransportDial, NullOnConnectedClosesFd) {
    TcpTransport server;
    const uint16_t port = listen_loopback(server, TcpTransportCallbacks{
        .on_accepted = [](TcpSocket s) { ::close(s.fd); },
    });
    ASSERT_GT(port, 0u);

    TcpTransport client;
    // on_connected is null — transport must close the fd, not leak it
    auto r = client.dial(
        Multiaddr("/ip4/127.0.0.1/tcp/" + std::to_string(port)),
        TcpTransportCallbacks{});  // no on_connected
    ASSERT_TRUE(r.is_ok());

    // Accept so the connection can complete
    if (wait_fd(server.listener_fds()[0], POLLIN, 500))
        server.on_accept_ready(server.listener_fds()[0]);

    if (!client.dialing_fds().empty()) {
        int dfd = client.dialing_fds().front();
        wait_fd(dfd, POLLOUT, 500);
        EXPECT_NO_FATAL_FAILURE(client.on_connect_ready(dfd));
    }
    // Either connected immediately (fd already closed) or on_connect_ready ran.
    // In both cases, no dangling fd should remain in pending list.
    EXPECT_TRUE(client.dialing_fds().empty());
}

// ── on_connect_ready() ────────────────────────────────────────────────────────

TEST(TcpTransportConnectReady, UnknownFdIsNoOp) {
    TcpTransport t;
    EXPECT_NO_FATAL_FAILURE(t.on_connect_ready(99));
}

TEST(TcpTransportConnectReady, RemovesPendingDialEntry) {
    TcpTransport server;
    const uint16_t port = listen_loopback(server, TcpTransportCallbacks{
        .on_accepted = [](TcpSocket s) { ::close(s.fd); },
    });
    ASSERT_GT(port, 0u);

    TcpTransport client;
    std::optional<TcpSocket> connected;

    client.dial(
        Multiaddr("/ip4/127.0.0.1/tcp/" + std::to_string(port)),
        TcpTransportCallbacks{
            .on_connected = [&](TcpSocket s) { connected = s; },
        });

    if (wait_fd(server.listener_fds()[0], POLLIN, 500))
        server.on_accept_ready(server.listener_fds()[0]);

    if (!connected.has_value() && !client.dialing_fds().empty()) {
        int dfd = client.dialing_fds().front();
        wait_fd(dfd, POLLOUT, 500);
        client.on_connect_ready(dfd);
    }

    EXPECT_TRUE(client.dialing_fds().empty());

    if (connected.has_value()) ::close(connected->fd);
}

// ── close_all() ──────────────────────────────────────────────────────────────

TEST(TcpTransportCloseAll, ClearsListeners) {
    TcpTransport t;
    ASSERT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0"), {}).is_ok());
    t.close_all();
    EXPECT_TRUE(t.listener_fds().empty());
}

TEST(TcpTransportCloseAll, ClearsPendingDials) {
    TcpTransport server;
    const uint16_t port = listen_loopback(server);
    ASSERT_GT(port, 0u);

    TcpTransport client;
    client.dial(
        Multiaddr("/ip4/127.0.0.1/tcp/" + std::to_string(port)),
        TcpTransportCallbacks{.on_connected = [](TcpSocket s) { ::close(s.fd); }});

    client.close_all();
    EXPECT_TRUE(client.dialing_fds().empty());
}

TEST(TcpTransportCloseAll, Idempotent) {
    TcpTransport t;
    ASSERT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0"), {}).is_ok());
    t.close_all();
    EXPECT_NO_FATAL_FAILURE(t.close_all());
    EXPECT_TRUE(t.listener_fds().empty());
}

TEST(TcpTransportCloseAll, DestructorDoesNotCrash) {
    // RAII: destroying a TcpTransport with active listeners must not crash.
    {
        TcpTransport t;
        ASSERT_TRUE(t.listen(Multiaddr("/ip4/127.0.0.1/tcp/0"), {}).is_ok());
    }
    SUCCEED();
}

TEST(TcpTransportCloseAll, DestructorWithPendingDialDoesNotCrash) {
    TcpTransport server;
    const uint16_t port = listen_loopback(server);
    ASSERT_GT(port, 0u);

    {
        TcpTransport client;
        client.dial(
            Multiaddr("/ip4/127.0.0.1/tcp/" + std::to_string(port)),
            TcpTransportCallbacks{.on_connected = [](TcpSocket s) { ::close(s.fd); }});
        // destructor fires here without calling on_connect_ready first
    }
    SUCCEED();
}
