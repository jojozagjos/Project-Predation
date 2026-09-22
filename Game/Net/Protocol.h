#pragma once

#include "Engine/Net/BitStream.h"
#include "Game/Player/PlayerTypes.h"

#include <array>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <string>
#include <vector>

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

// Bumped whenever the wire changes shape. Two ends that disagree are refused at the door rather
// than left to misread each other, which is what a wire mismatch actually looks like from inside.
inline constexpr uint16_t kProtocolVersion = 9;
// How many bits name a message type. Five, so there is room to add one.
inline constexpr uint32_t kMessageTypeBits = 5;
inline constexpr uint8_t kMaxPlayers = 4;
inline constexpr uint16_t kDefaultPort = 27015;

// Each input packet repeats the last few ticks of input. Losing one then costs nothing, because
// the next packet still carries what the lost one held. Redundancy is far cheaper than a resend:
// an input is only useful for a few milliseconds, and by the time a resend arrived it would be a
// tick the host had already run past.
inline constexpr uint8_t kInputRedundancy = 3;

// How far a voice carries, in metres.
//
// The host refuses to forward speech to anybody further away than this, so a player never receives
// audio they are not entitled to hear. That matters in a game where listening is how you find
// people: forwarding everything and letting each listener attenuate it would work and would also
// mean the whole conversation was on every machine, which is a thing somebody could read.
//
// Shorter than the mixer silences a sound at, so the cut always happens because of this rather than
// because a voice faded to nothing. Past it the mixer is doing nothing anyway.
inline constexpr float kVoiceRange = 28.0f;

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
    // client to host, reliable: my world is built, tell me what has already happened.
    //
    // The host used to send that the moment it let somebody in, which is a moment too early: a
    // client builds its world when it leaves the title screen, a frame or more after the welcome
    // arrives, so every door that had been opened and every item that had been taken was applied to
    // a world that was then thrown away and rebuilt from the map. Only the client knows when it is
    // ready, so only the client can ask.
    Ready,
    // Voice, both directions, unreliable.
    //
    // A client sends its own frames to the host and the host passes them on to whoever is close
    // enough to hear. Unreliable on purpose: a frame is useful for a twentieth of a second and a
    // resend would arrive after the word it belonged to, and the codec fills a gap better than a
    // late packet would anyway.
    Voice,
    // Host to client, unreliable, at the world-state rate: where each creature is and what its body
    // is doing. Its mind runs on the host alone.
    Creatures,
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
    NestBuilt,       // a creature has finished building a nest somewhere
    Count
};

struct WorldEventMessage
{
    WorldEventKind kind = WorldEventKind::Count;
    uint8_t index = 0;    // which door, pickup, locker or crate
    uint8_t player = 0;   // who did it, or who it happened to
    uint8_t other = 0;    // the other party: an occupant, a killer, a stack count
    uint16_t item = 0;    // item id, for a pickup
    // What a spawned pickup is carrying, for a weapon. kDefaultLoad when it has no particular state.
    uint16_t rounds = 511;
    uint16_t reserve = 511;
    bool flag = false;    // open, or hit
    // For a shot: whether what it struck was a fixed surface, as opposed to a person. A hole put on
    // somebody is left hanging in the air the moment they move, so this decides whether one is left
    // at all. Separate from `flag` because a round that hits a player has still hit something.
    bool flag2 = false;
    // Something that already happened, sent to catch a newcomer up rather than happening now. Applied
    // the same and heard not at all: a player joining a game used to arrive to the sound of every door
    // that had been opened and every item that had been dropped, all at once.
    bool quiet = false;
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
    // What state the thing is in, for things that have any: a weapon keeps the magazine it was
    // carrying. 511 means "whatever it starts with", which is what everything else sends.
    uint16_t rounds = 511;
    uint16_t reserve = 511;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
};

// The value that means "no particular state; use whatever this item starts with". Nine bits on the
// wire, so it is the largest number those bits can carry.
inline constexpr uint16_t kDefaultLoad = 511;

// Builds the event that tells everybody a pickup has appeared on the floor.
//
// One function because there are two callers -- the host putting its own item down, and the host
// serving a client's request to put one down -- and written out by hand they drifted: the second
// filled in everything except what the weapon was carrying, so the field kept its default, which
// means "whatever this item starts with". Every machine but the host then put a full magazine on
// the floor, and a weapon dropped by anybody other than the host was picked up full.
//
// A dropped weapon keeping its magazine is a rule about the game, not about either call site, so it
// is written once where it cannot be half-remembered.
WorldEventMessage PickupSpawnedEvent(uint8_t index, uint16_t item, uint8_t count, uint16_t rounds,
                                     uint16_t reserve, const glm::vec3& position,
                                     const glm::vec3& velocity);

// Where the loose rigid bodies have got to. Unreliable and periodic, because a crate sliding across
// the floor is a value that will be sent again rather than an event.
inline constexpr uint8_t kMaxDynamicBodies = 32;

