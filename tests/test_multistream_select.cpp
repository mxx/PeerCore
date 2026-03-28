#include <gtest/gtest.h>

#include "../src/protocol/multistream_select.hpp"

using namespace peercore;
using namespace peercore::protocol;

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
