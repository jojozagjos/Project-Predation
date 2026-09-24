#include "Game/Net/NetSession.h"

#include "Engine/Core/Log.h"
#include "Game/Player/PlayerController.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <map>
#include <utility>
#include <cmath>

namespace pred
{
namespace
{

float WrapAngle(float radians)
{
    while (radians > glm::pi<float>())
    {
        radians -= glm::two_pi<float>();
    }
    while (radians < -glm::pi<float>())
    {
        radians += glm::two_pi<float>();
    }
    return radians;
}

// Takes the short way round. Interpolating 179 degrees to -179 the long way would spin a body
// almost all the way about in a thirtieth of a second.
float LerpAngle(float from, float to, float t)
{
    return from + WrapAngle(to - from) * t;
}

PlayerSnapshot SnapshotOf(uint8_t id, const PlayerState& state)
{
    PlayerSnapshot snapshot;
    snapshot.playerId = id;
    snapshot.position = state.position;
    snapshot.velocity = state.velocity;
    snapshot.yaw = WrapAngle(state.yaw);
    snapshot.pitch = state.pitch;
    snapshot.stance = state.stance;
    snapshot.leanAmount = state.leanAmount;
    snapshot.stridePhase = state.stridePhase;
    snapshot.mantling = state.mantling;
    snapshot.mantlePhase = state.mantleDuration > 0.0f
                               ? std::clamp(state.mantleTime / state.mantleDuration, 0.0f, 1.0f)
                               : 0.0f;
    snapshot.mantleEdge = state.mantleEdge;
    snapshot.health = state.health;
    snapshot.grounded = state.grounded;
    snapshot.alive = state.alive;
    return snapshot;
}

void ApplySnapshot(RemotePlayerView& view, const PlayerSnapshot& snapshot)
{
    view.id = snapshot.playerId;
    view.position = snapshot.position;
    view.velocity = snapshot.velocity;
    view.yaw = snapshot.yaw;
    view.pitch = snapshot.pitch;
    view.stance = snapshot.stance;
    view.leanAmount = snapshot.leanAmount;
    view.stridePhase = snapshot.stridePhase;
    view.health = snapshot.health;
    view.grounded = snapshot.grounded;
    view.alive = snapshot.alive;
    view.heldItem = snapshot.heldItem;
    view.torchOn = snapshot.torchOn;
    view.aim = snapshot.aim;
    view.reloading = snapshot.reloading;
    view.reloadProgress = snapshot.reloadProgress;
    view.reloadEmpty = snapshot.reloadEmpty;
    view.mantling = snapshot.mantling;
    view.mantlePhase = snapshot.mantlePhase;
    view.mantleEdge = snapshot.mantleEdge;
    view.pingMs = snapshot.pingMs;
    view.heldBy = snapshot.heldBy;
    view.cocooned = snapshot.cocooned;
}

void SendPacket(Transport& transport, PeerId peer, Channel channel, BitWriter& writer)
{
    const std::vector<uint8_t>& bytes = writer.Finish();
    transport.Send(peer, channel, bytes.data(), bytes.size());
}

} // namespace

const char* SessionModeName(SessionMode mode)
{
    switch (mode)
    {
    case SessionMode::Offline: return "offline";
    case SessionMode::Host: return "host";
    case SessionMode::Client: return "client";
    }
    return "unknown";
}

// --- The host ----------------------------------------------------------------------------------

struct NetHost::Client
{
    PeerId peer = kInvalidPeer;
    uint8_t playerId = 0;
    std::string name;
    bool welcomed = false;
    // Inputs the host has been sent but not yet run, oldest first.
    std::vector<InputCommand> pending;
    uint32_t lastProcessed = 0;
    // The buffer has to fill before the first input is run, or the very first late packet leaves
    // the host with nothing to do and the player stutters on their first step.
    bool started = false;
    PlayerInput lastInput;
    PlayerController controller;
    // What they are holding. The host does not simulate their weapon, it only passes on what they
    // say they have out, because from outside that is all anyone can see.
    uint8_t heldItem = 0;
    float aim = 0.0f;
    bool reloading = false;
    float reloadProgress = 0.0f;
    bool reloadEmpty = false;
    bool torchOn = false;
    uint8_t heldBy = kNotHeld;
    bool cocooned = false;
    // What this client has taken out of the world. The host does not model their bag, only what it
    // handed them, which is enough to refuse a drop of something they never picked up.
    std::map<uint16_t, int> carried;
    // Their round trip, smoothed. Every input packet carries back the last tick this client saw,
    // and the host counts its own ticks, so the gap between the two is the round trip without
    // either machine having to carry a clock or agree what time it is. A single reading jumps
    // around by a tick or two depending on where in the client's frame the packet was sent, which
    // on screen is a number that will not sit still, so it is eased rather than shown raw.
    float pingMs = 0.0f;
    bool pingSeen = false;
};

NetHost::NetHost() = default;
NetHost::~NetHost()
{
    Stop();
}

bool NetHost::Start(std::unique_ptr<Transport> transport, const Config& config, PhysicsWorld& physics,
                    const PlayerConfig& playerConfig, const glm::vec3& spawn)
{
    if (transport == nullptr)
    {
        return false;
    }
    if (!transport->Listen(config.port))
    {
        return false;
    }

    m_transport = std::move(transport);
    m_config = config;
    m_physics = &physics;
    m_playerConfig = playerConfig;
    m_spawn = spawn;
    m_snapshotTimer = 0.0f;
    m_starvedTicks = 0;
    m_started = false;
    m_running = true;
    PRED_LOG_INFO(Network, "Hosting on port {} at {} Hz snapshots", config.port, config.snapshotHz);
    return true;
}

void NetHost::Stop()
{
    if (!m_running)
    {
        return;
    }
    for (auto& client : m_clients)
    {
        if (m_transport != nullptr)
        {
            m_transport->Disconnect(client->peer);
        }
        client->controller.Shutdown();
    }
    m_clients.clear();
    m_views.clear();
    m_transport.reset();
    m_running = false;
    PRED_LOG_INFO(Network, "Stopped hosting");
}

size_t NetHost::ConnectedCount() const
{
    return std::count_if(m_clients.begin(), m_clients.end(),
                         [](const std::unique_ptr<Client>& client) { return client->welcomed; });
}

NetHost::Client* NetHost::FindClient(PeerId peer)
{
    const auto found = std::find_if(m_clients.begin(), m_clients.end(),
                                    [&](const std::unique_ptr<Client>& client) { return client->peer == peer; });
    return found == m_clients.end() ? nullptr : found->get();
}

uint8_t NetHost::AllocatePlayerId() const
{
    // Zero is always the host. The lowest free slot is reused so a four-player game never runs out
    // of ids however many times people come and go.
    for (uint8_t id = 1; id < kMaxPlayers; ++id)
    {
        const bool taken = std::any_of(m_clients.begin(), m_clients.end(),
                                       [&](const std::unique_ptr<Client>& client)
                                       { return client->playerId == id; });
        if (!taken)
        {
            return id;
        }
    }
    return kMaxPlayers;
}

void NetHost::SendRejection(PeerId peer, JoinRejection reason)
{
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Rejected);
    RejectedMessage message;
    message.reason = reason;
    WriteRejected(writer, message);
    SendPacket(*m_transport, peer, Channel::Reliable, writer);
}

