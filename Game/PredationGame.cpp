#include "Game/PredationGame.h"
#include "Game/OverlayLayer.h"
#include "Game/PredationGameShared.h"

#include "Engine/Render/DepthConvention.h"
#include "Game/Creature/CreatureTuning.h"

#include "Engine/Core/BuildInfo.h"
#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Assets/GltfImport.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/World/Atmospherics.h"
#include "Game/World/TestMap.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/Surfaces.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Debug/FrameStats.h"
#include "Engine/Platform/Window.h"
#include "Engine/Audio/SoundDesign.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <limits>
#include <string>

namespace pred
{

extern CVar<float> cv_ambienceVolume;
// Every recording of a sound that is on disk.
//
// The folder is the registration. Assets/Audio/<name>/ holding any number of wav files means the
// game plays those and picks a different one each time; an empty folder, or no folder, means that
// sound is silent. Nothing has to be listed anywhere and nothing has to be rebuilt, which is the
// point: a sound can be replaced by somebody who does not build the game, and adding a second and
// third file is the whole of how a sound stops repeating.
//
// Trailing silence is trimmed, as it is for footsteps, because a downloaded pack usually carries
// some and it delays every sound after it in a queue by however long it is.
SoundVariants LoadSoundVariants(AudioEngine& audio, const char* name)
{
    SoundVariants variants;
    const std::filesystem::path folder = Paths::AssetsRoot() / "Audio" / name;
    std::error_code ec;
    if (std::filesystem::is_directory(folder, ec))
    {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            std::string extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".wav")
            {
                files.push_back(entry.path());
            }
        }
        // Sorted, so the order a machine happens to list a directory in cannot change which sound
        // is which between two runs on two machines.
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& path : files)
        {
            std::ifstream wav(path, std::ios::binary);
            if (!wav)
            {
                continue;
            }
            const std::string bytes((std::istreambuf_iterator<char>(wav)),
                                    std::istreambuf_iterator<char>());
            SoundData data;
            std::string error;
            if (!LoadWav(bytes.data(), bytes.size(), data, error))
            {
                PRED_LOG_WARN(Gameplay, "{}: {}", path.string(), error);
                continue;
            }
            TrimTrailingSilence(data);
            const SoundId id =
                audio.Add(std::string("recorded/") + name + "/" + path.filename().string(),
                          std::move(data));
            if (id != kInvalidSound)
            {
                variants.ids.push_back(id);
            }
        }
    }

    if (!variants.ids.empty())
    {
        PRED_LOG_INFO(Gameplay, "{}: {} recording(s) from Assets/Audio/{}", name, variants.ids.size(),
                      name);
        return variants;
    }
    // No file, no sound, and said so rather than quietly substituting something.
    //
    // This used to fall back to a recipe synthesised at startup, which meant a missing or misnamed
    // file was inaudible as a problem: the game made a noise, just not the one anybody had put
    // there. Silence and a line in the log is the honest version, and it is the one that tells
    // somebody replacing a sound that they have put it in the wrong place.
    PRED_LOG_WARN(Gameplay, "No wav files in Assets/Audio/{}; that sound will be silent", name);
    return variants;
}


bool PredationGame::OnInit(Application& app)
{
    m_app = &app;

    // The surface texture sets, for the maps to lay on as they are built.
    Surfaces::SetLibrary(&app.GetTextures());
    // ... and every one the last start used, begun reading side by side now, so building the maps below waits on them only
    // as far as it has to.
    Surfaces::PrefetchRemembered(Paths::UserDataDir() / "Cache" / "surfaces.txt");
    // (And a loading screen saying how far along it is, rather than an empty window, until the first frame.)
    app.ShowLoading("Loading", 0.05f);
    BuildTestMap(m_scene, app.GetMeshes(), &app.GetPhysics());
    // And the creature lab, far off to the east in the same world, with its nest.
    BuildLabMap(m_scene, app.GetMeshes(), &app.GetPhysics(), &m_levelLights);
    app.ShowLoading("Loading the ship", 0.2f);
    // And the ship, off on its own, where everybody is between deployments.
    m_ship.Build(m_scene, app.GetMeshes(), app.GetPhysics(), &m_levelLights);
    app.ShowLoading("Loading the world", 0.4f);
    // And the generated facility between them, the default one until a host asks for another.
    m_facility.Build(SiteSpec::kDefaultSeed, m_scene, app.GetMeshes(), app.GetPhysics(), &m_levelLights);
    app.ShowLoading("Loading", 0.6f);


    m_propSphereMesh = app.GetMeshes().Upload(Primitives::Sphere(kPropRadius, 20, 14), "prop_sphere");
    m_propBoxMesh = app.GetMeshes().Upload(Primitives::Box(glm::vec3(kPropSize)), "prop_box");

    PlayerConfig config;
    config.LoadFromFile(PlayerConfigPath());
    if (!m_player.Init(app.GetPhysics(), config, m_spawnPoint))
    {
        PRED_LOG_ERROR(Gameplay, "Player controller failed to initialize");
        return false;
    }
    // The water to swim in (kept up to date as sites come and go: UpdateWater), for this body and the host's of everybody else's.
    m_player.SetWater(&m_swimWater);
    m_host.SetWater(&m_swimWater);

    // The pool of bullet holes, made once and moved about thereafter.
    //
    // Creating an entity per round would grow the scene without limit across a match and put a mesh
    // upload on the frame a trigger is pulled. A fixed ring costs its whole size up front, never
    // allocates again, and the oldest hole quietly becomes the newest.
    {
        // Each slot has a mesh of its own, because a hole that wraps what it landed on is a
        // different shape on every surface. They start as the flat crater, which is also what any
        // of them is before it is first used.
        m_bulletHoleShape = BuildBulletHoleShape();
        const Material holeMaterial = Material::Diffuse({0.05f, 0.045f, 0.04f}, 0.95f);
        m_bulletHoles.reserve(kMaxBulletHoles);
        m_bulletHoleMeshes.reserve(kMaxBulletHoles);
        m_bulletHoleAttachments.assign(kMaxBulletHoles, HoleAttachment{});
        for (size_t i = 0; i < kMaxBulletHoles; ++i)
        {
            const MeshHandle mesh =
                app.GetMeshes().Upload(m_bulletHoleShape, "bullet_hole_" + std::to_string(i));
            m_bulletHoleMeshes.push_back(mesh);
            const Entity hole =
                m_scene.CreateMeshEntity("bullet_hole", Transform{}, mesh, holeMaterial);
            if (MeshRenderer* renderer = m_scene.GetMeshRenderer(hole))
            {
                // Hidden until it is used, and never a shadow caster: a decal lying on a wall would
                // otherwise shadow the wall it is lying on.
                renderer->visible = false;
                renderer->castsShadow = false;
            }
            m_bulletHoles.push_back(hole);
        }
    }

    m_body.Build(m_scene, app.GetMeshes(), m_player.Config());
    m_body.SetTextureLibrary(app.GetTextures());

    app.ShowLoading("Loading sounds", 0.7f);
    // Every sound the game makes, from a file on disk. Nothing is generated at run time any more.
    //
    // They used to be built from recipes in sounds.json at startup, which was the right call while
    // there was nobody to record anything and the wrong one to keep: a recipe cannot be opened in
    // an editor, sent to anybody, or replaced without learning what `bite` means. Every sound is now
    // a wav in Assets/Audio/<name>/, and replacing one is dropping a file on top of another file --
    // no build, no JSON, no restart beyond the next run.
    //
    // The recipes and the synthesiser are still in the engine, reached only by the `sound_bake`
    // console command that produced these files. That is where a generated sound belongs: in the
    // thing that makes the placeholder, not in the thing that ships.
    {
        AudioEngine& audio = app.GetAudio();
        // Every folder in Assets/Audio holding wavs is a sound, named by where it is: Creature/growl is
        // Assets/Audio/Creature/growl/. A new sound is a new folder and nothing else. The footsteps are
        // their own thing, listed in footsteps.json by surface.
        std::error_code ec;
        const std::filesystem::path root = Paths::AssetsRoot() / "Audio";
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec || !it->is_directory())
            {
                continue;
            }
            const std::string name = std::filesystem::relative(it->path(), root, ec).generic_string();
            if (name.rfind("Footsteps", 0) == 0)
            {
                it.disable_recursion_pending();
                continue;
            }
            bool holdsSound = false;
            for (const auto& file : std::filesystem::directory_iterator(it->path(), ec))
            {
                holdsSound = holdsSound || (file.is_regular_file() && file.path().extension() == ".wav");
            }
            if (!holdsSound || m_soundBank.count(name) != 0)
            {
                continue;
            }
            m_soundBank.emplace(name, LoadSoundVariants(audio, name.c_str()));
            m_soundKeys[SoundKey(name)] = name;
        }
        // And the handful reached for every frame, by name once here rather than looked up each time.
        m_sounds.gunshot = Sounds("Weapons/shot");
        m_sounds.dryFire = Sounds("Weapons/dry_fire");
        m_sounds.reloadOut = Sounds("Weapons/mag_out");
        m_sounds.reloadIn = Sounds("Weapons/mag_in");
        m_sounds.step = Sounds("Player/step");
        m_sounds.land = Sounds("Player/land");
        m_sounds.door = Sounds("World/door");
        m_sounds.locker = Sounds("World/locker");
        m_sounds.pickup = Sounds("World/pickup");
        m_sounds.drop = Sounds("World/drop");
        m_sounds.hurt = Sounds("Player/hurt");
        m_sounds.death = Sounds("Player/death");
    }

    LoadFootsteps(app.GetAudio());

    app.ShowLoading("Loading equipment", 0.85f);
    m_items.LoadFromFile(Paths::AssetsRoot() / "Data" / "items.json");
    // Weapons load before the icons, because a weapon item draws its icon from the weapon's own
    // model and would otherwise fall back to the placeholder block.
    m_weaponData.LoadFromFile(Paths::AssetsRoot() / "Data" / "weapons.json");
    ApplyWeaponClipTimings();
    // The devices' screens first: their icons show them.
    BuildDeviceFaces();
    m_itemIcons.Build(m_items, app.GetMeshes(), app.GetRenderer(), &m_weaponData, &app.GetTextures());
    m_world.SetTextures(app.GetTextures());
    m_world.Build(m_scene, app.GetMeshes(), app.GetPhysics(), m_interactions, m_items, &m_weaponData);
    // A game starts aboard.
    m_spawnPoint = m_ship.Spawn(0);
    m_spawnYaw = m_ship.SpawnYaw();
    // The ship's deployment console, standing in the testing area until there is a ship -- before the walkable surface is
    // worked out, which has to go round it -- and what sites are called and what the intercom says.
    BuildDeployConsole();
    BuildHelm();
    BuildAirlockControls();
    BuildLoadoutLocker();
    BuildBriefingScreens();
    LoadBriefing();
    BuildShipControls();
    // Somebody joining is made where they belong now: aboard, at the site, or at the spawn.
    m_host.SetSpawnFor([this](uint8_t player) { return ArrivalFor(player); });
    LoadMissionData();
    LoadUniverseData();
    m_planets.Init(app.GetShaders());
    m_sprites.Init(app.GetShaders());
    m_water.Init(app.GetShaders());
    // What is blended over the picture, drawn in the order it is given: the water, then the weather over it.
    bgfx::setViewMode(Renderer::kViewTranslucent, bgfx::ViewMode::Sequential);
    // The light's shadows -- the sun's, the torch's, a roof keeping the sky out -- are not settings: an older settings file
    // that turned one off (the old Low preset did) has it on again.
    cv_sunShadows.Set(true);
    cv_skyShadows.Set(true);
    cv_spotShadows.Set(true);
    // And anything saved from before the map was made bigger comes up to the shortest the settings offer: 24 m of shadow
    // in a level you can see a hundred metres across reads as shadows simply not working.
    if (cv_shadowDistance.Get() < 40.0f)
    {
        cv_shadowDistance.Set(40.0f);
    }
    LoadCinematics();
    app.GetPhysics().OptimizeBroadPhase();
    // The walkable surface the creature moves over, worked out from the level's solid geometry.
    // Once, here: the level does not change shape, and building it takes a noticeable fraction of
    // a second.
    BuildNavigation();

    // Checked once the whole level exists, so a piece placed on top of another is caught here
    // rather than by walking into it. 10 mm is below anything anyone would notice.
    ReportMapOverlaps(0.01f);

    // Editing player.json on disk applies immediately, without a rebuild or a restart.
    app.GetFileWatcher().Watch(PlayerConfigPath(),
                               [this](const std::filesystem::path&) { ReloadPlayerConfig(); });
    // And creatures.json: what every creature's mind shares, tuned while watching one.
    {
        const std::filesystem::path tuningFile = Paths::AssetsRoot() / "Data" / "creatures.json";
        const auto reloadTuning = [tuningFile](const std::filesystem::path&)
        {
            CreatureTuning tuning;
            if (LoadCreatureTuning(tuningFile, tuning))
            {
                SetTuning(tuning);
                PRED_LOG_INFO(AI, "Creature tuning read from {}", tuningFile.string());
            }
        };
        reloadTuning(tuningFile);
        app.GetFileWatcher().Watch(tuningFile, reloadTuning);
    }

    // Yaw 0 looks down -Z, which is where the test map is from the spawn point.
    m_lookYaw = 0.0f;
    m_lookPitch = 0.0f;
    m_camera.position = {0.0f, 3.2f, 22.0f};

    m_editor.Init(app);

    RegisterCommands();
    RegisterNetCommands();
    RegisterCreatureCommands();
    RegisterDroneCommands();
    RegisterMissionCommands();
    RegisterCinematicCommands();
    RegisterShipCommands();
    RegisterLoadoutCommands();
    RegisterCampaignCommands();
    // The game opens at the menu, with the world already built behind it.
    std::snprintf(m_joinAddress, sizeof(m_joinAddress), "%s", cv_lastAddress.Get().c_str());
    std::snprintf(m_playerName, sizeof(m_playerName), "%s", cv_playerName.Get().c_str());
    m_joinPort = std::clamp(cv_lastPort.Get(), 1024, 65535);
    ReturnToTitle();
    UpdateMouseCapture();

    PRED_LOG_INFO(Gameplay,
                  "Controls: WASD move, Space jump, Ctrl or C crouch, Z prone, Shift sprint, Alt "
                  "walk, Q and E lean. Left mouse fires, right mouse aims, R reloads. "
                  "F interact, G drop, 1 to 6 and the wheel select, Tab inventory. "
                  "P cycles first person, third person and free camera; in third person hold middle "
                  "mouse to orbit. F3 overlay, F5 respawn, backtick console, "
                  "Escape frees the cursor.");
#if PRED_DEV_TOOLS
    PRED_LOG_INFO(Gameplay, "Developer build: the model editor is on the title screen, or type "
                            "\"editor\" in the console.");
#endif
    if (!cv_crouchToggle.Get())
    {
        PRED_LOG_INFO(Gameplay, "Crouch and prone are hold-to-activate. Set input.crouch_toggle to "
                                "true in the console to make them toggle instead.");
    }
    app.ShowLoading("Ready", 1.0f);
    // What this start read, for the next to begin reading at once (PrefetchRemembered).
    Surfaces::RememberUsed(Paths::UserDataDir() / "Cache" / "surfaces.txt");
    return true;
}

void PredationGame::ReloadPlayerConfig()
{
    PlayerConfig config;
    if (config.LoadFromFile(PlayerConfigPath()))
    {
        m_player.Config() = config;
        m_player.ApplyConfigToCharacter();
    }
}

// --- World replication -------------------------------------------------------------------------
//
// One rule decides every question here: the host owns the world. A client that opens a door has
// asked to open it, and what it draws is what the host sends back. Offline the local player is the
// host, so single player and hosting are the same code with nobody to send to.

uint8_t PredationGame::LocalPlayerId() const
{
    return m_sessionMode == SessionMode::Client ? m_client.PlayerId() : 0;
}

bool PredationGame::PerformInteraction(InteractionKind kind, int index, uint8_t player)
{
    switch (kind)
    {
    case InteractionKind::Door:
    {
        if (!m_world.ToggleDoor(index, m_interactions))
        {
            // Locked: only a keycard in the hand, swiped at it, opens it (StartItemUse).
            if (const WorldObjects::Door* locked = m_world.GetDoor(index); locked != nullptr && locked->locked)
            {
                ShareSound("World/door_locked", locked->hinge + glm::vec3(0.0f, 1.0f, 0.0f), 0.8f);
            }
            return false;
        }
        // Heard by whoever did it, which is where every interaction sound in this function was
        // missing from.
        //
        // The sounds existed and were only ever played from the network event handlers -- the code
        // that runs on a machine being *told* something happened. The machine that actually did it
        // never runs those, so the player opening a door heard nothing, and in a solo game, where
        // there are no events at all, nothing made any sound ever. A client is not double-served
        // either: it sends a request and returns without performing, so it only hears the event.
        if (const WorldObjects::Door* opened = m_world.GetDoor(index); opened != nullptr)
        {
            PlaySound(m_sounds.door.Pick(), opened->hinge, 0.8f);
            MakeNoise(NoiseKind::Door, opened->hinge, NoiseReach::kDoor, player);
        }
        if (m_sessionMode == SessionMode::Host)
        {
            const WorldObjects::Door* door = m_world.GetDoor(index);
            WorldEventMessage event;
            event.kind = WorldEventKind::DoorMoved;
            event.index = static_cast<uint8_t>(index);
            event.flag = door != nullptr && std::abs(door->target - door->closedYaw) > 1e-3f;
            m_host.Broadcast(event);
        }
        return true;
    }

    case InteractionKind::Pickup:
    {
        WorldObjects::Pickup* pickup = m_world.GetPickup(index);
        if (pickup == nullptr || !pickup->alive)
        {
            return false;
        }
        // Only the player who asked gets it in their bag. Everyone else just sees it disappear,
        // which is all there is to see from outside.
        // Taken before the pickup is consumed, because consuming it destroys the entity this reads.
        glm::vec3 pickupAt{0.0f};
        if (const Transform* where = m_scene.GetTransform(pickup->entity); where != nullptr)
        {
            pickupAt = where->position;
        }
        const bool mine = player == LocalPlayerId();
        const int wanted = pickup->count;
        int stored = wanted;
        if (mine)
        {
            // The magazine comes with it, so a rifle dropped with three rounds left is picked up
            // with three rather than refilled by the act of taking it.
            stored = m_inventory.Add(m_items, pickup->item, pickup->count, pickup->rounds,
                                     pickup->reserve);
            if (stored <= 0)
            {
                m_app->GetConsole().Print("Inventory full");
                return false;
            }
        }

        if (stored >= wanted)
        {
            const ItemId taken = pickup->item;
            PlaySound(m_sounds.pickup.Pick(), pickupAt, 0.6f);
            MakeNoise(NoiseKind::Item, pickupAt, NoiseReach::kItem * 0.5f, player);
            m_world.ConsumePickup(index, m_scene, m_app->GetPhysics(), m_interactions);
            if (m_sessionMode == SessionMode::Host)
            {
                // Remembered, so the same client cannot later put down more than it took.
                if (player != 0)
                {
                    m_host.NoteCarried(player, static_cast<uint16_t>(taken), wanted);
                }
                WorldEventMessage event;
                event.kind = WorldEventKind::PickupTaken;
                event.index = static_cast<uint8_t>(index);
                event.player = player;
                m_host.Broadcast(event);
            }
        }
        else
        {
            // Partial pickup: leave the remainder on the floor rather than silently eating it.
            pickup->count -= stored;
            if (Interactable* interactable = m_interactions.Find(pickup->entity))
            {
                const ItemDefinition* definition = m_items.Get(pickup->item);
                interactable->name = definition != nullptr
                                         ? definition->name + " x" + std::to_string(pickup->count)
                                         : "Item";
            }
        }
        return true;
    }

    case InteractionKind::HidingSpot:
    {
        // One interaction, both directions: get in if it is empty, get out if you are the one in it.
        //
        // It used to be "get in" only. Leaving was a local call that told nobody, so everybody else
        // kept the locker shut and occupied for the rest of the match, and the player who had been
        // inside it could never be seen to come out. Making it a toggle means the same message, the
        // same path and the same broadcast carry both halves.
        WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(index);
        if (spot == nullptr)
        {
            return false;
        }
        const bool leaving = spot->occupied && spot->occupant == player;
        if (spot->occupied && !leaving)
        {
            return false; // somebody else is in there
        }
        PlaySound(m_sounds.locker.Pick(), spot->insidePosition, 0.8f);
        // A locker door is a door. Getting in where it can hear is how to be found in there.
        MakeNoise(NoiseKind::Door, spot->insidePosition, NoiseReach::kDoor * 0.7f, player);

        if (leaving)
        {
            spot->occupied = false;
            spot->occupant = 0;
            m_world.SetDoorOpen(spot->doorIndex, true, m_interactions);
            m_interactions.SetVerb(spot->entity, "Hide in");
            if (player == LocalPlayerId())
            {
                LeaveHidingSpot();
            }
            else if (m_sessionMode == SessionMode::Host)
            {
                m_host.PinPlayer(player, false, spot->exitPosition, spot->insideYaw);
            }
        }
        else if (player == LocalPlayerId())
        {
            EnterHidingSpot(index);
            spot->occupant = player;
        }
        else
        {
            // Somebody else got in. The door swings and the locker is taken -- and, on the host, their
            // body is pinned inside it, because the host is what decides where they are. Without that
            // the host went on walking them about outside a locker everybody could see was shut, and
            // their own machine was pulled back out of it a moment later.
            spot->occupied = true;
            spot->occupant = player;
            m_world.SetDoorOpen(spot->doorIndex, false, m_interactions);
            if (m_sessionMode == SessionMode::Host)
            {
                m_host.PinPlayer(player, true, spot->insidePosition, spot->insideYaw);
            }
        }

        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::LockerUsed;
            event.index = static_cast<uint8_t>(index);
            event.player = player;
            event.flag = !leaving;
            m_host.Broadcast(event);
        }
        return true;
    }

    case InteractionKind::Cocoon:
    {
        // Cutting somebody out of a cocoon: only the host has cocoons to cut, and the payload is who.
        if (!IsAuthority())
        {
            return false;
        }
        FreeFromCocoon(static_cast<uint8_t>(index), player);
        return true;
    }

    case InteractionKind::AmmoCrate:
    {
        if (player == LocalPlayerId())
        {
            TakeAmmunition(index);
        }
        else if (!m_world.DrawFromAmmoCrate(index, m_interactions))
        {
            return false;
        }
        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::AmmoTaken;
            event.index = static_cast<uint8_t>(index);
            event.player = player;
            m_host.Broadcast(event);
        }
        return true;
    }

    case InteractionKind::Terminal:
    case InteractionKind::Breaker:
    case InteractionKind::Launch:
    case InteractionKind::Deploy:
    case InteractionKind::Board:
        return PerformMissionInteraction(kind, index, player);

    case InteractionKind::Generic:
    default:
        return false;
    }
}

