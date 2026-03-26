#include "../include/peercore/node.hpp"

namespace peercore {

Node::Node(Options opts)
    : opts_(std::move(opts))
    , swarm_(peer_store_)
    , dht_(std::make_shared<DhtService>())
    , ping_(std::make_shared<PingService>())
    , identify_(std::make_shared<IdentifyService>()) {
    swarm_.register_handler(dht_);
    swarm_.register_handler(ping_);
    swarm_.register_handler(identify_);
}

Node::~Node() { stop(); }

Result<void> Node::start() {
    for (const auto& addr : opts_.listen_addrs) {
        auto res = swarm_.listen_on(addr);
        if (res.is_err()) return res;
    }
    auto res = swarm_.start();
    if (res.is_err()) return res;

    for (const auto& addr : opts_.bootstrap_addrs) {
        swarm_.dial_addr(addr);
    }
    return Result<void>::ok();
}

Result<void> Node::stop() {
    return swarm_.stop();
}

Result<void> Node::connect(const Multiaddr& addr) {
    return swarm_.dial_addr(addr);
}

Result<StreamHandle> Node::open_stream(const PeerId& /*peer*/,
                                        const ProtocolId& /*proto*/) {
    // TODO: look up existing connection for peer, then open stream
    return Result<StreamHandle>::err("Node::open_stream not implemented");
}

DhtService&      Node::dht()      { return *dht_; }
PingService&     Node::ping()     { return *ping_; }
IdentifyService& Node::identify() { return *identify_; }

DebugSnapshot Node::snapshot() const {
    return swarm_.snapshot();
}

}  // namespace peercore
