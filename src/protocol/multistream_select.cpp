#include "multistream_select.hpp"

#include <string>

namespace peercore::protocol {

namespace {

std::vector<uint8_t> encode_uvarint(uint64_t value) {
    std::vector<uint8_t> out;
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
    return out;
}

Result<uint64_t> decode_uvarint(ConstBytes& buf) {
    uint64_t value = 0;
    uint32_t shift = 0;
    size_t index = 0;

    while (index < buf.size()) {
        const uint8_t byte = buf[index++];
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            buf = buf.subspan(index);
            return Result<uint64_t>::ok(value);
        }
        shift += 7;
        if (shift >= 64) {
            return Result<uint64_t>::err("invalid multistream length");
        }
    }

    return Result<uint64_t>::err("incomplete multistream length");
}

std::string ensure_newline(std::string_view msg) {
    if (!msg.empty() && msg.back() == '\n') return std::string(msg);
    return std::string(msg) + "\n";
}

}  // namespace

Result<std::vector<uint8_t>> MultistreamSelect::prepare_outbound(
    const std::vector<ProtocolId>& proposals) {
    if (proposals.empty()) {
        return Result<std::vector<uint8_t>>::err("no protocols proposed");
    }

    auto header = encode_message(kHeader);
    auto proposal = encode_message(proposals.front());

    std::vector<uint8_t> out;
    out.reserve(header.size() + proposal.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), proposal.begin(), proposal.end());
    return Result<std::vector<uint8_t>>::ok(std::move(out));
}

Result<ProtocolId> MultistreamSelect::read_outbound_response(ConstBytes incoming,
                                                             std::string_view proposal) {
    auto header = decode_next(incoming);
    if (header.is_err()) return Result<ProtocolId>::err(header.error().message);
    if (header.value() != kHeader) {
        return Result<ProtocolId>::err("unexpected multistream header");
    }

    auto token = decode_next(incoming);
    if (token.is_err()) return Result<ProtocolId>::err(token.error().message);
    if (token.value() == proposal) {
        return Result<ProtocolId>::ok(std::move(token.value()));
    }
    if (token.value() == "na") {
        return Result<ProtocolId>::err("protocol not supported");
    }
    return Result<ProtocolId>::err("unexpected multistream response");
}

Result<InboundNegotiation> MultistreamSelect::negotiate_inbound(
    ConstBytes incoming,
    const std::vector<ProtocolId>& supported) {
    auto header = decode_next(incoming);
    if (header.is_err()) return Result<InboundNegotiation>::err(header.error().message);
    if (header.value() != kHeader) {
        return Result<InboundNegotiation>::err("unexpected multistream header");
    }

    auto proposal = decode_next(incoming);
    if (proposal.is_err()) return Result<InboundNegotiation>::err(proposal.error().message);

    InboundNegotiation result;
    auto encoded_header = encode_message(kHeader);
    result.outbound.insert(result.outbound.end(), encoded_header.begin(), encoded_header.end());

    for (const auto& supported_proto : supported) {
        if (supported_proto != proposal.value()) continue;
        auto encoded_proto = encode_message(supported_proto);
        result.outbound.insert(result.outbound.end(), encoded_proto.begin(), encoded_proto.end());
        result.protocol = supported_proto;
        return Result<InboundNegotiation>::ok(std::move(result));
    }

    auto encoded_na = encode_message("na");
    result.outbound.insert(result.outbound.end(), encoded_na.begin(), encoded_na.end());
    return Result<InboundNegotiation>::ok(std::move(result));
}

std::vector<uint8_t> MultistreamSelect::encode_message(std::string_view msg) {
    const auto payload = ensure_newline(msg);
    auto out = encode_uvarint(payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Result<std::string> MultistreamSelect::decode_next(ConstBytes& buf) {
    auto len = decode_uvarint(buf);
    if (len.is_err()) return Result<std::string>::err(len.error().message);
    if (buf.size() < len.value()) {
        return Result<std::string>::err("incomplete multistream message");
    }

    std::string token(reinterpret_cast<const char*>(buf.data()),
                      reinterpret_cast<const char*>(buf.data() + len.value()));
    buf = buf.subspan(len.value());

    if (token.empty() || token.back() != '\n') {
        return Result<std::string>::err("invalid multistream message");
    }
    token.pop_back();
    return Result<std::string>::ok(std::move(token));
}

}  // namespace peercore::protocol