// --- Damage ------------------------------------------------------------------------------------

void PredationGame::UpdateRespawns(float dt)
{
    // Death is a pause, not an end. Offline and as a host the clock is run here; a client is told
    // when it comes back, because whether it is alive is not its own decision to make.
    if (m_sessionMode == SessionMode::Client)
    {
        // Only the countdown on the screen. The host still says when.
        m_respawnTimer = std::max(m_respawnTimer - dt, 0.0f);
        return;
    }

    // Every death comes back, whatever caused it. A fall kills inside the player's own controller
    // rather than through KillPlayer, so it started no clock: somebody who fell to their death lay
    // there until they found the developer key, and a guest who did the same lay there for good.
    // So any body that is dead with no clock running is given one here, the same way as any other
    // death, and everybody is told.
    if (!m_player.State().alive && m_respawnTimer <= 0.0f)
    {
        KillPlayer(LocalPlayerId(), glm::vec3(0.0f));
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& other : m_host.Remotes())
        {
            const auto timer = m_remoteRespawnTimers.find(other.id);
            const bool clockRunning = timer != m_remoteRespawnTimers.end() && timer->second > 0.0f;
            if (!clockRunning && m_host.HealthOf(other.id) <= 0.0f)
            {
                KillPlayer(other.id, glm::vec3(0.0f));
            }
        }
    }

    // A client does not decide when it comes back, and this ran everywhere.
    //
    // Both ends counted the same seconds and each revived the player it owned, which agrees only
    // while both ends agree that the player died in the first place. A client that killed itself
    // locally, on fall damage from a climb the host had not run the same way, then revived itself
    // too: alive and playing on its own screen and lying on the floor for good on everybody else's.
    // Dying and coming back are the host's to say, like everything else about the world.
    if (m_sessionMode != SessionMode::Client && !m_player.State().alive && m_respawnTimer > 0.0f)
    {
        m_respawnTimer -= dt;
        if (m_respawnTimer <= 0.0f)
        {
            RespawnLocalPlayer(m_spawnPoint);
            if (m_sessionMode == SessionMode::Host)
            {
                WorldEventMessage event;
                event.kind = WorldEventKind::PlayerRespawned;
                event.player = 0;
                event.position = m_spawnPoint;
                m_host.Broadcast(event);
            }
        }
    }

    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }
    for (auto& entry : m_remoteRespawnTimers)
    {
        if (entry.second <= 0.0f)
        {
            continue;
        }
        entry.second -= dt;
        if (entry.second > 0.0f)
        {
            continue;
        }
        m_host.RespawnPlayer(entry.first, m_spawnPoint);
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerRespawned;
        event.player = entry.first;
        event.position = m_spawnPoint;
        m_host.Broadcast(event);
    }
}

void PredationGame::UpdateSpectating()
{
    // Dead, you watch a teammate through their own eyes. A free camera is deliberately not offered:
    // it would show you where the creature is, which is the one thing being dead should not tell
    // you. Cycling is by the interact key, because there is nothing else to press.
    if (m_player.State().alive)
    {
        m_spectating = -1;
        m_spectateEyeHeight = 0.0f;
        return;
    }
    // Driving a drone, there is its camera instead.
    if (m_supportDrone.Active())
    {
        m_spectating = -1;
        return;
    }

    const std::vector<RemotePlayerView>& remotes = RemotePlayers();
    const auto living = [&](int id)
    {
        for (const RemotePlayerView& remote : remotes)
        {
            if (remote.id == id && remote.alive)
            {
                return true;
            }
        }
        return false;
    };

    // Asked to watch somebody else, which is the only control a dead player has.
    if (m_spectateNext && m_spectating >= 0)
    {
        // The next living player after the one being watched, wrapping. Everybody is on the same
        // roster in the same order, so "next" means the same thing on every machine.
        for (size_t step = 1; step <= remotes.size(); ++step)
        {
            size_t at = 0;
            for (size_t i = 0; i < remotes.size(); ++i)
            {
                if (static_cast<int>(remotes[i].id) == m_spectating)
                {
                    at = i;
                    break;
                }
            }
            const RemotePlayerView& candidate = remotes[(at + step) % remotes.size()];
            if (candidate.alive)
            {
                m_spectating = candidate.id;
                break;
            }
        }
    }
    m_spectateNext = false;

    if (m_spectating >= 0 && living(m_spectating))
    {
        return;
    }
    m_spectating = -1;
    for (const RemotePlayerView& remote : remotes)
    {
        if (remote.alive)
        {
            m_spectating = remote.id;
            break;
        }
    }
}

float PredationGame::TorchIntensity() const
{
    return cv_torchIntensity.Get();
}

float PredationGame::TorchRange() const
{
    return cv_torchRange.Get();
}

float PredationGame::TorchInnerAngle() const
{
    return cv_torchInner.Get();
}

float PredationGame::TorchOuterAngle() const
{
    return cv_torchOuter.Get();
}

// --- The front end ---------------------------------------------------------------------------
//
// The world is built and simulating behind the menu rather than being loaded when you press a
// button, so the menu has a moving backdrop and starting a game is instant. That also means there
// is only ever one world, which is what keeps hosting, joining and leaving from needing their own
// loading paths.

void PredationGame::ResetWorld()
{
    // Everything that can be used up is put back. Starting a game has to start a game: doors shut,
    // lockers empty, crates full, and every item back on the bench, including the ones somebody
    // walked off with last time.
    RemoveAllDrones();
    m_deadForGood = false;
    m_wipeTimer = 0.0f;
    m_world.Clear(m_scene, m_app->GetPhysics(), m_interactions);
    m_world.Build(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                  &m_weaponData);
    m_world.AddFacility(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, m_ship.Placements());
    m_world.AddFacility(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                        m_facility.Placements());
    ResetMission();
    ClearFlares();
    m_torchCharge = 1.0f;

    m_inventory.Clear();
    m_weapon = WeaponState{};
    m_heldItem = kInvalidItem;
    m_body.ClearHeldItem(m_scene);
    m_hidingSpot = -1;
    m_tracers.clear();
    ClearBulletHoles();
    m_remoteRespawnTimers.clear();
    m_respawnTimer = 0.0f;
    ClearCorpses();
    m_spectating = -1;
    m_deathImpulse = glm::vec3(0.0f);
    m_localCollapsed = false;
    m_body.Revive();
    RespawnLocalPlayer(m_spawnPoint);
    PRED_LOG_INFO(Gameplay, "World reset");
}

void PredationGame::GoToMap(MapChoice map)
{
    float yaw = 0.0f;
    std::string arrived;
    switch (map)
    {
    case MapChoice::TestMap:
        m_spawnPoint = glm::vec3(0.0f, 0.5f, TestMapSpec::kSpawnZ);
        yaw = glm::pi<float>(); // south, across it
        arrived = "In the testing area. The creature lab is through the gate to the north.";
        break;
    case MapChoice::Lab:
        m_spawnPoint = LabSpec::kSpawn;
        yaw = 0.0f; // north, into it
        arrived = "In the creature lab, at the north end of the testing area. The nest is to the north.";
        break;
    case MapChoice::Ship:
        m_shipReady = false;
        m_spawnPoint = ShipArrival(LocalPlayerId(), yaw);
        arrived = "Aboard the ship.";
        break;
    case MapChoice::Facility:
    {
        // The ship is over its planet now, and its shuttle has gone down.
        m_shipOrbiting = m_facility.Seed();
        m_shipReady = false;
        if (m_campaignOpen && IsAuthority() && m_campaign.body >= 0 && !m_campaign.travel.underway)
        {
            m_campaign.region = m_campaign.travel.region;
            m_campaign.Learn(m_campaign.system, m_campaign.body, CampaignState::kKnownVisited);
            CampaignChanged();
        }
        // A deployment that is over is not gone back to: the site is put back as it was, its data on its terminal.
        if (m_mission.stage == MissionState::Stage::Over && IsAuthority() && m_screen == Screen::Playing)
        {
            ChangeFacility(m_facility.Seed());
        }
        // In the shuttle on the pad, facing its ramp: where the insertion hands everybody control.
        m_spawnPoint = MissionArrival(LocalPlayerId());
        CinePose arrival;
        yaw = m_facility.Shuttle().Socket("arrival", arrival)
                  ? std::atan2((arrival.rotation * glm::vec3(0.0f, 0.0f, -1.0f)).x, -(arrival.rotation * glm::vec3(0.0f, 0.0f, -1.0f)).z)
                  : m_facility.SpawnYaw();
        arrived = "At the site planned from seed " + std::to_string(m_facility.Seed()) + ": " +
                  std::to_string(m_facility.Plan().buildings.size()) + " buildings.";
        break;
    }
    }
    m_map = map;
    m_spawnYaw = yaw;
    if (m_screen != Screen::Playing)
    {
        // From the menu: a game of your own, there.
        StopSession();
        m_sessionMode = SessionMode::Offline;
        m_enteringMap = true;
        EnterWorld();
        m_enteringMap = false;
        if (map == MapChoice::Facility)
        {
            m_spawnPoint = MissionArrival(LocalPlayerId());
            RespawnLocalPlayer(m_spawnPoint);
            CinePose arrival;
            if (m_facility.Shuttle().Socket("arrival", arrival))
            {
                const glm::vec3 faces = arrival.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
                yaw = std::atan2(faces.x, -faces.z);
            }
        }
        m_lookYaw = yaw;
        m_lookPitch = 0.0f;
        m_player.State().yaw = m_lookYaw;
        RequestNavRebuild();
        if (map == MapChoice::Facility && HasCinematic("surface_insertion"))
        {
            PlayCinematic("surface_insertion");
        }
        return;
    }
    if (!IsAuthority())
    {
        m_app->GetConsole().PrintError("Only the host can take everybody somewhere else.");
        return;
    }
    // Everybody goes, and the creatures start again from wherever creatures come from there -- once there
    // is a way about the place they have gone to: the navigation is only ever of where the players are.
    if (map == MapChoice::Ship)
    {
        RemoveAllDrones();
        ClearOrders();
        if (m_campaignOpen)
        {
            // Back aboard from an expedition: over the same place, the shuttle ready to go down again; and a moment worth
            // saving at.
            m_campaign.region = -1;
            ChooseLandingRegion(m_campaign.travel.region);
            SaveCampaign(true, "back aboard");
        }
    }
    SpawnCreatures();
    RespawnLocalPlayer(m_spawnPoint);
    RequestNavRebuild();
    m_lookYaw = yaw;
    m_player.State().yaw = m_lookYaw;
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : m_host.Remotes())
        {
            // At the site, each in their own place in the shuttle; aboard, each in their own place in the briefing room.
            const glm::vec3 place = ArrivalFor(remote.id);
            m_host.RespawnPlayer(remote.id, place);
            WorldEventMessage event;
            event.kind = WorldEventKind::PlayerRespawned;
            event.player = remote.id;
            event.position = place;
            m_host.Broadcast(event);
        }
    }
    m_app->GetConsole().Print(arrived);
    // Going in is shown: the shuttle coming down onto the pad, its ramp down.
    if (map == MapChoice::Facility && HasCinematic("surface_insertion"))
    {
        PlayCinematic("surface_insertion");
    }
}

void PredationGame::ChangeFacility(uint16_t seed)
{
    if (seed == 0)
    {
        seed = 1;
    }
    m_facility.SetLook(SiteLookHere());
    m_climateAmount = m_facility.GetLook().weatherAmount;
    m_lastWadable = glm::vec3(0.0f);
    m_weatherHeld = false;
    const auto started = std::chrono::steady_clock::now();
    m_facility.Build(seed, m_scene, m_app->GetMeshes(), m_app->GetPhysics(), &m_levelLights);
    const auto built = std::chrono::steady_clock::now();
    // Its doors, lockers, crates and items went with it; everything that can be used up comes back with
    // the new one, in the same order on every machine. What people are carrying they keep.
    m_hidingSpot = -1;
    m_world.Clear(m_scene, m_app->GetPhysics(), m_interactions);
    m_world.Build(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, &m_weaponData);
    m_world.AddFacility(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, m_ship.Placements());
    m_world.AddFacility(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                        m_facility.Placements());
    ResetMission();
    ClearBulletHoles();
    ClearFlares();
    ClearCorpses();
    RequestNavRebuild();
    PRED_LOG_INFO(Gameplay, "Site {} made in {:.0f} ms ({:.0f} of it the site itself)", seed,
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
                  std::chrono::duration<double, std::milli>(built - started).count());
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::FacilityChanged;
        event.item = seed;
        m_host.Broadcast(event);
    }
}

void PredationGame::EnterWorld()
{
    PRED_LOG_INFO(Gameplay, "Entering the world");
    // A new game starts aboard, whatever the last one was doing -- unless it is going somewhere in particular.
    if (!m_enteringMap)
    {
        m_map = MapChoice::Ship;
        // A campaign with the ship standing at an outpost starts outside it, on the bay's floor by its stair -- the station
        // there to stand on first (ShipArrival).
        if (ShipLanded())
        {
            UpdateShipGround();
        }
        m_spawnPoint = ShipArrival(LocalPlayerId(), m_spawnYaw);
    }
    // A new game starts from the beginning. The world has been simulating behind the menu, and
    // whatever was done to it last time is still done.
    ResetWorld();
    m_lookYaw = m_spawnYaw;
    m_lookPitch = 0.0f;
    m_player.State().yaw = m_lookYaw;
    // Out in space -- unless this game begins by going down to a site, when the ship is over its planet.
    m_shipOrbiting = m_map == MapChoice::Facility ? m_facility.Seed() : 0;
    m_shipReady = false;
    m_cineGoTo.reset();
    ClearOrders();
    m_screen = Screen::Playing;
    // A campaign carries on from where the ship is: over a body, looked at and somewhere chosen to go down.
    if (m_campaignOpen && IsAuthority() && !m_campaign.travel.underway && m_campaign.body >= 0)
    {
        ArriveAtBody();
    }
    m_paused = false;
    m_titleStatus.clear();
    SetCameraMode(CameraMode::FirstPerson);
    m_wantMouseCaptured = true;
    // Made after the players are placed, so it can be put somewhere far from them. Only by
    // whoever runs the game; a client is shown the host's.
    SpawnCreatures();
}



// The key an action is on, as the player has it bound, for putting in a sentence.
//
// Text that names a key has to name the one the player actually pressed. "[F] Open" after
// somebody has moved Interact to E is a prompt that is wrong about the one thing it is for, and
// every one of these was written with the shipped binding typed into it.
std::string KeyFor(const Input& input, const char* action)
{
    const auto found = input.Bindings().find(action);
    if (found == input.Bindings().end() || found->second.empty())
    {
        return "unbound";
    }
    return Input::BindingName(found->second.front());
}

// --- Finding games ------------------------------------------------------------------------------
//
// Two ways a game can be reachable, and the player is asked about neither of them.
//
// What used to be here was four pages of questions: over the internet or by address, and if by
// address then which address, and what port, and had the router agreed to forward it. All of that
// is true and none of it is the player's problem. A game over the internet is joined by the code its
// host gives out; a game on your own network announces itself and is simply in the list, joined by
// clicking the row. There is no public list: a campaign is played among friends.




// --- Multiplayer -----------------------------------------------------------------------------
//
// The host runs the real simulation for everyone. A client predicts its own movement from local
// input and interpolates everybody else. Nothing a client sends is written into the world: the host
// runs the same movement code against its own physics and what comes out is what happened.

// How loud a step is for the stance it is taken in.
//
// Crouching is quieter and crawling quieter still, which is what anybody expects and what makes
// moving carefully worth doing. It matters more than it sounds: this is the first piece of the
// thing that will eventually decide whether the creature heard you, and putting the numbers in one
// place now means the hunt reads the same value the player hears.

void PredationGame::UpdateMantleStow()
{
    // Climbing takes both hands, so whatever was in them is put away for the duration and taken
    // back out at the top.
    //
    // Done through the selected slot rather than by hiding the model, so it is the same put-away
    // and the same bring-up as any other swap: the weapon goes down, the arms are free for the
    // climb, and it comes back up afterwards. Hiding it instead would have left the simulation
    // thinking a rifle was in hands that were holding a ledge.
    // The hands stay empty until they are off the ledge, not until the climb ends.
    //
    // The arms go on fading out of the climb for a moment after the body arrives at the top, which
    // is them letting go. Putting a weapon back in them during that fade means the hand is being
    // asked to be on a ledge and on a handguard at once: what came out was the weapon appearing in
    // mid air and the hands snapping across to it. It is a fifth of a second of waiting.
    // (And swimming takes them too: whatever was in them is put away until out of the water.)
    const bool climbing = m_player.State().mantling || m_body.ArmsAreClimbing() || m_player.State().swimming;
    if (climbing == m_mantleStowing)
    {
        return;
    }
    m_mantleStowing = climbing;
    if (climbing)
    {
        m_mantleStowedSlot = m_inventory.SelectedSlot();
        if (m_mantleStowedSlot != Inventory::kNoSlot)
        {
            m_inventory.SelectSlot(Inventory::kNoSlot);
        }
    }
    else if (m_mantleStowedSlot != Inventory::kNoSlot)
    {
        m_inventory.SelectSlot(m_mantleStowedSlot);
        m_mantleStowedSlot = Inventory::kNoSlot;
    }
}

void PredationGame::RespawnLocalPlayer(const glm::vec3& position)
{
    m_player.Respawn(position);
    // Standing, whatever you were doing when you died.
    //
    // The controller already comes back standing, and it was put straight back down again: crouch
    // and prone are toggles, the toggle still held whatever it held when the player was killed, and
    // the very next tick of input asked for it. So dying prone meant coming back to life lying on
    // the floor of the spawn room, which is not a thing anybody chose.
    m_crouchToggleState = false;
    m_proneToggleState = false;
    m_forceCrouch = false;
    m_forceProne = false;
    m_sprintToggleState = false;
    // And upright rather than in a heap. The body is a ragdoll from the moment it is killed, and
    // its bones stay wherever they fell until something puts them back: coming back to life without
    // clearing it gave the new body its corpse's arms and legs for the first few frames, which is
    // where the legs over the head came from.
    if (m_localCollapsed)
    {
        LeaveCorpse(m_body, LocalPlayerId());
    }
    m_localCollapsed = false;
    m_body.Revive();
    m_deathImpulse = glm::vec3(0.0f);
    m_spectating = -1;
    m_deadForGood = false;
    m_everybodyDownFor = 0.0f;
    m_playingDrone = false;
}

