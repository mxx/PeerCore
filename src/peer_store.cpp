#include "../include/peercore/peer_store.hpp"

#include <algorithm>

namespace peercore {

void PeerStore::add_addr(const PeerId& peer, const Multiaddr& addr) {
    auto* e = find_or_create(peer);
    e->addrs.push_back(addr);
}

std::vector<Multiaddr> PeerStore::get_addrs(const PeerId& peer) const {
    const auto* e = find(peer);
    if (!e) return {};
    return e->addrs;
}

void PeerStore::record_dial_success(const PeerId& peer) {
    find_or_create(peer)->dial_successes++;
}

void PeerStore::record_dial_failure(const PeerId& peer) {
    find_or_create(peer)->dial_failures++;
}

PeerStore::Entry* PeerStore::find_or_create(const PeerId& peer) {
    for (auto& e : entries_) {
        if (e.peer == peer) return &e;
    }
    entries_.push_back({peer, {}, 0, 0});
    return &entries_.back();
}

const PeerStore::Entry* PeerStore::find(const PeerId& peer) const {
    for (const auto& e : entries_) {
        if (e.peer == peer) return &e;
    }
    return nullptr;
}

}  // namespace peercore