void NetHost::HandleJoin(PeerId peer, BitReader& reader)
{
    JoinMessage join;
    if (!ReadJoin(reader, join))
    {
        PRED_LOG_WARN(Network, "Malformed join from peer {}", peer);
        return;
    }
    if (join.protocolVersion != kProtocolVersion)
    {
        PRED_LOG_WARN(Network, "Peer {} speaks protocol {}, we speak {}", peer, join.protocolVersion,
                      kProtocolVersion);
        SendRejection(peer, JoinRejection::VersionMismatch);
        return;
    }
    if (FindClient(peer) != nullptr)
    {
        return; // a repeated join, which happens when the first reply was lost
    }

    const uint8_t playerId = AllocatePlayerId();
    if (playerId >= kMaxPlayers)
    {
        SendRejection(peer, JoinRejection::ServerFull);
        return;
    }

    auto client = std::make_unique<Client>();
    client->peer = peer;
    client->playerId = playerId;
    client->name = join.name.empty() ? "operator" : join.name;
    if (!client->controller.Init(*m_physics, m_playerConfig, m_spawn))
    {
        PRED_LOG_ERROR(Network, "Could not create a body for peer {}", peer);
        SendRejection(peer, JoinRejection::ServerFull);
        return;
    }
    client->welcomed = true;

    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Welcome);
    WelcomeMessage welcome;
    welcome.playerId = playerId;
    welcome.tickRate = m_config.tickHz;
    welcome.snapshotRate = m_config.snapshotHz;
    WriteWelcome(writer, welcome);
    SendPacket(*m_transport, peer, Channel::Reliable, writer);

    PRED_LOG_INFO(Network, "{} joined as player {}", client->name, playerId);
    m_clients.push_back(std::move(client));
    // After the new client is on the list, not before: sending it first leaves them out of the
    // roster everybody else keeps, and leaves them with no roster at all.
    BroadcastPeerList();
}

void NetHost::HandlePacket(const NetPacket& packet)
{
    BitReader reader(packet.bytes.data(), packet.bytes.size());
    MessageType type = MessageType::Count;
    if (!ReadMessageHeader(reader, type))
    {
        return;
    }

    switch (type)
    {
    case MessageType::Join:
        HandleJoin(packet.peer, reader);
        break;

    case MessageType::Voice:
    {
        Client* client = FindClient(packet.peer);
        if (client == nullptr || !client->welcomed)
        {
            return;
        }
        VoiceMessage message;
        if (!ReadVoice(reader, message))
        {
            return;
        }
        // Whose voice it is, is the host's to say. A client fills in nothing here and could not be
        // believed if it did: taking its word would let anybody speak as anybody.
        const glm::vec3 from = client->controller.State().position;
        ForwardVoice(client->playerId, message.sequence, message.frame, from);
        // And the host's own ears, if it is close enough to hear it.
        if (glm::distance(m_localPosition, from) <= kVoiceRange)
        {
            VoiceHeard heard;
            heard.speaker = client->playerId;
            heard.sequence = message.sequence;
            heard.frame = std::move(message.frame);
            m_voiceHeard.push_back(std::move(heard));
        }
        return;
    }

    case MessageType::Input:
    {
        Client* client = FindClient(packet.peer);
        if (client == nullptr)
        {
            return; // input before a join is nothing to act on
        }
        InputMessage message;
        if (!ReadInput(reader, message))
        {
            PRED_LOG_WARN(Network, "Malformed input from player {}", client->playerId);
            return;
        }
        for (uint8_t i = 0; i < message.count; ++i)
        {
            const InputCommand& command = message.commands[i];
            // Every packet repeats the last few ticks, so most of what arrives is already known or
            // already run. Only what is genuinely new and genuinely ahead is kept.
            if (client->started && command.sequence <= client->lastProcessed)
            {
                continue;
            }
            const bool known = std::any_of(client->pending.begin(), client->pending.end(),
                                           [&](const InputCommand& held)
                                           { return held.sequence == command.sequence; });
            if (!known)
            {
                client->pending.push_back(command);
            }
        }
        // What they are holding rides along with the input, so the host knows without asking.
        SetPlayerTorch(client->playerId, message.torchOn);
        SetPlayerHeld(client->playerId, message.heldItem, message.aim, message.reloading,
                      message.reloadProgress, message.reloadEmpty);

        // And their round trip, from the tick they echoed back. Unsigned subtraction on sixteen
        // bits gives the right answer across the wrap without being told about it. An echo from a
        // tick the host has not reached is a client guessing or lying, and is thrown away rather
        // than turned into a negative ping.
        {
            const uint16_t now = static_cast<uint16_t>(m_tick & 0xFFFFu);
            const uint16_t elapsed = static_cast<uint16_t>(now - message.ackTick);
            constexpr uint16_t kMaxTicksBehind = 600; // ten seconds at sixty; past that it is noise
            if (elapsed <= kMaxTicksBehind)
            {
                const float sample =
                    static_cast<float>(elapsed) * 1000.0f / std::max<float>(m_config.tickHz, 1.0f);
                client->pingMs = client->pingSeen ? glm::mix(client->pingMs, sample, 0.15f) : sample;
                client->pingSeen = true;
            }
        }

        std::sort(client->pending.begin(), client->pending.end(),
                  [](const InputCommand& a, const InputCommand& b) { return a.sequence < b.sequence; });
        // A client that floods the host with inputs is spending its own bandwidth and gaining
        // nothing: the host still runs one per tick, and the rest are dropped here.
        constexpr size_t kMaxPending = 32;
        if (client->pending.size() > kMaxPending)
        {
            client->pending.erase(client->pending.begin(),
                                  client->pending.end() - static_cast<ptrdiff_t>(kMaxPending));
        }
        break;
    }

    case MessageType::Interact:
    {
        const Client* client = FindClient(packet.peer);
        InteractMessage message;
        if (client == nullptr || !ReadInteract(reader, message))
        {
            return;
        }
        // Queued rather than acted on. Whether the player is close enough to that door is a
        // question about the world, and the world is the game's, not the transport's.
        InteractRequest request;
        request.player = client->playerId;
        request.kind = message.kind;
        request.index = message.index;
        m_interactRequests.push_back(request);
        break;
    }

    case MessageType::Shot:
    {
        const Client* client = FindClient(packet.peer);
        ShotMessage message;
        if (client == nullptr || !ReadShot(reader, message))
        {
            return;
        }
        ShotRequest request;
        request.player = client->playerId;
        request.shot = message;
        m_shotRequests.push_back(request);
        break;
    }

    case MessageType::Ready:
    {
        // Now, not when they were let in. Their world exists now, so what they are told about it
        // will still be there a frame later.
        if (const Client* client = FindClient(packet.peer); client != nullptr)
        {
            m_joined.push_back(client->playerId);
        }
        break;
    }

    case MessageType::Drop:
    {
        const Client* client = FindClient(packet.peer);
        DropMessage message;
        if (client == nullptr || !ReadDrop(reader, message))
        {
            return;
        }
        DropRequest request;
        request.player = client->playerId;
        request.drop = message;
        m_dropRequests.push_back(request);
        break;
    }

    case MessageType::Leave:
        RemoveClient(packet.peer);
        break;

    default:
        break;
    }
}

