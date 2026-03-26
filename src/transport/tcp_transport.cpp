#include "tcp_transport.hpp"

// TODO: implement socket creation, non-blocking connect/accept

namespace peercore::transport {

TcpTransport::TcpTransport()  = default;
TcpTransport::~TcpTransport() { close_all(); }

Result<void> TcpTransport::listen(const Multiaddr& /*addr*/,
                                   TcpTransportCallbacks /*callbacks*/) {
    return Result<void>::err("TcpTransport::listen not implemented");
}

Result<void> TcpTransport::dial(const Multiaddr& /*addr*/,
                                 TcpTransportCallbacks /*callbacks*/) {
    return Result<void>::err("TcpTransport::dial not implemented");
}

void TcpTransport::on_accept_ready(RawFd /*listen_fd*/) {
    // TODO: call accept(2) and invoke callbacks.on_accepted
}

void TcpTransport::close_all() {
    // TODO: close all listener sockets
}

}  // namespace peercore::transport
