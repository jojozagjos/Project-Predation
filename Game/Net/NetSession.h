#pragma once

#include "Engine/Net/Transport.h"
#include "Game/Net/Prediction.h"
#include "Game/Net/Protocol.h"
#include "Game/Player/PlayerTypes.h"

#include <glm/vec3.hpp>

#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace pred
{

class PhysicsWorld;
class PlayerController;

enum class SessionMode : uint8_t
{
    Offline,
    Host,
    Client
};

const char* SessionModeName(SessionMode mode);

// Somebody else, as this machine knows them. Enough to draw and animate a body and nothing more:
// a remote player is never simulated here, only shown.
struct RemotePlayerView
{
    uint8_t id = 0;
    std::string name;
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

    // What they are carrying and what their hands are doing with it.
    uint8_t heldItem = 0;
    bool aiming = false;
    bool reloading = false;
    float reloadProgress = 0.0f;
};

// --- The host ----------------------------------------------------------------------------------

// Runs the real simulation for everybody. A client's packet is a request, never a result: the host
// runs the same movement code against its own physics world and what comes out is what happened.
// Nothing a client sends is written into the world directly.
class NetHost
{
public:
    struct Config
    {
        uint16_t port = kDefaultPort;
        uint16_t snapshotHz = 30; // how often clients hear back
        uint16_t tickHz = 60;
        // How many ticks of input to hold before running any. A packet that arrives a little late
        // then still has something in front of it, and the player does not stutter. Too large and
        // every client is playing further in the past than they need to.
        uint8_t inputBufferTicks = 2;
    };

    NetHost();
    ~NetHost();
    NetHost(const NetHost&) = delete;
    NetHost& operator=(const NetHost&) = delete;

    bool Start(std::unique_ptr<Transport> transport, const Config& config, PhysicsWorld& physics,
               const PlayerConfig& playerConfig, const glm::vec3& spawn);
    void Stop();
    bool Running() const { return m_running; }

    // Once per fixed tick, after the local player has been stepped. The host is also a player.
    void Tick(uint32_t tick, const PlayerState& localState, float dt);

    // --- The world -------------------------------------------------------------------------------
    //
    // The host owns every door, locker, crate and loose object. A client asks; it never acts. These
    // hand the requests to the game, which is what knows whether a door is within reach, and carry
    // the results back out to everybody.

    struct InteractRequest
    {
        uint8_t player = 0;
        uint8_t kind = 0;
        uint8_t index = 0;
    };

    struct ShotRequest
    {
        uint8_t player = 0;
        ShotMessage shot;
    };

    // Where somebody was, as opposed to where they are.
    struct PlayerPose
    {
        uint8_t id = 0;
        glm::vec3 position{0.0f};
        PlayerStance stance = PlayerStance::Standing;
        bool alive = true;
    };

    // Everyone's position at a tick in the recent past, for deciding what a shot hit.
    //
    // A client draws other players a fixed delay behind the newest snapshot, and that snapshot took
    // a one-way trip to get there. It therefore aims at where somebody was, not where they are, and
    // testing a shot against the present punishes a player for their own connection: at a hundred
    // milliseconds that is a metre of lead on a running target. Rewinding is the only way a shot
    // that looked like a hit is one.
    //
    // Bounded on purpose. A client that claims to have been looking a long way into the past would
    // otherwise get to shoot people where they used to be, which is the obvious way to cheat with
    // this, so the rewind is clamped to what a playable connection can justify.
    std::vector<PlayerPose> PosesAt(uint32_t tick) const;
    uint32_t CurrentTick() const { return m_tick; }

    std::vector<InteractRequest> TakeInteractRequests() { return std::exchange(m_interactRequests, {}); }
    std::vector<ShotRequest> TakeShotRequests() { return std::exchange(m_shotRequests, {}); }
    // Players who have just been let in. The game sends them the state of the world.
    std::vector<uint8_t> TakeJoined() { return std::exchange(m_joined, {}); }

    void Broadcast(const WorldEventMessage& event);
    void SendTo(uint8_t playerId, const WorldEventMessage& event);
    void SendWorldState(const WorldStateMessage& state);

    // What a client is holding and doing with it, so everyone sees the right thing in their hands.
    void SetPlayerHeld(uint8_t playerId, uint8_t heldItem, bool aiming, bool reloading, float progress);

    Transport* GetTransport() { return m_transport.get(); }
    const std::vector<RemotePlayerView>& Remotes() const { return m_views; }
    size_t ConnectedCount() const;
    // Ticks a client had no input for and the host had to repeat the last one. A rising number
    // means that client is losing packets or its clock has drifted.
    uint32_t StarvedTicks() const { return m_starvedTicks; }

private:
    struct Client;

    void HandlePacket(const NetPacket& packet);
    void HandleJoin(PeerId peer, BitReader& reader);
    void SendRejection(PeerId peer, JoinRejection reason);
    void RemoveClient(PeerId peer);
    Client* FindClient(PeerId peer);
    void BuildViews(const PlayerState& localState);
    void SendSnapshots(uint32_t tick, const PlayerState& localState);
    uint8_t AllocatePlayerId() const;

    std::unique_ptr<Transport> m_transport;
    std::vector<std::unique_ptr<Client>> m_clients;
    std::vector<RemotePlayerView> m_views;
    std::vector<NetPacket> m_incoming;
    std::vector<InteractRequest> m_interactRequests;
    std::vector<ShotRequest> m_shotRequests;
    struct HistoryEntry
    {
        uint32_t tick = 0;
        std::vector<PlayerPose> poses;
    };
    // A second of it. Longer than any connection worth rewinding for, and short enough that the
    // whole thing is a few kilobytes.
    static constexpr size_t kHistoryTicks = 60;
    // How far back a client is allowed to claim it was looking. A quarter of a second covers a bad
    // connection plus the interpolation delay; past that the claim is refused rather than believed.
    static constexpr uint32_t kMaxRewindTicks = 15;
    std::vector<HistoryEntry> m_history;
    uint32_t m_tick = 0;
    std::vector<uint8_t> m_joined;
    Config m_config;
    PhysicsWorld* m_physics = nullptr;
    PlayerConfig m_playerConfig;
    glm::vec3 m_spawn{0.0f};
    float m_snapshotTimer = 0.0f;
    uint32_t m_starvedTicks = 0;
    uint8_t m_localHeldItem = 0;
    bool m_localAiming = false;
    bool m_localReloading = false;
    float m_localReloadProgress = 0.0f;
    bool m_running = false;
};

// --- The client --------------------------------------------------------------------------------

// Predicts its own movement and interpolates everyone else.
//
// The local player is run immediately from local input, because waiting a round trip for the host
// to agree would make the controls feel dead. Everyone else is drawn a fixed delay behind the
// newest snapshot, between the two that bracket that moment, because extrapolating a body that has
// stopped or turned looks worse than being slightly late.
class NetClient
{
public:
    struct Config
    {
        // Two snapshot intervals at 30 Hz. Enough that the next snapshot has almost always arrived
        // before it is needed, and small enough that nobody notices the lag.
        float interpolationDelay = 0.066f;
        ReconciliationSettings reconciliation;
    };

    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    bool Connect(std::unique_ptr<Transport> transport, const std::string& address, uint16_t port,
                 const std::string& name, const Config& config);
    void Disconnect();

    bool Connecting() const { return m_transport != nullptr && !m_welcomed; }
    bool Connected() const { return m_welcomed; }
    uint8_t PlayerId() const { return m_playerId; }
    JoinRejection Rejection() const { return m_rejection; }

    // Once per fixed tick. Steps `local` from `input`, remembers the guess, sends it, and applies
    // any correction the host has sent back.
    void Tick(const PlayerInput& input, PlayerController& local, float dt);

    // Once per frame. Advances the interpolation clock and rebuilds the remote player views.
    void UpdateInterpolation(float frameDeltaSeconds);

    // Added to the local player's drawn position. A correction moves the simulation at once but
    // this is walked back to zero over a few frames, so a small disagreement is invisible rather
    // than a twitch.
    const glm::vec3& VisualOffset() const { return m_visualError; }

    // --- The world -------------------------------------------------------------------------------
    //
    // Changes the host has made, for the game to apply to its own copy of the world, and the
    // requests this client would like the host to consider.

    std::vector<WorldEventMessage> TakeWorldEvents() { return std::exchange(m_worldEvents, {}); }
    const WorldStateMessage& LatestWorldState() const { return m_worldState; }
    bool HasWorldState() const { return m_hasWorldState; }

    void SendInteract(uint8_t kind, uint8_t index);
    void SendShot(const ShotMessage& shot);

    const std::vector<RemotePlayerView>& Remotes() const { return m_views; }
    const ReconciliationResult& LastReconciliation() const { return m_lastReconciliation; }
    // How many times the host has disagreed. A count that climbs every second means prediction is
    // diverging, which is the first thing to look at when movement starts to feel rubbery.
    uint32_t CorrectionCount() const { return m_corrections; }
    uint32_t Sequence() const { return m_sequence; }
    // The host tick this client is drawing everyone else at. A shot carries it so the host can
    // rewind to the moment the shot was actually aimed.
    uint32_t RenderTick() const { return m_renderTick; }
    Transport* GetTransport() { return m_transport.get(); }

private:
    struct SnapshotRecord
    {
        float time = 0.0f;
        SnapshotMessage message;
    };

    void HandlePacket(const NetPacket& packet);
    void SendJoin();
    void SendInput();
    void Reconcile(const SnapshotMessage& snapshot, PlayerController& local, float dt);

    std::unique_ptr<Transport> m_transport;
    std::vector<NetPacket> m_incoming;
    std::vector<SnapshotRecord> m_snapshots;
    std::vector<RemotePlayerView> m_views;
    std::vector<WorldEventMessage> m_worldEvents;
    WorldStateMessage m_worldState;
    PredictionBuffer m_history;
    Config m_config;
    ReconciliationResult m_lastReconciliation;
    glm::vec3 m_visualError{0.0f};
    std::string m_name;
    float m_clock = 0.0f;
    float m_joinTimer = 0.0f;
    uint32_t m_sequence = 0;
    uint32_t m_lastAcknowledged = 0;
    uint32_t m_corrections = 0;
    uint32_t m_renderTick = 0;
    uint8_t m_playerId = 0;
    bool m_snapshotArrived = false;
    bool m_hasWorldState = false;
    JoinRejection m_rejection = JoinRejection::None;
    bool m_welcomed = false;
};

} // namespace pred
