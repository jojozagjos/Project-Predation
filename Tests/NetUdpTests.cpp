#include "Engine/Net/IceLink.h"
#include "Game/Net/IceCarrier.h"
#include "Engine/Net/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
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

namespace
{

// Two of these wired to each other are a pipe: what one sends, the other receives. It stands in for
// a connection punched through two routers, which is a thing no test can make happen.
//
// A carrier can have several far ends, so this one can be joined to several partners. Link n is
// whoever was joined nth, on both sides, which is the same arrangement the game makes: the host
// adds a link per invitation and the guest who answers it has that host as its only link.
class PairedCarrier final : public DatagramCarrier
{
public:
    size_t Links() const override { return m_far.size(); }

    bool Send(size_t link, const uint8_t* data, size_t bytes) override
    {
        if (link >= m_far.size() || m_far[link] == nullptr)
        {
            return false;
        }
        // Delivered to the link the far side holds this one under, which is not always the same
        // number: the host may know a guest as link 2 while that guest knows the host as link 0.
        PairedCarrier* target = m_far[link];
        size_t back = 0;
        for (size_t i = 0; i < target->m_far.size(); ++i)
        {
            if (target->m_far[i] == this)
            {
                back = i;
                break;
            }
        }
        target->m_incoming.emplace_back(back, std::vector<uint8_t>(data, data + bytes));
        return true;
    }

    bool Receive(size_t& link, std::vector<uint8_t>& out) override
    {
        if (m_incoming.empty())
        {
            return false;
        }
        link = m_incoming.front().first;
        out = std::move(m_incoming.front().second);
        m_incoming.erase(m_incoming.begin());
        return true;
    }

    bool Live(size_t link) const override { return link < m_far.size() && m_far[link] != nullptr; }

    static void Join(const std::shared_ptr<PairedCarrier>& a, const std::shared_ptr<PairedCarrier>& b)
    {
        a->m_far.push_back(b.get());
        b->m_far.push_back(a.get());
    }

private:
    std::vector<PairedCarrier*> m_far;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> m_incoming;
};

} // namespace

TEST_CASE("The transport works over a carrier as well as over a socket", "[net][udp][ice]")
{
    // A connection punched through two routers carries datagrams exactly as a socket does and
    // cannot be bound to or read from like one. Rather than writing the sequence numbers and the
    // acknowledgements a second time underneath it, the transport takes a carrier and uses that:
    // everything above, which is all of the game, cannot tell the difference.
    auto hostCarrier = std::make_shared<PairedCarrier>();
    auto clientCarrier = std::make_shared<PairedCarrier>();
    PairedCarrier::Join(hostCarrier, clientCarrier);

    auto host = CreateCarrierTransport(hostCarrier, 3u);
    auto client = CreateCarrierTransport(clientCarrier, 4u);
    REQUIRE(host != nullptr);
    REQUIRE(client != nullptr);

    // No port is bound and no address is resolved: the far end is wherever the carrier goes.
    REQUIRE(host->Listen(0));
    REQUIRE(client->Connect("ignored", 0));

    std::vector<NetPacket> atHost;
    std::vector<NetPacket> atClient;
    const auto pump = [&](int ticks)
    {
        for (int i = 0; i < ticks; ++i)
        {
            std::vector<NetPacket> batch;
            host->Poll(kTick, batch);
            for (NetPacket& packet : batch)
            {
                atHost.push_back(std::move(packet));
            }
            batch.clear();
            client->Poll(kTick, batch);
            for (NetPacket& packet : batch)
            {
                atClient.push_back(std::move(packet));
            }
        }
    };

    pump(10);
    REQUIRE(host->Peers().size() == 1);
    const PeerId clientPeer = host->Peers().front();

    // Both channels, both ways.
    const std::string reliable = "the door is open";
    const std::string unreliable = "where everybody is";
    host->Send(clientPeer, Channel::Reliable, reinterpret_cast<const uint8_t*>(reliable.data()),
               reliable.size());
    host->Send(clientPeer, Channel::Unreliable, reinterpret_cast<const uint8_t*>(unreliable.data()),
               unreliable.size());
    const std::string up = "I pulled the trigger";
    client->Send(kHostPeer, Channel::Reliable, reinterpret_cast<const uint8_t*>(up.data()), up.size());
    pump(10);

    REQUIRE(atClient.size() >= 2);
    bool sawReliable = false;
    bool sawUnreliable = false;
    for (const NetPacket& packet : atClient)
    {
        const std::string text(packet.bytes.begin(), packet.bytes.end());
        sawReliable = sawReliable || (text == reliable && packet.channel == Channel::Reliable);
        sawUnreliable = sawUnreliable || (text == unreliable && packet.channel == Channel::Unreliable);
    }
    CHECK(sawReliable);
    CHECK(sawUnreliable);

    REQUIRE(atHost.size() >= 1);
    CHECK(std::string(atHost.front().bytes.begin(), atHost.front().bytes.end()) == up);
}

