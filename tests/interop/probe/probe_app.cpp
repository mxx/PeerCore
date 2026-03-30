#include "probe_app.hpp"

#include "probe_events.hpp"

#include <peercore/protocol_handler.hpp>
#include <peercore/services/identify_service.hpp>

#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace peercore::interop::probe {

namespace {

constexpr std::string_view kEchoProtocol = "/test/echo/1.0.0";
constexpr std::string_view kEchoPayload = "interop-ping";

class EchoHandler final : public ProtocolHandler {
public:
    ProtocolId protocol_id() const override { return ProtocolId(kEchoProtocol); }

    void on_inbound_stream(StreamHandle stream) override {
        inbound_streams_.push_back(std::move(stream));
    }

    void on_outbound_stream_ready(StreamHandle stream) override {
        OutboundState state;
        state.stream = std::move(stream);
        outbound_streams_.push_back(std::move(state));
    }

    void on_tick() override {
        std::vector<StreamHandle> still_open;
        std::array<uint8_t, 1024> buf{};
        for (auto& stream : inbound_streams_) {
            auto read = stream->try_read(buf);
            if (read.is_ok() && read.value() > 0) {
                (void)stream->try_write(ConstBytes(buf.data(), read.value()));
            }
            if (stream->is_open()) still_open.push_back(stream);
        }
        inbound_streams_ = std::move(still_open);

        std::vector<OutboundState> active_outbound;
        active_outbound.reserve(outbound_streams_.size());
        for (auto& state : outbound_streams_) {
            if (!state.payload_sent) {
                auto wrote = state.stream->try_write(
                    ConstBytes(reinterpret_cast<const uint8_t*>(kEchoPayload.data()),
                               kEchoPayload.size()));
                if (wrote.is_ok() && wrote.value() == kEchoPayload.size()) {
                    state.payload_sent = true;
                    print_json_event(ProbeEvent{
                        .type = "stream_echo_sent",
                        .phase = "stream",
                        .detail = std::string(kEchoPayload),
                        .connection_id = state.stream->connection_id(),
                        .stream_id = state.stream->id(),
                        .protocol = ProtocolId(kEchoProtocol),
                    });
                }
            }

            if (state.payload_sent && !state.echo_received) {
                auto read = state.stream->try_read(buf);
                if (read.is_ok() && read.value() > 0) {
                    state.received.append(reinterpret_cast<const char*>(buf.data()), read.value());
                    if (state.received.size() >= kEchoPayload.size()) {
                        state.echo_received = true;
                        print_json_event(ProbeEvent{
                            .type = "stream_echo_received",
                            .phase = "stream",
                            .detail = state.received,
                            .connection_id = state.stream->connection_id(),
                            .stream_id = state.stream->id(),
                            .protocol = ProtocolId(kEchoProtocol),
                        });
                        (void)state.stream->close_write();
                    }
                }
            }

            if (state.stream->is_open()) active_outbound.push_back(std::move(state));
        }
        outbound_streams_ = std::move(active_outbound);
    }

private:
    struct OutboundState {
        StreamHandle stream;
        bool         payload_sent{false};
        bool         echo_received{false};
        std::string  received;
    };

    std::vector<StreamHandle> inbound_streams_;
    std::vector<OutboundState> outbound_streams_;
};

bool decode_hex(std::string_view text, std::vector<uint8_t>& out) {
    if (text.size() % 2 != 0) return false;
    out.clear();
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        unsigned int byte = 0;
        const auto pair = text.substr(i, 2);
        if (std::from_chars(pair.data(), pair.data() + pair.size(), byte, 16).ec != std::errc()) {
            return false;
        }
        out.push_back(static_cast<uint8_t>(byte));
    }
    return true;
}

Result<int> parse_int(std::string_view text) {
    int value = 0;
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc()) {
        return Result<int>::err("invalid integer: " + std::string(text));
    }
    return Result<int>::ok(value);
}

Result<ProbeMode> parse_mode(std::string_view text) {
    if (text == "auto") return Result<ProbeMode>::ok(ProbeMode::Auto);
    if (text == "listen") return Result<ProbeMode>::ok(ProbeMode::Listen);
    if (text == "dial") return Result<ProbeMode>::ok(ProbeMode::Dial);
    if (text == "echo-server") return Result<ProbeMode>::ok(ProbeMode::EchoServer);
    if (text == "echo-client") return Result<ProbeMode>::ok(ProbeMode::EchoClient);
    return Result<ProbeMode>::err("unsupported probe mode: " + std::string(text));
}

