#include "noise.hpp"

#include <sodium.h>

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace peercore::protocol::noise {

namespace {

constexpr std::string_view kSignaturePrefix = "noise-libp2p-static-key:";
constexpr uint8_t kKeyTypeEd25519 = 1;

std::vector<uint8_t> encode_uvarint(uint64_t value) {
    std::vector<uint8_t> out;
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
    return out;
}

Result<uint64_t> decode_uvarint(ConstBytes& buf) {
    uint64_t value = 0;
    uint32_t shift = 0;
    size_t index = 0;
    while (index < buf.size()) {
        const uint8_t byte = buf[index++];
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            buf = buf.subspan(index);
            return Result<uint64_t>::ok(value);
        }
        shift += 7;
        if (shift >= 64) {
            return Result<uint64_t>::err("invalid protobuf varint");
        }
    }
    return Result<uint64_t>::err("incomplete protobuf varint");
}

void append_field_varint(std::vector<uint8_t>& out, uint32_t field_no, uint64_t value) {
    auto key = encode_uvarint((static_cast<uint64_t>(field_no) << 3) | 0);
    auto encoded = encode_uvarint(value);
    out.insert(out.end(), key.begin(), key.end());
    out.insert(out.end(), encoded.begin(), encoded.end());
}

void append_field_bytes(std::vector<uint8_t>& out, uint32_t field_no, ConstBytes value) {
    auto key = encode_uvarint((static_cast<uint64_t>(field_no) << 3) | 2);
    auto len = encode_uvarint(value.size());
    out.insert(out.end(), key.begin(), key.end());
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), value.begin(), value.end());
}

Result<void> ensure_sodium_ready() {
    if (::sodium_init() < 0) {
        return Result<void>::err("sodium_init failed");
    }
    return Result<void>::ok();
}

std::array<uint8_t, 32> hash_sha256(ConstBytes bytes) {
    std::array<uint8_t, 32> out{};
    crypto_hash_sha256(out.data(), bytes.data(), bytes.size());
    return out;
}

Result<std::array<uint8_t, 32>> compute_shared_secret(const NoiseKeypair& local,
                                                      std::span<const uint8_t, 32> remote_pub) {
    std::array<uint8_t, 32> shared{};
    if (::crypto_scalarmult_curve25519(shared.data(),
                                       local.secret_key.data(),
                                       remote_pub.data()) != 0) {
        return Result<std::array<uint8_t, 32>>::err("curve25519 DH failed");
    }
    return Result<std::array<uint8_t, 32>>::ok(shared);
}

std::array<unsigned char, crypto_aead_chacha20poly1305_ietf_NPUBBYTES>
make_nonce(uint64_t nonce) {
    std::array<unsigned char, crypto_aead_chacha20poly1305_ietf_NPUBBYTES> out{};
    for (size_t index = 0; index < sizeof(nonce); ++index) {
        out[4 + index] = static_cast<unsigned char>((nonce >> (index * 8)) & 0xFF);
    }
    return out;
}

Result<std::array<uint8_t, 32>> hmac_sha256(ConstBytes key, ConstBytes data) {
    crypto_auth_hmacsha256_state state;
    if (::crypto_auth_hmacsha256_init(&state, key.data(), key.size()) != 0) {
        return Result<std::array<uint8_t, 32>>::err("hmac init failed");
    }
    ::crypto_auth_hmacsha256_update(&state, data.data(), data.size());
    std::array<uint8_t, 32> out{};
    ::crypto_auth_hmacsha256_final(&state, out.data());
    return Result<std::array<uint8_t, 32>>::ok(out);
}

Result<std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>>> hkdf2(
    std::span<const uint8_t, 32> chaining_key,
    ConstBytes input_material) {
    auto temp_key = hmac_sha256(chaining_key, input_material);
    if (temp_key.is_err()) {
        return Result<std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>>>::err(
            temp_key.error().message);
    }

    const std::array<uint8_t, 1> marker1{0x01};
    auto out1 = hmac_sha256(temp_key.value(), marker1);
    if (out1.is_err()) {
        return Result<std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>>>::err(
            out1.error().message);
    }

    std::array<uint8_t, 33> marker2{};
    std::copy(out1.value().begin(), out1.value().end(), marker2.begin());
    marker2.back() = 0x02;
    auto out2 = hmac_sha256(temp_key.value(), marker2);
    if (out2.is_err()) {
        return Result<std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>>>::err(
            out2.error().message);
    }

    return Result<std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>>>::ok(
        {out1.value(), out2.value()});
}

