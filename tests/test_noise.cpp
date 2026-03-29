#include <gtest/gtest.h>

#include "../src/protocol/noise/noise.hpp"

#include <sodium.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

using namespace peercore;
using namespace peercore::protocol::noise;

namespace {

Identity make_identity() {
    Identity identity;
    ::crypto_sign_keypair(identity.secret_key.data() + 32, identity.secret_key.data());
    identity.peer_id = PeerId::from_bytes(std::span<const uint8_t, 32>(
        identity.secret_key.data() + 32, 32));
    return identity;
}

size_t current_rss_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),
                  MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<size_t>(info.resident_size);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    size_t total_pages = 0;
    size_t resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
        return 0;
    }
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }
    return resident_pages * static_cast<size_t>(page_size);
#else
    return 0;
#endif
}

}  // namespace

TEST(NoiseHandshake, CompletesMinimalRoundTrip) {
    NoiseSession initiator;
    NoiseSession responder;

    auto msg1 = NoiseHandshake::write_msg1(initiator);
    ASSERT_EQ(msg1.size(), 32u);

    auto msg2 = NoiseHandshake::process_msg1(responder, msg1);
    ASSERT_TRUE(msg2.is_ok());
    ASSERT_GT(msg2.value().size(), 32u);

    auto msg3 = NoiseHandshake::process_msg2(initiator, msg2.value());
    ASSERT_TRUE(msg3.is_ok());
    ASSERT_FALSE(msg3.value().empty());
    ASSERT_TRUE(initiator.handshake_complete);
    ASSERT_FALSE(responder.handshake_complete);

    auto done = NoiseHandshake::process_msg3(responder, msg3.value());
    ASSERT_TRUE(done.is_ok());
    ASSERT_TRUE(responder.handshake_complete);
}

TEST(NoiseHandshake, EncryptsAndDecryptsBothDirections) {
    NoiseSession initiator;
    NoiseSession responder;

    auto msg1 = NoiseHandshake::write_msg1(initiator);
    auto msg2 = NoiseHandshake::process_msg1(responder, msg1);
    ASSERT_TRUE(msg2.is_ok());
    auto msg3 = NoiseHandshake::process_msg2(initiator, msg2.value());
    ASSERT_TRUE(msg3.is_ok());
    ASSERT_TRUE(NoiseHandshake::process_msg3(responder, msg3.value()).is_ok());

    const std::vector<uint8_t> hello{'h', 'e', 'l', 'l', 'o'};
    auto cipher1 = NoiseHandshake::encrypt(initiator.cs_send, hello);
    ASSERT_TRUE(cipher1.is_ok());
    auto plain1 = NoiseHandshake::decrypt(responder.cs_recv, cipher1.value());
    ASSERT_TRUE(plain1.is_ok());
    EXPECT_EQ(plain1.value(), hello);

    const std::vector<uint8_t> world{'w', 'o', 'r', 'l', 'd'};
    auto cipher2 = NoiseHandshake::encrypt(responder.cs_send, world);
    ASSERT_TRUE(cipher2.is_ok());
    auto plain2 = NoiseHandshake::decrypt(initiator.cs_recv, cipher2.value());
    ASSERT_TRUE(plain2.is_ok());
    EXPECT_EQ(plain2.value(), world);
}

TEST(NoiseHandshake, RejectsBadMessages) {
    NoiseSession session;

    auto msg2 = NoiseHandshake::process_msg1(session, std::vector<uint8_t>{1, 2, 3});
    ASSERT_TRUE(msg2.is_err());
    EXPECT_EQ(msg2.error().message, "invalid msg1 size");

    auto msg3 = NoiseHandshake::process_msg2(session, std::vector<uint8_t>{1, 2, 3});
    ASSERT_TRUE(msg3.is_err());
    EXPECT_EQ(msg3.error().message, "invalid msg2 size");

    auto done = NoiseHandshake::process_msg3(session, std::vector<uint8_t>{0x02});
    ASSERT_TRUE(done.is_err());
    EXPECT_EQ(done.error_message(), "invalid msg3 size");

    NoiseSession initiator;
    NoiseSession responder;
    auto msg1_ok = NoiseHandshake::write_msg1(initiator);
    auto msg2_ok = NoiseHandshake::process_msg1(responder, msg1_ok);
    ASSERT_TRUE(msg2_ok.is_ok());

    auto short_msg3 = NoiseHandshake::process_msg3(responder, std::vector<uint8_t>{0x02});
    ASSERT_TRUE(short_msg3.is_err());
    EXPECT_EQ(short_msg3.error_message(), "invalid msg3 size");
}

