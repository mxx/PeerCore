#pragma once

#include "../types.hpp"
#include "../protocol_handler.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace peercore {

struct PeerInfo {
    PeerId              peer_id;
    std::vector<Multiaddr> listen_addrs;
    std::string         agent_version;
    std::string         protocol_version;
    std::vector<ProtocolId> protocols;
};

class IdentifyService : public ProtocolHandler {
public:
    static constexpr std::string_view kProtocolId = "/ipfs/id/1.0.0";

    ProtocolId protocol_id() const override;

    void on_inbound_stream(StreamHandle stream)        override;
    void on_outbound_stream_ready(StreamHandle stream) override;
    void on_tick()                                     override;

    using IdentifyCallback = std::function<void(std::optional<PeerInfo>)>;
    void identify(StreamHandle stream, IdentifyCallback callback);
};

}  // namespace peercore
