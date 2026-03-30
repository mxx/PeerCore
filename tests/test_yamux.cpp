#include <gtest/gtest.h>

#include "../src/protocol/yamux/yamux.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using namespace peercore;
using namespace peercore::protocol::yamux;

namespace {

uint16_t read_be16(ConstBytes bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                                 static_cast<uint16_t>(bytes[1]));
}

uint32_t read_be32(ConstBytes bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

YamuxHeader parse_header(ConstBytes bytes) {
    EXPECT_GE(bytes.size(), 12u);
    return YamuxHeader{
        .version = bytes[0],
        .type = bytes[1],
        .flags = read_be16(bytes.subspan(2, 2)),
        .stream_id = read_be32(bytes.subspan(4, 4)),
        .length = read_be32(bytes.subspan(8, 4)),
    };
}

void transfer(YamuxSession& from, YamuxSession& to) {
    auto outgoing = from.drain_outgoing();
    if (outgoing.empty()) return;
    auto received = to.receive(ConstBytes(outgoing.data(), outgoing.size()));
    ASSERT_TRUE(received.is_ok()) << received.error_message();
}

void pump(YamuxSession& a, YamuxSession& b, int rounds = 1) {
    for (int i = 0; i < rounds; ++i) {
        transfer(a, b);
        transfer(b, a);
    }
}

void drain_stream(StreamHandle stream, std::vector<uint8_t>& out) {
    std::array<uint8_t, 8192> buf{};
    while (true) {
        auto read = stream->try_read(buf);
        if (read.is_err()) {
            EXPECT_EQ(read.error().message, "EAGAIN");
            break;
        }
        if (read.value() == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
}

}  // namespace

TEST(YamuxSession, AcknowledgesInboundSynFrame) {
    YamuxSession client(1, true);
    YamuxSession server(2, false);

    std::shared_ptr<YamuxStream> inbound_stream;
    server.set_accept_callback([&](std::shared_ptr<YamuxStream> stream) {
        inbound_stream = std::move(stream);
    });

    auto outbound_stream = client.open_stream();
    ASSERT_TRUE(outbound_stream.is_ok()) << outbound_stream.error().message;

    transfer(client, server);

    ASSERT_TRUE(inbound_stream);
    auto ack_bytes = server.drain_outgoing();
    ASSERT_EQ(ack_bytes.size(), 12u);

    const auto ack = parse_header(ConstBytes(ack_bytes.data(), ack_bytes.size()));
    EXPECT_EQ(ack.version, kYamuxVersion);
    EXPECT_EQ(ack.type, static_cast<uint8_t>(YamuxType::Data));
    EXPECT_EQ(ack.flags, static_cast<uint16_t>(YamuxFlag::ACK));
    EXPECT_EQ(ack.stream_id, outbound_stream.value()->id());
    EXPECT_EQ(ack.length, 0u);
}

TEST(YamuxSession, RepliesToPingWithAck) {
    YamuxSession server(2, false);

    std::vector<uint8_t> ping{
        kYamuxVersion,
        static_cast<uint8_t>(YamuxType::Ping),
        0,
        0,
        0,
        0,
        0,
        0,
        0x12,
        0x34,
        0x56,
        0x78,
    };
    auto received = server.receive(ConstBytes(ping.data(), ping.size()));
    ASSERT_TRUE(received.is_ok()) << received.error_message();

    auto reply = server.drain_outgoing();
    ASSERT_EQ(reply.size(), 12u);

    const auto ack = parse_header(ConstBytes(reply.data(), reply.size()));
    EXPECT_EQ(ack.version, kYamuxVersion);
    EXPECT_EQ(ack.type, static_cast<uint8_t>(YamuxType::Ping));
    EXPECT_EQ(ack.flags, static_cast<uint16_t>(YamuxFlag::ACK));
    EXPECT_EQ(ack.stream_id, 0u);
    EXPECT_EQ(ack.length, 0x12345678u);
}

TEST(YamuxSession, WindowUpdateAllowsTransferBeyondInitialWindow) {
    YamuxSession client(1, true);
    YamuxSession server(2, false);

    std::shared_ptr<YamuxStream> inbound_stream;
    server.set_accept_callback([&](std::shared_ptr<YamuxStream> stream) {
        inbound_stream = std::move(stream);
    });

    auto outbound_stream = client.open_stream();
    ASSERT_TRUE(outbound_stream.is_ok()) << outbound_stream.error().message;
    pump(client, server, 2);

    ASSERT_TRUE(inbound_stream);

    std::vector<uint8_t> payload(kInitialStreamWindow + 4096u);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i % 251u);
    }

    std::vector<uint8_t> received_payload;
    size_t total_written = 0;
    int guard = 0;
    while (total_written < payload.size()) {
        auto wrote = outbound_stream.value()->try_write(
            ConstBytes(payload.data() + total_written, payload.size() - total_written));
        if (wrote.is_ok()) {
            total_written += wrote.value();
        } else {
            EXPECT_EQ(wrote.error().message, "EAGAIN");
        }

        pump(client, server, 2);
        drain_stream(inbound_stream, received_payload);
        pump(server, client, 2);

        ASSERT_LT(++guard, 32);
    }

    pump(client, server, 2);
    drain_stream(inbound_stream, received_payload);

    ASSERT_EQ(received_payload.size(), payload.size());
    EXPECT_EQ(received_payload, payload);
}
