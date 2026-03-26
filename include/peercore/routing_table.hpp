#pragma once

#include "types.hpp"

#include <vector>

namespace peercore {

// Kademlia routing table (k-bucket structure)
class RoutingTable {
public:
    explicit RoutingTable(const PeerId& local_id, size_t k = 20);

    void insert(const PeerId& peer);
    void remove(const PeerId& peer);

    std::vector<PeerId> closest_peers(const PeerId& target, size_t k) const;
    size_t size() const;

private:
    PeerId local_id_;
    [[maybe_unused]] size_t k_;

    // Flat list for the minimal implementation; replace with k-buckets later
    std::vector<PeerId> peers_;

    static uint8_t leading_zeros(const PeerId& a, const PeerId& b);
};

}  // namespace peercore
