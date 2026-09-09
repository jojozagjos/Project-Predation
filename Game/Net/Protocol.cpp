#include "Game/Net/Protocol.h"

#include <algorithm>
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
    case MessageType::Count: break;
    }
    return "Unknown";
}

void WriteMessageHeader(BitWriter& writer, MessageType type)
{
    writer.WriteBits(static_cast<uint32_t>(type), 4);
}

bool ReadMessageHeader(BitReader& reader, MessageType& outType)
{
    const uint32_t raw = reader.ReadBits(4);
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
        writer.WriteBool(player.mantling);
        if (player.mantling)
        {
            writer.WriteQuantised(player.mantlePhase, 0.0f, 1.0f, 6);
            WritePosition(writer, player.mantleEdge);
        }
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
        player.mantling = reader.ReadBool();
        player.mantlePhase = player.mantling ? reader.ReadQuantised(0.0f, 1.0f, 6) : 0.0f;
        player.mantleEdge = player.mantling ? ReadPosition(reader) : glm::vec3(0.0f);

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
    WritePosition(writer, message.position);
    WriteVelocity(writer, message.velocity);
}

bool ReadDrop(BitReader& reader, DropMessage& out)
{
    out.item = reader.ReadByte();
    out.count = static_cast<uint8_t>(reader.ReadBits(6));
    out.position = ReadPosition(reader);
    out.velocity = ReadVelocity(reader);
    return !reader.Overran() && out.count > 0;
}

} // namespace pred
