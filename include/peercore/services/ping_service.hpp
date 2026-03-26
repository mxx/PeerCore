#pragma once

#include "../types.hpp"
#include "../protocol_handler.hpp"

#include <chrono>
#include <functional>
#include <optional>

namespace peercore {

class PingService : public ProtocolHandler {
public:
    static constexpr std::string_view kProtocolId = "/ipfs/ping/1.0.0";

    ProtocolId protocol_id() const override;

    void on_inbound_stream(StreamHandle stream)        override;
    void on_outbound_stream_ready(StreamHandle stream) override;
    void on_tick()                                     override;

    using RttCallback = std::function<void(std::optional<std::chrono::milliseconds>)>;

    void ping(StreamHandle stream, RttCallback callback);
};

}  // namespace peercore