struct DynamicBodyState
{
    uint8_t id = 0;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Who else is here and where they can be reached. Sent by the host whenever the roster changes.
//
// It exists for one reason: when the host goes, whoever is left has to find each other, and by
// then there is nobody to ask. The addresses are the ones the host saw, so on one network they are
// the machines' own addresses and work; through a router they are the hole that router punched,
// which stays open only as long as it stays open.
struct PeerEntry
{
    uint8_t id = 0;
    std::string name;
    std::string address; // empty for the host itself, which everyone already knows how to reach
};

struct PeerListMessage
{
    uint8_t count = 0;
    // Whether the host has started the game, or everybody is still in the lobby. A guest waits in
    // the lobby until this is set, and goes in when it is.
    bool started = false;
    std::array<PeerEntry, kMaxPlayers> peers{};
};

struct WorldStateMessage
{
    uint8_t count = 0;
    std::array<DynamicBodyState, kMaxDynamicBodies> bodies{};
};

// The creatures, as the host's simulation has them.
//
// State rather than events, like the loose bodies: a creature is only shown on every machine but
// the host's, so all anybody else needs is where it is and what its body is doing, and a lost packet
// is replaced a thirtieth of a second later. Sent even when there are none, so that a creature the
// host has removed disappears everywhere else too.
inline constexpr uint8_t kMaxCreatures = 8;
// Nobody has hold of this player.
inline constexpr uint8_t kNotHeld = 0xFF;

struct CreatureSnapshot
{
    uint8_t id = 0;
    // What it was made from, so every machine builds the same animal. In every packet rather than
    // once: something sent once can be missed, and a late joiner never had the chance to receive it.
    uint32_t seed = 0;
    glm::vec3 position{0.0f}; // at its feet
    float yaw = 0.0f;
    float speed = 0.0f;        // metres a second, for the walk cycle and for guessing ahead
    float windup = 0.0f;       // 0 to 1 through a strike's wind-up
    float health = 1.0f;       // as a fraction
    bool alive = true;
    // Lying as if dead while alive -- playing it. Drawn exactly as a death.
    bool down = false;
    float crouch = 0.0f;       // 0 to 1, how low it is creeping
    // What it is doing with its limbs and head: a blow, a grab, carrying somebody, a call, a door. Which,
    // how far through, which arm, and aimed where. Sent only when it is doing one.
    uint8_t action = 0;
    float actionPhase = 0.0f;
    int8_t actionSide = 1;
    glm::vec3 actionTarget{0.0f};
    // How far through a jump it is, 0 on the ground.
    float airborne = 0.0f;
    // What its head is turned to, when anything.
    bool look = false;
    glm::vec3 lookAt{0.0f};
};

struct CreatureStateMessage
{
    // Counts up with every message, so a packet that arrives after a newer one is recognised as old
    // and ignored instead of pulling the creature back to where it was.
    uint16_t sequence = 0;
    uint8_t count = 0;
    std::array<CreatureSnapshot, kMaxCreatures> creatures{};
};

const char* MessageTypeName(MessageType type);

enum class JoinRejection : uint8_t
{
    None = 0,
    VersionMismatch,
    ServerFull,
    Count
};

// One frame of somebody talking.
//
// `speaker` is filled in by the host on the way out. A client sending its own voice leaves it
// alone: the host knows perfectly well who a packet came from, and trusting a client to say would
// let one talk as somebody else.
//
// The sequence is what lets the far end tell a lost frame from a late one, so it can ask the codec
// to invent the gap rather than play a stale frame out of order.
struct VoiceMessage
{
    uint8_t speaker = 0;
    uint16_t sequence = 0;
    std::vector<uint8_t> frame;
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
    // What this client has in its hands, and what its hands are doing. It rides along with the
    // input because it changes at about the same rate and there is nowhere cheaper to put it. The
    // host cannot know any of it otherwise, which is why only the host was ever seen holding
    // anything.
    uint8_t heldItem = 0;
    float aim = 0.0f;
    bool reloading = false;
    float reloadProgress = 0.0f;
    bool reloadEmpty = false; // from an empty magazine, which has its own animation
    // Whether their torch is lit, which the host cannot work out for itself: it is a key this
    // client pressed and nothing else in the protocol implies it. Without it everybody's torch is
    // visible only to themselves, so two players standing in the same dark room see two different
    // rooms -- one lit, one not.
    bool torchOn = false;
    // The low bits of the last host tick this client saw, echoed straight back.
    //
    // That echo is the whole of how a ping is measured. The host knows when it sent every tick
    // because it counts them itself, so the gap between the tick it is on and the tick coming back
    // to it is the round trip, and nothing has to carry a clock or agree about what time it is.
    // Sixteen bits wraps every eighteen minutes at sixty ticks a second, which unsigned subtraction
    // handles without being told.
    uint16_t ackTick = 0;
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
    // How far the sights are up, not whether they are. Sights come up over a fifth of a second, and
    // sending a yes or no makes everyone else watch them snap.
    float aim = 0.0f;
    bool reloading = false;
    float reloadProgress = 0.0f;
    bool reloadEmpty = false; // from an empty magazine, which has its own animation
    // And whether their torch is lit, so the light it throws is in everybody's scene rather than
    // only in its owner's.
    bool torchOn = false;

    // Climbing. Sent as a flag, how far through it is, and where the ledge is, so everyone else
    // sees the climb rather than a body sliding up a wall. Only while it is happening, which is
    // under a second at a time.
    bool mantling = false;
    float mantlePhase = 0.0f;
    glm::vec3 mantleEdge{0.0f};

    // Their round trip to the host, in milliseconds, as the host measures it. Sent for everybody
    // rather than only for whoever is being written to, because a player list shows the whole
    // table: nobody else can measure a third party's connection, so the host has to say.
    uint16_t pingMs = 0;

    // Held by a creature -- being dragged off -- or wrapped up at its nest, and which creature. The host
    // pins them where it says; their own machine has to stop guessing where their legs are taking them.
    uint8_t heldBy = kNotHeld;
    bool cocooned = false;
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
void WritePeerList(BitWriter& writer, const PeerListMessage& message);
void WriteVoice(BitWriter& writer, const VoiceMessage& message);
void WriteCreatureState(BitWriter& writer, const CreatureStateMessage& message);

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
bool ReadPeerList(BitReader& reader, PeerListMessage& out);
bool ReadVoice(BitReader& reader, VoiceMessage& out);
bool ReadCreatureState(BitReader& reader, CreatureStateMessage& out);

} // namespace pred
