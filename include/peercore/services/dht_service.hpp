#pragma once

#include "../types.hpp"
#include "../protocol_handler.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace peercore {

class DhtService : public ProtocolHandler {
public:
    static constexpr std::string_view kProtocolId = "/kad/1.0.0";

    ProtocolId protocol_id() const override;

    void on_inbound_stream(StreamHandle stream)        override;
    void on_outbound_stream_ready(StreamHandle stream) override;
    void on_tick()                                     override;

    // DHT operations
    void find_peer(const PeerId& target,
                   std::function<void(std::vector<PeerId>)> callback);
    void provide(const PeerId& key);
};

}  // namespace peercore
