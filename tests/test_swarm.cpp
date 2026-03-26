#include <gtest/gtest.h>
#include <peercore/swarm.hpp>
#include <peercore/peer_store.hpp>

using namespace peercore;

TEST(Swarm, StartStop) {
    PeerStore ps;
    Swarm swarm(ps);
    EXPECT_TRUE(swarm.start().is_ok());
    EXPECT_TRUE(swarm.stop().is_ok());
}

TEST(Swarm, NoEventsInitially) {
    PeerStore ps;
    Swarm swarm(ps);
    swarm.start();
    EXPECT_FALSE(swarm.next_event().has_value());
}

TEST(Swarm, DialUnknownPeerFails) {
    PeerStore ps;
    Swarm swarm(ps);
    std::array<uint8_t, 32> raw{};
    auto peer = PeerId::from_bytes(raw);
    auto res = swarm.dial_peer(peer);
    EXPECT_TRUE(res.is_err());
}

TEST(Swarm, Snapshot) {
    PeerStore ps;
    Swarm swarm(ps);
    auto snap = swarm.snapshot();
    EXPECT_EQ(snap.connection_count, 0u);
}
