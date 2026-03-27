#include "tcp_transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "peercore/log.hpp"

namespace peercore::transport {

namespace {

constexpr std::string_view kComp = "peercore/tcp";

Result<sockaddr_in> to_sockaddr(const Multiaddr& addr) {
    auto endpoint = addr.parse_ip4_tcp();
    if (endpoint.is_err()) {
        return Result<sockaddr_in>::err(endpoint.error().message);
    }

    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_port = htons(endpoint.value().port);
    if (::inet_pton(AF_INET, endpoint.value().ip.c_str(), &out.sin_addr) != 1) {
        return Result<sockaddr_in>::err("invalid IPv4 address");
    }
    return Result<sockaddr_in>::ok(out);
}

Multiaddr from_sockaddr(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return Multiaddr::from_ip4_tcp(ip, ntohs(addr.sin_port));
}

Result<void> set_socket_common_flags(RawFd fd) {
    const int on = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        return Result<void>::err(std::string("setsockopt(SO_REUSEADDR) failed: ") +
                                 std::strerror(errno));
    }
    return Result<void>::ok();
}

Result<void> set_nonblocking(RawFd fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return Result<void>::err(std::string("fcntl(F_GETFL) failed: ") +
                                 std::strerror(errno));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return Result<void>::err(std::string("fcntl(F_SETFL) failed: ") +
                                 std::strerror(errno));
    }
    return Result<void>::ok();
}

void close_fd(RawFd& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

}  // namespace

TcpTransport::TcpTransport() = default;
TcpTransport::~TcpTransport() { close_all(); }

Result<void> TcpTransport::listen(const Multiaddr& addr, TcpTransportCallbacks callbacks) {
    auto socket = create_listen_socket(addr);
    if (socket.is_err()) return Result<void>::err(socket.error().message);

    listeners_.push_back(Listener{
        .fd = socket.value(),
        .addr = addr,
        .callbacks = std::move(callbacks),
    });
    PEERCORE_LOG_DEBUG(kComp, "listen fd={} addr={}", socket.value(), addr.to_string());
    return Result<void>::ok();
}

Result<void> TcpTransport::dial(const Multiaddr& addr, TcpTransportCallbacks callbacks) {
    auto socket = create_connect_socket(addr);
    if (socket.is_err()) return Result<void>::err(socket.error().message);

    RawFd fd = socket.value();
    auto socket_addr = to_sockaddr(addr);
    if (socket_addr.is_err()) {
        close_fd(fd);
        return Result<void>::err(socket_addr.error().message);
    }

    const int rc = ::connect(fd,
                             reinterpret_cast<const sockaddr*>(&socket_addr.value()),
                             sizeof(sockaddr_in));
    if (rc == 0) {
        PEERCORE_LOG_DEBUG(kComp, "dial connected immediately fd={} addr={}", fd, addr.to_string());
        if (callbacks.on_connected) {
            callbacks.on_connected(TcpSocket{.fd = fd, .remote_addr = addr});
        } else {
            close_fd(fd);
        }
        return Result<void>::ok();
    }

    if (errno != EINPROGRESS) {
        const std::string detail = std::strerror(errno);
        PEERCORE_LOG_DEBUG(kComp, "dial failed immediately addr={} err={}", addr.to_string(), detail);
        if (callbacks.on_dial_failed) callbacks.on_dial_failed(addr, detail);
        close_fd(fd);
        return Result<void>::ok();
    }

    pending_dials_.push_back(PendingDial{
        .fd = fd,
        .addr = addr,
        .callbacks = std::move(callbacks),
    });
    PEERCORE_LOG_TRACE(kComp, "dial pending fd={} addr={}", fd, addr.to_string());
    return Result<void>::ok();
}

void TcpTransport::on_accept_ready(RawFd listen_fd) {
    auto it = std::find_if(listeners_.begin(), listeners_.end(),
                           [listen_fd](const Listener& listener) {
                               return listener.fd == listen_fd;
                           });
    if (it == listeners_.end()) return;

    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        RawFd fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            PEERCORE_LOG_ERROR(kComp, "accept failed fd={} err={}", listen_fd, std::strerror(errno));
            return;
        }

        auto flags = set_socket_common_flags(fd);
        if (flags.is_ok()) flags = set_nonblocking(fd);
        if (flags.is_err()) {
            PEERCORE_LOG_ERROR(kComp, "accepted socket setup failed fd={} err={}", fd, flags.error_message());
            close_fd(fd);
            continue;
        }

        if (it->callbacks.on_accepted) {
            it->callbacks.on_accepted(TcpSocket{
                .fd = fd,
                .remote_addr = from_sockaddr(addr),
            });
        } else {
            close_fd(fd);
        }
    }
}

