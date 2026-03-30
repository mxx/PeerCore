#include <gtest/gtest.h>

#include "../src/protocol/yamux/yamux.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace peercore;
using namespace peercore::protocol::yamux;

namespace {

struct Fixture {
    std::string            name;
    bool                   is_client{false};
    std::vector<uint8_t>   input;
    std::vector<uint8_t>   expected_output;
    std::optional<StreamId> expected_accept_stream_id;
    std::vector<uint8_t>   expected_read;
};

std::string trim(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::vector<uint8_t> decode_hex(std::string_view hex) {
    auto cleaned = trim(hex);
    cleaned.erase(std::remove_if(cleaned.begin(),
                                 cleaned.end(),
                                 [](unsigned char ch) { return std::isspace(ch) != 0; }),
                  cleaned.end());
    EXPECT_EQ(cleaned.size() % 2u, 0u);

    std::vector<uint8_t> out;
    out.reserve(cleaned.size() / 2);
    for (size_t i = 0; i + 1 < cleaned.size(); i += 2) {
        unsigned int byte = 0;
        std::stringstream in;
        in << std::hex << cleaned.substr(i, 2);
        in >> byte;
        EXPECT_FALSE(in.fail());
        out.push_back(static_cast<uint8_t>(byte));
    }
    return out;
}

std::string encode_hex(ConstBytes bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kHex[(byte >> 4) & 0x0F]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

Fixture load_fixture(const std::filesystem::path& path) {
    Fixture fixture;
    std::ifstream in(path);
    EXPECT_TRUE(in.is_open()) << path;

    std::string line;
    while (std::getline(in, line)) {
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        const auto colon = trimmed.find(':');
        EXPECT_NE(colon, std::string::npos) << trimmed;
        const auto key = trim(trimmed.substr(0, colon));
        const auto value = trim(trimmed.substr(colon + 1));

        if (key == "name") {
            fixture.name = value;
        } else if (key == "role") {
            fixture.is_client = value == "client";
        } else if (key == "input_hex") {
            fixture.input = decode_hex(value);
        } else if (key == "expected_output_hex") {
            fixture.expected_output = decode_hex(value);
        } else if (key == "expected_accept_stream_id") {
            fixture.expected_accept_stream_id = static_cast<StreamId>(std::stoul(value));
        } else if (key == "expected_read_hex") {
            fixture.expected_read = decode_hex(value);
        } else {
            ADD_FAILURE() << "unknown fixture key in " << path << ": " << key;
        }
    }

    if (fixture.name.empty()) {
        fixture.name = path.stem().string();
    }
    return fixture;
}

std::vector<Fixture> load_fixtures() {
    std::vector<Fixture> fixtures;
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(PEERCORE_YAMUX_FIXTURE_DIR)) {
        if (entry.is_regular_file() && entry.path().extension() == ".fixture") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        fixtures.push_back(load_fixture(path));
    }
    return fixtures;
}

std::vector<uint8_t> drain_stream(StreamHandle stream) {
    std::vector<uint8_t> out;
    std::array<uint8_t, 1024> buf{};
    while (true) {
        auto read = stream->try_read(buf);
        if (read.is_err()) {
            EXPECT_EQ(read.error().message, "EAGAIN");
            break;
        }
        if (read.value() == 0) break;
        out.insert(out.end(),
                   buf.begin(),
                   buf.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
    return out;
}

}  // namespace

TEST(YamuxReplay, RunsFixtureSuite) {
    const auto fixtures = load_fixtures();
    ASSERT_FALSE(fixtures.empty());

    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.name);

        YamuxSession session(1, fixture.is_client);
        std::shared_ptr<YamuxStream> accepted_stream;
        session.set_accept_callback([&](std::shared_ptr<YamuxStream> stream) {
            accepted_stream = std::move(stream);
        });

        auto received = session.receive(ConstBytes(fixture.input.data(), fixture.input.size()));
        ASSERT_TRUE(received.is_ok()) << received.error_message();

        const auto outgoing = session.drain_outgoing();
        EXPECT_EQ(encode_hex(ConstBytes(outgoing.data(), outgoing.size())),
                  encode_hex(ConstBytes(fixture.expected_output.data(), fixture.expected_output.size())));

        if (fixture.expected_accept_stream_id.has_value()) {
            ASSERT_TRUE(accepted_stream) << "expected accepted stream";
            EXPECT_EQ(accepted_stream->id(), *fixture.expected_accept_stream_id);
        } else {
            EXPECT_FALSE(accepted_stream) << "unexpected accepted stream";
        }

        if (!fixture.expected_read.empty()) {
            ASSERT_TRUE(accepted_stream) << "expected readable accepted stream";
            EXPECT_EQ(drain_stream(accepted_stream), fixture.expected_read);
        }
    }
}