std::string mode_name(ProbeMode mode) {
    switch (mode) {
        case ProbeMode::Auto: return "auto";
        case ProbeMode::Listen: return "listen";
        case ProbeMode::Dial: return "dial";
        case ProbeMode::EchoServer: return "echo-server";
        case ProbeMode::EchoClient: return "echo-client";
    }
    return "unknown";
}

std::string yaml_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '\'') {
            out += "''";
        } else {
            out += ch;
        }
    }
    return out;
}

std::string_view phase_for_event_type(const std::string& type) {
    if (type == "probe_started" || type == "probe_config" || type == "probe_finished" ||
        type == "startup_error") {
        return "lifecycle";
    }
    if (type == "listener_started") return "transport";
    if (type == "protocol_negotiated" || type == "protocol_error") return "negotiation";
    if (type == "stream_opened" || type == "stream_accepted" || type == "stream_closed" ||
        type == "open_stream_requested" || type == "open_stream_error") {
        return "stream";
    }
    return "connection";
}

std::string swarm_event_type_name(SwarmEvent::Type type) {
    switch (type) {
        case SwarmEvent::Type::ListenerStarted: return "listener_started";
        case SwarmEvent::Type::IncomingConnection: return "incoming_connection";
        case SwarmEvent::Type::ConnectionEstablished: return "connection_established";
        case SwarmEvent::Type::ConnectionClosed: return "connection_closed";
        case SwarmEvent::Type::DialFailed: return "dial_failed";
        case SwarmEvent::Type::StreamOpened: return "stream_opened";
        case SwarmEvent::Type::StreamAccepted: return "stream_accepted";
        case SwarmEvent::Type::StreamClosed: return "stream_closed";
        case SwarmEvent::Type::ProtocolNegotiated: return "protocol_negotiated";
        case SwarmEvent::Type::ProtocolError: return "protocol_error";
        case SwarmEvent::Type::PeerIdentified: return "peer_identified";
    }
    return "unknown";
}

Result<void> write_inputs_yaml(const ProbeConfig& config) {
    if (!config.write_inputs_yaml.has_value()) return Result<void>::ok();

    std::ofstream out(*config.write_inputs_yaml);
    if (!out.is_open()) {
        return Result<void>::err("failed to open inputs yaml: " + *config.write_inputs_yaml);
    }

    out << "mode: '" << yaml_escape(mode_name(config.mode)) << "'\n";
    out << "runtime_ms: " << config.runtime_ms << "\n";
    out << "transport: '" << yaml_escape(config.transport_name.value_or("tcp")) << "'\n";
    out << "security: '" << yaml_escape(config.security_protocol.value_or("/noise")) << "'\n";
    out << "listen_addrs:\n";
    for (const auto& addr : config.listen_addrs) {
        out << "  - '" << yaml_escape(addr.to_string()) << "'\n";
    }
    if (config.dial_addr.has_value()) {
        out << "dial_addr: '" << yaml_escape(config.dial_addr->to_string()) << "'\n";
    }
    out << "muxers:\n";
    for (const auto& muxer : config.muxers) {
        out << "  - '" << yaml_escape(muxer) << "'\n";
    }
    if (config.open_protocol.has_value()) {
        out << "open_protocol: '" << yaml_escape(*config.open_protocol) << "'\n";
    }
    if (config.identity_secret_hex.has_value()) {
        out << "identity_secret_hex: '" << yaml_escape(*config.identity_secret_hex) << "'\n";
    }
    return Result<void>::ok();
}

Result<void> validate_config(ProbeConfig& config) {
    if (config.security_protocol.has_value() && *config.security_protocol != "/noise") {
        return Result<void>::err("unsupported security protocol: " + *config.security_protocol);
    }
    if (config.transport_name.has_value() && *config.transport_name != "tcp") {
        return Result<void>::err("unsupported transport: " + *config.transport_name);
    }
    if (config.mode == ProbeMode::Dial || config.mode == ProbeMode::EchoClient) {
        if (!config.dial_addr.has_value()) {
            return Result<void>::err("dial mode requires --dial");
        }
    }
    if (config.mode == ProbeMode::EchoClient && !config.open_protocol.has_value()) {
        config.open_protocol = ProtocolId(kEchoProtocol);
    }
    if (config.listen_addrs.empty()) {
        config.listen_addrs.emplace_back("/ip4/0.0.0.0/tcp/0");
    }
    return Result<void>::ok();
}

}  // namespace

