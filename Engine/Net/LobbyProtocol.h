#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// Finding each other over the internet, and then talking directly.
//
// Two PCs behind home routers cannot simply dial each other: a router lets in replies to things that
// went out, and nothing else. What both of them can reach is a server with a public address. So each
// tells the lobby server who it is, the server tells each where the other is -- both the address the
// server saw it arrive from, which is its router, and the addresses it has inside its own network --
// and then both send to each other at once. Each router sees a packet going out to the other before
// the other's arrives, takes the arriving one for a reply, and lets it in. That is hole punching.
//
// The server only makes introductions. Not one byte of the game goes through it, so it costs next to
// nothing to run: a lobby is a few dozen bytes every two seconds while it is open, and a join is a
// handful of datagrams once. That is the whole reason this replaced the relay, which forwarded every
// byte of every game and had to be paid for by the byte.
//
// It is not certain to work. A router that hands out a different outside port for every destination
// (a "symmetric" NAT, mostly found on phone networks and some offices) cannot be punched through, and
// the game says so plainly rather than failing silently. Everything else -- which is nearly every home
// router -- works with nothing opened and nothing configured.
//
// Portable on purpose: no engine headers, no logging, nothing but the standard library. The server is
// built on its own, on a Linux machine somewhere, from this file and three others.

inline constexpr uint16_t kLobbyServerPort = 27020;

// A lobby code is six characters from an alphabet chosen so nobody has to ask which letter it was.
//
// Digits 2 to 9 and the letters, less I and O: nought and one are what people read O and I as, and
// with those four gone exactly thirty-two symbols are left, five bits each, a billion codes. A code is
// read aloud over voice chat or typed off a screenshot, and every confusable pair is a mistake waiting
// to happen.
inline constexpr char kLobbyCodeAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
inline constexpr int kLobbyCodeLength = 6;

// Text to a code and back. Case and surrounding spaces are ignored on the way in, because nobody types
// a code the way it was printed. False for anything that is not a code, which is how the join box
// tells a code from an address without asking.
bool EncodeLobbyCode(const std::string& text, uint32_t& out);
std::string DecodeLobbyCode(uint32_t value);

// An IPv4 address and a port, in host byte order.
struct LobbyEndpoint
{
    uint32_t address = 0;
    uint16_t port = 0;

    bool Valid() const { return address != 0 && port != 0; }
    bool operator==(const LobbyEndpoint& other) const
    {
        return address == other.address && port == other.port;
    }
    bool operator!=(const LobbyEndpoint& other) const { return !(*this == other); }
    // "a.b.c.d", without the port.
    std::string AddressText() const;
    // "a.b.c.d:port".
    std::string ToString() const;
    // From "a.b.c.d". Names are not looked up here: that needs the network, and this file does not.
    static bool Parse(const std::string& address, uint16_t port, LobbyEndpoint& out);
};

// Where one machine might be reached: how the server saw it, plus its addresses on its own network.
// Six is more than any PC has adapters worth trying.
inline constexpr size_t kLobbyMaxCandidates = 6;
inline constexpr size_t kLobbyMaxNameLength = 24;
// Rows in one answer to "what is open". Sixteen short rows sit far inside one datagram.
inline constexpr size_t kLobbyMaxListed = 16;

enum class LobbyMessage : uint8_t
{
    // A game to the server.
    Host = 1, // open a lobby for the game I am running; the server picks the code
    Update,   // still here, this many players, started or not (every two seconds)
    Close,    // the game has ended
    Join,     // introduce me to the lobby with this code
    List,     // what is open to anybody?

    // The server to a game.
    Hosted = 16, // your lobby is open: here is its code
    Introduce,   // here is somebody to punch through to, and how
    Rejected,    // no, and why
    Listing,     // here is what is open

    // One game to another, straight through the holes the introduction opened.
    Probe = 32, // are you there? (carries the introduction's token)
    ProbeReply  // yes
};

enum class LobbyRejection : uint8_t
{
    None = 0,
    NoSuchLobby,
    LobbyFull,
    WrongVersion, // that lobby is running a different build
    ServerFull,
    TooFast, // more than the server takes from one address
    Malformed
};

const char* Describe(LobbyRejection reason);

// One row of the public list.
struct LobbyListing
{
    uint32_t code = 0;
    uint8_t players = 0;
    uint8_t maxPlayers = 0;
    bool started = false;
    std::string name;
};

// One message, in whichever direction. One struct rather than a type per message, because every field
// is small and most messages use two or three of them.
struct LobbyPacket
{
    LobbyMessage kind = LobbyMessage::Update;
    uint16_t version = 0;   // Host, Join, List: the game's own protocol version
    uint32_t code = 0;      // Update, Close, Join, Hosted, Introduce
    // Handed to the host with its code, and needed to update or close the lobby. Without it anybody
    // who had seen a code could close somebody else's game from the other side of the world.
    uint32_t secret = 0;    // Hosted, Update, Close
    uint32_t token = 0;     // Introduce, Probe, ProbeReply: which introduction this is
    uint8_t players = 0;    // Host, Update
    uint8_t maxPlayers = 0; // Host, Update
    bool started = false;   // Host, Update
    bool listed = false;    // Host, Update: show in the public list
    bool toHost = false;    // Introduce: you are the host, and this is a guest arriving
    LobbyRejection reason = LobbyRejection::None;
    std::string name;       // Host, Update, Introduce: what the lobby is called
    LobbyEndpoint seenAs;   // Hosted, Introduce: where the server saw you come from
    // Host, Update, Join: where I might be reached inside my own network.
    // Introduce: where they might be reached, the way the server saw them first.
    std::vector<LobbyEndpoint> candidates;
    std::vector<LobbyListing> lobbies; // Listing
};

// Under the smallest path MTU anybody still has, whatever is in it.
inline constexpr size_t kLobbyMaxDatagram = 1024;

std::vector<uint8_t> EncodeLobby(const LobbyPacket& packet);
// False for anything that is not one of ours, which includes anything a stranger sends to an open
// port. A server on the internet is reachable by everybody, so this is its first line.
bool DecodeLobby(const uint8_t* data, size_t bytes, LobbyPacket& out);
// Whether a datagram is one of these at all, without decoding it.
bool IsLobbyDatagram(const uint8_t* data, size_t bytes);

} // namespace pred
