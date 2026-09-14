#pragma once

#include "Engine/Application.h"
#include "Engine/Render/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/Inventory.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/Items/ItemAppearance.h"
#include "Game/Items/ItemIcons.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Audio/VoiceCapture.h"
#include "Engine/Audio/VoiceCodec.h"
#include "Game/Net/IceCarrier.h"
#include "Game/Net/NetSession.h"
#include "Engine/Net/PortMapper.h"
#include "Game/Player/PlayerBody.h"
#include "Game/Player/PlayerController.h"
#include "Game/Weapons/ShotResolver.h"
#include "Game/Weapons/WeaponDatabase.h"
#include "Game/Weapons/WeaponSystem.h"
#include "Game/World/TestMap.h"
#include "Tools/ModelEditor/ModelEditor.h"
#include "Game/World/WorldObjects.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace pred
{

// Milestone 3 game: a first-person player controller in the developer test map, with a free-flying
// inspection camera available on a key.
class PredationGame final : public Game
{
public:
    bool OnInit(Application& app) override;
    void OnShutdown() override;
    void OnEvent(const SDL_Event& event) override;
    void OnFixedUpdate(double fixedDt) override;
    void OnUpdate(double dt, double alpha) override;
    void OnRender() override;
    void OnImGui() override;

private:
    struct DynamicProp
    {
        Entity entity;
        BodyHandle body;
    };

    void RegisterCommands();
    void DrawDebugOverlays();
    void SyncDynamicProps();
    void SpawnProp(bool sphere, float impulse);
    void ClearProps();

    void SampleLook(float dt);
    PlayerInput BuildPlayerInput();
    void TryInteract();
    // Opens or closes the model editor and hands the view and the mouse over to it.
    // The editor's own scene. Nothing of the game is in it: a model is looked at against an empty
    // floor rather than against whatever happens to be at the spawn point.
    Scene m_editorScene;
    // A body to hold what is being built. The only question that matters about a grip is where the
    // hand ends up, and that cannot be answered without someone to hold it.
    PlayerBody m_editorBody;
    PlayerState m_editorState;
    PlayerView m_editorView;
    float m_editorClock = 0.0f;
    bool m_editorBodyBuilt = false;
    int m_editorStance = 0;
    bool m_editorWalking = false;
    // What the preview is doing with the weapon. Driven from buttons, because there is no simulation
    // behind it: the point is to watch one movement at a time and as often as you like.
    float m_editorAim = 0.0f;
    float m_editorDraw = 1.0f;
    float m_editorKick = 0.0f;
    // Putting it away, so an unequip clip can be watched. Runs 1 to 0 and then holds.
    float m_editorHolster = 1.0f;
    bool m_editorHolstering = false;
    float m_editorReload = -1.0f; // negative when not reloading, matching WeaponPose
    // A clip being watched on demand, and where in it. Empty means the built-in movements play,
    // which is what happens in the game.
    std::string m_editorClip;
    float m_editorClipTime = 0.0f;
    bool m_editorClipPlaying = true;
    WeaponDefinition m_editorPreviewWeapon;
    void UpdateEditorBody(float frameDeltaSeconds);
    // The weapon bench: point a weapon at a model, play what it does, and see where the hands land
    // against the sockets. Open beside the editor, because placing a grip means looking at where
    // the hand ends up and there is no other way to find out.
    void DrawWeaponBench();
    // Points a weapon at the model open in the editor, for the rest of this run. One function so
    // the button and the console command do exactly the same thing, and so the thing the button
    // does can be run without a button.
    void AssignModelToWeapon(WeaponId weapon);
    bool m_benchSockets = true;
    int m_benchWeapon = 0;
    // Placing something that is not a weapon in the hand: which one, and whether it is held.
    // Which item the bench is holding, or -1 for the model being edited. One choice, because one
    // pair of hands holds one thing.
    int m_benchItem = -1;
    bool m_benchItemHeld = false;

    // A first-person window onto the editor's own body.
    //
    // Everything else in the editor looks at the model from outside, and outside is not where the
    // player is: a grip that reads perfectly in the viewport can put the receiver across half the
    // screen from behind the eye, and there was no way to find that out without leaving the editor,
    // starting a game and picking the thing up. The body is already here and already holding it, so
    // this is its own eye rendered into a panel.
    void RenderEditorFirstPerson();
    void DrawEditorFirstPerson();
    void DestroyEditorFirstPerson();
    bool m_editorFirstPerson = true;
    // Crosshairs over the panel, for lining a sight up on the view axis by something other than eye.
    bool m_editorEyeReticle = true;
    bgfx::FrameBufferHandle m_editorEyeBuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_editorEyeTexture = BGFX_INVALID_HANDLE;
    // The panel is the shape of the game window, so a hold lined up in it is lined up in the game.
    uint16_t m_editorEyeHeightPixels = 360;
    // Equips whatever weapon the selected inventory slot carries, or nothing if it carries none.
    void SyncEquippedWeapon();
    // Applies the rounds fired this tick. Only the authority may call this; it is the one place a
    // shot turns into an effect on the world.
    void ResolveShots();
    // Fades the drawn tracers. Presentation only.
    void AgeTracers(float dt);
    const WeaponDefinition* EquippedWeapon() const;
    // Where the barrel is and which way it points, including recoil.
    glm::vec3 MuzzlePosition() const;
    glm::vec3 AimDirection() const;
    void DropSelected();
    // Puts one thing on the floor. A client asks the host and waits; the host does it and tells
    // everyone. `rounds` and `reserve` are negative for anything with no state of its own.
    void DropIntoWorld(ItemId item, int count, int rounds, int reserve, const glm::vec3& origin,
                       const glm::vec3& velocity);
    // Everything in the bag, scattered where the player fell.
    void DropEverything();
    // Refills the spare rounds for the equipped weapon from a crate in the world.
    void TakeAmmunition(int crateIndex);
    void EnterHidingSpot(int index);
    void LeaveHidingSpot();
    // Health and stamina, bottom left. Bars rather than numbers: both are things to glance at.
    // Takes no ImGui types, so this header does not have to know that ImGui exists.
    void DrawCondition();
    void DrawHud();
    // Who else is in the game, down the side: name, connection and whether they are still up.
    void DrawPlayerList();
    void DrawInventoryPanel();
    // Draws the item as it actually looks, from the offscreen atlas. Falls back to nothing rather
    // than a stand-in shape: an icon that disagrees with the object is worse than no icon.
    void DrawItemIcon(ItemId item, float boxSize) const;
    void UpdateMouseCapture();
    // Logs any level geometry that intersects other level geometry. Overlapping static solids read
    // as modelling errors from inside and can shove the player, so the build reports its own.
    void ReportMapOverlaps(float minPenetration);
    // Best-effort name for a physics body, by finding the nearest scene entity. Bodies are not
    // named, and several can belong to one entity, so this is a diagnostic aid rather than a lookup.
    std::string DescribeBody(BodyHandle body) const;
    void ReloadPlayerConfig();
    void DrawPlayerPanel();

    // --- The front end -------------------------------------------------------------------------
    // Where the player is: at the menu, or in the world. The world is built and simulating either
    // way, so the menu has something to sit in front of and starting a game is instant.
    enum class Screen : uint8_t
    {
        Title,
        Playing,
        // The model editor, which is a separate place rather than something switched on in the middle
        // of a game. It has its own scene, so the level is not standing behind whatever is being
        // built, and nothing in the world simulates while it is open.
        Editor
    };
    // The in-game pause menu: an overlay on the world rather than a place, so leaving it puts the
    // player back exactly where they were. In a session nothing stops simulating, because a shared
    // world cannot be paused by one person in it.
    // The settings panel, drawn inside the menu and inside the pause screen alike.
    // Punching a way through two routers by swapping a code with the other player.
    // The two things anybody came to the menu to do, each on its own page.
    void DrawTitleOpen();
    void DrawTitleOpenLocal();
    void DrawTitleJoin();
    void StartPunchedSession(bool asHost);
    void StopPunchedSession();
    void DrawPunchThrough();
    // Adds one more punched connection and starts it gathering. Returns which one it is. The host
    // calls this per invitation; a guest calls it once.
    size_t AddPunchLink();
    // One invitation: its code to send, a box for the reply, and what it is doing. Drawn on the
    // title screen while opening a game and from the pause menu once the game is running, because
    // the second and third players do not arrive at the same moment as the first.
    void DrawPunchSlot(size_t slot);
    // The host's invitations panel, on the pause menu.
    void DrawInvitePanel();
    // Hands any newly connected link to the running session. Called once a frame while hosting.
    void AdoptConnectedLinks();
    void DrawSettings();
    void DrawPauseMenu();
    // Where the weapon sits relative to the eye, for comparing the editor with the game.
    void ReportHold();
    // How far through a reload the local player is, 0 to 1. One place, because the pose and the wire
    // each had their own and only one of them was a fraction.
    float ReloadProgress() const;
    void DrawTitleScreen();
    void EnterWorld();
    void EnterEditor(const std::string& modelName);
    void ReturnToTitle();
    // Slowly circles the camera around the spawn area behind the menu.
    void UpdateTitleCamera(float frameDeltaSeconds);
    // Puts the level back to how it started, so a new game is a new game.
    void ResetWorld();

    // --- World replication ----------------------------------------------------------------------
    //
    // The host owns the world. A client asks for a door and applies what it is told; it never
    // decides. Offline the local player is the host, which is what keeps single player and hosting
    // on exactly one code path.
    bool IsAuthority() const { return m_sessionMode != SessionMode::Client; }
    uint8_t LocalPlayerId() const;

    // Carrying out an interaction, whoever asked for it. Returns false when it could not be done,
    // which for a request from a client is an answer in itself.
    bool PerformInteraction(InteractionKind kind, int index, uint8_t player);
    // Requests from clients, checked against the world before anything happens.
    void ServeClientRequests();
    // Changes the host has made, applied to this machine's copy of the world.
    void ApplyWorldEvent(const WorldEventMessage& event);
    // The drawn stand-in for another player, or null if they have none here yet. Declared after
    // RemoteAvatar in the class, so the definition is in the source file.
    struct RemoteAvatar;
    RemoteAvatar* AvatarFor(uint8_t id);
    void SendDynamicBodies();
    void ApplyDynamicBodies(const WorldStateMessage& state);
    // Everything a player who has just joined needs in order to see the world as it now is.
    void SendWorldToPlayer(uint8_t player);

    // Where a player is, for checking they are close enough to what they are asking for.
    glm::vec3 PlayerPosition(uint8_t player) const;
    // Everyone, where they are this instant.
    // Where a player's weapon is being drawn here, for the line a round leaves along.
    glm::vec3 MuzzleOf(uint8_t player, const glm::vec3& eye, const glm::vec3& direction) const;
    std::vector<NetHost::PlayerPose> PosesNow() const;

    // --- Damage ---------------------------------------------------------------------------------
    // Runs the rounds fired this tick against the world and against the other players. Host only:
    // this is the one place a shot turns into damage.
    // `poses` is who was where at the moment the shot was aimed, which for a client is a rewound
    // moment and for the host is now.
    void ResolvePlayerHits(const FireEvent& shot, uint8_t shooter, ShotResult& worldHit,
                           const std::vector<NetHost::PlayerPose>& poses);
    void ApplyPlayerDamage(uint8_t player, float amount, uint8_t killer, const glm::vec3& direction);
    void KillPlayer(uint8_t player, const glm::vec3& direction);
    // Death is a pause. The authority runs the clock and says when somebody comes back.
    // The host left. Whoever is left elects a successor and the rest follow it.
    void UpdateHostMigration(float dt);
    void UpdateRespawns(float dt);
    // Dead, you watch a teammate through their own eyes. Never a free camera: that would show you
    // where the creature is, which is the one thing being dead must not tell you.
    void UpdateSpectating();

    // --- Multiplayer ---------------------------------------------------------------------------
    void RegisterNetCommands();
    void StopSession();
    // Runs the host or the client for one tick. Returns true when it has already stepped the local
    // player, which a client does inside prediction so the replay uses the same code path.
    bool StepSession(const PlayerInput& input, float dt);
    // Builds a body for anyone who has appeared and animates everyone from their replicated state.
    void SyncRemoteAvatars(float frameDeltaSeconds);
    void DrawNetworkPanel();
    const std::vector<RemotePlayerView>& RemotePlayers() const;
    // Brings the local player back: upright, standing, and not still wearing their own corpse.
    // Puts what is in the hands away for a climb and takes it back out at the top.
    // Footsteps, landings and where the ears are. Called once a frame.
    void UpdateSounds(float dt);
    void PlaySound(SoundId sound, const glm::vec3& at, float gain = 1.0f, float pitch = 1.0f,
                   bool positioned = true);
    // Reads Data/footsteps.json and the clips it names. Missing or broken leaves the synthesised
    // footstep in place rather than making the game silent underfoot.
    void LoadFootsteps(AudioEngine& audio);

    // --- Proximity voice --------------------------------------------------------------------------
    //
    // Push to talk opens the microphone, and closing it again when the key comes up is the whole
    // privacy story: the operating system indicator means what it says because nothing holds the
    // device open when nobody is speaking.
    //
    // Each speaker gets a stream in the mixer and a voice positioned where they are standing, so the
    // attenuation, the panning and the distance cut are the same machinery a footstep goes through.
    void UpdateVoice(float dt);
    void StopTalking();
    // Plays or updates a speaker's stream. `at` is where they are, in the world.
    void HearVoice(uint8_t speaker, const std::vector<uint8_t>& frame, const glm::vec3& at);
    // One clip from the given surface, and that surface's loudness folded into `gain`.
    SoundId PickFootstep(float& gain, int surfaceIndex) const;
    // Which surface a position is standing on. The test map has a row of them; everywhere else
    // there is nothing to go on yet, so whatever the sound panel last chose stands.
    int SurfaceIndexAt(const glm::vec3& position) const;
    // The sound panel: master volume, what the mixer is doing, and somewhere to audition footsteps.
    // A footstep is judged in a sequence, so this can walk on the spot at a chosen cadence rather
    // than only playing one.
    void DrawSoundPanel();
    void UpdateMantleStow();
    // Whether the hands are busy with something that is not the inventory. Nothing changes hands
    // while both of them are on a ledge.
    bool HandsAreFree() const { return !m_mantleStowing; }
    void RespawnLocalPlayer(const glm::vec3& position);
    // The name to join or host under: whatever is in the box, trimmed, never empty.
    std::string PlayerName() const;

    Application* m_app = nullptr;
    Scene m_scene;

    PlayerController m_player;
    PlayerBody m_body;
    glm::vec3 m_spawnPoint{0.0f, 0.5f, TestMapSpec::kSpawnZ};

    ItemDatabase m_items;
    ItemIcons m_itemIcons;
    Inventory m_inventory;

    // The weapon simulation is deliberately separate from the controller. It is a pure function of
    // state, input and time, which is what lets a client run it the instant the trigger goes down
    // and a host run the identical code to decide what the round hit.
    WeaponDatabase m_weaponData;
    WeaponState m_weapon;
    std::vector<FireEvent> m_shots;
    // Where the last few rounds went, purely so they can be drawn. Never read by the simulation.
    struct Tracer
    {
        // Where the round is drawn from: the muzzle, so it looks like it came out of the gun.
        glm::vec3 from{0.0f};
        // Where it was really traced from: the eye, so the crosshair tells the truth. The two
        // differ by most of an arm's length, which is why the debug view draws both. For somebody
        // else's round this is the muzzle as well, because their eye never crosses the wire.
        glm::vec3 origin{0.0f};
        glm::vec3 to{0.0f};
        bool hit = false;
        float age = 0.0f;
    };
    std::vector<Tracer> m_tracers;
    InteractionSystem m_interactions;
    WorldObjects m_world;
    // -1 when not hidden. While hidden the player holds still inside the locker.
    // What is carried in the hand when it is not a weapon.
    ItemId m_heldItem = kInvalidItem;
    int m_hidingSpot = -1;
    bool m_inventoryOpen = false;

    // First person for play, third person for watching the body animate, fly to inspect the level.
    enum class CameraMode : uint8_t
    {
        FirstPerson,
        ThirdPerson,
        Fly
    };
    CameraMode m_cameraMode = CameraMode::FirstPerson;
    float m_thirdPersonDistance = 3.2f;
    // Third-person orbit, applied on top of the look angles. Holding the look button turns the
    // camera around the character without turning the character, so the animation can be inspected
    // from any side.
    float m_orbitYaw = 0.0f;
    float m_orbitPitch = 0.0f;

    void SetCameraMode(CameraMode mode);
    const char* CameraModeName() const;

    FlyCamera m_camera;

    // The model and animation editor. It runs inside the game so that what is built in it is drawn
    // by the same renderer and the same shader the world uses.
    ModelEditor m_editor;

    // Mouse capture has exactly one owner. `m_wantMouseCaptured` is the player's intent, toggled by
    // Escape and by clicking back into the world. The effective state additionally requires window
    // focus and that no debug UI is using the pointer, so the console and the tuning panels are
    // always reachable.
    bool m_wantMouseCaptured = true;
    bool m_mouseCaptured = false;
    bool m_windowFocused = true;
    // The first motion event after relative mode is enabled can carry the whole distance from
    // wherever the cursor was, which would snap the view. Swallow one frame of delta.
    bool m_discardNextMouseDelta = false;

    // Look angles are sampled every frame, outside the fixed tick, so aiming is never limited by
    // the simulation rate.
    float m_lookYaw = 0.0f;
    float m_lookPitch = 0.0f;

    // Latched between frames so a press that happens between two ticks is never dropped.
    bool m_jumpLatch = false;
    bool m_crouchPressLatch = false;
    bool m_pronePressLatch = false;
    // Debug stance override, so stances can be inspected without holding a key.
    glm::vec2 m_debugMove{0.0f};
    // A reload request holds for a moment rather than a single tick, so pressing it while the
    // trigger is down or mid-shot still reloads as soon as the weapon can accept it.
    int m_reloadLatch = 0;
    // Console-driven trigger, so firing can be exercised in a headless capture.
    int m_debugTriggerTicks = 0;
    // Visual recoil: one on the frame a round leaves, decaying away. Separate from the weapon's own
    // recoil, which moves the aim; this only moves the model.
    float m_weaponKick = 0.0f;
    // Runs 0 to 1 as a weapon is brought up. Reset whenever what is held changes, so swapping is a
    // movement rather than one model being substituted for another.
    float m_weaponDraw = 1.0f;
    // How far a weapon in the hands has been put away: 1 while it is out, 0 once it has gone. A
    // swap waits for this to run down, so an authored unequip has a moment to happen in.
    float m_weaponHolster = 1.0f;
    // The last frame's length, for presentation timers that live outside the fixed tick.
    float m_lastFrameSeconds = 1.0f / 60.0f;
    // Which inventory slot the equipped weapon's magazine belongs to, so it can be put back there
    // when something else comes out. Inventory::kNoSlot when nothing is in hand.
    int m_ammoSlot = -1;
    // Console-driven aim, so the sighted hold can be inspected in a headless capture.
    bool m_debugAim = false;
    // True only while the right button is held in the editor, which is when the mouse belongs to the
    // camera rather than to the panels.
    bool m_editorLooking = false;
    bool m_forceCrouch = false;
    bool m_forceProne = false;
    bool m_crouchToggleState = false;
    bool m_proneToggleState = false;
    bool m_sprintToggleState = false;

    // Another player's body on this machine. It is driven entirely from replicated state through
    // the same procedural animation the local body uses, so a remote player walks, crouches and
    // leans the way the local one does without any of it being sent: only where they are and what
    // they are doing crosses the wire.
    struct RemoteAvatar
    {
        uint8_t id = 0xFF;
        PlayerBody body;
        PlayerState state;
        PlayerView view;
        bool built = false;
        bool collapsed = false;
        uint8_t heldItem = 0xFF; // 0xFF forces the first sync to put something in their hands
        // Firing is an event, not a state: the snapshot cannot carry it, because the frame a shot
        // was fired on is usually not a frame a snapshot went out on. The shot event sets this to
        // one and it decays the same way the local one does, which is what makes somebody else's
        // weapon flash and recoil instead of firing silently.
        float weaponKick = 0.0f;
        // Raised the moment something arrives in their hands, so a remote weapon is brought up
        // rather than appearing already shouldered.
        float weaponDraw = 1.0f;
        // Where their body is actually drawn, eased towards where the wire says they are.
        //
        // On the host, everyone else's published position is rebuilt once per simulation tick while
        // the screen draws whenever it likes, so an avatar taken straight from it moves in steps.
        // From inside their head, spectating, those steps are the camera jumping back and forth; the
        // body they are attached to smears with them, which is the double image.
        glm::vec3 drawn{0.0f};
        bool drawnValid = false;
        // And where they are looking, eased the same way and for the same reason: on the host the
        // published angles step once a tick, which from inside their head is the view jumping.
        float lookYaw = 0.0f;
        float lookPitch = 0.0f;
        bool lookValid = false;
        // Where their last climb ended and how far through it was.
        //
        // The snapshot only carries these while somebody is climbing, and the hands go on fading
        // off the ledge for a moment after they stop: reading the wire's zero during that fade
        // pulled their weapon towards the world origin at full strength and then let it snap back,
        // which is the gun everyone saw fly away from a player pulling themselves over a wall.
        glm::vec3 mantleEdge{0.0f};
        float mantlePhase = 0.0f;
        // Their walk cycle last frame, for their footsteps.
        float lastStridePhase = 0.0f;
    };
    Screen m_screen = Screen::Title;
    bool m_paused = false;
    // Whether the settings panel is showing, on whichever screen is up. One flag, because only one
    // of those screens is ever on at a time.
    bool m_settingsOpen = false;
    // Swapping codes with the other player, and the connection it is trying to make.
    // Which page of the menu is showing. The first one is two buttons and a name.
    enum class TitlePage : uint8_t
    {
        Root,
        Open,
        OpenLocal,
        Join
    };
    TitlePage m_titlePage = TitlePage::Root;
    bool m_punching = false;
    bool m_punchingAsHost = false;

    // The punched connections, and the carrier that holds them for the transport.
    //
    // A hole punched through two routers joins exactly two machines, so one link is one other
    // player. A guest needs one, to the host. A host needs one per guest, which is why these are
    // lists: with a single link a game over the internet was two players and no more, whatever the
    // lobby size said. The carrier outlives any one link because the transport holds it for the
    // whole session, and link n there is the same n as here.
    std::shared_ptr<IceCarrier> m_iceCarrier;
    std::vector<std::shared_ptr<IceLink>> m_punchLinks;
    // What was pasted into the join box: a code, or an address. Separate from the per-invitation
    // buffers below because it is a different question asked in a different place.
    char m_joinInput[1400] = "";
    // Their code, pasted, one buffer per invitation. Long, because a code carries every address
    // that machine has.
    struct PunchSlot
    {
        char code[1400] = "";
        bool started = false; // whether this link has been handed to the session yet
    };
    std::vector<PunchSlot> m_punchSlots;
    // The invitation panel, shown from the pause menu once a host is already playing.
    bool m_inviteOpen = false;
    // A link started by the net_punch console command and nothing else: a way to find out from one
    // machine whether this connection can be described to another at all. Deliberately not one of
    // the session links above, because it is a diagnostic and joins nothing.
    std::shared_ptr<IceLink> m_probeLink;
    // Frames left before the punch report gives up waiting for a code. Debug tooling only.
    int m_punchReportIn = 0;
    // Frames left before a deferred hold report. Commands from --exec all run before the first
    // frame, when nothing is equipped, so a report taken then is about an empty hand.
    int m_holdReportIn = 0;
    float m_titleClock = 0.0f;
    // Kept between visits to the menu so rejoining the same friend does not mean typing the address
    // again. Sized for an address and a port; anything longer is not an address.
    char m_joinAddress[64] = "127.0.0.1";
    int m_joinPort = kDefaultPort;
    int m_hostPort = kDefaultPort;
    // What other players see this one called. Short on purpose: a player list is a narrow panel.
    char m_playerName[24] = "operator";
    std::string m_titleStatus;

    SessionMode m_sessionMode = SessionMode::Offline;
    NetHost m_host;
    // The router, asked to forward the port while this machine is hosting, so people outside the
    // house can reach it. Best effort: the menu says what happened.
    PortMapper m_ports;
    NetClient m_client;
    std::vector<std::unique_ptr<RemoteAvatar>> m_avatars;
    uint32_t m_networkTick = 0;
    // Which way the blow that killed the local player came from, so the body goes down that way.
    float m_worldStateTimer = 0.0f;
    glm::vec3 m_deathImpulse{0.0f};
    float m_respawnTimer = 0.0f;
    float m_migrationTimer = 0.0f;
    std::map<uint8_t, float> m_remoteRespawnTimers;
    // Whose eyes we are watching through while dead. -1 when alive or when nobody is left.
    // What was in the hands when a climb started, and whether a climb has hold of them. kNoSlot
    // when there was nothing to put away.
    int m_mantleStowedSlot = -1;
    bool m_mantleStowing = false;
    // Every sound the game plays, looked up by name once rather than on every shot.
    struct SoundSet
    {
        SoundId gunshot = kInvalidSound;
        SoundId dryFire = kInvalidSound;
        SoundId reloadOut = kInvalidSound;
        SoundId reloadIn = kInvalidSound;
        SoundId step = kInvalidSound;
        SoundId land = kInvalidSound;
        SoundId door = kInvalidSound;
        SoundId locker = kInvalidSound;
        SoundId pickup = kInvalidSound;
        SoundId drop = kInvalidSound;
        SoundId hurt = kInvalidSound;
        SoundId death = kInvalidSound;
    };
    SoundSet m_sounds;

    // Recorded footsteps, one group of clips per surface.
    //
    // The rest of the game's sounds are synthesised from a recipe, and footsteps are where that
    // stops being enough: a footstep is a physical event with a texture. Which surface is underfoot
    // is one value for the whole world today, because the test map is one floor; when the map has
    // more than one kind of ground under it, this is what the ground will pick.
    struct FootstepSurface
    {
        std::string name;
        float gain = 1.0f;
        std::vector<SoundId> clips;
    };
    std::vector<FootstepSurface> m_footsteps;
    int m_footstepSurface = 0;
    // Which clip was used last, so the same one is not heard twice running.
    mutable SoundId m_lastFootstep = kInvalidSound;
    // Deliberately not the simulation's random source: what a footstep sounds like must never be
    // able to change where a bullet goes.
    mutable std::minstd_rand m_footstepRandom{20260914u};
    // Walking on the spot from the sound panel: seconds since the last step, and how far apart they
    // are. Zero means not walking.
    float m_footstepAudition = 0.0f;
    float m_footstepCadence = 0.55f;

    // Proximity voice. The microphone, the codec, and one stream per person talking.
    VoiceCapture m_microphone;
    VoiceCodec m_voiceCodec;
    bool m_talking = false;
    uint16_t m_voiceSequence = 0;
    float m_voiceLevel = 0.0f;
    struct Speaker
    {
        uint8_t id = 0;
        StreamId stream = kInvalidStream;
        VoiceId voice = kInvalidVoice;
        VoiceCodec codec;
        // The last sequence played, so a frame that arrives out of order can be dropped and a gap
        // can be told from a reordering. One decoder per speaker, because a codec carries the state
        // of the conversation it is decoding and two people through one would be nonsense.
        uint16_t lastSequence = 0;
        bool started = false;
        float silentFor = 0.0f;
    };
    std::vector<std::unique_ptr<Speaker>> m_speakers;

    // Where the walk cycle had got to last frame, so a footfall is heard as the foot passes rather
    // than on a clock of its own.
    float m_lastStridePhase = 0.0f;
    int m_spectating = -1;
    // Set when a dead player asks to watch somebody else. Consumed on the next spectator update.
    bool m_spectateNext = false;
    // Eased like your own eye height, so a watched player crouching is a sink rather than a snap.
    float m_spectateEyeHeight = 0.0f;
    bool m_localCollapsed = false;
    NetConditions m_simulatedConditions;

    std::vector<DynamicProp> m_props;
    MeshHandle m_propSphereMesh;
    MeshHandle m_propBoxMesh;
    double m_time = 0.0;
};

} // namespace pred
