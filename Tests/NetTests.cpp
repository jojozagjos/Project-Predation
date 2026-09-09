#include "Engine/Net/BitStream.h"
#include "Engine/Net/Transport.h"
#include "Game/Net/Protocol.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <vector>

using namespace pred;

namespace
{

constexpr float kTick = 1.0f / 60.0f;

// Runs both ends for a while and hands back everything the receiver got.
std::vector<NetPacket> PumpUntil(Transport& receiver, size_t wanted, int maxTicks)
{
    std::vector<NetPacket> collected;
    std::vector<NetPacket> batch;
    for (int i = 0; i < maxTicks && collected.size() < wanted; ++i)
    {
        receiver.Poll(kTick, batch);
        for (NetPacket& packet : batch)
        {
            collected.push_back(std::move(packet));
        }
    }
    return collected;
}

void SendText(Transport& from, PeerId peer, Channel channel, const std::string& text)
{
    from.Send(peer, channel, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string TextOf(const NetPacket& packet)
{
    return std::string(packet.bytes.begin(), packet.bytes.end());
}

} // namespace

// --- The bit stream ----------------------------------------------------------------------------

TEST_CASE("Bit stream round-trips every kind of value", "[net][bitstream]")
{
    BitWriter writer;
    writer.WriteBool(true);
    writer.WriteBool(false);
    writer.WriteByte(0xA7);
    writer.WriteUInt(0xDEADBEEF);
    writer.WriteInt(-123456);
    writer.WriteFloat(3.14159f);
    writer.WriteVec3({1.5f, -2.25f, 1024.0f});
    writer.WriteBits(5u, 3);

    const std::vector<uint8_t>& bytes = writer.Finish();
    BitReader reader(bytes.data(), bytes.size());

    CHECK(reader.ReadBool() == true);
    CHECK(reader.ReadBool() == false);
    CHECK(reader.ReadByte() == 0xA7);
    CHECK(reader.ReadUInt() == 0xDEADBEEF);
    CHECK(reader.ReadInt() == -123456);
    CHECK(reader.ReadFloat() == 3.14159f);
    const glm::vec3 vector = reader.ReadVec3();
    CHECK(vector.x == 1.5f);
    CHECK(vector.y == -2.25f);
    CHECK(vector.z == 1024.0f);
    CHECK(reader.ReadBits(3) == 5u);
    CHECK_FALSE(reader.Overran());
}

TEST_CASE("Bits are packed, not rounded up to bytes", "[net][bitstream]")
{
    BitWriter writer;
    for (int i = 0; i < 8; ++i)
    {
        writer.WriteBool(i % 2 == 0);
    }
    CHECK(writer.BitsWritten() == 8);
    CHECK(writer.BytesWritten() == 1);

    BitWriter stance;
    for (int i = 0; i < 16; ++i)
    {
        stance.WriteBits(2u, 2);
    }
    CHECK(stance.BytesWritten() == 4);
}

TEST_CASE("Signed values cost what their magnitude costs", "[net][bitstream]")
{
    // Zigzag is the whole point of WriteSignedBits: without it the sign bits of a small negative
    // number force a full word.
    BitWriter writer;
    writer.WriteSignedBits(-3, 8);
    writer.WriteSignedBits(3, 8);
    writer.WriteSignedBits(0, 8);
    writer.WriteSignedBits(-64, 8);
    CHECK(writer.BitsWritten() == 32);

    const std::vector<uint8_t>& bytes = writer.Finish();
    BitReader reader(bytes.data(), bytes.size());
    CHECK(reader.ReadSignedBits(8) == -3);
    CHECK(reader.ReadSignedBits(8) == 3);
    CHECK(reader.ReadSignedBits(8) == 0);
    CHECK(reader.ReadSignedBits(8) == -64);
    CHECK_FALSE(reader.Overran());
}

TEST_CASE("Quantised values stay inside their error bound", "[net][bitstream]")
{
    constexpr float kMin = -512.0f;
    constexpr float kMax = 512.0f;
    constexpr int kBits = 20;
    const float step = (kMax - kMin) / static_cast<float>((1u << kBits) - 2u);

    for (float value : {-511.0f, -12.34f, 0.0f, 0.001f, 7.5f, 256.125f, 511.9f})
    {
        BitWriter writer;
        writer.WriteQuantised(value, kMin, kMax, kBits);
        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        const float decoded = reader.ReadQuantised(kMin, kMax, kBits);
        INFO("value " << value << " decoded " << decoded);
        CHECK(std::abs(decoded - value) <= step);
    }
}

TEST_CASE("The middle of a symmetric range survives quantisation exactly", "[net][bitstream]")
{
    // A resting stick has to decode to zero. Anything else and the player walks forward while
    // nobody is touching the controls.
    for (int bits : {4, 8, 10, 13, 16})
    {
        BitWriter writer;
        writer.WriteQuantised(0.0f, -1.0f, 1.0f, bits);
        writer.WriteQuantised(0.0f, -glm::pi<float>(), glm::pi<float>(), bits);
        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        INFO("bits " << bits);
        CHECK(reader.ReadQuantised(-1.0f, 1.0f, bits) == 0.0f);
        CHECK(reader.ReadQuantised(-glm::pi<float>(), glm::pi<float>(), bits) == 0.0f);
    }
}

TEST_CASE("Reading past the end reports an overrun instead of reading memory", "[net][bitstream]")
{
    BitWriter writer;
    writer.WriteUInt(42);
    const std::vector<uint8_t> bytes = writer.Finish();

    // Half a packet, the way a truncated or malicious one arrives.
    BitReader reader(bytes.data(), 2);
    CHECK(reader.ReadUInt() == 0u);
    CHECK(reader.Overran());
    CHECK(reader.BitsRemaining() == 0);

    BitReader empty(nullptr, 0);
    CHECK(empty.ReadByte() == 0);
    CHECK(empty.Overran());
}

// --- The protocol ------------------------------------------------------------------------------

TEST_CASE("An input message round-trips through the wire format", "[net][protocol]")
{
    InputMessage sent;
    sent.count = kInputRedundancy;
    for (uint8_t i = 0; i < kInputRedundancy; ++i)
    {
        sent.commands[i].sequence = 900 + i;
        PlayerInput& input = sent.commands[i].input;
        input.move = {0.0f, 1.0f};
        input.yaw = 1.25f;
        input.pitch = -0.4f;
        input.jump = i == 0;
        input.sprint = true;
        input.crouchHeld = i == 2;
        input.lean = -1.0f;
        input.speedScale = 0.5f;
    }

    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Input);
    WriteInput(writer, sent);
    const std::vector<uint8_t>& bytes = writer.Finish();

    // Three ticks of input in a packet small enough to send sixty times a second without noticing.
    CHECK(bytes.size() <= 32);

    BitReader reader(bytes.data(), bytes.size());
    MessageType type = MessageType::Count;
    REQUIRE(ReadMessageHeader(reader, type));
    CHECK(type == MessageType::Input);

    InputMessage received;
    REQUIRE(ReadInput(reader, received));
    REQUIRE(received.count == kInputRedundancy);
    for (uint8_t i = 0; i < kInputRedundancy; ++i)
    {
        INFO("command " << static_cast<int>(i));
        CHECK(received.commands[i].sequence == sent.commands[i].sequence);
        const PlayerInput& in = received.commands[i].input;
        CHECK(in.move.x == 0.0f);
        CHECK(in.move.y == Catch::Approx(1.0f).margin(0.005));
        CHECK(in.yaw == Catch::Approx(1.25f).margin(0.002));
        CHECK(in.pitch == Catch::Approx(-0.4f).margin(0.002));
        CHECK(in.jump == sent.commands[i].input.jump);
        CHECK(in.sprint);
        CHECK(in.crouchHeld == sent.commands[i].input.crouchHeld);
        CHECK(in.lean == Catch::Approx(-1.0f).margin(0.001));
        CHECK(in.speedScale == Catch::Approx(0.5f).margin(0.02));
    }
}

TEST_CASE("A snapshot round-trips and stays small enough to send at 30 Hz", "[net][protocol]")
{
    SnapshotMessage sent;
    sent.tick = 4321;
    sent.lastProcessedInput = 4310;
    sent.count = kMaxPlayers;
    for (uint8_t i = 0; i < kMaxPlayers; ++i)
    {
        PlayerSnapshot& player = sent.players[i];
        player.playerId = i;
        player.position = {1.5f * i, 2.0f, -3.25f * i};
        player.velocity = {0.0f, -4.0f, 2.5f};
        player.yaw = 0.75f;
        player.pitch = 0.1f;
        player.stance = static_cast<PlayerStance>(i % 3);
        player.leanAmount = 0.0f;
        player.stridePhase = 0.25f;
        player.health = 72.0f;
        player.grounded = i != 1;
        player.alive = i != 3;
    }

    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Snapshot);
    WriteSnapshot(writer, sent);
    const std::vector<uint8_t>& bytes = writer.Finish();

