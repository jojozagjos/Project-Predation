#include "Engine/Net/RelayCarrier.h"
#include "Engine/Net/RelayProtocol.h"
#include "Engine/Net/RelayServer.h"

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <mstcpip.h>
#    ifndef SIO_UDP_CONNRESET
#        define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#    endif
#else
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
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


// A UDP socket and the loop the relay executable puts around RelayServer, small enough to live in a
// test. The executable is the same thing with a signal handler and an argument parser.
struct RelaySocket
{
#if defined(_WIN32)
    SOCKET handle = INVALID_SOCKET;
#else
    int handle = -1;
#endif
    std::unordered_map<std::string, sockaddr_in> addresses;
    std::vector<RelayServer::Outgoing> outgoing;

    ~RelaySocket() { Close(); }

    bool Open(uint16_t port)
    {
#if defined(_WIN32)
        WSADATA winsock{};
        WSAStartup(MAKEWORD(2, 2), &winsock);
#endif
        handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
        if (handle == INVALID_SOCKET)
#else
        if (handle < 0)
#endif
        {
            return false;
        }
        sockaddr_in bound{};
        bound.sin_family = AF_INET;
        bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bound.sin_port = htons(port);
        if (bind(handle, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound)) != 0)
        {
            return false;
        }
#if defined(_WIN32)
        u_long nonBlocking = 1;
        ioctlsocket(handle, static_cast<long>(FIONBIO), &nonBlocking);
        DWORD behaviour = 0;
        DWORD returned = 0;
        WSAIoctl(handle, SIO_UDP_CONNRESET, &behaviour, sizeof(behaviour), nullptr, 0, &returned,
                 nullptr, nullptr);
#else
        const int flags = fcntl(handle, F_GETFL, 0);
        fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif
        return true;
    }

    uint16_t Port() const
    {
        sockaddr_in bound{};
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(bound));
#else
        socklen_t length = sizeof(bound);
