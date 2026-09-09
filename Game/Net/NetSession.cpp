#include "Game/Net/NetSession.h"

#include "Engine/Core/Log.h"
#include "Game/Player/PlayerController.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
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

    case MessageType::Leave:
        RemoveClient(packet.peer);
        break;

    default:
        break;
    }
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
    (*found)->controller.Shutdown();
    m_clients.erase(found);
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
            const InputCommand command = client->pending.front();
            client->pending.erase(client->pending.begin());
            client->lastProcessed = command.sequence;
            client->lastInput = command.input;
            client->controller.Step(command.input, dt);
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

    BuildViews(localState);

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
        m_views.push_back(std::move(view));
    }
}

void NetHost::SendSnapshots(uint32_t tick, const PlayerState& localState)
{
    SnapshotMessage snapshot;
    snapshot.tick = tick;
    snapshot.players[0] = SnapshotOf(0, localState);
    snapshot.count = 1;
    for (const auto& client : m_clients)
    {
        if (client->welcomed && snapshot.count < kMaxPlayers)
        {
            snapshot.players[snapshot.count++] = SnapshotOf(client->playerId, client->controller.State());
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
    m_visualError = glm::vec3(0.0f);
    m_history.Clear();
    m_snapshots.clear();
    m_views.clear();
    m_joinTimer = 0.0f;

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
        m_snapshots.push_back(std::move(record));
        m_snapshotArrived = true;
        constexpr size_t kKeep = 16;
        if (m_snapshots.size() > kKeep)
        {
            m_snapshots.erase(m_snapshots.begin());
        }
        break;
    }

    default:
        break;
    }
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
        m_welcomed = false;
        m_transport.reset();
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

    BitWriter writer;
    WriteMessageHeader(writer, MessageType::Input);
    WriteInput(writer, message);
    SendPacket(*m_transport, kHostPeer, Channel::Unreliable, writer);
}

void NetClient::Reconcile(const SnapshotMessage& snapshot, PlayerController& local, float dt)
{
    m_lastReconciliation = ReconciliationResult{};

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
        for (uint8_t i = 0; i < latest.count; ++i)
        {
            if (latest.players[i].playerId == m_playerId)
            {
                continue;
            }
            RemotePlayerView view;
            ApplySnapshot(view, latest.players[i]);
            m_views.push_back(view);
        }
        return;
    }

    const float span = std::max(newer->time - older->time, 1e-5f);
    const float t = std::clamp((renderTime - older->time) / span, 0.0f, 1.0f);

    for (uint8_t i = 0; i < newer->message.count; ++i)
    {
        const PlayerSnapshot& to = newer->message.players[i];
        if (to.playerId == m_playerId)
        {
            continue;
        }

        RemotePlayerView view;
        ApplySnapshot(view, to);

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
