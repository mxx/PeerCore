#include <gtest/gtest.h>
#include <peercore/swarm.hpp>
#include <peercore/peer_store.hpp>

#include <chrono>
#include <sodium.h>
#include <thread>
#include <vector>

using namespace peercore;

namespace {

constexpr std::string_view kTestPeerId =
    "12D3KooW00112233445566778899aabbccddeeff00112233445566778899aabb";

std::vector<SwarmEvent> drain_events(Swarm& swarm) {
    std::vector<SwarmEvent> events;
    while (auto ev = swarm.next_event()) {
        events.push_back(std::move(*ev));
    }
    return events;
}

Identity make_identity() {
    Identity identity;
    ::crypto_sign_keypair(identity.secret_key.data() + 32, identity.secret_key.data());
    identity.peer_id = PeerId::from_bytes(
        std::span<const uint8_t, 32>(identity.secret_key.data() + 32, 32));
    return identity;
}

}  // namespace

TEST(Swarm, StartStop) {
    PeerStore ps;
    Swarm swarm(ps);
    EXPECT_TRUE(swarm.start().is_ok());
    EXPECT_TRUE(swarm.stop().is_ok());
}

TEST(Swarm, NoEventsInitially) {
    PeerStore ps;
    Swarm swarm(ps);
    swarm.start();
    EXPECT_FALSE(swarm.next_event().has_value());
}

TEST(Swarm, DialUnknownPeerFails) {
    PeerStore ps;
    Swarm swarm(ps);
    std::array<uint8_t, 32> raw{};
    auto peer = PeerId::from_bytes(raw);
    auto res = swarm.dial_peer(peer);
    EXPECT_TRUE(res.is_err());
}

TEST(Swarm, Snapshot) {
    PeerStore ps;
    Swarm swarm(ps);
    auto snap = swarm.snapshot();
    EXPECT_EQ(snap.connection_count, 0u);
}

TEST(Swarm, ListenAndDialEstablishConnection) {
    ASSERT_GE(::sodium_init(), 0);
    PeerStore server_store;
    PeerStore client_store;
    auto server_identity = make_identity();
    auto client_identity = make_identity();
    Swarm server(server_store, server_identity, {"/mplex/6.7.0", "/yamux/1.0.0"});
    Swarm client(client_store, client_identity, {"/yamux/1.0.0"});

    ASSERT_TRUE(server.start().is_ok());
    ASSERT_TRUE(client.start().is_ok());

    auto listen_res = server.listen_on(Multiaddr("/ip4/127.0.0.1/tcp/0"));
    if (listen_res.is_err() &&
        listen_res.error_message().find("Operation not permitted") != std::string::npos) {
        GTEST_SKIP() << listen_res.error_message();
    }
    ASSERT_TRUE(listen_res.is_ok()) << listen_res.error_message();

    auto server_events = drain_events(server);
    ASSERT_FALSE(server_events.empty());
    ASSERT_EQ(server_events.front().type, SwarmEvent::Type::ListenerStarted);
    ASSERT_FALSE(server_events.front().detail.empty());

    ASSERT_TRUE(client.dial_addr(Multiaddr::from_ip4_tcp("127.0.0.1",
                                                         static_cast<uint16_t>(std::stoi(
                                                             server_events.front().detail.substr(
                                                                 server_events.front().detail.rfind('/') + 1))),
                                                         kTestPeerId)).is_ok());

    bool server_incoming = false;
    bool server_established = false;
    bool client_established = false;
    bool server_identified = false;
    bool client_identified = false;
    bool server_muxers_observed = false;
    bool client_muxers_observed = false;
    bool client_stream_opened = false;
    bool stream_open_requested = false;

    for (int i = 0; i < 100; ++i) {
        server.poll_once();
        client.poll_once();

        for (auto& ev : drain_events(server)) {
            if (ev.type == SwarmEvent::Type::IncomingConnection) server_incoming = true;
            if (ev.type == SwarmEvent::Type::PeerIdentified &&
                ev.peer_id == std::optional<PeerId>(client_identity.peer_id)) {
                server_identified = true;
            }
            if (ev.type == SwarmEvent::Type::ConnectionEstablished &&
                ev.peer_id == std::optional<PeerId>(client_identity.peer_id)) {
                server_established = true;
            }
            if (ev.type == SwarmEvent::Type::ProtocolNegotiated &&
                ev.peer_id == std::optional<PeerId>(client_identity.peer_id) &&
                ev.detail == "/yamux/1.0.0") {
                server_muxers_observed = true;
            }
        }
        for (auto& ev : drain_events(client)) {
            if (ev.type == SwarmEvent::Type::PeerIdentified &&
                ev.peer_id == std::optional<PeerId>(server_identity.peer_id)) {
                client_identified = true;
            }
            if (ev.type == SwarmEvent::Type::ConnectionEstablished &&
                ev.peer_id == std::optional<PeerId>(server_identity.peer_id)) {
                client_established = true;
            }
            if (ev.type == SwarmEvent::Type::StreamOpened) {
                client_stream_opened = true;
            }
            if (ev.type == SwarmEvent::Type::ProtocolNegotiated &&
                ev.peer_id == std::optional<PeerId>(server_identity.peer_id) &&
                ev.detail == "/mplex/6.7.0, /yamux/1.0.0") {
                client_muxers_observed = true;
            }
        }

        if (!stream_open_requested && client_established) {
            auto stream = client.open_stream(server_identity.peer_id, "/test/1.0.0");
            ASSERT_TRUE(stream.is_ok()) << stream.error().message;
            stream_open_requested = true;
        }

        if (server_incoming && server_established && client_established &&
            server_identified && client_identified &&
            server_muxers_observed && client_muxers_observed &&
            client_stream_opened) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(server_incoming);
    EXPECT_TRUE(server_identified);
    EXPECT_TRUE(server_established);
    EXPECT_TRUE(client_identified);
    EXPECT_TRUE(client_established);
    EXPECT_TRUE(server_muxers_observed);
    EXPECT_TRUE(client_muxers_observed);
    EXPECT_TRUE(client_stream_opened);
}