void mix_hash(NoiseSession& session, ConstBytes data) {
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, session.handshake_hash.data(), session.handshake_hash.size());
    crypto_hash_sha256_update(&state, data.data(), data.size());
    crypto_hash_sha256_final(&state, session.handshake_hash.data());
}

Result<void> initialize_symmetric(NoiseSession& session) {
    constexpr std::string_view kProtocolName = "Noise_XX_25519_ChaChaPoly_SHA256";

    session.remote_peer_id.reset();
    session.remote_extensions = {};
    session.handshake_complete = false;
    session.has_remote_ephemeral = false;
    session.has_remote_static = false;
    session.handshake_cipher = {};
    session.cs_send = {};
    session.cs_recv = {};

    session.handshake_hash.fill(0);
    if (kProtocolName.size() <= session.handshake_hash.size()) {
        std::copy(kProtocolName.begin(), kProtocolName.end(), session.handshake_hash.begin());
    } else {
        session.handshake_hash = hash_sha256(ConstBytes(
            reinterpret_cast<const uint8_t*>(kProtocolName.data()), kProtocolName.size()));
    }

    session.chaining_key = session.handshake_hash;
    mix_hash(session, ConstBytes{});
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> encrypt_with_key(std::span<const uint8_t, 32> key,
                                              uint64_t nonce,
                                              ConstBytes plaintext,
                                              ConstBytes ad) {
    std::vector<uint8_t> ciphertext(
        plaintext.size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long out_len = 0;
    const auto encoded_nonce = make_nonce(nonce);

    if (::crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext.data(),
                                                    &out_len,
                                                    plaintext.data(),
                                                    plaintext.size(),
                                                    ad.data(),
                                                    ad.size(),
                                                    nullptr,
                                                    encoded_nonce.data(),
                                                    key.data()) != 0) {
        return Result<std::vector<uint8_t>>::err("noise::encrypt failed");
    }

    ciphertext.resize(out_len);
    return Result<std::vector<uint8_t>>::ok(std::move(ciphertext));
}

Result<std::vector<uint8_t>> decrypt_with_key(std::span<const uint8_t, 32> key,
                                              uint64_t nonce,
                                              ConstBytes ciphertext,
                                              ConstBytes ad) {
    if (ciphertext.size() < crypto_aead_chacha20poly1305_ietf_ABYTES) {
        return Result<std::vector<uint8_t>>::err("ciphertext too short");
    }

    std::vector<uint8_t> plaintext(
        ciphertext.size() - crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long out_len = 0;
    const auto encoded_nonce = make_nonce(nonce);

    if (::crypto_aead_chacha20poly1305_ietf_decrypt(plaintext.data(),
                                                    &out_len,
                                                    nullptr,
                                                    ciphertext.data(),
                                                    ciphertext.size(),
                                                    ad.data(),
                                                    ad.size(),
                                                    encoded_nonce.data(),
                                                    key.data()) != 0) {
        return Result<std::vector<uint8_t>>::err("noise::decrypt failed");
    }

    plaintext.resize(out_len);
    return Result<std::vector<uint8_t>>::ok(std::move(plaintext));
}

Result<void> mix_key(NoiseSession& session, ConstBytes input_material) {
    auto derived = hkdf2(session.chaining_key, input_material);
    if (derived.is_err()) return Result<void>::err(derived.error().message);

    session.chaining_key = derived.value().first;
    session.handshake_cipher.key = derived.value().second;
    session.handshake_cipher.nonce = 0;
    session.handshake_cipher.has_key = true;
    return Result<void>::ok();
}

Result<void> mix_key_dh(NoiseSession& session,
                        const NoiseKeypair& local,
                        std::span<const uint8_t, 32> remote_pub) {
    auto shared = compute_shared_secret(local, remote_pub);
    if (shared.is_err()) return Result<void>::err(shared.error().message);
    return mix_key(session, shared.value());
}

Result<std::vector<uint8_t>> encrypt_and_hash(NoiseSession& session, ConstBytes plaintext) {
    if (!session.handshake_cipher.has_key) {
        std::vector<uint8_t> out(plaintext.begin(), plaintext.end());
        mix_hash(session, out);
        return Result<std::vector<uint8_t>>::ok(std::move(out));
    }

    auto ciphertext = encrypt_with_key(session.handshake_cipher.key,
                                       session.handshake_cipher.nonce,
                                       plaintext,
                                       session.handshake_hash);
    if (ciphertext.is_err()) return ciphertext;

    ++session.handshake_cipher.nonce;
    mix_hash(session, ciphertext.value());
    return ciphertext;
}

Result<std::vector<uint8_t>> decrypt_and_hash(NoiseSession& session, ConstBytes ciphertext) {
    if (!session.handshake_cipher.has_key) {
        std::vector<uint8_t> out(ciphertext.begin(), ciphertext.end());
        mix_hash(session, ciphertext);
        return Result<std::vector<uint8_t>>::ok(std::move(out));
    }

    auto plaintext = decrypt_with_key(session.handshake_cipher.key,
                                      session.handshake_cipher.nonce,
                                      ciphertext,
                                      session.handshake_hash);
    if (plaintext.is_err()) return plaintext;

    ++session.handshake_cipher.nonce;
    mix_hash(session, ciphertext);
    return plaintext;
}

Result<void> split_transport(NoiseSession& session) {
    auto derived = hkdf2(session.chaining_key, ConstBytes{});
    if (derived.is_err()) return Result<void>::err(derived.error().message);

    if (session.is_initiator) {
        session.cs_send = CipherState{.key = derived.value().first, .nonce = 0, .has_key = true};
        session.cs_recv =
            CipherState{.key = derived.value().second, .nonce = 0, .has_key = true};
    } else {
        session.cs_send = CipherState{.key = derived.value().second, .nonce = 0, .has_key = true};
        session.cs_recv = CipherState{.key = derived.value().first, .nonce = 0, .has_key = true};
    }
    return Result<void>::ok();
}

std::vector<uint8_t> serialize_public_key_ed25519(std::span<const uint8_t, 32> public_key) {
    std::vector<uint8_t> out;
    append_field_varint(out, 1, kKeyTypeEd25519);
    append_field_bytes(out, 2, public_key);
    return out;
}

Result<std::array<uint8_t, 32>> parse_public_key_ed25519(ConstBytes bytes) {
    std::optional<uint64_t> key_type;
    std::optional<std::array<uint8_t, 32>> public_key;

    while (!bytes.empty()) {
        auto key = decode_uvarint(bytes);
        if (key.is_err()) return Result<std::array<uint8_t, 32>>::err(key.error().message);

        const uint32_t field_no = static_cast<uint32_t>(key.value() >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(key.value() & 0x07);
        if (field_no == 1 && wire_type == 0) {
            auto value = decode_uvarint(bytes);
            if (value.is_err()) return Result<std::array<uint8_t, 32>>::err(value.error().message);
            key_type = value.value();
            continue;
        }
        if (field_no == 2 && wire_type == 2) {
            auto len = decode_uvarint(bytes);
            if (len.is_err()) return Result<std::array<uint8_t, 32>>::err(len.error().message);
            if (bytes.size() < len.value() || len.value() != 32) {
                return Result<std::array<uint8_t, 32>>::err("invalid identity public key");
            }
            std::array<uint8_t, 32> parsed{};
            std::copy_n(bytes.begin(), 32, parsed.begin());
            bytes = bytes.subspan(32);
            public_key = parsed;
            continue;
        }
        return Result<std::array<uint8_t, 32>>::err("unsupported public key encoding");
    }

    if (!key_type.has_value() || key_type.value() != kKeyTypeEd25519 || !public_key.has_value()) {
        return Result<std::array<uint8_t, 32>>::err("invalid identity public key");
    }
    return Result<std::array<uint8_t, 32>>::ok(*public_key);
}

std::vector<uint8_t> signature_message(std::span<const uint8_t, 32> static_pubkey) {
    std::vector<uint8_t> msg(kSignaturePrefix.begin(), kSignaturePrefix.end());
    msg.insert(msg.end(), static_pubkey.begin(), static_pubkey.end());
    return msg;
}

std::vector<uint8_t> serialize_extensions(const NoiseExtensions& extensions) {
    std::vector<uint8_t> out;
    for (const auto& muxer : extensions.stream_muxers) {
        append_field_bytes(out, 2, ConstBytes(reinterpret_cast<const uint8_t*>(muxer.data()),
                                              muxer.size()));
    }
    return out;
}

Result<NoiseExtensions> parse_extensions(ConstBytes bytes) {
    NoiseExtensions extensions;
    while (!bytes.empty()) {
        auto key = decode_uvarint(bytes);
        if (key.is_err()) return Result<NoiseExtensions>::err(key.error().message);
        const uint32_t field_no = static_cast<uint32_t>(key.value() >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(key.value() & 0x07);
        if (wire_type != 2) {
            return Result<NoiseExtensions>::err("unsupported noise extension");
        }

        auto len = decode_uvarint(bytes);
        if (len.is_err()) return Result<NoiseExtensions>::err(len.error().message);
        if (bytes.size() < len.value()) {
            return Result<NoiseExtensions>::err("invalid noise extension");
        }
        if (field_no == 2) {
            extensions.stream_muxers.emplace_back(
                reinterpret_cast<const char*>(bytes.data()),
                reinterpret_cast<const char*>(bytes.data() + len.value()));
        }
        bytes = bytes.subspan(len.value());
    }
    return Result<NoiseExtensions>::ok(std::move(extensions));
}

Result<std::vector<uint8_t>> make_local_payload(const NoiseSession& session) {
    if (!session.local_identity.has_value()) {
        return Result<std::vector<uint8_t>>::ok({});
    }
    return NoiseHandshake::make_handshake_payload(*session.local_identity,
                                                  session.static_key,
                                                  session.local_extensions);
}

Result<void> verify_remote_identity(NoiseSession& session, ConstBytes payload_bytes) {
    if (payload_bytes.empty()) {
        session.remote_peer_id.reset();
        session.remote_extensions = {};
        return Result<void>::ok();
    }

    auto payload = NoiseHandshake::parse_handshake_payload(payload_bytes);
    if (payload.is_err()) return Result<void>::err(payload.error().message);

    auto verified = NoiseHandshake::verify_handshake_payload(payload.value(),
                                                             session.remote_static_pub);
    if (verified.is_err()) return verified;

    auto identity_pub = parse_public_key_ed25519(payload.value().identity_key);
    if (identity_pub.is_err()) return Result<void>::err(identity_pub.error().message);

    session.remote_peer_id = PeerId::from_bytes(identity_pub.value());
    session.remote_extensions = payload.value().extensions;
    return Result<void>::ok();
}

NoiseKeypair take_or_generate_keypair(std::optional<NoiseKeypair>& configured) {
    if (configured.has_value()) {
        auto keypair = *configured;
        configured.reset();
        return keypair;
    }
    return NoiseHandshake::generate_keypair();
}
}  // namespace

NoiseKeypair NoiseHandshake::generate_keypair() {
    NoiseKeypair kp;
    auto sodium_ready = ensure_sodium_ready();
    (void)sodium_ready;
    ::randombytes_buf(kp.secret_key.data(), kp.secret_key.size());
    ::crypto_scalarmult_curve25519_base(kp.public_key.data(), kp.secret_key.data());
    return kp;
}

std::vector<uint8_t> NoiseHandshake::write_msg1(NoiseSession& session) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) return {};

    session.is_initiator = true;
    auto symmetric = initialize_symmetric(session);
    if (symmetric.is_err()) return {};
    session.ephemeral = take_or_generate_keypair(session.configured_ephemeral);
    session.static_key = take_or_generate_keypair(session.configured_static);

    std::vector<uint8_t> out(session.ephemeral.public_key.begin(),
                             session.ephemeral.public_key.end());
    mix_hash(session, out);
    auto payload = encrypt_and_hash(session, ConstBytes{});
    if (payload.is_err()) return {};
    out.insert(out.end(), payload.value().begin(), payload.value().end());
    return out;
}

Result<std::vector<uint8_t>> NoiseHandshake::process_msg1(NoiseSession& session,
                                                          ConstBytes msg1) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }
    if (msg1.size() < 32) {
        return Result<std::vector<uint8_t>>::err("invalid msg1 size");
    }
    session.is_initiator = false;
    auto symmetric = initialize_symmetric(session);
    if (symmetric.is_err()) {
        return Result<std::vector<uint8_t>>::err(symmetric.error_message());
    }
    std::copy_n(msg1.begin(), 32, session.remote_ephemeral_pub.begin());
    session.has_remote_ephemeral = true;
    mix_hash(session, msg1.subspan(0, 32));

    ConstBytes remote_payload_bytes(msg1.data() + 32, msg1.size() - 32);
    auto remote_payload = decrypt_and_hash(session, remote_payload_bytes);
    if (remote_payload.is_err()) {
        return Result<std::vector<uint8_t>>::err("noise msg1 payload decrypt failed: " +
                                                 remote_payload.error().message);
    }

    session.ephemeral = take_or_generate_keypair(session.configured_ephemeral);
    session.static_key = take_or_generate_keypair(session.configured_static);

    std::vector<uint8_t> out(session.ephemeral.public_key.begin(),
                             session.ephemeral.public_key.end());
    mix_hash(session, out);

    auto ee = mix_key_dh(session, session.ephemeral, session.remote_ephemeral_pub);
    if (ee.is_err()) return Result<std::vector<uint8_t>>::err(ee.error_message());

    auto encrypted_static = encrypt_and_hash(session, session.static_key.public_key);
    if (encrypted_static.is_err()) {
        return Result<std::vector<uint8_t>>::err(encrypted_static.error().message);
    }
    out.insert(out.end(), encrypted_static.value().begin(), encrypted_static.value().end());

    auto es = mix_key_dh(session, session.static_key, session.remote_ephemeral_pub);
    if (es.is_err()) return Result<std::vector<uint8_t>>::err(es.error_message());

    auto payload = make_local_payload(session);
    if (payload.is_err()) {
        return Result<std::vector<uint8_t>>::err(payload.error().message);
    }
    auto encrypted_payload = encrypt_and_hash(session, payload.value());
    if (encrypted_payload.is_err()) {
        return Result<std::vector<uint8_t>>::err(encrypted_payload.error().message);
    }
    out.insert(out.end(), encrypted_payload.value().begin(), encrypted_payload.value().end());

    return Result<std::vector<uint8_t>>::ok(std::move(out));
}

