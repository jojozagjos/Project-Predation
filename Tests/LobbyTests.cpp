#include "Engine/Net/LobbyClient.h"
#include "Engine/Net/LobbyDirectory.h"
#include "Engine/Net/LobbyProtocol.h"
#include "Engine/Net/LobbyServer.h"
#include "Engine/Net/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace pred;

// The lobby server and the way two games find each other through it. The directory is checked
// without any network; the last tests run a real server and two real games on 127.0.0.1.

namespace
{

LobbyEndpoint At(uint32_t address, uint16_t port)
{
    return LobbyEndpoint{address, port};
}

const LobbyEndpoint kHostOutside = At(0x51020304u, 40001);  // 81.2.3.4
const LobbyEndpoint kGuestOutside = At(0x5D0A0B0Cu, 51000); // 93.10.11.12
const LobbyEndpoint kHostInside = At(0xC0A80114u, 27015);   // 192.168.1.20

std::vector<LobbyDirectory::Reply> Send(LobbyDirectory& directory, const LobbyPacket& packet,
                                        const LobbyEndpoint& from, double now)
{
    const std::vector<uint8_t> bytes = EncodeLobby(packet);
    std::vector<LobbyDirectory::Reply> out;
    directory.Handle(bytes.data(), bytes.size(), from, now, out);
    return out;
}

LobbyPacket Decoded(const LobbyDirectory::Reply& reply)
{
    LobbyPacket packet;
    REQUIRE(DecodeLobby(reply.bytes.data(), reply.bytes.size(), packet));
    return packet;
}

LobbyPacket HostPacket(uint16_t version = 7, bool listed = false)
{
    LobbyPacket packet;
    packet.kind = LobbyMessage::Host;
    packet.version = version;
    packet.players = 1;
    packet.maxPlayers = 4;
    packet.listed = listed;
    packet.name = "kitchen";
    packet.candidates = {kHostInside};
    return packet;
}

LobbyPacket JoinPacket(uint32_t code, uint16_t version = 7)
{
    LobbyPacket packet;
    packet.kind = LobbyMessage::Join;
    packet.version = version;
    packet.code = code;
    packet.candidates = {At(0x0A000005u, 51000)};
    return packet;
}

// Opens a lobby and returns what the host was told.
LobbyPacket Open(LobbyDirectory& directory, double now = 0.0, bool listed = false,
                 const LobbyEndpoint& from = kHostOutside)
{
    const auto replies = Send(directory, HostPacket(7, listed), from, now);
    REQUIRE(replies.size() == 1);
    const LobbyPacket hosted = Decoded(replies[0]);
    REQUIRE(hosted.kind == LobbyMessage::Hosted);
    return hosted;
}

} // namespace

TEST_CASE("A lobby code is six characters nobody can misread, and an address is not one", "[lobby]")
{
    uint32_t value = 0;
    REQUIRE(EncodeLobbyCode("K7X2QM", value));
    CHECK(DecodeLobbyCode(value) == "K7X2QM");
    uint32_t again = 0;
    // However it was typed.
    REQUIRE(EncodeLobbyCode(" k7x-2qm\n", again));
    CHECK(again == value);
    // Nought, one, I and O are not in the alphabet; addresses and the wrong length are not codes.
    CHECK_FALSE(EncodeLobbyCode("K7X2Q0", value));
    CHECK_FALSE(EncodeLobbyCode("K7X2QI", value));
    CHECK_FALSE(EncodeLobbyCode("K7X2Q", value));
    CHECK_FALSE(EncodeLobbyCode("192.168.1.20", value));
    CHECK_FALSE(EncodeLobbyCode("10.1.5.7:27015", value));
}

