#include "../../include/peercore/services/identify_service.hpp"

namespace peercore {

ProtocolId IdentifyService::protocol_id() const {
    return std::string(kProtocolId);
}

void IdentifyService::on_inbound_stream(StreamHandle /*stream*/) {
    // TODO: send local PeerInfo as protobuf
}

void IdentifyService::on_outbound_stream_ready(StreamHandle /*stream*/) {
    // TODO: receive remote PeerInfo
}

void IdentifyService::on_tick() {}

void IdentifyService::identify(StreamHandle stream, IdentifyCallback callback) {
    (void)stream;
    callback(std::nullopt);  // TODO: implement
}

}  // namespace peercore
