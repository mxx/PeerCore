#include "../src/protocol/noise/noise.hpp"

#include <sodium.h>

#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace peercore;
using namespace peercore::protocol::noise;

namespace {

std::optional<std::vector<uint8_t>> decode_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned int byte = 0;
        std::istringstream in(std::string(hex.substr(i * 2, 2)));
        in >> std::hex >> byte;
        if (in.fail()) return std::nullopt;
        out[i] = static_cast<uint8_t>(byte);
    }
    return out;
}

std::string encode_hex(ConstBytes bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return out.str();
}

std::optional<NoiseKeypair> keypair_from_secret_hex(const char* hex) {
    if (hex == nullptr || *hex == '\0') return std::nullopt;
    auto decoded = decode_hex(hex);
    if (!decoded.has_value() || decoded->size() != 32) return std::nullopt;

    NoiseKeypair kp;
    std::copy(decoded->begin(), decoded->end(), kp.secret_key.begin());
    ::crypto_scalarmult_curve25519_base(kp.public_key.data(), kp.secret_key.data());
    return kp;
}

void print_json(std::string_view type,
                std::initializer_list<std::pair<std::string_view, std::string>> fields) {
    std::cout << "{\"type\":\"" << type << "\"";
    for (const auto& [key, value] : fields) {
        std::cout << ",\"" << key << "\":\"" << value << "\"";
    }
    std::cout << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (::sodium_init() < 0) {
        std::cerr << "failed to initialize libsodium\n";
        return 1;
    }

    bool generate_msg1 = false;
    std::optional<std::string> msg1_hex;
    std::optional<std::string> msg2_hex;
    std::optional<std::string> aead_probe_key_hex;
    std::optional<std::string> aead_probe_ad_hex;
    std::optional<std::string> aead_probe_pt_hex;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--generate-msg1") {
            generate_msg1 = true;
            continue;
        }
        if (arg == "--process-msg1" && i + 1 < argc) {
            msg1_hex = argv[++i];
            continue;
        }
        if (arg == "--process-msg2" && i + 1 < argc) {
            msg2_hex = argv[++i];
            continue;
        }
        if (arg == "--aead-probe" && i + 3 < argc) {
            aead_probe_key_hex = argv[++i];
            aead_probe_ad_hex = argv[++i];
            aead_probe_pt_hex = argv[++i];
            continue;
        }
    }

    NoiseSession initiator;
    initiator.configured_ephemeral = keypair_from_secret_hex(std::getenv("INIT_EPHEMERAL_SECRET_HEX"));
    initiator.configured_static = keypair_from_secret_hex(std::getenv("INIT_STATIC_SECRET_HEX"));

    if (generate_msg1) {
        auto msg1 = NoiseHandshake::write_msg1(initiator);
        if (msg1.empty()) {
            std::cerr << "failed to generate msg1\n";
            return 1;
        }
        print_json("msg1", {
            {"msg1_hex", encode_hex(msg1)},
            {"ephemeral_pub_hex", encode_hex(initiator.ephemeral.public_key)},
            {"static_pub_hex", encode_hex(initiator.static_key.public_key)},
        });
        return 0;
    }

    if (aead_probe_key_hex.has_value()) {
        auto key = decode_hex(*aead_probe_key_hex);
        auto ad = decode_hex(*aead_probe_ad_hex);
        auto pt = decode_hex(*aead_probe_pt_hex);
        if (!key.has_value() || key->size() != 32 || !ad.has_value() || !pt.has_value()) {
            std::cerr << "invalid aead probe hex inputs\n";
            return 2;
        }

        std::array<unsigned char, crypto_aead_chacha20poly1305_ietf_NPUBBYTES> nonce{};
        std::vector<uint8_t> ciphertext(
            pt->size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
        unsigned long long out_len = 0;
        if (::crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext.data(),
                                                        &out_len,
                                                        pt->data(),
                                                        pt->size(),
                                                        ad->data(),
                                                        ad->size(),
                                                        nullptr,
                                                        nonce.data(),
                                                        key->data()) != 0) {
            std::cerr << "aead encrypt failed\n";
            return 3;
        }
        ciphertext.resize(static_cast<size_t>(out_len));
        print_json("aead_probe", {
            {"key_hex", *aead_probe_key_hex},
            {"ad_hex", *aead_probe_ad_hex},
            {"pt_hex", *aead_probe_pt_hex},
            {"ct_hex", encode_hex(ciphertext)},
        });
        return 0;
    }

    if (msg1_hex.has_value()) {
        auto decoded = decode_hex(*msg1_hex);
        if (!decoded.has_value()) {
            std::cerr << "invalid msg1 hex\n";
            return 2;
        }

        NoiseSession responder;
        responder.configured_ephemeral = keypair_from_secret_hex(std::getenv("RESP_EPHEMERAL_SECRET_HEX"));
        responder.configured_static = keypair_from_secret_hex(std::getenv("RESP_STATIC_SECRET_HEX"));

        auto msg2 = NoiseHandshake::process_msg1(responder, *decoded);
        if (msg2.is_err()) {
            print_json("process_msg1_error", {
                {"detail", msg2.error().message},
                {"msg1_hex", *msg1_hex},
            });
            return 3;
        }

        print_json("process_msg1_ok", {
            {"msg1_hex", *msg1_hex},
            {"msg2_hex", encode_hex(msg2.value())},
            {"ephemeral_pub_hex", encode_hex(responder.ephemeral.public_key)},
            {"static_pub_hex", encode_hex(responder.static_key.public_key)},
        });
        return 0;
    }

    if (msg2_hex.has_value()) {
        auto msg1 = NoiseHandshake::write_msg1(initiator);
        if (msg1.empty()) {
            std::cerr << "failed to initialize initiator session\n";
            return 1;
        }
        auto decoded = decode_hex(*msg2_hex);
        if (!decoded.has_value()) {
            std::cerr << "invalid msg2 hex\n";
            return 2;
        }
        auto msg3 = NoiseHandshake::process_msg2(initiator, *decoded);
        if (msg3.is_err()) {
            print_json("process_msg2_error", {
                {"detail", msg3.error().message},
                {"msg1_hex", encode_hex(msg1)},
            });
            return 3;
        }
        print_json("process_msg2_ok", {
            {"msg1_hex", encode_hex(msg1)},
            {"msg3_hex", encode_hex(msg3.value())},
            {"remote_static_pub_hex", encode_hex(initiator.remote_static_pub)},
        });
        return 0;
    }

    std::cerr << "usage: interop_noise_trace --generate-msg1 | --process-msg1 <hex> | --process-msg2 <hex> | --aead-probe <key-hex> <ad-hex> <pt-hex>\n";
    return 2;
}