TEST_CASE("Every lobby message survives the wire, and damaged ones are refused", "[lobby]")
{
    LobbyPacket introduce;
    introduce.kind = LobbyMessage::Introduce;
    introduce.code = 0x1234567u;
    introduce.token = 0xDEADBEEFu;
    introduce.toHost = true;
    introduce.name = "kitchen pc";
    introduce.seenAs = kHostOutside;
    introduce.candidates = {kGuestOutside, kHostInside};
    const std::vector<uint8_t> bytes = EncodeLobby(introduce);

    LobbyPacket back;
    REQUIRE(DecodeLobby(bytes.data(), bytes.size(), back));
    CHECK(back.kind == LobbyMessage::Introduce);
    CHECK(back.code == introduce.code);
    CHECK(back.token == introduce.token);
    CHECK(back.toHost);
    CHECK(back.name == "kitchen pc");
    CHECK(back.seenAs == kHostOutside);
    REQUIRE(back.candidates.size() == 2);
    CHECK(back.candidates[0] == kGuestOutside);

    // Short, long, or somebody else's.
    for (size_t cut = 0; cut < bytes.size(); ++cut)
    {
        CHECK_FALSE(DecodeLobby(bytes.data(), cut, back));
    }
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    CHECK_FALSE(DecodeLobby(longer.data(), longer.size(), back));
    std::vector<uint8_t> stranger = bytes;
    stranger[0] ^= 0xFF;
    CHECK_FALSE(DecodeLobby(stranger.data(), stranger.size(), back));

    // Names are clamped and made printable.
    LobbyPacket host = HostPacket();
    host.name = std::string(80, 'x') + "\x01";
    const std::vector<uint8_t> hostBytes = EncodeLobby(host);
    REQUIRE(DecodeLobby(hostBytes.data(), hostBytes.size(), back));
    CHECK(back.name.size() == kLobbyMaxNameLength);
}

TEST_CASE("The server hands a host a code, and the same one if it asks again", "[lobby]")
{
    LobbyDirectory directory;
    const LobbyPacket first = Open(directory);
    CHECK(first.code != 0);
    CHECK(first.seenAs == kHostOutside);
    CHECK(DecodeLobbyCode(first.code).size() == 6);
    // The answer was lost and it asks again: one lobby, one code.
    const LobbyPacket second = Open(directory, 1.0);
    CHECK(second.code == first.code);
    CHECK(second.secret == first.secret);
    CHECK(directory.LobbyCount() == 1);
}

TEST_CASE("Joining introduces the guest and the host to each other", "[lobby]")
{
    LobbyDirectory directory;
    const LobbyPacket hosted = Open(directory);
    const auto replies = Send(directory, JoinPacket(hosted.code), kGuestOutside, 1.0);
    REQUIRE(replies.size() == 2);

    const LobbyPacket toGuest = Decoded(replies[0]);
    CHECK(replies[0].to == kGuestOutside);
    CHECK(toGuest.kind == LobbyMessage::Introduce);
    CHECK_FALSE(toGuest.toHost);
    CHECK(toGuest.name == "kitchen");
    // The host's outside address first, then its inside one.
    REQUIRE(toGuest.candidates.size() == 2);
    CHECK(toGuest.candidates[0] == kHostOutside);
    CHECK(toGuest.candidates[1] == kHostInside);

    const LobbyPacket toHost = Decoded(replies[1]);
    CHECK(replies[1].to == kHostOutside);
    CHECK(toHost.toHost);
    CHECK(toHost.token == toGuest.token);
    REQUIRE_FALSE(toHost.candidates.empty());
    CHECK(toHost.candidates[0] == kGuestOutside);

    // Asking again is the same introduction.
    const auto again = Send(directory, JoinPacket(hosted.code), kGuestOutside, 2.0);
    REQUIRE(again.size() == 2);
    CHECK(Decoded(again[0]).token == toGuest.token);
}

TEST_CASE("A join is refused, with the reason, when it cannot work", "[lobby]")
{
    LobbyDirectory directory;
    const LobbyPacket hosted = Open(directory);

    const auto reason = [&](const LobbyPacket& join, const LobbyEndpoint& from)
    {
        const auto replies = Send(directory, join, from, 1.0);
        REQUIRE(replies.size() == 1);
        const LobbyPacket answer = Decoded(replies[0]);
        REQUIRE(answer.kind == LobbyMessage::Rejected);
        return answer.reason;
    };
    CHECK(reason(JoinPacket(hosted.code ^ 1u), kGuestOutside) == LobbyRejection::NoSuchLobby);
    CHECK(reason(JoinPacket(hosted.code, 6), kGuestOutside) == LobbyRejection::WrongVersion);

    // Full: four players already.
    LobbyPacket update;
    update.kind = LobbyMessage::Update;
    update.version = 7;
    update.code = hosted.code;
    update.secret = hosted.secret;
    update.players = 4;
    update.maxPlayers = 4;
    Send(directory, update, kHostOutside, 1.0);
    CHECK(reason(JoinPacket(hosted.code), kGuestOutside) == LobbyRejection::LobbyFull);
}

