#pragma once

#include "types.hpp"

#include <vector>

namespace peercore {

class PeerStore {
public:
    void add_addr(const PeerId& peer, const Multiaddr& addr);
    std::vector<Multiaddr> get_addrs(const PeerId& peer) const;

    void record_dial_success(const PeerId& peer);
    void record_dial_failure(const PeerId& peer);

private:
    struct Entry {
        PeerId peer;
        std::vector<Multiaddr> addrs;
        uint32_t dial_successes{0};
        uint32_t dial_failures{0};
    };

    std::vector<Entry> entries_;

    Entry* find_or_create(const PeerId& peer);
    const Entry* find(const PeerId& peer) const;
};

}  // namespace peercore
