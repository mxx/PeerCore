#pragma once

#include "../../include/peercore/types.hpp"

#include <functional>

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
    std::function<void(Multiaddr, std::string)> on_dial_failed;
};

class TcpTransport {
public:
    TcpTransport();
    ~TcpTransport();

    Result<void> listen(const Multiaddr& addr, TcpTransportCallbacks callbacks);
    Result<void> dial(const Multiaddr& addr, TcpTransportCallbacks callbacks);

    // Called by EventLoop when the listen socket is readable
    void on_accept_ready(RawFd listen_fd);

    void close_all();

private:
    struct Listener {
        RawFd fd{-1};
        Multiaddr addr;
        TcpTransportCallbacks callbacks;
    };

    std::vector<Listener> listeners_;

    static Result<RawFd>     create_listen_socket(const Multiaddr& addr);
    static Result<TcpSocket> create_connected_socket(const Multiaddr& addr);
};

}  // namespace peercore::transport
