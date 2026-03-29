#include <peercore/peer_store.hpp>
#include <peercore/protocol_handler.hpp>
#include <peercore/swarm.hpp>

#include <sodium.h>

#include <chrono>
#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace peercore;

namespace {

Identity make_identity() {
    Identity identity;
    ::crypto_sign_keypair(identity.secret_key.data() + 32, identity.secret_key.data());
    identity.peer_id = PeerId::from_bytes(
        std::span<const uint8_t, 32>(identity.secret_key.data() + 32, 32));
    return identity;
}

class EchoHandler final : public ProtocolHandler {
public:
    ProtocolId protocol_id() const override { return "/test/echo/1.0.0"; }

    void on_inbound_stream(StreamHandle stream) override {
        inbound_streams_.push_back(std::move(stream));
    }

    void on_outbound_stream_ready(StreamHandle /*stream*/) override {}

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
    }

private:
    std::vector<StreamHandle> inbound_streams_;
};

std::string escape_json(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

void print_json_event(std::string_view type,
                      std::string_view detail,
                      std::optional<ConnectionId> connection_id = std::nullopt,
                      std::optional<StreamId> stream_id = std::nullopt,
                      std::optional<PeerId> peer_id = std::nullopt) {
    std::ostringstream out;
    out << "{\"type\":\"" << escape_json(type) << "\"";
    if (connection_id.has_value()) out << ",\"connection_id\":" << *connection_id;
    if (stream_id.has_value()) out << ",\"stream_id\":" << *stream_id;
    if (peer_id.has_value()) out << ",\"peer_id\":\"" << peer_id->to_string() << "\"";
    out << ",\"detail\":\"" << escape_json(detail) << "\"}";
    std::cout << out.str() << std::endl;
}

void print_usage() {
    std::cerr
        << "usage: interop_peercore_probe [--listen <multiaddr>]... [--dial <multiaddr>] "
           "[--muxer <protocol>]... [--runtime-ms <ms>] [--open-protocol <proto>]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (::sodium_init() < 0) {
        std::cerr << "failed to initialize libsodium\n";
        return 1;
    }

    std::vector<Multiaddr> listen_addrs;
    std::optional<Multiaddr> dial_addr;
    std::vector<ProtocolId> muxers;
    std::optional<ProtocolId> open_protocol;
    int runtime_ms = 5000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            listen_addrs.emplace_back(argv[++i]);
            continue;
        }
        if (arg == "--dial" && i + 1 < argc) {
            dial_addr = Multiaddr(argv[++i]);
            continue;
        }
        if (arg == "--muxer" && i + 1 < argc) {
            muxers.emplace_back(argv[++i]);
            continue;
        }
        if (arg == "--open-protocol" && i + 1 < argc) {
            open_protocol = ProtocolId(argv[++i]);
            continue;
        }
        if (arg == "--runtime-ms" && i + 1 < argc) {
            runtime_ms = std::atoi(argv[++i]);
            continue;
        }
        print_usage();
        return 2;
    }

    if (listen_addrs.empty()) {
        listen_addrs.emplace_back("/ip4/0.0.0.0/tcp/0");
    }

    PeerStore peer_store;
    auto identity = make_identity();
    Swarm swarm(peer_store, identity, muxers);
    swarm.register_handler(std::make_shared<EchoHandler>());

    auto started = swarm.start();
    if (started.is_err()) {
        print_json_event("startup_error", started.error_message());
        return 1;
    }

    print_json_event("probe_started", identity.peer_id.to_string());

    for (const auto& addr : listen_addrs) {
        auto listen = swarm.listen_on(addr);
        if (listen.is_err()) {
            print_json_event("listen_error", listen.error_message());
            return 1;
        }
    }

    if (dial_addr.has_value()) {
        auto dial = swarm.dial_addr(*dial_addr);
        if (dial.is_err()) {
            print_json_event("dial_error", dial.error_message());
            return 1;
        }
        print_json_event("dial_requested", dial_addr->to_string());
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(runtime_ms);
    bool opened_stream = false;

    while (std::chrono::steady_clock::now() < deadline) {
        swarm.poll_once();
        while (auto ev = swarm.next_event()) {
            print_json_event(
                [&]() -> std::string {
                    switch (ev->type) {
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
                }(),
                ev->detail,
                ev->connection_id ? std::optional<ConnectionId>(ev->connection_id) : std::nullopt,
                ev->stream_id,
                ev->peer_id);

            if (open_protocol.has_value() &&
                !opened_stream &&
                ev->type == SwarmEvent::Type::ConnectionEstablished &&
                ev->peer_id.has_value()) {
                auto stream = swarm.open_stream(*ev->peer_id, *open_protocol);
                if (stream.is_err()) {
                    print_json_event("open_stream_error",
                                     stream.error().message,
                                     ev->connection_id,
                                     std::nullopt,
                                     ev->peer_id);
                } else {
                    opened_stream = true;
                    print_json_event("open_stream_requested",
                                     *open_protocol,
                                     ev->connection_id,
                                     stream.value()->id(),
                                     ev->peer_id);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    (void)swarm.stop();
    print_json_event("probe_finished", "runtime elapsed");
    return 0;
}
