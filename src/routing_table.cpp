#include "../include/peercore/routing_table.hpp"

#include <algorithm>
#include <bit>

namespace peercore {

RoutingTable::RoutingTable(const PeerId& local_id, size_t k)
    : local_id_(local_id), k_(k) {}

void RoutingTable::insert(const PeerId& peer) {
    if (peer == local_id_) return;
    for (const auto& p : peers_) {
        if (p == peer) return;
    }
    peers_.push_back(peer);
}

void RoutingTable::remove(const PeerId& peer) {
    peers_.erase(std::remove(peers_.begin(), peers_.end(), peer), peers_.end());
}

std::vector<PeerId> RoutingTable::closest_peers(const PeerId& target, size_t k) const {
    // XOR distance sort
    auto xor_dist = [&](const PeerId& a, const PeerId& b) {
        std::array<uint8_t, 32> d{};
        for (size_t i = 0; i < 32; ++i) d[i] = a.bytes[i] ^ b.bytes[i];
        return d;
    };

    std::vector<PeerId> sorted = peers_;
    std::sort(sorted.begin(), sorted.end(), [&](const PeerId& a, const PeerId& b) {
        return xor_dist(a, target) < xor_dist(b, target);
    });

    if (sorted.size() > k) sorted.resize(k);
    return sorted;
}

size_t RoutingTable::size() const { return peers_.size(); }

uint8_t RoutingTable::leading_zeros(const PeerId& a, const PeerId& b) {
    for (size_t i = 0; i < 32; ++i) {
        uint8_t xb = a.bytes[i] ^ b.bytes[i];
        if (xb != 0) return static_cast<uint8_t>(i * 8 + std::countl_zero(xb));
    }
    return 255;  // all bits equal → max distance sentinel
}

}  // namespace peercore
