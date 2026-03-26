#pragma once

#include "muxed_stream.hpp"
#include "types.hpp"

namespace peercore {

class ProtocolHandler {
public:
    virtual ~ProtocolHandler() = default;

    virtual ProtocolId protocol_id() const = 0;

    virtual void on_inbound_stream(StreamHandle stream)         = 0;
    virtual void on_outbound_stream_ready(StreamHandle stream)  = 0;

    // Called periodically by the event loop
    virtual void on_tick() = 0;
};

}  // namespace peercore
