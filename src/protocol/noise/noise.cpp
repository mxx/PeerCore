#include "noise.hpp"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

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

Result<std::array<uint8_t, 32>> derive_key(std::span<const uint8_t, 32> shared_secret,
                                           std::string_view label) {
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, shared_secret.data(), shared_secret.size());
    crypto_hash_sha256_update(&state,
                              reinterpret_cast<const unsigned char*>(label.data()),
                              label.size());

    std::array<uint8_t, 32> key{};
    crypto_hash_sha256_final(&state, key.data());
    return Result<std::array<uint8_t, 32>>::ok(key);
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

Result<void> derive_transport_keys(NoiseSession& session) {
    if (!session.has_remote_ephemeral) {
        return Result<void>::err("missing remote ephemeral key");
    }

    auto shared = compute_shared_secret(session.ephemeral, session.remote_ephemeral_pub);
    if (shared.is_err()) return Result<void>::err(shared.error().message);

    const std::string_view send_label = session.is_initiator ? "init->resp" : "resp->init";
    const std::string_view recv_label = session.is_initiator ? "resp->init" : "init->resp";

    auto send_key = derive_key(shared.value(), send_label);
    if (send_key.is_err()) return Result<void>::err(send_key.error().message);
    auto recv_key = derive_key(shared.value(), recv_label);
    if (recv_key.is_err()) return Result<void>::err(recv_key.error().message);

    session.cs_send.key = send_key.value();
    session.cs_send.nonce = 0;
    session.cs_recv.key = recv_key.value();
    session.cs_recv.nonce = 0;
    return Result<void>::ok();
}

std::array<unsigned char, crypto_aead_chacha20poly1305_ietf_NPUBBYTES>
make_nonce(uint64_t nonce) {
    std::array<unsigned char, crypto_aead_chacha20poly1305_ietf_NPUBBYTES> out{};
    std::memcpy(out.data(), &nonce, sizeof(nonce));
    return out;
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
        if (field_no != 2 || wire_type != 2) {
            return Result<NoiseExtensions>::err("unsupported noise extension");
        }

        auto len = decode_uvarint(bytes);
        if (len.is_err()) return Result<NoiseExtensions>::err(len.error().message);
        if (bytes.size() < len.value()) {
            return Result<NoiseExtensions>::err("invalid noise extension");
        }
        extensions.stream_muxers.emplace_back(reinterpret_cast<const char*>(bytes.data()),
                                              reinterpret_cast<const char*>(bytes.data() + len.value()));
        bytes = bytes.subspan(len.value());
    }
    return Result<NoiseExtensions>::ok(std::move(extensions));
}

std::vector<uint8_t> serialize_handshake_message(const NoiseSession& session,
                                                 ConstBytes payload = {}) {
    std::vector<uint8_t> out;
    out.reserve(66 + payload.size());
    out.insert(out.end(),
               session.ephemeral.public_key.begin(),
               session.ephemeral.public_key.end());
    out.insert(out.end(),
               session.static_key.public_key.begin(),
               session.static_key.public_key.end());
    const uint16_t payload_len = static_cast<uint16_t>(payload.size());
    out.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Result<void> parse_handshake_message(NoiseSession& session,
                                     ConstBytes msg,
                                     std::vector<uint8_t>& payload) {
    if (msg.size() < 66) {
        return Result<void>::err("invalid noise handshake message");
    }

    std::copy_n(msg.begin(), 32, session.remote_ephemeral_pub.begin());
    std::copy_n(msg.begin() + 32, 32, session.remote_static_pub.begin());
    session.has_remote_ephemeral = true;

    const uint16_t payload_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(msg[64]) << 8) | msg[65]);
    if (msg.size() != static_cast<size_t>(payload_len) + 66) {
        return Result<void>::err("invalid noise handshake message");
    }

    payload.assign(msg.begin() + 66, msg.end());
    return Result<void>::ok();
}

Result<void> verify_remote_identity(NoiseSession& session, ConstBytes payload_bytes) {
    if (payload_bytes.empty()) {
        session.remote_peer_id.reset();
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
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> make_local_payload(const NoiseSession& session) {
    if (!session.local_identity.has_value()) {
        return Result<std::vector<uint8_t>>::ok({});
    }
    return NoiseHandshake::make_handshake_payload(*session.local_identity, session.static_key);
}

}  // namespace

NoiseKeypair NoiseHandshake::generate_keypair() {
    NoiseKeypair kp;
    auto sodium_ready = ensure_sodium_ready();
    (void)sodium_ready;
    ::crypto_box_keypair(kp.public_key.data(), kp.secret_key.data());
    return kp;
}

std::vector<uint8_t> NoiseHandshake::write_msg1(NoiseSession& session) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) return {};

    session.is_initiator = true;
    session.ephemeral = generate_keypair();
    session.static_key = generate_keypair();
    auto payload = make_local_payload(session);
    if (payload.is_err()) return {};
    return serialize_handshake_message(session, payload.value());
}

