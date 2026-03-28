#include <gtest/gtest.h>
#include <peercore/types.hpp>

using namespace peercore;

TEST(PeerId, EqualityAndFromBytes) {
    std::array<uint8_t, 32> raw{};
    raw[0] = 0xAB;
    auto a = PeerId::from_bytes(raw);
    auto b = PeerId::from_bytes(raw);
    EXPECT_EQ(a, b);
}

TEST(PeerId, StringRoundTrip) {
    std::array<uint8_t, 32> raw{};
    raw[0] = 0x12;
    raw[31] = 0xEF;
    auto peer = PeerId::from_bytes(raw);

    auto parsed = PeerId::from_string(peer.to_string());
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.value(), peer);
}

TEST(Result, OkAndErr) {
    auto ok  = Result<int>::ok(42);
    auto err = Result<int>::err("oops");
    EXPECT_TRUE(ok.is_ok());
    EXPECT_EQ(ok.value(), 42);
    EXPECT_TRUE(err.is_err());
    EXPECT_EQ(err.error().message, "oops");
}

TEST(ResultVoid, OkAndErr) {
    auto ok  = Result<void>::ok();
    auto err = Result<void>::err("fail");
    EXPECT_TRUE(ok.is_ok());
    EXPECT_TRUE(err.is_err());
}

TEST(Multiaddr, RoundTrip) {
    Multiaddr addr("/ip4/127.0.0.1/tcp/4001");
    EXPECT_EQ(addr.to_string(), "/ip4/127.0.0.1/tcp/4001");
}

TEST(Multiaddr, ParseIp4Tcp) {
    Multiaddr addr("/ip4/127.0.0.1/tcp/4001");
    auto parsed = addr.parse_ip4_tcp();
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.value().ip, "127.0.0.1");
    EXPECT_EQ(parsed.value().port, 4001);
}

TEST(Multiaddr, ParseIp4TcpRejectsUnsupportedShape) {
    Multiaddr addr("/ip4/127.0.0.1/udp/4001");
    auto parsed = addr.parse_ip4_tcp();
    ASSERT_TRUE(parsed.is_err());
    EXPECT_EQ(parsed.error().message, "only /ip4/<addr>/tcp/<port>[/p2p/<peer-id>] is supported");
}

TEST(Multiaddr, FromIp4TcpBuildsAddress) {
    auto addr = Multiaddr::from_ip4_tcp("10.0.0.8", 30303);
    EXPECT_EQ(addr.to_string(), "/ip4/10.0.0.8/tcp/30303");
}

TEST(Multiaddr, ParseIp4TcpWithPeerId) {
    Multiaddr addr("/ip4/127.0.0.1/tcp/4001/p2p/12D3KooW00112233445566778899aabbccddeeff00112233445566778899aabb");
    auto parsed = addr.parse_ip4_tcp();
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.value().ip, "127.0.0.1");
    EXPECT_EQ(parsed.value().port, 4001);
    ASSERT_TRUE(parsed.value().peer_id.has_value());
    EXPECT_EQ(*parsed.value().peer_id,
              "12D3KooW00112233445566778899aabbccddeeff00112233445566778899aabb");
}

TEST(Multiaddr, ParseIp4TcpRejectsEmptyPeerId) {
    Multiaddr addr("/ip4/127.0.0.1/tcp/4001/p2p");
    auto parsed = addr.parse_ip4_tcp();
    ASSERT_TRUE(parsed.is_err());
    EXPECT_EQ(parsed.error().message, "only /ip4/<addr>/tcp/<port>[/p2p/<peer-id>] is supported");
}

TEST(Multiaddr, FromIp4TcpWithPeerIdBuildsAddress) {
    auto addr = Multiaddr::from_ip4_tcp(
        "10.0.0.8", 30303, "12D3KooW00112233445566778899aabbccddeeff00112233445566778899aabb");
    EXPECT_EQ(addr.to_string(),
              "/ip4/10.0.0.8/tcp/30303/p2p/12D3KooW00112233445566778899aabbccddeeff00112233445566778899aabb");
}