void PredationGame::LeaveCorpse(const PlayerBody& body, uint8_t player)
{
    constexpr size_t kMostCorpses = 6;
    if (m_corpses.size() >= kMostCorpses)
    {
        for (const Entity entity : m_corpses.front().parts)
        {
            m_scene.Destroy(entity);
        }
        for (const Entity mark : m_corpses.front().marks)
        {
            m_scene.Destroy(mark);
        }
        m_corpses.erase(m_corpses.begin());
    }
    Corpse corpse;
    corpse.player = player;
    corpse.parts = body.LeaveCorpse(m_scene, corpse.at);
    corpse.id = m_nextCorpseId++;
    for (const Entity part : corpse.parts)
    {
        const Transform* where = m_scene.GetTransform(part);
        const MeshRenderer* renderer = m_scene.GetMeshRenderer(part);
        corpse.offsets.push_back(where != nullptr ? where->position - corpse.at : glm::vec3(0.0f));
        corpse.turns.push_back(where != nullptr ? where->rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        corpse.sizes.push_back(where != nullptr ? where->scale : glm::vec3(1.0f));
        corpse.looks.push_back(renderer != nullptr ? renderer->material : Material{});
        corpse.flesh.push_back(1.0f);
        corpse.gone.push_back(false);
    }
    if (!corpse.parts.empty())
    {
        PRED_LOG_INFO(Gameplay, "Player {} left a body at {:.1f} {:.1f} {:.1f}", player, corpse.at.x, corpse.at.y, corpse.at.z);
        m_corpses.push_back(std::move(corpse));
    }
}

void PredationGame::ClearCorpses()
{
    for (const Corpse& corpse : m_corpses)
    {
        for (const Entity entity : corpse.parts)
        {
            m_scene.Destroy(entity);
        }
        for (const Entity mark : corpse.marks)
        {
            m_scene.Destroy(mark);
        }
    }
    m_corpses.clear();
}

void PredationGame::OnShutdown()
{
    // A navigation rebuild still going on its worker is building into a mesh this game owns, and logs when it is done:
    // let it finish before any of that goes. Quitting within a second or two of arriving somewhere -- while the site's
    // navigation was still being built -- crashed on the way out.
    if (m_navRebuild.valid())
    {
        m_navRebuild.wait();
    }
    CloseCampaign();
    StopSession();
    DestroyEditorFirstPerson();
    DestroySystemMapTarget();
    m_planets.Shutdown();
    m_sprites.Shutdown();
    m_water.Shutdown();
    m_editor.Shutdown(m_editorScene);
    if (m_editorBodyBuilt)
    {
        m_editorBody.Destroy(m_editorScene);
        m_editorBodyBuilt = false;
    }
    m_itemIcons.Shutdown();
    ClearProps();
    // Here, while the scene still exists. Left to the destructor, they went after it: members are
    // destroyed in the reverse of the order they are declared, m_creatures is declared long before
    // m_scene, and each creature then took its pieces out of a scene that had already been freed.
    // Every run with a creature in it crashed on the way out.
    ClearCreatures();
    m_body.Destroy(m_scene);
    m_player.Shutdown();
    m_scene.Clear();
    PRED_LOG_INFO(Gameplay, "Game shutdown");
}

void PredationGame::OnEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // Clicking back into the world re-captures. Clicking an ImGui window must not, or dragging
        // a slider would yank the pointer away mid-drag.
        //
        // Nor while paused. The pause menu is a small box in the middle of the screen, so most of
        // the window is not over it, and a click out there was reading as "back to the game": the
        // pointer vanished, the view started turning, and the menu was still up in front of it.
        // Leaving the pause menu is a thing you do on purpose, with Escape or with Resume.
        if (!m_wantMouseCaptured && !m_paused && !m_app->IsConsoleOpen() &&
            !m_app->IsUiCapturingMouse() && !m_cineEditor.IsOpen())
        {
            m_wantMouseCaptured = true;
        }
        break;
    case SDL_EVENT_DROP_FILE:
        // Adding an asset, in one gesture.
        //
        // It used to mean finding the file in Explorer, copying its path, switching to the game,
        // opening the editor, pasting the path into a box and pressing a button. The window can
        // simply be handed the file. Dropped with the editor closed it opens the editor on it,
        // because somebody dragging a model at the title screen has said what they want clearly
        // enough.
        if (event.drop.data != nullptr)
        {
            OnFileDropped(event.drop.data);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        m_windowFocused = false;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        m_windowFocused = true;
        break;
    default:
        break;
    }
}

void PredationGame::UpdateMouseCapture()
{
    // Relative mode hides the cursor and feeds deltas, which is right for looking around and wrong
    // for everything else. The console suspends capture without discarding the player's intent, so
    // closing it hands the mouse straight back to the game.
    //
    // ImGui's hover state deliberately does not appear here. Under relative mode the OS cursor
    // stops moving, so ImGui's idea of the pointer freezes wherever it last was; if that happened
    // to be over a panel, testing it here would release capture, let the cursor reappear over the
    // same panel, and oscillate. Whether the UI is hovered only matters when deciding to re-capture
    // on a click, which is handled in OnEvent.
    // The menu is pointed at, so the pointer is never taken while it is up. Said here as well as
    // where the click is handled, because this is the one place capture is actually decided and a
    // rule that matters this much should not depend on every caller having remembered it.
    const bool shouldCapture = m_wantMouseCaptured && m_windowFocused && !m_paused &&
                               !m_app->IsConsoleOpen() && m_screen == Screen::Playing;

    if (shouldCapture != m_mouseCaptured)
    {
        m_mouseCaptured = shouldCapture;
        m_app->GetWindow().SetRelativeMouse(shouldCapture);
        if (shouldCapture)
        {
            m_discardNextMouseDelta = true;
        }
    }
}

Creature* PredationGame::CreatureHoldingMe()
{
    if (IsAuthority())
    {
        const auto grip = m_grips.find(LocalPlayerId());
        return grip != m_grips.end() ? CreatureById(grip->second.creature) : nullptr;
    }
    if (m_sessionMode == SessionMode::Client && m_client.HeldByHost() && !m_client.CocoonedByHost())
    {
        return CreatureById(m_client.HeldByCreature());
    }
    return nullptr;
}

void PredationGame::UpdateGrabbedView(float dt)
{
    // Held: your head is turned to the thing that has you -- you can still look away a little, and it
    // pulls you back -- and every struggle jerks the view.
    Creature* holder = m_player.State().alive ? CreatureHoldingMe() : nullptr;
    const bool held = holder != nullptr;
    if (held && !m_wasHeld)
    {
        m_grabFlash = 1.0f;
        m_heldFor = 0.0f;
    }
    m_wasHeld = held;
    m_grabFlash = std::max(m_grabFlash - dt * 1.6f, 0.0f);
    m_struggleKick = std::max(m_struggleKick - dt * 5.0f, 0.0f);
    const bool jump = m_app->GetInput().IsActionDown("jump") && !m_paused && !m_app->IsConsoleOpen();
    if (held && jump && !m_struggleJumpWasDown)
    {
        m_struggleKick = 1.0f;
        m_struggleSide = -m_struggleSide;
    }
    m_struggleJumpWasDown = jump;
    if (!held)
    {
        return;
    }
    m_heldFor += dt;
    const glm::vec3 toFace = holder->Eye() - m_player.View().eyePosition;
    if (glm::length(toFace) < 0.05f)
    {
        return;
    }
    const glm::vec3 direction = glm::normalize(toFace);
    const float yaw = std::atan2(direction.x, -direction.z);
    const float pitch = std::asin(std::clamp(direction.y, -0.99f, 0.99f));
    const float pull = 1.0f - std::exp(-4.0f * dt);
    m_lookYaw += std::remainder(yaw - m_lookYaw, glm::two_pi<float>()) * pull;
    m_lookPitch += (pitch - m_lookPitch) * pull;
}

void PredationGame::SampleLook(float /*dt*/)
{
    Input& input = m_app->GetInput();
    if (!m_mouseCaptured || m_app->IsConsoleOpen() || CinematicHoldsPlayers())
    {
        return;
    }

    const glm::vec2 delta = input.MouseDelta();
    if (m_discardNextMouseDelta)
    {
        // The first report after relative mode is enabled can carry the distance from wherever the
        // cursor happened to be, which snaps the view somewhere arbitrary.
        m_discardNextMouseDelta = false;
        return;
    }
    // Slower with the sights up, by as much as the player asked: a small correction at a distance is
    // a small movement of the hand, and at full sensitivity it overshoots.
    const float sighted = glm::mix(1.0f, std::clamp(cv_aimSensitivity.Get(), 0.1f, 2.0f), m_weapon.aim);
    const float sensitivity = glm::radians(cv_mouseSensitivity.Get()) * sighted;
    const float vertical = (cv_invertY.Get() ? delta.y : -delta.y) * sensitivity;
    const float limit = glm::radians(m_player.Config().maxPitchDegrees);
    // Lying down, the eye is a third of a metre off the floor and there is a body in the way.
    // Looking straight down from there puts the camera through the ground and shows the underside
    // of the level, so the downward half of the range is cut to what a neck could manage anyway.
    const float downLimit = m_player.State().stance == PlayerStance::Prone
                                ? glm::radians(m_player.Config().pronePitchDownDegrees)
                                : limit;

    // Holding the look button in third person orbits the camera instead of turning the character,
    // which is the only way to see the animation from the front.
    if (m_cameraMode == CameraMode::ThirdPerson && input.IsActionDown("orbit"))
    {
        m_orbitYaw += delta.x * sensitivity;
        m_orbitPitch = std::clamp(m_orbitPitch + vertical, -limit, limit);
        return;
    }

    m_lookYaw += delta.x * sensitivity;
    m_lookPitch = std::clamp(m_lookPitch + vertical, -downLimit, limit);
    // Shut in a locker there is only the door in front of you and the slits in it: the view goes a little
    // way either side and up and down, to look out along them, and no further. Turning round in there
    // to stare at the back of it was never something a body in a locker could do.
    // Not the free camera, which is not a body in a locker but somebody looking round the level.
    if (const WorldObjects::HidingSpot* spot = m_hidingSpot >= 0 && m_cameraMode != CameraMode::Fly ? m_world.GetHidingSpot(m_hidingSpot) : nullptr)
    {
        const float across = std::remainder(m_lookYaw - spot->insideYaw, glm::two_pi<float>());
        m_lookYaw = spot->insideYaw + std::clamp(across, -glm::radians(kLockerLookYawDegrees), glm::radians(kLockerLookYawDegrees));
        m_lookPitch = std::clamp(m_lookPitch, -glm::radians(kLockerLookDownDegrees), glm::radians(kLockerLookUpDegrees));
    }
    if (m_lookYaw > glm::pi<float>())
    {
        m_lookYaw -= glm::two_pi<float>();
    }
    else if (m_lookYaw < -glm::pi<float>())
    {
        m_lookYaw += glm::two_pi<float>();
    }
}

PlayerInput PredationGame::BuildPlayerInput()
{
    Input& input = m_app->GetInput();
    PlayerInput result;
    // Where the player is pointing, plus whatever the weapon has kicked it by.
    //
    // The recoil was already here and it only moved the rounds: the barrel climbed and the camera
    // did not, so a burst walked off the target while the crosshair sat still. That is the wrong way
    // round -- what a player feels as recoil is the view moving, and the rounds following it is what
    // makes the crosshair keep telling the truth while it does.
    //
    // Added to the input rather than to the camera alone, so it reaches the three places that have
    // to agree: the view, the body everybody else sees, and the shot. A head that does not kick when
    // the weapon does looks like somebody else is firing.
    //
    // It is an offset that decays to nothing, so it never moves where the player is actually aiming.
    // Pulling down against it works the way it should: the mouse moves m_lookPitch, the recoil
    // recovers to zero on top of it, and the two do not fight.
    const glm::vec2 aimed = AimAngles();
    result.yaw = aimed.x;
    result.pitch = aimed.y;

    if (m_app->IsConsoleOpen() || m_screen != Screen::Playing || m_paused || CinematicHoldsPlayers() || m_mapOpen)
    {
        // Console open, paused, at the title, or held by a cinematic: keep looking where we are and stop everything else.
        //
        // The console case was here already. The paused one was not, and the pause screen is not a
        // screenshot of the game -- it is the game with a window over it, still running, still
        // reading the keyboard. So WASD walked the character around behind the menu while the player
        // was reading it, and anything else bound to a key did whatever it does. A pause that the
        // character walks through is not a pause.
        m_jumpLatch = false;
        m_crouchPressLatch = false;
        m_pronePressLatch = false;
        return result;
    }

    result.move.y = (input.IsActionDown("move_forward") ? 1.0f : 0.0f) -
                    (input.IsActionDown("move_back") ? 1.0f : 0.0f);
    result.move.x = (input.IsActionDown("move_right") ? 1.0f : 0.0f) -
                    (input.IsActionDown("move_left") ? 1.0f : 0.0f);
    // Console-driven movement, so animation can be inspected in a headless capture.
    result.move += m_debugMove;

    // The latch carries a press that landed between two fixed ticks.
    result.jump = m_jumpLatch;
    m_jumpLatch = false;

    result.sprint = cv_sprintToggle.Get() ? m_sprintToggleState : input.IsActionDown("sprint");
    result.jumpHeld = input.IsActionDown("jump");
    result.crouchDown = input.IsActionDown("crouch");

    // Toggles are applied here, in the tick, so a press can never fall between two of them.
    if (cv_crouchToggle.Get())
    {
        if (m_crouchPressLatch)
        {
            m_crouchToggleState = !m_crouchToggleState;
            m_proneToggleState = false;
        }
        if (m_pronePressLatch)
        {
            m_proneToggleState = !m_proneToggleState;
            m_crouchToggleState = false;
        }
    }
    m_crouchPressLatch = false;
    m_pronePressLatch = false;

    result.crouchHeld = cv_crouchToggle.Get() ? m_crouchToggleState : input.IsActionDown("crouch");
    result.proneHeld = cv_crouchToggle.Get() ? m_proneToggleState : input.IsActionDown("prone");
    result.walk = input.IsActionDown("walk");

    result.lean = (input.IsActionDown("lean_right") ? 1.0f : 0.0f) -
                  (input.IsActionDown("lean_left") ? 1.0f : 0.0f);

    if (m_forceLeanOn)
    {
        result.lean = m_forceLean;
    }

    // Debug override from the `stance` console command.
    result.crouchHeld = result.crouchHeld || m_forceCrouch;
    result.proneHeld = result.proneHeld || m_forceProne;
    return result;
}

const char* PredationGame::CameraModeName() const
{
    switch (m_cameraMode)
    {
    case CameraMode::ThirdPerson:
        return "third person";
    case CameraMode::Fly:
        return "free camera";
    case CameraMode::FirstPerson:
    default:
        return "first person";
    }
}

void PredationGame::SetCameraMode(CameraMode mode)
{
    // The free camera starts where you are, looking where you look, not wherever it was last left.
    if (mode == CameraMode::Fly && m_cameraMode != CameraMode::Fly && m_screen != Screen::Title)
    {
        m_camera.position = m_renderEye;
        m_camera.yaw = m_lookYaw;
        m_camera.pitch = m_lookPitch;
    }
    m_cameraMode = mode;
    // Orbit is an inspection tool, not a persistent state; entering third person starts behind the
    // character rather than wherever it was last left.
    m_orbitYaw = 0.0f;
    m_orbitPitch = 0.0f;
    if (m_app != nullptr)
    {
        m_app->GetConsole().Print(std::string("Camera: ") + CameraModeName());
    }
}

const SoundVariants& PredationGame::Sounds(const std::string& name) const
{
    static const SoundVariants kSilence;
    const auto found = m_soundBank.find(name);
    return found != m_soundBank.end() ? found->second : kSilence;
}





void PredationGame::TryInteract()
{
    const InteractionSystem::Focus& focus = m_interactions.CurrentFocus();
    if (m_hidingSpot >= 0)
    {
        // Leaving is the same interaction as entering, so it takes the same road: a client asks,
        // the host decides and tells everybody. Done as a local call it told nobody, and every
        // other machine kept the locker shut and occupied for the rest of the match.
        const int spot = m_hidingSpot;
        if (m_sessionMode == SessionMode::Client)
        {
            m_client.SendInteract(static_cast<uint8_t>(InteractionKind::HidingSpot),
                                  static_cast<uint8_t>(spot));
        }
        else
        {
            PerformInteraction(InteractionKind::HidingSpot, spot, LocalPlayerId());
        }
        return;
    }
    if (!focus.valid)
    {
        return;
    }
    // The loadout locker is everybody's own: its screen opens here, and only what is drawn from it is told to the host.
    if (focus.kind == InteractionKind::Loadout)
    {
        m_loadoutOpen ? CloseLoadout() : OpenLoadout();
        return;
    }
    // The boarding door: opened (only on the ground) or shut, asked of the host. Shut with somebody in it, it waits until
    // they are clear (ShipMap::UpdateAirlock).
    if (focus.kind == InteractionKind::Airlock)
    {
        if (m_campaignOpen)
        {
            AskCampaign(CampaignAction::Door, m_campaign.doorOpen ? 0 : 1);
            PlayNamed("UI/confirm", m_renderEye, 0.5f, 0.8f, false);
        }
        return;
    }
    // The helm: setting out on the course plotted, asked of the host.
    if (focus.kind == InteractionKind::Helm)
    {
        if (m_campaignOpen)
        {
            SetOut();
        }
        return;
    }
    // So is the navigation console, in a campaign: the map opens here, and what is chosen on it is asked of the host.
    if (focus.kind == InteractionKind::Deploy && m_campaignOpen)
    {
        // One at a time: somebody else at it already, and it stays theirs.
        if (!m_mapOpen && m_mapHolder != kNobody && m_mapHolder != LocalPlayerId())
        {
            PlayNamed("UI/back", m_renderEye, 0.5f, 1.0f, false);
            return;
        }
        m_mapOpen ? CloseSystemMap() : OpenSystemMap();
        return;
    }
    // A terminal with no power does nothing, and there is nothing to ask the host: it clicks, and the objective says
    // what to do about it.
    if (focus.kind == InteractionKind::Terminal && !m_mission.powered)
    {
        m_missionFoundNoPower = true;
        PlayNamed("World/terminal_dead", m_missionPlan.terminal, 0.7f);
        return;
    }

    // A client asks; it does not act. Everything in the world belongs to the host, so what happens
    // next arrives as an event and is applied the same way another player's interaction would be.
    if (m_sessionMode == SessionMode::Client)
    {
        // The one thing worth checking before asking: whether there is room. The host cannot know
        // what is in this player's bag, so if it hands over something that will not fit the item is
        // gone. Checking here costs nothing and is the same answer the offline path gives.
        if (focus.kind == InteractionKind::Pickup)
        {
            const WorldObjects::Pickup* pickup = m_world.GetPickup(focus.payload);
            if (pickup != nullptr && !m_inventory.CanAdd(m_items, pickup->item, 1))
            {
                m_app->GetConsole().Print("Inventory full");
                return;
            }
        }
        // And the same for a crate: asking when the weapon is already full spends the crate for nothing.
        if (focus.kind == InteractionKind::AmmoCrate && !CanTakeAmmunition())
        {
            m_app->GetConsole().Print(EquippedWeapon() == nullptr ? "Nothing to load"
                                                                  : "Already carrying full spare magazines");
            return;
        }
        m_client.SendInteract(static_cast<uint8_t>(focus.kind), static_cast<uint8_t>(focus.payload));
        return;
    }

    PerformInteraction(focus.kind, focus.payload, LocalPlayerId());
}

void PredationGame::DropSelected()
{
    if (m_hidingSpot >= 0)
    {
        return;
    }
    const Inventory::Slot slot = m_inventory.Selected();
    if (slot.IsEmpty())
    {
        return;
    }

    // What this one is carrying goes with it. For the weapon in hand that is the live state rather
    // than the slot's copy, which is only written back when something else comes out.
    const bool inHand = m_inventory.SelectedSlot() == m_ammoSlot && m_weapon.HasWeapon();
    const int rounds = inHand ? m_weapon.rounds : slot.rounds;
    const int reserve = inHand ? m_weapon.reserve : slot.reserve;

    const int removed = m_inventory.RemoveFromSlot(m_inventory.SelectedSlot(), 1);
    if (removed <= 0)
    {
        return;
    }
    if (inHand)
    {
        // The weapon has left the hand, so the state that was riding on it belongs to nothing now.
        m_weapon = WeaponState{};
        m_ammoSlot = Inventory::kNoSlot;
    }

    const PlayerView& view = m_player.View();
    const glm::vec3 origin = ClearDropPoint(view.eyePosition, view.eyePosition + view.Forward() * 0.6f);
    const glm::vec3 throwVelocity = view.Forward() * 2.5f;

    DropIntoWorld(slot.item, removed, rounds, reserve, origin, throwVelocity);
}

glm::vec3 PredationGame::ClearDropPoint(const glm::vec3& eye, const glm::vec3& wanted) const
{
    // Short of anything solid between the eye and where it was going to be: dropped facing a wall it was made on the
    // wall's far side, and anything could be put through one.
    const glm::vec3 along = wanted - eye;
    const float length = glm::length(along);
    if (length < 1e-3f)
    {
        return wanted;
    }
    const glm::vec3 direction = along / length;
    constexpr float kRoom = 0.35f; // about half the largest thing carried
    const RayHit hit = m_app->GetPhysics().RayCastStatic(eye, direction, length + kRoom);
    if (!hit)
    {
        return wanted;
    }
    return eye + direction * std::max(hit.distance - kRoom, 0.05f);
}

// Puts one thing on the floor, wherever it came from.
//
// A client asks to put it down and waits to be told. Dropping locally made an item nobody else had:
// it could not be picked up, the indices the two machines used stopped agreeing, and dropping the
// same thing twice made two of it.
void PredationGame::DropIntoWorld(ItemId item, int count, int rounds, int reserve,
                                  const glm::vec3& origin, const glm::vec3& velocity)
{
    if (m_sessionMode == SessionMode::Client)
    {
        DropMessage message;
        message.item = static_cast<uint16_t>(item);
        message.count = static_cast<uint8_t>(count);
        message.rounds = LoadToWire(rounds);
        message.reserve = LoadToWire(reserve);
        message.position = origin;
        message.velocity = velocity;
        m_client.SendDrop(message);
        return;
    }

    const int index =
        m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                            item, count, origin, velocity, rounds, reserve);
    MakeNoise(NoiseKind::Item, origin, NoiseReach::kItem, -1);

    // Everyone else has to see it land, and it has to be there to pick up. The throw is sent too,
    // so it arcs on their screen rather than appearing on the floor.
    if (m_sessionMode == SessionMode::Host && index >= 0)
    {
        m_host.Broadcast(PickupSpawnedEvent(static_cast<uint8_t>(index), static_cast<uint16_t>(item),
                                            static_cast<uint8_t>(count), LoadToWire(rounds),
                                            LoadToWire(reserve), origin, velocity));
    }
}

