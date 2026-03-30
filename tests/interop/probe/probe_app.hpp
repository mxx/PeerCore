#pragma once

#include <peercore/peer_store.hpp>
#include <peercore/swarm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace peercore::interop::probe {

enum class ProbeMode { Auto, Listen, Dial, EchoServer, EchoClient };

struct ProbeConfig {
    std::vector<Multiaddr>      listen_addrs;
    std::optional<Multiaddr>    dial_addr;
    std::vector<ProtocolId>     muxers;
    std::optional<ProtocolId>   open_protocol;
    std::optional<ProtocolId>   security_protocol;
    std::optional<std::string>  transport_name;
    std::optional<std::string>  identity_secret_hex;
    std::optional<std::string>  write_inputs_yaml;
    int                         runtime_ms{5000};
    ProbeMode                   mode{ProbeMode::Auto};
    bool                        enable_echo_handler{true};
};

std::string          usage_string();
Result<ProbeConfig>  parse_probe_args(int argc, char** argv);
Identity             make_identity(const std::optional<std::string>& secret_hex);
int                  run_probe(const ProbeConfig& config);

}  // namespace peercore::interop::probe