void NetHost::NoteCarried(uint8_t player, uint16_t item, int count)
{
    for (auto& client : m_clients)
    {
        if (client->playerId == player)
        {
            client->carried[item] += count;
            return;
        }
    }
}

bool NetHost::TakeCarried(uint8_t player, uint16_t item, int count)
{
    for (auto& client : m_clients)
    {
        if (client->playerId != player)
        {
            continue;
        }
        const auto found = client->carried.find(item);
        if (found == client->carried.end() || found->second < count)
        {
            PRED_LOG_WARN(Network, "Player {} tried to put down something they never picked up",
                          player);
            return false;
        }
        found->second -= count;
        return true;
    }
    return false;
}

std::vector<NetHost::PlayerPose> NetHost::PosesAt(uint32_t tick) const
{
    if (m_history.empty())
    {
        return {};
    }

    // Clamped before it is trusted. A client asking to be rewound further than a playable
    // connection could justify is either badly wrong about the time or trying it on, and either way
    // the answer is the same: it gets the oldest rewind anyone is allowed.
    const uint32_t oldest = m_tick > kMaxRewindTicks ? m_tick - kMaxRewindTicks : 0;
    const uint32_t wanted = std::clamp(tick, oldest, m_tick);

    const HistoryEntry* best = &m_history.front();
    uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
    for (const HistoryEntry& entry : m_history)
    {
        const uint32_t distance = entry.tick > wanted ? entry.tick - wanted : wanted - entry.tick;
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = &entry;
        }
    }
    return best->poses;
}

void NetHost::ApplyDamageTo(uint8_t playerId, float amount, const char* cause)
{
    for (auto& client : m_clients)
    {
        if (client->playerId == playerId)
        {
            client->controller.ApplyDamage(amount, cause);
            return;
        }
    }
}

void NetHost::RespawnPlayer(uint8_t playerId, const glm::vec3& position)
{
    for (auto& client : m_clients)
    {
        if (client->playerId == playerId)
        {
            client->controller.Respawn(position);
            return;
        }
    }
}

float NetHost::HealthOf(uint8_t playerId) const
{
    for (const auto& client : m_clients)
    {
        if (client->playerId == playerId)
        {
            return client->controller.State().health;
        }
    }
    return 0.0f;
}


std::vector<NetHost::VoiceHeard> NetHost::TakeVoice()
{
    return std::exchange(m_voiceHeard, {});
}

void NetHost::ForwardVoice(uint8_t speaker, uint16_t sequence, const std::vector<uint8_t>& frame,
                           const glm::vec3& from)
{
    if (frame.empty())
    {
        return;
    }
    VoiceMessage message;
    message.speaker = speaker;
    message.sequence = sequence;
    message.frame = frame;

    BitWriter writer(frame.size() + 8);
    WriteMessageHeader(writer, MessageType::Voice);
    WriteVoice(writer, message);
    const std::vector<uint8_t>& bytes = writer.Finish();

    for (const std::unique_ptr<Client>& client : m_clients)
    {
        if (!client->welcomed || client->playerId == speaker)
        {
            continue;
        }
        // Only to people who could actually hear it. The host owns every player's position, so this
        // is a real distance and not a guess, and a player never receives speech they are not
        // entitled to hear. Forwarding everything and letting each listener attenuate it would work
        // and would also put the whole conversation on every machine.
        const glm::vec3 to = client->controller.State().position;
        if (glm::distance(to, from) > kVoiceRange)
        {
            continue;
        }
        m_transport->Send(client->peer, Channel::Unreliable, bytes.data(), bytes.size());
    }
}

void NetHost::SendVoice(uint16_t sequence, const std::vector<uint8_t>& frame, const glm::vec3& from)
{
    // Player zero is the host. It never sends to itself: it hears its own microphone directly, and
    // hearing yourself a network round trip later is the classic way to make somebody stop talking.
    ForwardVoice(0, sequence, frame, from);
}

void NetHost::BroadcastPeerList()
{
    if (m_transport == nullptr)
    {
        return;
    }
    // Sent whenever the roster changes, and only then. It is what lets the players left find each
    // other if this machine goes: by that point there is nobody to ask.
    PeerListMessage list;
    list.started = m_started;
    // The host goes in first, with no address. Everybody already knows how to reach this machine,
    // so the entry is there for the name rather than for the address: without it a player list on
    // a client has a row it cannot put a name to, which is the row belonging to whoever is running
    // the game.
    {
        PeerEntry& self = list.peers[list.count++];
        self.id = 0;
        self.name = m_config.name;
    }
    for (const auto& client : m_clients)
    {
        if (client->welcomed && list.count < kMaxPlayers)
        {
            PeerEntry& entry = list.peers[list.count++];
            entry.id = client->playerId;
            entry.name = client->name;
            entry.address = m_transport->AddressOf(client->peer);
        }
    }

    BitWriter writer;
    WriteMessageHeader(writer, MessageType::PeerList);
    WritePeerList(writer, list);
    const std::vector<uint8_t>& bytes = writer.Finish();
    for (const auto& client : m_clients)
    {
        if (client->welcomed)
        {
            m_transport->Send(client->peer, Channel::Reliable, bytes.data(), bytes.size());
        }
    }
}

void NetHost::SetStarted(bool started)
{
    if (m_started == started)
    {
        return;
    }
    m_started = started;
    BroadcastPeerList();
}