TEST(NoiseHandshake, ExchangesAndVerifiesRemotePeerIdentity) {
    ASSERT_GE(::sodium_init(), 0);

    NoiseSession initiator;
    NoiseSession responder;
    initiator.local_identity = make_identity();
    responder.local_identity = make_identity();
    initiator.local_extensions.stream_muxers = {"/yamux/1.0.0"};
    responder.local_extensions.stream_muxers = {"/mplex/6.7.0", "/yamux/1.0.0"};

    auto msg1 = NoiseHandshake::write_msg1(initiator);
    ASSERT_FALSE(msg1.empty());

    auto msg2 = NoiseHandshake::process_msg1(responder, msg1);
    ASSERT_TRUE(msg2.is_ok()) << msg2.error().message;
    EXPECT_FALSE(responder.remote_peer_id.has_value());

    auto msg3 = NoiseHandshake::process_msg2(initiator, msg2.value());
    ASSERT_TRUE(msg3.is_ok()) << msg3.error().message;
    ASSERT_TRUE(initiator.remote_peer_id.has_value());
    EXPECT_EQ(*initiator.remote_peer_id, responder.local_identity->peer_id);
    ASSERT_EQ(initiator.remote_extensions.stream_muxers.size(), 2u);
    EXPECT_EQ(initiator.remote_extensions.stream_muxers[0], "/mplex/6.7.0");
    EXPECT_EQ(initiator.remote_extensions.stream_muxers[1], "/yamux/1.0.0");

    ASSERT_TRUE(NoiseHandshake::process_msg3(responder, msg3.value()).is_ok());
    ASSERT_TRUE(responder.remote_peer_id.has_value());
    EXPECT_EQ(*responder.remote_peer_id, initiator.local_identity->peer_id);
    ASSERT_EQ(responder.remote_extensions.stream_muxers.size(), 1u);
    EXPECT_EQ(responder.remote_extensions.stream_muxers[0], "/yamux/1.0.0");
}

TEST(NoiseHandshake, HandshakePayloadRoundTripsAndVerifies) {
    ASSERT_GE(::sodium_init(), 0);

    const auto identity = make_identity();
    const auto static_key = NoiseHandshake::generate_keypair();

    auto payload = NoiseHandshake::make_handshake_payload(
        identity, static_key, NoiseExtensions{.stream_muxers = {"/yamux/1.0.0", "/mplex/6.7.0"}});
    ASSERT_TRUE(payload.is_ok());

    auto parsed = NoiseHandshake::parse_handshake_payload(payload.value());
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.value().extensions.stream_muxers.size(), 2u);
    EXPECT_EQ(parsed.value().extensions.stream_muxers[0], "/yamux/1.0.0");
    EXPECT_EQ(parsed.value().extensions.stream_muxers[1], "/mplex/6.7.0");

    auto verified = NoiseHandshake::verify_handshake_payload(parsed.value(), static_key.public_key);
    EXPECT_TRUE(verified.is_ok()) << verified.error_message();
}

