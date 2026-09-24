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
    float aim = 0.0f;
    bool reloading = false;
    float reloadProgress = 0.0f;
    bool reloadEmpty = false;
    bool torchOn = false;

    // Held by a creature, or wrapped up at its nest.
    uint8_t heldBy = kNotHeld;
    bool cocooned = false;

    // Climbing, so a remote player is seen hauling themselves over a ledge rather than sliding up
    // a wall.
    bool mantling = false;
    float mantlePhase = 0.0f;
    glm::vec3 mantleEdge{0.0f};

    // Their round trip to the host in milliseconds, as the host measures it. Zero on the host's own
    // row, which is the only honest figure for a machine talking to itself.
    uint16_t pingMs = 0;
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
        // What to call this machine on everyone else's player list.
        std::string name = "host";
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

    struct DropRequest
    {
        uint8_t player = 0;
        DropMessage drop;
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
    std::vector<DropRequest> TakeDropRequests() { return std::exchange(m_dropRequests, {}); }
    // Somebody using what is in their hand, with who they are filled in by the host.
    struct ItemUseRequest
    {
        uint8_t player = 0;
        ItemUseMessage use;
    };
    std::vector<ItemUseRequest> TakeItemUses() { return std::exchange(m_itemUses, {}); }
    // Health back, into the controller the host simulates for them.
    void HealPlayer(uint8_t playerId, float amount);
    // The host keeps a tally of what each client has picked up, so a client cannot put down
    // something it never had. Without it, dropping is a way to make items out of nothing.
    void NoteCarried(uint8_t player, uint16_t item, int count);
    bool TakeCarried(uint8_t player, uint16_t item, int count);
    int CarriedCount(uint8_t player, uint16_t item) const;
    // Players who have just been let in. The game sends them the state of the world.
    std::vector<uint8_t> TakeJoined() { return std::exchange(m_joined, {}); }

    // Somebody who has gone, and what they were carrying when they went. Whatever the host handed
    // them belongs to the world, not to a connection: without this it leaves with them, and what is
    // left behind is their body still holding a copy of it that nobody can take.
    struct Departure
    {
        uint8_t player = 0;
        glm::vec3 position{0.0f};
        std::vector<std::pair<uint16_t, int>> carried;
    };
    std::vector<Departure> TakeDeparted() { return std::exchange(m_departed, {}); }

    // Voice heard from somebody else, waiting to be played. Taken rather than read, because each
    // frame is played once and holding them would be a growing buffer of old speech.
    //
    // Every frame a client sends comes through here, whether the host is near enough to hear it or not:
    // `audible` says whether it is; the creatures learn from all of them.
    struct VoiceHeard
    {
        uint8_t speaker = 0;
        uint16_t sequence = 0;
        std::vector<uint8_t> frame;
        bool audible = true;
    };
    std::vector<VoiceHeard> TakeVoice();

    // The host talking. Goes to everybody within earshot of `from`.
    void SendVoice(uint16_t sequence, const std::vector<uint8_t>& frame, const glm::vec3& from);
    // A creature saying back something it heard, from where it is. Goes to everybody within earshot,
    // marked as the creature's, so it is played from its mouth.
    void SendCreatureVoice(uint8_t creature, uint16_t sequence, const std::vector<uint8_t>& frame, const glm::vec3& from);

    void Broadcast(const WorldEventMessage& event);
    // Who is here and where. Sent when the roster changes, so that if this machine goes the players
    // left know where to find each other.
    void BroadcastPeerList();
    // Into the game from the lobby, or back. Everybody is told at once, by the roster.
    void SetStarted(bool started);
    bool Started() const { return m_started; }
    void SendTo(uint8_t playerId, const WorldEventMessage& event);
    void SendWorldState(const WorldStateMessage& state);
    void SendCreatureState(const CreatureStateMessage& state);

    // What a client is holding and doing with it, so everyone sees the right thing in their hands.
    void SetPlayerHeld(uint8_t playerId, uint8_t heldItem, float aim, bool reloading, float progress,
                       bool reloadEmpty = false);
    // And whether their torch is lit. Separate from the hands because it is one bit that changes
    // when a key is pressed rather than every frame, and because nothing else in the protocol
    // implies it: without this, a torch is visible only to the player holding it.
    void SetPlayerTorch(uint8_t playerId, bool on);
    // A creature has hold of a player, or has let go (`by` kNotHeld): the host pins them where the
    // creature has them, every tick, and says so in the snapshot so their own machine does too.
    void SetPlayerGrabbed(uint8_t playerId, uint8_t by, bool cocooned, const glm::vec3& feet, float yaw);
    // Pins a client where the world says they are -- inside a locker -- or lets them go again. Without
    // this the host went on simulating them walking about outside the locker they had climbed into.
    void PinPlayer(uint8_t playerId, bool pinned, const glm::vec3& feet, float yaw);
    // What a client last pressed, for anything the host decides from it: struggling in a grip.
    PlayerInput LastInputOf(uint8_t playerId) const;
    // Damage a client. The host owns their body, so this is where their health actually changes:
    // the published view is rebuilt from the controller every tick, so writing to that changed
    // nothing and health came back the moment it was read again.
    // `cause` is for the log: what did it, in a word.
    void ApplyDamageTo(uint8_t playerId, float amount, const char* cause = "gunfire");
    void RespawnPlayer(uint8_t playerId, const glm::vec3& position);
    float HealthOf(uint8_t playerId) const;

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
    std::vector<DropRequest> m_dropRequests;
    std::vector<VoiceHeard> m_voiceHeard;
    std::vector<ItemUseRequest> m_itemUses;
    // Where the host itself is, kept each tick, so a voice arriving between ticks can be told
    // whether the host is near enough to hear it without the caller having to pass it in.
    glm::vec3 m_localPosition{0.0f};
    // One place that decides who hears a frame and sends it to them, used by a client relaying
    // through and by the host talking itself.
    void ForwardVoice(uint8_t speaker, bool creature, uint16_t sequence, const std::vector<uint8_t>& frame,
                      const glm::vec3& from);
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
    std::vector<Departure> m_departed;
    Config m_config;
    PhysicsWorld* m_physics = nullptr;
    PlayerConfig m_playerConfig;
    glm::vec3 m_spawn{0.0f};
    float m_snapshotTimer = 0.0f;
    uint32_t m_starvedTicks = 0;
    // Whether the game has started or everybody is still in the lobby.
    bool m_started = false;
    uint8_t m_localHeldItem = 0;
    float m_localAim = 0.0f;
    bool m_localReloading = false;
    float m_localReloadProgress = 0.0f;
    bool m_localReloadEmpty = false;
    bool m_localTorch = false;
    uint8_t m_localHeldBy = kNotHeld;
    bool m_localCocooned = false;
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
    // Whether a creature has hold of this player, as the host last said.
    bool HeldByHost() const { return m_heldByHost; }
    bool CocoonedByHost() const { return m_cocoonedByHost; }
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
    // What this client has in its hands, sent up with the next input. The host cannot see inside
    // another machine, so unless this is set nobody else ever sees you holding anything.
    void SetHeld(uint8_t heldItem, float aim, bool reloading, float progress, bool reloadEmpty = false);
    // Whether this machine.s torch is lit, so the host can put it in everybody else.s scene.
    void SetTorch(bool on);

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

    // Everyone else in the game, as the host last described them.
    //
    // Kept for one purpose: if the host goes, whoever is left elects the lowest surviving player
    // number as the new host and the rest connect to the address recorded here. There is nobody to
    // ask by then, which is why it has to be known in advance.
    struct KnownPeer
    {
        uint8_t id = 0;
        std::string name;
        std::string address;
    };
    const std::vector<KnownPeer>& Peers() const { return m_peers; }
    // Whether the host has started the game. Until it has, this client waits in the lobby.
    bool HostStarted() const { return m_hostStarted; }
    // What to call a player, from the roster. Falls back to their number while a roster is in
    // flight.
    std::string NameOf(uint8_t id) const;
    const std::string& Name() const { return m_name; }
    // True once the host has gone and this client is the one that should take over.
    bool ShouldBecomeHost() const;
    // Where the successor is, when it is not this machine.
    std::string SuccessorAddress() const;
    bool HostLost() const { return m_hostLost; }
    // The port this session is on. A successor has to listen where everyone will look for it,
    // which is where they were already connected, not wherever the menu happened to be set to.
    uint16_t SessionPort() const { return m_sessionPort; }

    std::vector<WorldEventMessage> TakeWorldEvents() { return std::exchange(m_worldEvents, {}); }
    const WorldStateMessage& LatestWorldState() const { return m_worldState; }
    bool HasWorldState() const { return m_hasWorldState; }
    // The newest creature state, and a count of how many have arrived, so the game can tell a new
    // one from the one it has already applied.
    const CreatureStateMessage& LatestCreatureState() const { return m_creatureState; }
    uint32_t CreatureStatesReceived() const { return m_creatureStatesReceived; }

    void SendInteract(uint8_t kind, uint8_t index);
    // Sent once, after this machine has built its world: only then can the host tell it what has
    // already happened without the answer being thrown away by the build.
    void SendReady();
    void SendDrop(const DropMessage& drop);
    void SendItemUse(const ItemUseMessage& use);
    // My microphone, on its way to the host, which decides who is close enough to hear it.
    void SendVoice(uint16_t sequence, const std::vector<uint8_t>& frame);
    // Voice from other people, waiting to be played. Taken rather than read: each frame is played
    // once, and holding them would be a growing buffer of old speech. `creature` when it is a creature
    // saying something back, and `speaker` is then the creature's number.
    struct VoiceHeard
    {
        uint8_t speaker = 0;
        uint16_t sequence = 0;
        std::vector<uint8_t> frame;
        bool creature = false;
    };
    std::vector<VoiceHeard> TakeVoice();
    void SendShot(const ShotMessage& shot);

    const std::vector<RemotePlayerView>& Remotes() const { return m_views; }
    const ReconciliationResult& LastReconciliation() const { return m_lastReconciliation; }
    // How many times the host has disagreed. A count that climbs every second means prediction is
    // diverging, which is the first thing to look at when movement starts to feel rubbery.
    uint32_t CorrectionCount() const { return m_corrections; }
    uint32_t Sequence() const { return m_sequence; }
    // The newest of this client's inputs the host says it has run.
    uint32_t LastAcknowledged() const { return m_lastAcknowledged; }
    // The host tick this client is drawing everyone else at. A shot carries it so the host can
    // rewind to the moment the shot was actually aimed.
    uint32_t RenderTick() const { return m_renderTick; }
    // This machine's round trip to the host in milliseconds, as the host measured it.
    uint16_t PingMs() const { return m_pingMs; }
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
    std::vector<KnownPeer> m_peers;
    bool m_hostStarted = false;
    std::vector<WorldEventMessage> m_worldEvents;
    std::vector<VoiceHeard> m_voiceIn;
    WorldStateMessage m_worldState;
    CreatureStateMessage m_creatureState;
    uint32_t m_creatureStatesReceived = 0;
    PredictionBuffer m_history;
    bool m_heldByHost = false;
    bool m_cocoonedByHost = false;
    Config m_config;
    ReconciliationResult m_lastReconciliation;
    glm::vec3 m_visualError{0.0f};
    std::string m_name;
    float m_clock = 0.0f;
    float m_joinTimer = 0.0f;
    uint32_t m_sequence = 0;
    uint32_t m_lastAcknowledged = 0;
    uint32_t m_corrections = 0;
    uint8_t m_heldItem = 0;
    float m_heldAim = 0.0f;
    bool m_heldReloading = false;
    float m_heldReloadProgress = 0.0f;
    bool m_heldReloadEmpty = false;
    bool m_torchOn = false;
    uint32_t m_renderTick = 0;
    // The newest host tick this machine has seen, echoed back with every input so the host can time
    // the round trip.
    uint32_t m_lastSnapshotTick = 0;
    uint16_t m_pingMs = 0;
    uint8_t m_playerId = 0;
    bool m_snapshotArrived = false;
    bool m_hasWorldState = false;
    bool m_hostLost = false;
    uint16_t m_sessionPort = kDefaultPort;
    JoinRejection m_rejection = JoinRejection::None;
    bool m_welcomed = false;
};

} // namespace pred
