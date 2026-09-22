#include "Engine/Net/LobbyClient.h"
#include "Engine/Net/LobbyProtocol.h"
#include "Engine/Net/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using namespace pred;

// How two games find each other: codes, the probes they punch through with, the STUN answer that
// says how a socket looks from outside, and the whole thing end to end over real sockets on this PC.
// The lobby server itself is JavaScript (Tools/LobbyWorker) and is tested there; here a stand-in with
// the same answers plays its part, so these tests need nothing but this machine.

TEST_CASE("A lobby code is six characters nobody can misread, and an address is not one", "[lobby]")
{
    uint32_t value = 0;
    REQUIRE(EncodeLobbyCode("K7X2QM", value));
    CHECK(DecodeLobbyCode(value) == "K7X2QM");
    uint32_t again = 0;
    REQUIRE(EncodeLobbyCode(" k7x-2qm\n", again));
    CHECK(again == value);
    CHECK_FALSE(EncodeLobbyCode("K7X2Q0", value));
    CHECK_FALSE(EncodeLobbyCode("K7X2QI", value));
    CHECK_FALSE(EncodeLobbyCode("K7X2Q", value));
    CHECK_FALSE(EncodeLobbyCode("192.168.1.20", value));
    CHECK_FALSE(EncodeLobbyCode("10.1.5.7:27015", value));
}

TEST_CASE("A probe survives the wire, and nothing else passes for one", "[lobby]")
{
    LobbyPacket probe;
    probe.kind = LobbyMessage::ProbeReply;
    probe.token = 0xDEADBEEFu;
    const std::vector<uint8_t> bytes = EncodeLobby(probe);
    LobbyPacket back;
    REQUIRE(DecodeLobby(bytes.data(), bytes.size(), back));
    CHECK(back.kind == LobbyMessage::ProbeReply);
    CHECK(back.token == 0xDEADBEEFu);
    for (size_t cut = 0; cut < bytes.size(); ++cut)
    {
        CHECK_FALSE(DecodeLobby(bytes.data(), cut, back));
    }
    std::vector<uint8_t> wrongKind = bytes;
    wrongKind[4] = 7;
    CHECK_FALSE(DecodeLobby(wrongKind.data(), wrongKind.size(), back));

    LobbyEndpoint endpoint;
    REQUIRE(LobbyEndpoint::ParseWithPort("81.2.3.4:27015", endpoint));
    CHECK(endpoint.ToString() == "81.2.3.4:27015");
    CHECK_FALSE(LobbyEndpoint::ParseWithPort("81.2.3.4", endpoint));
    CHECK_FALSE(LobbyEndpoint::ParseWithPort("81.2.3.400:5", endpoint));
}

