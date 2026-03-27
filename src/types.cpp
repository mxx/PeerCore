#include "../include/peercore/types.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace peercore {

namespace {

Result<Multiaddr::Ip4TcpEndpoint> parse_ip4_tcp_text(std::string_view text) {
    std::array<std::string, 5> parts{};

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

    if (index != 4 || parts[0] != "ip4" || parts[2] != "tcp") {
        return Result<Multiaddr::Ip4TcpEndpoint>::err(
            "only /ip4/<addr>/tcp/<port> is supported");
    }

    char* port_end = nullptr;
    const long port = std::strtol(parts[3].c_str(), &port_end, 10);
    if (port_end == nullptr || *port_end != '\0' || port < 0 || port > 65535) {
        return Result<Multiaddr::Ip4TcpEndpoint>::err("invalid tcp port");
    }

    return Result<Multiaddr::Ip4TcpEndpoint>::ok(Multiaddr::Ip4TcpEndpoint{
        .ip = std::move(parts[1]),
        .port = static_cast<uint16_t>(port),
    });
}

}  // namespace

// ── PeerId ────────────────────────────────────────────────────────────────────

std::string PeerId::to_string() const {
    // Base58btc representation (stub: hex for now)
    std::ostringstream oss;
    oss << "12D3KooW";  // placeholder prefix
    for (uint8_t b : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

PeerId PeerId::from_bytes(std::span<const uint8_t, 32> b) {
    PeerId p;
    std::memcpy(p.bytes.data(), b.data(), 32);
    return p;
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

}  // namespace peercore