Result<std::vector<uint8_t>> NoiseHandshake::process_msg2(NoiseSession& session,
                                                          ConstBytes msg2) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }
    constexpr size_t kEncryptedStaticLen = 32 + crypto_aead_chacha20poly1305_ietf_ABYTES;

    if (msg2.size() < 32 + kEncryptedStaticLen) {
        return Result<std::vector<uint8_t>>::err("invalid msg2 size");
    }
    std::copy_n(msg2.begin(), 32, session.remote_ephemeral_pub.begin());
    session.has_remote_ephemeral = true;
    mix_hash(session, msg2.subspan(0, 32));

    auto ee = mix_key_dh(session, session.ephemeral, session.remote_ephemeral_pub);
    if (ee.is_err()) return Result<std::vector<uint8_t>>::err(ee.error_message());

    ConstBytes encrypted_static(msg2.data() + 32, kEncryptedStaticLen);
    auto remote_static = decrypt_and_hash(session, encrypted_static);
    if (remote_static.is_err()) {
        return Result<std::vector<uint8_t>>::err("noise msg2 static decrypt failed: " +
                                                 remote_static.error().message);
    }
    if (remote_static.value().size() != 32) {
        return Result<std::vector<uint8_t>>::err("invalid noise static key");
    }
    std::copy_n(remote_static.value().begin(), 32, session.remote_static_pub.begin());
    session.has_remote_static = true;

    auto es = mix_key_dh(session, session.ephemeral, session.remote_static_pub);
    if (es.is_err()) return Result<std::vector<uint8_t>>::err(es.error_message());

    ConstBytes encrypted_payload(msg2.data() + 32 + kEncryptedStaticLen,
                                 msg2.size() - 32 - kEncryptedStaticLen);
    auto remote_payload = decrypt_and_hash(session, encrypted_payload);
    if (remote_payload.is_err()) {
        return Result<std::vector<uint8_t>>::err("noise msg2 payload decrypt failed: " +
                                                 remote_payload.error().message);
    }
    auto verified = verify_remote_identity(session, remote_payload.value());
    if (verified.is_err()) {
        return Result<std::vector<uint8_t>>::err(verified.error_message());
    }

    std::vector<uint8_t> response;

    auto encrypted_local_static = encrypt_and_hash(session, session.static_key.public_key);
    if (encrypted_local_static.is_err()) {
        return Result<std::vector<uint8_t>>::err(encrypted_local_static.error().message);
    }
    response.insert(response.end(),
                    encrypted_local_static.value().begin(),
                    encrypted_local_static.value().end());

    auto se = mix_key_dh(session, session.static_key, session.remote_ephemeral_pub);
    if (se.is_err()) return Result<std::vector<uint8_t>>::err(se.error_message());

    auto local_payload = make_local_payload(session);
    if (local_payload.is_err()) {
        return Result<std::vector<uint8_t>>::err(local_payload.error().message);
    }
    auto encrypted_local_payload = encrypt_and_hash(session, local_payload.value());
    if (encrypted_local_payload.is_err()) {
        return Result<std::vector<uint8_t>>::err(encrypted_local_payload.error().message);
    }
    response.insert(response.end(),
                    encrypted_local_payload.value().begin(),
                    encrypted_local_payload.value().end());

    auto transport = split_transport(session);
    if (transport.is_err()) {
        return Result<std::vector<uint8_t>>::err(transport.error_message());
    }
    session.handshake_complete = true;
    return Result<std::vector<uint8_t>>::ok(std::move(response));
}

