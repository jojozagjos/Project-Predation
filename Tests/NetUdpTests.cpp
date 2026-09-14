#include "Engine/Net/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pred;

// These run over a real socket on 127.0.0.1 rather than a stub. The loopback transport proves the
// game logic; only a real socket proves the packet format, the sequence numbers, the resends and
// the acknowledgements, which is where a networking bug actually lives.
namespace
{

constexpr float kTick = 1.0f / 60.0f;

struct UdpPair
{
    std::unique_ptr<Transport> host = CreateUdpTransport(11u);
    std::unique_ptr<Transport> client = CreateUdpTransport(22u);
    std::vector<NetPacket> atHost;
    std::vector<NetPacket> atClient;
    PeerId clientPeer = kInvalidPeer;

    explicit UdpPair(uint16_t port)
    {
        REQUIRE(host->Listen(port));
        REQUIRE(client->Connect("127.0.0.1", port));
    }

    // The simulated clock runs faster than the wall clock on purpose: a millisecond of real waiting
    // is enough for a datagram to cross localhost, and the transport's own timers should not have
    // to be waited out in real time for a test to finish.
    void Pump(int iterations)
    {
        std::vector<NetPacket> batch;
        for (int i = 0; i < iterations; ++i)
        {
            host->Poll(kTick, batch);
            atHost.insert(atHost.end(), batch.begin(), batch.end());
            client->Poll(kTick, batch);
            atClient.insert(atClient.end(), batch.begin(), batch.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool Establish(int iterations = 40)
    {
        Pump(iterations);
        const std::vector<PeerId> joined = host->TakeConnected();
        if (joined.empty())
        {
            return false;
        }
        clientPeer = joined[0];
        return !client->TakeConnected().empty();
    }
};

void SendText(Transport& from, PeerId peer, Channel channel, const std::string& text)
{
    from.Send(peer, channel, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string TextOf(const NetPacket& packet)
{
    return std::string(packet.bytes.begin(), packet.bytes.end());
}

} // namespace

TEST_CASE("A client reaches a host over a real socket", "[net][udp]")
{
    UdpPair pair(47811);
    REQUIRE(pair.Establish());

    CHECK(pair.clientPeer != kInvalidPeer);
    CHECK(pair.host->Peers().size() == 1);
    CHECK(pair.client->Peers().size() == 1);
    CHECK(pair.client->Peers()[0] == kHostPeer);
}

TEST_CASE("Payloads cross a real socket both ways on both channels", "[net][udp]")
{
    UdpPair pair(47812);
    REQUIRE(pair.Establish());

    SendText(*pair.client, kHostPeer, Channel::Unreliable, "input");
    SendText(*pair.client, kHostPeer, Channel::Reliable, "join");
    SendText(*pair.host, pair.clientPeer, Channel::Unreliable, "snapshot");
    SendText(*pair.host, pair.clientPeer, Channel::Reliable, "welcome");

    pair.Pump(30);

    REQUIRE(pair.atHost.size() >= 2);
    REQUIRE(pair.atClient.size() >= 2);

    bool sawInput = false;
    bool sawJoin = false;
    for (const NetPacket& packet : pair.atHost)
    {
        sawInput = sawInput || (TextOf(packet) == "input" && packet.channel == Channel::Unreliable);
        sawJoin = sawJoin || (TextOf(packet) == "join" && packet.channel == Channel::Reliable);
        CHECK(packet.peer == pair.clientPeer);
    }
    CHECK(sawInput);
    CHECK(sawJoin);

    bool sawSnapshot = false;
    bool sawWelcome = false;
    for (const NetPacket& packet : pair.atClient)
    {
        sawSnapshot = sawSnapshot || TextOf(packet) == "snapshot";
        sawWelcome = sawWelcome || TextOf(packet) == "welcome";
        CHECK(packet.peer == kHostPeer);
    }
    CHECK(sawSnapshot);
    CHECK(sawWelcome);
}

TEST_CASE("A reliable message survives a link that drops half of everything", "[net][udp]")
{
    UdpPair pair(47813);
    REQUIRE(pair.Establish());

    NetConditions awful;
    awful.lossPercent = 50.0f;
    pair.client->SetConditions(awful);

    SendText(*pair.client, kHostPeer, Channel::Reliable, "fire");
    pair.Pump(120); // long enough for several resends

    bool arrived = false;
    int copies = 0;
    for (const NetPacket& packet : pair.atHost)
    {
        if (TextOf(packet) == "fire")
        {
            arrived = true;
            ++copies;
        }
    }
    CHECK(arrived);
    // Resent until acknowledged, but handed to the game exactly once however many copies crossed.
    CHECK(copies == 1);
}

TEST_CASE("Reliable messages arrive in order over a bad real link", "[net][udp]")
{
    UdpPair pair(47814);
    REQUIRE(pair.Establish());

    NetConditions awful;
    awful.latencyMs = 20.0f;
    awful.jitterMs = 60.0f;
    awful.lossPercent = 25.0f;
    awful.duplicatePercent = 20.0f;
    pair.client->SetConditions(awful);

    constexpr int kCount = 12;
    for (int i = 0; i < kCount; ++i)
    {
        SendText(*pair.client, kHostPeer, Channel::Reliable, "m" + std::to_string(i));
    }
    pair.Pump(240);

    std::vector<std::string> received;
    for (const NetPacket& packet : pair.atHost)
    {
        received.push_back(TextOf(packet));
    }
    REQUIRE(received.size() == kCount);
    for (int i = 0; i < kCount; ++i)
    {
        INFO("position " << i);
        CHECK(received[static_cast<size_t>(i)] == "m" + std::to_string(i));
    }
}

TEST_CASE("Disconnecting over a real socket is noticed at the other end", "[net][udp]")
{
    UdpPair pair(47815);
    REQUIRE(pair.Establish());

    pair.client->Disconnect(kHostPeer);
    pair.Pump(30);

    CHECK(pair.host->Peers().empty());
    CHECK(pair.host->TakeDisconnected() == std::vector<PeerId>{pair.clientPeer});
}

TEST_CASE("Two hosts cannot bind the same UDP port", "[net][udp]")
{
    auto first = CreateUdpTransport();
    auto second = CreateUdpTransport();
    REQUIRE(first->Listen(47816));
    CHECK_FALSE(second->Listen(47816));
}

TEST_CASE("The host can say where it can be reached", "[net][udp]")
{
    // Telling a player to hand out "your address" is advice nobody can act on: the obvious thing to
    // look up is the public address, which belongs to the router rather than to the machine, and a
    // friend on the same network cannot reach it. So the game lists the addresses it is actually
    // listening on.
    const std::vector<std::string> addresses = LocalNetworkAddresses();

    // Loopback is the one that always works and never helps, so it is left out.
    for (const std::string& address : addresses)
    {
        INFO("address " << address);
        CHECK(address.rfind("127.", 0) != 0);
        CHECK(address.find_first_not_of("0123456789.") == std::string::npos);
    }

    // And whatever it lists, a host really does accept a connection on the loopback that is not
    // listed, because it binds every interface rather than one.
    auto host = CreateUdpTransport(1);
    REQUIRE(host->Listen(48311));
    auto client = CreateUdpTransport(2);
    REQUIRE(client->Connect("127.0.0.1", 48311));

    std::vector<NetPacket> packets;
    bool joined = false;
    for (int i = 0; i < 120 && !joined; ++i)
    {
        host->Poll(1.0f / 60.0f, packets);
        client->Poll(1.0f / 60.0f, packets);
        joined = !host->Peers().empty();
    }
    CHECK(joined);
}

TEST_CASE("A host stops taking peers rather than growing without limit", "[net][udp][security]")
{
    // An open UDP port is reachable by anybody who knows the address, and a connect request is the
    // cheapest thing to send: one datagram bought a peer with two vectors in it, and a stream of
    // datagrams with different source addresses bought as many as the sender cared to send. Not a
    // way in, but a way to use the machine up. A full game is four players.
    auto host = CreateUdpTransport(99u);
    REQUIRE(host->Listen(41200));

    std::vector<std::unique_ptr<Transport>> callers;
    for (int i = 0; i < 24; ++i)
    {
        auto caller = CreateUdpTransport(static_cast<uint32_t>(200 + i));
        REQUIRE(caller->Connect("127.0.0.1", 41200));
        callers.push_back(std::move(caller));
    }

    std::vector<NetPacket> packets;
    for (int i = 0; i < 40; ++i)
    {
        host->Poll(kTick, packets);
        for (auto& caller : callers)
        {
            caller->Poll(kTick, packets);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    INFO("the host took " << host->Peers().size() << " of 24 callers");
    CHECK(host->Peers().size() <= 16);
    // And it still took some, so the limit is a limit rather than a refusal to work.
    CHECK(host->Peers().size() >= 4);
}
