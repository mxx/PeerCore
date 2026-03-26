#include "../include/peercore/types.hpp"

#include <cstring>
#include <sstream>
#include <iomanip>

namespace peercore {

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

}  // namespace peercore
