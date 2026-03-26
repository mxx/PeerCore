#pragma once

#include "peer_store.hpp"
#include "swarm.hpp"
#include "types.hpp"
#include "services/dht_service.hpp"
#include "services/identify_service.hpp"
#include "services/ping_service.hpp"

#include <memory>
#include <vector>

namespace peercore {

class Node {
public:
    struct Options {
        Identity              identity;
        std::vector<Multiaddr> listen_addrs;
        std::vector<Multiaddr> bootstrap_addrs;
    };

    explicit Node(Options opts);
    ~Node();

    // Not copyable
    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;

    Result<void> start();
    Result<void> stop();

    Result<void>         connect(const Multiaddr& addr);
    Result<StreamHandle> open_stream(const PeerId& peer, const ProtocolId& proto);

    DhtService&      dht();
    PingService&     ping();
    IdentifyService& identify();

    DebugSnapshot snapshot() const;

private:
    Options opts_;

    PeerStore                    peer_store_;
    Swarm                        swarm_;
    std::shared_ptr<DhtService>      dht_;
    std::shared_ptr<PingService>     ping_;
    std::shared_ptr<IdentifyService> identify_;
};

}  // namespace peercore
