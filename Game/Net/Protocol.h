#pragma once

#include "Engine/Net/BitStream.h"
#include "Game/Player/PlayerTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace pred
{

// The wire format. Everything that crosses between machines is written and read here and nowhere
// else, so there is one place to look when a packet is wrong and one place to change when the
// format moves on.
//
// Two rules hold throughout:
//   - Every read is checked. A packet is attacker-controlled input, so a reader that believes a
//     count field it was handed will happily be told to allocate four billion players.
//   - Values are quantised to what the game can actually perceive. A position good to two
//     millimetres is indistinguishable from a full float and costs less than half as much.

inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr uint8_t kMaxPlayers = 4;
inline constexpr uint16_t kDefaultPort = 27015;

// Each input packet repeats the last few ticks of input. Losing one then costs nothing, because
// the next packet still carries what the lost one held. Redundancy is far cheaper than a resend:
// an input is only useful for a few milliseconds, and by the time a resend arrived it would be a
// tick the host had already run past.
inline constexpr uint8_t kInputRedundancy = 3;

enum class MessageType : uint8_t
{
    Join = 1,   // client to host, reliable: let me in, here is who I am
    Welcome,    // host to client, reliable: you are player N and here is the tick
    Rejected,   // host to client, reliable: no, and this is why
    PeerList,   // host to client, reliable: who else is here
    Input,      // client to host, unreliable, every tick
    Snapshot,   // host to client, unreliable, at the send rate
    Leave,      // either direction, reliable
    Count
};

const char* MessageTypeName(MessageType type);

enum class JoinRejection : uint8_t
{
    None = 0,
    VersionMismatch,
    ServerFull,
    Count
};

struct JoinMessage
{
    uint16_t protocolVersion = kProtocolVersion;
    std::string name;
};

struct WelcomeMessage
{
    uint8_t playerId = 0;
    uint32_t tick = 0;
    uint16_t tickRate = 60;
    uint16_t snapshotRate = 30;
};

struct RejectedMessage
{
    JoinRejection reason = JoinRejection::None;
    uint16_t serverVersion = kProtocolVersion;
};

// One tick of intent, with the tick it belongs to. The host replies with the last sequence it ran,
// which is how the client knows which of its predicted ticks are settled and which it must replay.
struct InputCommand
{
    uint32_t sequence = 0;
    PlayerInput input;
};

struct InputMessage
{
    uint8_t count = 0;
    std::array<InputCommand, kInputRedundancy> commands{};
};

// What one player looks like from outside. Deliberately smaller than PlayerState: a remote player
// needs to be drawn and heard, not simulated, so timers and buffers stay on the host.
struct PlayerSnapshot
{
    uint8_t playerId = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    PlayerStance stance = PlayerStance::Standing;
    float leanAmount = 0.0f;
    float stridePhase = 0.0f;
    float health = 100.0f;
    bool grounded = true;
    bool alive = true;
};

struct SnapshotMessage
{
    uint32_t tick = 0;
    // The last input sequence the host ran for the client this snapshot is addressed to. Every
    // client gets a different value here, which is why a snapshot is written per client rather
    // than once and broadcast.
    uint32_t lastProcessedInput = 0;
    uint8_t count = 0;
    std::array<PlayerSnapshot, kMaxPlayers> players{};
};

// --- Writing -----------------------------------------------------------------------------------

void WriteMessageHeader(BitWriter& writer, MessageType type);
void WriteJoin(BitWriter& writer, const JoinMessage& message);
void WriteWelcome(BitWriter& writer, const WelcomeMessage& message);
void WriteRejected(BitWriter& writer, const RejectedMessage& message);
void WriteInput(BitWriter& writer, const InputMessage& message);
void WriteSnapshot(BitWriter& writer, const SnapshotMessage& message);

// --- Reading -----------------------------------------------------------------------------------
//
// Every one of these returns false on a malformed, truncated or implausible packet. The caller
// drops the packet; it never half-applies one.

bool ReadMessageHeader(BitReader& reader, MessageType& outType);
bool ReadJoin(BitReader& reader, JoinMessage& out);
bool ReadWelcome(BitReader& reader, WelcomeMessage& out);
bool ReadRejected(BitReader& reader, RejectedMessage& out);
bool ReadInput(BitReader& reader, InputMessage& out);
bool ReadSnapshot(BitReader& reader, SnapshotMessage& out);

} // namespace pred