    // A full four-player snapshot is 89 bytes: 160 bits per player plus a 71-bit header, the last
    // eight of those being what is in their hands. At the 30 Hz send rate that is 2.7 kB/s to each
    // client, so a host with three of them spends under 8 kB/s upstream. The bound is here to catch
    // a field being added carelessly, not to be tight.
    CHECK(bytes.size() <= 96);

    BitReader reader(bytes.data(), bytes.size());
    MessageType type = MessageType::Count;
    REQUIRE(ReadMessageHeader(reader, type));
    CHECK(type == MessageType::Snapshot);

    SnapshotMessage received;
    REQUIRE(ReadSnapshot(reader, received));
    CHECK(received.tick == sent.tick);
    CHECK(received.lastProcessedInput == sent.lastProcessedInput);
    REQUIRE(received.count == kMaxPlayers);
    for (uint8_t i = 0; i < kMaxPlayers; ++i)
    {
        INFO("player " << static_cast<int>(i));
        const PlayerSnapshot& player = received.players[i];
        CHECK(player.playerId == i);
        CHECK(player.position.x == Catch::Approx(sent.players[i].position.x).margin(0.002));
        CHECK(player.position.y == Catch::Approx(sent.players[i].position.y).margin(0.002));
        CHECK(player.position.z == Catch::Approx(sent.players[i].position.z).margin(0.002));
        CHECK(player.velocity.y == Catch::Approx(-4.0f).margin(0.01));
        CHECK(player.yaw == Catch::Approx(0.75f).margin(0.002));
        CHECK(player.stance == sent.players[i].stance);
        CHECK(player.leanAmount == 0.0f);
        CHECK(player.health == Catch::Approx(72.0f).margin(0.5));
        CHECK(player.grounded == sent.players[i].grounded);
        CHECK(player.alive == sent.players[i].alive);
    }
}

