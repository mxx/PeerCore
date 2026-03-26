#include "../../include/peercore/services/ping_service.hpp"

namespace peercore {

ProtocolId PingService::protocol_id() const {
    return std::string(kProtocolId);
}

void PingService::on_inbound_stream(StreamHandle stream) {
    // Reflect 32 bytes back to sender
    (void)stream;
    // TODO: read 32 bytes, write them back
}

void PingService::on_outbound_stream_ready(StreamHandle /*stream*/) {
    // TODO: send 32 random bytes, start RTT timer
}

void PingService::on_tick() {}

void PingService::ping(StreamHandle stream, RttCallback callback) {
    (void)stream;
    callback(std::nullopt);  // TODO: implement
}

}  // namespace peercore