std::string usage_string() {
    return "usage: interop_peercore_probe [--mode <auto|listen|dial|echo-server|echo-client>] "
           "[--listen <multiaddr>]... [--dial <multiaddr>] [--muxer <protocol>]... "
           "[--security <protocol>] [--transport <name>] [--runtime-ms <ms>] "
           "[--open-protocol <proto>] [--identity-secret-hex <hex>] "
           "[--write-inputs-yaml <path>]";
}

Result<ProbeConfig> parse_probe_args(int argc, char** argv) {
    ProbeConfig config;
    if (const char* env_secret = std::getenv("PEERCORE_IDENTITY_SECRET_HEX")) {
        if (*env_secret != '\0') config.identity_secret_hex = std::string(env_secret);
    }
    if (const char* env_inputs = std::getenv("PEERCORE_PROBE_WRITE_INPUTS_YAML")) {
        if (*env_inputs != '\0') config.write_inputs_yaml = std::string(env_inputs);
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            config.listen_addrs.emplace_back(argv[++i]);
            continue;
        }
        if (arg == "--dial" && i + 1 < argc) {
            config.dial_addr = Multiaddr(argv[++i]);
            continue;
        }
        if (arg == "--muxer" && i + 1 < argc) {
            config.muxers.emplace_back(argv[++i]);
            continue;
        }
        if (arg == "--open-protocol" && i + 1 < argc) {
            config.open_protocol = ProtocolId(argv[++i]);
            continue;
        }
        if (arg == "--runtime-ms" && i + 1 < argc) {
            auto parsed = parse_int(argv[++i]);
            if (parsed.is_err()) return Result<ProbeConfig>::err(parsed.error().message);
            config.runtime_ms = parsed.value();
            continue;
        }
        if (arg == "--mode" && i + 1 < argc) {
            auto parsed = parse_mode(argv[++i]);
            if (parsed.is_err()) return Result<ProbeConfig>::err(parsed.error().message);
            config.mode = parsed.value();
            continue;
        }
        if (arg == "--security" && i + 1 < argc) {
            config.security_protocol = ProtocolId(argv[++i]);
            continue;
        }
        if (arg == "--transport" && i + 1 < argc) {
            config.transport_name = std::string(argv[++i]);
            continue;
        }
        if (arg == "--identity-secret-hex" && i + 1 < argc) {
            config.identity_secret_hex = std::string(argv[++i]);
            continue;
        }
        if (arg == "--write-inputs-yaml" && i + 1 < argc) {
            config.write_inputs_yaml = std::string(argv[++i]);
            continue;
        }
        return Result<ProbeConfig>::err("unrecognized argument: " + std::string(arg));
    }

    auto validated = validate_config(config);
    if (validated.is_err()) return Result<ProbeConfig>::err(validated.error_message());
    return Result<ProbeConfig>::ok(std::move(config));
}

Identity make_identity(const std::optional<std::string>& secret_hex) {
    Identity identity;
    if (!secret_hex.has_value()) {
        ::crypto_sign_keypair(identity.secret_key.data() + 32, identity.secret_key.data());
        identity.peer_id = PeerId::from_bytes(
            std::span<const uint8_t, 32>(identity.secret_key.data() + 32, 32));
        return identity;
    }

    std::vector<uint8_t> decoded;
    if (!decode_hex(*secret_hex, decoded)) {
        throw std::runtime_error("invalid identity secret hex");
    }

    if (decoded.size() == 32) {
        ::crypto_sign_seed_keypair(identity.secret_key.data() + 32,
                                   identity.secret_key.data(),
                                   decoded.data());
    } else if (decoded.size() == identity.secret_key.size()) {
        std::copy(decoded.begin(), decoded.end(), identity.secret_key.begin());
    } else {
        throw std::runtime_error("identity secret hex must decode to 32-byte seed or 64-byte secret key");
    }

    identity.peer_id = PeerId::from_bytes(
        std::span<const uint8_t, 32>(identity.secret_key.data() + 32, 32));
    return identity;
}

