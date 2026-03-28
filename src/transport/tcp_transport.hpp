#pragma once

#include "../../include/peercore/types.hpp"

#include <functional>
#include <netinet/in.h>
#include <vector>

namespace peercore::transport {

using RawFd = int;

struct TcpSocket {
    RawFd fd{-1};
    Multiaddr remote_addr;
};

// Callbacks invoked by the transport
struct TcpTransportCallbacks {
    std::function<void(TcpSocket)> on_accepted;   // inbound connection
    std::function<void(TcpSocket)> on_connected;  // outbound dial succeeded
    std::function<void(const Multiaddr&, std::string)> on_dial_failed;
};

class TcpTransport {
public:
    TcpTransport();
    ~TcpTransport();

    Result<void> listen(const Multiaddr& addr, TcpTransportCallbacks callbacks);

    // Initiates a non-blocking outbound connection.
    // Returns ok if the dial was *initiated* (regardless of connection outcome).
    // The result is always delivered via callbacks: on_connected or on_dial_failed.
    // Returns err only if the address is malformed or socket() fails.
    Result<void> dial(const Multiaddr& addr, TcpTransportCallbacks callbacks);

    // Called by EventLoop when the listen socket is readable
    void on_accept_ready(RawFd listen_fd);
    // Called by EventLoop when a non-blocking connect socket becomes writable
    void on_connect_ready(RawFd socket_fd);

    std::vector<RawFd> listener_fds() const;
    std::vector<RawFd> dialing_fds() const;
    Result<Multiaddr> local_addr(RawFd fd) const;

    // Close and discard all listeners and pending dials.
    // Safe to call multiple times. Also called by the destructor.
    void close_all();

private:
    struct Listener {
        RawFd fd{-1};
        Multiaddr addr;
        TcpTransportCallbacks callbacks;
    };

    struct PendingDial {
        RawFd fd{-1};
        Multiaddr addr;
        TcpTransportCallbacks callbacks;
    };

    std::vector<Listener> listeners_;
    std::vector<PendingDial> pending_dials_;

    static Result<RawFd> create_listen_socket(const sockaddr_in& addr);
    static Result<RawFd> create_connect_socket();
};

}  // namespace peercore::transport