TEST(NoiseHandshake, RejectsInvalidPayloadSignature) {
    ASSERT_GE(::sodium_init(), 0);

    const auto identity = make_identity();
    const auto static_key = NoiseHandshake::generate_keypair();

    auto payload = NoiseHandshake::make_handshake_payload(identity, static_key);
    ASSERT_TRUE(payload.is_ok());
    auto parsed = NoiseHandshake::parse_handshake_payload(payload.value());
    ASSERT_TRUE(parsed.is_ok());

    parsed.value().identity_sig[0] ^= 0x01;
    auto verified = NoiseHandshake::verify_handshake_payload(parsed.value(), static_key.public_key);
    ASSERT_TRUE(verified.is_err());
    EXPECT_EQ(verified.error_message(), "invalid noise static key signature");
}

TEST(NoiseHandshake, RejectsTamperedHandshakeCiphertexts) {
    NoiseSession initiator;
    NoiseSession responder;

    auto msg1 = NoiseHandshake::write_msg1(initiator);
    auto msg2 = NoiseHandshake::process_msg1(responder, msg1);
    ASSERT_TRUE(msg2.is_ok());

    auto tampered_msg2 = msg2.value();
    tampered_msg2.back() ^= 0x01;
    auto bad_msg2 = NoiseHandshake::process_msg2(initiator, tampered_msg2);
    ASSERT_TRUE(bad_msg2.is_err());
    EXPECT_NE(bad_msg2.error().message.find("noise::decrypt failed"), std::string::npos);
    EXPECT_FALSE(initiator.handshake_complete);

    NoiseSession clean_initiator;
    NoiseSession clean_responder;
    auto clean_msg1 = NoiseHandshake::write_msg1(clean_initiator);
    auto clean_msg2 = NoiseHandshake::process_msg1(clean_responder, clean_msg1);
    ASSERT_TRUE(clean_msg2.is_ok());

    auto msg3 = NoiseHandshake::process_msg2(clean_initiator, clean_msg2.value());
    ASSERT_TRUE(msg3.is_ok());

    auto tampered_msg3 = msg3.value();
    tampered_msg3.back() ^= 0x01;
    auto bad_msg3 = NoiseHandshake::process_msg3(clean_responder, tampered_msg3);
    ASSERT_TRUE(bad_msg3.is_err());
    EXPECT_NE(bad_msg3.error_message().find("noise::decrypt failed"), std::string::npos);
    EXPECT_FALSE(clean_responder.handshake_complete);
}

TEST(NoiseHandshake, FramesMessagesWithTwoByteLengthPrefix) {
    const std::vector<uint8_t> msg{'n', 'o', 'i', 's', 'e'};
    auto framed = NoiseHandshake::encode_frame(msg);
    ASSERT_TRUE(framed.is_ok());
    ASSERT_EQ(framed.value().size(), msg.size() + 2);
    EXPECT_EQ(framed.value()[0], 0x00);
    EXPECT_EQ(framed.value()[1], 0x05);

    auto decoded = NoiseHandshake::decode_frame(framed.value());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded.value(), msg);
}

TEST(NoiseHandshake, FrameSupportsMaximumPayloadSize) {
    const std::vector<uint8_t> msg(std::numeric_limits<uint16_t>::max(), 0xAB);
    auto framed = NoiseHandshake::encode_frame(msg);
    ASSERT_TRUE(framed.is_ok());
    ASSERT_EQ(framed.value().size(), msg.size() + 2);

    auto decoded = NoiseHandshake::decode_frame(framed.value());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded.value(), msg);
}

TEST(NoiseHandshake, RejectsMalformedFrameLengths) {
    const std::vector<uint8_t> too_short{0x00, 0x05, 'a', 'b', 'c', 'd'};
    auto short_result = NoiseHandshake::decode_frame(too_short);
    ASSERT_TRUE(short_result.is_err());
    EXPECT_EQ(short_result.error().message, "invalid noise frame length");

    const std::vector<uint8_t> too_long{0x00, 0x02, 'a', 'b', 'c'};
    auto long_result = NoiseHandshake::decode_frame(too_long);
    ASSERT_TRUE(long_result.is_err());
    EXPECT_EQ(long_result.error().message, "invalid noise frame length");
}

