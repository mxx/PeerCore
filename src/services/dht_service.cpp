#include "../../include/peercore/services/dht_service.hpp"

namespace peercore {

ProtocolId DhtService::protocol_id() const {
    return std::string(kProtocolId);
}

void DhtService::on_inbound_stream(StreamHandle /*stream*/) {
    // TODO: handle incoming Kad RPC
}

void DhtService::on_outbound_stream_ready(StreamHandle /*stream*/) {
    // TODO: send pending Kad request
}

void DhtService::on_tick() {
    // TODO: refresh buckets, republish records
}

void DhtService::find_peer(const PeerId& /*target*/,
                            std::function<void(std::vector<PeerId>)> callback) {
    callback({});  // TODO: initiate iterative lookup
}

void DhtService::provide(const PeerId& /*key*/) {
    // TODO: announce to closest peers
}

}  // namespace peercore
