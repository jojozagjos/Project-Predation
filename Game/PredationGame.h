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
#include "Engine/Net/LobbyClient.h"
#include "Game/Net/NetSession.h"
#include "Engine/Net/LanDiscovery.h"
#include "Engine/Net/PortMapper.h"
#include "Engine/Navigation/NavMesh.h"
#include "Game/Creature/Creature.h"
#include "Game/Creature/Noise.h"
#include "Game/Creature/VoiceMemory.h"
#include "Game/Player/PlayerBody.h"
#include "Game/Player/PlayerController.h"
#include "Game/Weapons/BulletHole.h"
#include "Game/Weapons/ShotResolver.h"
#include "Game/Weapons/WeaponDatabase.h"
#include "Game/Weapons/WeaponSystem.h"
#include "Game/World/LevelLights.h"
#include "Game/World/TestMap.h"
#include "Game/World/LabMap.h"
#include "Tools/ModelEditor/ModelEditor.h"
#include "Game/World/WorldObjects.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <future>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace pred
{

// One sound the game asks for, and every recording of it there happens to be.
//
// Most of these are synthesised at startup from a recipe, which is why the game ships with
// almost no audio: a few hundred kilobytes of samples built in a few milliseconds. A recipe is a
// stand-in though, and the way it shows is repetition -- the same gunshot, sample for sample,
// forty times a magazine.
//
// So a folder of recordings replaces one. Drop wav files into Assets/Audio/<name>/ and they are
// used instead of the recipe, and a different one is picked each time it plays. Nothing has to
// be registered anywhere: the folder is the registration.
struct SoundVariants
{
    std::vector<SoundId> ids;
    SoundId Pick() const
    {
        if (ids.empty())
        {
            return kInvalidSound;
        }
        if (ids.size() == 1)
        {
            return ids.front();
        }
        return ids[static_cast<size_t>(std::rand()) % ids.size()];
    }
};

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
    // What F3 adds in the public build: ping, or everybody's ping when hosting.
    void DrawConnectionReadout();

    // --- The creature --------------------------------------------------------------------------
    //
    // In PredationGameCreatures.cpp. Only the authority -- the host, or a game played alone -- runs a
    // creature's mind; everybody else is shown where it is.
    NavMesh m_nav;
    // Each one holds a reference to m_scene, which is declared further down and so is destroyed
    // first. OnShutdown clears these before that can matter.
    std::vector<std::unique_ptr<Creature>> m_creatures;
    // Sounds made since the creatures last listened. Reported by whatever made them, drained each
    // tick by the creatures' hearing.
    std::vector<Noise> m_noises;
    float m_creatureClock = 0.0f;
    bool m_showBrain = false;
    int m_inspectedCreature = 0;
    // The inspector: keep it on whichever creature is nearest, and what its timeline is filtered by.
    bool m_brainFollowNearest = false;
    char m_brainFilter[64] = {};
    std::string m_brainTab;
    // When each player last made a voice noise, so talking is a sound every half second rather than
    // fifty times a second.
    std::map<int, float> m_lastVoiceNoise;
    // What the creatures have heard people say, on the host, and what they are saying back now: which
    // creature, the frames, how far through, and the clock that paces them at the speed they were said.
    VoiceMemory m_voiceMemory;
    struct Mimicry
    {
        uint8_t creature = 0;
        std::vector<std::vector<uint8_t>> frames;
        size_t next = 0;
        float clock = 0.0f;
    };
    std::vector<Mimicry> m_mimicry;
    uint16_t m_mimicSequence = 0;
    // A creature says something back in `player`'s voice, from where it is.
    void StartMimicry(const Creature& creature, int player);
    void UpdateMimicry(float dt);
    // The wire: the number the next creature the host makes will go by, the count its messages
    // carry, and which of the client's received messages has been applied.
    uint8_t m_nextCreatureId = 0;
    uint16_t m_creatureSequence = 0;
    uint32_t m_appliedCreatureStates = 0;
    // A game starts without its creature; it arrives a while later, somewhere nobody is looking.
    // How many are still to come, when, and the seed the first is made from.
    int m_arrivalsPending = 0;
    float m_arrivalAt = 0.0f;
    uint32_t m_arrivalSeed = 0;
    void UpdateArrivals();
    // A point on the walkable surface that no player can see and none is near, for something to
    // appear at without anybody watching it appear.
    bool FindUnseenPoint(uint32_t seed, glm::vec3& out) const;
    void SendCreatureState();
    void ApplyCreatureState(const CreatureStateMessage& state);
    void BuildNavigation();
    void SpawnCreatures();
    // Somewhere far from `awayFrom`, or, when `exactly` is given, on the walkable surface nearest it.
    bool SpawnCreature(uint32_t seed, const glm::vec3& awayFrom, const glm::vec3* exactly = nullptr);
    void ClearCreatures();
    void UpdateCreatures(float dt);
    void UpdateCreatureVisuals(float dt);
    void MakeNoise(NoiseKind kind, const glm::vec3& at, float reach, int player);
    // After the authority has worked out what a round hit: the creature hears the shot and the
    // impact, and takes the damage if it was what was hit -- in which case the result stops being a
    // surface, so no bullet hole is left on it. True when it struck a creature.
    bool OnShotResolved(ShotResult& result, const glm::vec3& origin, int shooter);
    float LightAt(const glm::vec3& feet, bool torchOn) const;
    Creature* CreatureForBody(BodyHandle body);
    Creature* CreatureById(uint8_t id);

    // Creatures with hold of players: who has whom, and how hard each is struggling.
    struct Grip
    {
        uint8_t creature = 0;
        float struggle = 0.0f;
        bool jumpWasDown = false;
        float struggleHeardAt = -10.0f;
    };
    std::map<uint8_t, Grip> m_grips;
    // Until when each player cannot be grabbed again, having just got free of something.
    std::map<uint8_t, float> m_grabImmunity;
    // What the player pressed this tick, for struggling in a grip.
    PlayerInput m_lastInput;
    // Players wrapped up at a nest, alive, until somebody cuts them free or it kills them.
    struct Cocoon
    {
        uint8_t player = 0;
        glm::vec3 feet{0.0f};
        float yaw = 0.0f;
        float owed = 0.0f; // damage owed, paid a point at a time
    };
    std::vector<Cocoon> m_cocoons;
    // Blows each locked door has taken from a creature breaking it down.
    std::map<int, int> m_doorBlows;
    // Each creature, where it last moved from and since when: for noticing one that has stood still too long.
    struct Stillness
    {
        glm::vec3 at{0.0f};
        float since = 0.0f;
        bool reported = false;
    };
    std::map<uint16_t, Stillness> m_stillness;
    // Creatures calling to each other: made this tick, heard by the others the next.
    struct Call
    {
        glm::vec3 at{0.0f};
        uint8_t by = 0;
    };
    std::vector<Call> m_calls;
    std::vector<Call> m_callsHeard;
    // The nests creatures have built. Nothing is here at the start of a game: a nest exists only
    // because something built it. A nest is a heart on a wall, beating, and growth that spreads from it
    // across the floor, the walls and the ceiling for as long as it lives. None of it is solid -- a
    // creature once built itself into its own nest and never got out -- and the heart can be shot.
    struct NestPatch
    {
        glm::vec3 at{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        float size = 1.0f;     // how far it reaches from its middle, in metres, grown
        float appears = 0.0f;  // how old the nest is when it starts to show
        float spin = 0.0f;     // turned about its surface, so no two look alike
        float fromHeart = 0.0f;
        int variant = 0;
        Entity entity;
    };
    struct Nest
    {
        glm::vec3 at{0.0f};       // the floor where it was built
        glm::vec3 stand{0.0f};    // where a creature stands at it, in front of the heart
        glm::vec3 heart{0.0f};    // the middle of the heart
        glm::vec3 wall{0.0f};     // where the heart is rooted
        glm::vec3 normal{0.0f, 0.0f, 1.0f}; // out of the surface it is rooted in
        glm::quat facing{1.0f, 0.0f, 0.0f, 0.0f};
        uint8_t owner = 0;
        uint16_t seed = 0;
        float age = 0.0f;         // seconds since it was built
        float health = 0.0f;
        bool dead = false;
        float deadFor = 0.0f;
        // Rotted away to nothing: drawn no more, kept only so every machine numbers nests alike.
        bool gone = false;
        // How much of their full spread the roots on the wall have room for.
        float rootScale = 1.0f;
        float beat = 0.0f;        // where it is in its beat, 0 to 1
        float flinch = 0.0f;      // how hard it was last struck, fading
        std::vector<NestPatch> patches;
        // Built on a worker: sculpting a heart takes longer than a frame should.
        std::future<std::vector<MeshData>> building;
        MeshHandle heartMesh;
        MeshHandle rootsMesh;
        std::vector<MeshHandle> growthMeshes;
        Entity heartEntity;
        Entity rootsEntity;
        BodyHandle heartBody;
    };
    std::vector<Nest> m_nests;
    void BuildNest(const glm::vec3& at, uint16_t seed, uint8_t owner, bool announce, int index = -1, float age = 0.0f);
    void ClearNests();
    // Everything drawn and solid of one nest taken out of the world.
    void ReleaseNest(Nest& nest);
    // When the last nest died, on the creature clock: another is not built straight away.
    float m_nestDiedAt = -1.0e9f;
    // The living nest nearest a point, when there is one within `reach`.
    const Nest* NestNear(const glm::vec3& point, float reach) const;
    // Which nest's heart a body is, or -1.
    int NestForBody(BodyHandle body) const;
    // Where a creature stands at a nest, and where the nest will spread to and when.
    void PlaceNestStand(Nest& nest) const;
    void PlanNestGrowth(Nest& nest) const;
    // The heart shot, on the host: down to nothing and the whole nest dies.
    void HurtNest(int index, float damage, int by);
    // What the host said about a heart: how much of it is left, and whether that was news.
    void SetNestHealth(int index, float fraction, bool quiet);
    // Every nest growing, beating and withering, on every machine.
    void UpdateNests(float dt);
    // The glow of each living heart, for the lights of the frame.
    void GatherNestLights(std::vector<PunctualLight>& lights) const;
    // How much a heart can take, whole.
    float NestWholeHealth() const;
    // Rebuilding the walkable surface on a worker thread after the level has changed shape, and putting
    // it in place once it is ready.
    void RequestNavRebuild();
    void FinishNavRebuild();
    std::future<void> m_navRebuild;
    std::unique_ptr<NavMesh> m_navSpare;
    bool m_navRebuilding = false;
    // What is drawn round each cocooned player, on every machine, and what somebody uses to cut them out.
    std::map<uint8_t, Entity> m_cocoonEntities;
    MeshHandle m_cocoonMesh;
    // Where a creature holds somebody it has hold of, and which way they face.
    glm::vec3 GripPoint(const Creature& creature, float& yaw) const;
    // Pins (or lets go, `by` kNotHeld) a player where a creature has them, whoever's machine they are on.
    void PinPlayer(uint8_t player, uint8_t by, bool cocooned, const glm::vec3& feet, float yaw);
    void TryGrab(Creature& creature, int target, const std::vector<SensedPlayer>& players);
    void ReleaseGrip(uint8_t player, const char* why);
    // A player who has gone: whatever the world was holding for them is let go of.
    void ForgetPlayer(uint8_t player);
    // Whether a creature has hold of this machine's player, or has them wrapped up: no shooting, no
    // reloading, no using anything while something has you.
    bool HeldLocally() const;
    void UpdateGrips(float dt);
    void WrapInCocoon(uint8_t player, const Creature& creature);
    void UpdateCocoons(float dt);
    void FreeFromCocoon(uint8_t player, uint8_t helper);
    // The cocoons, drawn round whoever is in one and offered to everybody else to cut open.
    void ShowCocoons();
    void OpenDoorForCreature(int door);
    void BashDoor(int door, const Creature& creature);
    void DrawBrainInspector();
    void DrawCreatureOverlays(DebugDraw& draw);
    void RegisterCreatureCommands();
    void SyncDynamicProps();
    void SpawnProp(bool sphere, float impulse);
    void ClearProps();

    void SampleLook(float dt);
    PlayerInput BuildPlayerInput();
    void TryInteract();
    // Every weapon's reload times from its model's reload clips, so an animation made in the editor
    // takes as long in the game as it does in the editor. At start, and after the editor saves.
    void ApplyWeaponClipTimings();
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
    bool m_editorReloadEmpty = false;
    // The first-person preview plays whatever clip the timeline has open, at the playhead, so a key
    // can be judged from the eyes it will be seen through while it is being set.
    bool m_benchFollowTimeline = true;
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
    // Its use, shaped on the bench: playing (seconds in, or negative when not), which half of it, and
    // where the scrubber stands when it is not playing.
    float m_benchUseTime = -1.0f;
    bool m_benchUseSecond = false;
    float m_benchUseScrub = 0.0f;

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
    // What somebody is holding, as this machine knows it: our own weapon, or the one the host was
    // told a remote player has out. Null for an empty hand or somebody we cannot find.
    const WeaponDefinition* WeaponHeldBy(uint8_t player) const;
    // A sound by the name of its folder in Assets/Audio; silence for one that is not there.
    const SoundVariants& Sounds(const std::string& name) const;
    const SoundVariants& GunshotFor(const WeaponDefinition* weapon) const;
    // Where a round starts: just in front of the eye, so what is under the crosshair is what is hit.
    // Not where the barrel is drawn -- see DrawnMuzzle.
    glm::vec3 MuzzlePosition() const;
    // Where the barrel is drawn, which is where a tracer and anybody else's view of the shot start.
    glm::vec3 DrawnMuzzle() const;
    glm::vec2 AimAngles() const;
    void ApplyRecoilToView();
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
    // Whether there is any point asking for more: false when the weapon is already carrying full spares.
    bool CanTakeAmmunition() const;
    // Puts the rounds in, once whoever decides has decided.
    void GiveAmmunition(int crateIndex);
    void EnterHidingSpot(int index);
    void LeaveHidingSpot();
    // Health and stamina, bottom left. Bars rather than numbers: both are things to glance at.
    // Takes no ImGui types, so this header does not have to know that ImGui exists.
    void DrawCondition();
    void DrawHud();
    // Who else is in the game, down the side: name, connection and whether they are still up.
    void DrawPlayerList();
    void DrawInventoryPanel();
    // Dragging one slot of the bag onto another, with a weapon in the hands keeping its magazine.
    void MoveInventorySlot(int from, int to);
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

    // The browser: a list of games you can actually join, rather than a page of questions about
    // ports and addresses. Everything about how a game is reached belongs to the two lists it can
    // be in, not to the player.
    void DrawTitleBrowse();
    void DrawTitleHost();
    // What this machine's game is called in everybody else's list. Falls back to the player's own
    // name, because an unnamed row still needs the one thing that tells it apart: whose it is.
    std::string LobbyName() const;
    // Finding games. On your own network that is a beacon and costs nothing; everywhere else it is
    // a question to the lobby server, which knows every public game.
    void StartBrowsing();
    void StopBrowsing();
    void UpdateDiscovery(float frameDeltaSeconds);
    // Hosting. One button, and every way in at once: the network beacon, a code from the lobby
    // server, and the router asked to let people straight in. Goes to the lobby, where the host
    // waits for everybody and then starts the game.
    void StartHosting();
    bool LobbyServerConfigured() const;
    // Joining. By address, by code, or whichever of those was typed into the box; each says why if
    // it did not work.
    bool JoinAddress(const std::string& address, int port);
    bool JoinCode(uint32_t code);
    bool JoinTyped(const std::string& text);
    // A join by code, while it is still finding its way to the host: the lobby server introduces
    // this machine, and then the two punch through to each other before the game connects.
    void UpdateJoining(float frameDeltaSeconds);
    // Everybody together before the game: the code, who is here, and the host's Start button.
    void DrawLobby();
    void DrawLobbyPlayers();
    // The code and the other ways in, for the lobby and the pause menu.
    void DrawInvite();
    // Leaving the lobby for the game, for the host and everybody with it.
    void StartTheGame();
    void DrawSettings();
    void DrawKeyBindings();
    void UpdateRebinding();
    std::vector<std::string> ChangedActions() const;
    void DrawPauseMenu();
    // Where the weapon sits relative to the eye, for comparing the editor with the game.
    void ReportHold();
    // How far through a reload the local player is, 0 to 1. One place, because the pose and the wire
    // each had their own and only one of them was a fraction.
    float ReloadProgress() const;
    void DrawTitleScreen();
    void EnterWorld();
    // Takes the game to the creature lab, or back to the test map.
    void GoToMap(bool lab);
    void EnterEditor(const std::string& modelName);
    // A file dragged onto the window. A model opens the editor on itself; anything else says so
    // rather than being quietly ignored, because a file that vanishes when you drop it is worse
    // than one that is refused.
    void OnFileDropped(const std::string& path);
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
    // The same question asked honestly: false when there is nobody here by that number, rather than
    // quietly answering with our own position.
    bool PlayerPositionIfKnown(uint8_t player, glm::vec3& out) const;
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
    // `killer` is a player id, or kNoKiller when it was not a player. `cause` is for the log.
    static constexpr uint8_t kNoKiller = 0xFF;
    void ApplyPlayerDamage(uint8_t player, float amount, uint8_t killer, const glm::vec3& direction,
                           const char* cause = "gunfire");
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

    // --- The sounds of things happening (PredationGameSounds.cpp) -------------------------------------
    static uint16_t SoundKey(const std::string& name);
    void PlayNamed(const std::string& name, const glm::vec3& at, float gain = 1.0f, float pitch = 1.0f,
                   bool positioned = true);
    // Heard here and, when hosting, by everybody else: for the sounds nothing else already carries.
    void ShareSound(const std::string& name, const glm::vec3& at, float gain = 1.0f);
    void HearSharedSound(const WorldEventMessage& event);
    // A sound from a player: in your own head when it is yours, from their body when it is not.
    void PlayerSound(uint8_t player, const std::string& name, float gain);
    void QueueSound(float delay, const std::string& name, const glm::vec3& at, float gain, bool positioned = true);
    void PlayReloadCues(const WeaponDefinition* weapon, bool empty, float from, float to, const glm::vec3& at,
                        bool positioned, float gain, int player);
    void UpdateWorldSounds(float dt);
    void UpdateCreatureSounds(float dt);
    void UpdateAmbience(float dt);
    // The front end's small sounds: the pointer finding a button, a press, a menu opening and closing.
    void MenuSounds();
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
    // The loopback test: the microphone through the same gate, played back out of the speakers.
    void UpdateMicrophoneTest(float dt);
    void StopTalking();
    // Plays or updates a speaker's stream. `at` is where they are, in the world.
    void HearVoice(uint8_t speaker, const std::vector<uint8_t>& frame);
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
    // Where this frame is drawn from: the eye, the free camera, or whoever is being spectated.
    glm::vec3 m_renderEye{0.0f};
    // And which way the picture looks: where the ears face.
    glm::vec3 m_renderForward{0.0f, 0.0f, -1.0f};

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
        // Which way the surface it hit faces, so the hole can be laid flat on it.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        float age = 0.0f;
        // Whether what it struck was a fixed surface. A round that stops in a person leaves no
        // hole: they walk away and it would hang in the air where they were standing.
        bool surface = false;
        // What it struck, so a mark left on something that moves can move with it. Invalid when the
        // round landed on the level itself, which needs no following.
        BodyHandle body;
        // Set once the round has arrived and its hole has been placed, so it is placed once rather
        // than every frame for as long as the tracer lives.
        bool marked = false;
        // Where on the body it struck, in that body's own frame at the moment of the shot.
        //
        // The hole goes down when the drawn round arrives, which is a few frames after the shot was
        // resolved -- and the shove from the hit is applied at once. A ball struck by a round has
        // already rolled away by the time its hole is placed, and a hole placed at the world point
        // where the ball used to be hangs beside it for the rest of its life. Kept in the body's
        // frame, the point goes wherever the body went.
        bool anchored = false;
        glm::vec3 localTo{0.0f};
        glm::vec3 localNormal{0.0f, 1.0f, 0.0f};
    };
    // Records where a tracer's round struck in the frame of the body it struck, if that body moves.
    // Called as a tracer is made, before anything has had a chance to shove the body.
    void AnchorTracer(Tracer& tracer) const;
    std::vector<Tracer> m_tracers;
    // Heard where a round landed: stone and steel, or flesh.
    void PlayImpact(const Tracer& tracer);

    // Bullet holes, as a ring of entities that are moved rather than created and destroyed.
    //
    // A hole is a mark on a wall and it should stay there: an impact that flashes and vanishes says
    // a round arrived, and a hole says a round arrived and where, which is the thing a player reads
    // a room with. Fixed count because they must not grow without limit over a long match -- the
    // oldest is reused, so a wall somebody empties a magazine into keeps the last of them.
    static constexpr size_t kMaxBulletHoles = 96;
    std::vector<Entity> m_bulletHoles;
    // What each hole is stuck to, and where on it.
    //
    // A hole on a wall never moves, and for a long time every hole was treated that way. Shoot a
    // crate or a dropped weapon and the mark stayed at the point in the world where the round
    // arrived while the thing it hit slid out from under it -- a black disc hanging in the air. So a
    // hole now remembers the body it landed on and where it sits in that body's own frame, and rides
    // it. An invalid handle means it landed on the level itself and needs no further thought.
    struct HoleAttachment
    {
        BodyHandle body;
        glm::vec3 localPosition{0.0f};
        glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::vector<HoleAttachment> m_bulletHoleAttachments;
    size_t m_nextBulletHole = 0;
    // The crater every hole starts from, and one mesh per slot in the ring.
    //
    // One shared mesh used to do for all of them, because every hole was the same flat disc. A hole
    // that wraps what it landed on is a different shape on every surface, so each slot owns its own
    // and it is rewritten when the slot is reused -- sixty-one vertices, a few times a second at
    // most, against a ring that never grows.
    MeshData m_bulletHoleShape;
    std::vector<MeshHandle> m_bulletHoleMeshes;
    void PlaceBulletHole(const glm::vec3& at, const glm::vec3& normal, BodyHandle on = {});
    void ClearBulletHoles();
    void FollowBulletHoles();
    InteractionSystem m_interactions;
    WorldObjects m_world;
    // -1 when not hidden. While hidden the player holds still inside the locker.
    // What is carried in the hand when it is not a weapon.
    ItemId m_heldItem = kInvalidItem;

    // --- Using what is in the hand (PredationGameItems.cpp) -------------------------------------
    //
    // Fire, with an item in the hand rather than a weapon, uses it: a medical kit patches you up, a
    // battery goes into the torch, a keycard opens a locked door, a flare is struck and then thrown,
    // and a sample container is turned over and looked at. Each use takes a while and moves the hand,
    // which everybody sees; what it does is the host's to decide.
    struct ItemUseState
    {
        bool active = false;
        ItemId item = kInvalidItem;
        bool second = false;   // the second half of a use: a lit flare being thrown
        bool released = false; // the flare has left the hand
        float time = 0.0f;
        float seconds = 1.0f;
        uint8_t target = 0xFF; // who is being patched up, 0xFF for yourself
        uint8_t door = 0xFF;   // which door is being opened
    };
    ItemUseState m_itemUse;
    void UpdateItemUse(float dt);
    void StartItemUse(const ItemDefinition& item);
    void FinishItemUse();
    void StopItemUse();
    // The host deciding what a use did, for anybody: patching somebody up, opening a door, a flare
    // lit or thrown, one of something used up.
    void HandleItemUse(uint8_t player, const ItemUseMessage& use);
    void ApplyHeal(uint8_t user, uint8_t target, float amount);
    void UnlockDoor(int door, uint8_t player);
    void OnItemUsedEvent(const WorldEventMessage& event);
    void TellItemUse(uint8_t phase, const ItemDefinition& item, float amount = 0.0f);
    // Other people's uses in progress, for their hands, and who is holding a lit flare and for how long.
    struct RemoteUse
    {
        ItemId item = kInvalidItem;
        bool second = false;
        float time = 0.0f;
        float seconds = 1.0f;
    };
    std::map<uint8_t, RemoteUse> m_remoteUses;
    std::map<uint8_t, float> m_flareHeldBy;
    void UpdateRemoteItemUse(RemoteAvatar& avatar, uint8_t id, float dt);
    // The torch's cell: 1 full, 0 flat. It runs down while the torch is on, dims as it goes, and a
    // battery fills it again.
    float m_torchCharge = 1.0f;
    float TorchStrength() const;
    // Seconds left on the flare burning in this player's own hand, 0 when there is none.
    float m_flareBurn = 0.0f;
    // Flares burning where they were thrown, on every machine.
    struct BurningFlare
    {
        Entity entity;
        BodyHandle body;
        float burn = 0.0f;
        float spent = 0.0f;
        float noiseIn = 0.8f;
        VoiceId hiss = kInvalidVoice;
    };
    std::vector<BurningFlare> m_burningFlares;
    MeshHandle m_flareMesh;
    void ThrowFlare(uint8_t player, const glm::vec3& from, const glm::vec3& velocity, float burn, bool announce);
    void UpdateFlares(float dt);
    void ClearFlares();
    void GatherItemLights(std::vector<PunctualLight>& lights) const;
    // How lit a point is by flares, burning or held, 0 to 1: what a creature sees by.
    float FlareLightAt(const glm::vec3& point) const;
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
    // A fire press seen by the frame, held until the next fixed tick has used it.
    bool m_firePressLatch = false;
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
    // A lean held from the console, for testing without a keyboard. Off unless `lean` set it.
    bool m_forceLeanOn = false;
    float m_forceLean = 0.0f;
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
        // Whether their torch is lit, straight off the wire, so the light it throws is in this
        // machine.s scene too. See where the environment lights are chosen.
        bool torchOn = false;
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
    // A settings tab asked for from outside, pushed into the tab bar once. Empty when none is.
    std::string m_settingsTab;
    // Which page of the menu is showing. There used to be four, one per way of reaching a game,
    // and the player had to know which of them their situation was before they could look at
    // anything. Now there are two: a list of games, and the page for making one.
    enum class TitlePage : uint8_t
    {
        Root,
        Browse,
        Host
    };
    TitlePage m_titlePage = TitlePage::Root;
    // Which list is showing: public games from the lobby server, or games on this network.
    bool m_online = false;
    // Which of the two the tab bar showed last frame, so a change made elsewhere can be pushed to it.
    bool m_onlineShown = false;
    // Finding games on this network, and the beacon that puts this machine in everybody else's
    // list while it is hosting one.
    LanListener m_browser;
    LanBeacon m_beacon;
    // Asking the lobby server what public games are open. Web requests only: browsing has to work
    // before there is a game, and stops the moment there is one.
    LobbyClient m_lobbyBrowser;
    // This machine's lobby: the host's registration and introductions, or a guest's way in.
    LobbyClient m_lobby;
    // A guest's transport while it is still finding the host, before the game has it. The lobby
    // server has seen this socket and the holes are punched for it, so it is this one the game
    // connects over, not a new one.
    std::unique_ptr<Transport> m_joinTransport;
    // What this machine's game is called in other people's lists, and whether it is in the public
    // one or only reachable by its code.
    char m_lobbyName[24] = "";
    bool m_listPublicly = false;
    // Which games on the network have already been mentioned in the log, so appearing and going
    // are each said once rather than every frame.
    std::vector<std::string> m_seenOnLan;
    // Asks the router to let people straight in, as one more way to reach a host whose router
    // cannot be punched through. Best effort and silent: the code is the way in either way.
    PortMapper m_ports;
    bool m_portsAnnounced = false;
    // The host is in the lobby, waiting for everybody, and has not started the game.
    bool m_inLobby = false;
    // The flashlight, and where its beam is currently pointing -- which is not quite where the
    // player is looking, because it trails the view and catches up. `m_torchAimed` is false until
    // the first frame it is on, so switching it on snaps the beam to the view instead of sweeping
    // it across the room from wherever it was left.
    bool m_micTest = false;
    // Whether the microphone.s output is actually leaving this machine this frame, as opposed to the
    // microphone merely being open. The two are different whenever the gate is shut, and the
    // difference is the thing somebody setting a threshold needs to see.
    // Whether anything has gone out recently, rather than whether anything went out this frame.
    //
    // A voice frame is twenty milliseconds and the game draws at two hundred a second, so a packet
    // leaves on about one frame in twelve. Read per frame, the label under the meter said "sending"
    // for one frame and "open, gate shut" for eleven, which at that rate is not a flicker anybody
    // resolves as a flicker -- it reads as two different labels drawn on top of each other, both
    // half faded. That is what "I can see the text under it fade and see 2 text at once" was.
    //
    // Held for a fifth of a second after the last packet, which is longer than the gap between them
    // and far shorter than anybody's idea of "just now".
    float m_voiceSendingFor = 0.0f;
    bool m_voiceSending = false;
    // Which action is waiting for a key, empty when none is. A name rather than an index, so it
    // cannot go stale if the list is reordered.
    std::string m_rebinding;
    StreamId m_micTestStream = kInvalidStream;
    bool m_torchOn = false;
    bool m_torchAimed = false;
    glm::vec3 m_torchAim{0.0f, 0.0f, -1.0f};

    // What was pasted into the join box: a lobby code, or an address for a game on this network.
    char m_joinInput[1400] = "";
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
        SoundVariants gunshot;
        SoundVariants dryFire;
        SoundVariants reloadOut;
        SoundVariants reloadIn;
        SoundVariants step;
        SoundVariants land;
        SoundVariants door;
        SoundVariants locker;
        SoundVariants pickup;
        SoundVariants drop;
        SoundVariants hurt;
        SoundVariants death;
    };
    SoundSet m_sounds;
    // Every other folder in Assets/Audio, by its name.
    std::map<std::string, SoundVariants> m_soundBank;
    // The same names by the key a Sound event carries.
    std::map<uint16_t, std::string> m_soundKeys;
    struct QueuedSound
    {
        float at = 0.0f;
        std::string name;
        glm::vec3 position{0.0f};
        float gain = 1.0f;
        bool positioned = true;
    };
    std::vector<QueuedSound> m_queuedSounds;
    float m_soundClock = 0.0f;
    // What each other player was doing last frame, so the sounds of it changing can be made here.
    struct HeardPlayer
    {
        bool known = false;
        bool reloading = false;
        float reload = 0.0f;
        uint8_t held = 0;
        bool torch = false;
        bool grounded = true;
        float fall = 0.0f;
    };
    std::map<uint8_t, HeardPlayer> m_heardPlayers;
    // And the same for this player's own hands and lungs.
    WeaponId m_soundWeapon = kInvalidWeapon;
    bool m_soundReloading = false;
    float m_soundReload = 0.0f;
    bool m_soundGrounded = true;
    bool m_exhausted = false;
    float m_breathAt = 0.0f;
    bool m_dryTriggerWas = false;
    // Each creature as it was last heard: what its body was doing, how hurt, and when it next breathes.
    struct HeardCreature
    {
        bool known = false;
        RigAction action = RigAction::None;
        float health = 1.0f;
        bool alive = true;
        float breathAt = 0.0f;
        float voiceAt = 0.0f;
        float hurtAt = -10.0f;
        Behavior doing = Behavior::Roam;
    };
    std::map<uint8_t, HeardCreature> m_heardCreatures;
    std::vector<float> m_crateLids;
    // The level's own lamps, and the buzz of the nearest failing one.
    LevelLights m_levelLights;
    float m_lightClock = 0.0f;
    // The main camera's view and projection, last frame: for putting HUD text on things in the world.
    glm::mat4 m_viewProjection{1.0f};
    VoiceId m_buzz = kInvalidVoice;
    // The loops that make up where you are, each faded in by how much you are there: a room, the air in
    // its ducts, the open air, the inside of a vent, a nest. See UpdateAmbience.
    struct AmbienceLoop
    {
        const char* name;
        float gain;          // at full strength
        VoiceId voice = kInvalidVoice;
        float weight = 0.0f; // how much of it is heard now, eased
    };
    AmbienceLoop m_ambienceLoops[5] = {{"Ambience/room_tone", 0.22f},
                                       {"Ambience/vent", 0.12f},
                                       {"Ambience/wind", 0.3f},
                                       {"Ambience/vent_close", 0.32f},
                                       {"Ambience/nest", 0.45f}};
    // Where the listener is, measured a few times a second: under a roof, in somewhere tight, how near a
    // nest.
    float m_zoneProbeAt = 0.0f;
    float m_zoneIndoor = 1.0f;
    float m_zoneTight = 0.0f;
    float m_zoneNest = 0.0f;
    float m_ambienceClock = 0.0f;
    float m_ambienceNext = 25.0f;
    unsigned int m_menuHovered = 0;
    bool m_menuWasPaused = false;
    int m_menuWasScreen = -1;

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
        // Where their body was the last time we could find it. A speaker we cannot place must not be
        // played at our own head, which is the loudest place there is: see SpeakerPosition.
        glm::vec3 at{0.0f};
        bool located = false;
    };
    std::vector<std::unique_ptr<Speaker>> m_speakers;
    // Where to play a speaker from, remembering the last place we could put them.
    glm::vec3 SpeakerPosition(Speaker& speaker) const;
    // Speakers from this number up are creatures saying back what they heard: the creature's own number
    // added to it. Players are under it.
    static constexpr uint8_t kCreatureSpeaker = 32;

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
