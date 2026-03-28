#pragma once

#include "../../include/peercore/types.hpp"

#include <optional>
#include <vector>

namespace peercore::protocol {

// Implements the multistream-select 1.0 negotiation protocol.
// https://github.com/multiformats/multistream-select

struct InboundNegotiation {
    std::vector<uint8_t> outbound;
    std::optional<ProtocolId> protocol;
};

class MultistreamSelect {
public:
    // Build an initiator request for the first proposed protocol.
    static Result<std::vector<uint8_t>> prepare_outbound(
        const std::vector<ProtocolId>& proposals);

    // Parse the responder's reply for a single proposed protocol.
    static Result<ProtocolId> read_outbound_response(ConstBytes incoming,
                                                     std::string_view proposal);

    // Responder: parse one request and build the corresponding reply.
    static Result<InboundNegotiation> negotiate_inbound(
        ConstBytes incoming,
        const std::vector<ProtocolId>& supported);

private:
    static constexpr std::string_view kHeader = "/multistream/1.0.0";

    static std::vector<uint8_t> encode_message(std::string_view msg);
    static Result<std::string>  decode_next(ConstBytes& buf);
};

}  // namespace peercore::protocol