int run_probe(const ProbeConfig& config) {
    PeerStore peer_store;
    Identity identity;
    try {
        identity = make_identity(config.identity_secret_hex);
    } catch (const std::exception& ex) {
        print_json_event(ProbeEvent{
            .type = "startup_error",
            .phase = "lifecycle",
            .detail = ex.what(),
        });
        return 1;
    }

    Swarm swarm(peer_store, identity, config.muxers);
    swarm.register_handler(std::make_shared<IdentifyService>());
    if (config.enable_echo_handler) {
        swarm.register_handler(std::make_shared<EchoHandler>());
    }

    auto started = swarm.start();
    if (started.is_err()) {
        print_json_event(ProbeEvent{
            .type = "startup_error",
            .phase = "lifecycle",
            .detail = started.error_message(),
        });
        return 1;
    }

    print_json_event(ProbeEvent{
        .type = "probe_started",
        .phase = "lifecycle",
        .detail = identity.peer_id.to_string(),
    });
    print_json_event(ProbeEvent{
        .type = "probe_config",
        .phase = "lifecycle",
        .detail = "mode=" + mode_name(config.mode) +
                  ", transport=" + config.transport_name.value_or("tcp") +
                  ", security=" + config.security_protocol.value_or("/noise"),
        .protocol = config.open_protocol,
    });
    auto inputs_written = write_inputs_yaml(config);
    if (inputs_written.is_err()) {
        print_json_event(ProbeEvent{
            .type = "startup_error",
            .phase = "lifecycle",
            .detail = inputs_written.error_message(),
        });
        return 1;
    }

    for (const auto& addr : config.listen_addrs) {
        auto listen = swarm.listen_on(addr);
        if (listen.is_err()) {
            print_json_event(ProbeEvent{
                .type = "listen_error",
                .phase = "transport",
                .detail = listen.error_message(),
            });
            return 1;
        }
    }

    if (config.dial_addr.has_value()) {
        auto dial = swarm.dial_addr(*config.dial_addr);
        if (dial.is_err()) {
            print_json_event(ProbeEvent{
                .type = "dial_error",
                .phase = "connection",
                .detail = dial.error_message(),
            });
            return 1;
        }
        print_json_event(ProbeEvent{
            .type = "dial_requested",
            .phase = "connection",
            .detail = config.dial_addr->to_string(),
        });
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(config.runtime_ms);
    bool opened_stream = false;

    while (std::chrono::steady_clock::now() < deadline) {
        swarm.poll_once();
        while (auto ev = swarm.next_event()) {
            const auto type_name = swarm_event_type_name(ev->type);
            const auto protocol = ev->type == SwarmEvent::Type::ProtocolNegotiated
                                      ? std::optional<ProtocolId>(ev->detail)
                                      : std::nullopt;
            print_json_event(ProbeEvent{
                .type = type_name,
                .phase = std::string(phase_for_event_type(type_name)),
                .detail = ev->detail,
                .connection_id = ev->connection_id ? std::optional<ConnectionId>(ev->connection_id)
                                                   : std::nullopt,
                .stream_id = ev->stream_id,
                .peer_id = ev->peer_id,
                .protocol = protocol,
            });

            if (config.open_protocol.has_value() &&
                !opened_stream &&
                ev->type == SwarmEvent::Type::ConnectionEstablished &&
                ev->peer_id.has_value()) {
                auto stream = swarm.open_stream(*ev->peer_id, *config.open_protocol);
                if (stream.is_err()) {
                    print_json_event(ProbeEvent{
                        .type = "open_stream_error",
                        .phase = "stream",
                        .detail = stream.error().message,
                        .connection_id = ev->connection_id
                                             ? std::optional<ConnectionId>(ev->connection_id)
                                             : std::nullopt,
                        .peer_id = ev->peer_id,
                        .protocol = config.open_protocol,
                    });
                } else {
                    opened_stream = true;
                    print_json_event(ProbeEvent{
                        .type = "open_stream_requested",
                        .phase = "stream",
                        .detail = *config.open_protocol,
                        .connection_id = ev->connection_id
                                             ? std::optional<ConnectionId>(ev->connection_id)
                                             : std::nullopt,
                        .stream_id = stream.value()->id(),
                        .peer_id = ev->peer_id,
                        .protocol = config.open_protocol,
                    });
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    (void)swarm.stop();
    print_json_event(ProbeEvent{
        .type = "probe_finished",
        .phase = "lifecycle",
        .detail = "runtime elapsed",
    });
    return 0;
}

}  // namespace peercore::interop::probe
