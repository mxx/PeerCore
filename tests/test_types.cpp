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