Result<void> NoiseHandshake::process_msg3(NoiseSession& session, ConstBytes msg3) {
    constexpr size_t kEncryptedStaticLen = 32 + crypto_aead_chacha20poly1305_ietf_ABYTES;

    if (msg3.size() < kEncryptedStaticLen) {
        return Result<void>::err("invalid msg3 size");
    }

    ConstBytes encrypted_static(msg3.data(), kEncryptedStaticLen);
    auto remote_static = decrypt_and_hash(session, encrypted_static);
    if (remote_static.is_err()) {
        return Result<void>::err("noise msg3 static decrypt failed: " +
                                 remote_static.error().message);
    }
    if (remote_static.value().size() != 32) {
        return Result<void>::err("invalid noise static key");
    }
    std::copy_n(remote_static.value().begin(), 32, session.remote_static_pub.begin());
    session.has_remote_static = true;

    auto se = mix_key_dh(session, session.ephemeral, session.remote_static_pub);
    if (se.is_err()) return Result<void>::err(se.error_message());

    ConstBytes encrypted_payload(msg3.data() + kEncryptedStaticLen,
                                 msg3.size() - kEncryptedStaticLen);
    auto remote_payload = decrypt_and_hash(session, encrypted_payload);
    if (remote_payload.is_err()) {
        return Result<void>::err("noise msg3 payload decrypt failed: " +
                                 remote_payload.error().message);
    }

    auto verified = verify_remote_identity(session, remote_payload.value());
    if (verified.is_err()) return verified;

    auto transport = split_transport(session);
    if (transport.is_err()) return transport;
    session.handshake_complete = true;
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> NoiseHandshake::encrypt(CipherState& cs,
                                                     ConstBytes plaintext) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }
    if (!cs.has_key) {
        return Result<std::vector<uint8_t>>::err("noise cipher state is not initialized");
    }
    if (cs.nonce == std::numeric_limits<uint64_t>::max()) {
        return Result<std::vector<uint8_t>>::err("noise nonce exhausted");
    }

    auto ciphertext = encrypt_with_key(cs.key, cs.nonce, plaintext, ConstBytes{});
    if (ciphertext.is_err()) return ciphertext;

    ++cs.nonce;
    return ciphertext;
}