TEST_CASE("Only the host can close its lobby, and a quiet one is forgotten", "[lobby]")
{
    LobbyDirectory directory;
    const LobbyPacket hosted = Open(directory);

    LobbyPacket close;
    close.kind = LobbyMessage::Close;
    close.code = hosted.code;
    close.secret = hosted.secret ^ 1u; // somebody who has only seen the code
    Send(directory, close, kGuestOutside, 1.0);
    CHECK(directory.LobbyCount() == 1);
    close.secret = hosted.secret;
    Send(directory, close, kHostOutside, 1.0);
    CHECK(directory.LobbyCount() == 0);

    // Kept alive by updates, and gone fifteen seconds after they stop.
    const LobbyPacket reopened = Open(directory, 10.0);
    LobbyPacket update;
    update.kind = LobbyMessage::Update;
    update.version = 7;
    update.code = reopened.code;
    update.secret = reopened.secret;
    update.maxPlayers = 4;
    const auto ack = Send(directory, update, kHostOutside, 20.0);
    REQUIRE(ack.size() == 1);
    CHECK(Decoded(ack[0]).kind == LobbyMessage::Hosted);
    directory.Expire(30.0);
    CHECK(directory.LobbyCount() == 1);
    directory.Expire(36.0);
    CHECK(directory.LobbyCount() == 0);

    // And the host is told so on its next update, rather than updating nothing for ever.
    const auto gone = Send(directory, update, kHostOutside, 37.0);
    REQUIRE(gone.size() == 1);
    CHECK(Decoded(gone[0]).reason == LobbyRejection::NoSuchLobby);
}

TEST_CASE("A host the server forgot gets its old code back", "[lobby]")
{
    // A server restart: a new directory with nothing in it, and a host asking for the code it had.
    LobbyDirectory before;
    const uint32_t code = Open(before).code;
    LobbyDirectory after(LobbyDirectory::Settings{4096, 16, 15.0, 20.0, 40.0, 12345u});
    LobbyPacket again = HostPacket();
    again.code = code;
    const auto replies = Send(after, again, kHostOutside, 0.0);
    REQUIRE(replies.size() == 1);
    CHECK(Decoded(replies[0]).code == code);
}

TEST_CASE("The public list shows public lobbies on the same version, and nothing else", "[lobby]")
{
    LobbyDirectory directory;
    Open(directory, 0.0, true, kHostOutside);                    // public
    Open(directory, 0.0, false, At(0x51020305u, 40001));         // by code only
    Send(directory, HostPacket(6, true), At(0x51020306u, 40001), 0.0); // public, older build

    LobbyPacket list;
    list.kind = LobbyMessage::List;
    list.version = 7;
    const auto replies = Send(directory, list, kGuestOutside, 1.0);
    REQUIRE(replies.size() == 1);
    const LobbyPacket listing = Decoded(replies[0]);
    REQUIRE(listing.lobbies.size() == 1);
    CHECK(listing.lobbies[0].name == "kitchen");
    CHECK(listing.lobbies[0].maxPlayers == 4);
}

TEST_CASE("One address cannot flood the server or fill it with lobbies", "[lobby]")
{
    LobbyDirectory directory;
    LobbyPacket list;
    list.kind = LobbyMessage::List;
    list.version = 7;
    int answered = 0;
    for (int i = 0; i < 500; ++i)
    {
        answered += static_cast<int>(Send(directory, list, kGuestOutside, 0.001 * i).size());
    }
    // The burst and a little refill, not five hundred.
    CHECK(answered < 60);
    CHECK(directory.GetStats().limited > 400);

    // Garbage is ignored, and never answered.
    const std::vector<uint8_t> junk(64, 0xAB);
    std::vector<LobbyDirectory::Reply> out;
    directory.Handle(junk.data(), junk.size(), At(0x01020304u, 5), 100.0, out);
    CHECK(out.empty());

    // Sixteen lobbies from one outside address, then no more.
    LobbyDirectory crowded;
    int opened = 0;
    for (uint16_t port = 1; port <= 40; ++port)
    {
        const auto replies = Send(crowded, HostPacket(), At(0x51020304u, port), 100.0 + port);
        opened += !replies.empty() && Decoded(replies[0]).kind == LobbyMessage::Hosted ? 1 : 0;
    }
    CHECK(opened == 16);
}

namespace
{

// A real server and real sockets on this machine, stepped in lockstep.
struct LiveLobby
{
    LobbyServer server;
    double clock = 0.0;

    LiveLobby() { REQUIRE(server.Start(0)); }