TEST_CASE("A join and a welcome round-trip", "[net][protocol]")
{
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Join);
    JoinMessage join;
    join.name = "operator-two";
    WriteJoin(writer, join);
    const std::vector<uint8_t>& bytes = writer.Finish();

    BitReader reader(bytes.data(), bytes.size());
    MessageType type = MessageType::Count;
    REQUIRE(ReadMessageHeader(reader, type));
    CHECK(type == MessageType::Join);

    JoinMessage received;
    REQUIRE(ReadJoin(reader, received));
    CHECK(received.protocolVersion == kProtocolVersion);
    CHECK(received.name == "operator-two");

    BitWriter reply;
    WriteMessageHeader(reply, MessageType::Welcome);
    WelcomeMessage welcome;
    welcome.playerId = 2;
    welcome.tick = 99;
    WriteWelcome(reply, welcome);
    const std::vector<uint8_t>& replyBytes = reply.Finish();

    BitReader replyReader(replyBytes.data(), replyBytes.size());
    REQUIRE(ReadMessageHeader(replyReader, type));
    CHECK(type == MessageType::Welcome);
    WelcomeMessage decoded;
    REQUIRE(ReadWelcome(replyReader, decoded));
    CHECK(decoded.playerId == 2);
    CHECK(decoded.tick == 99);
    CHECK(decoded.tickRate == 60);
}