Result<std::vector<uint8_t>> NoiseHandshake::decrypt(CipherState& cs,
                                                     ConstBytes ciphertext) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }
    if (!cs.has_key) {
        return Result<std::vector<uint8_t>>::err("noise cipher state is not initialized");
    }
    if (cs.nonce == std::numeric_limits<uint64_t>::max()) {
        return Result<std::vector<uint8_t>>::err("noise nonce exhausted");
    }

    auto plaintext = decrypt_with_key(cs.key, cs.nonce, ciphertext, ConstBytes{});
    if (plaintext.is_err()) return plaintext;

    ++cs.nonce;
    return plaintext;
}

Result<std::vector<uint8_t>> NoiseHandshake::make_handshake_payload(
    const Identity& identity,
    const NoiseKeypair& static_key,
    const NoiseExtensions& extensions) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }

    const auto public_key = serialize_public_key_ed25519(
        std::span<const uint8_t, 32>(identity.secret_key.data() + 32, 32));
    const auto message = signature_message(static_key.public_key);

    std::vector<uint8_t> signature(crypto_sign_BYTES);
    unsigned long long sig_len = 0;
    if (::crypto_sign_detached(signature.data(),
                               &sig_len,
                               message.data(),
                               message.size(),
                               identity.secret_key.data()) != 0) {
        return Result<std::vector<uint8_t>>::err("failed to sign noise static key");
    }
    signature.resize(sig_len);

    std::vector<uint8_t> out;
    append_field_bytes(out, 1, public_key);
    append_field_bytes(out, 2, signature);

    const auto encoded_extensions = serialize_extensions(extensions);
    if (!encoded_extensions.empty()) {
        append_field_bytes(out, 4, encoded_extensions);
    }
    return Result<std::vector<uint8_t>>::ok(std::move(out));
}

