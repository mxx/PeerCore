#include <gtest/gtest.h>

#include "../src/protocol/multistream_select.hpp"

#include <fstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

using namespace peercore;
using namespace peercore::protocol;

namespace {

size_t current_rss_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),
                  MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<size_t>(info.resident_size);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    size_t total_pages = 0;
    size_t resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
        return 0;
    }
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }
    return resident_pages * static_cast<size_t>(page_size);
#else
    return 0;
#endif
}

}  // namespace

TEST(MultistreamSelect, RoundTripsSupportedProtocol) {
    const ProtocolId proto = "/noise";

    auto outbound = MultistreamSelect::prepare_outbound({proto});
    ASSERT_TRUE(outbound.is_ok());

    auto inbound = MultistreamSelect::negotiate_inbound(outbound.value(), {proto});
    ASSERT_TRUE(inbound.is_ok());
    ASSERT_TRUE(inbound.value().protocol.has_value());
    EXPECT_EQ(*inbound.value().protocol, proto);

    auto selected = MultistreamSelect::read_outbound_response(inbound.value().outbound, proto);
    ASSERT_TRUE(selected.is_ok());
    EXPECT_EQ(selected.value(), proto);
}

TEST(MultistreamSelect, ReturnsNaForUnsupportedProtocol) {
    auto outbound = MultistreamSelect::prepare_outbound({"/yamux/1.0.0"});
    ASSERT_TRUE(outbound.is_ok());

    auto inbound = MultistreamSelect::negotiate_inbound(outbound.value(), {"/noise"});
    ASSERT_TRUE(inbound.is_ok());
    EXPECT_FALSE(inbound.value().protocol.has_value());

    auto selected = MultistreamSelect::read_outbound_response(
        inbound.value().outbound, "/yamux/1.0.0");
    ASSERT_TRUE(selected.is_err());
    EXPECT_EQ(selected.error().message, "protocol not supported");
}

TEST(MultistreamSelect, RejectsUnexpectedHeader) {
    const std::vector<uint8_t> incoming{
        0x04, 'b', 'a', 'd', '\n',
        0x07, '/', 'n', 'o', 'i', 's', 'e', '\n',
    };

    auto inbound = MultistreamSelect::negotiate_inbound(incoming, {"/noise"});
    ASSERT_TRUE(inbound.is_err());
    EXPECT_EQ(inbound.error().message, "unexpected multistream header");
}

TEST(MultistreamSelect, RequiresAtLeastOneProposal) {
    auto outbound = MultistreamSelect::prepare_outbound({});
    ASSERT_TRUE(outbound.is_err());
    EXPECT_EQ(outbound.error().message, "no protocols proposed");
}

TEST(MultistreamSelect, RejectsOverflowOrTruncatedLengthPrefixes) {
    const std::vector<uint8_t> overflow_length{
        0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00,
    };
    auto overflow = MultistreamSelect::negotiate_inbound(overflow_length, {"/noise"});
    ASSERT_TRUE(overflow.is_err());
    EXPECT_EQ(overflow.error().message, "invalid multistream length");

    const std::vector<uint8_t> truncated_length{0x80};
    auto truncated = MultistreamSelect::negotiate_inbound(truncated_length, {"/noise"});
    ASSERT_TRUE(truncated.is_err());
    EXPECT_EQ(truncated.error().message, "incomplete multistream length");

    const std::vector<uint8_t> incomplete_message{0x05, 'a', 'b'};
    auto incomplete = MultistreamSelect::negotiate_inbound(incomplete_message, {"/noise"});
    ASSERT_TRUE(incomplete.is_err());
    EXPECT_EQ(incomplete.error().message, "incomplete multistream message");
}

TEST(MultistreamSelect, FatigueNegotiationKeepsMemoryBounded) {
    constexpr size_t kWarmupIterations = 200;
    constexpr size_t kIterations = 20000;
    constexpr size_t kMaxAllowedGrowthBytes = 24ull * 1024ull * 1024ull;
    const ProtocolId proposal = "/noise";

    auto one_round = [&proposal]() -> bool {
        auto outbound = MultistreamSelect::prepare_outbound({proposal});
        if (outbound.is_err()) return false;

        auto inbound = MultistreamSelect::negotiate_inbound(outbound.value(), {proposal});
        if (inbound.is_err() || !inbound.value().protocol.has_value()) return false;

        auto selected = MultistreamSelect::read_outbound_response(inbound.value().outbound,
                                                                  proposal);
        return selected.is_ok() && selected.value() == proposal;
    };

    for (size_t i = 0; i < kWarmupIterations; ++i) {
        ASSERT_TRUE(one_round());
    }

    const size_t rss_before = current_rss_bytes();
    for (size_t i = 0; i < kIterations; ++i) {
        ASSERT_TRUE(one_round());
    }
    const size_t rss_after = current_rss_bytes();

    if (rss_before > 0 && rss_after > 0) {
        const size_t growth = (rss_after > rss_before) ? (rss_after - rss_before) : 0;
        EXPECT_LT(growth, kMaxAllowedGrowthBytes)
            << "rss_before=" << rss_before << ", rss_after=" << rss_after;
    }
}