void NetHost::Broadcast(const WorldEventMessage& event)
{
    if (m_transport == nullptr)
    {
        return;
    }
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::WorldEvent);
    WriteWorldEvent(writer, event);
    const std::vector<uint8_t>& bytes = writer.Finish();
    for (const auto& client : m_clients)
    {
        if (client->welcomed)
        {
            m_transport->Send(client->peer, Channel::Reliable, bytes.data(), bytes.size());
        }
    }
}

void NetHost::SendTo(uint8_t playerId, const WorldEventMessage& event)
{
    if (m_transport == nullptr)
    {
        return;
    }
    for (const auto& client : m_clients)
    {
        if (client->welcomed && client->playerId == playerId)
        {
            BitWriter writer;
            WriteMessageHeader(writer, MessageType::WorldEvent);
            WriteWorldEvent(writer, event);
            const std::vector<uint8_t>& bytes = writer.Finish();
            m_transport->Send(client->peer, Channel::Reliable, bytes.data(), bytes.size());
            return;
        }
    }
}

void NetHost::SendCreatureState(const CreatureStateMessage& state)
{
    // Even with none: an empty message is how a client learns the last one has gone.
    if (m_transport == nullptr)
    {
        return;
    }
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Creatures);
    WriteCreatureState(writer, state);
    const std::vector<uint8_t>& bytes = writer.Finish();
    for (const auto& client : m_clients)
    {
        if (client->welcomed)
        {
            m_transport->Send(client->peer, Channel::Unreliable, bytes.data(), bytes.size());
        }
    }
}

void NetHost::SendWorldState(const WorldStateMessage& state)
{
    if (m_transport == nullptr || state.count == 0)
    {
        return;
    }
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::WorldState);
    WriteWorldState(writer, state);
    const std::vector<uint8_t>& bytes = writer.Finish();
    for (const auto& client : m_clients)
    {
        if (client->welcomed)
        {
            m_transport->Send(client->peer, Channel::Unreliable, bytes.data(), bytes.size());
        }
    }
}

void NetHost::SetPlayerHeld(uint8_t playerId, uint8_t heldItem, float aim, bool reloading,
                            float progress, bool reloadEmpty)
{
    if (playerId == 0)
    {
        // The host is player zero, and its own hands go into the snapshot the same way.
        m_localHeldItem = heldItem;
        m_localAim = aim;
        m_localReloading = reloading;
        m_localReloadProgress = progress;
        m_localReloadEmpty = reloadEmpty;
        return;
    }
    for (auto& client : m_clients)
    {
        if (client->playerId == playerId)
        {
            client->heldItem = heldItem;
            client->aim = aim;
            client->reloading = reloading;
            client->reloadProgress = progress;
            client->reloadEmpty = reloadEmpty;
            return;
        }
    }
}

void NetHost::SetPlayerTorch(uint8_t playerId, bool on)
{
    if (playerId == 0)
    {
        m_localTorch = on;
        return;
    }
    for (auto& client : m_clients)
    {
        if (client->playerId == playerId)
        {
            client->torchOn = on;
            return;
        }
    }
}

void NetHost::PinPlayer(uint8_t playerId, bool pinned, const glm::vec3& feet, float yaw)
{
    if (playerId == 0)
    {
        return; // the host pins its own player through its own controller
    }
    for (auto& client : m_clients)
    {
        if (client->playerId != playerId)
        {
            continue;
        }
        if (pinned)
        {
            client->controller.Attach(feet, yaw);
        }
        else if (client->controller.IsAttached())
        {
            client->controller.Detach(feet);
        }
        return;
    }
}

void NetHost::SetPlayerGrabbed(uint8_t playerId, uint8_t by, bool cocooned, const glm::vec3& feet, float yaw)
{
    if (playerId == 0)
    {
        m_localHeldBy = by;
        m_localCocooned = cocooned && by != kNotHeld;
        return;
    }
    for (auto& client : m_clients)
    {
        if (client->playerId != playerId)
        {
            continue;
        }
        const bool wasHeld = client->heldBy != kNotHeld;
        client->heldBy = by;
        client->cocooned = cocooned && by != kNotHeld;
        if (by != kNotHeld)
        {
            client->controller.Attach(feet, yaw);
        }
        else if (wasHeld)
        {
            client->controller.Detach(feet);
        }
        return;
    }
}

PlayerInput NetHost::LastInputOf(uint8_t playerId) const
{
    for (const auto& client : m_clients)
    {
        if (client->playerId == playerId)
        {
            return client->lastInput;
        }
    }
    return PlayerInput{};
}

void NetHost::RemoveClient(PeerId peer)
{
    const auto found = std::find_if(m_clients.begin(), m_clients.end(),
                                    [&](const std::unique_ptr<Client>& client) { return client->peer == peer; });
    if (found == m_clients.end())
    {
        return;
    }
    PRED_LOG_INFO(Network, "{} left", (*found)->name);

    // What the host handed them goes back to the world, at their feet. It belongs to the world
    // rather than to the connection: a player who logs out holding the keycard otherwise takes it
    // with them and nobody can finish, and what everyone else is left looking at is a body still
    // holding a copy of something that no longer exists.
    Departure departure;
    departure.player = (*found)->playerId;
    departure.position = (*found)->controller.State().position;
    for (const auto& [item, count] : (*found)->carried)
    {
        if (count > 0)
        {
            departure.carried.emplace_back(item, count);
        }
    }
    m_departed.push_back(std::move(departure));

    (*found)->controller.Shutdown();
    m_clients.erase(found);
    BroadcastPeerList();
}