    LobbyClient::Settings Settings() const
    {
        LobbyClient::Settings settings;
        settings.server = "127.0.0.1";
        settings.serverPort = server.Port();
        settings.version = 7;
        return settings;
    }

    void Step(std::vector<std::pair<Transport*, LobbyClient*>> games)
    {
        std::vector<NetPacket> packets;
        for (auto& game : games)
        {
            game.first->Poll(1.0f / 60.0f, packets);
        }
        clock += 1.0 / 60.0;
        server.Poll(clock);
        for (auto& game : games)
        {
            game.second->Poll(1.0f / 60.0f, game.first);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};

} // namespace

TEST_CASE("Two games find each other by code and connect directly", "[lobby][udp]")
{
    LiveLobby live;

    std::unique_ptr<Transport> host = CreateUdpTransport(1u);
    REQUIRE(host->Listen(47931));
    LobbyClient hostLobby;
    REQUIRE(hostLobby.Host(live.Settings(), "kitchen", false, 4, 47931));
    for (int i = 0; i < 120 && hostLobby.Status() != LobbyClient::State::Open; ++i)
    {
        live.Step({{host.get(), &hostLobby}});
    }
    REQUIRE(hostLobby.Status() == LobbyClient::State::Open);
    const uint32_t code = hostLobby.Code();
    REQUIRE(code != 0);

    // The guest opens a socket, asks for the code, and punches through to the host over it.
    std::unique_ptr<Transport> guest = CreateUdpTransport(2u);
    REQUIRE(guest->Open(0));
    REQUIRE(guest->LocalPort() != 0);
    LobbyClient guestLobby;
    REQUIRE(guestLobby.Join(live.Settings(), code, guest->LocalPort()));
    for (int i = 0; i < 300 && guestLobby.Status() != LobbyClient::State::Reached &&
                    guestLobby.Status() != LobbyClient::State::Failed;
         ++i)
    {
        live.Step({{host.get(), &hostLobby}, {guest.get(), &guestLobby}});
    }
    INFO(guestLobby.Message());
    REQUIRE(guestLobby.Status() == LobbyClient::State::Reached);
    CHECK(guestLobby.LobbyName() == "kitchen");
    CHECK(guestLobby.Reached().port == 47931);

    // And then an ordinary connection, over the same socket the lobby used.
    REQUIRE(guest->Connect(guestLobby.Reached().AddressText(), guestLobby.Reached().port));
    bool joined = false;
    for (int i = 0; i < 120 && !joined; ++i)
    {
        live.Step({{host.get(), &hostLobby}, {guest.get(), &guestLobby}});
        joined = !host->TakeConnected().empty();
    }
    CHECK(joined);
    CHECK_FALSE(guest->TakeConnected().empty());

    // Closing the lobby frees the code at once.
    hostLobby.Close(host.get());
    for (int i = 0; i < 10; ++i)
    {
        live.Step({{host.get(), &hostLobby}});
    }
    CHECK(live.server.Directory().LobbyCount() == 0);
}

TEST_CASE("A wrong code is refused in words, and a missing server is said to be missing", "[lobby][udp]")
{
    LiveLobby live;
    std::unique_ptr<Transport> guest = CreateUdpTransport(3u);
    REQUIRE(guest->Open(0));
    LobbyClient lobby;
    uint32_t code = 0;
    REQUIRE(EncodeLobbyCode("ZZZZZZ", code));
    REQUIRE(lobby.Join(live.Settings(), code, guest->LocalPort()));
    for (int i = 0; i < 120 && lobby.Status() != LobbyClient::State::Failed; ++i)
    {
        live.Step({{guest.get(), &lobby}});
    }
    REQUIRE(lobby.Status() == LobbyClient::State::Failed);
    CHECK(lobby.Message().find("no game with that code") != std::string::npos);

    // Nothing listening at all: said within the answer time, not left spinning.
    LobbyClient::Settings missing = live.Settings();
    missing.serverPort = static_cast<uint16_t>(live.server.Port() == 47999 ? 47998 : 47999);
    missing.answerSeconds = 1.0f;
    LobbyClient nowhere;
    REQUIRE(nowhere.Join(missing, code, guest->LocalPort()));
    for (int i = 0; i < 90 && nowhere.Status() != LobbyClient::State::Failed; ++i)
    {
        live.Step({{guest.get(), &nowhere}});
    }
    REQUIRE(nowhere.Status() == LobbyClient::State::Failed);
    CHECK(nowhere.Message().find("not answering") != std::string::npos);
}
