#include "Engine/Net/LanDiscovery.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace pred;

namespace
{

LanLobby Beacon(const std::string& name, uint16_t port, uint8_t players = 1, bool started = false)
{
    LanLobby lobby;
    lobby.name = name;
    lobby.port = port;
    lobby.players = players;
    lobby.maxPlayers = 4;
    lobby.started = started;
    lobby.protocol = 4;
    return lobby;
}

bool RoundTrip(const LanLobby& in, LanLobby& out)
{
    const std::vector<uint8_t> bytes = EncodeLanBeacon(in);
    return DecodeLanBeacon(bytes.data(), bytes.size(), out);
}

} // namespace

TEST_CASE("A beacon survives the wire", "[lan]")
{
    LanLobby heard;
    REQUIRE(RoundTrip(Beacon("kitchen pc", 27015, 3, true), heard));
    CHECK(heard.name == "kitchen pc");
    CHECK(heard.port == 27015);
    CHECK(heard.players == 3);
    CHECK(heard.maxPlayers == 4);
    CHECK(heard.started);
    CHECK(heard.protocol == 4);
    // The one field a beacon does not get to fill in.
    CHECK(heard.address.empty());
}

TEST_CASE("A beacon is not believed about anything it should not be", "[lan][security]")
{
    // Anybody on the network can send one of these and it is drawn on everybody's screen, so the
    // decoder is the boundary.
    SECTION("a long name is clamped rather than refusing the whole beacon")
    {
        LanLobby heard;
        REQUIRE(RoundTrip(Beacon(std::string(200, 'x'), 27015), heard));
        CHECK(heard.name.size() == kLanMaxNameLength);
    }

    SECTION("control characters do not reach the screen")
    {
        LanLobby heard;
        REQUIRE(RoundTrip(Beacon(std::string("hi\x01\x1b[2J\x7f", 8), 27015), heard));
        for (const char c : heard.name)
        {
            CHECK(c >= 0x20);
            CHECK(c < 0x7F);
        }
    }

    SECTION("a beacon pointing at no port is refused")
    {
        // The only thing clicking it could do is fail, so it never appears.
        LanLobby heard;
        CHECK_FALSE(RoundTrip(Beacon("nowhere", 0), heard));
    }

    SECTION("somebody else's traffic on the port is ignored")
    {
        const std::string noise = "GET / HTTP/1.1\r\n\r\n";
        LanLobby heard;
        CHECK_FALSE(DecodeLanBeacon(reinterpret_cast<const uint8_t*>(noise.data()), noise.size(),
                                    heard));
        CHECK_FALSE(DecodeLanBeacon(nullptr, 0, heard));
        const uint8_t truncated[4] = {0x50, 0x4C, 0x41, 0x4E};
        CHECK_FALSE(DecodeLanBeacon(truncated, sizeof(truncated), heard));
    }
}

TEST_CASE("The list of games on the network keeps up with them", "[lan]")
{
    std::vector<LanLobby> list;

    LanLobby first = Beacon("theirs", 27015, 1);
    first.address = "192.168.1.20";
    MergeLanLobby(list, first);
    REQUIRE(list.size() == 1);

    // The same host again is the same row, not a second one, and the newer count wins.
    LanLobby again = Beacon("theirs", 27015, 3);
    again.address = "192.168.1.20";
    MergeLanLobby(list, again);
    REQUIRE(list.size() == 1);
    CHECK(list[0].players == 3);

    // A different machine is a different row even with the same name and port, because the address
    // is what you connect to.
    LanLobby other = Beacon("theirs", 27015, 1);
    other.address = "192.168.1.21";
    MergeLanLobby(list, other);
    CHECK(list.size() == 2);

    // Quiet for a moment is not gone: one lost datagram must not make a game flicker out of the
    // list and back in while somebody is trying to click it.
    AgeLanLobbies(list, 1.0f);
    CHECK(list.size() == 2);

    // Gone for good, though, is gone.
    AgeLanLobbies(list, kLanForgetSeconds);
    CHECK(list.empty());
}

TEST_CASE("A machine hearing a beacon again stops it expiring", "[lan]")
{
    std::vector<LanLobby> list;
    LanLobby lobby = Beacon("steady", 27015);
    lobby.address = "10.0.0.5";

    for (int i = 0; i < 20; ++i)
    {
        AgeLanLobbies(list, 1.0f);
        MergeLanLobby(list, lobby);
    }
    REQUIRE(list.size() == 1);
    CHECK(list[0].silentFor == 0.0f);
}

TEST_CASE("A roomful of beacons cannot grow the list without bound", "[lan][security]")
{
    // The discovery port is reachable by everybody on the network, and each unique address is a
    // new row. The cap is far beyond any house and well short of anything that matters.
    std::vector<LanLobby> list;
    for (int i = 0; i < 500; ++i)
    {
        LanLobby lobby = Beacon("flood", 27015);
        lobby.address = "10.0.0." + std::to_string(i);
        MergeLanLobby(list, lobby);
    }
    CHECK(list.size() <= 32);
}