Result<NoiseHandshakePayload> NoiseHandshake::parse_handshake_payload(ConstBytes payload) {
    NoiseHandshakePayload out;

    while (!payload.empty()) {
        auto key = decode_uvarint(payload);
        if (key.is_err()) return Result<NoiseHandshakePayload>::err(key.error().message);

        const uint32_t field_no = static_cast<uint32_t>(key.value() >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(key.value() & 0x07);
        if (wire_type != 2) {
            return Result<NoiseHandshakePayload>::err("invalid noise handshake payload");
        }

        auto len = decode_uvarint(payload);
        if (len.is_err()) return Result<NoiseHandshakePayload>::err(len.error().message);
        if (payload.size() < len.value()) {
            return Result<NoiseHandshakePayload>::err("invalid noise handshake payload");
        }

        ConstBytes field = payload.subspan(0, len.value());
        payload = payload.subspan(len.value());

        switch (field_no) {
            case 1:
                out.identity_key.assign(field.begin(), field.end());
                break;
            case 2:
                out.identity_sig.assign(field.begin(), field.end());
                break;
            case 4: {
                auto extensions = parse_extensions(field);
                if (extensions.is_err()) {
                    return Result<NoiseHandshakePayload>::err(extensions.error().message);
                }
                out.extensions = std::move(extensions.value());
                break;
            }
            default:
                break;
        }
    }

    if (out.identity_key.empty() || out.identity_sig.empty()) {
        return Result<NoiseHandshakePayload>::err("missing identity data in handshake payload");
    }
    return Result<NoiseHandshakePayload>::ok(std::move(out));
}

Result<void> NoiseHandshake::verify_handshake_payload(const NoiseHandshakePayload& payload,
                                                      std::span<const uint8_t, 32> static_pubkey) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<void>::err(sodium_ready.error_message());
    }

    auto identity_pub = parse_public_key_ed25519(payload.identity_key);
    if (identity_pub.is_err()) return Result<void>::err(identity_pub.error().message);

    const auto message = signature_message(static_pubkey);
    if (::crypto_sign_verify_detached(payload.identity_sig.data(),
                                      message.data(),
                                      message.size(),
                                      identity_pub.value().data()) != 0) {
        return Result<void>::err("invalid noise static key signature");
    }
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> NoiseHandshake::encode_frame(ConstBytes message) {
    if (message.size() > std::numeric_limits<uint16_t>::max()) {
        return Result<std::vector<uint8_t>>::err("noise frame too large");
    }

    std::vector<uint8_t> out;
    out.reserve(message.size() + 2);
    const uint16_t len = static_cast<uint16_t>(message.size());
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.insert(out.end(), message.begin(), message.end());
    return Result<std::vector<uint8_t>>::ok(std::move(out));
}

Result<std::vector<uint8_t>> NoiseHandshake::decode_frame(ConstBytes frame) {
    if (frame.size() < 2) {
        return Result<std::vector<uint8_t>>::err("incomplete noise frame");
    }

    const uint16_t len = static_cast<uint16_t>((static_cast<uint16_t>(frame[0]) << 8) | frame[1]);
    frame = frame.subspan(2);
    if (frame.size() != len) {
        return Result<std::vector<uint8_t>>::err("invalid noise frame length");
    }
    return Result<std::vector<uint8_t>>::ok(std::vector<uint8_t>(frame.begin(), frame.end()));
}

}  // namespace peercore::protocol::noise