TEST_CASE("Malformed packets are rejected rather than half-applied", "[net][protocol]")
{
    SECTION("a truncated snapshot")
    {
        SnapshotMessage sent;
        sent.count = kMaxPlayers;
        BitWriter writer;
        WriteSnapshot(writer, sent);
        const std::vector<uint8_t> bytes = writer.Finish();

        BitReader reader(bytes.data(), bytes.size() / 2);
        SnapshotMessage received;
        CHECK_FALSE(ReadSnapshot(reader, received));
    }

    SECTION("an unknown message type")
    {
        BitWriter writer;
        writer.WriteBits(static_cast<uint32_t>(MessageType::Count) + 3u, 4);
        const std::vector<uint8_t> bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        MessageType type = MessageType::Count;
        CHECK_FALSE(ReadMessageHeader(reader, type));
    }

    SECTION("a player count nobody could have")
    {
        BitWriter writer;
        writer.WriteUInt(1);
        writer.WriteUInt(1);
        writer.WriteBits(7u, 3); // seven players in a four-player game
        const std::vector<uint8_t> bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        SnapshotMessage received;
        CHECK_FALSE(ReadSnapshot(reader, received));
    }

    SECTION("an empty packet")
    {
        BitReader reader(nullptr, 0);
        MessageType type = MessageType::Count;
        CHECK_FALSE(ReadMessageHeader(reader, type));
    }
}

TEST_CASE("A diagonal move cannot be made faster than a straight one", "[net][protocol]")
{
    // The one place a client could cheat for free if the host simply believed the packet.
    InputMessage sent;
    sent.count = 1;
    sent.commands[0].input.move = {1.0f, 1.0f};

    BitWriter writer;
    WriteInput(writer, sent);
    const std::vector<uint8_t>& bytes = writer.Finish();

    BitReader reader(bytes.data(), bytes.size());
    InputMessage received;
    REQUIRE(ReadInput(reader, received));
    CHECK(glm::length(received.commands[0].input.move) == Catch::Approx(1.0f).margin(0.005));
}

// --- The transport -----------------------------------------------------------------------------

TEST_CASE("A client connects to a host and both are told", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    auto client = CreateLoopbackTransport();

    REQUIRE(host->Listen(40001));
    CHECK(host->IsListening());
    REQUIRE(client->Connect("loopback", 40001));

    const std::vector<PeerId> hostSaw = host->TakeConnected();
    const std::vector<PeerId> clientSaw = client->TakeConnected();
    REQUIRE(hostSaw.size() == 1);
    REQUIRE(clientSaw.size() == 1);
    CHECK(clientSaw[0] == kHostPeer);
    CHECK(host->Peers() == hostSaw);

    // Reading the events clears them.
    CHECK(host->TakeConnected().empty());
}

TEST_CASE("Connecting to a port nobody is listening on fails", "[net][transport]")
{
    auto client = CreateLoopbackTransport();
    CHECK_FALSE(client->Connect("loopback", 40002));
    CHECK(client->Peers().empty());
}

TEST_CASE("Two hosts cannot take the same port", "[net][transport]")
{
    auto first = CreateLoopbackTransport();
    auto second = CreateLoopbackTransport();
    REQUIRE(first->Listen(40003));
    CHECK_FALSE(second->Listen(40003));
}

TEST_CASE("Packets cross in both directions on a perfect link", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    auto client = CreateLoopbackTransport();
    REQUIRE(host->Listen(40004));
    REQUIRE(client->Connect("loopback", 40004));
    const PeerId clientPeer = host->TakeConnected()[0];

    SendText(*client, kHostPeer, Channel::Unreliable, "input");
    SendText(*host, clientPeer, Channel::Reliable, "welcome");

    const std::vector<NetPacket> atHost = PumpUntil(*host, 1, 4);
    REQUIRE(atHost.size() == 1);
    CHECK(atHost[0].peer == clientPeer);
    CHECK(atHost[0].channel == Channel::Unreliable);
    CHECK(TextOf(atHost[0]) == "input");

    const std::vector<NetPacket> atClient = PumpUntil(*client, 1, 4);
    REQUIRE(atClient.size() == 1);
    CHECK(atClient[0].peer == kHostPeer);
    CHECK(atClient[0].channel == Channel::Reliable);
    CHECK(TextOf(atClient[0]) == "welcome");
}