#endif
        if (getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0)
        {
            return 0;
        }
        return ntohs(bound.sin_port);
    }

    void Pump(RelayServer& relay, float dt)
    {
        std::array<uint8_t, 1400> buffer{};
        for (int guard = 0; guard < 256; ++guard)
        {
            sockaddr_in from{};
#if defined(_WIN32)
            int fromLength = static_cast<int>(sizeof(from));
#else
            socklen_t fromLength = sizeof(from);
#endif
            const int received =
                recvfrom(handle, reinterpret_cast<char*>(buffer.data()),
                         static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                         &fromLength);
            if (received <= 0)
            {
                break;
            }
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
            std::string key = std::string(text) + ":" + std::to_string(ntohs(from.sin_port));
            addresses[key] = from;
            relay.Receive(key, buffer.data(), static_cast<size_t>(received), outgoing);
        }
        relay.Tick(dt, outgoing);
        for (const RelayServer::Outgoing& entry : outgoing)
        {
            const auto found = addresses.find(entry.to);
            if (found == addresses.end())
            {
                continue;
            }
            sendto(handle, reinterpret_cast<const char*>(entry.datagram.data()),
                   static_cast<int>(entry.datagram.size()), 0,
                   reinterpret_cast<const sockaddr*>(&found->second), sizeof(found->second));
        }
        outgoing.clear();
    }

    void Close()
    {
#if defined(_WIN32)
        if (handle != INVALID_SOCKET)
        {
            closesocket(handle);
            handle = INVALID_SOCKET;
            WSACleanup();
        }
#else
        if (handle >= 0)
        {
            close(handle);
            handle = -1;
        }
#endif
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

TEST_CASE("The relay says out loud what happened to it", "[relay][diagnostics]")
{
    // The relay's only output used to be a line printed when the number of lobbies changed. A second
    // player joining an existing lobby does not change that number, so the one event anybody runs
    // this program to watch for was the one event it never mentioned -- and "my friend never reached
    // the relay" and "my friend reached it and the game link failed" produced identical consoles.
    //
    // This is that hole, as a test: every event that distinguishes those two cases has to appear.
    using Kind = RelayServer::Note::Kind;
    const auto kindsOf = [](const std::vector<RelayServer::Note>& notes)
    {
        std::vector<Kind> kinds;
        for (const RelayServer::Note& note : notes)
        {
            kinds.push_back(note.kind);
        }
        return kinds;
    };
    const auto has = [&](const std::vector<RelayServer::Note>& notes, Kind kind)
    {
        const std::vector<Kind> kinds = kindsOf(notes);
        return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
    };

    RelayHarness relay;
    const uint32_t code = relay.OpenLobby("host");
    REQUIRE(has(relay.server.Notes(), Kind::Opened));
    CHECK(relay.server.Notes().front().code == code);
    CHECK(relay.server.Notes().front().client == "host");
    relay.server.Notes().clear();

    // The one that used to be silent.
    relay.JoinLobby("guest", code);
    REQUIRE(has(relay.server.Notes(), Kind::Joined));
    CHECK(relay.server.Notes().front().client == "guest");
    CHECK(relay.server.Notes().front().slot == 1);
    relay.server.Notes().clear();

    // A wrong code, which is what a mistyped invitation looks like from here.
    RelayPacket wrong;
    wrong.kind = RelayMessage::Join;
    wrong.code = code ^ 0x5A5Au;
    relay.Send("stranger", wrong);
    REQUIRE(has(relay.server.Notes(), Kind::Refused));
    CHECK(relay.server.Notes().front().reason == RelayRejection::NoSuchLobby);
    relay.server.Notes().clear();

    // And traffic that is not this game at all, which is a different fault from no traffic.
    const uint8_t rubbish[] = {0xDE, 0xAD, 0xBE, 0xEF};
    relay.server.Receive("scanner", rubbish, sizeof(rubbish), relay.out);
    REQUIRE(has(relay.server.Notes(), Kind::Ignored));
    CHECK(relay.server.Notes().front().client == "scanner");
    relay.server.Notes().clear();

    // Silence drops the guest, and says that is why rather than that they left.
    std::vector<RelayServer::Outgoing> out;
    relay.server.Tick(RelayServer::Settings{}.timeoutSeconds + 1.0f, out);
    REQUIRE(has(relay.server.Notes(), Kind::Left));
    bool sawTimeout = false;
    for (const RelayServer::Note& note : relay.server.Notes())
    {
        sawTimeout = sawTimeout || (note.kind == Kind::Left && note.timedOut);
    }
    CHECK(sawTimeout);
    CHECK(has(relay.server.Notes(), Kind::Closed));
}

TEST_CASE("Notes do not pile up when nobody is reading them", "[relay][diagnostics]")
{
    // A relay left running for a week with no operator watching must not grow a note per datagram.
    RelayHarness relay;
    const uint8_t rubbish[] = {0xDE, 0xAD};
    for (size_t i = 0; i < RelayServer::kMaxNotes * 3; ++i)
    {
        relay.server.Receive("scanner", rubbish, sizeof(rubbish), relay.out);
    }
    CHECK(relay.server.Notes().size() == RelayServer::kMaxNotes);
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

// --- Over a real socket --------------------------------------------------------------------------

TEST_CASE("Two carriers meet through a relay on a real socket", "[relay][carrier][socket]")
{
    // Everything above is the relay's logic without a network. This is the other half: two real
    // sockets, a real relay loop, a lobby opened and joined by code, and the game's own transport
    // running over the top of it without knowing any of that has happened.
    //
    // Loopback, so it proves the plumbing rather than the traversal. Whether two routers cooperate
    // is exactly the question a relay removes: there is nothing to traverse, only an outbound
    // connection, and if that fails nothing else would have worked either.
    RelayServer::Settings settings;
    settings.seed = 12345u;
    RelayServer relay(settings);

    // The relay loop, driven by hand so the test owns the clock.
    RelaySocket socket;
    REQUIRE(socket.Open(0));
    const uint16_t port = socket.Port();
    INFO("relay on port " << port);

    RelayCarrier::Settings carrierSettings;
    carrierSettings.relayHost = "127.0.0.1";
    carrierSettings.relayPort = port;
    carrierSettings.connectTimeoutSeconds = 5.0f;

    RelayCarrier host;
    REQUIRE(host.Host(carrierSettings));

    const auto pump = [&](int milliseconds)
    {
        for (int i = 0; i < milliseconds; ++i)
        {
            socket.Pump(relay, 0.001f);
            host.Poll(0.001f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // The relay answers with a code.
    for (int i = 0; i < 200 && host.Status() == RelayCarrier::State::Connecting; ++i)
    {
        pump(5);
    }
    REQUIRE(host.Status() == RelayCarrier::State::Ready);
    const std::string code = host.CodeText();
    INFO("lobby code " << code);
    REQUIRE(code.size() == static_cast<size_t>(kRelayCodeLength));

    // Somebody joins with it, typed the way a person would type it.
    uint32_t typed = 0;
    REQUIRE(EncodeRelayCode(code, typed));
    RelayCarrier guest;
    REQUIRE(guest.Join(carrierSettings, typed));

    const auto pumpBoth = [&](int milliseconds)
    {
        for (int i = 0; i < milliseconds; ++i)
        {
            socket.Pump(relay, 0.001f);
            host.Poll(0.001f);
            guest.Poll(0.001f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    for (int i = 0; i < 200 && guest.Status() == RelayCarrier::State::Connecting; ++i)
    {
        pumpBoth(5);
    }
    REQUIRE(guest.Status() == RelayCarrier::State::Ready);

    // Both now know about each other, so both have a link to send on.
    for (int i = 0; i < 100 && (host.Links() == 0 || guest.Links() == 0); ++i)
    {
        pumpBoth(5);
    }
    REQUIRE(host.Links() >= 1);
    REQUIRE(guest.Links() >= 1);

    // And the bytes go across.
    const std::string hello = "the door is open";
    REQUIRE(host.Send(0, reinterpret_cast<const uint8_t*>(hello.data()), hello.size()));
    std::vector<uint8_t> heard;
    size_t fromLink = 0;
    bool arrived = false;
    for (int i = 0; i < 200 && !arrived; ++i)
    {
        pumpBoth(5);
        arrived = guest.Receive(fromLink, heard);
    }
    REQUIRE(arrived);
    CHECK(std::string(heard.begin(), heard.end()) == hello);

    // Back the other way, which is the direction a router would have refused.
    const std::string reply = "coming through";
    REQUIRE(guest.Send(fromLink, reinterpret_cast<const uint8_t*>(reply.data()), reply.size()));
    arrived = false;
    for (int i = 0; i < 200 && !arrived; ++i)
    {
        pumpBoth(5);
        arrived = host.Receive(fromLink, heard);
    }
    REQUIRE(arrived);
    CHECK(std::string(heard.begin(), heard.end()) == reply);
}