void TcpTransport::on_connect_ready(RawFd socket_fd) {
    auto it = std::find_if(pending_dials_.begin(), pending_dials_.end(),
                           [socket_fd](const PendingDial& dial) {
                               return dial.fd == socket_fd;
                           });
    if (it == pending_dials_.end()) return;

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
        so_error = errno;
    }

    PendingDial dial = std::move(*it);
    pending_dials_.erase(it);

    if (so_error != 0) {
        const std::string detail = std::strerror(so_error);
        PEERCORE_LOG_DEBUG(kComp, "dial failed fd={} addr={} err={}",
                           socket_fd, dial.addr.to_string(), detail);
        if (dial.callbacks.on_dial_failed) {
            dial.callbacks.on_dial_failed(dial.addr, detail);
        }
        close_fd(dial.fd);
        return;
    }

    PEERCORE_LOG_DEBUG(kComp, "dial connected fd={} addr={}", socket_fd, dial.addr.to_string());
    if (dial.callbacks.on_connected) {
        dial.callbacks.on_connected(TcpSocket{
            .fd = dial.fd,
            .remote_addr = dial.addr,
        });
    } else {
        close_fd(dial.fd);
    }
}

std::vector<RawFd> TcpTransport::listener_fds() const {
    std::vector<RawFd> fds;
    fds.reserve(listeners_.size());
    for (const auto& listener : listeners_) {
        fds.push_back(listener.fd);
    }
    return fds;
}

std::vector<RawFd> TcpTransport::dialing_fds() const {
    std::vector<RawFd> fds;
    fds.reserve(pending_dials_.size());
    for (const auto& dial : pending_dials_) {
        fds.push_back(dial.fd);
    }
    return fds;
}

Result<Multiaddr> TcpTransport::local_addr(RawFd fd) const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return Result<Multiaddr>::err(std::string("getsockname failed: ") +
                                      std::strerror(errno));
    }
    return Result<Multiaddr>::ok(from_sockaddr(addr));
}

void TcpTransport::close_all() {
    for (auto& listener : listeners_) {
        close_fd(listener.fd);
    }
    listeners_.clear();

    for (auto& dial : pending_dials_) {
        close_fd(dial.fd);
    }
    pending_dials_.clear();
}

Result<RawFd> TcpTransport::create_listen_socket(const Multiaddr& addr) {
    auto socket_addr = to_sockaddr(addr);
    if (socket_addr.is_err()) return Result<RawFd>::err(socket_addr.error().message);

    RawFd fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return Result<RawFd>::err(std::string("socket() failed: ") + std::strerror(errno));
    }

    auto flags = set_socket_common_flags(fd);
    if (flags.is_ok()) flags = set_nonblocking(fd);
    if (flags.is_err()) {
        close_fd(fd);
        return Result<RawFd>::err(flags.error_message());
    }

    if (::bind(fd,
               reinterpret_cast<const sockaddr*>(&socket_addr.value()),
               sizeof(sockaddr_in)) < 0) {
        const std::string detail = std::strerror(errno);
        close_fd(fd);
        return Result<RawFd>::err(std::string("bind() failed: ") + detail);
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        const std::string detail = std::strerror(errno);
        close_fd(fd);
        return Result<RawFd>::err(std::string("listen() failed: ") + detail);
    }

    return Result<RawFd>::ok(fd);
}

Result<RawFd> TcpTransport::create_connect_socket(const Multiaddr& addr) {
    auto socket_addr = to_sockaddr(addr);
    if (socket_addr.is_err()) return Result<RawFd>::err(socket_addr.error().message);

    RawFd fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return Result<RawFd>::err(std::string("socket() failed: ") + std::strerror(errno));
    }

    auto flags = set_socket_common_flags(fd);
    if (flags.is_ok()) flags = set_nonblocking(fd);
    if (flags.is_err()) {
        close_fd(fd);
        return Result<RawFd>::err(flags.error_message());
    }

    return Result<RawFd>::ok(fd);
}

}  // namespace peercore::transport