TEST_CASE("Latency holds a packet back for about the right time", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    auto client = CreateLoopbackTransport();
    REQUIRE(host->Listen(40005));
    REQUIRE(client->Connect("loopback", 40005));

    NetConditions conditions;
    conditions.latencyMs = 100.0f;
    client->SetConditions(conditions);

    SendText(*client, kHostPeer, Channel::Unreliable, "late");

    std::vector<NetPacket> batch;
    int ticks = 0;
    while (batch.empty() && ticks < 60)
    {
        host->Poll(kTick, batch);
        ++ticks;
    }
    REQUIRE(batch.size() == 1);
    const float elapsed = static_cast<float>(ticks) * kTick;
    CHECK(elapsed >= 0.1f);
    CHECK(elapsed <= 0.1f + 2.0f * kTick);
}

TEST_CASE("Unreliable packets are lost and reliable ones are not", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    auto client = CreateLoopbackTransport();
    REQUIRE(host->Listen(40006));
    REQUIRE(client->Connect("loopback", 40006));

    NetConditions conditions;
    conditions.lossPercent = 100.0f;
    client->SetConditions(conditions);

    for (int i = 0; i < 10; ++i)
    {
        SendText(*client, kHostPeer, Channel::Unreliable, "snapshot");
    }
    CHECK(PumpUntil(*host, 1, 60).empty());
    CHECK(client->Stats().packetsDropped == 10);

    // The same link, the same 100% loss, on the channel that promises delivery.
    SendText(*client, kHostPeer, Channel::Reliable, "fire");
    const std::vector<NetPacket> arrived = PumpUntil(*host, 1, 120);
    REQUIRE(arrived.size() == 1);
    CHECK(TextOf(arrived[0]) == "fire");
}

TEST_CASE("Reliable packets arrive in order however bad the jitter", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    auto client = CreateLoopbackTransport();
    REQUIRE(host->Listen(40007));
    REQUIRE(client->Connect("loopback", 40007));

    NetConditions conditions;
    conditions.latencyMs = 40.0f;
    conditions.jitterMs = 120.0f; // jitter three times the latency: packets overtake each other
    conditions.lossPercent = 20.0f;
    client->SetConditions(conditions);

    constexpr int kCount = 24;
    for (int i = 0; i < kCount; ++i)
    {
        SendText(*client, kHostPeer, Channel::Reliable, std::to_string(i));
    }

    const std::vector<NetPacket> arrived = PumpUntil(*host, kCount, 600);
    REQUIRE(arrived.size() == kCount);
    for (int i = 0; i < kCount; ++i)
    {
        INFO("position " << i);
        CHECK(TextOf(arrived[static_cast<size_t>(i)]) == std::to_string(i));
    }
}

TEST_CASE("A duplicated unreliable packet is delivered twice", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    auto client = CreateLoopbackTransport();
    REQUIRE(host->Listen(40008));
    REQUIRE(client->Connect("loopback", 40008));

    NetConditions conditions;
    conditions.duplicatePercent = 100.0f;
    client->SetConditions(conditions);

    SendText(*client, kHostPeer, Channel::Unreliable, "once");
    const std::vector<NetPacket> arrived = PumpUntil(*host, 2, 10);
    CHECK(arrived.size() == 2);
    CHECK(client->Stats().packetsDuplicated == 1);
}

