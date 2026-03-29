#pragma once

#include "../../../include/peercore/types.hpp"

#include <array>
#include <optional>
#include <vector>

namespace peercore::protocol::noise {

// Minimal libp2p Noise XX handshake over a raw transport.
// Uses libsodium for Curve25519 DH, ChaCha20-Poly1305 AEAD, and SHA-256.

struct NoiseKeypair {
    std::array<uint8_t, 32> public_key{};
    std::array<uint8_t, 32> secret_key{};
};

struct CipherState {
    std::array<uint8_t, 32> key{};
    uint64_t nonce{0};
    bool     has_key{false};
};

struct NoiseExtensions {
    std::vector<ProtocolId> stream_muxers;
};

struct NoiseSession {
    bool is_initiator{false};
    std::optional<Identity> local_identity;
    std::optional<PeerId>   remote_peer_id;
    NoiseExtensions         local_extensions;
    NoiseExtensions         remote_extensions;

    // Ephemeral keypair for this session
    NoiseKeypair ephemeral;
    NoiseKeypair static_key;
    std::optional<NoiseKeypair> configured_ephemeral;
    std::optional<NoiseKeypair> configured_static;
    std::array<uint8_t, 32> remote_ephemeral_pub{};
    bool has_remote_ephemeral{false};
    std::array<uint8_t, 32> remote_static_pub{};
    bool has_remote_static{false};

    std::array<uint8_t, 32> chaining_key{};
    std::array<uint8_t, 32> handshake_hash{};
    CipherState             handshake_cipher;

    // Derived transport cipher states (send / receive)
    CipherState cs_send;
    CipherState cs_recv;

    bool handshake_complete{false};
};

struct NoiseHandshakePayload {
    std::vector<uint8_t> identity_key;
    std::vector<uint8_t> identity_sig;
    NoiseExtensions      extensions;
};

class NoiseHandshake {
public:
    // Generate a new ephemeral keypair (calls libsodium)
    static NoiseKeypair generate_keypair();

    // Initiator: produce msg1 (→ e)
    static std::vector<uint8_t> write_msg1(NoiseSession& session);

    // Responder: consume msg1, produce msg2 (← e)
    static Result<std::vector<uint8_t>> process_msg1(NoiseSession& session,
                                                      ConstBytes msg1);

    // Initiator: consume msg2, derive transport keys, produce msg3 ack
    static Result<std::vector<uint8_t>> process_msg2(NoiseSession& session,
                                                      ConstBytes msg2);

    // Responder: consume msg3, handshake complete
    static Result<void> process_msg3(NoiseSession& session, ConstBytes msg3);

    // Encrypt / decrypt transport messages after handshake
    static Result<std::vector<uint8_t>> encrypt(CipherState& cs, ConstBytes plaintext);
    static Result<std::vector<uint8_t>> decrypt(CipherState& cs, ConstBytes ciphertext);

    // libp2p Noise handshake payload helpers
    static Result<std::vector<uint8_t>> make_handshake_payload(
        const Identity& identity,
        const NoiseKeypair& static_key,
        const NoiseExtensions& extensions = {});
    static Result<NoiseHandshakePayload> parse_handshake_payload(ConstBytes payload);
    static Result<void> verify_handshake_payload(const NoiseHandshakePayload& payload,
                                                 std::span<const uint8_t, 32> static_pubkey);

    // Wire framing helpers: 2-byte big-endian length prefix.
    static Result<std::vector<uint8_t>> encode_frame(ConstBytes message);
    static Result<std::vector<uint8_t>> decode_frame(ConstBytes frame);
};

}  // namespace peercore::protocol::noise
