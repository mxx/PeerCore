#include "noise.hpp"

#include <sodium.h>

// TODO: implement full Noise_XX handshake using libsodium primitives:
//   - crypto_scalarmult_curve25519 for DH
//   - crypto_aead_chacha20poly1305_ietf for AEAD
//   - crypto_hash_sha256 for hashing

namespace peercore::protocol::noise {

NoiseKeypair NoiseHandshake::generate_keypair() {
    NoiseKeypair kp;
    crypto_box_keypair(kp.public_key.data(), kp.secret_key.data());
    return kp;
}

std::vector<uint8_t> NoiseHandshake::write_msg1(NoiseSession& session) {
    session.ephemeral = generate_keypair();
    // TODO: return serialised ephemeral public key
    (void)session;
    return {};
}

Result<std::vector<uint8_t>> NoiseHandshake::process_msg1(NoiseSession& session,
                                                            ConstBytes /*msg1*/) {
    (void)session;
    return Result<std::vector<uint8_t>>::err("noise::process_msg1 not implemented");
}

Result<std::vector<uint8_t>> NoiseHandshake::process_msg2(NoiseSession& session,
                                                            ConstBytes /*msg2*/) {
    (void)session;
    return Result<std::vector<uint8_t>>::err("noise::process_msg2 not implemented");
}

Result<void> NoiseHandshake::process_msg3(NoiseSession& session, ConstBytes /*msg3*/) {
    (void)session;
    return Result<void>::err("noise::process_msg3 not implemented");
}

Result<std::vector<uint8_t>> NoiseHandshake::encrypt(CipherState& cs,
                                                       ConstBytes plaintext) {
    (void)cs; (void)plaintext;
    return Result<std::vector<uint8_t>>::err("noise::encrypt not implemented");
}

Result<std::vector<uint8_t>> NoiseHandshake::decrypt(CipherState& cs,
                                                       ConstBytes ciphertext) {
    (void)cs; (void)ciphertext;
    return Result<std::vector<uint8_t>>::err("noise::decrypt not implemented");
}

}  // namespace peercore::protocol::noise