TEST_CASE("A STUN answer is read the way real servers write it", "[lobby][stun]")
{
    // The IPv4 response from RFC 5769, section 2.2: a software name, the mapped address, and then an
    // integrity check and a fingerprint this game has no use for and must step over.
    const std::vector<uint8_t> response = {
        0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
        0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63,
        0x74, 0x6f, 0x72, 0x20, 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
        0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9,
        0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};
    const StunTransaction transaction{0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
    REQUIRE(IsStunDatagram(response.data(), response.size()));
    LobbyEndpoint mapped;
    REQUIRE(DecodeStunResponse(response.data(), response.size(), transaction, mapped));
    CHECK(mapped.ToString() == "192.0.2.1:32853");

    // Somebody else's answer is not ours.
    StunTransaction other = transaction;
    other[0] ^= 1;
    CHECK_FALSE(DecodeStunResponse(response.data(), response.size(), other, mapped));

    // And our own request and answer go round.
    const std::vector<uint8_t> request = EncodeStunRequest(transaction);
    StunTransaction heard{};
    REQUIRE(DecodeStunRequest(request.data(), request.size(), heard));
    CHECK(heard == transaction);
    const std::vector<uint8_t> answer = EncodeStunResponse(transaction, LobbyEndpoint{0x51020304u, 40001});
    REQUIRE(DecodeStunResponse(answer.data(), answer.size(), transaction, mapped));
    CHECK(mapped.ToString() == "81.2.3.4:40001");
    // A game datagram or a probe is not STUN.
    const std::vector<uint8_t> probe = EncodeLobby(LobbyPacket{});
    CHECK_FALSE(IsStunDatagram(probe.data(), probe.size()));
}

namespace
{

std::future<HttpResult> Ready(int status, const nlohmann::json& body)
{
    std::promise<HttpResult> promise;
    promise.set_value(HttpResult{status, body.dump(), {}});
    return promise.get_future();
}

// The lobby server's answers, as far as these tests need them. The real one is Tools/LobbyWorker.
struct FakeLobbyServer
{
    struct Lobby
    {
        std::string secret;
        std::string name;
        nlohmann::json candidates;
        std::vector<nlohmann::json> guests; // waiting to be handed to the host
        int version = 0;
    };
    std::map<std::string, Lobby> lobbies;
    bool silent = false; // no answers at all, as a server that is down
    int requests = 0;

    LobbyClient::Requester Requester()
    {
        return [this](const std::string& method, const std::string& url, const std::string& text)
        {
            ++requests;
            if (silent)
            {
                std::promise<HttpResult> promise;
                promise.set_value(HttpResult{0, {}, "could not connect to the server"});
                return promise.get_future();
            }
            const std::string path = url.substr(url.find('/', url.find("://") + 3));
            const nlohmann::json body = text.empty() ? nlohmann::json::object() : nlohmann::json::parse(text);
            if (method == "POST" && path == "/host")
            {
                const std::string code = "K7X2QM";
                lobbies[code] = Lobby{"s3cret", body.value("name", ""), body["candidates"], {}, body.value("version", 0)};
                return Ready(200, {{"code", code}, {"secret", "s3cret"}});
            }
            if (method == "POST" && path == "/update")
            {
                const auto found = lobbies.find(body.value("code", ""));
                if (found == lobbies.end())
                {
                    return Ready(404, {{"error", "no-such-lobby"}});
                }
                found->second.candidates = body["candidates"];
                nlohmann::json guests = nlohmann::json::array();
                for (const nlohmann::json& guest : found->second.guests)
                {
                    guests.push_back(guest);
                }
                found->second.guests.clear();
                return Ready(200, {{"guests", guests}});
            }
            if (method == "POST" && path == "/join")
            {
                const auto found = lobbies.find(body.value("code", ""));
                if (found == lobbies.end())
                {
                    return Ready(404, {{"error", "no-such-lobby"}});
                }
                if (body.value("version", 0) != found->second.version)
                {
                    return Ready(409, {{"error", "wrong-version"}});
                }
                found->second.guests.push_back({{"token", "a1b2c3d4"}, {"candidates", body["candidates"]}});
                return Ready(200, {{"token", "a1b2c3d4"},
                                   {"name", found->second.name},
                                   {"started", false},
                                   {"candidates", found->second.candidates}});
            }
            if (method == "POST" && path == "/close")
            {
                lobbies.erase(body.value("code", ""));
                return Ready(200, nlohmann::json::object());
            }
            if (method == "GET" && path.rfind("/list", 0) == 0)
            {
                nlohmann::json rows = nlohmann::json::array();
                for (const auto& [code, lobby] : lobbies)
                {
                    rows.push_back({{"code", code}, {"name", lobby.name}, {"players", 1}, {"maxPlayers", 4}, {"started", false}});
                }
                return Ready(200, {{"lobbies", rows}});
            }
            return Ready(404, {{"error", "unknown-request"}});
        };
    }
};

// A STUN server on this PC: answers each binding request with the address it came from, or, when
// `skew` is set, with a different port -- which is what a strict router looks like from outside.
struct LocalStun
{
    std::unique_ptr<Transport> socket = CreateUdpTransport(9u);
    uint16_t skew = 0;
    LocalStun() { REQUIRE(socket->Open(0)); }
    std::string Address() const { return "127.0.0.1:" + std::to_string(socket->LocalPort()); }
    void Answer()
    {
        std::vector<NetPacket> none;
        socket->Poll(0.0f, none);
        for (const Transport::UnframedDatagram& datagram : socket->TakeUnframed())
        {
            StunTransaction transaction{};
            LobbyEndpoint from;
            if (DecodeStunRequest(datagram.bytes.data(), datagram.bytes.size(), transaction) &&
                LobbyEndpoint::Parse(datagram.address, static_cast<uint16_t>(datagram.port + skew), from))
            {
                const std::vector<uint8_t> answer = EncodeStunResponse(transaction, from);
                socket->SendUnframed(datagram.address, datagram.port, answer.data(), answer.size());
            }
        }
    }
};

struct Game
{
    Transport* transport = nullptr;
    LobbyClient* lobby = nullptr;
};

void Step(std::vector<Game> games, std::vector<LocalStun*> stuns = {})
{
    std::vector<NetPacket> packets;
    for (Game& game : games)
    {
        if (game.transport != nullptr)
        {
            game.transport->Poll(1.0f / 60.0f, packets);
        }
    }
    for (LocalStun* stun : stuns)
    {
        stun->Answer();
    }
    for (Game& game : games)
    {
        game.lobby->Poll(1.0f / 60.0f, game.transport);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

LobbyClient::Settings SettingsFor(FakeLobbyServer& server, const std::vector<std::string>& stun)
{
    LobbyClient::Settings settings;
    settings.server = "https://lobby.test";
    settings.stunServers = stun;
    settings.version = 7;
    settings.request = server.Requester();
    return settings;
}

} // namespace

TEST_CASE("Two games find each other by code and connect directly", "[lobby][udp]")
{
    FakeLobbyServer server;
    LocalStun stun;

    std::unique_ptr<Transport> host = CreateUdpTransport(1u);
    REQUIRE(host->Listen(47931));
    LobbyClient hostLobby;
    REQUIRE(hostLobby.Host(SettingsFor(server, {stun.Address()}), "kitchen", false, 4, 47931));
    for (int i = 0; i < 300 && hostLobby.Status() != LobbyClient::State::Open; ++i)
    {
        Step({{host.get(), &hostLobby}}, {&stun});
    }
    REQUIRE(hostLobby.Status() == LobbyClient::State::Open);
    CHECK(hostLobby.CodeText() == "K7X2QM");
    // STUN said what this socket looks like from outside -- here, from this same PC.
    CHECK(hostLobby.SeenAs().ToString() == "127.0.0.1:47931");
    CHECK_FALSE(hostLobby.StrictRouter());

    // The guest opens a socket, asks for the code, and punches through to the host over it.
    std::unique_ptr<Transport> guest = CreateUdpTransport(2u);
    REQUIRE(guest->Open(0));
    LobbyClient guestLobby;
    REQUIRE(guestLobby.Join(SettingsFor(server, {stun.Address()}), hostLobby.Code(), guest->LocalPort()));
    for (int i = 0; i < 900 && guestLobby.Status() != LobbyClient::State::Reached &&
                    guestLobby.Status() != LobbyClient::State::Failed;
         ++i)
    {
        Step({{host.get(), &hostLobby}, {guest.get(), &guestLobby}}, {&stun});
    }
    INFO(guestLobby.Message() << " requests " << server.requests << " host state " << static_cast<int>(hostLobby.Status()));
    REQUIRE(guestLobby.Status() == LobbyClient::State::Reached);
    CHECK(guestLobby.LobbyName() == "kitchen");
    CHECK(guestLobby.Reached().port == 47931);

    // And then an ordinary connection, over the same socket the lobby used.
    REQUIRE(guest->Connect(guestLobby.Reached().AddressText(), guestLobby.Reached().port));
    bool joined = false;
    for (int i = 0; i < 120 && !joined; ++i)
    {
        Step({{host.get(), &hostLobby}, {guest.get(), &guestLobby}}, {&stun});
        joined = !host->TakeConnected().empty();
    }
    CHECK(joined);
    CHECK_FALSE(guest->TakeConnected().empty());

    // Closing the lobby frees the code.
    hostLobby.Close();
    for (int i = 0; i < 50 && !server.lobbies.empty(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.lobbies.empty());
}

TEST_CASE("A router that changes its port for everybody is recognised", "[lobby][stun]")
{
    // Two STUN servers that see the same socket on different outside ports: a hole made for one is no
    // use to anybody else, and the host is warned before a friend finds out the hard way.
    FakeLobbyServer server;
    LocalStun first;
    LocalStun second;
    second.skew = 7;
    std::unique_ptr<Transport> host = CreateUdpTransport(3u);
    REQUIRE(host->Listen(47932));
    LobbyClient lobby;
    REQUIRE(lobby.Host(SettingsFor(server, {first.Address(), second.Address()}), "strict", false, 4, 47932));
    for (int i = 0; i < 300 && lobby.Status() != LobbyClient::State::Open; ++i)
    {
        Step({{host.get(), &lobby}}, {&first, &second});
    }
    REQUIRE(lobby.Status() == LobbyClient::State::Open);
    CHECK(lobby.StrictRouter());
}

TEST_CASE("Wrong codes, other versions and a missing server are all said in words", "[lobby]")
{
    FakeLobbyServer server;
    std::unique_ptr<Transport> socket = CreateUdpTransport(4u);
    REQUIRE(socket->Open(0));

    const auto failWith = [&](LobbyClient::Settings settings, uint32_t code)
    {
        settings.answerSeconds = 1.0f;
        LobbyClient lobby;
        REQUIRE(lobby.Join(settings, code, socket->LocalPort()));
        for (int i = 0; i < 240 && lobby.Status() != LobbyClient::State::Failed; ++i)
        {
            Step({{socket.get(), &lobby}});
        }
        REQUIRE(lobby.Status() == LobbyClient::State::Failed);
        return lobby.Message();
    };

    uint32_t code = 0;
    REQUIRE(EncodeLobbyCode("ZZZZZZ", code));
    CHECK(failWith(SettingsFor(server, {}), code).find("no game with that code") != std::string::npos);

    server.lobbies["ZZZZZZ"] = FakeLobbyServer::Lobby{"x", "old", nlohmann::json::array(), {}, 6};
    CHECK(failWith(SettingsFor(server, {}), code).find("different version") != std::string::npos);

    server.silent = true;
    CHECK(failWith(SettingsFor(server, {}), code).find("not answering") != std::string::npos);
}

TEST_CASE("The public list comes from the lobby server, and a host it forgot opens again", "[lobby]")
{
    FakeLobbyServer server;
    std::unique_ptr<Transport> host = CreateUdpTransport(5u);
    REQUIRE(host->Listen(47933));
    LobbyClient lobby;
    REQUIRE(lobby.Host(SettingsFor(server, {}), "kitchen", true, 4, 47933));
    for (int i = 0; i < 300 && lobby.Status() != LobbyClient::State::Open; ++i)
    {
        Step({{host.get(), &lobby}});
    }
    REQUIRE(lobby.Status() == LobbyClient::State::Open);

    LobbyClient browser;
    REQUIRE(browser.Browse(SettingsFor(server, {})));
    for (int i = 0; i < 30 && !browser.Heard(); ++i)
    {
        Step({{nullptr, &browser}});
    }
    REQUIRE(browser.Heard());
    REQUIRE(browser.Lobbies().size() == 1);
    CHECK(browser.Lobbies()[0].name == "kitchen");

    // The server restarts and forgets everything. The host is told on its next update and opens the
    // same code again.
    server.lobbies.clear();
    for (int i = 0; i < 600 && server.lobbies.empty(); ++i)
    {
        Step({{host.get(), &lobby}});
    }
    CHECK(server.lobbies.count("K7X2QM") == 1);
    CHECK(lobby.CodeText() == "K7X2QM");
}

// Against the game's real lobby server on Cloudflare and the real public STUN servers, so it needs the
// internet and is hidden by default ("[.]"). Run it by its tag: PredationTests "[live]".
TEST_CASE("Two games find each other through the real lobby server", "[.][live]")
{
    LobbyClient::Settings settings;
    settings.server = "https://project-predation.josephgslade.workers.dev";
    settings.version = 7;

    std::unique_ptr<Transport> host = CreateUdpTransport(11u);
    REQUIRE(host->Listen(47941));
    LobbyClient hostLobby;
    REQUIRE(hostLobby.Host(settings, "live test", false, 4, 47941));
    const auto step = [&](std::vector<Game> games)
    {
        std::vector<NetPacket> packets;
        for (Game& game : games)
        {
            if (game.transport != nullptr)
            {
                game.transport->Poll(1.0f / 60.0f, packets);
            }
            game.lobby->Poll(1.0f / 60.0f, game.transport);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    };
    for (int i = 0; i < 900 && hostLobby.Status() != LobbyClient::State::Open &&
                    hostLobby.Status() != LobbyClient::State::Failed;
         ++i)
    {
        step({{host.get(), &hostLobby}});
    }
    INFO(hostLobby.Message());
    REQUIRE(hostLobby.Status() == LobbyClient::State::Open);
    INFO("code " << hostLobby.CodeText() << ", seen from outside as " << hostLobby.SeenAs().ToString());
    CHECK(hostLobby.SeenAs().Valid());

    std::unique_ptr<Transport> guest = CreateUdpTransport(12u);
    REQUIRE(guest->Open(0));
    LobbyClient guestLobby;
    REQUIRE(guestLobby.Join(settings, hostLobby.Code(), guest->LocalPort()));
    for (int i = 0; i < 1200 && guestLobby.Status() != LobbyClient::State::Reached &&
                    guestLobby.Status() != LobbyClient::State::Failed;
         ++i)
    {
        step({{host.get(), &hostLobby}, {guest.get(), &guestLobby}});
    }
    INFO(guestLobby.Message());
    REQUIRE(guestLobby.Status() == LobbyClient::State::Reached);
    CHECK(guestLobby.LobbyName() == "live test");
    hostLobby.Close();
}
