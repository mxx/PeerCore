#pragma once

#include "../../../include/peercore/types.hpp"

#include <array>
#include <vector>

namespace peercore::protocol::noise {

// Noise_XX handshake pattern over a raw TCP socket.
// Uses libsodium for Curve25519 DH, ChaCha20-Poly1305 AEAD, and SHA-256.

struct NoiseKeypair {
    std::array<uint8_t, 32> public_key{};
    std::array<uint8_t, 32> secret_key{};
};

struct CipherState {
    std::array<uint8_t, 32> key{};
    uint64_t nonce{0};
};

struct NoiseSession {
    bool is_initiator{false};

    // Ephemeral keypair for this session
    NoiseKeypair ephemeral;

    // Remote static public key (available after handshake)
    std::array<uint8_t, 32> remote_static_pub{};

    // Derived transport cipher states (send / receive)
    CipherState cs_send;
    CipherState cs_recv;

    bool handshake_complete{false};
};

class NoiseHandshake {
public:
    // Generate a new ephemeral keypair (calls libsodium)
    static NoiseKeypair generate_keypair();

    // Initiator: produce msg1 (→ e)
    static std::vector<uint8_t> write_msg1(NoiseSession& session);

    // Responder: consume msg1, produce msg2 (← e, ee, s, es)
    static Result<std::vector<uint8_t>> process_msg1(NoiseSession& session,
                                                      ConstBytes msg1);

    // Initiator: consume msg2, produce msg3 (→ s, se)
    static Result<std::vector<uint8_t>> process_msg2(NoiseSession& session,
                                                      ConstBytes msg2);

    // Responder: consume msg3, handshake complete
    static Result<void> process_msg3(NoiseSession& session, ConstBytes msg3);

    // Encrypt / decrypt transport messages after handshake
    static Result<std::vector<uint8_t>> encrypt(CipherState& cs, ConstBytes plaintext);
    static Result<std::vector<uint8_t>> decrypt(CipherState& cs, ConstBytes ciphertext);
};

}  // namespace peercore::protocol::noise
