#pragma once

#include <peercore/types.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace peercore::interop::probe {

inline std::string escape_json(std::string_view value) {
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

struct ProbeEvent {
    std::string               type;
    std::string               phase;
    std::string               detail;
    std::optional<ConnectionId> connection_id;
    std::optional<StreamId>   stream_id;
    std::optional<PeerId>     peer_id;
    std::optional<ProtocolId> protocol;
};

inline uint64_t event_timestamp_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

inline void print_json_event(const ProbeEvent& event) {
    std::ostringstream out;
    out << "{\"type\":\"" << escape_json(event.type) << "\"";
    out << ",\"phase\":\"" << escape_json(event.phase) << "\"";
    out << ",\"timestamp_ms\":" << event_timestamp_ms();
    if (event.connection_id.has_value()) out << ",\"connection_id\":" << *event.connection_id;
    if (event.stream_id.has_value()) out << ",\"stream_id\":" << *event.stream_id;
    if (event.peer_id.has_value()) out << ",\"peer_id\":\"" << event.peer_id->to_string() << "\"";
    if (event.protocol.has_value()) out << ",\"protocol\":\"" << escape_json(*event.protocol) << "\"";
    out << ",\"detail\":\"" << escape_json(event.detail) << "\"}";
    std::cout << out.str() << std::endl;
}

}  // namespace peercore::interop::probe