Result<std::vector<uint8_t>> NoiseHandshake::process_msg1(NoiseSession& session,
                                                          ConstBytes msg1) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }
    session.is_initiator = false;
    std::vector<uint8_t> remote_payload;
    auto parsed = parse_handshake_message(session, msg1, remote_payload);
    if (parsed.is_err()) {
        return Result<std::vector<uint8_t>>::err(parsed.error_message());
    }

    auto verified = verify_remote_identity(session, remote_payload);
    if (verified.is_err()) {
        return Result<std::vector<uint8_t>>::err(verified.error_message());
    }

    session.ephemeral = generate_keypair();
    session.static_key = generate_keypair();

    auto keys = derive_transport_keys(session);
    if (keys.is_err()) {
        return Result<std::vector<uint8_t>>::err(keys.error_message());
    }

    auto payload = make_local_payload(session);
    if (payload.is_err()) {
        return Result<std::vector<uint8_t>>::err(payload.error().message);
    }
    return Result<std::vector<uint8_t>>::ok(serialize_handshake_message(session, payload.value()));
}

Result<std::vector<uint8_t>> NoiseHandshake::process_msg2(NoiseSession& session,
                                                          ConstBytes msg2) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }
    std::vector<uint8_t> remote_payload;
    auto parsed = parse_handshake_message(session, msg2, remote_payload);
    if (parsed.is_err()) {
        return Result<std::vector<uint8_t>>::err(parsed.error_message());
    }

    auto verified = verify_remote_identity(session, remote_payload);
    if (verified.is_err()) {
        return Result<std::vector<uint8_t>>::err(verified.error_message());
    }

    auto keys = derive_transport_keys(session);
    if (keys.is_err()) {
        return Result<std::vector<uint8_t>>::err(keys.error_message());
    }

    session.handshake_complete = true;
    return Result<std::vector<uint8_t>>::ok(std::vector<uint8_t>{0x01});
}

Result<void> NoiseHandshake::process_msg3(NoiseSession& session, ConstBytes msg3) {
    if (msg3.size() != 1 || msg3[0] != 0x01) {
        return Result<void>::err("invalid msg3");
    }

    session.handshake_complete = true;
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> NoiseHandshake::encrypt(CipherState& cs,
                                                     ConstBytes plaintext) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }

    std::vector<uint8_t> ciphertext(
        plaintext.size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long out_len = 0;
    const auto nonce = make_nonce(cs.nonce);

    if (::crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext.data(),
                                                    &out_len,
                                                    plaintext.data(),
                                                    plaintext.size(),
                                                    nullptr,
                                                    0,
                                                    nullptr,
                                                    nonce.data(),
                                                    cs.key.data()) != 0) {
        return Result<std::vector<uint8_t>>::err("noise::encrypt failed");
    }

    ciphertext.resize(out_len);
    ++cs.nonce;
    return Result<std::vector<uint8_t>>::ok(std::move(ciphertext));
}

Result<std::vector<uint8_t>> NoiseHandshake::decrypt(CipherState& cs,
                                                     ConstBytes ciphertext) {
    auto sodium_ready = ensure_sodium_ready();
    if (sodium_ready.is_err()) {
        return Result<std::vector<uint8_t>>::err(sodium_ready.error_message());
    }
    if (ciphertext.size() < crypto_aead_chacha20poly1305_ietf_ABYTES) {
        return Result<std::vector<uint8_t>>::err("ciphertext too short");
    }

    std::vector<uint8_t> plaintext(
        ciphertext.size() - crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long out_len = 0;
    const auto nonce = make_nonce(cs.nonce);

    if (::crypto_aead_chacha20poly1305_ietf_decrypt(plaintext.data(),
                                                    &out_len,
                                                    nullptr,
                                                    ciphertext.data(),
                                                    ciphertext.size(),
                                                    nullptr,
                                                    0,
                                                    nonce.data(),
                                                    cs.key.data()) != 0) {
        return Result<std::vector<uint8_t>>::err("noise::decrypt failed");
    }

    plaintext.resize(out_len);
    ++cs.nonce;
    return Result<std::vector<uint8_t>>::ok(std::move(plaintext));
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
                return Result<NoiseHandshakePayload>::err("unsupported noise handshake payload field");
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