TEST_CASE("A host serves three clients without crossing their mail", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    std::vector<std::unique_ptr<Transport>> clients;
    for (int i = 0; i < 3; ++i)
    {
        clients.push_back(CreateLoopbackTransport());
    }
    REQUIRE(host->Listen(40009));
    for (auto& client : clients)
    {
        REQUIRE(client->Connect("loopback", 40009));
    }

    const std::vector<PeerId> peers = host->TakeConnected();
    REQUIRE(peers.size() == 3);

    for (size_t i = 0; i < peers.size(); ++i)
    {
        SendText(*host, peers[i], Channel::Unreliable, "for-" + std::to_string(i));
    }

    for (size_t i = 0; i < clients.size(); ++i)
    {
        const std::vector<NetPacket> arrived = PumpUntil(*clients[i], 1, 4);
        REQUIRE(arrived.size() == 1);
        CHECK(TextOf(arrived[0]) == "for-" + std::to_string(i));
    }
}

TEST_CASE("Disconnecting tells both ends", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    auto client = CreateLoopbackTransport();
    REQUIRE(host->Listen(40010));
    REQUIRE(client->Connect("loopback", 40010));
    const PeerId clientPeer = host->TakeConnected()[0];
    client->TakeConnected();

    host->Disconnect(clientPeer);
    CHECK(host->Peers().empty());
    CHECK(host->TakeDisconnected() == std::vector<PeerId>{clientPeer});
    CHECK(client->TakeDisconnected() == std::vector<PeerId>{kHostPeer});
    CHECK(client->Peers().empty());
}

TEST_CASE("A client that goes away leaves the host intact", "[net][transport]")
{
    auto host = CreateLoopbackTransport();
    REQUIRE(host->Listen(40011));
    PeerId clientPeer = kInvalidPeer;
    {
        auto client = CreateLoopbackTransport();
        REQUIRE(client->Connect("loopback", 40011));
        clientPeer = host->TakeConnected()[0];
    }

    CHECK(host->TakeDisconnected() == std::vector<PeerId>{clientPeer});
    CHECK(host->Peers().empty());
    // Sending to a peer that is gone is a no-op, not a crash.
    SendText(*host, clientPeer, Channel::Reliable, "anyone there");
}

TEST_CASE("A rotation round-trips in 29 bits", "[net][bitstream]")
{
    // Dropping the largest component and rebuilding it costs about a tenth of a degree, which is
    // far below what anyone can see on a crate sliding across a floor.
    const glm::quat rotations[] = {
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        glm::normalize(glm::quat(0.5f, 0.5f, 0.5f, 0.5f)),
        glm::normalize(glm::quat(0.1f, -0.7f, 0.2f, 0.65f)),
        glm::normalize(glm::quat(-0.3f, 0.4f, -0.85f, 0.1f)),
    };

    for (const glm::quat& rotation : rotations)
    {
        BitWriter writer;
        writer.WriteQuaternion(rotation);
        CHECK(writer.BitsWritten() == 29);

        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        const glm::quat decoded = reader.ReadQuaternion();

        // q and -q are the same rotation, so compare what they do rather than their components.
        const glm::vec3 axis{0.37f, -0.51f, 0.77f};
        const glm::vec3 before = rotation * axis;
        const glm::vec3 after = decoded * axis;
        INFO("turned a vector " << glm::degrees(std::acos(std::clamp(
                    glm::dot(glm::normalize(before), glm::normalize(after)), -1.0f, 1.0f)))
                                << " degrees off");
        CHECK(glm::distance(before, after) < 0.005f);
    }
}