void NetHost::Tick(uint32_t tick, const PlayerState& localState, float dt)
{
    if (!m_running || m_transport == nullptr)
    {
        return;
    }

    m_transport->Poll(dt, m_incoming);
    for (const NetPacket& packet : m_incoming)
    {
        HandlePacket(packet);
    }
    m_transport->TakeConnected(); // a peer is not a player until it says who it is
    for (const PeerId peer : m_transport->TakeDisconnected())
    {
        RemoveClient(peer);
    }

    for (auto& client : m_clients)
    {
        if (!client->welcomed)
        {
            continue;
        }

        if (!client->started && client->pending.size() > m_config.inputBufferTicks)
        {
            client->started = true;
        }

        if (client->started && !client->pending.empty())
        {
            // One input a tick, and more when the queue has grown past what it is meant to hold.
            //
            // Exactly one a tick meant the queue could only ever grow: a burst of packets, or a few
            // ticks filled in with a repeated input while nothing arrived, and every input after that
            // ran that much later for the rest of the game. On the host that player fell behind and
            // stayed behind -- their body walking a beat after their own screen, further after every
            // hiccup -- while nothing on their own machine looked wrong. Two extra at most a tick, so
            // catching up is quick without being a jump.
            int steps = 1;
            const size_t target = static_cast<size_t>(m_config.inputBufferTicks) + 1;
            if (client->pending.size() > target)
            {
                steps += std::min<int>(2, static_cast<int>(client->pending.size() - target));
            }
            for (int i = 0; i < steps && !client->pending.empty(); ++i)
            {
                const InputCommand command = client->pending.front();
                client->pending.erase(client->pending.begin());
                client->lastProcessed = command.sequence;
                client->lastInput = command.input;
                client->controller.Step(command.input, dt);
            }
        }
        else if (client->started)
        {
            // Nothing arrived in time. Repeat the last input so the player keeps moving instead of
            // stopping dead, but never repeat a jump: one press has to mean one jump.
            PlayerInput repeated = client->lastInput;
            repeated.jump = false;
            client->controller.Step(repeated, dt);
            ++m_starvedTicks;
        }
    }

    // Hosting on your own costs the socket and nothing else.
    //
    // Everything below here exists to tell other machines what happened, and with nobody connected
    // there is nobody to tell: the views describe an empty table, the rewind history is a second of
    // one player's own position that only a client's shot would ever look at, and the snapshots go
    // nowhere. Opening a game and then playing alone used to run all of it anyway, every tick,
    // which is a server doing a server's work for an audience of none. The socket stays open,
    // because that is what somebody joining knocks on.
    if (m_clients.empty())
    {
        m_tick = tick;
        m_history.clear();
        m_views.clear();
        m_snapshotTimer = 0.0f;
        return;
    }

    BuildViews(localState);

    // Where everybody is, kept for a second, so a shot can be tested against where the shooter saw
    // them rather than where they have got to since.
    m_tick = tick;
    HistoryEntry entry;
    entry.tick = tick;
    m_localPosition = localState.position;
    entry.poses.push_back({0, localState.position, localState.stance, localState.alive});
    for (const auto& client : m_clients)
    {
        if (client->welcomed)
        {
            const PlayerState& state = client->controller.State();
            entry.poses.push_back({client->playerId, state.position, state.stance, state.alive});
        }
    }
    m_history.push_back(std::move(entry));
    if (m_history.size() > kHistoryTicks)
    {
        m_history.erase(m_history.begin());
    }

    const float interval = 1.0f / std::max<float>(m_config.snapshotHz, 1);
    m_snapshotTimer += dt;
    if (m_snapshotTimer >= interval)
    {
        m_snapshotTimer = std::fmod(m_snapshotTimer, interval);
        SendSnapshots(tick, localState);
    }
}

void NetHost::BuildViews(const PlayerState& localState)
{
    (void)localState;
    m_views.clear();
    for (const auto& client : m_clients)
    {
        if (!client->welcomed)
        {
            continue;
        }
        RemotePlayerView view;
        ApplySnapshot(view, SnapshotOf(client->playerId, client->controller.State()));
        view.name = client->name;
        // What they are holding is not part of the state the host simulates for them, it is what
        // they told us, so it has to be put back in here. Leaving it out is why the host never saw
        // anybody else holding anything, and why rounds from a client left their face on the host's
        // screen: the muzzle is looked up from the weapon being drawn for them, and there was none.
        view.heldItem = client->heldItem;
        view.torchOn = client->torchOn;
        view.heldBy = client->heldBy;
        view.cocooned = client->cocooned;
        view.aim = client->aim;
        view.reloading = client->reloading;
        view.reloadProgress = client->reloadProgress;
        view.reloadEmpty = client->reloadEmpty;
        view.pingMs = static_cast<uint16_t>(std::lround(client->pingMs));
        m_views.push_back(std::move(view));
    }
}

void NetHost::SendSnapshots(uint32_t tick, const PlayerState& localState)
{
    SnapshotMessage snapshot;
    snapshot.tick = tick;
    snapshot.players[0] = SnapshotOf(0, localState);
    snapshot.players[0].heldItem = m_localHeldItem;
    snapshot.players[0].aim = m_localAim;
    snapshot.players[0].reloading = m_localReloading;
    snapshot.players[0].reloadProgress = m_localReloadProgress;
    snapshot.players[0].reloadEmpty = m_localReloadEmpty;
    snapshot.players[0].torchOn = m_localTorch;
    snapshot.players[0].heldBy = m_localHeldBy;
    snapshot.players[0].cocooned = m_localCocooned;
    snapshot.count = 1;
    for (const auto& client : m_clients)
    {
        if (client->welcomed && snapshot.count < kMaxPlayers)
        {
            PlayerSnapshot& entry = snapshot.players[snapshot.count++];
            entry = SnapshotOf(client->playerId, client->controller.State());
            entry.heldItem = client->heldItem;
            entry.aim = client->aim;
            entry.reloading = client->reloading;
            entry.reloadProgress = client->reloadProgress;
            entry.reloadEmpty = client->reloadEmpty;
            entry.torchOn = client->torchOn;
            entry.heldBy = client->heldBy;
            entry.cocooned = client->cocooned;
            entry.pingMs = static_cast<uint16_t>(std::lround(client->pingMs));
        }
    }

    // One snapshot per client rather than one broadcast, because each has to be told which of its
    // own inputs the host has run. That single number is what lets a client know which of its
    // guesses are settled and which it still has to replay.
    for (const auto& client : m_clients)
    {
        if (!client->welcomed)
        {
            continue;
        }
        snapshot.lastProcessedInput = client->lastProcessed;
        BitWriter writer;
        WriteMessageHeader(writer, MessageType::Snapshot);
        WriteSnapshot(writer, snapshot);
        SendPacket(*m_transport, client->peer, Channel::Unreliable, writer);
    }
}

// --- The client --------------------------------------------------------------------------------

NetClient::NetClient() = default;
NetClient::~NetClient()
{
    Disconnect();
}

bool NetClient::Connect(std::unique_ptr<Transport> transport, const std::string& address, uint16_t port,
                        const std::string& name, const Config& config)
{
    if (transport == nullptr || !transport->Connect(address, port))
    {
        return false;
    }

    m_transport = std::move(transport);
    m_config = config;
    m_name = name;
    m_welcomed = false;
    m_rejection = JoinRejection::None;
    m_sequence = 0;
    m_lastAcknowledged = 0;
    m_corrections = 0;
    m_clock = 0.0f;
    m_sessionPort = port;
    m_hostLost = false;
    m_peers.clear();
    m_hostStarted = false;
    m_visualError = glm::vec3(0.0f);
    m_history.Clear();
    m_snapshots.clear();
    m_views.clear();
    m_joinTimer = 0.0f;
    // The last session's creatures are not this one's, and its sequence numbers mean nothing here.
    m_creatureState = CreatureStateMessage{};
    m_creatureStatesReceived = 0;

    // The join is not sent here. Reaching a host takes a handshake of its own first, and a message
    // handed to a transport that has no connection yet goes nowhere: it is sent from Tick once the
    // transport says the peer exists.
    PRED_LOG_INFO(Network, "Joining {}:{} as {}", address, port, name);
    return true;
}

