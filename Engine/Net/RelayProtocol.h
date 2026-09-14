#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// Talking to a relay.
//
// Two machines behind home routers cannot reach each other: a router forwards a reply to something
// that went out, and refuses anything that did not. Hole punching works around that and mostly
// works; it needs both ends to describe themselves to each other first, it needs one to be
// "controlling" and the other "controlled", and on a symmetric NAT it cannot work at all.
//
// A relay sidesteps the whole problem. Everybody connects outwards to one machine that both can
// reach, and it forwards between them. Outbound always works — it is why the web works without
// anybody configuring a router — so there are no candidates to gather, no roles to negotiate, and
// nothing that can fail on one network and not another. The costs are honest and small: one extra
// hop of latency, and whoever runs the relay pays the bandwidth, which for a four player game is
// about two hundred kilobits a second.
//
// The relay is deliberately stupid. It knows about lobbies and slots and nothing else: not the
// game, not who is the host, not what a snapshot is. Everything above it — the handshake, the
// reliability, the prediction — runs exactly as it does over a plain socket, because as far as the
// transport is concerned a relayed link is just another carrier.

// How many people one lobby holds. Three bits on the wire, and above what the game allows, so the
// limit that matters is the game's and this never becomes the thing quietly capping a lobby.
inline constexpr uint8_t kRelayMaxSlots = 8;
inline constexpr uint8_t kRelayHostSlot = 0;
inline constexpr uint8_t kRelayNoSlot = 0xFF;

// A lobby code is six characters from an alphabet chosen so nobody has to ask which letter it was.
//
// Digits 2 to 9 and the letters, less I and O. Nought and one are gone because they are the things
// people read O and I as, and with those four out what is left is exactly thirty-two symbols: five
// bits each, six characters, a billion codes. A code is read aloud over voice chat or typed off a
// screenshot, and every confusable pair is a mistake waiting to happen.
inline constexpr char kRelayCodeAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
inline constexpr int kRelayCodeLength = 6;

// Text to bits and back. Case is ignored on the way in, because nobody types a code the way it was
// printed. Returns false for anything that is not a code, which is how the join box tells a code
// from an address without asking.
bool EncodeRelayCode(const std::string& text, uint32_t& out);
std::string DecodeRelayCode(uint32_t value);

enum class RelayMessage : uint8_t
{
    // Client to relay.
    Host = 1,      // open a lobby; the relay picks the code
    Join,          // join one by code
    Data,          // forward this to a slot in my lobby
    Leave,         //
    KeepAlive,     // so the router's hole and the relay's idea of me both stay alive

    // Relay to client.
    Hosted = 8,    // your lobby is open, here is its code
    Joined,        // you are in, here is your slot
    Rejected,      // and why
    Relayed,       // something from another slot
    PeerJoined,    // somebody arrived
    PeerLeft       // somebody went
};

enum class RelayRejection : uint8_t
{
    None = 0,
    NoSuchLobby,
    LobbyFull,
    RelayFull,
    TooFast, // more than this relay will take from one address
    Malformed
};

const char* Describe(RelayRejection reason);

// One message, in whichever direction. A single struct rather than a type per message, because
// every field is small and the alternative is a visitor over eleven types that mostly do nothing.
struct RelayPacket
{
    RelayMessage kind = RelayMessage::KeepAlive;
    uint32_t code = 0;                // Join, Hosted
    uint8_t slot = kRelayNoSlot;      // Data (to), Relayed (from), Joined/Hosted (yours), Peer*
    RelayRejection reason = RelayRejection::None;
    std::vector<uint8_t> payload;     // Data, Relayed
};

// The most a relayed payload may be. Under the smallest path MTU once the relay's own header is on
// the front, so a forwarded game datagram never has to be fragmented.
inline constexpr size_t kRelayMaxPayload = 1100;

std::vector<uint8_t> EncodeRelay(const RelayPacket& packet);
// False for anything that is not one of ours, which includes anything a stranger sends to an open
// UDP port. A relay on the internet is reachable by everybody, so this is the first line of it.
bool DecodeRelay(const uint8_t* data, size_t bytes, RelayPacket& out);

} // namespace pred