TEST_CASE("World events carry only what their kind needs", "[net][protocol]")
{
    SECTION("a door is a handful of bits")
    {
        WorldEventMessage sent;
        sent.kind = WorldEventKind::DoorMoved;
        sent.index = 3;
        sent.flag = true;

        BitWriter writer;
        WriteMessageHeader(writer, MessageType::WorldEvent);
        WriteWorldEvent(writer, sent);
        // Four bits of message type, four of event kind, six of index and one flag.
        CHECK(writer.BitsWritten() == 15);

        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        MessageType type = MessageType::Count;
        REQUIRE(ReadMessageHeader(reader, type));
        CHECK(type == MessageType::WorldEvent);

        WorldEventMessage received;
        REQUIRE(ReadWorldEvent(reader, received));
        CHECK(received.kind == WorldEventKind::DoorMoved);
        CHECK(received.index == 3);
        CHECK(received.flag);
    }

    SECTION("a dropped item carries where it was thrown")
    {
        WorldEventMessage sent;
        sent.kind = WorldEventKind::PickupSpawned;
        sent.index = 12;
        sent.item = 5;
        sent.other = 3;
        sent.position = {1.5f, 0.8f, -2.25f};
        sent.direction = {0.5f, 2.0f, -1.5f};

        BitWriter writer;
        WriteWorldEvent(writer, sent);
        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());

        WorldEventMessage received;
        REQUIRE(ReadWorldEvent(reader, received));
        CHECK(received.index == 12);
        CHECK(received.item == 5);
        CHECK(received.other == 3);
        CHECK(received.position.x == Catch::Approx(1.5f).margin(0.002));
        CHECK(received.position.z == Catch::Approx(-2.25f).margin(0.002));
        CHECK(received.direction.y == Catch::Approx(2.0f).margin(0.05));
    }

    SECTION("a death carries the direction of the blow")
    {
        WorldEventMessage sent;
        sent.kind = WorldEventKind::PlayerDied;
        sent.player = 2;
        sent.other = 1;
        sent.direction = {0.0f, 1.0f, -6.0f};

        BitWriter writer;
        WriteWorldEvent(writer, sent);
        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());

        WorldEventMessage received;
        REQUIRE(ReadWorldEvent(reader, received));
        CHECK(received.player == 2);
        CHECK(received.other == 1);
        CHECK(received.direction.z == Catch::Approx(-6.0f).margin(0.05));
    }

    SECTION("an event naming a player nobody could be is rejected")
    {
        BitWriter writer;
        writer.WriteBits(static_cast<uint32_t>(WorldEventKind::PlayerDied), 4);
        writer.WriteBits(7u, 3); // player seven in a four-player game
        writer.WriteBits(0u, 3);
        writer.WriteBits(0u, 36);
        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        WorldEventMessage received;
        CHECK_FALSE(ReadWorldEvent(reader, received));
    }
}

TEST_CASE("A shot request round-trips its aim", "[net][protocol]")
{
    ShotMessage sent;
    sent.shotNumber = 41;
    sent.origin = {2.0f, 1.6f, -3.0f};
    sent.direction = glm::normalize(glm::vec3(0.3f, -0.1f, -0.95f));

    BitWriter writer;
    WriteShot(writer, sent);
    const std::vector<uint8_t>& bytes = writer.Finish();
    BitReader reader(bytes.data(), bytes.size());

    ShotMessage received;
    REQUIRE(ReadShot(reader, received));
    CHECK(received.shotNumber == 41);
    CHECK(received.origin.y == Catch::Approx(1.6f).margin(0.002));
    // A tenth of a degree at sixty metres is six centimetres, which is inside a torso.
    CHECK(glm::distance(received.direction, sent.direction) < 0.005f);
}

TEST_CASE("Loose objects are sent as state, and stay small", "[net][protocol]")
{
    WorldStateMessage sent;
    sent.count = 16;
    for (uint8_t i = 0; i < sent.count; ++i)
    {
        sent.bodies[i].id = i;
        sent.bodies[i].position = {static_cast<float>(i), 0.4f, -static_cast<float>(i) * 0.5f};
        sent.bodies[i].rotation = glm::normalize(glm::quat(0.6f, 0.1f * static_cast<float>(i), 0.3f, 0.7f));
    }

    BitWriter writer;
    WriteWorldState(writer, sent);
    const std::vector<uint8_t>& bytes = writer.Finish();
    // Sixteen loose objects in under 200 bytes, at the same rate as a snapshot.
    CHECK(bytes.size() < 200);

    BitReader reader(bytes.data(), bytes.size());
    WorldStateMessage received;
    REQUIRE(ReadWorldState(reader, received));
    REQUIRE(received.count == 16);
    for (uint8_t i = 0; i < 16; ++i)
    {
        CHECK(received.bodies[i].id == i);
        CHECK(received.bodies[i].position.x == Catch::Approx(static_cast<float>(i)).margin(0.002));
    }
}