TEST_CASE("Two ends punch through to each other and the game runs over it", "[net][udp][ice]")
{
    // The whole path, end to end: two agents gather what they know about themselves, swap the one
    // line of text a player would paste, dial each other, and then carry the game's own transport.
    // Both are on this machine, so what gets used is the host candidate rather than a hole through
    // a router, and no public server is asked: what is being checked is the plumbing, which is the
    // part that can be wrong in a way no amount of trying it with a friend would explain.
    IceLink::Settings settings;
    settings.stunHost.clear(); // nothing to ask, and nothing to wait for

    auto a = std::make_shared<IceLink>();
    auto b = std::make_shared<IceLink>();
    REQUIRE(a->Start(settings));
    REQUIRE(b->Start(settings));

    const auto waitFor = [](const std::shared_ptr<IceLink>& link, auto&& ready, int seconds)
    {
        for (int i = 0; i < seconds * 100 && !ready(link); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return ready(link);
    };
    const auto hasCode = [](const std::shared_ptr<IceLink>& link) { return !link->LocalCode().empty(); };

    REQUIRE(waitFor(a, hasCode, 5));
    REQUIRE(waitFor(b, hasCode, 5));

    // What the two players paste to each other.
    REQUIRE(a->SetRemoteCode(b->LocalCode()));
    REQUIRE(b->SetRemoteCode(a->LocalCode()));

    const auto connected = [](const std::shared_ptr<IceLink>& link)
    { return link->Status() == IceLink::State::Connected; };
    INFO("a is " << static_cast<int>(a->Status()) << ", b is " << static_cast<int>(b->Status()));
    REQUIRE(waitFor(a, connected, 10));
    REQUIRE(waitFor(b, connected, 10));

    // And the game over the top of it, with nothing about the game aware of any of the above.
    auto host = CreateCarrierTransport(std::make_shared<IceCarrier>(a), 11u);
    auto client = CreateCarrierTransport(std::make_shared<IceCarrier>(b), 12u);
    REQUIRE(host->Listen(0));
    REQUIRE(client->Connect("punched", 0));

    std::vector<NetPacket> atClient;
    std::vector<NetPacket> batch;
    PeerId clientPeer = kInvalidPeer;
    const std::string message = "the lights just went out";
    for (int i = 0; i < 400 && atClient.empty(); ++i)
    {
        host->Poll(kTick, batch);
        batch.clear();
        client->Poll(kTick, batch);
        for (NetPacket& packet : batch)
        {
            atClient.push_back(std::move(packet));
        }
        if (clientPeer == kInvalidPeer && !host->Peers().empty())
        {
            clientPeer = host->Peers().front();
            host->Send(clientPeer, Channel::Reliable,
                       reinterpret_cast<const uint8_t*>(message.data()), message.size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    REQUIRE(clientPeer != kInvalidPeer);
    REQUIRE_FALSE(atClient.empty());
    CHECK(std::string(atClient.front().bytes.begin(), atClient.front().bytes.end()) == message);
}

TEST_CASE("A host takes a full lobby over punched connections", "[net][udp][ice]")
{
    // A punched hole joins two machines and no more, so a carrier used to be a single pipe and a
    // game over the internet was two players however large the lobby said it was. The host now
    // holds one link per guest, each becoming an ordinary peer with its own stand-in address, so
    // everything above this — the reliability, the roster, the snapshots — is unchanged and does
    // not know the difference between a punched link and a socket.
    auto hostCarrier = std::make_shared<PairedCarrier>();
    std::vector<std::shared_ptr<PairedCarrier>> guestCarriers;
    for (int i = 0; i < 3; ++i)
    {
        guestCarriers.push_back(std::make_shared<PairedCarrier>());
        PairedCarrier::Join(hostCarrier, guestCarriers.back());
    }
    REQUIRE(hostCarrier->Links() == 3);

    auto host = CreateCarrierTransport(hostCarrier, 11u);
    std::vector<std::unique_ptr<Transport>> guests;
    for (int i = 0; i < 3; ++i)
    {
        guests.push_back(CreateCarrierTransport(guestCarriers[static_cast<size_t>(i)],
                                                static_cast<uint32_t>(20 + i)));
        REQUIRE(guests.back() != nullptr);
    }

    REQUIRE(host->Listen(0));
    for (auto& guest : guests)
    {
        REQUIRE(guest->Connect("ignored", 0));
    }

    std::vector<NetPacket> atHost;
    std::vector<std::vector<NetPacket>> atGuest(guests.size());
    const auto pump = [&](int ticks)
    {
        for (int i = 0; i < ticks; ++i)
        {
            std::vector<NetPacket> batch;
            host->Poll(kTick, batch);
            for (NetPacket& packet : batch)
            {
                atHost.push_back(std::move(packet));
            }
            for (size_t g = 0; g < guests.size(); ++g)
            {
                batch.clear();
                guests[g]->Poll(kTick, batch);
                for (NetPacket& packet : batch)
                {
                    atGuest[g].push_back(std::move(packet));
                }
            }
        }
    };

    pump(20);

    // Three guests, three peers, told apart.
    INFO("host holds " << host->Peers().size() << " peers");
    REQUIRE(host->Peers().size() == 3);
    const std::vector<PeerId> peers = host->Peers();
    CHECK(peers[0] != peers[1]);
    CHECK(peers[1] != peers[2]);
    CHECK(peers[0] != peers[2]);

    // A message to one guest reaches that guest and nobody else. This is the part a single shared
    // pipe cannot do at all: everything sent went to everyone.
    const std::string secret = "only for the second";
    host->Send(peers[1], Channel::Reliable, reinterpret_cast<const uint8_t*>(secret.data()),
               secret.size());
    pump(20);

    const auto heard = [&](size_t guest)
    {
        for (const NetPacket& packet : atGuest[guest])
        {
            if (std::string(packet.bytes.begin(), packet.bytes.end()) == secret)
            {
                return true;
            }
        }
        return false;
    };
    CHECK_FALSE(heard(0));
    CHECK(heard(1));
    CHECK_FALSE(heard(2));

    // And each guest's own words come back tagged with the right sender.
    for (size_t g = 0; g < guests.size(); ++g)
    {
        const std::string line = "guest " + std::to_string(g);
        guests[g]->Send(kHostPeer, Channel::Reliable, reinterpret_cast<const uint8_t*>(line.data()),
                        line.size());
    }
    pump(20);

    int matched = 0;
    for (const NetPacket& packet : atHost)
    {
        const std::string text(packet.bytes.begin(), packet.bytes.end());
        for (size_t g = 0; g < guests.size(); ++g)
        {
            if (text == "guest " + std::to_string(g))
            {
                CHECK(packet.peer == peers[g]);
                ++matched;
            }
        }
    }
    INFO("matched " << matched << " of 3 guest messages to the right peer");
    CHECK(matched == 3);
}