TEST(NoiseHandshake, RejectsNonceExhaustion) {
    CipherState cs{};
    cs.has_key = true;
    cs.key.fill(0x11);
    cs.nonce = std::numeric_limits<uint64_t>::max();

    const std::vector<uint8_t> payload{'x'};
    auto encrypted = NoiseHandshake::encrypt(cs, payload);
    ASSERT_TRUE(encrypted.is_err());
    EXPECT_EQ(encrypted.error().message, "noise nonce exhausted");

    auto decrypted = NoiseHandshake::decrypt(cs, std::vector<uint8_t>(16, 0x00));
    ASSERT_TRUE(decrypted.is_err());
    EXPECT_EQ(decrypted.error().message, "noise nonce exhausted");
}

TEST(NoiseHandshake, RejectsTruncatedOrMalformedHandshakePayload) {
    ASSERT_GE(::sodium_init(), 0);

    const auto identity = make_identity();
    const auto static_key = NoiseHandshake::generate_keypair();

    auto payload = NoiseHandshake::make_handshake_payload(identity, static_key);
    ASSERT_TRUE(payload.is_ok());
    ASSERT_GT(payload.value().size(), 8u);

    auto truncated = payload.value();
    truncated.pop_back();
    auto parsed_truncated = NoiseHandshake::parse_handshake_payload(truncated);
    ASSERT_TRUE(parsed_truncated.is_err());
    EXPECT_EQ(parsed_truncated.error().message, "invalid noise handshake payload");

    const std::vector<uint8_t> malformed_wire_type{0x08, 0x01};
    auto parsed_malformed = NoiseHandshake::parse_handshake_payload(malformed_wire_type);
    ASSERT_TRUE(parsed_malformed.is_err());
    EXPECT_EQ(parsed_malformed.error().message, "invalid noise handshake payload");
}

TEST(NoiseHandshake, FatigueHandshakeAndTransportKeepsMemoryBounded) {
    constexpr size_t kWarmupIterations = 128;
    constexpr size_t kIterations = 2048;
    constexpr size_t kMaxAllowedGrowthBytes = 48ull * 1024ull * 1024ull;

    auto one_round = [](uint8_t marker) -> bool {
        NoiseSession initiator;
        NoiseSession responder;

        auto msg1 = NoiseHandshake::write_msg1(initiator);
        if (msg1.empty()) return false;
        auto msg2 = NoiseHandshake::process_msg1(responder, msg1);
        if (msg2.is_err()) return false;
        auto msg3 = NoiseHandshake::process_msg2(initiator, msg2.value());
        if (msg3.is_err()) return false;
        auto done = NoiseHandshake::process_msg3(responder, msg3.value());
        if (done.is_err()) return false;

        std::vector<uint8_t> payload(256, marker);
        auto cipher1 = NoiseHandshake::encrypt(initiator.cs_send, payload);
        if (cipher1.is_err()) return false;
        auto plain1 = NoiseHandshake::decrypt(responder.cs_recv, cipher1.value());
        if (plain1.is_err() || plain1.value() != payload) return false;

        payload[0] ^= 0x5A;
        auto cipher2 = NoiseHandshake::encrypt(responder.cs_send, payload);
        if (cipher2.is_err()) return false;
        auto plain2 = NoiseHandshake::decrypt(initiator.cs_recv, cipher2.value());
        if (plain2.is_err() || plain2.value() != payload) return false;

        return true;
    };

    for (size_t i = 0; i < kWarmupIterations; ++i) {
        ASSERT_TRUE(one_round(static_cast<uint8_t>(i)));
    }

    const size_t rss_before = current_rss_bytes();
    for (size_t i = 0; i < kIterations; ++i) {
        ASSERT_TRUE(one_round(static_cast<uint8_t>(i)));
    }
    const size_t rss_after = current_rss_bytes();

    if (rss_before > 0 && rss_after > 0) {
        const size_t growth = (rss_after > rss_before) ? (rss_after - rss_before) : 0;
        EXPECT_LT(growth, kMaxAllowedGrowthBytes)
            << "rss_before=" << rss_before << ", rss_after=" << rss_after;
    }
}