void NetClient::SendJoin()
{
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Join);
    JoinMessage join;
    join.name = m_name;
    WriteJoin(writer, join);
    SendPacket(*m_transport, kHostPeer, Channel::Reliable, writer);
}

void NetClient::Disconnect()
{
    if (m_transport == nullptr)
    {
        return;
    }
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Leave);
    SendPacket(*m_transport, kHostPeer, Channel::Reliable, writer);
    m_transport->Disconnect(kHostPeer);
    m_transport.reset();
    m_welcomed = false;
    m_history.Clear();
    m_snapshots.clear();
    m_views.clear();
}

void NetClient::HandlePacket(const NetPacket& packet)
{
    BitReader reader(packet.bytes.data(), packet.bytes.size());
    MessageType type = MessageType::Count;
    if (!ReadMessageHeader(reader, type))
    {
        return;
    }

    switch (type)
    {
    case MessageType::Welcome:
    {
        WelcomeMessage welcome;
        if (!ReadWelcome(reader, welcome))
        {
            return;
        }
        m_playerId = welcome.playerId;
        m_welcomed = true;
        PRED_LOG_INFO(Network, "Joined as player {}", m_playerId);
        break;
    }

    case MessageType::Rejected:
    {
        RejectedMessage rejected;
        if (!ReadRejected(reader, rejected))
        {
            return;
        }
        m_rejection = rejected.reason;
        PRED_LOG_WARN(Network, "Join refused, reason {}", static_cast<int>(rejected.reason));
        break;
    }

    case MessageType::Snapshot:
    {
        SnapshotRecord record;
        record.time = m_clock;
        if (!ReadSnapshot(reader, record.message))
        {
            return;
        }
        // Snapshots can overtake each other on the way here. An older one arriving after a newer
        // one is stale by definition and there is nothing in it worth having.
        if (!m_snapshots.empty() && record.message.tick <= m_snapshots.back().message.tick)
        {
            return;
        }
        m_lastSnapshotTick = record.message.tick;
        // This machine's own row carries the round trip the host measured for it. Nobody can time
        // their own connection from one end, so it is read back rather than worked out here.
        for (uint8_t i = 0; i < record.message.count; ++i)
        {
            if (record.message.players[i].playerId == m_playerId)
            {
                m_pingMs = record.message.players[i].pingMs;
                break;
            }
        }
        m_snapshots.push_back(std::move(record));
        m_snapshotArrived = true;
        constexpr size_t kKeep = 16;
        if (m_snapshots.size() > kKeep)
        {
            m_snapshots.erase(m_snapshots.begin());
        }
        break;
    }

    case MessageType::PeerList:
    {
        PeerListMessage list;
        if (ReadPeerList(reader, list))
        {
            m_hostStarted = list.started;
            m_peers.clear();
            for (uint8_t i = 0; i < list.count; ++i)
            {
                m_peers.push_back({list.peers[i].id, list.peers[i].name, list.peers[i].address});
            }
        }
        break;
    }

    case MessageType::WorldEvent:
    {
        WorldEventMessage event;
        if (ReadWorldEvent(reader, event))
        {
            // Queued rather than applied here. What a door or a locker is belongs to the game; this
            // only knows that one changed.
            m_worldEvents.push_back(event);
        }
        break;
    }

    case MessageType::WorldState:
    {
        WorldStateMessage state;
        if (ReadWorldState(reader, state))
        {
            m_worldState = state;
            m_hasWorldState = true;
        }
        break;
    }

    case MessageType::Creatures:
    {
        CreatureStateMessage state;
        if (!ReadCreatureState(reader, state))
        {
            break;
        }
        // Older than the one already held means it was overtaken on the way. The difference is
        // taken as signed so the count wrapping round from 65535 to 0 still reads as newer.
        const auto ahead = static_cast<int16_t>(static_cast<uint16_t>(state.sequence - m_creatureState.sequence));
        if (m_creatureStatesReceived == 0 || ahead > 0)
        {
            m_creatureState = state;
            ++m_creatureStatesReceived;
        }
        break;
    }

    case MessageType::Voice:
    {
        VoiceMessage message;
        if (!ReadVoice(reader, message) || message.speaker == m_playerId)
        {
            // Never our own voice back. The host does not send it, but a client that played it
            // would hear itself a round trip late, which is the classic way to stop somebody
            // talking mid-sentence.
            break;
        }
        VoiceHeard heard;
        heard.speaker = message.speaker;
        heard.sequence = message.sequence;
        heard.frame = std::move(message.frame);
        m_voiceIn.push_back(std::move(heard));
        break;
    }

    default:
        break;
    }
}

void NetClient::SendInteract(uint8_t kind, uint8_t index)
{
    if (m_transport == nullptr || !m_welcomed)
    {
        return;
    }
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Interact);
    InteractMessage message;
    message.kind = kind;
    message.index = index;
    WriteInteract(writer, message);
    // Reliable: opening a door is a thing that happens once, and a lost request is a door that
    // never opens rather than a frame that looks slightly wrong.
    SendPacket(*m_transport, kHostPeer, Channel::Reliable, writer);
}

void NetClient::SendReady()
{
    if (m_transport == nullptr || !m_welcomed)
    {
        return;
    }
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Ready);
    SendPacket(*m_transport, kHostPeer, Channel::Reliable, writer);
}


void NetClient::SendVoice(uint16_t sequence, const std::vector<uint8_t>& frame)
{
    if (m_transport == nullptr || !Connected() || frame.empty())
    {
        return;
    }
    VoiceMessage message;
    // The speaker is left at zero: the host fills in who this came from, because it knows and
    // because a client that could name the speaker could name somebody else.
    message.sequence = sequence;
    message.frame = frame;

    BitWriter writer(frame.size() + 8);
    WriteMessageHeader(writer, MessageType::Voice);
    WriteVoice(writer, message);
    SendPacket(*m_transport, kHostPeer, Channel::Unreliable, writer);
}

std::vector<NetClient::VoiceHeard> NetClient::TakeVoice()
{
    return std::exchange(m_voiceIn, {});
}

