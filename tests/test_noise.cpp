#include <gtest/gtest.h>

#include "../src/protocol/noise/noise.hpp"

#include <sodium.h>

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
    EXPECT_EQ(done.error_message(), "missing msg3 handshake secret");

    NoiseSession initiator;
    NoiseSession responder;
    auto msg1_ok = NoiseHandshake::write_msg1(initiator);
    auto msg2_ok = NoiseHandshake::process_msg1(responder, msg1_ok);
    ASSERT_TRUE(msg2_ok.is_ok());

    auto short_msg3 = NoiseHandshake::process_msg3(responder, std::vector<uint8_t>{0x02});
    ASSERT_TRUE(short_msg3.is_err());
    EXPECT_EQ(short_msg3.error_message(), "ciphertext too short");
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