// Everything you were carrying goes on the floor where you fell.
//
// Death is meant to matter, and the way it matters most in an extraction game is that what you were
// carrying stops being yours. Scattered rather than stacked, so a kill leaves a spread of things to
// pick through rather than one pile occupying a single point.
void PredationGame::DropEverything()
{
    const glm::vec3 at = m_player.State().position + glm::vec3(0.0f, 0.35f, 0.0f);
    for (int i = 0; i < m_inventory.SlotCount(); ++i)
    {
        const Inventory::Slot slot = m_inventory.At(i);
        if (slot.IsEmpty())
        {
            continue;
        }
        // Whatever was in the hand carries its live magazine; the rest carry the slot's copy.
        const bool inHand = i == m_ammoSlot && m_weapon.HasWeapon();
        const int rounds = inHand ? m_weapon.rounds : slot.rounds;
        const int reserve = inHand ? m_weapon.reserve : slot.reserve;

        const float angle = glm::two_pi<float>() * static_cast<float>(i) /
                            static_cast<float>(std::max(m_inventory.SlotCount(), 1));
        const glm::vec3 scatter{std::cos(angle) * 1.2f, 1.4f, std::sin(angle) * 1.2f};
        DropIntoWorld(slot.item, slot.count, rounds, reserve, at, scatter);
    }

    m_inventory.Clear();
    m_weapon = WeaponState{};
    m_ammoSlot = Inventory::kNoSlot;
}

void PredationGame::EnterHidingSpot(int index)
{
    WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(index);
    // Taken by somebody else, no. Taken by us is how a client hears it: the host said yes, and the news
    // marks the locker occupied -- by us -- before it gets here. Refusing that as "occupied" was why a
    // client never really got into a locker: pinned in it by the host, but looking about freely.
    if (spot == nullptr || (spot->occupied && spot->occupant != LocalPlayerId()))
    {
        return;
    }

    spot->occupied = true;
    spot->occupant = LocalPlayerId();
    m_hidingSpot = index;
    // Attached, not teleported. A locker is barely wider than the player's capsule, so left to
    // simulate normally Jolt pushes them straight back out through the side of it.
    m_player.Attach(spot->insidePosition, spot->insideYaw);
    m_lookYaw = spot->insideYaw;
    // Eyes to the slits.
    m_lookPitch = 0.0f;
    // The door swings shut behind the player, which is most of what makes hiding feel like hiding.
    m_world.SetDoorOpen(spot->doorIndex, false, m_interactions);
    m_interactions.SetVerb(spot->entity, "Leave");
    m_app->GetConsole().Print("Hidden. Press " + KeyFor(m_app->GetInput(), "interact") + " to leave.");
}

void PredationGame::PressInteract()
{
    // Alive it opens doors; dead it is the only control there is, so it moves you to the next teammate. A
    // free camera is deliberately not offered: it would show a dead player where the creature is, which is
    // the one thing being dead should not tell them.
    if (m_player.State().alive)
    {
        TryInteract();
    }
    else if (!m_supportDrone.Active())
    {
        m_spectateNext = true;
    }
}

void PredationGame::LeaveHidingSpot()
{
    WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(m_hidingSpot);
    m_hidingSpot = -1;
    if (spot == nullptr)
    {
        return;
    }
    spot->occupied = false;
    m_world.SetDoorOpen(spot->doorIndex, true, m_interactions);
    m_interactions.SetVerb(spot->entity, "Hide in");
    m_player.Detach(spot->exitPosition);
}

void PredationGame::OnFixedUpdate(double fixedDt)
{
    const auto dt = static_cast<float>(fixedDt);
    m_world.Update(m_scene, m_app->GetPhysics(), m_interactions, dt);

    PlayerInput input = BuildPlayerInput();
    if (m_screen == Screen::Title)
    {
        // The world keeps simulating behind the menu so the backdrop is alive and starting a game
        // is instant, but nothing the player does at the menu reaches the character.
        input = PlayerInput{};
        input.yaw = m_player.State().yaw;
        input.pitch = m_player.State().pitch;
    }
    m_lastInput = input;
    // Dead counts as restrained: a body on the floor does not fire, aim or reload.
    const bool restrained = m_hidingSpot >= 0 || m_cameraMode == CameraMode::Fly || !m_player.State().alive ||
                            HeldLocally();

    // The weapon runs before the movement, because aiming down the sights slows the player and the
    // controller needs that this tick rather than next.
    WeaponInput weaponInput;
    if (!restrained && !m_inventoryOpen && !m_loadoutOpen && !m_mapOpen && !CinematicHoldsPlayers())
    {
        Input& raw = m_app->GetInput();
        // Held, or pressed since the last tick: a click that goes down and up between two ticks
        // is still a shot.
        weaponInput.trigger = raw.IsActionDown("fire") || m_firePressLatch || m_debugTriggerTicks > 0;
        // And only where the sights would mean anything. Against a wall the body refuses to raise
        // them, so the simulation refuses too: otherwise the player would be walking at aiming
        // pace and shooting at aiming accuracy while looking at a weapon held at their hip.
        weaponInput.aim = (raw.IsActionDown("aim") || m_debugAim) && m_body.AimHasRoom();
        // The press itself is noticed in OnUpdate, once per frame, and held here. Asking "was it
        // pressed" from inside a fixed tick only works on the frames that have a tick in them, and
        // at a couple of hundred frames a second most do not: the key went down on a frame with no
        // tick, the frame after no longer counted it as pressed, and so three presses in four did
        // nothing at all.
        weaponInput.reload = m_reloadLatch > 0;
    }
    m_firePressLatch = false;
    m_debugTriggerTicks = std::max(m_debugTriggerTicks - 1, 0);

    m_shots.clear();
    if (const WeaponDefinition* definition = EquippedWeapon())
    {
        WeaponSim::Step(*definition, weaponInput, m_weapon, MuzzlePosition(), AimDirection(), dt, m_shots);
        input.speedScale = 1.0f - (1.0f - definition->aimSpeedScale) * m_weapon.aim;
    }
    else
    {
        WeaponSim::Step(WeaponDefinition{}, weaponInput, m_weapon, MuzzlePosition(), AimDirection(), dt,
                        m_shots);
    }

    // A click, and nothing: the trigger pulled with no rounds in it and none to put in.
    if (weaponInput.trigger && !m_dryTriggerWas && EquippedWeapon() != nullptr && m_weapon.rounds <= 0 &&
        m_weapon.reserve <= 0 && !m_weapon.IsReloading())
    {
        PlayNamed("Weapons/dry_fire", MuzzlePosition(), 0.6f, 1.0f, false);
    }
    m_dryTriggerWas = weaponInput.trigger;

    // Whatever the weapon just kicked goes into the player's own aim, where it stays for them to
    // pull back down. Straight after the step, so the same tick that fires is the tick that moves.
    ApplyRecoilToView();

    // Single player, so this process is the authority. When there is a host, a client stops here and
    // sends m_shots instead; nothing above this line ever touches another player's health.
    ResolveShots();

    // The request is held for half a second rather than one tick, so pressing reload part way
    // through a shot still takes effect once the weapon can accept it.
    m_reloadLatch = m_weapon.IsReloading() ? 0 : std::max(m_reloadLatch - 1, 0);

    if (m_hidingSpot >= 0)
    {
        // Hidden: the player can still look around, and that is all. Crouching or going prone
        // inside a locker put the body through the floor of it, and there is nowhere to lean to.
        input.move = glm::vec2(0.0f);
        input.jump = false;
        input.lean = 0.0f;
        input.crouchHeld = false;
        input.proneHeld = false;
        m_crouchToggleState = false;
        m_proneToggleState = false;
        m_forceCrouch = false;
        m_forceProne = false;
    }
    if (m_cameraMode == CameraMode::Fly)
    {
        // The free camera is an inspection tool, not a different game mode. The player keeps
        // simulating underneath it, standing still and holding its own facing, so the body stays
        // alive and stances can be examined from outside. Skipping the step entirely froze the
        // player and made every stance look identical.
        input.move = glm::vec2(0.0f);
        input.jump = false;
        input.yaw = m_player.State().yaw;
        input.pitch = m_player.State().pitch;
    }

    // The water to swim in, as the site has it now.
    UpdateWater();
    // Wading: slower the deeper the water (or acid), about half pace by the knees. (Ice is walked on; lava is not walked in.)
    using Liquid = SiteMap::Look::Liquid;
    const Liquid liquid = m_facility.GetLook().liquid;
    if (m_facility.Built() && m_facility.Contains(m_player.State().position) && (liquid == Liquid::Water || liquid == Liquid::Acid))
    {
        const glm::vec3 at = m_player.State().position;
        const float depth = m_facility.Terrain().WaterDepth(at.x, at.z);
        if (depth > 0.05f)
        {
            input.speedScale *= glm::mix(1.0f, 0.45f, glm::smoothstep(0.1f, 0.9f, depth));
        }
    }

    // In a session the host and the client each own how the local player is stepped: the host runs
    // it directly and then everyone else, the client runs it inside prediction so that the first
    // guess and every replay of it go through exactly the same code.
    if (!StepSession(input, dt))
    {
        m_player.Step(input, dt);
    }
    // No further round a site than anybody may go (SiteTerrain::kRoam): the land goes on to the horizon, the shuttle's comms do
    // not -- turned back at the edge, and told so.
    if (m_facility.Built() && m_player.State().alive && m_facility.Contains(m_player.State().position))
    {
        const glm::vec3 at = m_player.State().position;
        const glm::vec3 kept = m_facility.WithinRoam(at);
        if (glm::distance(glm::vec2(at.x, at.z), glm::vec2(kept.x, kept.z)) > 1.0e-3f)
        {
            m_player.Teleport(kept);
            if (m_time - m_roamToldAt > 4.0)
            {
                m_roamToldAt = m_time;
                ShowCampaignNotice("Out of the shuttle's comms range. Turn back.");
            }
        }
        // Water is swum in (PlayerController::StepSwim). Acid no deeper than the waist: put back where it was last shallow
        // enough, and it burns through the suit for as long as it is waded in. Lava can be walked into, and burns as long as
        // anybody is in it (below). Frozen, it is ground like any other.
        const float depth = m_facility.Terrain().WaterDepth(at.x, at.z);
        if (liquid != Liquid::Ice && liquid != Liquid::Lava)
        {
            if (liquid == Liquid::Acid && depth > 1.0f && glm::length(m_lastWadable) > 0.0f)
            {
                m_player.Teleport(m_lastWadable);
                if (m_time - m_roamToldAt > 4.0)
                {
                    m_roamToldAt = m_time;
                    ShowCampaignNotice("Too deep to wade any further in acid.");
                }
            }
            else if (depth < 0.8f && m_player.State().grounded)
            {
                m_lastWadable = at;
            }
            if (liquid == Liquid::Acid && depth > 0.05f)
            {
                if (m_time - m_roamToldAt > 4.0)
                {
                    m_roamToldAt = m_time;
                    ShowCampaignNotice("Acid. It is eating through the suit.");
                }
            }
        }
    }
    // Burns, by whoever decides what hurts, for everybody from where they are: waded in acid, or in lava -- only in it, not
    // near it (by its heat a third of a metre over its level, the ground metres back from a gentle shore burnt), and fast:
    // a few seconds at the ankles, a moment deeper. Given out a few at a time, not a sliver every frame.
    if (IsAuthority() && m_facility.Built() && (liquid == Liquid::Acid || liquid == Liquid::Lava))
    {
        std::vector<std::pair<uint8_t, glm::vec3>> here;
        if (m_player.State().alive)
        {
            here.emplace_back(LocalPlayerId(), m_player.State().position);
        }
        if (m_sessionMode == SessionMode::Host)
        {
            for (const RemotePlayerView& remote : m_host.Remotes())
            {
                if (remote.id != LocalPlayerId() && remote.alive)
                {
                    here.emplace_back(remote.id, remote.position);
                }
            }
        }
        for (const auto& [id, at] : here)
        {
            if (!m_facility.Contains(at))
            {
                continue;
            }
            const float depth = m_facility.Terrain().WaterDepth(at.x, at.z);
            const bool burning = depth > (liquid == Liquid::Acid ? 0.05f : 0.02f);
            float& burn = m_liquidBurns[id];
            burn += burning ? dt * (liquid == Liquid::Acid ? 8.0f : 35.0f + 150.0f * std::min(depth, 1.0f)) : 0.0f;
            if (burn >= 4.0f)
            {
                ApplyPlayerDamage(id, burn, kNoKiller, glm::vec3(0.0f, -1.0f, 0.0f), liquid == Liquid::Lava ? "lava" : "acid");
                burn = 0.0f;
            }
        }
    }
    // Dead, the same input drives the drone, and its owner tells everybody where it is.
    UpdateDrone(input, dt);
    if (m_sessionMode == SessionMode::Host)
    {
        m_host.SetPlayerDrone(0, LocalDroneState());
    }
    else if (m_sessionMode == SessionMode::Client)
    {
        m_client.SetDrone(LocalDroneState());
    }

    UpdateHostLost();
    UpdateRespawns(dt);
    // A navigation rebuild finished on a worker thread goes in here, between ticks, before anything
    // plans a route this tick.
    FinishNavRebuild();
    {
        PRED_PROFILE_SCOPE("Fixed: creatures");
        // Not while a cinematic holds the world: nobody is in it to be hunted.
        if (!CinematicHoldsWorld())
        {
            UpdateCreatures(dt);
        }
    }
    UpdateDroneThreats(dt);
    UpdateWipe(dt);
    UpdateMission(dt);
    UpdateShip();

    // The toggles mirror the stance the body is actually in, every tick, not just when a change is
    // refused. They are a request, and the body is the answer; a request that has been answered is
    // spent. Anything else lets the two drift apart, and then the next press asks for the stance you
    // are already in and nothing happens.
    //
    // On a client this matters twice over. The host owns the stance, and reconciliation writes its
    // answer over the prediction: with the toggle left alone, a client could hold "standing" while
    // the host held "prone", stand up locally every tick and be pulled back down by the next
    // snapshot. Spamming the key was the quickest way to get there.
    // (Not while swimming: a swimmer's body is a crouched one, and read back as crouch toggled on it dived and kept diving.)
    if (cv_crouchToggle.Get() && !m_player.State().swimming)
    {
        m_crouchToggleState = m_player.State().stance == PlayerStance::Crouching;
        m_proneToggleState = m_player.State().stance == PlayerStance::Prone;
    }
}

