#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// Finding each other over the internet, and then talking directly.
//
// Two PCs behind home routers cannot simply dial each other: a router lets in replies to things that
// went out, and nothing else. So:
//
//   1. Each game asks a public STUN server what its socket looks like from outside -- the router's
//      address and the port the router gave it. (STUN is a tiny standard for exactly this question;
//      Cloudflare and Google run free ones.)
//   2. The lobby server -- a small web service -- hands the host a code, and when a guest types it
//      tells each where the other might be: that outside address, and their addresses inside their
//      own networks.
//   3. Both send probes to every address the other might be at, at once. Each router sees its own
//      game's probe go out first, takes the other's arriving probe for the reply, and lets it in.
//      That is hole punching, and from then on the game runs directly, PC to PC.
//
// Not one byte of the game goes through the lobby server, so it costs nothing to run.
//
// It cannot get through a router that hands out a different outside port for every destination (a
// "symmetric" one: some phone hotspots and offices). The game notices that from the STUN answers and
// says so plainly rather than failing silently.

// A lobby code is six characters from an alphabet chosen so nobody has to ask which letter it was.
//
// Digits 2 to 9 and the letters, less I and O: nought and one are what people read O and I as, and
// with those four gone exactly thirty-two symbols are left, five bits each, a billion codes.
inline constexpr char kLobbyCodeAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
inline constexpr int kLobbyCodeLength = 6;

// Text to a code and back. Case, spaces and dashes are ignored on the way in, because nobody types a
// code the way it was printed. False for anything that is not a code, which is how the join box tells
// a code from an address without asking.
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
    // From "a.b.c.d:port".
    static bool ParseWithPort(const std::string& text, LobbyEndpoint& out);
};

inline constexpr size_t kLobbyMaxCandidates = 6;
inline constexpr size_t kLobbyMaxNameLength = 24;

// Why the lobby server said no. Its answers are words in JSON; these are what the game makes of them.
enum class LobbyRejection : uint8_t
{
    None = 0,
    NoSuchLobby,
    LobbyFull,
    WrongVersion, // that lobby is running a different build
    ServerFull,
    TooFast,
    Malformed
};

const char* Describe(LobbyRejection reason);
LobbyRejection RejectionFromText(const std::string& error);

// One row of the public list.
struct LobbyListing
{
    uint32_t code = 0;
    uint8_t players = 0;
    uint8_t maxPlayers = 0;
    bool started = false;
    std::string name;
};

// --- Probes: one game to another, straight through the holes -----------------------------------

enum class LobbyMessage : uint8_t
{
    Probe = 32, // are you there? (carries the introduction's token)
    ProbeReply  // yes
};

struct LobbyPacket
{
    LobbyMessage kind = LobbyMessage::Probe;
    uint32_t token = 0; // which introduction this is, from the lobby server
};

std::vector<uint8_t> EncodeLobby(const LobbyPacket& packet);
// False for anything that is not one of ours, which includes anything a stranger sends to an open
// port.
bool DecodeLobby(const uint8_t* data, size_t bytes, LobbyPacket& out);
bool IsLobbyDatagram(const uint8_t* data, size_t bytes);

// --- STUN: what this socket looks like from outside ---------------------------------------------
//
// Only the one question the game needs (RFC 5389 "binding"), and only the answer to it.

using StunTransaction = std::array<uint8_t, 12>;

std::vector<uint8_t> EncodeStunRequest(const StunTransaction& transaction);
bool IsStunDatagram(const uint8_t* data, size_t bytes);
// The answer to `transaction`: the address the server saw the request arrive from.
bool DecodeStunResponse(const uint8_t* data, size_t bytes, const StunTransaction& transaction,
                        LobbyEndpoint& mapped);
// The server's half, for tests: a request's transaction, and the answer to it.
bool DecodeStunRequest(const uint8_t* data, size_t bytes, StunTransaction& transaction);
std::vector<uint8_t> EncodeStunResponse(const StunTransaction& transaction, const LobbyEndpoint& mapped);

} // namespace pred
