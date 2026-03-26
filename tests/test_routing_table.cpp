#include <gtest/gtest.h>
#include <peercore/routing_table.hpp>

using namespace peercore;

static PeerId make_peer(uint8_t byte) {
    std::array<uint8_t, 32> raw{};
    raw[0] = byte;
    return PeerId::from_bytes(raw);
}

TEST(RoutingTable, InsertAndSize) {
    RoutingTable rt(make_peer(0));
    rt.insert(make_peer(1));
    rt.insert(make_peer(2));
    EXPECT_EQ(rt.size(), 2u);
}

TEST(RoutingTable, IgnoresSelf) {
    auto local = make_peer(0);
    RoutingTable rt(local);
    rt.insert(local);
    EXPECT_EQ(rt.size(), 0u);
}

TEST(RoutingTable, NoDuplicates) {
    RoutingTable rt(make_peer(0));
    rt.insert(make_peer(1));
    rt.insert(make_peer(1));
    EXPECT_EQ(rt.size(), 1u);
}

TEST(RoutingTable, Remove) {
    RoutingTable rt(make_peer(0));
    rt.insert(make_peer(1));
    rt.remove(make_peer(1));
    EXPECT_EQ(rt.size(), 0u);
}

TEST(RoutingTable, ClosestPeers) {
    auto local = make_peer(0);
    RoutingTable rt(local);
    for (uint8_t i = 1; i <= 10; ++i) rt.insert(make_peer(i));

    auto closest = rt.closest_peers(make_peer(1), 3);
    EXPECT_EQ(closest.size(), 3u);
}
