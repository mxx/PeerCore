#pragma once

#include "../../include/peercore/types.hpp"

#include <functional>
#include <vector>

namespace peercore::protocol {

// Implements the multistream-select 1.0 negotiation protocol.
// https://github.com/multiformats/multistream-select

using NegotiateCallback = std::function<void(Result<ProtocolId>)>;

class MultistreamSelect {
public:
    // Initiator: propose a list of protocols; callback receives the agreed one
    static void negotiate_outbound(MutableBytes send_buf,
                                   const std::vector<ProtocolId>& proposals,
                                   NegotiateCallback callback);

    // Responder: given supported protocols, select the first match
    static void negotiate_inbound(MutableBytes send_buf,
                                  ConstBytes incoming,
                                  const std::vector<ProtocolId>& supported,
                                  NegotiateCallback callback);

private:
    static constexpr std::string_view kHeader = "/multistream/1.0.0\n";

    static std::vector<uint8_t> encode_message(std::string_view msg);
    static std::string          decode_next(ConstBytes& buf);
};

}  // namespace peercore::protocol
