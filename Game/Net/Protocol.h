#pragma once

#include "Engine/Net/BitStream.h"
#include "Game/Player/PlayerTypes.h"

#include <array>
#include <glm/gtc/quaternion.hpp>
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
    Input,       // client to host, unreliable, every tick
    Snapshot,    // host to client, unreliable, at the send rate
    Leave,       // either direction, reliable
    WorldEvent,  // host to client, reliable: something in the world changed
    Interact,    // client to host, reliable: I want to use that
    Shot,        // client to host, reliable: I pulled the trigger
    WorldState,  // host to client, unreliable: where the loose physics has got to
    Drop,        // client to host, reliable: I am putting this down
    Count
};

// Discrete changes to the world. Reliable, because each one is a thing that happened rather than a
// value that will be sent again: missing "the door opened" leaves the door shut for that player for
// ever, where missing a snapshot costs a thirtieth of a second.
enum class WorldEventKind : uint8_t
{
    DoorMoved = 1,   // a door was opened or closed
    PickupTaken,     // something was picked up off the floor
    PickupSpawned,   // something was dropped onto it
    LockerUsed,      // somebody got into or out of a locker
    AmmoTaken,       // a crate was drawn from
    ShotFired,       // so everyone sees the flash and the tracer
    PlayerDamaged,   // health changed, and by whose hand
    PlayerDied,      // with the direction of the blow, for the ragdoll
    PlayerRespawned,
    Count
};

struct WorldEventMessage
{
    WorldEventKind kind = WorldEventKind::Count;
    uint8_t index = 0;    // which door, pickup, locker or crate
    uint8_t player = 0;   // who did it, or who it happened to
    uint8_t other = 0;    // the other party: an occupant, a killer, a stack count
    uint16_t item = 0;    // item id, for a pickup
    bool flag = false;    // open, or hit
    float amount = 0.0f;  // damage, or remaining health
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f};
};

// What a client asks the host to do. It is a request: the host checks the player is close enough
// and that the thing is still there before anything happens.
struct InteractMessage
{
    uint8_t kind = 0; // matches InteractionKind
    uint8_t index = 0;
};

// A shot the client has already played for itself. The host re-runs it: it owns the ammunition, the
// fire rate and what the round hit.
struct ShotMessage
{
    uint32_t shotNumber = 0;
    // The host tick this client was drawing everyone else at when it pulled the trigger. Remote
    // players are shown a fixed delay in the past, so this is the only moment at which the shot
    // was aimed at anything, and it is the moment the host has to rewind to.
    uint32_t renderTick = 0;
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f};
};

// A client putting something down. It is a request like any other: the host spawns the item and
// tells everybody, and the client waits to be told rather than dropping one of its own. A client
// that dropped locally created an item nobody else had, which could not then be picked up and
// could be dropped again for a second copy.
struct DropMessage
{
    uint16_t item = 0;
    uint8_t count = 1;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
};

// Where the loose rigid bodies have got to. Unreliable and periodic, because a crate sliding across
// the floor is a value that will be sent again rather than an event.
inline constexpr uint8_t kMaxDynamicBodies = 32;

struct DynamicBodyState
{
    uint8_t id = 0;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct WorldStateMessage
{
    uint8_t count = 0;
    std::array<DynamicBodyState, kMaxDynamicBodies> bodies{};
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

    // What is in their hands, and what those hands are doing. Sent as the item rather than the
    // weapon so a medical kit shows up as well as a rifle: from outside, holding something is
    // holding something.
    uint8_t heldItem = 0;
    bool aiming = false;
    bool reloading = false;
    float reloadProgress = 0.0f;
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
void WriteWorldEvent(BitWriter& writer, const WorldEventMessage& message);
void WriteInteract(BitWriter& writer, const InteractMessage& message);
void WriteShot(BitWriter& writer, const ShotMessage& message);
void WriteWorldState(BitWriter& writer, const WorldStateMessage& message);
void WriteDrop(BitWriter& writer, const DropMessage& message);

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
bool ReadWorldEvent(BitReader& reader, WorldEventMessage& out);
bool ReadInteract(BitReader& reader, InteractMessage& out);
bool ReadShot(BitReader& reader, ShotMessage& out);
bool ReadWorldState(BitReader& reader, WorldStateMessage& out);
bool ReadDrop(BitReader& reader, DropMessage& out);

} // namespace pred
