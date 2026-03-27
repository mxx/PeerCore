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
    EXPECT_EQ(parsed.error().message, "only /ip4/<addr>/tcp/<port> is supported");
}

TEST(Multiaddr, FromIp4TcpBuildsAddress) {
    auto addr = Multiaddr::from_ip4_tcp("10.0.0.8", 30303);
    EXPECT_EQ(addr.to_string(), "/ip4/10.0.0.8/tcp/30303");
}
