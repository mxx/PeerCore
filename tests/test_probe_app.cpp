#include <gtest/gtest.h>

#include "interop/probe/probe_app.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace peercore;
using namespace peercore::interop::probe;

namespace {

class EnvVarGuard {
public:
    explicit EnvVarGuard(std::string name)
        : name_(std::move(name)) {
        const char* current = std::getenv(name_.c_str());
        if (current != nullptr) {
            had_original_ = true;
            original_ = current;
        }
    }

    ~EnvVarGuard() {
        if (had_original_) {
            ::setenv(name_.c_str(), original_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    bool        had_original_{false};
    std::string original_;
};

std::vector<char*> argv_for(std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) argv.push_back(arg.data());
    return argv;
}

std::string repeat_hex_byte(std::string_view byte, size_t count) {
    std::string out;
    out.reserve(byte.size() * count);
    for (size_t i = 0; i < count; ++i) out += byte;
    return out;
}

}  // namespace

TEST(ProbeApp, ParsesEchoClientDefaults) {
    std::vector<std::string> args{
        "interop_peercore_probe",
        "--mode",
        "echo-client",
        "--dial",
        "/ip4/127.0.0.1/tcp/4001",
        "--muxer",
        "/yamux/1.0.0",
        "--runtime-ms",
        "2000",
        "--security",
        "/noise",
        "--transport",
        "tcp",
    };
    auto argv = argv_for(args);

    auto parsed = parse_probe_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(parsed.is_ok()) << parsed.error().message;
    EXPECT_EQ(parsed.value().mode, ProbeMode::EchoClient);
    ASSERT_TRUE(parsed.value().dial_addr.has_value());
    EXPECT_EQ(parsed.value().dial_addr->to_string(), "/ip4/127.0.0.1/tcp/4001");
    EXPECT_EQ(parsed.value().runtime_ms, 2000);
    ASSERT_TRUE(parsed.value().open_protocol.has_value());
    EXPECT_EQ(*parsed.value().open_protocol, "/test/echo/1.0.0");
    ASSERT_TRUE(parsed.value().security_protocol.has_value());
    EXPECT_EQ(*parsed.value().security_protocol, "/noise");
    ASSERT_TRUE(parsed.value().transport_name.has_value());
    EXPECT_EQ(*parsed.value().transport_name, "tcp");
}

TEST(ProbeApp, RejectsUnsupportedTransport) {
    std::vector<std::string> args{
        "interop_peercore_probe",
        "--transport",
        "udp",
    };
    auto argv = argv_for(args);

    auto parsed = parse_probe_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(parsed.is_err());
    EXPECT_EQ(parsed.error().message, "unsupported transport: udp");
}

TEST(ProbeApp, UsesIdentitySecretFromEnvironment) {
    EnvVarGuard guard("PEERCORE_IDENTITY_SECRET_HEX");
    const auto seed_hex = repeat_hex_byte("11", 32);
    ::setenv("PEERCORE_IDENTITY_SECRET_HEX", seed_hex.c_str(), 1);

    std::vector<std::string> args{"interop_peercore_probe"};
    auto argv = argv_for(args);

    auto parsed = parse_probe_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(parsed.is_ok()) << parsed.error().message;
    ASSERT_TRUE(parsed.value().identity_secret_hex.has_value());
    EXPECT_EQ(*parsed.value().identity_secret_hex, seed_hex);
}

TEST(ProbeApp, ParsesInputsYamlPath) {
    std::vector<std::string> args{
        "interop_peercore_probe",
        "--write-inputs-yaml",
        "/tmp/peercore-inputs.yaml",
    };
    auto argv = argv_for(args);

    auto parsed = parse_probe_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(parsed.is_ok()) << parsed.error().message;
    ASSERT_TRUE(parsed.value().write_inputs_yaml.has_value());
    EXPECT_EQ(*parsed.value().write_inputs_yaml, "/tmp/peercore-inputs.yaml");
}

TEST(ProbeApp, IdentityFromSeedAndSecretMatch) {
    const auto seed_hex = repeat_hex_byte("22", 32);
    const auto from_seed = make_identity(seed_hex);

    ASSERT_EQ(from_seed.secret_key.size(), 64u);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string secret_key_hex;
    secret_key_hex.reserve(from_seed.secret_key.size() * 2);
    for (const auto byte : from_seed.secret_key) {
        secret_key_hex.push_back(kHex[(byte >> 4) & 0x0F]);
        secret_key_hex.push_back(kHex[byte & 0x0F]);
    }

    const auto from_secret = make_identity(secret_key_hex);
    EXPECT_EQ(from_seed.peer_id, from_secret.peer_id);
    EXPECT_EQ(from_seed.secret_key, from_secret.secret_key);
}