void NetClient::SendDrop(const DropMessage& drop)
{
    if (m_transport == nullptr || !m_welcomed)
    {
        return;
    }
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Drop);
    WriteDrop(writer, drop);
    SendPacket(*m_transport, kHostPeer, Channel::Reliable, writer);
}

void NetClient::SendShot(const ShotMessage& shot)
{
    if (m_transport == nullptr || !m_welcomed)
    {
        return;
    }
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Shot);
    WriteShot(writer, shot);
    SendPacket(*m_transport, kHostPeer, Channel::Reliable, writer);
}

void NetClient::Tick(const PlayerInput& input, PlayerController& local, float dt)
{
    if (m_transport == nullptr)
    {
        return;
    }

    m_snapshotArrived = false;
    m_transport->Poll(dt, m_incoming);
    for (const NetPacket& packet : m_incoming)
    {
        HandlePacket(packet);
    }
    if (!m_transport->TakeDisconnected().empty())
    {
        // Only a host that was actually reached can be lost. A connection that never came up is a
        // failure to join, and a client that never joined knows nothing about who else is playing:
        // treating that as a lost host had every one of them declare itself the new one.
        const bool wasInGame = m_welcomed;
        m_welcomed = false;
        m_transport.reset();
        m_hostLost = wasInGame;
        PRED_LOG_WARN(Network, "Lost the connection to the host");
        return;
    }

    if (!m_welcomed)
    {
        // Ask to be let in as soon as there is a connection to ask over, and keep asking. The join
        // goes on the reliable channel so the transport resends it, but a host that was still
        // starting up would have nothing to resend to.
        m_transport->TakeConnected();
        if (!m_transport->Peers().empty())
        {
            m_joinTimer -= dt;
            if (m_joinTimer <= 0.0f)
            {
                SendJoin();
                m_joinTimer = 1.0f;
            }
        }
        return; // nothing to predict until the host says who we are
    }

    // Settle up with the host before guessing again, so the replay starts from the truth.
    if (m_snapshotArrived && !m_snapshots.empty())
    {
        Reconcile(m_snapshots.back().message, local, dt);
    }

    ++m_sequence;
    local.Step(input, dt);
    m_history.Record(m_sequence, input, local.State());
    SendInput();
}

bool NetClient::ShouldBecomeHost() const
{
    if (!m_hostLost)
    {
        return false;
    }
    // The lowest surviving player number takes over. Everyone was given the same roster, so
    // everyone reaches the same answer without having to agree on one, which is just as well
    // because the machine they would have agreed through is the one that left.
    for (const KnownPeer& peer : m_peers)
    {
        // The host is on the roster too, for its name. It is also the machine that has just gone,
        // so it is never a candidate to take over from itself: counting it here would mean nobody
        // ever decided they were next and the game simply stopped.
        if (peer.id != 0 && peer.id < m_playerId)
        {
            return false;
        }
    }
    return true;
}

std::string NetClient::NameOf(uint8_t id) const
{
    for (const KnownPeer& peer : m_peers)
    {
        if (peer.id == id)
        {
            return peer.name;
        }
    }
    // Somebody who has arrived since the last roster went out. A number is a poor name but it is
    // better than a blank row, and the roster is a reliable message so it is a moment behind at
    // worst.
    return "player " + std::to_string(static_cast<int>(id) + 1);
}

std::string NetClient::SuccessorAddress() const
{
    if (!m_hostLost)
    {
        return {};
    }
    const KnownPeer* best = nullptr;
    for (const KnownPeer& peer : m_peers)
    {
        if (peer.id == m_playerId || peer.address.empty())
        {
            continue;
        }
        if (best == nullptr || peer.id < best->id)
        {
            best = &peer;
        }
    }
    return best != nullptr ? best->address : std::string{};
}

void NetClient::SetTorch(bool on)
{
    m_torchOn = on;
}

void NetClient::SetHeld(uint8_t heldItem, float aim, bool reloading, float progress, bool reloadEmpty)
{
    m_heldItem = heldItem;
    m_heldAim = aim;
    m_heldReloading = reloading;
    m_heldReloadProgress = progress;
    m_heldReloadEmpty = reloadEmpty;
}

void NetClient::SendInput()
{
    const std::vector<PredictedTick> recent = m_history.After(m_lastAcknowledged);
    if (recent.empty())
    {
        return;
    }

    InputMessage message;
    const size_t count = std::min<size_t>(recent.size(), kInputRedundancy);
    const size_t first = recent.size() - count;
    for (size_t i = 0; i < count; ++i)
    {
        message.commands[i].sequence = recent[first + i].sequence;
        message.commands[i].input = recent[first + i].input;
    }
    message.count = static_cast<uint8_t>(count);
    message.heldItem = m_heldItem;
    message.aim = m_heldAim;
    message.reloading = m_heldReloading;
    message.reloadProgress = m_heldReloadProgress;
    message.reloadEmpty = m_heldReloadEmpty;
    message.torchOn = m_torchOn;
    // The last host tick this machine has seen, handed straight back so the host can time the round
    // trip against its own tick counter. Nothing here has to know what the time is.
    message.ackTick = static_cast<uint16_t>(m_lastSnapshotTick & 0xFFFFu);

    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Input);
    WriteInput(writer, message);
    SendPacket(*m_transport, kHostPeer, Channel::Unreliable, writer);
}

