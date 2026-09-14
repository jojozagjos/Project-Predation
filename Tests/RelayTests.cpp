#include "Engine/Net/RelayProtocol.h"
#include "Engine/Net/RelayServer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

using namespace pred;

namespace
{

// Drives the relay without a socket. `from` is whatever the socket would have called the sender;
// the relay treats it as opaque, so a string is as good as an address and reads better in a test.
struct RelayHarness
{
    RelayServer server;
    std::vector<RelayServer::Outgoing> out;

    explicit RelayHarness(const RelayServer::Settings& settings = {}) : server(settings) {}

    void Send(const std::string& from, const RelayPacket& packet)
    {
        const std::vector<uint8_t> bytes = EncodeRelay(packet);
        server.Receive(from, bytes.data(), bytes.size(), out);
    }

    void Tick(float dt) { server.Tick(dt, out); }

    // Everything sent to one client since the last Clear, decoded.
    std::vector<RelayPacket> To(const std::string& who) const
    {
        std::vector<RelayPacket> found;
        for (const RelayServer::Outgoing& entry : out)
        {
            if (entry.to != who)
            {
                continue;
            }
            RelayPacket packet;
            if (DecodeRelay(entry.datagram.data(), entry.datagram.size(), packet))
            {
                found.push_back(std::move(packet));
            }
        }
        return found;
    }

    // The first message of a kind sent to somebody, or nothing.
    bool First(const std::string& who, RelayMessage kind, RelayPacket& outPacket) const
    {
        for (const RelayPacket& packet : To(who))
        {
            if (packet.kind == kind)
            {
                outPacket = packet;
                return true;
            }
        }
        return false;
    }

    void Clear() { out.clear(); }

    uint32_t OpenLobby(const std::string& host)
    {
        RelayPacket request;
        request.kind = RelayMessage::Host;
        Send(host, request);
        RelayPacket hosted;
        REQUIRE(First(host, RelayMessage::Hosted, hosted));
        Clear();
        return hosted.code;
    }

    uint8_t JoinLobby(const std::string& who, uint32_t code)
    {
        RelayPacket request;
        request.kind = RelayMessage::Join;
        request.code = code;
        Send(who, request);
        RelayPacket joined;
        REQUIRE(First(who, RelayMessage::Joined, joined));
        return joined.slot;
    }
};

} // namespace

TEST_CASE("A lobby code survives being written down", "[relay][code]")
{
    // A code is read aloud over voice chat or typed off a screenshot, so the alphabet leaves out
    // every pair people confuse. Nought and one are gone, which is what lets O and I go too.
    const std::string alphabet = kRelayCodeAlphabet;
    CHECK(alphabet.size() == 32);
    for (const char confusable : {'0', '1', 'I', 'O'})
    {
        INFO("alphabet must not contain " << confusable);
        CHECK(alphabet.find(confusable) == std::string::npos);
    }

    for (uint32_t value : {0u, 1u, 12345u, 0x3FFFFFFFu})
    {
        const std::string text = DecodeRelayCode(value);
        REQUIRE(text.size() == static_cast<size_t>(kRelayCodeLength));
        uint32_t back = 0;
        REQUIRE(EncodeRelayCode(text, back));
        CHECK(back == value);
    }

    // However somebody types it back.
    uint32_t typed = 0;
    const std::string printed = DecodeRelayCode(987654u);
    std::string lower = printed;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    REQUIRE(EncodeRelayCode(lower, typed));
    CHECK(typed == 987654u);
    // With the punctuation people add to long codes.
    REQUIRE(EncodeRelayCode(printed.substr(0, 3) + "-" + printed.substr(3), typed));
    CHECK(typed == 987654u);

    // And things that are not codes are refused, which is how the join box tells one from an address.
    uint32_t ignored = 0;
    CHECK_FALSE(EncodeRelayCode("192.168.1.20:27015", ignored));
    CHECK_FALSE(EncodeRelayCode("", ignored));
    CHECK_FALSE(EncodeRelayCode("ABC", ignored));          // too short
    CHECK_FALSE(EncodeRelayCode("ABCDEFG", ignored));      // too long
    CHECK_FALSE(EncodeRelayCode("ABCDE0", ignored));       // a symbol no code contains
}

