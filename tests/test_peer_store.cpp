#include <gtest/gtest.h>
#include <peercore/peer_store.hpp>

using namespace peercore;

static PeerId make_peer(uint8_t byte) {
    std::array<uint8_t, 32> raw{};
    raw[0] = byte;
    return PeerId::from_bytes(raw);
}

TEST(PeerStore, AddAndGetAddrs) {
    PeerStore store;
    auto peer = make_peer(1);
    Multiaddr addr("/ip4/1.2.3.4/tcp/1234");

    store.add_addr(peer, addr);
    auto addrs = store.get_addrs(peer);
    ASSERT_EQ(addrs.size(), 1u);
    EXPECT_EQ(addrs[0].to_string(), "/ip4/1.2.3.4/tcp/1234");
}

TEST(PeerStore, UnknownPeerReturnsEmpty) {
    PeerStore store;
    auto addrs = store.get_addrs(make_peer(99));
    EXPECT_TRUE(addrs.empty());
}

TEST(PeerStore, DialStats) {
    PeerStore store;
    auto peer = make_peer(2);
    store.record_dial_success(peer);
    store.record_dial_failure(peer);
    // No crash; stats are recorded internally
}