void PredationGame::OnUpdate(double dt, double alpha)
{
    m_time += dt;
    UpdatePerfReport();

    Application& app = *m_app;
    Input& input = app.GetInput();
    Renderer& renderer = app.GetRenderer();
    const auto deltaSeconds = static_cast<float>(dt);
    // A frame that took long enough to be seen as a stutter, in the log with when it was, so it can be found.
    if (dt > 0.03 && (m_screen == Screen::Playing || m_screen == Screen::Title))
    {
        // With what took the time in it, the longest parts first.
        std::vector<FrameStats::Timing> parts = FrameStats::Instance().Timings();
        std::sort(parts.begin(), parts.end(), [](const FrameStats::Timing& a, const FrameStats::Timing& b) { return a.milliseconds > b.milliseconds; });
        std::string where;
        for (size_t i = 0; i < parts.size() && i < 3; ++i)
        {
            where += fmt::format("{}{} {:.0f}", i > 0 ? ", " : "", parts[i].name, parts[i].milliseconds);
        }
        PRED_LOG_INFO(Engine, "Long frame: {:.0f} ms{} [{}]", dt * 1000.0, m_cine.Active() ? " (in a cinematic)" : m_screen == Screen::Title ? " (title)" : "",
                      where);
    }

    {
        PRED_PROFILE_SCOPE("Update: campaign");
        UpdateCampaign(deltaSeconds);
    }
    {
        PRED_PROFILE_SCOPE("Update: travel");
        UpdateTravel(deltaSeconds);
    }

    // --- Input that is sampled per frame, not per tick ------------------------------------------
    SampleLook(deltaSeconds);
    UpdateGrabbedView(deltaSeconds);

    // The editor drives its own camera and hides the world, so nothing below has to know it exists.
    //
    // The cursor stays free, because everything in an editor is done by pointing at it. Holding the
    // right button takes the mouse for as long as it is held and turns the camera, then hands it
    // straight back. Keeping the mouse locked the whole time, as this used to, made every panel
    // unreachable; releasing it without this made the camera impossible to turn.
    if (m_editor.IsOpen())
    {
        const bool wantLook = input.IsMouseDown(MouseButton::Right) &&
                              (m_editorLooking || !app.IsUiCapturingMouse());
        if (wantLook != m_editorLooking)
        {
            m_editorLooking = wantLook;
            m_wantMouseCaptured = wantLook;
            UpdateMouseCapture();
        }
        if (m_editorLooking && !m_discardNextMouseDelta)
        {
            const glm::vec2 delta = input.MouseDelta();
            const float sensitivity = glm::radians(cv_mouseSensitivity.Get());
            m_lookYaw += delta.x * sensitivity;
            m_lookPitch = std::clamp(m_lookPitch - delta.y * sensitivity, glm::radians(-89.0f),
                                     glm::radians(89.0f));
        }
        m_discardNextMouseDelta = false;

        m_editor.Camera().yaw = m_lookYaw;
        m_editor.Camera().pitch = m_lookPitch;
        m_editor.Camera().moveSpeed = m_editor.CameraSpeed();
        // WASD moves whenever the editor has the keyboard, held button or not: an editor where you
        // have to hold a mouse button to walk is one you cannot drive with one hand.
        //
        // Ctrl is left out of it. The fly camera takes Ctrl as "down", and in here Ctrl is the
        // modifier on undo and redo, so every Ctrl+Z sank the view. Q and E do up and down instead,
        // which is where an editor usually puts them, and Space still lifts.
        m_editor.Camera().ctrlMovesDown = false;
        const bool typing = ImGui::GetIO().WantCaptureKeyboard;
        if (!typing)
        {
            m_editor.Camera().Update(input, deltaSeconds, false);
            const float vertical = (input.IsActionDown("lean_right") ? 1.0f : 0.0f) -
                                   (input.IsActionDown("lean_left") ? 1.0f : 0.0f);
            m_editor.Camera().position.y += vertical * m_editor.CameraSpeed() * deltaSeconds;
        }
        m_editor.Update(m_editorScene, app.GetMeshes(), deltaSeconds);
        UpdateEditorBody(deltaSeconds);

        // The editor knows how big the model is and the game owns the camera, so framing is asked
        // for rather than done. A model that opens off screen looks exactly like one that failed to
        // load, and either way the first thing anyone does is go hunting for it.
        glm::vec3 framePosition{0.0f};
        glm::vec3 frameTarget{0.0f};
        if (m_editor.TakeFrameRequest(framePosition, frameTarget))
        {
            m_camera.position = framePosition;
            const glm::vec3 toTarget = frameTarget - framePosition;
            if (glm::length(toTarget) > 1e-4f)
            {
                const glm::vec3 look = glm::normalize(toTarget);
                m_lookYaw = std::atan2(look.x, -look.z);
                m_lookPitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
            }
            m_editor.Camera().position = m_camera.position;
        }

        m_camera = m_editor.Camera();

        // Pointing at the viewport: grabbing a handle, dragging it, or selecting what is under it.
        // The game owns the camera and the pointer and the editor owns the model, so the ray is
        // built here and every question about it answered there. Not while the right button is
        // held, because that is looking around, and not over a panel, because that belongs to the
        // panel.
        if (!m_editorLooking && (!app.IsUiCapturingMouse() || m_editor.Dragging()))
        {
            const float width = static_cast<float>(std::max<int>(renderer.Width(), 1));
            const float height = static_cast<float>(std::max<int>(renderer.Height(), 1));
            const glm::vec2 pointer = input.MousePosition();

            // From the pointer through the near plane, in the same projection the frame was drawn
            // with. Working it out from the field of view rather than by unprojecting a matrix keeps
            // this readable, and the two agree because both start from the same cvar.
            const float aspect = width / height;
            const float halfHorizontal = std::tan(glm::radians(cv_fov.Get()) * 0.5f);
            const float halfVertical = halfHorizontal / aspect;
            const float x = (pointer.x / width) * 2.0f - 1.0f;
            const float y = 1.0f - (pointer.y / height) * 2.0f;

            const glm::vec3 forward = m_camera.Forward();
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
            const glm::vec3 up = glm::cross(right, forward);
            const glm::vec3 direction = glm::normalize(forward + right * (x * halfHorizontal) +
                                                       up * (y * halfVertical));
            // A press either grabs a handle or selects. Grabbing wins, because the handles sit on
            // top of the thing they move and a click on one is never a click on it.
            if (input.WasMousePressed(MouseButton::Left))
            {
                if (!m_editor.BeginDrag(m_camera.position, direction))
                {
                    m_editor.SelectUnderRay(m_camera.position, direction);
                }
            }
            else if (m_editor.Dragging())
            {
                if (input.IsMouseDown(MouseButton::Left))
                {
                    m_editor.UpdateDrag(m_camera.position, direction);
                }
                else
                {
                    m_editor.EndDrag();
                }
            }
        }

        // Undo. Ctrl+Z and Ctrl+Y, because those are the two every editor uses and an editor that
        // does not have them is one nobody dares experiment in.
        if (!app.IsConsoleOpen() && !app.IsUiCapturingKeyboard())
        {
            const bool control =
                input.IsKeyDown(SDL_SCANCODE_LCTRL) || input.IsKeyDown(SDL_SCANCODE_RCTRL);
            const bool shift =
                input.IsKeyDown(SDL_SCANCODE_LSHIFT) || input.IsKeyDown(SDL_SCANCODE_RSHIFT);
            if (control && input.WasKeyPressed(SDL_SCANCODE_Z))
            {
                if (shift)
                {
                    m_editor.Redo();
                }
                else
                {
                    m_editor.Undo();
                }
            }
            if (control && input.WasKeyPressed(SDL_SCANCODE_Y))
            {
                m_editor.Redo();
            }
            if (!control && input.WasKeyPressed(SDL_SCANCODE_F))
            {
                m_editor.FrameModel();
            }
        }
    }

    // The cinematic editor has the pointer and the keys while it is open: the players are held meanwhile.
    UpdateCinematicEditor(deltaSeconds);

    // Nothing bound to a game key does anything at the menu. The menu is pointed at and typed into,
    // and a stray W while filling in an address must not make the character walk.
    //
    // Dead, what is still yours: the pause menu, the drone -- its hop is the jump key, its lamp the torch key --
    // and, without one, whose view to watch (interact). Everything else waits for a body.
    const bool dead = !m_player.State().alive;
    if (!app.IsConsoleOpen() && m_screen == Screen::Playing && dead && !CinematicHoldsPlayers())
    {
        if (input.WasActionPressed("jump"))
        {
            m_jumpLatch = true;
        }
        if (input.WasActionPressed("flashlight") && m_supportDrone.Active())
        {
            m_supportDrone.lightOn = !m_supportDrone.lightOn;
            PlayNamed(m_supportDrone.lightOn ? "Player/torch_on" : "Player/torch_off", m_supportDrone.Position(), 0.4f, 1.2f, true);
        }
        if (input.WasActionPressed("interact") && !m_paused)
        {
            PressInteract();
        }
#if PRED_DEV_TOOLS
        if (input.WasActionPressed("respawn"))
        {
            RespawnLocalPlayer(m_spawnPoint);
        }
#endif
    }

    // Nor does anything while dead. A body on the floor was still picking things up, opening doors,
    // getting into lockers, dropping its inventory and changing what it had in hand: none of the
    // actions asked whether the player was alive, because the camera moves to a teammate and it
    // looked as though nothing was being driven any more. Movement is already refused by the
    // controller; this is everything else.
    if (!app.IsConsoleOpen() && m_screen == Screen::Playing && !dead && !m_paused && !CinematicHoldsPlayers())
    {
        if (input.WasActionPressed("jump"))
        {
            m_jumpLatch = true;
        }
        // The same for the weapon. Reload is held for half a second so a press part way through a
        // shot still takes once the weapon can accept it; a fire press only until the next tick.
        if (input.WasActionPressed("reload"))
        {
            m_reloadLatch = 30;
        }
        // Not a click that was aimed at a menu: the one that pressed Resume must not also fire.
        if (input.WasActionPressed("fire") && !m_paused && !m_inventoryOpen && !m_loadoutOpen && !m_mapOpen && !CinematicHoldsPlayers() &&
            !ImGui::GetIO().WantCaptureMouse)
        {
            m_firePressLatch = true;
        }
        // Latched rather than applied here, for the same reason a jump is. Fixed updates run before
        // this function every frame, so a stance toggled here was not seen by the simulation until
        // the frame after, and on a frame with no fixed step at all it could be missed entirely.
        if (input.WasActionPressed("crouch"))
        {
            m_crouchPressLatch = true;
        }
        if (input.WasActionPressed("prone"))
        {
            m_pronePressLatch = true;
        }
        if (input.WasActionPressed("sprint") && cv_sprintToggle.Get())
        {
            m_sprintToggleState = !m_sprintToggleState;
        }
        if (input.WasActionPressed("interact"))
        {
            PressInteract();
        }
        if (input.WasActionPressed("drop"))
        {
            DropSelected();
        }
        if (input.WasActionPressed("inventory"))
        {
            m_loadoutOpen = false;
            m_inventoryOpen = !m_inventoryOpen;
            // The panel is clickable, so it needs the pointer back.
            m_wantMouseCaptured = !m_inventoryOpen;
        }
        // Nothing changes hands while both of them are on a ledge. The climb has put away whatever
        // was out and will take it back out at the top; letting a number key run in the middle of
        // that would leave the wrong thing in the hands when the climb finished, because the climb
        // restores what it took rather than what is selected now.
        if (HandsAreFree())
        {
            for (int slot = 0; slot < 6; ++slot)
            {
                if (input.WasActionPressed("slot_" + std::to_string(slot + 1)))
                {
                    // Pressing the number for what is already out puts it away again. One key for
                    // both, because a separate holster key is one nobody finds.
                    m_inventory.SelectSlot(m_inventory.SelectedSlot() == slot ? Inventory::kNoSlot
                                                                              : slot);
                }
            }
            if (const float wheel = input.WheelDelta(); std::abs(wheel) > 0.1f)
            {
                m_inventory.SelectNext(wheel > 0.0f ? -1 : 1);
            }
        }
#if PRED_DEV_TOOLS
        if (input.WasActionPressed("toggle_camera"))
        {
            // Cycles first person, third person, fly. Third person exists so the body animation can
            // actually be watched, which is impossible from inside the head.
            SetCameraMode(m_cameraMode == CameraMode::FirstPerson    ? CameraMode::ThirdPerson
                          : m_cameraMode == CameraMode::ThirdPerson ? CameraMode::Fly
                                                                    : CameraMode::FirstPerson);
        }
#endif
        if (input.WasActionPressed("flashlight") && !m_torchOn && m_torchCharge <= 0.0f)
        {
            PlayNamed("Player/torch_off", m_player.State().position, 0.45f, 0.8f, false);
            m_app->GetConsole().Print("The torch cell is flat. It needs a fresh battery.");
        }
        else if (input.WasActionPressed("flashlight"))
        {
            m_torchOn = !m_torchOn;
            PlayNamed(m_torchOn ? "Player/torch_on" : "Player/torch_off", m_player.State().position, 0.45f, 1.0f, false);
        }
#if PRED_DEV_TOOLS
        if (input.WasActionPressed("respawn"))
        {
            RespawnLocalPlayer(m_spawnPoint);
        }
#endif
    }
    // Alive or dead -- but not while a cinematic has the picture: nothing opens over one.
    if (!app.IsConsoleOpen() && m_screen == Screen::Playing && !m_cine.Active() && !m_cineEditor.IsOpen())
    {
        if (input.WasActionPressed("quit_capture"))
        {
            // Escape opens the pause menu and frees the pointer; Escape again closes it and takes
            // the pointer back. It used to leave the game outright on the second press, which meant
            // there was no way to let go of the mouse for a moment without ending up at the title
            // screen, and no way to leave deliberately either: the same key did both.
            //
            // Settings is a page inside the pause menu, so Escape there goes back one step rather
            // than all the way out. Escape should undo the last thing you opened, and dropping
            // straight into the game from three levels in is a surprise every time.
            if (m_paused && m_settingsOpen)
            {
                m_settingsOpen = false;
            }
            else if (m_loadoutOpen)
            {
                CloseLoadout();
            }
            else if (m_mapOpen)
            {
                CloseSystemMap();
            }
            else if (m_inventoryOpen)
            {
                m_inventoryOpen = false;
                m_wantMouseCaptured = true;
            }
            else
            {
                m_paused = !m_paused;
                m_wantMouseCaptured = !m_paused;
            }
        }
    }

    // Escape leaves the editor for the menu. It is a place rather than an overlay, so leaving it is
    // going somewhere else rather than switching something off.
    if (!app.IsConsoleOpen() && m_screen == Screen::Editor &&
        input.WasActionPressed("quit_capture") && !m_editorLooking)
    {
        ReturnToTitle();
    }
    UpdateMouseCapture();

    // --- Camera -----------------------------------------------------------------------------------
    // Always update the player's view, even while flying. It is presentation-only, and the body is
    // placed from its interpolated position, so skipping it would leave the body behind at the
    // origin whenever the free camera is active.
    m_player.UpdateView(deltaSeconds, static_cast<float>(alpha));
    if (cv_eyeLog.Get())
    {
        const PlayerView& eye = m_player.View();
        PRED_LOG_INFO(Gameplay, "eye {:.3f} over feet (wanted {:.3f}), feet y {:.3f} xz {:.2f} {:.2f}", eye.eyePosition.y - eye.renderPosition.y,
                      eye.eyeHeight + eye.stepOffset + eye.landingDip + eye.bobOffset, eye.renderPosition.y, eye.renderPosition.x, eye.renderPosition.z);
    }

    UpdateDroneVisuals(deltaSeconds);
    {
        PRED_PROFILE_SCOPE("Update: cinematic");
        UpdateCinematic(deltaSeconds);
    }
    {
        PRED_PROFILE_SCOPE("Update: devices");
        UpdateDevices(deltaSeconds);
    }
    {
        PRED_PROFILE_SCOPE("Update: ship travel");
        UpdateShipTravel(deltaSeconds);
    }
    {
        PRED_PROFILE_SCOPE("Update: orders");
        UpdateOrders(deltaSeconds);
    }
    m_particles.Update(m_scene, deltaSeconds);
    // The organism's rhythm, where its signs are plain (Signs::Pulse, by the campaign's clock, so the same moment for everybody
    // and on every world): a faint glow through its formations as it swells, and the lamps dipping with it where it is
    // strongest. Nothing said about it.
    {
        const float signs = m_facility.Built() ? m_facility.GetLook().signs : 0.0f;
        const float pulse = signs > 0.5f ? Signs::Pulse(m_campaign.clock, m_universeData.signRhythm) : 0.0f;
        const float plain = std::clamp((signs - 0.5f) * 2.0f, 0.0f, 1.0f);
        // And each formation swelling a little with it, about its foot, and settling: slow, as breathing is, and only seen by
        // somebody watching (a percent and a half at the most).
        const float swell = 1.0f + 0.015f * pulse * glm::smoothstep(0.3f, 1.0f, signs);
        const std::vector<Entity>& signEntities = m_facility.SignEntities();
        const std::vector<glm::vec3>& feet = m_facility.SignFeet();
        for (size_t i = 0; i < signEntities.size(); ++i)
        {
            if (MeshRenderer* sign = m_scene.GetMeshRenderer(signEntities[i]))
            {
                sign->material.emissive = m_universeData.signGlow * (pulse * plain * 0.8f);
            }
            if (Transform* placed = i < feet.size() ? m_scene.GetTransform(signEntities[i]) : nullptr)
            {
                placed->position = feet[i] * (1.0f - swell);
                placed->scale = glm::vec3(swell);
            }
        }
        m_levelLights.SetDim(1.0f - 0.35f * pulse * glm::smoothstep(0.65f, 1.0f, signs));
    }
    // The site's weather, blowing with its wind -- not indoors, and not in the shuttle's cabin.
    {
        // How much of its weather is coming down just now: its spells come and go (Weather::SpellAt), by the campaign's clock.
        if (m_facility.Built() && !m_weatherHeld)
        {
            m_facility.SetWeatherAmount(std::clamp(m_climateAmount * Weather::SpellAt(m_facility.Seed(), m_campaign.clock), 0.0f, 1.0f));
        }
        const SiteMap::Look& look = m_facility.GetLook();
        const bool falling = m_screen == Screen::Playing && m_facility.Built() && m_facility.Contains(m_renderEye);
        const uint32_t seed = m_facility.Seed();
        const float windAngle = SiteMap::WindAngle(seed);
        const float windSpeed = falling ? static_cast<float>(PlaceConditions(m_facility.Plan().sky.fogEnd).wind) * 0.12f : 0.0f;
        float ground = m_renderEye.y - 1.7f;
        if (falling)
        {
            if (const RayHit below = m_app->GetPhysics().RayCastStatic(m_renderEye, {0.0f, -1.0f, 0.0f}, 60.0f))
            {
                ground = below.position.y;
            }
        }
        m_facility.ShowFarLand(m_scene, m_renderEye);
        // The grass round the eye, where the world grows it.
        if (falling)
        {
            m_facility.UpdateCover(m_scene, m_app->GetMeshes(), m_cameraMode == CameraMode::Fly ? m_camera.position : m_renderEye,
                                   std::clamp(cv_grass.Get(), 0.0f, 1.5f));
        }
        // And the plants sway with it: the wind itself, gusting (Environment::wind).
        m_windClock += deltaSeconds;
        const float plantWind = falling ? static_cast<float>(PlaceConditions(m_facility.Plan().sky.fogEnd).wind) * Weather::GustAt(m_windClock) : 0.0f;
        m_scene.GetEnvironment().wind = glm::vec2(std::cos(windAngle), std::sin(windAngle)) * plantWind;
        m_scene.GetEnvironment().windClock = m_windClock;
        m_windTravel = glm::mod(m_windTravel + m_scene.GetEnvironment().wind * deltaSeconds, glm::vec2(100000.0f));
        // The clouds drift on the wind up there -- steady, a few times the ground's, its gusts left on the ground -- by the
        // campaign's clock, so every player sees the same sky.
        {
            const float aloft = static_cast<float>(PlaceConditions(m_facility.Plan().sky.fogEnd).wind) * 3.0f + 4.0f;
            const float since = static_cast<float>(std::fmod(m_campaign.clock, 20000.0));
            m_scene.GetEnvironment().cloudDrift = glm::vec2(std::cos(windAngle), std::sin(windAngle)) * (aloft * since * 0.001f);
        }
        // And what the weather leaves on things: wet in rain, getting so over a minute and drying over several; snow lying
        // as it falls, over a few minutes, and going slower still -- and on a frozen world, old snow on everything that faces
        // up whatever the sky is doing. A site just arrived at is as wet or snowed on as the last hour's weather has left it.
        {
            const Weather::Kind kind = Weather::KindOf(look.weather);
            const bool built = m_facility.Built();
            const auto wetFor = [&](float amount) { return built && kind == Weather::Kind::Rain ? std::clamp(amount * 1.3f, 0.0f, 1.0f) : 0.0f; };
            const auto snowFor = [&](float amount)
            { return built ? std::max(kind == Weather::Kind::Snow ? std::min(amount, 1.0f) : 0.0f, look.biome == "frozen" ? 0.55f : 0.0f) : 0.0f; };
            const auto towards = [](float& value, float to, float up, float down, float seconds)
            {
                const float step = (to > value ? up : down) * seconds;
                value = to > value ? std::min(value + step, to) : std::max(value - step, to);
            };
            constexpr float kWetting = 1.0f / 60.0f;
            constexpr float kDrying = 1.0f / 400.0f;
            constexpr float kSnowing = 1.0f / 300.0f;
            constexpr float kMelting = 1.0f / 900.0f;
            if (seed != m_surfaceWeatherSite)
            {
                m_surfaceWeatherSite = seed;
                m_surfaceWet = 0.0f;
                m_surfaceSnow = snowFor(0.0f);
                for (double back = 3600.0; back > 0.0; back -= 20.0)
                {
                    const float then = m_weatherHeld ? look.weatherAmount : m_climateAmount * Weather::SpellAt(seed, m_campaign.clock - back);
                    towards(m_surfaceWet, wetFor(then), kWetting, kDrying, 20.0f);
                    towards(m_surfaceSnow, snowFor(then), kSnowing, kMelting, 20.0f);
                }
                // The shuttle comes down into it now: the ground under it kept as it is, and itself dry and clean.
                m_shuttleOut = 0.0f;
                m_shelterWet = m_surfaceWet;
                m_shelterSnow = m_surfaceSnow;
            }
            towards(m_surfaceWet, wetFor(look.weatherAmount), kWetting, kDrying, deltaSeconds);
            towards(m_surfaceSnow, snowFor(look.weatherAmount), kSnowing, kMelting, deltaSeconds);
            // Under the shuttle it dries as it does anywhere, and the snow there goes as it goes everywhere else.
            towards(m_shelterWet, 0.0f, 0.0f, kDrying, deltaSeconds);
            m_shelterSnow = std::min(m_shelterSnow, m_surfaceSnow);
            m_shuttleOut += deltaSeconds;
            Environment& surface = m_scene.GetEnvironment();
            surface.wetness = falling ? m_surfaceWet : 0.0f;
            surface.snowCover = falling ? m_surfaceSnow : 0.0f;
            surface.waterLevel = falling && m_facility.Terrain().HasWater() ? m_facility.Terrain().WaterLevel() : -1.0e9f;
            surface.waterClock = static_cast<float>(std::fmod(m_campaign.clock, 3600.0));
            surface.waterWets = m_facility.GetLook().liquid == SiteMap::Look::Liquid::Lava ? 0.0f : 1.0f;
            VehicleProp& shuttle = m_facility.Shuttle();
            if (falling && shuttle.Built())
            {
                // Wet soon and snowed on slowly as it has been out longer (short of the ground's 1: it is not the ground).
                shuttle.SetWeathering(m_scene, std::min(m_shuttleOut * kSnowing, 0.99f));
                const glm::vec3 middle = shuttle.Middle();
                surface.shelter = {middle.x, middle.z, shuttle.Reach()};
                surface.shelterWet = std::min(m_shelterWet, 1.0f);
                surface.shelterSnow = m_shelterSnow;
            }
            else
            {
                surface.shelter = glm::vec3(0.0f);
                surface.shelterWet = surface.shelterSnow = 0.0f;
            }
            UpdateLightning(deltaSeconds, falling ? (kind == Weather::Kind::Rain   ? glm::smoothstep(0.75f, 0.95f, look.weatherAmount)
                                                     : kind == Weather::Kind::Ash ? 0.6f * glm::smoothstep(0.85f, 1.0f, look.weatherAmount)
                                                                                  : 0.0f)
                                                  : 0.0f);
        }
        // Lit and fogged as last frame's world was (the light changes slowly).
        const Environment& environment = m_scene.GetEnvironment();
        Weather::Light light;
        light.light = environment.ambientSky * 1.4f + environment.ambientGround * 0.4f + environment.sunColor * environment.sunIntensity * 0.55f;
        light.light += glm::vec3(0.75f, 0.8f, 1.0f) * m_lightningFlash * 1.2f;
        light.fogColour = environment.fogColor;
        light.fogStart = environment.fogStart;
        light.fogEnd = environment.fogEnd;
        m_weather.Update(m_renderEye, ground, deltaSeconds, Weather::KindOf(look.weather), falling ? look.weatherAmount : 0.0f, look.ground,
                         glm::vec3(std::cos(windAngle), 0.0f, std::sin(windAngle)) * windSpeed, light, cv_weatherDetail.Get(),
                         [this](const glm::vec3& at, float reach)
                         {
                             // From under a roof nothing may reach in at all; from outside, a wall hides whatever goes
                             // through it, and only what would show in a doorway matters (eased between, as the fog is).
                             const float margin = glm::mix(reach, std::min(reach, 0.6f), m_eyeOutdoors);
                             return m_facility.Plan().Indoors(at, margin) || m_facility.Shuttle().Within(at, 0.4f + margin);
                         });
    }

    glm::mat4 view;
    glm::vec3 viewPosition;
    switch (m_cameraMode)
    {
    case CameraMode::Fly:
        if (m_screen == Screen::Title)
        {
            // The menu drives the camera itself, and typing an address into it must not fly it.
            UpdateTitleCamera(deltaSeconds);
        }
        else
        {
            m_camera.yaw = m_lookYaw;
            m_camera.pitch = m_lookPitch;
            m_camera.moveSpeed = cv_flySpeed.Get();
            m_camera.Update(input, deltaSeconds, false); // look is applied above, this only moves
        }
        view = m_camera.View();
        viewPosition = m_camera.position;
        break;

    case CameraMode::ThirdPerson:
    {
        // Orbits the torso along the look direction. Framing on the body itself, rather than a fixed
        // height above the feet, keeps the character centred in every stance; a constant height
        // works for standing and then looks straight over the top of them once they lie down. The
        // body follows the interpolated position, so the camera inherits that and does not judder.
        const glm::vec3 focus = (m_body.GetPose().GlobalPosition(m_body.Rig().head) +
                                 m_body.GetPose().GlobalPosition(m_body.Rig().pelvis)) *
                                0.5f +
                                (m_player.View().renderPosition - m_bodyPosedAt);
        const float orbitYaw = m_lookYaw + m_orbitYaw;
        const float orbitPitch = std::clamp(m_lookPitch + m_orbitPitch, glm::radians(-85.0f),
                                            glm::radians(85.0f));
        const float cp = std::cos(orbitPitch);
        const glm::vec3 lookDirection{std::sin(orbitYaw) * cp, std::sin(orbitPitch),
                                      -std::cos(orbitYaw) * cp};

        // Pull the camera in when something is in the way, so it does not end up inside a wall or a
        // pillar with the player hidden behind it.
        constexpr float kCameraSkin = 0.25f;
        m_thirdPersonDistance = std::clamp(cv_thirdDistance.Get(), 0.5f, 20.0f);
        float distance = m_thirdPersonDistance;
        const RayHit blocked =
            app.GetPhysics().RayCast(focus, -lookDirection, m_thirdPersonDistance + kCameraSkin);
        if (blocked)
        {
            distance = std::max(0.35f, blocked.distance - kCameraSkin);
        }

        viewPosition = focus - lookDirection * distance;
        // Never let the orbit drop the camera through the floor. Looking down at a prone character
        // puts the camera below a focus that is itself only a few centimetres up, and the occlusion
        // ray then pulls it hard into the body. The player's own feet are the ground reference.
        viewPosition.y = std::max(viewPosition.y, m_player.View().renderPosition.y + 0.4f);
        view = glm::lookAtRH(viewPosition, focus, glm::vec3(0.0f, 1.0f, 0.0f));
        break;
    }

    case CameraMode::FirstPerson:
    default:
    {
        // Dead and driving a drone: its camera is the only eye left.
        if (m_supportDrone.Active())
        {
            view = m_supportDrone.ViewMatrix();
            viewPosition = m_supportDrone.Eye();
            break;
        }
        // Dead, the camera moves to a living teammate's eyes. Their look angles come from the
        // snapshot, so you see what they see rather than steering a camera of your own.
        const RemotePlayerView* watched = nullptr;
        if (m_spectating >= 0)
        {
            for (const RemotePlayerView& remote : RemotePlayers())
            {
                if (remote.id == m_spectating)
                {
                    watched = &remote;
                    break;
                }
            }
        }

        // Their body is already being posed on this machine every frame, and it is anchored to an
        // eye that eases between stance heights exactly as your own does. Taking that eye rather
        // than working a second one out here is what stops the camera and the head it is inside
        // disagreeing: they were computed separately, so crouching moved one before the other and
        // the camera spent the difference inside the skull.
        RemoteAvatar* avatar = watched != nullptr ? AvatarFor(watched->id) : nullptr;
        if (avatar != nullptr)
        {
            PlayerView spectated = avatar->view;
            // The same eased angles their body is posed with, not the raw ones off the wire. Taking
            // the wire's meant the camera and the head it is inside were looking in slightly
            // different directions, and the difference arrived in steps.
            spectated.yaw = avatar->lookYaw;
            spectated.pitch = avatar->lookPitch;
            view = spectated.ViewMatrix();
            viewPosition = spectated.eyePosition;
            m_spectateEyeHeight = avatar->view.eyeHeight;
        }
        else if (watched != nullptr)
        {
            // No body for them yet, on the frame they joined. Eased, so the fallback does not snap
            // either.
            const float target = m_player.Config().EyeHeightForStance(watched->stance);
            m_spectateEyeHeight =
                m_spectateEyeHeight <= 0.0f
                    ? target
                    : m_spectateEyeHeight + (target - m_spectateEyeHeight) *
                                                (1.0f - std::exp(-m_player.Config().eyeTransitionSpeed *
                                                                 deltaSeconds));

            const glm::vec3 leanRight{std::cos(watched->yaw), 0.0f, std::sin(watched->yaw)};
            PlayerView spectated;
            spectated.eyePosition = watched->position + glm::vec3(0.0f, m_spectateEyeHeight, 0.0f) +
                                    leanRight * (watched->leanAmount * m_player.Config().leanSideOffset);
            spectated.yaw = watched->yaw;
            spectated.pitch = watched->pitch;
            spectated.leanRoll =
                glm::radians(m_player.Config().leanAngleDegrees) * watched->leanAmount;
            view = spectated.ViewMatrix();
            viewPosition = spectated.eyePosition;
        }
        else
        {
        {
            // The weapon's shove, on the camera and on nothing else.
            //
            // It never touches the look angles, so it cannot move where the player is aiming and
            // cannot fight them for the mouse: rounds still go exactly where the crosshair was. It
            // is the jolt a shot gives the head, and it is over in a fraction of a second.
            PlayerView shaken = m_player.View();
            const float shake = std::clamp(cv_cameraShake.Get(), 0.0f, 1.0f);
            shaken.pitch += glm::radians(m_weapon.shakePitch) * shake;
            shaken.yaw += glm::radians(m_weapon.shakeYaw) * shake;
            // Held: jolted about as it carries you, and jerked with every struggle.
            if (m_wasHeld)
            {
                const float t = static_cast<float>(m_time);
                const float carried = 0.6f + 0.4f * std::sin(t * 2.1f);
                shaken.pitch += glm::radians(1.6f * std::sin(t * 11.0f) * carried + 5.0f * m_struggleKick) * shake;
                shaken.yaw += glm::radians(1.2f * std::sin(t * 7.3f + 1.0f) * carried) * shake;
                shaken.leanRoll += glm::radians(4.0f * std::sin(t * 3.1f) + 9.0f * m_struggleKick * m_struggleSide) * shake;
                shaken.eyePosition.y += 0.03f * std::sin(t * 13.0f) * carried * shake;
            }
            // The step's dip and a landing's drop are taken back out of the picture as far as the
            // player asked -- the picture only. The body still anchors itself to the real eye, which is
            // what lowers the hips far enough for a leg to reach its next step.
            const float bob = std::clamp(cv_headBob.Get(), 0.0f, 1.0f);
            shaken.eyePosition.y -= shaken.bobOffset * (1.0f - bob) + shaken.landingDip * (1.0f - shake);
            view = shaken.ViewMatrix();
        }
            viewPosition = m_player.View().eyePosition;
        }
        break;
    }
    }

    // A cinematic playing takes the picture, and hands it back to whichever of those it would have been.
    CinematicCamera(view, viewPosition, deltaSeconds, cv_fov.Get());

    // Where the picture is actually taken from this frame -- the eye, the free camera, or, dead, the
    // teammate being watched. Everything drawn from the camera reads this rather than working it out
    // again: the shadow maps used to be fitted round the local player's own eye, so spectating somebody
    // across the map left them standing in a place with no shadows at all.
    m_renderEye = viewPosition;
    // Out of the view itself, whichever camera made it. The ears were on the fly camera, which in first
    // person is left wherever it was last used -- at the spawn, facing north -- so every sound was heard
    // from there: from one side, whichever way the player turned.
    m_renderForward = -glm::vec3(glm::inverse(view)[2]);

    // What is in the player's hands has to be settled before the body is posed, not after. Deciding
    // it afterwards left a newly equipped weapon sitting at the world origin for one frame, which
    // reads as the gun flying in from the middle of the map.
    //
    // The selected slot decides what is held, so this is checked every frame rather than hooked
    // onto each of the several places a slot can change. A climb is one of those places: it puts
    // whatever was out away and takes it back at the top, which has to happen before the sync sees
    // the slot rather than a frame after it.
    UpdateMantleStow();
    SyncEquippedWeapon();
    UpdateItemUse(m_lastFrameSeconds);
    m_world.SetHaveKeycard(m_heldItem != kInvalidItem && m_heldItem == m_items.IdOf("keycard"));
    // Inside a locker there is nowhere to hold a rifle: it is stowed rather than drawn, because a
    // metre of barrel held in front of the chest goes straight through the door.
    const WeaponDefinition* weapon = m_hidingSpot >= 0 ? nullptr : EquippedWeapon();
    m_body.SetWeapon(m_scene, app.GetMeshes(), weapon);
    if (weapon != nullptr)
    {
        PlayerBody::WeaponPose pose;
        pose.aim = m_weapon.aim;
        pose.reloading = m_weapon.IsReloading();
        pose.reloadEmpty = m_weapon.reloadFromEmpty;
        pose.emptyHold = !m_weapon.IsReloading() && m_weapon.rounds <= 0 && EquippedWeapon() != nullptr;
        // Runs 0 at the start of the reload to 1 at the end, so the animation does not have to know
        // how long any particular weapon takes.
        pose.reload = pose.reloading ? ReloadProgress() : -1.0f;
        pose.kick = m_weaponKick;
        pose.draw = m_weaponDraw;
        pose.holster = m_weaponHolster;
        m_body.SetWeaponPose(pose);
    }

    // The kick is presentation, so it decays per frame rather than per tick. Firing sets it to one
    // in ResolveShots, which is the only place that knows a round actually left the barrel.
    m_weaponKick = std::max(m_weaponKick - deltaSeconds * 7.0f, 0.0f);
    // Drawing a weapon runs 0 to 1 over its own moment, so swapping is a movement rather than a
    // substitution.
    m_weaponDraw = std::min(m_weaponDraw + deltaSeconds * 3.2f, 1.0f);
    m_lastFrameSeconds = deltaSeconds;
    if (m_holdReportIn > 0 && --m_holdReportIn == 0)
    {
        ReportHold();
    }

    // The body follows the simulation every frame. Its head is only drawn from the fly camera,
    // because in first person the camera sits inside it -- and not while a cinematic has the picture, or is still
    // bringing it back to the eyes: the body was seen from outside, headless, until the camera got there.
    const bool cameraAway = m_cine.Active() || m_cineHandBack > m_cineHandBackTotal * 0.1f;
    m_body.Tuning().hideHead = m_cameraMode == CameraMode::FirstPerson && m_player.State().alive && !cameraAway;

    // Death hands the body over to the ragdoll, once. Everything below it is animation, and that is
    // exactly what stops.
    if (!m_player.State().alive && !m_localCollapsed)
    {
        m_body.Collapse(m_deathImpulse);
        m_localCollapsed = true;
        // And the bag goes on the floor. Once, on the frame the player goes down, which is what the
        // collapse latch is already for.
        DropEverything();
    }
    else if (m_player.State().alive && m_localCollapsed)
    {
        LeaveCorpse(m_body, LocalPlayerId());
        m_body.Revive();
        m_localCollapsed = false;
    }

    m_body.Update(m_scene, m_player.State(), m_player.View(), m_player.Config(), app.GetPhysics(),
                  deltaSeconds);
    m_bodyPosedAt = m_player.View().renderPosition;

    UpdateSpectating();

    // Everyone else, driven the same way from replicated state.
    if (m_sessionMode != SessionMode::Offline)
    {
        m_client.UpdateInterpolation(deltaSeconds);
        SyncRemoteAvatars(deltaSeconds);
    }
    // And the loose items, which the host owns and everyone else follows.
    if (m_sessionMode == SessionMode::Client)
    {
        m_world.FollowNetworkState(m_app->GetPhysics(), deltaSeconds);
    }

    // A join finishes when the host answers and has started the game. Until then the guest waits in
    // the lobby, and goes in with everybody else the moment the host presses Start.
    if (m_screen == Screen::Title && m_sessionMode == SessionMode::Client && m_client.Connected() &&
        m_client.HostStarted())
    {
        EnterWorld();
        // And only now ask what has already happened. The world this answer describes exists as of
        // this line; asked any earlier, the answer arrives before there is anything to apply it to
        // and the build that follows wipes it.
        m_client.SendReady();
    }

    const float aspect = renderer.Height() > 0
                             ? static_cast<float>(renderer.Width()) / static_cast<float>(renderer.Height())
                             : 16.0f / 9.0f;
    // A cinematic may want its own lens and to see much further: a planet from orbit, a ship coming in from far off.
    const float horizontal = glm::radians(m_cineFov > 0.0f ? m_cineFov : cv_fov.Get());
    const float verticalFov = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);
    // On the stage, where the ship is only ever filmed from metres off, the near plane further out: the depth buffer then
    // has far more to give the far end, and the ship's plates do not fight each other a few hundred metres away.
    const bool onStage = glm::distance(m_renderEye, ShipSpec::kStage) < ShipSpec::kStageReach;
    const bool titleShot = m_screen == Screen::Title;
    const float nearPlane = titleShot ? 2.0f : m_cineFar > 0.0f ? (onStage ? 2.0f : 0.25f) : 0.05f;
    // Out to the horizon at a site (SiteTerrain::kFar and a little more).
    const float farPlane = titleShot ? 3000.0f : m_cineFar > 0.0f ? m_cineFar : 4500.0f;
    const glm::mat4 projection = Depth::Perspective(verticalFov, aspect, nearPlane, farPlane);
    renderer.SetCamera(view, projection);
    m_renderVerticalFov = verticalFov;
    m_renderAspect = aspect;
    // Kept for the HUD, which puts some of what it says on things in the world.
    m_viewProjection = projection * view;

    // Where the eye actually is and which way it faces, whichever camera is driving.
    //
    // m_camera is the free-flying inspection camera and is not where the player is looking unless
    // that is the mode. Taking both out of the view matrix instead works for first person, third
    // person, the free camera and spectating somebody else, because it is the matrix all four of
    // them produced.
    const glm::vec3 eyePosition = viewPosition;
    const glm::vec3 eyeForward = -glm::vec3(view[0][2], view[1][2], view[2][2]);

    // --- World ------------------------------------------------------------------------------------
    Environment& environment = m_scene.GetEnvironment();
    environment.skySunDirection = glm::vec3(0.0f);
    environment.skySun = 1.0f;
    environment.skyAir = 0.0f;
    environment.skyHaze = 0.0f;
    environment.sunDisc = 0.0047f;
    environment.airHaze = 0.03f;
    environment.cloudCover = 0.0f;
    environment.cloudDark = 0.0f;
    environment.lightning = 0.0f;
    environment.bolt = glm::vec4(0.0f);
    environment.fogStart = cv_fogStart.Get();
    environment.fogEnd = cv_fogEnd.Get();
    environment.sunIntensity = cv_sunIntensity.Get();
    // Out at the site it is night: its own sky, fog and light, while the picture is taken from there.
    {
        const bool atSite = m_screen == Screen::Playing && m_facility.Built() && m_facility.Contains(m_renderEye);
        // In the open air or under a roof (a building, the shuttle's cabin): how the fog falls on what is seen (Environment).
        // Eased over about half a second, so walking through a door the air thins rather than vanishing; a cut (the camera
        // somewhere else entirely) takes it at once.
        {
            const float outdoors = atSite && (m_facility.Plan().Indoors(m_renderEye) || m_facility.Shuttle().Aboard(m_renderEye)) ? 0.0f : 1.0f;
            const bool cut = glm::distance(m_renderEye, m_eyeOutdoorsAt) > 3.0f;
            m_eyeOutdoors = cut ? outdoors : m_eyeOutdoors + (outdoors - m_eyeOutdoors) * std::min(m_lastFrameSeconds * 5.0f, 1.0f);
            m_eyeOutdoorsAt = m_renderEye;
            environment.eyeOutdoors = m_eyeOutdoors;
        }
        // The title is in space too.
        const bool title = m_screen == Screen::Title;
        const bool inShip = !atSite && (title || m_ship.Contains(m_renderEye));
        // One ship in the picture, the one it is taken of.
        m_ship.ShowFor(m_scene, m_renderEye);
        environment.stars = 0.0f;
        environment.planetRadius = 0.0f;
        environment.ownRings = glm::vec4(0.0f);
        environment.planetAirWarm = 0.0f;
        // The system's own nebula over its stars, from its number: one of a few kinds -- the red of glowing hydrogen, the
        // blue of dust lit by stars, a green-teal, a dusky violet -- and in some systems none worth seeing.
        {
            const uint64_t which = m_campaignOpen ? m_campaign.system : 0x5EEDull;
            uint32_t h = static_cast<uint32_t>(which ^ (which >> 32)) * 0x9E3779B1u;
            h ^= h >> 15;
            h *= 0x85EBCA6Bu;
            h ^= h >> 13;
            static const glm::vec3 kNebulae[] = {{0.9f, 0.32f, 0.38f}, {0.35f, 0.5f, 0.95f}, {0.3f, 0.8f, 0.7f}, {0.65f, 0.4f, 0.85f}};
            environment.nebulaColor = kNebulae[h % 4u];
            environment.nebula = (h >> 8) % 5u == 0u ? 0.0f : 0.5f + 0.5f * static_cast<float>((h >> 12) & 0xFFu) / 255.0f;
            environment.nebulaSeed = static_cast<float>((h >> 20) & 0x3FFu) * 0.37f;
        }
        if (inShip)
        {
            // Space: black, the stars, a hard sun low off the starboard quarter, and -- over a site -- its planet below
            // ahead, the colour of its ice. The dark inside is the lamps' to light.
            const glm::vec3 towardsSun = glm::normalize(glm::vec3(0.8f, 0.18f, 0.55f));
            environment.sunDirection = -towardsSun;
            environment.sunColor = {1.0f, 0.95f, 0.88f};
            environment.sunIntensity = 1.7f;
            environment.ambientSky = {0.05f, 0.054f, 0.066f};
            environment.ambientGround = {0.032f, 0.032f, 0.036f};
            environment.fogColor = {0.0f, 0.0f, 0.0f};
            environment.fogStart = 5000.0f;
            environment.fogEnd = 20000.0f;
            environment.stars = 1.0f;
            // Over a site, its planet; under way to one, it comes up ahead over the second half of the journey, from a
            // point to as big as it is from orbit.
            const float journey = m_shipTravelTotal > 0.0f ? 1.0f - m_shipTravel / m_shipTravelTotal : 1.0f;
            const float nearing = m_shipTravel > 0.0f ? glm::smoothstep(0.45f, 1.0f, journey) : 1.0f;
            const uint32_t planetSeed = m_shipTravel > 0.0f ? m_facility.Seed() : m_shipOrbiting;
            if (planetSeed != 0 && nearing > 0.0f)
            {
                const uint32_t seed = planetSeed;
                const float tint = static_cast<float>((seed * 2654435761u) >> 24) / 255.0f;
                // Under way, dead ahead of the bow, which is pointed at it; in orbit, below ahead.
                environment.planetDirection = m_shipTravel > 0.0f ? glm::normalize(ShipSpec::kTravelHeading)
                                                                  : glm::normalize(glm::vec3(0.12f, -0.42f, -0.9f));
                environment.planetRadius = glm::mix(0.01f, 0.6f, nearing * nearing);
                environment.planet = PlanetLook::Plain(glm::mix(glm::vec3(0.46f, 0.52f, 0.6f), glm::vec3(0.58f, 0.6f, 0.62f), tint), 0.7f);
            }
            for (glm::vec4& body : environment.skyBodies)
            {
                body = glm::vec4(0.0f);
            }
            if (m_campaignOpen && !title)
            {
                SetSpaceSky(environment);
                if (GroundCinematic())
                {
                    // Lifting off or setting down, the world's sky as it is from where the camera is: the sky's own from the
                    // ground, giving way to space only as the camera itself climbs (one watching from the pad keeps the
                    // day's sky, the ship going up into it) -- never one turning into the other in a frame.
                    const SkyState space = SkyState::Of(environment);
                    const std::vector<SpaceBody> spaceBodies = m_spaceBodies;
                    SetGroundSky(environment);
                    const float ground = 1.0f - glm::smoothstep(250.0f, 2500.0f, eyePosition.y - (ShipSpec::kStage.y + ShipSpec::kFieldGround));
                    SkyState::Of(environment).Toward(space, 1.0f - ground).Into(environment);
                    // The worlds out in space, once it is more space than sky.
                    if (ground < 0.5f)
                    {
                        m_spaceBodies = spaceBodies;
                    }
                }
                else if (ShipLanded())
                {
                    SetGroundSky(environment);
                }
            }
            if (title)
            {
                // The title's planet: an ice world below the ship, the way the camera looks, its day side towards the
                // sun off to the right and night coming across it -- its ground and cloud there to be seen.
                const glm::vec3 ahead{-std::sin(kTitleAngle), 0.0f, -std::cos(kTitleAngle)};
                const glm::vec3 right{std::cos(kTitleAngle), 0.0f, -std::sin(kTitleAngle)};
                const glm::vec3 titleSun = glm::normalize(right * 0.9f + glm::vec3(0.0f, 0.3f, 0.0f) + ahead * 0.55f);
                environment.sunDirection = -titleSun;
                environment.sunIntensity = 2.2f;
                environment.planetDirection = glm::normalize(ahead * 0.55f + glm::vec3(0.0f, -0.83f, 0.0f));
                environment.planetRadius = 0.92f;
                environment.planet = PlanetLook::Plain({0.6f, 0.67f, 0.76f}, 0.9f);
                environment.planet.groundA = {0.74f, 0.79f, 0.86f};
                environment.planet.groundB = {0.42f, 0.48f, 0.57f};
                environment.planet.ice = 0.4f;
                environment.planet.clouds = 0.35f;
                environment.planetAirWarm = 0.0f;
            }
        }
        else if (m_skyInShip && !atSite)
        {
            const Environment usual;
            environment.sunDirection = usual.sunDirection;
            environment.sunColor = usual.sunColor;
            environment.ambientSky = usual.ambientSky;
            environment.ambientGround = usual.ambientGround;
            environment.fogColor = usual.fogColor;
        }
        m_skyInShip = inShip;
        const StarSystem* siteSystem = atSite && m_campaignOpen ? CurrentSystem() : nullptr;
        const Body* siteBody = siteSystem != nullptr ? siteSystem->Find(m_campaign.body) : nullptr;
        if (siteBody != nullptr && m_campaign.travel.region >= 0 && m_campaign.travel.region < static_cast<int>(siteBody->regions.size()))
        {
            // In a campaign, the sky of the world it is on, at the hour it is there -- and the land round the site seen out to the
            // horizon on a clear day (SiteTerrain::FarMesh), hazier the thicker its air and its weather; no haze at all without air.
            SetSkyOver(*siteSystem, *siteBody, m_campaign.travel.region, environment);
            if (siteBody->air >= 0.015f)
            {
                float visibility = 1.0f;
                if (const WeatherDef* weather = m_universeData.Weather(siteBody->weather))
                {
                    visibility *= weather->visibility;
                }
                if (const AtmosphereDef* atmosphere = m_universeData.Atmosphere(siteBody->atmosphere))
                {
                    visibility *= atmosphere->visibility;
                }
                environment.fogStart = 60.0f * visibility;
                environment.fogEnd = std::clamp(2400.0f * visibility, 700.0f, 3000.0f);
            }
            // And the weather thickens it far more: a sandstorm or a blizzard down to a few tens of metres, the fog the
            // colour of what is in the air, the day dimmed under it, and how far anybody can see coming and going with the
            // gusts.
            const SiteMap::Look& look = m_facility.GetLook();
            const Weather::Kind kind = Weather::KindOf(look.weather);
            if (kind != Weather::Kind::None && look.weatherAmount > 0.0f && siteBody->air >= 0.015f)
            {
                const float amount = std::clamp(look.weatherAmount, 0.0f, 1.0f);
                const float thickens = kind == Weather::Kind::Dust ? 0.85f : kind == Weather::Kind::Snow ? 0.8f : kind == Weather::Kind::Ash ? 0.75f : 0.55f;
                const float gust = std::clamp((Weather::GustAt(static_cast<float>(std::fmod(m_campaign.clock, 10000.0))) - 0.4f) / 1.2f, 0.0f, 1.0f);
                const float sight = (1.0f - amount * thickens) * glm::mix(1.15f, 0.85f, gust);
                environment.fogEnd = std::max(environment.fogEnd * std::pow(std::max(sight, 0.0f), 1.5f), 25.0f);
                environment.fogStart = std::min(environment.fogStart, environment.fogEnd * 0.08f);
                const glm::vec3 lit = environment.ambientSky + environment.sunColor * environment.sunIntensity * 0.25f;
                const glm::vec3 stuff = kind == Weather::Kind::Dust   ? look.ground * glm::vec3(0.9f, 0.82f, 0.7f)
                                        : kind == Weather::Kind::Snow ? glm::vec3(0.8f, 0.83f, 0.87f)
                                        : kind == Weather::Kind::Ash  ? glm::vec3(0.2f, 0.19f, 0.19f)
                                                                      : glm::vec3(0.5f, 0.54f, 0.58f);
                const float thick = amount * thickens;
                environment.fogColor = glm::mix(environment.fogColor, stuff * lit, thick);
                environment.sunIntensity *= glm::mix(1.0f, 0.4f, thick);
                environment.skyHaze = thick;
            }
            // Clouds, by the weather: a few fair-weather ones on a clear day (more or fewer by the world), a grey lid in fog,
            // rain or snow, low and dark in a storm, ash's heavy and brown-black; little over a dust storm, whose haze is the
            // sky. Fewer the thinner the air, none without it.
            {
                const float amount = std::clamp(look.weatherAmount, 0.0f, 1.0f);
                const float fair = 0.2f + 0.3f * static_cast<float>((m_facility.Seed() * 2246822519u) >> 24) / 255.0f;
                float cover = siteBody->weather == "fog" ? 0.9f : fair;
                float dark = siteBody->weather == "fog" ? 0.35f : 0.0f;
                if (kind == Weather::Kind::Rain)
                {
                    cover = 0.8f + 0.2f * glm::smoothstep(0.3f, 0.8f, amount);
                    dark = 0.3f + 0.65f * amount;
                }
                else if (kind == Weather::Kind::Snow)
                {
                    cover = 0.85f + 0.15f * amount;
                    dark = 0.2f + 0.3f * amount;
                }
                else if (kind == Weather::Kind::Ash)
                {
                    cover = 0.95f;
                    dark = 0.8f;
                }
                else if (kind == Weather::Kind::Dust)
                {
                    cover = 0.15f;
                    dark = 0.3f;
                }
                const float air = glm::smoothstep(0.015f, 0.3f, siteBody->air);
                // Gathering as a spell of it comes on and breaking up as it eases: between the fair-weather sky and its own.
                const float gathered = kind == Weather::Kind::None ? 1.0f : glm::smoothstep(0.0f, 0.3f, amount);
                const float usual = siteBody->weather == "fog" ? 0.35f : 0.0f;
                environment.cloudCover = glm::mix(fair, cover, gathered) * air;
                environment.cloudDark = glm::mix(usual, dark, gathered);
                // And under a heavy, dark sky the whole of the day is dimmer and greyer -- the light from above, the sun through
                // it, the air and the haze over the horizon -- not a white sky over a storm.
                const float gloom = environment.cloudCover * environment.cloudDark;
                environment.ambientSky *= glm::mix(1.0f, 0.42f, gloom);
                environment.ambientGround *= glm::mix(1.0f, 0.5f, gloom);
                environment.sunIntensity *= glm::mix(1.0f, 0.2f, gloom * environment.cloudCover);
                const float grey = glm::dot(environment.fogColor, glm::vec3(0.2126f, 0.7152f, 0.0722f));
                environment.fogColor = glm::mix(environment.fogColor, glm::vec3(grey), 0.6f * gloom) * glm::mix(1.0f, 0.45f, gloom);
                environment.skyHaze = std::max(environment.skyHaze, 0.7f * gloom);
            }
            // And lightning: the world under the open sky lit by it, cold and white, for the moment it lasts -- what is under a
            // roof only through its doors and windows (the ambient is the sky's, which a roof keeps out) -- and the sky with it.
            // (Environment::lightning: the renderer adds it to the light from the sky, and the sky draws its own.)
            environment.lightning = m_lightningFlash;
            environment.bolt = m_lightningBolt;
        }
        else if (atSite)
        {
            const SitePlan::Sky& sky = m_facility.Plan().sky;
            environment.sunDirection = sky.sunDirection;
            environment.sunColor = sky.sunColor;
            environment.sunIntensity = sky.sunIntensity;
            environment.ambientSky = sky.ambientSky;
            environment.ambientGround = sky.ambientGround;
            environment.fogColor = sky.fogColor;
            environment.fogStart = sky.fogStart;
            environment.fogEnd = sky.fogEnd;
        }
        else if (m_skyAtSite)
        {
            const Environment usual;
            environment.sunDirection = usual.sunDirection;
            environment.sunColor = usual.sunColor;
            environment.ambientSky = usual.ambientSky;
            environment.ambientGround = usual.ambientGround;
            environment.fogColor = usual.fogColor;
        }
        m_skyAtSite = atSite;
        // A cinematic can see further through the fog, and lift the dark a little at the site, for a shot of the place.
        if (m_cine.Active())
        {
            const CinematicSampler sampler = m_cine.Sampler();
            const float fog = sampler.FogScale(m_cine.Time());
            environment.fogStart *= fog;
            environment.fogEnd *= fog;
            if (atSite)
            {
                const float light = sampler.AmbientScale(m_cine.Time());
                environment.ambientSky *= light;
                environment.ambientGround *= light;
                environment.sunIntensity *= light;
            }
        }
    }
    // How hazy the air is where the eye is, for light to show in (Environment::airHaze): as thick as the fog out at a site --
    // a sandstorm's, a fog's -- and some dust in the air of its buildings; the ship's and the outposts' clean.
    if (m_screen == Screen::Playing && m_facility.Built() && m_facility.Contains(m_renderEye))
    {
        environment.airHaze = glm::mix(0.1f, std::clamp(28.0f / std::max(environment.fogEnd, 1.0f), 0.04f, 1.5f), std::clamp(environment.eyeOutdoors, 0.0f, 1.0f));
    }
    // Under water: the eye below the surface of a site's water. Murk all round in the water's own colour, lit from above,
    // a few metres to see by (further the clearer it is), nothing of the sky -- and everything heard dull through it.
    m_underwater = 0.0f;
    if (m_screen == Screen::Playing && m_swimWaterSet && m_swimWater.depth(m_renderEye.x, m_renderEye.z) > 0.0f)
    {
        m_underwater = std::clamp((m_swimWater.SurfaceAt(m_renderEye.x, m_renderEye.z) - m_renderEye.y) / 0.06f, 0.0f, 1.0f);
    }
    if (m_underwater > 0.0f)
    {
        glm::vec3 tint;
        float clarity = 1.0f;
        SiteWaterLook(tint, clarity);
        const glm::vec3 lit = environment.ambientSky + environment.sunColor * environment.sunIntensity * std::max(environment.sunDirection.y * -1.0f, 0.1f) * 0.4f;
        // (Its own deep colour and some of the sky's, as it looks from above: a lake's own colour alone was green under a blue
        // surface.)
        const glm::vec3 murk = glm::mix(tint * 5.0f * lit, environment.fogColor * 0.45f, 0.45f);
        environment.fogColor = glm::mix(environment.fogColor, murk, m_underwater);
        environment.fogStart = glm::mix(environment.fogStart, 0.0f, m_underwater);
        environment.fogEnd = glm::mix(environment.fogEnd, 5.0f + clarity * 6.0f, m_underwater);
        environment.skyHaze = std::max(environment.skyHaze, m_underwater);
        environment.airHaze = std::max(environment.airHaze, 0.8f * m_underwater);
        // The sun reaches down dimmed, the deeper the less, scattered on the way: its shadows faint down there, not as sharp
        // as in air. The light is mostly the water's own glow from above.
        const float below = std::max(m_swimWater.SurfaceAt(m_renderEye.x, m_renderEye.z) - m_renderEye.y, 0.0f);
        environment.sunIntensity *= glm::mix(1.0f, 0.3f * std::exp(-below / std::max(clarity * 2.0f, 0.5f)), m_underwater);
        environment.ambientSky = glm::mix(environment.ambientSky, environment.ambientSky * 0.6f + murk * 0.8f, m_underwater);
    }
    app.GetAudio().SetSubmerged(m_underwater);
    environment.exposure = cv_exposure.Get();
    environment.contrast = cv_contrast.Get();
    // The prints in soft ground near the eye, for the ground to be drawn pressed in by them.
    m_footprints.Gather(environment, eyePosition, m_campaign.clock);

    // The finishing pass. Fear: something close and after you, or with hold of you, closes the edges
    // of the picture in and drains it, and it lets go slowly.
    {
        PostProcess::Settings& post = app.GetPostProcess().GetSettings();
        post.enabled = cv_post.Get();
        post.ambientOcclusion = std::clamp(cv_ambientOcclusion.Get(), 0.0f, 1.0f);
        post.fogStart = m_scene.GetEnvironment().fogStart;
        post.fogEnd = m_scene.GetEnvironment().fogEnd;
        post.projection = projection;
        // Where the sun is in the picture, for its flare: its direction, a point at no distance at all, through the camera.
        {
            const glm::vec4 clip = projection * view * glm::vec4(-m_scene.GetEnvironment().sunDirection, 0.0f);
            post.sunAhead = clip.w > 1.0e-4f ? glm::vec3(clip.x / clip.w, clip.y / clip.w, 1.0f) : glm::vec3(0.0f);
            post.lensFlare = std::max(cv_lensFlare.Get(), 0.0f);
        }
        // Editing a cinematic, the picture is in the editor's preview: the whole of it, smaller.
        glm::vec2 previewMin;
        glm::vec2 previewMax;
        if (m_cineEditor.Preview(previewMin, previewMax))
        {
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            post.outX = static_cast<int>(previewMin.x * scale.x);
            post.outY = static_cast<int>(previewMin.y * scale.y);
            post.outWidth = static_cast<int>((previewMax.x - previewMin.x) * scale.x);
            post.outHeight = static_cast<int>((previewMax.y - previewMin.y) * scale.y);
        }
        else
        {
            post.outWidth = post.outHeight = 0;
        }
        post.exposure = environment.exposure;
        post.contrast = environment.contrast;
        post.bloom = std::clamp(cv_bloom.Get(), 0.0f, 3.0f);
        post.grain = cv_grain.Get() ? 0.035f : 0.0f;
        post.vignette = std::clamp(cv_vignette.Get(), 0.0f, 0.9f);
        post.adapt = std::clamp(cv_eyeAdapt.Get(), 0.0f, 1.0f);
        post.adaptReference = std::max(cv_eyeReference.Get(), 1.0e-4f);
        post.deltaSeconds = m_lastFrameSeconds;
        float fear = 0.0f;
        if (m_player.State().alive && m_screen == Screen::Playing)
        {
            const glm::vec3 me = m_player.State().position;
            for (const std::unique_ptr<Creature>& creature : m_creatures)
            {
                if (!creature->Alive() || creature->Down())
                {
                    continue;
                }
                const Behavior doing = creature->Doing();
                const bool after = doing == Behavior::Hunt || doing == Behavior::Attack || doing == Behavior::Drag ||
                                   doing == Behavior::Stalk || doing == Behavior::Search;
                const float near = std::clamp(1.0f - glm::distance(creature->Position(), me) / 14.0f, 0.0f, 1.0f);
                fear = std::max(fear, near * (after ? 1.0f : 0.45f));
            }
            if (m_grips.count(LocalPlayerId()) != 0 || (m_sessionMode == SessionMode::Client && m_client.HeldByHost()))
            {
                fear = 1.0f;
            }
            fear = std::max(fear, std::clamp(1.0f - m_player.State().health / 40.0f, 0.0f, 1.0f) * 0.6f);
        }
        const float rate = fear > m_fearShown ? 3.0f : 0.4f;
        m_fearShown += (fear - m_fearShown) * (1.0f - std::exp(-rate * m_lastFrameSeconds));
        post.fear = m_fearShown;
        post.flash = m_grabFlash;
    }

    // The flashlight, carried at the shoulder rather than screwed to the eye.
    //
    // A torch held in the hand is the honest version and it is also the one that fights everything
    // else: the hand moves with the weapon, the weapon moves with the wall corrections, and the beam
    // would swing around the room every time the gun was nudged. Carried near the eye it points
    // roughly where the player is looking, which is what anybody wants from a torch.
    //
    // "Roughly", because it does not point exactly there. A torch is a thing with weight held in a
    // hand on the end of an arm, and none of that turns at the speed of a wrist on a mouse. The beam
    // trails the view and catches up, which is most of what makes a light read as carried rather
    // than as a property of the camera -- and in a dark room it is the difference between a beam
    // that sweeps a wall and a spot that is simply always in the middle of the screen.
    //
    // Offset down and to the side so the beam has some parallax against the walls: exactly on the
    // eye, every surface is lit face-on and the room reads flat.
    {
        PunctualLight& torch = environment.lights[0];
        // Not while dead, and not while looking out of somebody else's head.
        //
        // Spectating puts the camera at the spectated player's eye, and this light is placed from
        // that eye -- so a dead player's own torch was being hung on the person they were watching,
        // on top of that person's torch, at the same place and pointing the same way. Two lights in
        // one spot is twice the brightness, which is what "when you die it stacks your flashlight
        // when spectating" was. A corpse does not hold a torch.
        // Nor while a cinematic has the camera: hung on its eye, the torch lit the shot.
        const bool torchLit = m_torchOn && m_screen == Screen::Playing &&
                              m_player.State().alive && m_spectating < 0 && !m_cine.Active();
        if (DroneLamp(torch))
        {
            // Dead and driving a drone: the lamp on its mast, in the torch's place.
            m_torchAimed = false;
        }
        else if (torchLit)
        {
            // Held by the player, not by the camera: in third person the camera is behind them, and a torch hung on it lit
            // their own back white and everything round them.
            glm::vec3 torchEye = eyePosition;
            glm::vec3 torchForward = eyeForward;
            if (m_cameraMode == CameraMode::ThirdPerson)
            {
                const float pitch = m_lookPitch;
                torchEye = m_body.GetPose().GlobalPosition(m_body.Rig().head);
                torchForward = {std::sin(m_lookYaw) * std::cos(pitch), std::sin(pitch), -std::cos(m_lookYaw) * std::cos(pitch)};
            }
            // Straight up or straight down leaves no sideways direction to offset along, and
            // normalising that zero vector would put the torch at NaN and take the whole frame's
            // shading with it. The offset simply goes away at the poles, which is where it matters
            // least: the beam is on the floor or the ceiling and has no wall to rake across.
            const glm::vec3 across = glm::cross(torchForward, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 right =
                glm::length(across) > 1e-3f ? glm::normalize(across) : glm::vec3(0.0f);

            // Snapped into place the first frame it comes on, then eased. Easing from wherever the
            // beam was last pointing means switching the torch on after a turn sweeps it across the
            // room to catch up, which looks like a fault rather than like weight.
            // Aimed in at what is in the middle of the view, from where it is held to the side: so the spot is round the
            // crosshair on whatever wall it is looking at, not always a hand's width off it.
            const glm::vec3 lens = torchEye + torchForward * cv_torchReach.Get() + right * 0.10f - glm::vec3(0.0f, 0.10f, 0.0f);
            float ahead = cv_torchRange.Get();
            if (const RayHit hit = m_app->GetPhysics().RayCastStatic(torchEye, torchForward, ahead))
            {
                ahead = std::max(hit.distance, 1.5f);
            }
            const glm::vec3 toward = torchEye + torchForward * ahead - lens;
            const glm::vec3 wanted = glm::length(toward) > 1e-3f ? glm::normalize(toward) : torchForward;
            if (!m_torchAimed)
            {
                m_torchAim = wanted;
                m_torchAimed = true;
            }
            else
            {
                const float follow = 1.0f - std::exp(-cv_torchFollow.Get() * deltaSeconds);
                m_torchAim = glm::normalize(glm::mix(m_torchAim, wanted, follow) + glm::vec3(1e-5f));
            }

            // Ahead of the weapon, not level with the eye.
            //
            // A light beside the eye has the player's own rifle a hand span in front of its lens and
            // square in the middle of its beam, and an inverse square at that range blows the weapon
            // to white however the falloff is softened. Real weapon lights are clamped near the
            // muzzle for exactly this reason: put the source past the thing you are holding and the
            // thing you are holding stops being the brightest object in the room.
            torch.position = lens;
            torch.direction = m_torchAim;
            torch.color = glm::vec3(1.0f, 0.97f, 0.88f);
            torch.intensity = cv_torchIntensity.Get() * TorchStrength();
            torch.range = cv_torchRange.Get();
            torch.innerAngle = cv_torchInner.Get();
            torch.outerAngle = cv_torchOuter.Get();
        }
        else
        {
            torch.intensity = 0.0f;
            // So it snaps to the view next time rather than easing over from where it was left.
            m_torchAimed = false;
        }
        torch.sourceRadius = cv_torchSourceRadius.Get();
    }

    // And the flash throws light, which is most of what sells it.
    //
    // A muzzle flash that does not light anything is a picture stuck on the end of the barrel: the
    // room stays exactly as dark as it was while a fire the size of a fist burns in the middle of
    // it. One frame of a wall, a hand and the ceiling lit from below is worth more than any amount
    // of work on the shape, and in a dark corridor it is the only thing that tells you where you
    // are.
    //
    // A bulb rather than a cone -- an explosion at the crown throws light everywhere, including back
    // down the weapon and onto the player's own arms. Short range, because it is small and brief;
    // very bright, because for the moment it exists it is the brightest thing in the level.
    {
        PunctualLight& flash = environment.lights[1];
        // Only while there is somebody alive holding a weapon to flash. A player killed in the moment
        // after firing left the flash at whatever brightness it had reached, and it stayed there for
        // good -- a light burning on the floor where the weapon fell.
        const float strength = m_body.MuzzleFlashStrength();
        if (strength > 0.001f && m_screen == Screen::Playing && m_player.State().alive && m_body.HasWeapon())
        {
            flash.position = m_body.MuzzlePoint();
            flash.direction = glm::vec3(0.0f, -1.0f, 0.0f);
            flash.color = glm::vec3(1.0f, 0.86f, 0.62f);
            flash.intensity = cv_flashIntensity.Get() * strength;
            flash.range = cv_flashRange.Get();
            flash.innerAngle = 180.0f;
            flash.outerAngle = 180.0f;
            flash.sourceRadius = 0.35f;
        }
        else
        {
            flash.intensity = 0.0f;
        }
    }

    // And everybody else's torches and muzzle flashes, into whatever slots are left.
    //
    // Without this, a light is visible only to the player carrying it. Two people standing in the
    // same dark room saw two different rooms: each one's own torch lit the walls for them and for
    // nobody else, and a shot fired beside you lit nothing. That is the largest way two machines
    // disagreed about what the world looks like, and none of it was the renderer -- the lights were
    // simply never in anybody else's scene.
    //
    // There are four slots and up to seven things that want one, so they compete. The score is what
    // a light actually contributes at the eye -- its brightness over the square of how far away it
    // is -- which puts your own torch first by a mile, because it is at the eye, and picks the
    // nearest of everybody else's after that. Cutting the faintest is the one choice that cannot be
    // noticed.
    {
        struct Candidate
        {
            PunctualLight light;
            float score = 0.0f;
        };
        std::vector<Candidate> candidates;
        const auto consider = [&](const PunctualLight& light)
        {
            if (light.intensity <= 0.0f || light.range <= 0.0f)
            {
                return;
            }
            const float distance = glm::length(light.position - eyePosition);
            candidates.push_back({light, light.intensity / std::max(distance * distance, 0.05f)});
        };

        // The two already filled in above keep their meaning; they are re-entered here so they
        // compete on the same terms as everyone else's rather than being special.
        consider(environment.lights[0]);
        consider(environment.lights[1]);

        for (const std::unique_ptr<RemoteAvatar>& avatar : m_avatars)
        {
            if (avatar == nullptr || !avatar->built || !avatar->state.alive)
            {
                continue;
            }
            if (avatar->torchOn && !m_cine.Active())
            {
                // From their eye, pointing where they are looking. Not eased the way the local one
                // is: that easing is about the weight of a thing held in your own hand, and from
                // outside it is a beam that already lags behind their head by a network round trip.
                const glm::vec3 forward = avatar->view.Forward();
                PunctualLight torch;
                torch.position = avatar->view.eyePosition + forward * cv_torchReach.Get();
                torch.direction = forward;
                torch.color = glm::vec3(1.0f, 0.97f, 0.88f);
                torch.intensity = cv_torchIntensity.Get();
                torch.range = cv_torchRange.Get();
                torch.innerAngle = cv_torchInner.Get();
                torch.outerAngle = cv_torchOuter.Get();
                torch.sourceRadius = cv_torchSourceRadius.Get();
                consider(torch);
            }

            const float strength = avatar->body.MuzzleFlashStrength();
            if (strength > 0.001f && avatar->body.HasWeapon())
            {
                PunctualLight flash;
                flash.position = avatar->body.MuzzlePoint();
                flash.direction = glm::vec3(0.0f, -1.0f, 0.0f);
                flash.color = glm::vec3(1.0f, 0.86f, 0.62f);
                flash.intensity = cv_flashIntensity.Get() * strength;
                flash.range = cv_flashRange.Get();
                flash.innerAngle = 180.0f;
                flash.outerAngle = 180.0f;
                flash.sourceRadius = 0.35f;
                consider(flash);
            }
        }

        // Everybody else's drone lamps.
        for (const auto& [owner, proxy] : m_remoteDrones)
        {
            if (proxy.Active() && proxy.lightOn && !proxy.Disabled())
            {
                PunctualLight lamp;
                DroneLampFor(proxy, lamp);
                consider(lamp);
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

        // And the level's own lamps, lit or flickering or not at all, for each surface to choose from.
        m_levelLights.Update(m_scene, m_lightClock);
        m_levelLights.UpdateDoorways(m_world);
        // A door swinging changes what the lamps round it can see: theirs are drawn again while it moves, and
        // once more when it has stopped, so the last drawing is of the door where it came to rest.
        m_doorWasMoving.resize(m_world.Doors().size(), false);
        for (size_t d = 0; d < m_world.Doors().size(); ++d)
        {
            const WorldObjects::Door& door = m_world.Doors()[d];
            const bool moving = door.IsMoving();
            if (moving || m_doorWasMoving[d])
            {
                app.GetSceneRenderer().InvalidateLampShadows(door.hinge, door.panelOffset.x * 2.0f + 0.3f);
            }
            m_doorWasMoving[d] = moving;
        }
        environment.sceneLights.clear();
        PRED_PROFILE_SCOPE("Update: gather lamps");
        m_levelLights.Gather(environment.sceneLights);
        for (PunctualLight& lamp : environment.sceneLights)
        {
            lamp.intensity *= std::max(cv_lampScale.Get(), 0.0f);
        }
        GatherNestLights(environment.sceneLights);
        GatherItemLights(environment.sceneLights);
        for (size_t i = 0; i < kMaxPunctualLights; ++i)
        {
            if (i < candidates.size())
            {
                environment.lights[i] = candidates[i].light;
            }
            else
            {
                environment.lights[i] = PunctualLight{};
                environment.lights[i].intensity = 0.0f;
            }
        }
    }

    // Behind everything: the dark, or under water the water's own murk (the sky is not drawn there).
    if (m_underwater > 0.5f)
    {
        const glm::vec3 murk = glm::clamp(m_scene.GetEnvironment().fogColor, glm::vec3(0.0f), glm::vec3(1.0f));
        const auto byte = [](float v) { return static_cast<uint32_t>(v * 255.0f + 0.5f); };
        renderer.SetClearColor((byte(murk.r) << 24) | (byte(murk.g) << 16) | (byte(murk.b) << 8) | 0xffu);
    }
    else
    {
        renderer.SetClearColor(0x11131aff);
    }

    // What the player can reach, decided by where they are looking rather than by proximity -- and nothing at all dead (a
    // drone or a spectator is not where the body is, and cannot pick anything up).
    if (m_hidingSpot >= 0 || !m_player.State().alive)
    {
        m_interactions.ClearFocus();
    }
    else
    {
        const PlayerView& playerView = m_player.View();
        m_interactions.UpdateFocus(m_scene, app.GetPhysics(), playerView.eyePosition,
                                   playerView.Forward());
    }

    AgeTracers(deltaSeconds);
    // Marks left on anything the physics moves go where it goes.
    FollowBulletHoles();
    // Beacons and the lobby, out and in. Here rather than on the browser screen because the outgoing half belongs
    // to a running game: somebody hosting has to stay findable after they go in, which is exactly
    // when the screen that would have ticked it has stopped being drawn.
    UpdateDiscovery(deltaSeconds);
    // Before the voice and the rest: while a key is being rebound, that key press belongs to the
    // settings screen and to nothing else.
    UpdateRebinding();
    UpdateVoice(deltaSeconds);
    UpdateMicrophoneTest(deltaSeconds);
    UpdateSounds(deltaSeconds);
    SyncDynamicProps();
    UpdateCreatureVisuals(deltaSeconds);
    UpdateFlares(deltaSeconds);
    app.GetSceneRenderer().SetWireframe(cv_wireframe.Get());

    // Occlusion settings, pushed every frame rather than when they change, so the console and the
    // graphics page move the same numbers and neither has to tell the renderer it did.
    ShadowSettings& shadows = app.GetSceneRenderer().Shadows();
    shadows.sunEnabled = cv_sunShadows.Get();
    shadows.skyEnabled = cv_skyShadows.Get();
    app.GetSceneRenderer().SetLampShadowsEnabled(cv_lampShadows.Get());
    shadows.distance = std::clamp(cv_shadowDistance.Get(), 10.0f, 160.0f);
    shadows.indoorLight = std::clamp(cv_indoorLight.Get(), 0.0f, 1.0f);
    shadows.sunBias = cv_sunShadowBias.Get();
    shadows.sunNormalOffset = cv_sunShadowOffset.Get();
    shadows.skyBias = cv_skyShadowBias.Get();
    shadows.skyNormalOffset = cv_skyShadowOffset.Get();
    shadows.spotEnabled = cv_spotShadows.Get();
    shadows.debugView = std::clamp(cv_occlusionDebug.Get(), 0, 4);
    app.SetEntityCount(m_scene.EntityCount());
}

void PredationGame::SpawnProp(bool sphere, float impulse)
{
    PhysicsWorld& physics = m_app->GetPhysics();

    const glm::vec3 origin = m_cameraMode == CameraMode::Fly ? m_camera.position : m_player.View().eyePosition;
    const glm::vec3 forward = m_cameraMode == CameraMode::Fly ? m_camera.Forward() : m_player.View().Forward();

    Transform transform;
    // Spread successive props over a small grid: dropping several into the same point leaves them
    // deeply interpenetrating, and the separation impulse flings them across the map.
    //
    // Counted from the middle of the grid, so the first one lands where the view is pointing. It
    // used to start in a corner, which put a single dropped ball most of a metre to the side and
    // most of a metre closer than it was asked for -- under the gun, out of sight.
    const int index = static_cast<int>(m_props.size());
    const int cell = (index + 4) % 9;
    constexpr float spread = 0.9f;
    const glm::vec3 jitter{static_cast<float>(cell % 3 - 1) * spread,
                           static_cast<float>(index / 9) * spread,
                           static_cast<float>(cell / 3 - 1) * spread};
    transform.position = origin + forward * 2.5f + jitter;

    const BodyHandle body = sphere
                                ? physics.CreateSphere(kPropRadius, transform, BodyMotion::Dynamic, 400.0f)
                                : physics.CreateBox(glm::vec3(kPropSize * 0.5f), transform,
                                                    BodyMotion::Dynamic, 400.0f);
    if (!body.IsValid())
    {
        m_app->GetConsole().PrintError("Could not create a physics body for the prop");
        return;
    }
    physics.SetLinearVelocity(body, forward * impulse);

    const Entity entity = m_scene.CreateMeshEntity("prop", transform,
                                                   sphere ? m_propSphereMesh : m_propBoxMesh, kPropMaterial);
    m_props.push_back(DynamicProp{entity, body});
}

void PredationGame::ClearProps()
{
    PhysicsWorld& physics = m_app->GetPhysics();
    for (const DynamicProp& prop : m_props)
    {
        physics.DestroyBody(prop.body);
        m_scene.Destroy(prop.entity);
    }
    m_props.clear();
}

void PredationGame::SyncDynamicProps()
{
    PhysicsWorld& physics = m_app->GetPhysics();
    for (const DynamicProp& prop : m_props)
    {
        Transform* transform = m_scene.GetTransform(prop.entity);
        if (transform != nullptr && physics.IsValid(prop.body))
        {
            *transform = physics.GetTransform(prop.body);
        }
    }
}

void PredationGame::OnRender()
{
    Application& app = *m_app;

    // Inventory icons are drawn into their own offscreen target before the world is. It happens on
    // one frame only, because items do not change; the reserved view ids sort ahead of the UI that
    // samples the result.
    m_itemIcons.Render(app.GetSceneRenderer(), app.GetMeshes());

    // The editor draws its own scene. The level is not behind it, which is the point of it being a
    // place rather than a mode: a model is judged against an empty floor.
    if (m_screen == Screen::Editor)
    {
        // The panel first, into its own target: its view id sorts ahead of everything on screen, so
        // what the UI samples this frame is this frame's picture rather than the last one's.
        RenderEditorFirstPerson();
        app.GetSceneRenderer().Draw(Renderer::kViewMain, m_editorScene, app.GetMeshes(),
                                    m_camera.position);
        DrawDebugOverlays();
        return;
    }

    // A new frame for the planets: at most one more strip of one body's face baked (PlanetRenderer).
    m_planets.NewFrame();
    // The system map into its own target, before the UI that shows it.
    RenderSystemMap();

    const glm::vec3 viewPosition = m_renderEye;
    // Depth from the sun and depth from overhead, both fitted around the eye, before anything is
    // shaded. This is where a room with a roof on it becomes dark: nothing declares it dark, the
    // roof is simply between it and the sky.
    PRED_PROFILE_SCOPE("Render: submit");
    app.GetSceneRenderer().RenderShadows(Renderer::kViewSunShadow, Renderer::kViewSunNearShadow, Renderer::kViewSunFarShadow, Renderer::kViewSkyShadow,
                                         Renderer::kViewSpotShadow, m_scene, app.GetMeshes(),
                                         viewPosition);

    // And the mirrors, which is the world drawn a second time from a camera reflected across their
    // plane. Skipped entirely when the eye is behind them or too far away to see one: this is a
    // whole extra pass over the scene, and it is worth nothing at all from the wrong side.
    //
    // The near limit is generous rather than tight. A mirror going flat as you back away from it is
    // far more noticeable than a mirror you cannot make out, so this is set to where the panels are
    // a few pixels across rather than to where the reflection stops being informative.
    app.GetSkyRenderer().SetBrightness(cv_skyBrightness.Get());
    const float inFront =
        glm::dot(viewPosition, glm::vec3(TestMapSpec::kMirrorPlane)) + TestMapSpec::kMirrorPlane.w;
    // Only in the testing area, where the mirrors are: their plane runs on for ever, and the ship -- a long way from them but
    // close in front of the plane -- drew the whole world a second time for nothing.
    if (cv_reflections.Get() && m_map == MapChoice::TestMap && inFront > 0.05f && inFront < cv_reflectionDistance.Get())
    {
        app.GetSceneRenderer().RenderReflection(
            Renderer::kViewReflectionSky, Renderer::kViewReflection, m_scene, app.GetMeshes(),
            TestMapSpec::kMirrorPlane, app.GetRenderer().ViewMatrix(), app.GetRenderer().ProjectionMatrix(),
            viewPosition);
    }
    else
    {
        // Explicitly, rather than by leaving it alone: a stale reflection from wherever the camera
        // was standing last time it saw one is worse than no reflection at all.
        app.GetSceneRenderer().NoReflection();
    }
    // The sky behind the world, where none of it is (the world is drawn first: Renderer::kViewMain), and the bodies in it --
    // not from under water, where what is behind everything is the water.
    if (m_underwater <= 0.5f)
    {
        app.GetSkyRenderer().Draw(Renderer::kViewSky, m_scene.GetEnvironment(), app.GetRenderer().ViewMatrix(),
                                  app.GetRenderer().ProjectionMatrix(), true);
        DrawSpaceBodies();
    }
    app.GetSceneRenderer().SetCullFrustum(app.GetRenderer().ProjectionMatrix() * app.GetRenderer().ViewMatrix());
    // Out in space the camera sees for kilometres, and the site and the testing area are only one or two away: not in
    // the picture, only the ship and what is round it (the shuttle leaving showed the site hanging in space behind it).
    if (m_skyInShip)
    {
        app.GetSceneRenderer().SetDrawRegion(m_renderEye, 1200.0f);
    }
    if (cv_clusteredLights.Get())
    {
        app.GetSceneRenderer().SetClusterCamera(app.GetRenderer().ViewMatrix(), app.GetRenderer().ProjectionMatrix());
    }
#if PRED_DEV_TOOLS
    app.GetSceneRenderer().SetFullbright(cv_fullbright.Get());
#else
    app.GetSceneRenderer().SetFullbright(false);
#endif
    app.GetSceneRenderer().Draw(Renderer::kViewMain, m_scene, app.GetMeshes(), viewPosition);
    // The site's water, if it has any and the eye is over its land, blended over the world: its colour by its world -- a sea
    // blue-green and clear, a lake browner, a jungle's murky -- rain on it as falls there, waves with its wind.
    if (const Mesh* water = m_facility.WaterMesh().IsValid() ? app.GetMeshes().Get(m_facility.WaterMesh()) : nullptr)
    {
        const SitePlan& plan = m_facility.Plan();
        const glm::vec2 middle{plan.origin.x + plan.size * 0.5f, plan.origin.z + plan.size * 0.5f};
        if (glm::distance(glm::vec2(viewPosition.x, viewPosition.z), middle) < 2500.0f)
        {
            const SiteMap::Look& look = m_facility.GetLook();
            WaterRenderer::Look waterLook;
            SiteWaterLook(waterLook.tint, waterLook.clarity);
            waterLook.below = m_underwater > 0.5f;
            waterLook.rain = Weather::KindOf(look.weather) == Weather::Kind::Rain ? std::clamp(look.weatherAmount, 0.0f, 1.0f) : 0.0f;
            // Or lava, acid or ice in its place: acid yellow-green and thick, bubbling (the rings rain leaves, always there).
            switch (look.liquid)
            {
            case SiteMap::Look::Liquid::Lava: waterLook.kind = 1; break;
            case SiteMap::Look::Liquid::Acid:
                waterLook.kind = 2;
                waterLook.rain = std::max(waterLook.rain, 0.25f);
                break;
            case SiteMap::Look::Liquid::Ice: waterLook.kind = 3; break;
            default: break;
            }
            waterLook.wind = glm::length(m_scene.GetEnvironment().wind) + 2.0f;
            waterLook.seconds = static_cast<float>(std::fmod(m_campaign.clock, 3600.0));
            // (The same waves the swimmers ride: UpdateWater.)
            waterLook.waves = m_seaWaves;
            waterLook.fineMiddle = m_facility.Terrain().WaterMiddle();
            waterLook.fineReach = m_facility.Terrain().WaterFineReach();
            waterLook.skyBrightness = cv_skyBrightness.Get();
            waterLook.linear = app.GetPostProcess().Active();
            m_water.Draw(Renderer::kViewTranslucent, Depth::Test(), *water, m_scene.GetEnvironment(), viewPosition, waterLook);
        }
    }
    // The air with something in it -- the weather's flakes, streaks and puffs -- over the world and the sky, blended.
    // (And the plumes of the site's vents and what its air does, by the clock, with the weather's.)
    m_airSprites = m_weather.Sprites();
    if (m_facility.Built())
    {
        Vents::Plumes(m_facility.GetVents(), m_facility.GetLook().biome, std::fmod(m_campaign.clock, 86400.0),
                      glm::vec3(m_scene.GetEnvironment().wind.x, 0.0f, m_scene.GetEnvironment().wind.y),
                      m_scene.GetEnvironment(), viewPosition, m_airSprites);
        Atmospherics::Sprites(m_facility.GetLook().biome,
                              glm::vec3(m_scene.GetEnvironment().wind.x, 0.0f, m_scene.GetEnvironment().wind.y), m_scene.GetEnvironment(), viewPosition,
                              m_windTravel, m_facility.Terrain(), m_airSprites);
    }
    // None of it under water: not seen from under it at all, and from above, nothing falling on past its surface.
    if (m_underwater > 0.5f)
    {
        m_airSprites.clear();
    }
    else if (m_swimWaterSet)
    {
        // (Its highest crests for the big soft ones, which stand over a stretch of it.)
        const float surface = m_swimWater.level + m_seaWaves.Highest();
        m_airSprites.erase(std::remove_if(m_airSprites.begin(), m_airSprites.end(),
                                          [&](const Sprite& sprite)
                                          { return m_swimWater.depth(sprite.at.x, sprite.at.z) > 0.0f && sprite.at.y < m_swimWater.SurfaceAt(sprite.at.x, sprite.at.z); }),
                           m_airSprites.end());
        // And a big soft one over it -- a puff of mist in the rain -- kept clear of it: its card through the surface showed
        // as a blot cut off flat along the water. Lifted until its edge is over it, and fainter the lower it was.
        for (Sprite& sprite : m_airSprites)
        {
            const float half = sprite.size * 0.5f;
            if (half > 0.3f && sprite.at.y - half < surface && m_swimWater.depth(sprite.at.x, sprite.at.z) > -half)
            {
                sprite.alpha *= glm::smoothstep(0.0f, half, sprite.at.y - surface);
                sprite.at.y = surface + half;
            }
        }
    }
    m_sprites.Draw(Renderer::kViewTranslucent, Depth::Test(), viewPosition, m_airSprites);
    DrawDebugOverlays();
}

void PredationGame::UpdateLightning(float dt, float storm)
{
    // Where and when every strike is comes from the campaign's clock and the site, not from this machine's dice: everybody
    // sees the same bolt at the same moment and hears the same thunder after it. A storm's time is cut into stretches, as
    // long as the storm is bad (forty-five seconds to nine), and in each one strike at a time of its own, somewhere of its
    // own -- mostly far off, now and then close.
    const auto hash = [](uint32_t a, uint32_t b)
    {
        uint32_t h = a * 0x8DA6B343u ^ b * 0xD8163841u;
        h ^= h >> 13;
        h *= 0x5BD1E995u;
        h ^= h >> 15;
        return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
    };
    const double now = m_campaign.clock;
    bool strike = false;
    float distance = 0.0f;
    float yaw = 0.0f;
    float shape = 0.0f;
    if (storm > 0.0f && m_lightningClock >= 0.0 && now > m_lightningClock && now - m_lightningClock < 2.0)
    {
        const double stretch = glm::mix(45.0, 9.0, static_cast<double>(storm));
        const uint32_t site = m_facility.Seed();
        for (auto k = static_cast<int64_t>(std::floor(m_lightningClock / stretch)); k <= static_cast<int64_t>(std::floor(now / stretch)); ++k)
        {
            const auto slot = static_cast<uint32_t>(k);
            const double at = (static_cast<double>(k) + 0.1 + 0.8 * hash(site, slot * 5u)) * stretch;
            if (at > m_lightningClock && at <= now)
            {
                strike = true;
                distance = 400.0f + 5600.0f * std::pow(hash(site, slot * 5u + 1u), 1.4f);
                yaw = hash(site, slot * 5u + 2u) * glm::two_pi<float>();
                shape = hash(site, slot * 5u + 3u) * 100.0f;
            }
        }
    }
    // (A clock that has jumped -- a day skipped, the host's arriving -- is not a storm's worth of strikes at once.)
    m_lightningClock = now;
    // And one asked for (lightning_now), on this machine only, to look at.
    if (m_lightningIn <= 0.0f)
    {
        m_lightningIn = 1.0f;
        const auto random = [] { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); };
        strike = true;
        distance = m_lightningAim.y > 0.0f ? m_lightningAim.y : 400.0f + 5600.0f * std::pow(random(), 1.4f);
        yaw = m_lightningAim.x >= 0.0f ? glm::radians(m_lightningAim.x) : random() * glm::two_pi<float>();
        shape = random() * 100.0f;
        m_lightningAim = glm::vec2(-1.0f);
    }
    if (strike)
    {
        PRED_LOG_DEBUG(Gameplay, "Lightning {:.0f} m off at {:.0f} degrees", distance, glm::degrees(yaw));
        m_lightningAge = 0.0f;
        m_lightningDistance = distance;
        m_lightningSeed = shape;
        // Up to the cloud base, a kilometre and some up: high in the sky close by, low over the horizon far off.
        const float top = 1100.0f / std::sqrt(1100.0f * 1100.0f + distance * distance);
        m_lightningBolt = {yaw, top, shape, 0.0f};
        // The thunder, as long after as sound takes to come that far: a crack close by, a long roll far off, quieter the
        // further.
        const float near = 1.0f - glm::smoothstep(400.0f, 6000.0f, distance);
        const glm::vec3 towards{std::sin(yaw), 0.3f, -std::cos(yaw)};
        QueueSound(distance / 343.0f, distance < 1300.0f ? "Weather/thunder_near" : "Weather/thunder", m_renderEye + towards * 2.0f,
                   glm::mix(0.1f, 0.42f, near), false);
    }
    m_lightningFlash = 0.0f;
    m_lightningBolt.w = 0.0f;
    if (m_lightningAge < 0.0f)
    {
        return;
    }
    // In one to four strokes a tenth of a second or so apart, each a hard flash dying in a few hundredths, the sky glowing a
    // little after; as bright as it is near.
    m_lightningAge += dt;
    const int strokes = 1 + static_cast<int>(std::fmod(m_lightningSeed * 7.31f, 4.0f));
    float flash = 0.0f;
    float bolt = 0.0f;
    for (int i = 0; i < strokes; ++i)
    {
        const float at = static_cast<float>(i) * 0.085f + std::fmod(m_lightningSeed * (3.7f + static_cast<float>(i)), 0.05f);
        const float since = m_lightningAge - at;
        if (since >= 0.0f)
        {
            const float strength = i == 0 ? 1.0f : 0.55f + 0.4f * std::fmod(m_lightningSeed * (1.9f + static_cast<float>(i)), 1.0f);
            flash += strength * std::exp(-since / 0.045f);
            bolt += strength * std::exp(-since / 0.03f);
        }
    }
    flash += 0.12f * std::exp(-m_lightningAge / 0.35f);
    const float bright = std::clamp(1.2f - m_lightningDistance / 6000.0f, 0.3f, 1.0f);
    m_lightningFlash = flash * bright;
    m_lightningBolt.w = std::min(bolt, 1.5f) * std::clamp(1.4f - m_lightningDistance / 6000.0f, 0.3f, 1.0f);
    if (m_lightningAge > 1.2f)
    {
        m_lightningAge = -1.0f;
    }
}

} // namespace pred
