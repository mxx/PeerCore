#include "multistream_select.hpp"

// TODO: implement length-prefixed message framing and protocol negotiation

namespace peercore::protocol {

void MultistreamSelect::negotiate_outbound(MutableBytes /*send_buf*/,
                                            const std::vector<ProtocolId>& /*proposals*/,
                                            NegotiateCallback callback) {
    callback(Result<ProtocolId>::err("multistream_select::negotiate_outbound not implemented"));
}

void MultistreamSelect::negotiate_inbound(MutableBytes /*send_buf*/,
                                           ConstBytes /*incoming*/,
                                           const std::vector<ProtocolId>& /*supported*/,
                                           NegotiateCallback callback) {
    callback(Result<ProtocolId>::err("multistream_select::negotiate_inbound not implemented"));
}

std::vector<uint8_t> MultistreamSelect::encode_message(std::string_view msg) {
    // varint-length-prefixed message
    std::vector<uint8_t> out;
    // TODO: encode varint(msg.size()) + msg + '\n'
    (void)msg;
    return out;
}

std::string MultistreamSelect::decode_next(ConstBytes& /*buf*/) {
    // TODO: read varint-length-prefixed message
    return {};
}

}  // namespace peercore::protocol
