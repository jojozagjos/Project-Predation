#include "Game/Net/Protocol.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

namespace pred
{
namespace
{

// How much precision each field is given. Chosen from what the game can perceive, not from what a
// float can hold. A consequence worth noticing: a value that will not fit in the range simply has
// no encoding, so a modified client cannot claim to be ten kilometres away or moving at Mach 3.
constexpr float kPositionMin = -512.0f;
constexpr float kPositionMax = 512.0f;
constexpr int kPositionBits = 20; // about 1 mm across a kilometre

constexpr float kVelocityMin = -64.0f;
constexpr float kVelocityMax = 64.0f;
constexpr int kVelocityBits = 12; // about 3 cm/s, which over one snapshot interval is a millimetre

constexpr int kYawBits = 13;   // about a twentieth of a degree
constexpr int kPitchBits = 12;
constexpr int kUnitBits = 10;  // for values in [-1, 1]
constexpr int kLeanBits = 8;
constexpr int kPhaseBits = 8;
constexpr int kHealthBits = 8;
constexpr int kSpeedScaleBits = 7;
constexpr int kStanceBits = 2;
constexpr int kSequenceDeltaBits = 6;
constexpr size_t kMaxNameLength = 24;
// The tick a client echoes back so the host can time the round trip. Twelve bits is four thousand
// ticks, over a minute at the tick rate, so the wrap is never reached by a delay anybody is still
// playing through; unsigned subtraction handles it either way.
constexpr int kAckTickBits = 12;
// And the round trip itself, in four-millisecond steps up to about a second. A ping is read off a
// panel at a glance, so a millisecond of precision is four times the bits for a digit nobody can
// see change, and anything past a second is unplayable rather than interesting.
constexpr int kPingBits = 8;
constexpr uint16_t kPingStepMs = 4;
constexpr uint16_t kMaxPingMs = ((1 << kPingBits) - 1) * kPingStepMs;

void WritePosition(BitWriter& writer, const glm::vec3& value)
{
    writer.WriteQuantised(value.x, kPositionMin, kPositionMax, kPositionBits);
    writer.WriteQuantised(value.y, kPositionMin, kPositionMax, kPositionBits);
    writer.WriteQuantised(value.z, kPositionMin, kPositionMax, kPositionBits);
}

glm::vec3 ReadPosition(BitReader& reader)
{
    const float x = reader.ReadQuantised(kPositionMin, kPositionMax, kPositionBits);
    const float y = reader.ReadQuantised(kPositionMin, kPositionMax, kPositionBits);
    const float z = reader.ReadQuantised(kPositionMin, kPositionMax, kPositionBits);
    return {x, y, z};
}

void WriteVelocity(BitWriter& writer, const glm::vec3& value)
{
    writer.WriteQuantised(value.x, kVelocityMin, kVelocityMax, kVelocityBits);
    writer.WriteQuantised(value.y, kVelocityMin, kVelocityMax, kVelocityBits);
    writer.WriteQuantised(value.z, kVelocityMin, kVelocityMax, kVelocityBits);
}

glm::vec3 ReadVelocity(BitReader& reader)
{
    const float x = reader.ReadQuantised(kVelocityMin, kVelocityMax, kVelocityBits);
    const float y = reader.ReadQuantised(kVelocityMin, kVelocityMax, kVelocityBits);
    const float z = reader.ReadQuantised(kVelocityMin, kVelocityMax, kVelocityBits);
    return {x, y, z};
}

void WriteLookAngles(BitWriter& writer, float yaw, float pitch)
{
    writer.WriteQuantised(yaw, -glm::pi<float>(), glm::pi<float>(), kYawBits);
    writer.WriteQuantised(pitch, -glm::half_pi<float>(), glm::half_pi<float>(), kPitchBits);
}

void ReadLookAngles(BitReader& reader, float& yaw, float& pitch)
{
    yaw = reader.ReadQuantised(-glm::pi<float>(), glm::pi<float>(), kYawBits);
    pitch = reader.ReadQuantised(-glm::half_pi<float>(), glm::half_pi<float>(), kPitchBits);
}

void WritePlayerInput(BitWriter& writer, const PlayerInput& input)
{
    writer.WriteQuantised(input.move.x, -1.0f, 1.0f, kUnitBits);
    writer.WriteQuantised(input.move.y, -1.0f, 1.0f, kUnitBits);
    WriteLookAngles(writer, input.yaw, input.pitch);
    writer.WriteBool(input.jump);
    writer.WriteBool(input.sprint);
    writer.WriteBool(input.walk);
    writer.WriteBool(input.crouchHeld);
    writer.WriteBool(input.proneHeld);
    writer.WriteQuantised(input.lean, -1.0f, 1.0f, 4);
    writer.WriteQuantised(input.speedScale, 0.0f, 2.0f, kSpeedScaleBits);
}

PlayerInput ReadPlayerInput(BitReader& reader)
{
    PlayerInput input;
    input.move.x = reader.ReadQuantised(-1.0f, 1.0f, kUnitBits);
    input.move.y = reader.ReadQuantised(-1.0f, 1.0f, kUnitBits);
    ReadLookAngles(reader, input.yaw, input.pitch);
    input.jump = reader.ReadBool();
    input.sprint = reader.ReadBool();
    input.walk = reader.ReadBool();
    input.crouchHeld = reader.ReadBool();
    input.proneHeld = reader.ReadBool();
    input.lean = reader.ReadQuantised(-1.0f, 1.0f, 4);
    input.speedScale = reader.ReadQuantised(0.0f, 2.0f, kSpeedScaleBits);

    // The move vector is clamped rather than trusted. Quantisation already keeps each axis inside
    // [-1, 1], but a client that sent (1, 1) would move faster diagonally than anyone else.
    const float length = glm::length(input.move);
    if (length > 1.0f)
    {
        input.move /= length;
    }
    return input;
}

} // namespace

const char* MessageTypeName(MessageType type)
{
    switch (type)
    {
    case MessageType::Join: return "Join";
    case MessageType::Welcome: return "Welcome";
    case MessageType::Rejected: return "Rejected";
    case MessageType::PeerList: return "PeerList";
    case MessageType::Input: return "Input";
    case MessageType::Snapshot: return "Snapshot";
    case MessageType::Leave: return "Leave";
    case MessageType::WorldEvent: return "WorldEvent";
    case MessageType::Interact: return "Interact";
    case MessageType::Shot: return "Shot";
    case MessageType::WorldState: return "WorldState";
    case MessageType::Drop: return "Drop";
    case MessageType::Ready: return "Ready";
    case MessageType::Voice: return "Voice";
    case MessageType::Creatures: return "Creatures";
    case MessageType::Count: break;
    }
    return "Unknown";
}

// Five bits, not four. Four left room for fifteen message types and there were thirteen, so the
// next one added would have been unrepresentable and the one after that would have been read as
// something else entirely.
void WriteMessageHeader(BitWriter& writer, MessageType type)
{
    writer.WriteBits(static_cast<uint32_t>(type), kMessageTypeBits);
}

bool ReadMessageHeader(BitReader& reader, MessageType& outType)
{
    const uint32_t raw = reader.ReadBits(kMessageTypeBits);
    if (reader.Overran() || raw == 0 || raw >= static_cast<uint32_t>(MessageType::Count))
    {
        return false;
    }
    outType = static_cast<MessageType>(raw);
    return true;
}

void WriteJoin(BitWriter& writer, const JoinMessage& message)
{
    writer.WriteBits(message.protocolVersion, 16);
    const size_t length = std::min(message.name.size(), kMaxNameLength);
    writer.WriteBits(static_cast<uint32_t>(length), 5);
    for (size_t i = 0; i < length; ++i)
    {
        writer.WriteByte(static_cast<uint8_t>(message.name[i]));
    }
}

bool ReadJoin(BitReader& reader, JoinMessage& out)
{
    out.protocolVersion = static_cast<uint16_t>(reader.ReadBits(16));
    const uint32_t length = reader.ReadBits(5);
    if (length > kMaxNameLength)
    {
        return false;
    }
    out.name.clear();
    out.name.reserve(length);
    for (uint32_t i = 0; i < length; ++i)
    {
        const uint8_t byte = reader.ReadByte();
        // A name is drawn on other people's screens, so only printable ASCII survives the trip.
        out.name.push_back(byte >= 32 && byte < 127 ? static_cast<char>(byte) : '?');
    }
    return !reader.Overran();
}

void WriteWelcome(BitWriter& writer, const WelcomeMessage& message)
{
    writer.WriteBits(message.playerId, 3);
    writer.WriteUInt(message.tick);
    writer.WriteBits(message.tickRate, 10);
    writer.WriteBits(message.snapshotRate, 10);
}

bool ReadWelcome(BitReader& reader, WelcomeMessage& out)
{
    out.playerId = static_cast<uint8_t>(reader.ReadBits(3));
    out.tick = reader.ReadUInt();
    out.tickRate = static_cast<uint16_t>(reader.ReadBits(10));
    out.snapshotRate = static_cast<uint16_t>(reader.ReadBits(10));
    if (reader.Overran() || out.playerId >= kMaxPlayers || out.tickRate == 0 || out.snapshotRate == 0)
    {
        return false;
    }
    return true;
}

void WriteRejected(BitWriter& writer, const RejectedMessage& message)
{
    writer.WriteBits(static_cast<uint32_t>(message.reason), 3);
    writer.WriteBits(message.serverVersion, 16);
}

bool ReadRejected(BitReader& reader, RejectedMessage& out)
{
    const uint32_t reason = reader.ReadBits(3);
    out.serverVersion = static_cast<uint16_t>(reader.ReadBits(16));
    if (reader.Overran() || reason >= static_cast<uint32_t>(JoinRejection::Count))
    {
        return false;
    }
    out.reason = static_cast<JoinRejection>(reason);
    return true;
}

void WriteInput(BitWriter& writer, const InputMessage& message)
{
    const uint8_t count = std::clamp<uint8_t>(message.count, 1, kInputRedundancy);
    writer.WriteBits(count, 2);

    // The first sequence in full, the rest as gaps from it. Consecutive ticks are the normal case,
    // so those gaps are one or two and cost six bits instead of thirty-two.
    const uint32_t base = message.commands[0].sequence;
    writer.WriteUInt(base);
    WritePlayerInput(writer, message.commands[0].input);

    for (uint8_t i = 1; i < count; ++i)
    {
        const uint32_t sequence = message.commands[i].sequence;
        const uint32_t delta = sequence - base;
        const bool small = delta < (1u << kSequenceDeltaBits);
        writer.WriteBool(small);
        if (small)
        {
            writer.WriteBits(delta, kSequenceDeltaBits);
        }
        else
        {
            writer.WriteUInt(sequence);
        }
        WritePlayerInput(writer, message.commands[i].input);
    }

    // What is in their hands. Once per packet rather than once per tick of input in it, because it
    // does not change three times in three ticks.
    writer.WriteBits(message.heldItem, 6);
    writer.WriteQuantised(message.aim, 0.0f, 1.0f, 5);
    writer.WriteBool(message.reloading);
    if (message.reloading)
    {
        writer.WriteQuantised(message.reloadProgress, 0.0f, 1.0f, 6);
    }
    writer.WriteBool(message.torchOn);
    writer.WriteBits(message.ackTick & 0xFFFu, kAckTickBits);
}

bool ReadInput(BitReader& reader, InputMessage& out)
{
    const uint32_t count = reader.ReadBits(2);
    if (count == 0 || count > kInputRedundancy)
    {
        return false;
    }
    out.count = static_cast<uint8_t>(count);

    const uint32_t base = reader.ReadUInt();
    out.commands[0].sequence = base;
    out.commands[0].input = ReadPlayerInput(reader);

    for (uint32_t i = 1; i < count; ++i)
    {
        const bool small = reader.ReadBool();
        out.commands[i].sequence = small ? base + reader.ReadBits(kSequenceDeltaBits) : reader.ReadUInt();
        out.commands[i].input = ReadPlayerInput(reader);
    }

    out.heldItem = static_cast<uint8_t>(reader.ReadBits(6));
    out.aim = reader.ReadQuantised(0.0f, 1.0f, 5);
    out.reloading = reader.ReadBool();
    out.reloadProgress = out.reloading ? reader.ReadQuantised(0.0f, 1.0f, 6) : 0.0f;
    out.torchOn = reader.ReadBool();
    out.ackTick = static_cast<uint16_t>(reader.ReadBits(kAckTickBits));
    return !reader.Overran();
}

void WriteSnapshot(BitWriter& writer, const SnapshotMessage& message)
{
    writer.WriteUInt(message.tick);
    writer.WriteUInt(message.lastProcessedInput);
    const uint8_t count = std::min<uint8_t>(message.count, kMaxPlayers);
    writer.WriteBits(count, 3);

    for (uint8_t i = 0; i < count; ++i)
    {
        const PlayerSnapshot& player = message.players[i];
        writer.WriteBits(player.playerId, 3);
        writer.WriteBool(player.alive);
        WritePosition(writer, player.position);
        WriteVelocity(writer, player.velocity);
        WriteLookAngles(writer, player.yaw, player.pitch);
        writer.WriteBits(static_cast<uint32_t>(player.stance), kStanceBits);
        writer.WriteQuantised(player.leanAmount, -1.0f, 1.0f, kLeanBits);
        writer.WriteQuantised(player.stridePhase, 0.0f, 1.0f, kPhaseBits);
        writer.WriteQuantised(player.health, 0.0f, 100.0f, kHealthBits);
        writer.WriteBool(player.grounded);
        writer.WriteBits(player.heldItem, 6);
        writer.WriteQuantised(player.aim, 0.0f, 1.0f, 5);
        writer.WriteBool(player.reloading);
        // Only worth a byte, and only when there is a reload to be part way through.
        if (player.reloading)
        {
            writer.WriteQuantised(player.reloadProgress, 0.0f, 1.0f, 6);
        }
        writer.WriteBool(player.torchOn);
        writer.WriteBool(player.mantling);
        if (player.mantling)
        {
            writer.WriteQuantised(player.mantlePhase, 0.0f, 1.0f, 6);
            WritePosition(writer, player.mantleEdge);
        }
        // Ten bits, so anything past a second reads as a second. A connection worse than that is
        // unplayable and the exact figure stops being worth a bit.
        writer.WriteBits(std::min<uint16_t>(player.pingMs, kMaxPingMs) / kPingStepMs, kPingBits);
    }
}

bool ReadSnapshot(BitReader& reader, SnapshotMessage& out)
{
    out.tick = reader.ReadUInt();
    out.lastProcessedInput = reader.ReadUInt();
    const uint32_t count = reader.ReadBits(3);
    if (count > kMaxPlayers)
    {
        return false;
    }
    out.count = static_cast<uint8_t>(count);

    for (uint32_t i = 0; i < count; ++i)
    {
        PlayerSnapshot& player = out.players[i];
        player.playerId = static_cast<uint8_t>(reader.ReadBits(3));
        player.alive = reader.ReadBool();
        player.position = ReadPosition(reader);
        player.velocity = ReadVelocity(reader);
        ReadLookAngles(reader, player.yaw, player.pitch);
        const uint32_t stance = reader.ReadBits(kStanceBits);
        player.leanAmount = reader.ReadQuantised(-1.0f, 1.0f, kLeanBits);
        player.stridePhase = reader.ReadQuantised(0.0f, 1.0f, kPhaseBits);
        player.health = reader.ReadQuantised(0.0f, 100.0f, kHealthBits);
        player.grounded = reader.ReadBool();
        player.heldItem = static_cast<uint8_t>(reader.ReadBits(6));
        player.aim = reader.ReadQuantised(0.0f, 1.0f, 5);
        player.reloading = reader.ReadBool();
        player.reloadProgress = player.reloading ? reader.ReadQuantised(0.0f, 1.0f, 6) : 0.0f;
        player.torchOn = reader.ReadBool();
        player.mantling = reader.ReadBool();
        player.mantlePhase = player.mantling ? reader.ReadQuantised(0.0f, 1.0f, 6) : 0.0f;
        player.mantleEdge = player.mantling ? ReadPosition(reader) : glm::vec3(0.0f);
        player.pingMs = static_cast<uint16_t>(reader.ReadBits(kPingBits) * kPingStepMs);

        if (player.playerId >= kMaxPlayers || stance > static_cast<uint32_t>(PlayerStance::Prone))
        {
            return false;
        }
        player.stance = static_cast<PlayerStance>(stance);
    }
    return !reader.Overran();
}

// --- World events ------------------------------------------------------------------------------
//
// Each kind writes only the fields it uses. A door opening is six bits; writing the whole struct
// would be thirty bytes of zeroes for the privilege of one shared code path.

void WriteWorldEvent(BitWriter& writer, const WorldEventMessage& message)
{
    writer.WriteBits(static_cast<uint32_t>(message.kind), 4);
    writer.WriteBool(message.quiet);
    switch (message.kind)
    {
    case WorldEventKind::DoorMoved:
        writer.WriteBits(message.index, 6);
        writer.WriteBool(message.flag);
        break;

    case WorldEventKind::PickupTaken:
        writer.WriteBits(message.index, 8);
        writer.WriteBits(message.player, 3);
        break;

    case WorldEventKind::PickupSpawned:
        writer.WriteBits(message.index, 8);
        writer.WriteBits(message.item, 8);
        writer.WriteBits(message.other, 6);
        // What it is carrying, so a rifle dropped with three rounds left is picked up with three.
        writer.WriteBits(std::min<uint32_t>(message.rounds, kDefaultLoad), 9);
        writer.WriteBits(std::min<uint32_t>(message.reserve, kDefaultLoad), 9);
        WritePosition(writer, message.position);
        WriteVelocity(writer, message.direction);
        break;

    case WorldEventKind::LockerUsed:
        writer.WriteBits(message.index, 5);
        writer.WriteBits(message.player, 3);
        writer.WriteBool(message.flag); // getting in, or getting out
        break;

    case WorldEventKind::AmmoTaken:
        writer.WriteBits(message.index, 5);
        writer.WriteBits(message.player, 3);
        break;

    case WorldEventKind::ShotFired:
        writer.WriteBits(message.player, 3);
        WritePosition(writer, message.position);
        WritePosition(writer, message.direction); // the far end of the trace, not a unit vector
        writer.WriteBool(message.flag);
        writer.WriteBool(message.flag2);
        break;

    case WorldEventKind::PlayerDamaged:
        writer.WriteBits(message.player, 3);
        writer.WriteBits(message.other, 3);
        writer.WriteQuantised(message.amount, 0.0f, 100.0f, 8);
        break;

    case WorldEventKind::PlayerDied:
        writer.WriteBits(message.player, 3);
        writer.WriteBits(message.other, 3);
        WriteVelocity(writer, message.direction);
        // How long until they are back, in whole seconds, so their screen can count it down.
        writer.WriteBits(std::min<uint32_t>(static_cast<uint32_t>(std::max(message.amount, 0.0f) + 0.5f), 63), 6);
        break;

    case WorldEventKind::PlayerRespawned:
        writer.WriteBits(message.player, 3);
        WritePosition(writer, message.position);
        break;

    case WorldEventKind::Count:
        break;
    }
}

bool ReadWorldEvent(BitReader& reader, WorldEventMessage& out)
{
    const uint32_t kind = reader.ReadBits(4);
    if (kind == 0 || kind >= static_cast<uint32_t>(WorldEventKind::Count))
    {
        return false;
    }
    out.kind = static_cast<WorldEventKind>(kind);
    out.quiet = reader.ReadBool();

    switch (out.kind)
    {
    case WorldEventKind::DoorMoved:
        out.index = static_cast<uint8_t>(reader.ReadBits(6));
        out.flag = reader.ReadBool();
        break;

    case WorldEventKind::PickupTaken:
        out.index = reader.ReadByte();
        out.player = static_cast<uint8_t>(reader.ReadBits(3));
        break;

    case WorldEventKind::PickupSpawned:
        out.index = reader.ReadByte();
        out.item = reader.ReadByte();
        out.other = static_cast<uint8_t>(reader.ReadBits(6));
        out.rounds = static_cast<uint16_t>(reader.ReadBits(9));
        out.reserve = static_cast<uint16_t>(reader.ReadBits(9));
        out.position = ReadPosition(reader);
        out.direction = ReadVelocity(reader);
        break;

    case WorldEventKind::LockerUsed:
        out.index = static_cast<uint8_t>(reader.ReadBits(5));
        out.player = static_cast<uint8_t>(reader.ReadBits(3));
        out.flag = reader.ReadBool();
        break;

    case WorldEventKind::AmmoTaken:
        out.index = static_cast<uint8_t>(reader.ReadBits(5));
        out.player = static_cast<uint8_t>(reader.ReadBits(3));
        break;

    case WorldEventKind::ShotFired:
        out.player = static_cast<uint8_t>(reader.ReadBits(3));
        out.position = ReadPosition(reader);
        out.direction = ReadPosition(reader);
        out.flag = reader.ReadBool();
        out.flag2 = reader.ReadBool();
        break;

    case WorldEventKind::PlayerDamaged:
        out.player = static_cast<uint8_t>(reader.ReadBits(3));
        out.other = static_cast<uint8_t>(reader.ReadBits(3));
        out.amount = reader.ReadQuantised(0.0f, 100.0f, 8);
        break;

    case WorldEventKind::PlayerDied:
        out.player = static_cast<uint8_t>(reader.ReadBits(3));
        out.other = static_cast<uint8_t>(reader.ReadBits(3));
        out.direction = ReadVelocity(reader);
        out.amount = static_cast<float>(reader.ReadBits(6));
        break;

    case WorldEventKind::PlayerRespawned:
        out.player = static_cast<uint8_t>(reader.ReadBits(3));
        out.position = ReadPosition(reader);
        break;

    case WorldEventKind::Count:
        return false;
    }

    return !reader.Overran() && out.player < kMaxPlayers;
}

void WriteInteract(BitWriter& writer, const InteractMessage& message)
{
    writer.WriteBits(message.kind, 3);
    writer.WriteBits(message.index, 8);
}

bool ReadInteract(BitReader& reader, InteractMessage& out)
{
    out.kind = static_cast<uint8_t>(reader.ReadBits(3));
    out.index = reader.ReadByte();
    return !reader.Overran();
}

void WriteShot(BitWriter& writer, const ShotMessage& message)
{
    writer.WriteUInt(message.shotNumber);
    writer.WriteUInt(message.renderTick);
    WritePosition(writer, message.origin);
    // A direction is a unit vector, so it costs the same as a rotation and no more.
    writer.WriteQuaternion(glm::quat(glm::vec3(0.0f, 0.0f, 1.0f), message.direction));
}

bool ReadShot(BitReader& reader, ShotMessage& out)
{
    out.shotNumber = reader.ReadUInt();
    out.renderTick = reader.ReadUInt();
    out.origin = ReadPosition(reader);
    const glm::quat rotation = reader.ReadQuaternion();
    out.direction = glm::normalize(rotation * glm::vec3(0.0f, 0.0f, 1.0f));
    return !reader.Overran();
}

void WriteWorldState(BitWriter& writer, const WorldStateMessage& message)
{
    const uint8_t count = std::min<uint8_t>(message.count, kMaxDynamicBodies);
    writer.WriteBits(count, 6);
    for (uint8_t i = 0; i < count; ++i)
    {
        writer.WriteByte(message.bodies[i].id);
        WritePosition(writer, message.bodies[i].position);
        writer.WriteQuaternion(message.bodies[i].rotation);
    }
}

bool ReadWorldState(BitReader& reader, WorldStateMessage& out)
{
    const uint32_t count = reader.ReadBits(6);
    if (count > kMaxDynamicBodies)
    {
        return false;
    }
    out.count = static_cast<uint8_t>(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        out.bodies[i].id = reader.ReadByte();
        out.bodies[i].position = ReadPosition(reader);
        out.bodies[i].rotation = reader.ReadQuaternion();
    }
    return !reader.Overran();
}

void WriteDrop(BitWriter& writer, const DropMessage& message)
{
    writer.WriteBits(message.item, 8);
    writer.WriteBits(message.count, 6);
    writer.WriteBits(std::min<uint32_t>(message.rounds, kDefaultLoad), 9);
    writer.WriteBits(std::min<uint32_t>(message.reserve, kDefaultLoad), 9);
    WritePosition(writer, message.position);
    WriteVelocity(writer, message.velocity);
}

bool ReadDrop(BitReader& reader, DropMessage& out)
{
    out.item = reader.ReadByte();
    out.count = static_cast<uint8_t>(reader.ReadBits(6));
    out.rounds = static_cast<uint16_t>(reader.ReadBits(9));
    out.reserve = static_cast<uint16_t>(reader.ReadBits(9));
    out.position = ReadPosition(reader);
    out.velocity = ReadVelocity(reader);
    return !reader.Overran() && out.count > 0;
}

namespace
{
void WriteText(BitWriter& writer, const std::string& text, size_t limit)
{
    const size_t length = std::min(text.size(), limit);
    writer.WriteBits(static_cast<uint32_t>(length), 6);
    for (size_t i = 0; i < length; ++i)
    {
        writer.WriteByte(static_cast<uint8_t>(text[i]));
    }
}

bool ReadText(BitReader& reader, std::string& out, size_t limit)
{
    const uint32_t length = reader.ReadBits(6);
    if (length > limit)
    {
        return false;
    }
    out.clear();
    out.reserve(length);
    for (uint32_t i = 0; i < length; ++i)
    {
        const uint8_t byte = reader.ReadByte();
        out.push_back(byte >= 32 && byte < 127 ? static_cast<char>(byte) : '?');
    }
    return !reader.Overran();
}
} // namespace


// The most one voice frame may carry. Opus at speech bitrates produces about eighty bytes for a
// twenty millisecond frame; this is several times that, so a legitimate frame never hits it, and it
// is the bound that stops a forged packet claiming a kilobyte.
constexpr size_t kMaxVoiceBytes = 320;

void WriteVoice(BitWriter& writer, const VoiceMessage& message)
{
    writer.WriteBits(message.speaker, 3);
    writer.WriteBits(message.sequence, 16);
    const auto length = static_cast<uint32_t>(std::min(message.frame.size(), kMaxVoiceBytes));
    writer.WriteBits(length, 9); // 511, comfortably over the cap
    for (uint32_t i = 0; i < length; ++i)
    {
        writer.WriteByte(message.frame[i]);
    }
}

bool ReadVoice(BitReader& reader, VoiceMessage& out)
{
    out.speaker = static_cast<uint8_t>(reader.ReadBits(3));
    out.sequence = static_cast<uint16_t>(reader.ReadBits(16));
    const uint32_t length = reader.ReadBits(9);
    if (out.speaker >= kMaxPlayers || length > kMaxVoiceBytes)
    {
        return false;
    }
    // Checked against what is actually present before anything is reserved, so a packet claiming
    // bytes it does not have buys an allocation for free.
    if (reader.BitsRemaining() < static_cast<size_t>(length) * 8)
    {
        return false;
    }
    out.frame.resize(length);
    for (uint32_t i = 0; i < length; ++i)
    {
        out.frame[i] = reader.ReadByte();
    }
    return !reader.Overran();
}

namespace
{
// A creature runs at six metres a second at most; eight leaves room without spending a bit on it.
constexpr float kCreatureSpeedMax = 8.0f;
constexpr int kCreatureSpeedBits = 7;
constexpr int kCreatureCountBits = 4;
} // namespace

void WriteCreatureState(BitWriter& writer, const CreatureStateMessage& message)
{
    writer.WriteBits(message.sequence, 16);
    const uint8_t count = std::min<uint8_t>(message.count, kMaxCreatures);
    writer.WriteBits(count, kCreatureCountBits);
    for (uint8_t i = 0; i < count; ++i)
    {
        const CreatureSnapshot& creature = message.creatures[i];
        writer.WriteByte(creature.id);
        writer.WriteUInt(creature.seed);
        WritePosition(writer, creature.position);
        // Wrapped into [-pi, pi] first: the yaw a body turns through is not bounded, and a value
        // outside the range would be clamped to its end -- a creature that had turned round twice
        // would be drawn facing somewhere it was not.
        writer.WriteQuantised(std::remainder(creature.yaw, glm::two_pi<float>()), -glm::pi<float>(),
                              glm::pi<float>(), kYawBits);
        writer.WriteQuantised(creature.speed, 0.0f, kCreatureSpeedMax, kCreatureSpeedBits);
        writer.WriteQuantised(creature.windup, 0.0f, 1.0f, 5);
        writer.WriteQuantised(creature.health, 0.0f, 1.0f, 7);
        writer.WriteBool(creature.alive);
        writer.WriteBool(creature.down);
        writer.WriteQuantised(creature.crouch, 0.0f, 1.0f, 3);
    }
}

bool ReadCreatureState(BitReader& reader, CreatureStateMessage& out)
{
    out.sequence = static_cast<uint16_t>(reader.ReadBits(16));
    const uint32_t count = reader.ReadBits(kCreatureCountBits);
    if (count > kMaxCreatures)
    {
        return false;
    }
    out.count = static_cast<uint8_t>(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        CreatureSnapshot& creature = out.creatures[i];
        creature.id = reader.ReadByte();
        creature.seed = reader.ReadUInt();
        creature.position = ReadPosition(reader);
        creature.yaw = reader.ReadQuantised(-glm::pi<float>(), glm::pi<float>(), kYawBits);
        creature.speed = reader.ReadQuantised(0.0f, kCreatureSpeedMax, kCreatureSpeedBits);
        creature.windup = reader.ReadQuantised(0.0f, 1.0f, 5);
        creature.health = reader.ReadQuantised(0.0f, 1.0f, 7);
        creature.alive = reader.ReadBool();
        creature.down = reader.ReadBool();
        creature.crouch = reader.ReadQuantised(0.0f, 1.0f, 3);
    }
    return !reader.Overran();
}

void WritePeerList(BitWriter& writer, const PeerListMessage& message)
{
    const uint8_t count = std::min<uint8_t>(message.count, kMaxPlayers);
    writer.WriteBits(count, 3);
    writer.WriteBool(message.started);
    for (uint8_t i = 0; i < count; ++i)
    {
        writer.WriteBits(message.peers[i].id, 3);
        WriteText(writer, message.peers[i].name, kMaxNameLength);
        WriteText(writer, message.peers[i].address, 48);
    }
}

bool ReadPeerList(BitReader& reader, PeerListMessage& out)
{
    const uint32_t count = reader.ReadBits(3);
    if (count > kMaxPlayers)
    {
        return false;
    }
    out.count = static_cast<uint8_t>(count);
    out.started = reader.ReadBool();
    for (uint32_t i = 0; i < count; ++i)
    {
        out.peers[i].id = static_cast<uint8_t>(reader.ReadBits(3));
        if (!ReadText(reader, out.peers[i].name, kMaxNameLength) ||
            !ReadText(reader, out.peers[i].address, 48))
        {
            return false;
        }
    }
    return !reader.Overran();
}

WorldEventMessage PickupSpawnedEvent(uint8_t index, uint16_t item, uint8_t count, uint16_t rounds,
                                     uint16_t reserve, const glm::vec3& position,
                                     const glm::vec3& velocity)
{
    WorldEventMessage event;
    event.kind = WorldEventKind::PickupSpawned;
    event.index = index;
    event.item = item;
    // `other` carries the stack count for this event. It is the one field on WorldEventMessage whose
    // meaning changes with the kind, which is most of why building this by hand went wrong.
    event.other = count;
    event.rounds = rounds;
    event.reserve = reserve;
    event.position = position;
    // And `direction` is the throw, so it arcs on everybody's screen rather than appearing on the
    // floor already at rest.
    event.direction = velocity;
    return event;
}

} // namespace pred