void NetClient::Reconcile(const SnapshotMessage& snapshot, PlayerController& local, float dt)
{
    m_lastReconciliation = ReconciliationResult{};

    // Held by a creature: the host has them, and says where they are. There is nothing to guess -- their
    // legs are not taking them anywhere -- so the guessing stops until they are let go.
    for (uint8_t i = 0; i < snapshot.count; ++i)
    {
        const PlayerSnapshot& entry = snapshot.players[i];
        if (entry.playerId != m_playerId)
        {
            continue;
        }
        if (entry.heldBy != kNotHeld)
        {
            local.Attach(entry.position, entry.yaw);
            m_heldByHost = true;
            m_cocoonedByHost = entry.cocooned;
            m_history.Clear();
            m_lastAcknowledged = std::max(m_lastAcknowledged, snapshot.lastProcessedInput);
            return;
        }
        if (m_heldByHost)
        {
            m_heldByHost = false;
            local.Detach(entry.position);
            m_cocoonedByHost = false;
            m_history.Clear();
            m_lastAcknowledged = std::max(m_lastAcknowledged, snapshot.lastProcessedInput);
            return;
        }
        break;
    }

    if (snapshot.lastProcessedInput <= m_lastAcknowledged)
    {
        return; // the host has not run anything new for us
    }

    const PlayerSnapshot* mine = nullptr;
    for (uint8_t i = 0; i < snapshot.count; ++i)
    {
        if (snapshot.players[i].playerId == m_playerId)
        {
            mine = &snapshot.players[i];
            break;
        }
    }
    if (mine == nullptr)
    {
        return;
    }

    const PredictedTick* predicted = m_history.Find(snapshot.lastProcessedInput);
    if (predicted == nullptr)
    {
        // The host answered about a tick this client no longer remembers, which means the round
        // trip is longer than two seconds or the client has just joined. Take the host state and
        // start guessing again from there.
        PlayerState state = local.State();
        state.position = mine->position;
        state.velocity = mine->velocity;
        state.stance = mine->stance;
        state.health = mine->health;
        local.RestoreState(state);
        m_history.Clear();
        m_lastAcknowledged = snapshot.lastProcessedInput;
        m_lastReconciliation.corrected = true;
        m_lastReconciliation.snapped = true;
        ++m_corrections;
        return;
    }

    const float error = glm::length(mine->position - predicted->state.position);
    m_lastReconciliation.errorDistance = error;

    if (error > m_config.reconciliation.positionTolerance)
    {
        const glm::vec3 wasAt = local.State().position;

        // The host is right about where the body is; the client keeps its own timers and gait,
        // because those are its own bookkeeping and resetting them would turn a small positional
        // disagreement into a visible stumble.
        PlayerState corrected = predicted->state;
        corrected.position = mine->position;
        corrected.velocity = mine->velocity;
        corrected.stance = mine->stance;
        corrected.grounded = mine->grounded;
        corrected.health = mine->health;
        corrected.alive = mine->alive;
        local.RestoreState(corrected);

        // Replay everything the host had not seen when it sent this.
        const std::vector<PredictedTick> replay = m_history.After(snapshot.lastProcessedInput);
        for (const PredictedTick& tick : replay)
        {
            local.Step(tick.input, dt);
            m_history.UpdateState(tick.sequence, local.State());
        }

        m_lastReconciliation.corrected = true;
        m_lastReconciliation.replayedTicks = static_cast<uint32_t>(replay.size());
        ++m_corrections;

        // Carry the difference as a drawing offset and walk it off over a few frames. The
        // simulation is already correct; this only stops the camera twitching.
        m_visualError += wasAt - local.State().position;
        if (glm::length(m_visualError) > m_config.reconciliation.snapDistance)
        {
            m_visualError = glm::vec3(0.0f);
            m_lastReconciliation.snapped = true;
        }
    }

    m_lastAcknowledged = snapshot.lastProcessedInput;
    m_history.DropTo(snapshot.lastProcessedInput);
}

void NetClient::UpdateInterpolation(float frameDeltaSeconds)
{
    m_clock += frameDeltaSeconds;

    const float decay = std::exp(-m_config.reconciliation.errorDecayRate * frameDeltaSeconds);
    m_visualError *= decay;
    if (glm::length(m_visualError) < 1e-4f)
    {
        m_visualError = glm::vec3(0.0f);
    }

    if (m_snapshots.empty())
    {
        m_views.clear();
        return;
    }

    // Draw everyone else slightly in the past, between the two snapshots that bracket that moment.
    // Extrapolating instead would guess, and a guess about a body that has just stopped or turned
    // is worse than being sixty milliseconds late.
    const float renderTime = m_clock - m_config.interpolationDelay;

    const SnapshotRecord* older = nullptr;
    const SnapshotRecord* newer = nullptr;
    for (size_t i = 0; i + 1 < m_snapshots.size(); ++i)
    {
        if (m_snapshots[i].time <= renderTime && m_snapshots[i + 1].time >= renderTime)
        {
            older = &m_snapshots[i];
            newer = &m_snapshots[i + 1];
            break;
        }
    }

    m_views.clear();
    if (older == nullptr || newer == nullptr)
    {
        // Not enough history yet, or the connection has stalled and nothing new has arrived. Show
        // the most recent truth rather than nothing.
        const SnapshotMessage& latest = m_snapshots.back().message;
        m_renderTick = latest.tick;
        for (uint8_t i = 0; i < latest.count; ++i)
        {
            if (latest.players[i].playerId == m_playerId)
            {
                continue;
            }
            RemotePlayerView view;
            ApplySnapshot(view, latest.players[i]);
            view.name = NameOf(view.id);
            m_views.push_back(view);
        }
        return;
    }

    const float span = std::max(newer->time - older->time, 1e-5f);
    const float t = std::clamp((renderTime - older->time) / span, 0.0f, 1.0f);

    // Which host tick is on screen right now. A shot carries this up, so the host can test it
    // against the world as it looked to whoever fired rather than as it is.
    m_renderTick = static_cast<uint32_t>(
        glm::mix(static_cast<float>(older->message.tick), static_cast<float>(newer->message.tick), t));

    for (uint8_t i = 0; i < newer->message.count; ++i)
    {
        const PlayerSnapshot& to = newer->message.players[i];
        if (to.playerId == m_playerId)
        {
            continue;
        }

        RemotePlayerView view;
        ApplySnapshot(view, to);
        // Names never cross in a snapshot: they arrive once on the roster and are matched up here.
        // A snapshot goes out thirty times a second and a name does not change, so sending one with
        // every body would be most of the packet.
        view.name = NameOf(view.id);

        for (uint8_t j = 0; j < older->message.count; ++j)
        {
            const PlayerSnapshot& from = older->message.players[j];
            if (from.playerId != to.playerId)
            {
                continue;
            }
            view.position = glm::mix(from.position, to.position, t);
            view.velocity = glm::mix(from.velocity, to.velocity, t);
            view.yaw = LerpAngle(from.yaw, to.yaw, t);
            view.pitch = glm::mix(from.pitch, to.pitch, t);
            view.leanAmount = glm::mix(from.leanAmount, to.leanAmount, t);
            // The stride phase wraps at one, so it interpolates the same way an angle does: take
            // the shorter way round, or a foot crossing the end of its cycle would walk backwards
            // through the whole stride to get to the next step.
            const float phaseDelta = to.stridePhase - from.stridePhase;
            const float shortest = phaseDelta - std::floor(phaseDelta + 0.5f);
            float phase = std::fmod(from.stridePhase + shortest * t, 1.0f);
            view.stridePhase = phase < 0.0f ? phase + 1.0f : phase;
            // Stance is a choice, not a number: it changes at a moment rather than gradually.
            view.stance = t < 0.5f ? from.stance : to.stance;
            break;
        }

        m_views.push_back(view);
    }
}

} // namespace pred