TEST_CASE("Opening a lobby and joining it by code", "[relay]")
{
    RelayHarness relay;
    const uint32_t code = relay.OpenLobby("host");
    CHECK(relay.server.LobbyCount() == 1);
    CHECK(relay.server.CodeOf("host") == code);

    const uint8_t slot = relay.JoinLobby("guest", code);
    CHECK(slot == 1);
    CHECK(relay.server.ClientCount() == 2);

    // The host is told somebody arrived, and the newcomer is told who was already there. Both
    // matter: without the second, a guest cannot address the host it just joined.
    RelayPacket announced;
    REQUIRE(relay.First("host", RelayMessage::PeerJoined, announced));
    CHECK(announced.slot == 1);

    bool sawHost = false;
    for (const RelayPacket& packet : relay.To("guest"))
    {
        sawHost = sawHost || (packet.kind == RelayMessage::PeerJoined && packet.slot == kRelayHostSlot);
    }
    CHECK(sawHost);
}

TEST_CASE("A wrong code is refused and says so", "[relay]")
{
    RelayHarness relay;
    relay.OpenLobby("host");

    RelayPacket request;
    request.kind = RelayMessage::Join;
    request.code = 999999u; // not the one that was opened
    relay.Send("stranger", request);

    RelayPacket rejected;
    REQUIRE(relay.First("stranger", RelayMessage::Rejected, rejected));
    CHECK(rejected.reason == RelayRejection::NoSuchLobby);
    CHECK(relay.server.ClientCount() == 1);
}

TEST_CASE("A lobby fills up and then refuses", "[relay]")
{
    RelayHarness relay;
    const uint32_t code = relay.OpenLobby("host");
    for (int i = 1; i < kRelayMaxSlots; ++i)
    {
        relay.Clear();
        const uint8_t slot = relay.JoinLobby("guest" + std::to_string(i), code);
        CHECK(slot == static_cast<uint8_t>(i));
    }
    CHECK(relay.server.ClientCount() == kRelayMaxSlots);

    relay.Clear();
    RelayPacket request;
    request.kind = RelayMessage::Join;
    request.code = code;
    relay.Send("one too many", request);
    RelayPacket rejected;
    REQUIRE(relay.First("one too many", RelayMessage::Rejected, rejected));
    CHECK(rejected.reason == RelayRejection::LobbyFull);
}

TEST_CASE("Data goes to the slot it is addressed to and nowhere else", "[relay]")
{
    RelayHarness relay;
    const uint32_t code = relay.OpenLobby("host");
    relay.JoinLobby("a", code);
    relay.JoinLobby("b", code);
    relay.Clear();

    const std::string secret = "for the host only";
    RelayPacket data;
    data.kind = RelayMessage::Data;
    data.slot = kRelayHostSlot;
    data.payload.assign(secret.begin(), secret.end());
    relay.Send("a", data);

    RelayPacket relayed;
    REQUIRE(relay.First("host", RelayMessage::Relayed, relayed));
    CHECK(std::string(relayed.payload.begin(), relayed.payload.end()) == secret);
    // Stamped with who it came from, which the relay knows and the sender cannot forge: `a` is slot
    // one, and it said nothing about that.
    CHECK(relayed.slot == 1);

    // Nobody else heard it, which is the part a shared pipe cannot do.
    CHECK(relay.To("b").empty());
    CHECK(relay.To("a").empty());
}

