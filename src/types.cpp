#include "../include/peercore/types.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace peercore {

namespace {

constexpr uint8_t kIdentityMultihashCode = 0x00;
constexpr uint8_t kIdentityPayloadLength = 36;
constexpr std::array<uint8_t, 4> kEd25519PublicKeyPrefix{{0x08, 0x01, 0x12, 0x20}};
constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

Result<Multiaddr::Ip4TcpEndpoint> parse_ip4_tcp_text(std::string_view text) {
    std::array<std::string, 7> parts{};

    size_t start = 0;
    size_t index = 0;
    while (start <= text.size() && index < parts.size()) {
        const size_t slash = text.find('/', start);
        const size_t end = slash == std::string_view::npos ? text.size() : slash;
        if (end > start) {
            parts[index++] = std::string(text.substr(start, end - start));
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }

    if ((index != 4 && index != 6) || parts[0] != "ip4" || parts[2] != "tcp") {
        return Result<Multiaddr::Ip4TcpEndpoint>::err(
            "only /ip4/<addr>/tcp/<port>[/p2p/<peer-id>] is supported");
    }

    char* port_end = nullptr;
    const long port = std::strtol(parts[3].c_str(), &port_end, 10);
    if (port_end == nullptr || *port_end != '\0' || port < 0 || port > 65535) {
        return Result<Multiaddr::Ip4TcpEndpoint>::err("invalid tcp port");
    }

    std::optional<std::string> peer_id;
    if (index == 6) {
        if (parts[4] != "p2p" || parts[5].empty()) {
            return Result<Multiaddr::Ip4TcpEndpoint>::err("invalid p2p peer id");
        }
        peer_id = std::move(parts[5]);
    }

    return Result<Multiaddr::Ip4TcpEndpoint>::ok(Multiaddr::Ip4TcpEndpoint{
        .ip = std::move(parts[1]),
        .port = static_cast<uint16_t>(port),
        .peer_id = std::move(peer_id),
    });
}

std::optional<uint8_t> base58_value(char c) {
    const auto pos = kBase58Alphabet.find(c);
    if (pos == std::string_view::npos) return std::nullopt;
    return static_cast<uint8_t>(pos);
}

std::string encode_base58(ConstBytes data) {
    size_t leading_zeroes = 0;
    while (leading_zeroes < data.size() && data[leading_zeroes] == 0) {
        ++leading_zeroes;
    }

    std::vector<uint8_t> digits;
    digits.reserve(data.size() * 2);

    for (const auto byte : data) {
        uint32_t carry = byte;
        for (auto& digit : digits) {
            carry += static_cast<uint32_t>(digit) << 8;
            digit = static_cast<uint8_t>(carry % 58);
            carry /= 58;
        }
        while (carry > 0) {
            digits.push_back(static_cast<uint8_t>(carry % 58));
            carry /= 58;
        }
    }

    std::string out;
    out.reserve(leading_zeroes + digits.size());
    out.append(leading_zeroes, '1');
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        out.push_back(kBase58Alphabet[*it]);
    }
    if (out.empty()) out = "1";
    return out;
}

Result<std::vector<uint8_t>> decode_base58(std::string_view text) {
    size_t leading_zeroes = 0;
    while (leading_zeroes < text.size() && text[leading_zeroes] == '1') {
        ++leading_zeroes;
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(text.size());

    for (const auto ch : text) {
        const auto value = base58_value(ch);
        if (!value.has_value()) {
            return Result<std::vector<uint8_t>>::err("invalid base58 peer id");
        }

        uint32_t carry = *value;
        for (auto& byte : bytes) {
            carry += static_cast<uint32_t>(byte) * 58;
            byte = static_cast<uint8_t>(carry & 0xFF);
            carry >>= 8;
        }
        while (carry > 0) {
            bytes.push_back(static_cast<uint8_t>(carry & 0xFF));
            carry >>= 8;
        }
    }

    std::vector<uint8_t> decoded;
    decoded.reserve(leading_zeroes + bytes.size());
    decoded.insert(decoded.end(), leading_zeroes, 0);
    decoded.insert(decoded.end(), bytes.rbegin(), bytes.rend());
    return Result<std::vector<uint8_t>>::ok(std::move(decoded));
}

}  // namespace

// ── PeerId ────────────────────────────────────────────────────────────────────

std::string PeerId::to_string() const {
    std::array<uint8_t, 2 + kEd25519PublicKeyPrefix.size() + 32> payload{};
    payload[0] = kIdentityMultihashCode;
    payload[1] = kIdentityPayloadLength;
    std::copy(kEd25519PublicKeyPrefix.begin(), kEd25519PublicKeyPrefix.end(), payload.begin() + 2);
    std::copy(bytes.begin(), bytes.end(), payload.begin() + 2 + kEd25519PublicKeyPrefix.size());
    return encode_base58(payload);
}

PeerId PeerId::from_bytes(std::span<const uint8_t, 32> b) {
    PeerId p;
    std::memcpy(p.bytes.data(), b.data(), 32);
    return p;
}

Result<PeerId> PeerId::from_string(std::string_view text) {
    const auto decoded = decode_base58(text);
    if (decoded.is_err()) return Result<PeerId>::err(decoded.error().message);

    PeerId p;
    const auto& bytes = decoded.value();
    if (bytes.size() != 2 + kEd25519PublicKeyPrefix.size() + p.bytes.size()) {
        return Result<PeerId>::err("invalid peer id payload");
    }
    if (bytes[0] != kIdentityMultihashCode || bytes[1] != kIdentityPayloadLength) {
        return Result<PeerId>::err("unsupported peer id multihash");
    }
    if (!std::equal(kEd25519PublicKeyPrefix.begin(),
                    kEd25519PublicKeyPrefix.end(),
                    bytes.begin() + 2)) {
        return Result<PeerId>::err("unsupported peer id key type");
    }
    std::copy(bytes.begin() + 2 + kEd25519PublicKeyPrefix.size(), bytes.end(), p.bytes.begin());
    return Result<PeerId>::ok(p);
}

// ── Multiaddr ─────────────────────────────────────────────────────────────────

Multiaddr::Multiaddr(std::string_view text) {
    // Minimal: store raw UTF-8 bytes for now
    // TODO: parse proper multiaddr binary encoding
    bytes.assign(text.begin(), text.end());
}

std::string Multiaddr::to_string() const {
    return std::string(bytes.begin(), bytes.end());
}

Result<Multiaddr::Ip4TcpEndpoint> Multiaddr::parse_ip4_tcp() const {
    return parse_ip4_tcp_text(to_string());
}

Multiaddr Multiaddr::from_ip4_tcp(std::string_view ip, uint16_t port) {
    return Multiaddr(std::string("/ip4/") + std::string(ip) +
                     "/tcp/" + std::to_string(port));
}

Multiaddr Multiaddr::from_ip4_tcp(std::string_view ip,
                                  uint16_t port,
                                  std::string_view peer_id) {
    return Multiaddr(std::string("/ip4/") + std::string(ip) +
                     "/tcp/" + std::to_string(port) +
                     "/p2p/" + std::string(peer_id));
}

}  // namespace peercore