TEST_CASE("A client cannot speak into a lobby it is not in", "[relay][security]")
{
    RelayHarness relay;
    const uint32_t code = relay.OpenLobby("host");
    relay.JoinLobby("guest", code);
    relay.Clear();

    // A stranger who has joined nothing, addressing slot zero.
    RelayPacket data;
    data.kind = RelayMessage::Data;
    data.slot = kRelayHostSlot;
    const std::string intrusion = "hello";
    data.payload.assign(intrusion.begin(), intrusion.end());
    relay.Send("stranger", data);

    CHECK(relay.To("host").empty());
    CHECK(relay.To("guest").empty());

    // And somebody in a different lobby, addressing a slot that exists in theirs.
    const uint32_t other = relay.OpenLobby("other host");
    (void)other;
    relay.Clear();
    relay.Send("other host", data);
    CHECK(relay.To("host").empty());
    CHECK(relay.To("guest").empty());
}

TEST_CASE("Going quiet drops you, and the rest are told", "[relay]")
{
    RelayServer::Settings settings;
    settings.timeoutSeconds = 5.0f;
    RelayHarness relay(settings);
    const uint32_t code = relay.OpenLobby("host");
    relay.JoinLobby("guest", code);
    relay.Clear();

    // The host keeps talking; the guest does not.
    RelayPacket alive;
    alive.kind = RelayMessage::KeepAlive;
    for (int i = 0; i < 12; ++i)
    {
        relay.Send("host", alive);
        relay.Tick(1.0f);
    }

    RelayPacket left;
    REQUIRE(relay.First("host", RelayMessage::PeerLeft, left));
    CHECK(left.slot == 1);
    CHECK(relay.server.ClientCount() == 1);
    CHECK(relay.server.LobbyCount() == 1);
}

TEST_CASE("The lobby closes when the host goes", "[relay]")
{
    // The others cannot play without a host, and leaving the code alive would let somebody join a
    // game that is not there.
    RelayHarness relay;
    const uint32_t code = relay.OpenLobby("host");
    relay.JoinLobby("guest", code);
    relay.Clear();

    RelayPacket leave;
    leave.kind = RelayMessage::Leave;
    relay.Send("host", leave);

    RelayPacket told;
    REQUIRE(relay.First("guest", RelayMessage::Rejected, told));
    CHECK(told.reason == RelayRejection::NoSuchLobby);
    CHECK(relay.server.LobbyCount() == 0);
}

TEST_CASE("A flood from one address is cut off", "[relay][security]")
{
    // An open UDP port is reachable by everybody. One address must not be able to make the relay do
    // unbounded work, and a relayed game is about a hundred and fifty datagrams a second per player.
    RelayServer::Settings settings;
    settings.messagesPerSecond = 100.0f;
    RelayHarness relay(settings);
    const uint32_t code = relay.OpenLobby("host");
    relay.JoinLobby("flooder", code);
    relay.Clear();

    RelayPacket data;
    data.kind = RelayMessage::Data;
    data.slot = kRelayHostSlot;
    data.payload = {1, 2, 3, 4};
    for (int i = 0; i < 1000; ++i)
    {
        relay.Send("flooder", data);
    }

    const size_t delivered = relay.To("host").size();
    INFO("relayed " << delivered << " of 1000 flooded messages");
    // The allowance it started with and no more, rather than all thousand.
    CHECK(delivered <= 110);
    CHECK(delivered > 0);

    // And it recovers: a second of quiet refills the bucket.
    relay.Clear();
    relay.Tick(1.0f);
    relay.Send("flooder", data);
    CHECK(relay.To("host").size() == 1);
}

TEST_CASE("Nonsense on the port is ignored rather than answered", "[relay][security]")
{
    // Answering a scanner tells it something is here. Anything that is not ours gets nothing at all.
    RelayHarness relay;
    const char* noise = "GET / HTTP/1.1\r\nHost: example\r\n\r\n";
    relay.server.Receive("scanner", reinterpret_cast<const uint8_t*>(noise), std::strlen(noise),
                         relay.out);
    CHECK(relay.out.empty());

    const std::vector<uint8_t> empty;
    relay.server.Receive("scanner", empty.data(), 0, relay.out);
    CHECK(relay.out.empty());
    CHECK(relay.server.LobbyCount() == 0);
}
