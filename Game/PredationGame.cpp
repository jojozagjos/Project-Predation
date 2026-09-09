#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/World/TestMap.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace pred
{
namespace
{

CVar<float> cv_fov{"r.fov", 90.0f, "Horizontal field of view in degrees", CVarFlags::Archive};
CVar<float> cv_mouseSensitivity{"input.mouse_sensitivity", 0.12f, "Mouse look sensitivity in degrees per pixel",
                                CVarFlags::Archive};
CVar<bool> cv_invertY{"input.invert_y", false, "Invert vertical mouse look", CVarFlags::Archive};
CVar<bool> cv_crouchToggle{"input.crouch_toggle", true, "Crouch and prone toggle instead of being held",
                           CVarFlags::Archive};
CVar<bool> cv_sprintToggle{"input.sprint_toggle", false, "Sprint toggles instead of being held",
                           CVarFlags::Archive};
CVar<float> cv_flySpeed{"cam.fly_speed", 6.0f, "Fly camera speed in meters per second"};
// Adjustable because a prone body needs the camera much further back than a standing one, and
// inspecting the prone roll from three metres puts the camera inside the character.
CVar<float> cv_thirdDistance{"cam.third_distance", 3.2f, "How far the third-person camera sits behind the character",
                             CVarFlags::Archive};
// Remembered between runs, so rejoining the same friend does not mean typing the address again.
CVar<std::string> cv_lastAddress{"net.last_address", "127.0.0.1", "Address the join box opens with",
                                 CVarFlags::Archive};
CVar<int> cv_lastPort{"net.last_port", kDefaultPort, "Port the join box opens with", CVarFlags::Archive};
// On by default: a four-player extraction game where rounds pass through your team is a different
// game, and a quieter one.
CVar<bool> cv_friendlyFire{"game.friendly_fire", true, "Rounds hurt other players", CVarFlags::Archive};
CVar<bool> cv_showGrid{"debug.show_grid", false, "Draw the reference grid"};
CVar<bool> cv_wireframe{"r.wireframe", false, "Draw scene meshes as wireframe"};
CVar<float> cv_fogStart{"r.fog_start", 12.0f, "Fog start distance in meters"};
CVar<float> cv_fogEnd{"r.fog_end", 90.0f, "Fog end distance in meters"};
CVar<float> cv_sunIntensity{"r.sun_intensity", 2.2f, "Directional light intensity"};

constexpr float kPropRadius = 0.3f;
constexpr float kPropSize = 0.5f;
const Material kPropMaterial = Material::Diffuse({0.70f, 0.55f, 0.25f}, 0.55f);

std::filesystem::path PlayerConfigPath()
{
    return Paths::AssetsRoot() / "Data" / "player.json";
}

} // namespace

bool PredationGame::OnInit(Application& app)
{
    m_app = &app;

    BuildTestMap(m_scene, app.GetMeshes(), &app.GetPhysics());

    m_propSphereMesh = app.GetMeshes().Upload(Primitives::Sphere(kPropRadius, 20, 14), "prop_sphere");
    m_propBoxMesh = app.GetMeshes().Upload(Primitives::Box(glm::vec3(kPropSize)), "prop_box");

    PlayerConfig config;
    config.LoadFromFile(PlayerConfigPath());
    if (!m_player.Init(app.GetPhysics(), config, m_spawnPoint))
    {
        PRED_LOG_ERROR(Gameplay, "Player controller failed to initialize");
        return false;
    }

    m_body.Build(m_scene, app.GetMeshes(), m_player.Config());

    m_items.LoadFromFile(Paths::AssetsRoot() / "Data" / "items.json");
    // Weapons load before the icons, because a weapon item draws its icon from the weapon's own
    // model and would otherwise fall back to the placeholder block.
    m_weaponData.LoadFromFile(Paths::AssetsRoot() / "Data" / "weapons.json");
    m_itemIcons.Build(m_items, app.GetMeshes(), app.GetRenderer(), &m_weaponData);
    m_world.Build(m_scene, app.GetMeshes(), app.GetPhysics(), m_interactions, m_items, &m_weaponData);
    app.GetPhysics().OptimizeBroadPhase();

    // Checked once the whole level exists, so a piece placed on top of another is caught here
    // rather than by walking into it. 10 mm is below anything anyone would notice.
    ReportMapOverlaps(0.01f);

    // Editing player.json on disk applies immediately, without a rebuild or a restart.
    app.GetFileWatcher().Watch(PlayerConfigPath(),
                               [this](const std::filesystem::path&) { ReloadPlayerConfig(); });

    // Yaw 0 looks down -Z, which is where the test map is from the spawn point.
    m_lookYaw = 0.0f;
    m_lookPitch = 0.0f;
    m_camera.position = {0.0f, 3.2f, 22.0f};

    m_editor.Init(app);

    RegisterCommands();
    RegisterNetCommands();
    // The game opens at the menu, with the world already built behind it.
    std::snprintf(m_joinAddress, sizeof(m_joinAddress), "%s", cv_lastAddress.Get().c_str());
    m_joinPort = std::clamp(cv_lastPort.Get(), 1024, 65535);
    ReturnToTitle();
    UpdateMouseCapture();

    PRED_LOG_INFO(Gameplay,
                  "Controls: WASD move, Space jump, Ctrl or C crouch, Z prone, Shift sprint, Alt "
                  "walk, Q and E lean. Left mouse fires, right mouse aims, R reloads. "
                  "F interact, G drop, 1 to 6 and the wheel select, Tab inventory. "
                  "P cycles first person, third person and free camera; in third person hold middle "
                  "mouse to orbit. F2 model editor, F3 overlay, F5 respawn, backtick console, "
                  "Escape frees the cursor.");
    if (!cv_crouchToggle.Get())
    {
        PRED_LOG_INFO(Gameplay, "Crouch and prone are hold-to-activate. Set input.crouch_toggle to "
                                "true in the console to make them toggle instead.");
    }
    return true;
}

std::string PredationGame::DescribeBody(BodyHandle body) const
{
    if (m_app == nullptr || !m_app->GetPhysics().IsValid(body))
    {
        return "<unknown>";
    }
    const glm::vec3 position = m_app->GetPhysics().GetTransform(body).position;

    std::string best = "<unnamed>";
    float bestDistance = std::numeric_limits<float>::max();
    m_scene.ForEachMeshRenderer([&](Entity entity, const Transform& transform, const MeshRenderer&) {
        const float distance = glm::distance(transform.position, position);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = m_scene.Name(entity);
        }
    });
    // A body far from every entity centre is one of several making up a compound object, such as a
    // locker wall, so say so rather than claiming a name that is only roughly right.
    return bestDistance > 1.5f ? best + "?" : best;
}

void PredationGame::ReportMapOverlaps(float minPenetration)
{
    if (m_app == nullptr)
    {
        return;
    }
    const std::vector<PhysicsWorld::StaticOverlap> overlaps =
        m_app->GetPhysics().FindStaticOverlaps(minPenetration);
    if (overlaps.empty())
    {
        PRED_LOG_INFO(Gameplay, "Map geometry check: no static solids overlap by more than {:.0f} mm",
                      minPenetration * 1000.0f);
        return;
    }

    PRED_LOG_WARN(Gameplay, "Map geometry check: {} overlapping pairs of static solids", overlaps.size());
    for (const PhysicsWorld::StaticOverlap& overlap : overlaps)
    {
        PRED_LOG_WARN(Gameplay, "  {} into {}, {:.0f} mm deep at ({:.2f}, {:.2f}, {:.2f})",
                      DescribeBody(overlap.a), DescribeBody(overlap.b), overlap.penetration * 1000.0f,
                      overlap.position.x, overlap.position.y, overlap.position.z);
    }
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

void PredationGame::RegisterCommands()
{
    Console& console = m_app->GetConsole();

    console.RegisterCommand("respawn", "Return the player to the spawn point with full health",
                            [this](const std::vector<std::string>&) { m_player.Respawn(m_spawnPoint); });

    console.RegisterCommand(
        "teleport", "Move the player to a position: teleport <x> <y> <z>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 4)
            {
                m_app->GetConsole().PrintError("usage: teleport <x> <y> <z>");
                return;
            }
            m_player.Teleport({std::strtof(args[1].c_str(), nullptr), std::strtof(args[2].c_str(), nullptr),
                               std::strtof(args[3].c_str(), nullptr)});
        },
        "teleport <x> <y> <z>");

    console.RegisterCommand(
        "camera", "Switch camera: camera <first|third|fly>",
        [this](const std::vector<std::string>& args)
        {
            const std::string which = args.size() > 1 ? args[1] : "first";
            if (which == "first")
            {
                SetCameraMode(CameraMode::FirstPerson);
            }
            else if (which == "third")
            {
                SetCameraMode(CameraMode::ThirdPerson);
            }
            else if (which == "fly")
            {
                SetCameraMode(CameraMode::Fly);
            }
            else
            {
                m_app->GetConsole().PrintError("usage: camera <first|third|fly>");
            }
        },
        "camera <first|third|fly>");

    console.RegisterCommand("fly", "Toggle the free-flying inspection camera",
                            [this](const std::vector<std::string>&)
                            {
                                SetCameraMode(m_cameraMode == CameraMode::Fly ? CameraMode::FirstPerson
                                                                              : CameraMode::Fly);
                            });

    console.RegisterCommand(
        "cam_orbit", "Set the third-person orbit offset: cam_orbit <yaw> <pitch>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 3)
            {
                m_app->GetConsole().PrintError("usage: cam_orbit <yaw> <pitch>");
                return;
            }
            m_orbitYaw = glm::radians(std::strtof(args[1].c_str(), nullptr));
            m_orbitPitch = glm::radians(std::strtof(args[2].c_str(), nullptr));
        },
        "cam_orbit <yaw> <pitch>");

    console.RegisterCommand(
        "stance", "Hold a stance for inspection: stance <stand|crouch|prone|auto>",
        [this](const std::vector<std::string>& args)
        {
            const std::string which = args.size() > 1 ? args[1] : "auto";
            m_forceCrouch = which == "crouch";
            m_forceProne = which == "prone";
            if (which != "stand" && which != "crouch" && which != "prone" && which != "auto")
            {
                m_app->GetConsole().PrintError("usage: stance <stand|crouch|prone|auto>");
                return;
            }
            m_app->GetConsole().Print("Stance override: " + which);
        },
        "stance <stand|crouch|prone|auto>");

    console.RegisterCommand(
        "player_yaw", "Face the player in a given direction, for inspection: player_yaw <degrees>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: player_yaw <degrees>");
                return;
            }
            m_player.State().yaw = glm::radians(std::strtof(args[1].c_str(), nullptr));
            m_app->GetConsole().Print("Player yaw set");
        },
        "player_yaw <degrees>");

    console.RegisterCommand(
        "map_overlaps", "List level geometry that intersects other level geometry",
        [this](const std::vector<std::string>& args)
        {
            const float minimum = args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : 0.01f;
            const std::vector<PhysicsWorld::StaticOverlap> overlaps =
                m_app->GetPhysics().FindStaticOverlaps(minimum);
            if (overlaps.empty())
            {
                m_app->GetConsole().Print("No static solids overlap");
                return;
            }
            for (const PhysicsWorld::StaticOverlap& overlap : overlaps)
            {
                char buffer[256];
                std::snprintf(buffer, sizeof(buffer), "%s into %s, %.0f mm at %.2f %.2f %.2f",
                              DescribeBody(overlap.a).c_str(), DescribeBody(overlap.b).c_str(),
                              overlap.penetration * 1000.0f, overlap.position.x, overlap.position.y,
                              overlap.position.z);
                m_app->GetConsole().Print(buffer);
            }
        },
        "map_overlaps [minimum metres]");
    console.RegisterCommand("player_reload", "Reload player.json from disk",
                            [this](const std::vector<std::string>&) { ReloadPlayerConfig(); });

    console.RegisterCommand("player_save", "Write the current player tuning back to player.json",
                            [this](const std::vector<std::string>&)
                            {
                                if (m_player.Config().SaveToFile(PlayerConfigPath()))
                                {
                                    m_app->GetConsole().Print("Saved " + PlayerConfigPath().string());
                                }
                            });

    console.RegisterCommand("player_state", "Print the player's simulation state",
                            [this](const std::vector<std::string>&)
                            {
                                const PlayerState& state = m_player.State();
                                char buffer[256];
                                std::snprintf(buffer, sizeof(buffer),
                                              "pos %.2f %.2f %.2f  speed %.2f m/s  %s  %s  health %.0f",
                                              state.position.x, state.position.y, state.position.z,
                                              state.HorizontalSpeed(), PlayerStanceName(state.stance),
                                              state.grounded ? "grounded" : "airborne", state.health);
                                m_app->GetConsole().Print(buffer);
                            });

    console.RegisterCommand("cam_reset", "Reset the fly camera",
                            [this](const std::vector<std::string>&)
                            {
                                m_camera = FlyCamera{};
                                m_camera.position = {0.0f, 3.2f, 22.0f};
                            });

    console.RegisterCommand(
        "cam_pos", "Print or set the fly camera position, and optionally its yaw and pitch",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() >= 4)
            {
                m_camera.position = glm::vec3(std::strtof(args[1].c_str(), nullptr),
                                              std::strtof(args[2].c_str(), nullptr),
                                              std::strtof(args[3].c_str(), nullptr));
                m_cameraMode = CameraMode::Fly;
            }
            if (args.size() >= 6)
            {
                m_lookYaw = glm::radians(std::strtof(args[4].c_str(), nullptr));
                m_lookPitch = glm::radians(std::strtof(args[5].c_str(), nullptr));
            }
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "camera %.2f %.2f %.2f  yaw %.1f pitch %.1f",
                          m_camera.position.x, m_camera.position.y, m_camera.position.z,
                          glm::degrees(m_lookYaw), glm::degrees(m_lookPitch));
            m_app->GetConsole().Print(buffer);
        },
        "cam_pos [x y z [yaw pitch]]");

    console.RegisterCommand(
        "give", "Put an item straight into the inventory: give <key> [count]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: give <key> [count]");
                return;
            }
            const ItemId id = m_items.IdOf(args[1]);
            if (id == kInvalidItem)
            {
                m_app->GetConsole().PrintError("no such item: " + args[1]);
                return;
            }
            const int count = args.size() >= 3 ? std::atoi(args[2].c_str()) : 1;
            const int stored = m_inventory.Add(m_items, id, std::max(count, 1));
            m_app->GetConsole().Print("Stored " + std::to_string(stored) + " " + args[1]);
        },
        "give <key> [count]");

    console.RegisterCommand(
        "move", "Drive the player from the console, for inspecting animation: move <x> <y> (-1..1)",
        [this](const std::vector<std::string>& args)
        {
            m_debugMove = args.size() >= 3 ? glm::vec2(std::strtof(args[1].c_str(), nullptr),
                                                       std::strtof(args[2].c_str(), nullptr))
                                           : glm::vec2(0.0f);
        },
        "move <x> <y>");

    console.RegisterCommand(
        "give_weapon", "Put a weapon in the inventory and select it: give_weapon <key>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: give_weapon <key>");
                return;
            }
            const WeaponDefinition* weapon = m_weaponData.Find(args[1]);
            if (weapon == nullptr)
            {
                m_app->GetConsole().PrintError("no such weapon: " + args[1]);
                return;
            }
            const ItemId id = m_items.IdOf(weapon->item);
            if (id == kInvalidItem || m_inventory.Add(m_items, id, 1) <= 0)
            {
                m_app->GetConsole().PrintError("no room for " + weapon->name);
                return;
            }
            for (int i = 0; i < m_inventory.SlotCount(); ++i)
            {
                if (m_inventory.At(i).item == id)
                {
                    m_inventory.SelectSlot(i);
                    break;
                }
            }
            SyncEquippedWeapon();
            m_app->GetConsole().Print("Equipped " + weapon->name);
        },
        "give_weapon <key>");

    console.RegisterCommand(
        "fire", "Hold the trigger for a number of ticks, for testing without a mouse: fire [ticks]",
        [this](const std::vector<std::string>& args)
        { m_debugTriggerTicks = args.size() >= 2 ? std::atoi(args[1].c_str()) : 1; }, "fire [ticks]");

    console.RegisterCommand("weapon_state", "Print the equipped weapon's simulation state",
                            [this](const std::vector<std::string>&)
                            {
                                const WeaponDefinition* weapon = EquippedWeapon();
                                if (weapon == nullptr)
                                {
                                    m_app->GetConsole().Print("Unarmed");
                                    return;
                                }
                                char buffer[224];
                                std::snprintf(buffer, sizeof(buffer),
                                              "%s  %d/%d  %s  aim %.2f  spread %.2f deg  shots %u",
                                              weapon->name.c_str(), m_weapon.rounds, m_weapon.reserve,
                                              m_weapon.IsReloading() ? "reloading" : "ready", m_weapon.aim,
                                              WeaponSim::CurrentSpread(*weapon, m_weapon),
                                              m_weapon.shotCount);
                                m_app->GetConsole().Print(buffer);
                            });

    console.RegisterCommand(
        "aim", "Hold or release the sights, for inspecting the hold without a mouse: aim [0|1]",
        [this](const std::vector<std::string>& args)
        { m_debugAim = args.size() < 2 || std::atoi(args[1].c_str()) != 0; }, "aim [0|1]");

    console.RegisterCommand("reload", "Start a reload",
                            [this](const std::vector<std::string>&) { m_reloadLatch = 30; });

    console.RegisterCommand(
        "editor", "Open or close the model and animation editor: editor [model name]",
        [this](const std::vector<std::string>& args)
        {
            ToggleEditor();
            if (m_editor.IsOpen() && args.size() >= 2)
            {
                m_editor.Load(args[1]);
            }
        },
        "editor [model]");

    console.RegisterCommand(
        "model_export",
        "Write a weapon's built-in shape out as an editable model: model_export <weapon> [model name]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: model_export <weapon> [model name]");
                return;
            }
            const WeaponDefinition* weapon = m_weaponData.Find(args[1]);
            if (weapon == nullptr)
            {
                m_app->GetConsole().PrintError("no such weapon: " + args[1]);
                return;
            }
            const std::string modelName = args.size() >= 3 ? args[2] : weapon->key;
            if (ExportWeaponModel(*weapon, modelName))
            {
                m_app->GetConsole().Print("Wrote model '" + modelName +
                                          "'. Open it with: editor " + modelName);
                m_app->GetConsole().Print("Add \"model\": \"" + modelName +
                                          "\" to the weapon in weapons.json to use it in the game.");
            }
        },
        "model_export <weapon> [model]");

    console.RegisterCommand(
        "look", "Point the view, for inspecting poses without a mouse: look <yaw> [pitch]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: look <yaw> [pitch]");
                return;
            }
            m_lookYaw = glm::radians(std::strtof(args[1].c_str(), nullptr));
            if (args.size() >= 3)
            {
                m_lookPitch = glm::radians(std::strtof(args[2].c_str(), nullptr));
            }
        },
        "look <yaw> [pitch]");

    console.RegisterCommand("interact", "Use whatever the player is looking at",
                            [this](const std::vector<std::string>&) { TryInteract(); });

    console.RegisterCommand("drop", "Drop the selected item in front of the player",
                            [this](const std::vector<std::string>&) { DropSelected(); });

    console.RegisterCommand("inventory", "Open or close the inventory panel",
                            [this](const std::vector<std::string>&)
                            {
                                m_inventoryOpen = !m_inventoryOpen;
                                UpdateMouseCapture();
                            });

    console.RegisterCommand("scene_stats", "Print scene and mesh statistics",
                            [this](const std::vector<std::string>&)
                            {
                                const SceneRenderer::Stats& stats = m_app->GetSceneRenderer().LastStats();
                                char buffer[220];
                                std::snprintf(buffer, sizeof(buffer),
                                              "entities %zu   meshes %zu   submitted %zu   triangles %zu",
                                              m_scene.EntityCount(), m_app->GetMeshes().Count(),
                                              stats.meshesSubmitted, stats.trianglesSubmitted);
                                m_app->GetConsole().Print(buffer);
                            });

    console.RegisterCommand(
        "phys_drop", "Spawn a dynamic prop in front of the view: phys_drop [sphere|box] [count]",
        [this](const std::vector<std::string>& args)
        {
            const bool sphere = args.size() < 2 || args[1] != "box";
            const int count = args.size() > 2 ? std::atoi(args[2].c_str()) : 1;
            for (int i = 0; i < std::clamp(count, 1, 64); ++i)
            {
                SpawnProp(sphere, 6.0f);
            }
            m_app->GetConsole().Print("Props in world: " + std::to_string(m_props.size()));
        },
        "phys_drop [sphere|box] [count]");

    console.RegisterCommand("phys_clear", "Remove every dynamic prop",
                            [this](const std::vector<std::string>&)
                            {
                                const size_t removed = m_props.size();
                                ClearProps();
                                m_app->GetConsole().Print("Removed " + std::to_string(removed) + " props");
                            });
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

glm::vec3 PredationGame::PlayerPosition(uint8_t player) const
{
    if (player == LocalPlayerId())
    {
        return m_player.State().position;
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.id == player)
        {
            return remote.position;
        }
    }
    return m_player.State().position;
}

bool PredationGame::PerformInteraction(InteractionKind kind, int index, uint8_t player)
{
    switch (kind)
    {
    case InteractionKind::Door:
    {
        if (!m_world.ToggleDoor(index, m_interactions))
        {
            return false;
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
        const bool mine = player == LocalPlayerId();
        const int wanted = pickup->count;
        int stored = wanted;
        if (mine)
        {
            stored = m_inventory.Add(m_items, pickup->item, pickup->count);
            if (stored <= 0)
            {
                m_app->GetConsole().Print("Inventory full");
                return false;
            }
        }

        if (stored >= wanted)
        {
            const ItemId taken = pickup->item;
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
        WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(index);
        if (spot == nullptr || (spot->occupied && player != LocalPlayerId()))
        {
            return false;
        }
        if (player == LocalPlayerId())
        {
            EnterHidingSpot(index);
        }
        else
        {
            // Somebody else got in. Their body is drawn from their replicated position anyway, so
            // all this machine has to do is swing the door and mark the locker taken.
            spot->occupied = true;
            m_world.SetDoorOpen(spot->doorIndex, false, m_interactions);
        }
        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::LockerUsed;
            event.index = static_cast<uint8_t>(index);
            event.player = player;
            event.flag = true;
            m_host.Broadcast(event);
        }
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

    case InteractionKind::Generic:
    default:
        return false;
    }
}

void PredationGame::ServeClientRequests()
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }

    for (const NetHost::InteractRequest& request : m_host.TakeInteractRequests())
    {
        if (request.kind >= static_cast<uint8_t>(InteractionKind::AmmoCrate) + 1)
        {
            continue;
        }
        const auto kind = static_cast<InteractionKind>(request.kind);

        // A client says what it wants, never where it is. The host checks the distance itself
        // against the position it simulated, so reach cannot be claimed.
        const Interactable* target = m_interactions.FindByPayload(kind, request.index);
        if (target == nullptr || !target->enabled)
        {
            continue;
        }
        const Transform* transform = m_scene.GetTransform(target->entity);
        if (transform == nullptr)
        {
            continue;
        }
        const glm::vec3 focus = transform->position + target->focusOffset;
        const glm::vec3 from = PlayerPosition(request.player) + glm::vec3(0.0f, 1.2f, 0.0f);
        // A little slack over the prompt's own range, because the client asked at a position the
        // host has since moved them on from.
        if (glm::distance(focus, from) > target->range + 1.0f)
        {
            PRED_LOG_DEBUG(Network, "Player {} asked for something out of reach", request.player);
            continue;
        }
        PerformInteraction(kind, request.index, request.player);
    }

    for (const NetHost::ShotRequest& request : m_host.TakeShotRequests())
    {
        // The client has already played the shot for itself. The host decides what it hit.
        const WeaponDefinition* definition = EquippedWeapon();
        FireEvent shot;
        shot.origin = request.shot.origin;
        shot.direction = glm::normalize(request.shot.direction);
        shot.range = definition != nullptr ? definition->range : 60.0f;
        shot.damage = definition != nullptr ? definition->damage : 20.0f;
        shot.sequence = request.shot.shotNumber;

        ShotResult result = ResolveShot(m_app->GetPhysics(), shot);
        // Rewound to the moment this client says it was looking at, clamped by the host to what a
        // playable connection could justify.
        ResolvePlayerHits(shot, request.player, result, m_host.PosesAt(request.shot.renderTick));

        WorldEventMessage event;
        event.kind = WorldEventKind::ShotFired;
        event.player = request.player;
        // The trace starts at the eye, so that what is under the crosshair is what gets hit. The
        // drawn line has to start at the muzzle instead, or everyone else watches rounds come out
        // of the shooter's face. Where their muzzle is, is where this machine is drawing their gun.
        event.position = MuzzleOf(request.player, shot.origin, shot.direction);
        event.direction = result ? result.position : shot.origin + shot.direction * shot.range;
        event.flag = result.hit;
        m_host.Broadcast(event);

        Tracer tracer;
        tracer.from = event.position;
        tracer.to = event.direction;
        tracer.hit = event.flag;
        m_tracers.push_back(tracer);
    }

    for (const NetHost::DropRequest& request : m_host.TakeDropRequests())
    {
        // Only what the host actually handed them. Otherwise dropping is a way to make items out of
        // nothing, which is what let one get duplicated.
        if (!m_host.TakeCarried(request.player, request.drop.item, request.drop.count))
        {
            continue;
        }
        const int index = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                              m_interactions, m_items,
                                              static_cast<ItemId>(request.drop.item),
                                              request.drop.count, request.drop.position,
                                              request.drop.velocity);
        if (index < 0)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupSpawned;
        event.index = static_cast<uint8_t>(index);
        event.item = request.drop.item;
        event.other = request.drop.count;
        event.position = request.drop.position;
        event.direction = request.drop.velocity;
        m_host.Broadcast(event);
    }

    for (const uint8_t player : m_host.TakeJoined())
    {
        SendWorldToPlayer(player);
    }
}

void PredationGame::SendWorldToPlayer(uint8_t player)
{
    // Somebody who has just walked in has to be told what has already happened, or every door that
    // was opened before they arrived is shut on their screen for the rest of the game.
    for (size_t i = 0; i < m_world.Doors().size(); ++i)
    {
        const WorldObjects::Door& door = m_world.Doors()[i];
        if (!door.IsOpen())
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::DoorMoved;
        event.index = static_cast<uint8_t>(i);
        event.flag = true;
        m_host.SendTo(player, event);
    }

    for (size_t i = 0; i < m_world.Pickups().size(); ++i)
    {
        if (m_world.Pickups()[i].alive)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupTaken;
        event.index = static_cast<uint8_t>(i);
        event.player = 0;
        m_host.SendTo(player, event);
    }

    for (size_t i = 0; i < m_world.HidingSpots().size(); ++i)
    {
        if (!m_world.HidingSpots()[i].occupied)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::LockerUsed;
        event.index = static_cast<uint8_t>(i);
        event.player = 0;
        event.flag = true;
        m_host.SendTo(player, event);
    }
}

void PredationGame::ApplyWorldEvent(const WorldEventMessage& event)
{
    switch (event.kind)
    {
    case WorldEventKind::DoorMoved:
        m_world.SetDoorOpen(event.index, event.flag, m_interactions);
        break;

    case WorldEventKind::PickupTaken:
    {
        // This is the host answering. Asking put nothing in the bag, because asking is all a client
        // does, so the item goes in now. Without this the thing simply vanished: the host took it
        // out of the world and nobody ever gave it to anyone.
        //
        // What it was is read from this machine's own copy rather than sent, because both ends
        // built the same world and have applied the same events, so the index means the same thing
        // on both. Sending it again would be sending something already known.
        const WorldObjects::Pickup* pickup = m_world.GetPickup(event.index);
        if (pickup != nullptr && pickup->alive && event.player == LocalPlayerId())
        {
            const int stored = m_inventory.Add(m_items, pickup->item, pickup->count);
            if (stored < pickup->count)
            {
                // The bag filled between asking and being answered. Rare, and worth saying out loud
                // rather than losing the difference in silence.
                PRED_LOG_WARN(Gameplay, "Only had room for {} of {}", stored, pickup->count);
            }
        }
        m_world.ConsumePickup(event.index, m_scene, m_app->GetPhysics(), m_interactions);
        break;
    }

    case WorldEventKind::PickupSpawned:
        m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                            static_cast<ItemId>(event.item), static_cast<int>(event.other),
                            event.position, event.direction);
        break;

    case WorldEventKind::LockerUsed:
        if (WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(event.index))
        {
            if (event.player == LocalPlayerId())
            {
                break; // our own, already applied when we asked
            }
            spot->occupied = event.flag;
            m_world.SetDoorOpen(spot->doorIndex, !event.flag, m_interactions);
        }
        break;

    case WorldEventKind::AmmoTaken:
        if (event.player != LocalPlayerId())
        {
            m_world.DrawFromAmmoCrate(event.index, m_interactions);
        }
        break;

    case WorldEventKind::ShotFired:
        if (event.player != LocalPlayerId())
        {
            // Somebody else's round. Drawn, never resolved: what it hit was decided by the host.
            Tracer tracer;
            tracer.from = event.position;
            tracer.to = event.direction;
            tracer.hit = event.flag;
            m_tracers.push_back(tracer);
        }
        break;

    case WorldEventKind::PlayerDamaged:
        if (event.player == LocalPlayerId())
        {
            // The host is the authority on health, so this is set rather than subtracted.
            m_player.State().health = std::max(m_player.State().health - event.amount, 0.0f);
        }
        break;

    case WorldEventKind::PlayerDied:
        if (event.player == LocalPlayerId())
        {
            m_player.State().health = 0.0f;
            m_player.State().alive = false;
            m_deathImpulse = event.direction;
        }
        break;

    case WorldEventKind::PlayerRespawned:
        if (event.player == LocalPlayerId())
        {
            m_player.Respawn(event.position);
        }
        break;

    case WorldEventKind::Count:
        break;
    }
}

void PredationGame::SendDynamicBodies()
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }

    // At the snapshot rate, not the tick rate. Sending loose objects sixty times a second doubles
    // what they cost for a value nobody can see change twice in a thirtieth of a second, and it is
    // the largest message the host sends.
    m_worldStateTimer += 1.0f / 60.0f;
    constexpr float kInterval = 1.0f / 30.0f;
    if (m_worldStateTimer < kInterval)
    {
        return;
    }
    m_worldStateTimer = std::fmod(m_worldStateTimer, kInterval);

    // Loose objects are sent as state rather than as events: a crate sliding across the floor is a
    // value that will be sent again in a thirtieth of a second, so losing one costs nothing.
    WorldStateMessage state;
    PhysicsWorld& physics = m_app->GetPhysics();

    for (size_t i = 0; i < m_world.Pickups().size() && state.count < kMaxDynamicBodies; ++i)
    {
        const WorldObjects::Pickup& pickup = m_world.Pickups()[i];
        if (!pickup.alive || !physics.IsValid(pickup.body))
        {
            continue;
        }
        const Transform transform = physics.GetTransform(pickup.body);
        DynamicBodyState& entry = state.bodies[state.count++];
        entry.id = static_cast<uint8_t>(i);
        entry.position = transform.position;
        entry.rotation = transform.rotation;
    }

    m_host.SendWorldState(state);
}

void PredationGame::ApplyDynamicBodies(const WorldStateMessage& state)
{
    PhysicsWorld& physics = m_app->GetPhysics();
    for (uint8_t i = 0; i < state.count; ++i)
    {
        const DynamicBodyState& entry = state.bodies[i];
        const WorldObjects::Pickup* pickup = m_world.GetPickup(entry.id);
        if (pickup == nullptr || !pickup->alive || !physics.IsValid(pickup->body))
        {
            continue;
        }
        // Placed, not simulated. A client running its own physics for a dropped rifle would
        // disagree with everyone else within a second, and there is nothing to be gained by it.
        Transform transform;
        transform.position = entry.position;
        transform.rotation = entry.rotation;
        physics.SetTransform(pickup->body, transform);
    }
}

// --- Damage ------------------------------------------------------------------------------------

glm::vec3 PredationGame::MuzzleOf(uint8_t player, const glm::vec3& eye, const glm::vec3& direction) const
{
    // Where that player's weapon is being drawn on this machine. Falls back to a point in front of
    // the eye when there is no body for them, which is better than the eye itself: a round leaving
    // somebody's face is the thing this exists to stop.
    for (const auto& avatar : m_avatars)
    {
        if (avatar->id == player && avatar->body.HasWeapon())
        {
            return avatar->body.MuzzlePoint();
        }
    }
    return eye + direction * 0.45f - glm::vec3(0.0f, 0.12f, 0.0f);
}

std::vector<NetHost::PlayerPose> PredationGame::PosesNow() const
{
    // Who is where at this instant. The host's own shots are aimed at what is on its own screen,
    // which is the present, so there is nothing to rewind.
    std::vector<NetHost::PlayerPose> poses;
    poses.push_back({LocalPlayerId(), m_player.State().position, m_player.State().stance,
                     m_player.State().alive});
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        poses.push_back({remote.id, remote.position, remote.stance, remote.alive});
    }
    return poses;
}

void PredationGame::ResolvePlayerHits(const FireEvent& shot, uint8_t shooter, ShotResult& worldHit,
                                      const std::vector<NetHost::PlayerPose>& poses)
{
    if (!cv_friendlyFire.Get())
    {
        return;
    }

    // Players are tested against directly rather than through the physics world, because a
    // character capsule is a moving query volume rather than a body a ray can find. Doing it here
    // also puts every hit decision in one place, which is where rewinding for lag will go.
    const float maxDistance = worldHit ? worldHit.distance : shot.range;
    const float radius = m_player.Config().radius;

    uint8_t bestPlayer = kMaxPlayers;
    float bestDistance = maxDistance;
    glm::vec3 bestPoint{0.0f};

    const auto testPlayer = [&](uint8_t id, const glm::vec3& feet, PlayerStance stance)
    {
        if (id == shooter)
        {
            return;
        }
        const float height = m_player.Config().HeightForStance(stance);
        // The capsule as a segment from its lower to its upper centre, which is what the character
        // controller actually is.
        const glm::vec3 low = feet + glm::vec3(0.0f, radius, 0.0f);
        const glm::vec3 high = feet + glm::vec3(0.0f, std::max(height - radius, radius), 0.0f);

        // Closest approach between the shot ray and the capsule's axis.
        const glm::vec3 axis = high - low;
        const glm::vec3 toLow = low - shot.origin;
        const float axisLengthSq = glm::dot(axis, axis);
        const float rayDotAxis = glm::dot(shot.direction, axis);
        const float rayDotToLow = glm::dot(shot.direction, toLow);
        const float axisDotToLow = glm::dot(axis, toLow);

        const float denominator = axisLengthSq - rayDotAxis * rayDotAxis;
        float alongRay = 0.0f;
        float alongAxis = 0.0f;
        if (std::abs(denominator) > 1e-5f)
        {
            alongRay = (rayDotToLow * axisLengthSq - axisDotToLow * rayDotAxis) / denominator;
            alongAxis = (alongRay * rayDotAxis - axisDotToLow) / std::max(axisLengthSq, 1e-5f);
        }
        else
        {
            alongRay = rayDotToLow;
        }
        alongRay = std::clamp(alongRay, 0.0f, bestDistance);
        alongAxis = std::clamp(alongAxis, 0.0f, 1.0f);

        const glm::vec3 onRay = shot.origin + shot.direction * alongRay;
        const glm::vec3 onAxis = low + axis * alongAxis;
        if (glm::distance(onRay, onAxis) > radius || alongRay <= 0.05f || alongRay >= bestDistance)
        {
            return;
        }
        bestPlayer = id;
        bestDistance = alongRay;
        bestPoint = onRay;
    };

    for (const NetHost::PlayerPose& pose : poses)
    {
        if (pose.alive)
        {
            testPlayer(pose.id, pose.position, pose.stance);
        }
    }

    if (bestPlayer >= kMaxPlayers)
    {
        return;
    }

    // A player in the way stops the round before whatever the world trace found.
    worldHit.hit = true;
    worldHit.position = bestPoint;
    worldHit.distance = bestDistance;
    ApplyPlayerDamage(bestPlayer, shot.damage, shooter, shot.direction);
}

void PredationGame::ApplyPlayerDamage(uint8_t player, float amount, uint8_t killer,
                                      const glm::vec3& direction)
{
    if (m_sessionMode == SessionMode::Client)
    {
        return; // clients never decide damage
    }

    float remaining = 0.0f;
    if (player == 0)
    {
        m_player.ApplyDamage(amount, "gunfire");
        remaining = m_player.State().health;
    }
    else
    {
        // A remote player's health lives on the host, in the view it publishes.
        remaining = 0.0f;
        for (const RemotePlayerView& remote : m_host.Remotes())
        {
            if (remote.id == player)
            {
                remaining = std::max(remote.health - amount, 0.0f);
                break;
            }
        }
    }

    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerDamaged;
        event.player = player;
        event.other = killer;
        event.amount = amount;
        m_host.Broadcast(event);
    }

    if (remaining <= 0.0f)
    {
        KillPlayer(player, direction);
    }
}

void PredationGame::KillPlayer(uint8_t player, const glm::vec3& direction)
{
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerDied;
        event.player = player;
        event.direction = direction * 6.0f;
        m_host.Broadcast(event);
    }
    if (player == LocalPlayerId())
    {
        m_player.State().alive = false;
        m_player.State().health = 0.0f;
        m_deathImpulse = direction * 6.0f;
    }
    PRED_LOG_INFO(Gameplay, "Player {} died", player);
}

// --- The front end ---------------------------------------------------------------------------
//
// The world is built and simulating behind the menu rather than being loaded when you press a
// button, so the menu has a moving backdrop and starting a game is instant. That also means there
// is only ever one world, which is what keeps hosting, joining and leaving from needing their own
// loading paths.

void PredationGame::EnterWorld()
{
    PRED_LOG_INFO(Gameplay, "Entering the world");
    m_screen = Screen::Playing;
    m_titleStatus.clear();
    SetCameraMode(CameraMode::FirstPerson);
    m_wantMouseCaptured = true;
}

void PredationGame::ReturnToTitle()
{
    PRED_LOG_INFO(Gameplay, "Back to the title screen");
    StopSession();
    m_screen = Screen::Title;
    m_titleStatus.clear();
    m_wantMouseCaptured = false;
    m_inventoryOpen = false;
    if (m_hidingSpot >= 0)
    {
        LeaveHidingSpot();
    }
    SetCameraMode(CameraMode::Fly);
}

void PredationGame::UpdateTitleCamera(float frameDeltaSeconds)
{
    m_titleClock += frameDeltaSeconds;

    // A slow arc around the spawn area, looking back at it. Slow enough that it reads as a held
    // shot rather than as a camera being flown.
    constexpr float kRadius = 9.0f;
    constexpr float kHeight = 2.6f;
    const float angle = m_titleClock * 0.06f;
    const glm::vec3 centre = m_spawnPoint + glm::vec3(0.0f, 1.1f, -2.0f);

    m_camera.position = centre + glm::vec3(std::sin(angle) * kRadius, kHeight, std::cos(angle) * kRadius);
    const glm::vec3 toCentre = centre - m_camera.position;
    // Yaw zero looks down -Z and increases turning right, which is what this atan2 encodes. Writing
    // it the other way round aims the camera at the mirror image of where you meant.
    m_camera.yaw = std::atan2(toCentre.x, -toCentre.z);
    m_camera.pitch = std::asin(glm::clamp(glm::normalize(toCentre).y, -1.0f, 1.0f));
}

void PredationGame::DrawTitleScreen()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size = viewport->WorkSize;

    // A dark wash over the world so the type reads whatever the camera happens to be pointing at.
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->WorkPos,
                                                 {viewport->WorkPos.x + size.x, viewport->WorkPos.y + size.y},
                                                 IM_COL32(6, 8, 10, 165));

    ImGui::SetNextWindowPos({viewport->WorkPos.x + size.x * 0.5f, viewport->WorkPos.y + size.y * 0.5f},
                            ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({420.0f, 0.0f}, ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##title", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(226, 232, 236, 255));
    ImGui::SetWindowFontScale(2.4f);
    ImGui::TextUnformatted("PROJECT PREDATION");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::TextDisabled("ACRD  //  Anomalous Containment & Research Directorate");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const ImVec2 wide{-1.0f, 34.0f};

    if (m_sessionMode == SessionMode::Client && !m_client.Connected())
    {
        // Joining takes a moment and can fail, so it gets its own state rather than dropping the
        // player into an empty world and leaving them to work out that nothing happened.
        ImGui::TextUnformatted("Joining...");
        if (m_client.Rejection() == JoinRejection::ServerFull)
        {
            ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "That game is full.");
        }
        else if (m_client.Rejection() == JoinRejection::VersionMismatch)
        {
            ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "That host is running a different version.");
        }
        if (ImGui::Button("Cancel", wide))
        {
            StopSession();
        }
        ImGui::End();
        return;
    }

    if (ImGui::Button("Play on your own", wide))
    {
        StopSession();
        EnterWorld();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Host a game");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine(86.0f);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("##hostport", &m_hostPort, 0, 0);
    m_hostPort = std::clamp(m_hostPort, 1024, 65535);
    ImGui::SameLine();
    ImGui::TextDisabled("others join on your address");
    if (ImGui::Button("Open a game", wide))
    {
        StopSession();
        NetHost::Config config;
        config.port = static_cast<uint16_t>(m_hostPort);
        auto transport = CreateUdpTransport();
        transport->SetConditions(m_simulatedConditions);
        if (m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(), m_spawnPoint))
        {
            m_sessionMode = SessionMode::Host;
            EnterWorld();
        }
        else
        {
            m_titleStatus = "Could not open port " + std::to_string(m_hostPort);
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Join a game");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Address");
    ImGui::SameLine(86.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##joinaddress", m_joinAddress, sizeof(m_joinAddress));
    ImGui::SameLine();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputInt("##joinport", &m_joinPort, 0, 0);
    m_joinPort = std::clamp(m_joinPort, 1024, 65535);
    if (ImGui::Button("Join", wide))
    {
        // Kept for next time, in the archived config, so the box opens on the last game joined.
        cv_lastAddress.Set(m_joinAddress);
        cv_lastPort.Set(m_joinPort);
        StopSession();
        auto transport = CreateUdpTransport();
        transport->SetConditions(m_simulatedConditions);
        NetClient::Config config;
        if (m_client.Connect(std::move(transport), m_joinAddress, static_cast<uint16_t>(m_joinPort),
                             "operator", config))
        {
            m_sessionMode = SessionMode::Client;
            m_titleStatus.clear();
        }
        else
        {
            m_titleStatus = std::string("Could not reach ") + m_joinAddress;
        }
    }

    if (!m_titleStatus.empty())
    {
        ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "%s", m_titleStatus.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Quit", wide))
    {
        m_app->RequestQuit();
    }
    ImGui::TextDisabled("Escape returns here from a game.");

    ImGui::End();
}

// --- Multiplayer -----------------------------------------------------------------------------
//
// The host runs the real simulation for everyone. A client predicts its own movement from local
// input and interpolates everybody else. Nothing a client sends is written into the world: the host
// runs the same movement code against its own physics and what comes out is what happened.

const std::vector<RemotePlayerView>& PredationGame::RemotePlayers() const
{
    static const std::vector<RemotePlayerView> kNone;
    switch (m_sessionMode)
    {
    case SessionMode::Host: return m_host.Remotes();
    case SessionMode::Client: return m_client.Remotes();
    case SessionMode::Offline: break;
    }
    return kNone;
}

void PredationGame::StopSession()
{
    if (m_sessionMode == SessionMode::Host)
    {
        m_host.Stop();
    }
    else if (m_sessionMode == SessionMode::Client)
    {
        m_client.Disconnect();
    }
    for (auto& avatar : m_avatars)
    {
        avatar->body.Destroy(m_scene);
    }
    m_avatars.clear();
    m_sessionMode = SessionMode::Offline;
    m_networkTick = 0;
}

bool PredationGame::StepSession(const PlayerInput& input, float dt)
{
    ++m_networkTick;

    // What is in the local player's hands, so everyone else sees it.
    const ItemDefinition* held = m_items.Get(m_inventory.Selected().item);
    const uint8_t heldId = held != nullptr ? static_cast<uint8_t>(held->id) : 0;

    if (m_sessionMode == SessionMode::Host)
    {
        m_host.SetPlayerHeld(0, heldId, m_weapon.aim > 0.5f, m_weapon.IsReloading(),
                             m_weapon.IsReloading() ? 1.0f - m_weapon.reloadRemaining : 0.0f);

        // The host is a player too: it steps itself first, then runs everyone else from what they
        // sent, then tells them all where everybody ended up.
        m_player.Step(input, dt);
        m_host.Tick(m_networkTick, m_player.State(), dt);
        ServeClientRequests();
        SendDynamicBodies();
        return true;
    }

    if (m_sessionMode == SessionMode::Client)
    {
        // The client's step happens inside prediction, so the same call is used for the first guess
        // and for every replay of it. Doing it here as well would run each input twice.
        m_client.Tick(input, m_player, dt);
        for (const WorldEventMessage& event : m_client.TakeWorldEvents())
        {
            ApplyWorldEvent(event);
        }
        if (m_client.HasWorldState())
        {
            ApplyDynamicBodies(m_client.LatestWorldState());
        }
        return true;
    }

    return false;
}

void PredationGame::SyncRemoteAvatars(float frameDeltaSeconds)
{
    const std::vector<RemotePlayerView>& remotes = RemotePlayers();

    // Anyone who has gone gets their body taken away.
    for (size_t i = 0; i < m_avatars.size();)
    {
        const uint8_t id = m_avatars[i]->id;
        const bool present = std::any_of(remotes.begin(), remotes.end(),
                                         [&](const RemotePlayerView& view) { return view.id == id; });
        if (present)
        {
            ++i;
        }
        else
        {
            m_avatars[i]->body.Destroy(m_scene);
            m_avatars.erase(m_avatars.begin() + static_cast<ptrdiff_t>(i));
        }
    }

    const PlayerConfig& config = m_player.Config();
    for (const RemotePlayerView& remote : remotes)
    {
        RemoteAvatar* avatar = nullptr;
        for (auto& candidate : m_avatars)
        {
            if (candidate->id == remote.id)
            {
                avatar = candidate.get();
                break;
            }
        }
        if (avatar == nullptr)
        {
            auto fresh = std::make_unique<RemoteAvatar>();
            fresh->id = remote.id;
            fresh->body.Build(m_scene, m_app->GetMeshes(), config);
            // Other people have heads. The body hides its own by default because in first person
            // the camera lives inside it, which is true of exactly one body on this machine.
            fresh->body.Tuning().hideHead = false;
            fresh->view.eyeHeight = config.EyeHeightForStance(remote.stance);
            fresh->built = true;
            m_avatars.push_back(std::move(fresh));
            avatar = m_avatars.back().get();
        }

        // Only where they are and what they are doing crosses the wire. The walk cycle, the lean,
        // the arms and the head all come out of the same procedural body the local player uses, run
        // here from replicated state. A gait is expensive to send and cheap to reproduce.
        avatar->state.position = remote.position;
        avatar->state.velocity = remote.velocity;
        avatar->state.yaw = remote.yaw;
        avatar->state.pitch = remote.pitch;
        avatar->state.stance = remote.stance;
        avatar->state.desiredStance = remote.stance;
        avatar->state.leanAmount = remote.leanAmount;
        avatar->state.stridePhase = remote.stridePhase;
        avatar->state.health = remote.health;
        avatar->state.alive = remote.alive;
        avatar->state.grounded = remote.grounded;

        // The eye eases towards the stance's height rather than being set to it, exactly as the
        // local player's own view does. The body is anchored to the eye, so setting it outright
        // dropped a remote player straight into a crouch in one frame while their own screen
        // showed them sinking into it.
        const float targetEye = config.EyeHeightForStance(remote.stance);
        avatar->view.eyeHeight +=
            (targetEye - avatar->view.eyeHeight) *
            (1.0f - std::exp(-config.eyeTransitionSpeed * frameDeltaSeconds));

        avatar->view.renderPosition = remote.position;
        avatar->view.eyePosition = remote.position + glm::vec3(0.0f, avatar->view.eyeHeight, 0.0f);
        avatar->view.yaw = remote.yaw;
        avatar->view.pitch = remote.pitch;
        avatar->view.leanRoll = glm::radians(config.leanAngleDegrees) * remote.leanAmount;

        // Put in their hands what the snapshot says they are holding. This was replicated and then
        // never used, which is why everybody else appeared empty-handed however obviously they were
        // carrying a rifle.
        if (avatar->heldItem != remote.heldItem)
        {
            avatar->heldItem = remote.heldItem;
            const ItemDefinition* item = m_items.Get(static_cast<ItemId>(remote.heldItem));
            const WeaponDefinition* weapon =
                item != nullptr ? m_weaponData.Get(m_weaponData.ForItem(item->key)) : nullptr;

            avatar->body.SetWeapon(m_scene, m_app->GetMeshes(), weapon);
            if (weapon == nullptr && item != nullptr)
            {
                avatar->body.SetHeldItem(m_scene, m_app->GetMeshes(), item->key,
                                         ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
            }
            else
            {
                avatar->body.ClearHeldItem(m_scene);
            }
        }

        PlayerBody::WeaponPose weaponPose;
        weaponPose.aim = remote.aiming ? 1.0f : 0.0f;
        weaponPose.reloading = remote.reloading;
        weaponPose.reload = remote.reloadProgress;
        avatar->body.SetWeaponPose(weaponPose);

        // Somebody else going down collapses the same way, from the state the host sent.
        if (!remote.alive && !avatar->collapsed)
        {
            avatar->body.Collapse(remote.velocity * 0.5f + glm::vec3(0.0f, 1.0f, 0.0f));
            avatar->collapsed = true;
        }
        else if (remote.alive && avatar->collapsed)
        {
            avatar->body.Revive();
            avatar->collapsed = false;
        }

        avatar->body.Update(m_scene, avatar->state, avatar->view, config, m_app->GetPhysics(),
                            frameDeltaSeconds);
    }
}

void PredationGame::RegisterNetCommands()
{
    Console& console = m_app->GetConsole();

    console.RegisterCommand(
        "net_host", "Start hosting on a UDP port: net_host [port]",
        [this](const std::vector<std::string>& args)
        {
            StopSession();
            NetHost::Config config;
            config.port = args.size() > 1 ? static_cast<uint16_t>(std::strtoul(args[1].c_str(), nullptr, 10))
                                          : kDefaultPort;
            auto transport = CreateUdpTransport();
            transport->SetConditions(m_simulatedConditions);
            if (!m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(),
                              m_spawnPoint))
            {
                m_app->GetConsole().PrintError("Could not open UDP port " + std::to_string(config.port));
                return;
            }
            m_sessionMode = SessionMode::Host;
            // Hosting from the console at the menu should put you in the game, the same as the
            // button does. Joining does not, because it is not a game until the host answers.
            EnterWorld();
            m_app->GetConsole().Print("Hosting on port " + std::to_string(config.port) + " for up to " +
                                      std::to_string(kMaxPlayers) + " players");
        });

    console.RegisterCommand(
        "net_join", "Join a host: net_join [address] [port]",
        [this](const std::vector<std::string>& args)
        {
            StopSession();
            const std::string address = args.size() > 1 ? args[1] : "127.0.0.1";
            const auto port = args.size() > 2
                                  ? static_cast<uint16_t>(std::strtoul(args[2].c_str(), nullptr, 10))
                                  : kDefaultPort;
            auto transport = CreateUdpTransport();
            transport->SetConditions(m_simulatedConditions);
            NetClient::Config config;
            if (!m_client.Connect(std::move(transport), address, port, "operator", config))
            {
                m_app->GetConsole().PrintError("Could not reach " + address);
                return;
            }
            m_sessionMode = SessionMode::Client;
            m_app->GetConsole().Print("Joining " + address + ":" + std::to_string(port));
        });

    console.RegisterCommand("net_leave", "Leave the session, or stop hosting",
                            [this](const std::vector<std::string>&)
                            {
                                StopSession();
                                m_app->GetConsole().Print("Back to single player");
                            });

    console.RegisterCommand(
        "net_sim", "Simulate a bad connection: net_sim <latency ms> <jitter ms> <loss %>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 4)
            {
                m_app->GetConsole().PrintError("usage: net_sim <latency ms> <jitter ms> <loss %>");
                return;
            }
            m_simulatedConditions.latencyMs = std::strtof(args[1].c_str(), nullptr);
            m_simulatedConditions.jitterMs = std::strtof(args[2].c_str(), nullptr);
            m_simulatedConditions.lossPercent = std::strtof(args[3].c_str(), nullptr);

            Transport* transport = m_sessionMode == SessionMode::Host   ? m_host.GetTransport()
                                   : m_sessionMode == SessionMode::Client ? m_client.GetTransport()
                                                                          : nullptr;
            if (transport != nullptr)
            {
                transport->SetConditions(m_simulatedConditions);
            }
            m_app->GetConsole().Print("Simulating " + args[1] + " ms latency, " + args[2] +
                                      " ms jitter, " + args[3] + "% loss");
        });

    console.RegisterCommand(
        "net_status", "Print the state of the session",
        [this](const std::vector<std::string>&)
        {
            Console& out = m_app->GetConsole();
            out.Print(std::string("Session: ") + SessionModeName(m_sessionMode));
            if (m_sessionMode == SessionMode::Host)
            {
                out.Print("Clients: " + std::to_string(m_host.ConnectedCount()));
                out.Print("Starved ticks: " + std::to_string(m_host.StarvedTicks()));
            }
            else if (m_sessionMode == SessionMode::Client)
            {
                out.Print(m_client.Connected() ? "Connected as player " + std::to_string(m_client.PlayerId())
                                               : "Still trying to join");
                out.Print("Corrections: " + std::to_string(m_client.CorrectionCount()));
            }
        });
}

void PredationGame::DrawNetworkPanel()
{
    ImGui::SetNextWindowPos(ImVec2(420.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Network"))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Session: %s", SessionModeName(m_sessionMode));

    Transport* transport = m_sessionMode == SessionMode::Host     ? m_host.GetTransport()
                           : m_sessionMode == SessionMode::Client ? m_client.GetTransport()
                                                                  : nullptr;

    if (m_sessionMode == SessionMode::Offline)
    {
        ImGui::TextWrapped("Single player. Use net_host in the console to open a game, or net_join "
                           "<address> to enter one.");
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        ImGui::Text("Clients: %d of %d", static_cast<int>(m_host.ConnectedCount()), kMaxPlayers - 1);
        ImGui::Text("Starved ticks: %u", m_host.StarvedTicks());
        ImGui::TextDisabled("A starved tick is one where a client's input had not arrived and the "
                            "host repeated the last one.");
    }
    else
    {
        ImGui::Text("%s", m_client.Connected() ? "Connected" : "Joining");
        ImGui::Text("Player %u, tick %u", m_client.PlayerId(), m_client.Sequence());
        ImGui::Text("Corrections: %u", m_client.CorrectionCount());
        const ReconciliationResult& last = m_client.LastReconciliation();
        ImGui::Text("Last error: %.3f m over %u replayed ticks", static_cast<double>(last.errorDistance),
                    last.replayedTicks);
        ImGui::Text("Hidden offset: %.3f m", static_cast<double>(glm::length(m_client.VisualOffset())));
    }

    if (transport != nullptr)
    {
        const NetStats& stats = transport->Stats();
        ImGui::Separator();
        ImGui::Text("Sent %llu packets, %llu bytes", static_cast<unsigned long long>(stats.packetsSent),
                    static_cast<unsigned long long>(stats.bytesSent));
        ImGui::Text("Received %llu packets, %llu bytes",
                    static_cast<unsigned long long>(stats.packetsReceived),
                    static_cast<unsigned long long>(stats.bytesReceived));
        ImGui::Text("Dropped %llu", static_cast<unsigned long long>(stats.packetsDropped));

        ImGui::Separator();
        ImGui::TextUnformatted("Simulated conditions");
        bool changed = ImGui::SliderFloat("Latency ms", &m_simulatedConditions.latencyMs, 0.0f, 300.0f);
        changed |= ImGui::SliderFloat("Jitter ms", &m_simulatedConditions.jitterMs, 0.0f, 150.0f);
        changed |= ImGui::SliderFloat("Loss %", &m_simulatedConditions.lossPercent, 0.0f, 50.0f);
        if (changed)
        {
            transport->SetConditions(m_simulatedConditions);
        }
    }

    ImGui::Separator();
    ImGui::Text("Other players: %d", static_cast<int>(RemotePlayers().size()));
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        ImGui::Text("  %u %s  %.1f %.1f %.1f  %s", remote.id, remote.name.c_str(),
                    static_cast<double>(remote.position.x), static_cast<double>(remote.position.y),
                    static_cast<double>(remote.position.z), PlayerStanceName(remote.stance));
    }

    ImGui::End();
}

void PredationGame::OnShutdown()
{
    StopSession();
    m_editor.Shutdown(m_scene);
    m_itemIcons.Shutdown();
    ClearProps();
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
        if (!m_wantMouseCaptured && !m_app->IsConsoleOpen() && !m_app->IsUiCapturingMouse())
        {
            m_wantMouseCaptured = true;
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
    // The menu is pointed at, so the pointer is never taken while it is up.
    const bool shouldCapture = m_wantMouseCaptured && m_windowFocused && !m_app->IsConsoleOpen() &&
                               m_screen == Screen::Playing;

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

void PredationGame::SampleLook(float /*dt*/)
{
    Input& input = m_app->GetInput();
    if (!m_mouseCaptured || m_app->IsConsoleOpen())
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
    const float sensitivity = glm::radians(cv_mouseSensitivity.Get());
    const float vertical = (cv_invertY.Get() ? delta.y : -delta.y) * sensitivity;
    const float limit = glm::radians(m_player.Config().maxPitchDegrees);

    // Holding the look button in third person orbits the camera instead of turning the character,
    // which is the only way to see the animation from the front.
    if (m_cameraMode == CameraMode::ThirdPerson && input.IsActionDown("orbit"))
    {
        m_orbitYaw += delta.x * sensitivity;
        m_orbitPitch = std::clamp(m_orbitPitch + vertical, -limit, limit);
        return;
    }

    m_lookYaw += delta.x * sensitivity;
    m_lookPitch = std::clamp(m_lookPitch + vertical, -limit, limit);
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
    result.yaw = m_lookYaw;
    result.pitch = m_lookPitch;

    if (m_app->IsConsoleOpen())
    {
        // Console open: keep looking where we are, but stop moving.
        m_jumpLatch = false;
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

const WeaponDefinition* PredationGame::EquippedWeapon() const
{
    return m_weaponData.Get(m_weapon.weapon);
}

void PredationGame::SyncEquippedWeapon()
{
    // The selected slot decides what is in your hands. Keeping it that way means there is no second
    // notion of "equipped" to fall out of step with the inventory.
    const Inventory::Slot& slot = m_inventory.Selected();
    const ItemDefinition* item = m_items.Get(slot.item);
    const WeaponId wanted = item != nullptr ? m_weaponData.ForItem(item->key) : kInvalidWeapon;

    // Anything that is not a weapon is still carried in a hand. Selecting a medical kit should put
    // the medical kit in your hand, not leave you empty-handed holding an inventory entry.
    const ItemId heldNow = (item != nullptr && wanted == kInvalidWeapon) ? item->id : kInvalidItem;
    if (heldNow != m_heldItem)
    {
        m_heldItem = heldNow;
        if (heldNow == kInvalidItem)
        {
            m_body.ClearHeldItem(m_scene);
        }
        else
        {
            m_body.SetHeldItem(m_scene, m_app->GetMeshes(), item->key,
                               ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
        }
    }

    if (wanted == m_weapon.weapon)
    {
        return;
    }
    // Anything arriving in the hands is brought up rather than appearing already held.
    m_weaponDraw = 0.0f;

    if (wanted == kInvalidWeapon)
    {
        m_weapon = WeaponState{};
        return;
    }
    if (const WeaponDefinition* definition = m_weaponData.Get(wanted))
    {
        // Rounds do not carry between weapons; each comes with its own magazine and reserve. Swapping
        // back and forth would otherwise be a free reload.
        WeaponSim::Equip(*definition, m_weapon);
        PRED_LOG_INFO(Gameplay, "Equipped {} ({} rounds, {} spare)", definition->name, m_weapon.rounds,
                      m_weapon.reserve);
    }
}

glm::vec3 PredationGame::AimDirection() const
{
    // Recoil is an offset on top of where the player is pointing, not a change to it, so it decays
    // back and hands their aim over intact. Rounds leave along the same line the camera looks down,
    // so what you see under the crosshair is what you hit.
    const float yaw = m_lookYaw + glm::radians(m_weapon.recoilYaw);
    const float pitch = std::clamp(m_lookPitch + glm::radians(m_weapon.recoilPitch),
                                   glm::radians(-89.0f), glm::radians(89.0f));
    const float cp = std::cos(pitch);
    return {std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
}

glm::vec3 PredationGame::MuzzlePosition() const
{
    const WeaponDefinition* definition = EquippedWeapon();
    const glm::vec3 eye = m_player.View().eyePosition;
    if (definition == nullptr)
    {
        return eye;
    }
    // From the eye rather than from the model's barrel. A round that starts at the visible muzzle
    // can be on the far side of a doorframe the player is peering round, so they shoot the wall
    // they can see past. Starting at the eye means what is under the crosshair is what is hit.
    return eye + AimDirection() * (definition->muzzleForward * 0.5f);
}

void PredationGame::AgeTracers(float dt)
{
    constexpr float kTracerSeconds = 0.12f;
    for (Tracer& tracer : m_tracers)
    {
        tracer.age += dt;
    }
    std::erase_if(m_tracers, [](const Tracer& tracer) { return tracer.age > kTracerSeconds; });
}

void PredationGame::ResolveShots()
{
    if (m_shots.empty())
    {
        return;
    }
    PhysicsWorld& physics = m_app->GetPhysics();
    m_weaponKick = 1.0f;

    for (const FireEvent& shot : m_shots)
    {
        if (m_sessionMode == SessionMode::Client)
        {
            // Fired for feel and sent up. The tracer is drawn straight away because a weapon that
            // waits a round trip to go off feels broken; what it actually hit is the host's answer.
            ShotMessage message;
            message.shotNumber = shot.sequence;
            message.origin = shot.origin;
            message.direction = shot.direction;
            // The moment this client was drawing everyone else at, so the host can test the shot
            // against what was actually on screen when the trigger went down.
            message.renderTick = m_client.RenderTick();
            m_client.SendShot(message);

            const ShotResult predicted = ResolveShot(physics, shot);
            Tracer tracer;
            tracer.from = shot.origin;
            tracer.to = predicted ? predicted.position : shot.origin + shot.direction * shot.range;
            tracer.hit = predicted.hit;
            m_tracers.push_back(tracer);
            continue;
        }

        ShotResult result = ResolveShot(physics, shot);
        ResolvePlayerHits(shot, LocalPlayerId(), result, PosesNow());

        Tracer tracer;
        tracer.from = shot.origin;
        tracer.to = result ? result.position : shot.origin + shot.direction * shot.range;
        tracer.hit = result.hit;
        m_tracers.push_back(tracer);

        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::ShotFired;
            event.player = 0;
            event.position = MuzzlePosition();
            event.direction = tracer.to;
            event.flag = tracer.hit;
            m_host.Broadcast(event);
        }

        if (!result)
        {
            continue;
        }
        // Nothing in the level has health yet, so for now a hit shows itself by shoving whatever it
        // struck. When creatures arrive this is where their damage is applied, and it stays on the
        // authority's side of the line.
        if (physics.IsValid(result.body))
        {
            physics.AddImpulse(result.body, shot.direction * (result.damage * 0.35f));
        }
    }
}

void PredationGame::ToggleEditor()
{
    m_editor.SetOpen(m_scene, !m_editor.IsOpen());
    if (m_editor.IsOpen())
    {
        // The editor owns the view while it is open, so the player is left standing. The mouse is
        // released because everything in the editor is done with the pointer.
        SetCameraMode(CameraMode::Fly);
        m_wantMouseCaptured = false;
        UpdateMouseCapture();
        m_app->GetConsole().Print(
            "Model editor open. Hold right mouse to look, WASD to move, F2 to close.");
    }
    else
    {
        SetCameraMode(CameraMode::FirstPerson);
        m_wantMouseCaptured = true;
        UpdateMouseCapture();
    }
}

void PredationGame::TryInteract()
{
    const InteractionSystem::Focus& focus = m_interactions.CurrentFocus();
    if (m_hidingSpot >= 0)
    {
        LeaveHidingSpot();
        return;
    }
    if (!focus.valid)
    {
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
        m_client.SendInteract(static_cast<uint8_t>(focus.kind), static_cast<uint8_t>(focus.payload));
        return;
    }

    PerformInteraction(focus.kind, focus.payload, LocalPlayerId());
}

void PredationGame::TakeAmmunition(int crateIndex)
{
    const WeaponDefinition* definition = EquippedWeapon();
    if (definition == nullptr)
    {
        m_app->GetConsole().Print("Nothing to load");
        return;
    }
    if (m_weapon.reserve >= definition->reserveOnPickup)
    {
        m_app->GetConsole().Print("Already carrying full spare magazines");
        return;
    }
    if (!m_world.DrawFromAmmoCrate(crateIndex, m_interactions))
    {
        m_app->GetConsole().Print("The crate is empty");
        return;
    }

    // Fills the spare rounds, not the magazine. What is in the weapon still has to be reloaded, so
    // resupplying never doubles as a free reload in the middle of a fight.
    const int taken = definition->reserveOnPickup - m_weapon.reserve;
    m_weapon.reserve = definition->reserveOnPickup;
    m_app->GetConsole().Print("Took " + std::to_string(taken) + " rounds for the " + definition->name);
    PRED_LOG_INFO(Gameplay, "Resupplied {} rounds from crate {}", taken, crateIndex);
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

    const int removed = m_inventory.RemoveFromSlot(m_inventory.SelectedSlot(), 1);
    if (removed <= 0)
    {
        return;
    }

    const PlayerView& view = m_player.View();
    const glm::vec3 origin = view.eyePosition + view.Forward() * 0.6f;
    const glm::vec3 throwVelocity = view.Forward() * 2.5f;

    // A client asks to put it down and waits to be told. Dropping locally made an item nobody else
    // had: it could not be picked up, the indices the two machines used stopped agreeing, and
    // dropping the same thing twice made two of it.
    if (m_sessionMode == SessionMode::Client)
    {
        DropMessage message;
        message.item = static_cast<uint16_t>(slot.item);
        message.count = static_cast<uint8_t>(removed);
        message.position = origin;
        message.velocity = throwVelocity;
        m_client.SendDrop(message);
        return;
    }

    const int index = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                          m_interactions, m_items, slot.item, removed, origin,
                                          throwVelocity);

    // Everyone else has to see it land, and it has to be there to pick up. The throw is sent too,
    // so it arcs on their screen rather than appearing on the floor.
    if (m_sessionMode == SessionMode::Host && index >= 0)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupSpawned;
        event.index = static_cast<uint8_t>(index);
        event.item = static_cast<uint16_t>(slot.item);
        event.other = static_cast<uint8_t>(removed);
        event.position = origin;
        event.direction = throwVelocity;
        m_host.Broadcast(event);
    }
}

void PredationGame::EnterHidingSpot(int index)
{
    WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(index);
    if (spot == nullptr || spot->occupied)
    {
        return;
    }

    spot->occupied = true;
    m_hidingSpot = index;
    // Attached, not teleported. A locker is barely wider than the player's capsule, so left to
    // simulate normally Jolt pushes them straight back out through the side of it.
    m_player.Attach(spot->insidePosition, spot->insideYaw);
    m_lookYaw = spot->insideYaw;
    // The door swings shut behind the player, which is most of what makes hiding feel like hiding.
    m_world.SetDoorOpen(spot->doorIndex, false, m_interactions);
    m_interactions.SetVerb(spot->entity, "Leave");
    m_app->GetConsole().Print("Hidden. Press F to leave.");
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
    const bool restrained = m_hidingSpot >= 0 || m_cameraMode == CameraMode::Fly;

    // The weapon runs before the movement, because aiming down the sights slows the player and the
    // controller needs that this tick rather than next.
    WeaponInput weaponInput;
    if (!restrained && !m_inventoryOpen)
    {
        Input& raw = m_app->GetInput();
        weaponInput.trigger = raw.IsActionDown("fire") || m_debugTriggerTicks > 0;
        weaponInput.aim = raw.IsActionDown("aim") || m_debugAim;
        if (raw.WasActionPressed("reload"))
        {
            m_reloadLatch = 30;
        }
        weaponInput.reload = m_reloadLatch > 0;
    }
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

    // In a session the host and the client each own how the local player is stepped: the host runs
    // it directly and then everyone else, the client runs it inside prediction so that the first
    // guess and every replay of it go through exactly the same code.
    if (!StepSession(input, dt))
    {
        m_player.Step(input, dt);
    }

    // The toggles follow what the body actually did. A stance change can be refused, by a ceiling
    // overhead or by the capsule being somewhere it cannot grow, and when that happened the toggle
    // still flipped: the button and the body then disagreed, so the next press asked for the stance
    // you were already in and nothing happened. That is what made crouch and prone go dead until
    // something else moved you.
    if (cv_crouchToggle.Get() && m_player.State().stanceBlocked)
    {
        m_crouchToggleState = m_player.State().stance == PlayerStance::Crouching;
        m_proneToggleState = m_player.State().stance == PlayerStance::Prone;
    }
}

void PredationGame::OnUpdate(double dt, double alpha)
{
    m_time += dt;

    Application& app = *m_app;
    Input& input = app.GetInput();
    Renderer& renderer = app.GetRenderer();
    const auto deltaSeconds = static_cast<float>(dt);

    // --- Input that is sampled per frame, not per tick ------------------------------------------
    SampleLook(deltaSeconds);

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
        m_editor.Camera().Update(input, deltaSeconds, false);
        m_editor.Update(m_scene, app.GetMeshes(), deltaSeconds);
        m_camera = m_editor.Camera();
    }

    // Nothing bound to a game key does anything at the menu. The menu is pointed at and typed into,
    // and a stray W while filling in an address must not make the character walk.
    if (!app.IsConsoleOpen() && m_screen == Screen::Playing)
    {
        if (input.WasActionPressed("jump"))
        {
            m_jumpLatch = true;
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
            TryInteract();
        }
        if (input.WasActionPressed("drop"))
        {
            DropSelected();
        }
        if (input.WasActionPressed("inventory"))
        {
            m_inventoryOpen = !m_inventoryOpen;
            // The panel is clickable, so it needs the pointer back.
            m_wantMouseCaptured = !m_inventoryOpen;
        }
        for (int slot = 0; slot < 6; ++slot)
        {
            if (input.WasActionPressed("slot_" + std::to_string(slot + 1)))
            {
                m_inventory.SelectSlot(slot);
            }
        }
        if (const float wheel = input.WheelDelta(); std::abs(wheel) > 0.1f)
        {
            m_inventory.SelectNext(wheel > 0.0f ? -1 : 1);
        }
        if (input.WasActionPressed("editor"))
        {
            ToggleEditor();
        }
        if (input.WasActionPressed("toggle_camera"))
        {
            // Cycles first person, third person, fly. Third person exists so the body animation can
            // actually be watched, which is impossible from inside the head.
            SetCameraMode(m_cameraMode == CameraMode::FirstPerson    ? CameraMode::ThirdPerson
                          : m_cameraMode == CameraMode::ThirdPerson ? CameraMode::Fly
                                                                    : CameraMode::FirstPerson);
        }
        if (input.WasActionPressed("respawn"))
        {
            m_player.Respawn(m_spawnPoint);
                                m_deathImpulse = glm::vec3(0.0f);
        }
        if (input.WasActionPressed("quit_capture"))
        {
            // Escape first frees the pointer for the debug UI, and pressing it again with the
            // pointer already free leaves the game. Toggling capture alone left no way out to the
            // menu; going straight to the menu would have taken the debug panels away from anyone
            // who only wanted the cursor back.
            if (m_wantMouseCaptured)
            {
                m_wantMouseCaptured = false;
            }
            else
            {
                ReturnToTitle();
            }
        }
    }
    UpdateMouseCapture();

    // --- Camera -----------------------------------------------------------------------------------
    // Always update the player's view, even while flying. It is presentation-only, and the body is
    // placed from its interpolated position, so skipping it would leave the body behind at the
    // origin whenever the free camera is active.
    m_player.UpdateView(deltaSeconds, static_cast<float>(alpha));

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
                                0.5f;
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
        view = m_player.View().ViewMatrix();
        viewPosition = m_player.View().eyePosition;
        break;
    }

    // What is in the player's hands has to be settled before the body is posed, not after. Deciding
    // it afterwards left a newly equipped weapon sitting at the world origin for one frame, which
    // reads as the gun flying in from the middle of the map.
    //
    // The selected slot decides what is held, so this is checked every frame rather than hooked
    // onto each of the several places a slot can change.
    SyncEquippedWeapon();
    // Inside a locker there is nowhere to hold a rifle: it is stowed rather than drawn, because a
    // metre of barrel held in front of the chest goes straight through the door.
    const WeaponDefinition* weapon = m_hidingSpot >= 0 ? nullptr : EquippedWeapon();
    m_body.SetWeapon(m_scene, app.GetMeshes(), weapon);
    if (weapon != nullptr)
    {
        PlayerBody::WeaponPose pose;
        pose.aim = m_weapon.aim;
        pose.reloading = m_weapon.IsReloading();
        // Runs 0 at the start of the reload to 1 at the end, so the animation does not have to know
        // how long any particular weapon takes.
        pose.reload = pose.reloading
                          ? 1.0f - m_weapon.reloadRemaining / std::max(weapon->reloadSeconds, 0.01f)
                          : -1.0f;
        pose.kick = m_weaponKick;
        pose.draw = m_weaponDraw;
        m_body.SetWeaponPose(pose);
    }

    // The kick is presentation, so it decays per frame rather than per tick. Firing sets it to one
    // in ResolveShots, which is the only place that knows a round actually left the barrel.
    m_weaponKick = std::max(m_weaponKick - deltaSeconds * 7.0f, 0.0f);
    // Drawing a weapon runs 0 to 1 over its own moment, so swapping is a movement rather than a
    // substitution.
    m_weaponDraw = std::min(m_weaponDraw + deltaSeconds * 3.2f, 1.0f);

    // The body follows the simulation every frame. Its head is only drawn from the fly camera,
    // because in first person the camera sits inside it.
    m_body.Tuning().hideHead = m_cameraMode == CameraMode::FirstPerson && m_player.State().alive;

    // Death hands the body over to the ragdoll, once. Everything below it is animation, and that is
    // exactly what stops.
    if (!m_player.State().alive && !m_localCollapsed)
    {
        m_body.Collapse(m_deathImpulse);
        m_localCollapsed = true;
    }
    else if (m_player.State().alive && m_localCollapsed)
    {
        m_body.Revive();
        m_localCollapsed = false;
    }

    m_body.Update(m_scene, m_player.State(), m_player.View(), m_player.Config(), app.GetPhysics(),
                  deltaSeconds);

    // Everyone else, driven the same way from replicated state.
    if (m_sessionMode != SessionMode::Offline)
    {
        m_client.UpdateInterpolation(deltaSeconds);
        SyncRemoteAvatars(deltaSeconds);
    }

    // A join finishes when the host answers, which can be a moment after the button was pressed.
    if (m_screen == Screen::Title && m_sessionMode == SessionMode::Client && m_client.Connected())
    {
        EnterWorld();
    }

    const float aspect = renderer.Height() > 0
                             ? static_cast<float>(renderer.Width()) / static_cast<float>(renderer.Height())
                             : 16.0f / 9.0f;
    const float horizontal = glm::radians(cv_fov.Get());
    const float verticalFov = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);
    const glm::mat4 projection = renderer.HomogeneousDepth()
                                     ? glm::perspectiveRH_NO(verticalFov, aspect, 0.05f, 500.0f)
                                     : glm::perspectiveRH_ZO(verticalFov, aspect, 0.05f, 500.0f);
    renderer.SetCamera(view, projection);
    (void)viewPosition;

    // --- World ------------------------------------------------------------------------------------
    Environment& environment = m_scene.GetEnvironment();
    environment.fogStart = cv_fogStart.Get();
    environment.fogEnd = cv_fogEnd.Get();
    environment.sunIntensity = cv_sunIntensity.Get();
    renderer.SetClearColor(0x11131aff);

    // What the player can reach, decided by where they are looking rather than by proximity.
    if (m_hidingSpot >= 0)
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
    SyncDynamicProps();
    app.GetSceneRenderer().SetWireframe(cv_wireframe.Get());
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
    const int index = static_cast<int>(m_props.size());
    constexpr float spread = 0.9f;
    const glm::vec3 jitter{static_cast<float>(index % 3 - 1) * spread,
                           static_cast<float>(index / 9) * spread,
                           static_cast<float>((index / 3) % 3 - 1) * spread};
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

    const glm::vec3 viewPosition = m_cameraMode == CameraMode::Fly ? m_camera.position : m_player.View().eyePosition;
    app.GetSceneRenderer().Draw(Renderer::kViewMain, m_scene, app.GetMeshes(), viewPosition);
    DrawDebugOverlays();
}

void PredationGame::DrawDebugOverlays()
{
    DebugDraw& draw = m_app->GetDebugDraw();

    if (m_editor.IsOpen())
    {
        m_editor.DrawOverlays(draw);
        return;
    }

    // Tracers are always drawn, not gated behind a debug category: without a muzzle flash or a
    // projectile model yet, this is the only thing that shows a round actually left the barrel.
    for (const Tracer& tracer : m_tracers)
    {
        draw.Line(tracer.from, tracer.to, tracer.hit ? Color::kYellow : Color::kWhite);
        if (tracer.hit)
        {
            draw.Sphere(tracer.to, 0.045f, Color::kRed, 8);
        }
    }

    if (cv_showGrid.Get())
    {
        draw.Grid(24.0f, 1.0f, 0.02f);
        draw.Axes(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.02f, 0.0f)), 1.5f);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Physics))
    {
        m_app->GetPhysics().DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Player) && m_cameraMode != CameraMode::FirstPerson)
    {
        // Only worth drawing the capsule when it is not wrapped around the camera.
        m_player.DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Animation))
    {
        m_body.DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Rendering))
    {
        const MeshLibrary& meshes = m_app->GetMeshes();
        m_scene.ForEachMeshRenderer(
            [&](Entity, const Transform& transform, const MeshRenderer& renderer)
            {
                const Mesh* mesh = meshes.Get(renderer.mesh);
                if (mesh == nullptr || !mesh->bounds.IsValid())
                {
                    return;
                }
                // Drawn in the entity's own frame. Building an axis-aligned box from the position
                // and scale alone ignored the rotation, so anything turned showed a bounds box
                // sitting at an angle to the collider it was supposed to describe.
                const glm::mat4 model =
                    transform.Matrix() * glm::translate(glm::mat4(1.0f), mesh->bounds.Center());
                draw.BoxOriented(model, mesh->bounds.HalfExtents(), Color::kCyan);
            });
    }
}

void PredationGame::DrawPlayerPanel()
{
    PlayerConfig& config = m_player.Config();
    const PlayerState& state = m_player.State();
    const PlayerController::Debug& diagnostics = m_player.Diagnostics();

    ImGui::Text("Stance %-9s %s", PlayerStanceName(state.stance),
                state.stanceBlocked ? "(blocked overhead)" : "");
    ImGui::Text("Speed %5.2f / %5.2f m/s   %s", diagnostics.horizontalSpeed, diagnostics.targetSpeed,
                state.grounded ? "grounded" : (state.onSteepSlope ? "on steep slope" : "airborne"));
    ImGui::Text("Position %.2f %.2f %.2f", state.position.x, state.position.y, state.position.z);
    ImGui::Text("Vertical %6.2f m/s   slope %4.1f deg   step %+.3f m", state.velocity.y,
                diagnostics.slopeAngleDegrees, diagnostics.lastStepUp);
    ImGui::Text("Health %.0f %s", state.health, state.alive ? "" : "(dead)");
    ImGui::TextUnformatted(m_mouseCaptured ? "Mouse captured. Escape frees the cursor."
                                           : "Cursor free. Escape or click the world to look again.");
    if (state.landingImpactSpeed > 0.0f)
    {
        ImGui::Text("Last landing impact %.1f m/s", state.landingImpactSpeed);
    }

    ImGui::Separator();
    if (ImGui::Button("Respawn"))
    {
        m_player.Respawn(m_spawnPoint);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to player.json"))
    {
        config.SaveToFile(PlayerConfigPath());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
    {
        ReloadPlayerConfig();
    }

    bool capsuleChanged = false;

    if (ImGui::CollapsingHeader("Speeds", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Move", &config.moveSpeed, 0.5f, 10.0f, "%.2f m/s");
        ImGui::SliderFloat("Sprint", &config.sprintSpeed, 0.5f, 14.0f, "%.2f m/s");
        ImGui::SliderFloat("Slow walk", &config.walkSpeed, 0.2f, 6.0f, "%.2f m/s");
        ImGui::SliderFloat("Crouch", &config.crouchSpeed, 0.2f, 6.0f, "%.2f m/s");
        ImGui::SliderFloat("Prone", &config.proneSpeed, 0.1f, 3.0f, "%.2f m/s");
        ImGui::SliderFloat("Backward scale", &config.backwardScale, 0.3f, 1.0f, "%.2f");
        ImGui::SliderFloat("Strafe scale", &config.strafeScale, 0.3f, 1.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Acceleration", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Ground accel", &config.groundAcceleration, 5.0f, 200.0f, "%.0f m/s2");
        ImGui::SliderFloat("Ground decel", &config.groundDeceleration, 5.0f, 200.0f, "%.0f m/s2");
        ImGui::SliderFloat("Air accel", &config.airAcceleration, 0.0f, 60.0f, "%.1f m/s2");
    }

    if (ImGui::CollapsingHeader("Jump and gravity", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Jump height", &config.jumpHeight, 0.1f, 2.5f, "%.2f m");
        ImGui::SliderFloat("Gravity scale", &config.gravityScale, 0.5f, 4.0f, "%.2f");
        ImGui::SliderFloat("Fall gravity scale", &config.fallGravityScale, 0.5f, 5.0f, "%.2f");
        ImGui::SliderFloat("Coyote time", &config.coyoteTime, 0.0f, 0.4f, "%.3f s");
        ImGui::SliderFloat("Jump buffer", &config.jumpBufferTime, 0.0f, 0.4f, "%.3f s");
    }

    if (ImGui::CollapsingHeader("Capsule"))
    {
        capsuleChanged |= ImGui::SliderFloat("Stand height", &config.standHeight, 1.0f, 2.4f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Crouch height", &config.crouchHeight, 0.6f, 1.6f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Prone height", &config.proneHeight, 0.3f, 1.0f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Radius", &config.radius, 0.15f, 0.6f, "%.2f m");
        ImGui::SliderFloat("Step height", &config.stepHeight, 0.05f, 0.8f, "%.2f m");
        ImGui::SliderFloat("Max slope", &config.maxSlopeAngle, 20.0f, 70.0f, "%.0f deg");
        ImGui::TextUnformatted("Step height and slope apply on respawn.");
    }

    if (ImGui::CollapsingHeader("Camera feel", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Stand eye", &config.standEyeHeight, 1.0f, 2.0f, "%.2f m");
        ImGui::SliderFloat("Crouch eye", &config.crouchEyeHeight, 0.5f, 1.5f, "%.2f m");
        ImGui::SliderFloat("Prone eye", &config.proneEyeHeight, 0.1f, 1.0f, "%.2f m");
        ImGui::SliderFloat("Eye transition", &config.eyeTransitionSpeed, 1.0f, 30.0f, "%.1f");
        ImGui::SliderFloat("Step smoothing", &config.stepSmoothSpeed, 1.0f, 40.0f, "%.1f");
        ImGui::SliderFloat("Landing dip", &config.landingDipPerSpeed, 0.0f, 0.05f, "%.4f m per m/s");
        ImGui::SliderFloat("Landing dip max", &config.landingDipMax, 0.0f, 0.6f, "%.2f m");
        ImGui::SliderFloat("Landing recovery", &config.landingRecoverSpeed, 1.0f, 25.0f, "%.1f");
        ImGui::SliderFloat("Footfall dip", &config.bobAmount, 0.0f, 0.16f, "%.3f m");
        ImGui::SliderFloat("Walk lower", &config.bobWalkLower, 0.0f, 0.16f, "%.3f m");
        ImGui::SliderFloat("Dip at speed", &config.bobSprintScale, 1.0f, 3.0f, "%.2f x");
        ImGui::SliderFloat("Max pitch", &config.maxPitchDegrees, 45.0f, 89.9f, "%.1f deg");
    }

    if (ImGui::CollapsingHeader("Fall damage"))
    {
        ImGui::Checkbox("Enabled", &config.fallDamageEnabled);
        ImGui::SliderFloat("Free below", &config.fallDamageMinSpeed, 1.0f, 25.0f, "%.1f m/s");
        ImGui::SliderFloat("Lethal at", &config.fallDamageLethalSpeed, 5.0f, 60.0f, "%.1f m/s");
    }

    if (ImGui::CollapsingHeader("Body and gait", ImGuiTreeNodeFlags_DefaultOpen))
    {
        PlayerBody::Config& body = m_body.Tuning();
        bool visible = body.visible;
        if (ImGui::Checkbox("Show body", &visible))
        {
            m_body.SetVisible(m_scene, visible);
        }
        ImGui::SliderFloat("Stride base", &config.strideLengthBase, 0.4f, 2.0f, "%.2f m");
        ImGui::SliderFloat("Stride per m/s", &config.strideLengthPerSpeed, 0.0f, 0.6f, "%.3f m");
        ImGui::SliderFloat("Stance walk", &config.stanceFractionWalk, 0.4f, 0.85f, "%.2f");
        ImGui::SliderFloat("Stance run", &config.stanceFractionRun, 0.3f, 0.7f, "%.2f");
        ImGui::SliderFloat("Step height", &body.stepHeight, 0.0f, 0.4f, "%.3f m");
        ImGui::SliderFloat("Step reach", &body.stepReachMargin, 0.7f, 0.99f, "%.2f");
        ImGui::SliderFloat("Hip sway", &body.hipSwayAmount, 0.0f, 0.12f, "%.3f m");
        ImGui::SliderFloat("Hip bob", &body.hipBobAmount, 0.0f, 0.12f, "%.3f m");
        ImGui::SliderFloat("Lean per m/s", &body.leanPerSpeed, 0.0f, 6.0f, "%.2f deg");
        ImGui::SliderFloat("Max lean", &body.maxLean, 0.0f, 40.0f, "%.0f deg");
        ImGui::SliderFloat("Arm swing", &body.armSwingDegrees, 0.0f, 60.0f, "%.0f deg");
        ImGui::SliderFloat("Foot smoothing", &body.footPlantSmoothing, 2.0f, 60.0f, "%.0f");
        ImGui::TextUnformatted("Skeleton overlay: enable the ANIMATION debug category.");
    }

    if (capsuleChanged)
    {
        m_player.ApplyConfigToCharacter();
    }
}

void PredationGame::DrawHud()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f};
    constexpr ImGuiWindowFlags kHudFlags = ImGuiWindowFlags_NoDecoration |
                                           ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;

    // Reticle. The gap opens with the weapon's current cone, so the crosshair says where rounds can
    // actually go rather than always promising the centre of the screen.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 reticleColor = IM_COL32(230, 230, 235, 150);
    float gap = 2.0f;
    if (const WeaponDefinition* weapon = EquippedWeapon())
    {
        // Half the vertical field of view maps to half the screen height, so a cone in degrees
        // converts to pixels through the same projection the world is drawn with.
        const float halfFov = glm::radians(cv_fov.Get()) * 0.5f;
        const float aspect = viewport->Size.x / std::max(viewport->Size.y, 1.0f);
        const float verticalHalfFov = std::atan(std::tan(halfFov) / std::max(aspect, 0.01f));
        const float spread = glm::radians(WeaponSim::CurrentSpread(*weapon, m_weapon));
        gap = 2.0f + std::tan(spread) / std::max(std::tan(verticalHalfFov), 1e-3f) * viewport->Size.y * 0.5f;
        gap = std::min(gap, viewport->Size.y * 0.25f);
    }
    const float tick = 5.0f;
    draw->AddLine({centre.x - gap - tick, centre.y}, {centre.x - gap, centre.y}, reticleColor, 1.5f);
    draw->AddLine({centre.x + gap, centre.y}, {centre.x + gap + tick, centre.y}, reticleColor, 1.5f);
    draw->AddLine({centre.x, centre.y - gap - tick}, {centre.x, centre.y - gap}, reticleColor, 1.5f);
    draw->AddLine({centre.x, centre.y + gap}, {centre.x, centre.y + gap + tick}, reticleColor, 1.5f);
    // Interaction prompt, just below the reticle.
    const InteractionSystem::Focus& focus = m_interactions.CurrentFocus();
    const std::string prompt = m_hidingSpot >= 0 ? std::string("Leave Locker") : focus.prompt;
    if (!prompt.empty())
    {
        const std::string line = "[F]  " + prompt;
        ImGui::SetNextWindowPos({centre.x, centre.y + 42.0f}, ImGuiCond_Always, {0.5f, 0.0f});
        if (ImGui::Begin("##Prompt", nullptr, kHudFlags))
        {
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::End();
    }

    // Hotbar: one icon per slot, with the selected one picked out and stack counts in the corner.
    constexpr float kSlotSize = 46.0f;
    constexpr float kSlotGap = 6.0f;

    ImGui::SetNextWindowPos({centre.x, viewport->Pos.y + viewport->Size.y - 16.0f}, ImGuiCond_Always,
                            {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##Hotbar", nullptr, kHudFlags))
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        for (int i = 0; i < m_inventory.SlotCount(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0f, kSlotGap);
            }

            const Inventory::Slot& slot = m_inventory.At(i);
            const bool selected = i == m_inventory.SelectedSlot();
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            list->AddRectFilled(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                                IM_COL32(18, 20, 26, selected ? 190 : 130), 4.0f);
            list->AddRect(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                          selected ? IM_COL32(235, 205, 130, 235) : IM_COL32(120, 124, 134, 150), 4.0f,
                          0, selected ? 2.0f : 1.0f);

            if (const ItemDefinition* definition = m_items.Get(slot.item); definition != nullptr)
            {
                DrawItemIcon(slot.item, kSlotSize);
                if (slot.count > 1)
                {
                    const std::string count = std::to_string(slot.count);
                    list->AddText({origin.x + kSlotSize - 6.0f - ImGui::CalcTextSize(count.c_str()).x,
                                   origin.y + kSlotSize - 17.0f},
                                  IM_COL32(240, 240, 245, 235), count.c_str());
                }
            }

            const std::string number = std::to_string(i + 1);
            list->AddText({origin.x + 4.0f, origin.y + 2.0f},
                          selected ? IM_COL32(235, 205, 130, 220) : IM_COL32(150, 154, 164, 170),
                          number.c_str());

            ImGui::Dummy({kSlotSize, kSlotSize});
        }
    }
    ImGui::End();

    // Ammunition, bottom right, away from the hotbar. Reads magazine over reserve, the way a
    // shooter always has, and says so plainly while the magazine is out.
    if (const WeaponDefinition* weapon = EquippedWeapon())
    {
        ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x - 24.0f,
                                 viewport->Pos.y + viewport->Size.y - 20.0f},
                                ImGuiCond_Always, {1.0f, 1.0f});
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("##Ammo", nullptr, kHudFlags))
        {
            if (m_weapon.IsReloading())
            {
                ImGui::TextColored({0.85f, 0.80f, 0.55f, 1.0f}, "Reloading");
            }
            else
            {
                const ImVec4 colour = m_weapon.rounds == 0 ? ImVec4(0.88f, 0.42f, 0.38f, 1.0f)
                                                           : ImVec4(0.90f, 0.91f, 0.94f, 1.0f);
                ImGui::TextColored(colour, "%d / %d", m_weapon.rounds, m_weapon.reserve);
            }
            ImGui::TextDisabled("%s  %s", weapon->name.c_str(), FireModeName(weapon->mode));
        }
        ImGui::End();
    }

    if (m_inventoryOpen)
    {
        DrawInventoryPanel();
    }
}

void PredationGame::DrawItemIcon(ItemId item, float boxSize) const
{
    const ItemIcons::Icon* icon = m_itemIcons.Find(item);
    if (icon == nullptr || !m_itemIcons.IsReady())
    {
        return;
    }

    // Inset a little, so the render's own framing does not touch the slot border.
    const float inset = boxSize * 0.06f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(ImGuiLayer::TextureId(m_itemIcons.Texture())),
                                        {origin.x + inset, origin.y + inset},
                                        {origin.x + boxSize - inset, origin.y + boxSize - inset},
                                        {icon->uv0.x, icon->uv0.y}, {icon->uv1.x, icon->uv1.y});
}

void PredationGame::DrawInventoryPanel()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f,
                             viewport->Pos.y + viewport->Size.y * 0.5f},
                            ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("Inventory", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        constexpr float kSlotSize = 76.0f;
        constexpr int kColumns = 3;

        for (int i = 0; i < m_inventory.SlotCount(); ++i)
        {
            if (i % kColumns != 0)
            {
                ImGui::SameLine(0.0f, 10.0f);
            }
            ImGui::BeginGroup();

            const Inventory::Slot& slot = m_inventory.At(i);
            const bool selected = i == m_inventory.SelectedSlot();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* list = ImGui::GetWindowDrawList();

            list->AddRectFilled(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                                IM_COL32(26, 28, 36, 220), 4.0f);
            list->AddRect(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                          selected ? IM_COL32(235, 205, 130, 235) : IM_COL32(110, 114, 124, 160), 4.0f,
                          0, selected ? 2.0f : 1.0f);

            const ItemDefinition* definition = m_items.Get(slot.item);
            if (definition != nullptr)
            {
                DrawItemIcon(slot.item, kSlotSize);
                // The count rides on the icon, as it does on the hotbar, so the caption below only
                // ever has to hold a name.
                if (slot.count > 1)
                {
                    const std::string count = std::to_string(slot.count);
                    list->AddText({origin.x + kSlotSize - 6.0f - ImGui::CalcTextSize(count.c_str()).x,
                                   origin.y + kSlotSize - 17.0f},
                                  IM_COL32(240, 240, 245, 235), count.c_str());
                }
            }

            // An invisible button over the slot makes it clickable for selecting.
            ImGui::InvisibleButton(("##slot" + std::to_string(i)).c_str(), {kSlotSize, kSlotSize});
            if (ImGui::IsItemClicked())
            {
                m_inventory.SelectSlot(i);
            }
            if (definition != nullptr && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", definition->name.c_str());
            }

            // One line, clipped to the slot. Wrapping split words across lines and made a tidy grid
            // look broken.
            const std::string label = definition != nullptr ? definition->name : std::string("Empty");
            const ImVec2 caption = ImGui::GetCursorScreenPos();
            list->PushClipRect(caption, {caption.x + kSlotSize, caption.y + ImGui::GetTextLineHeight()}, true);
            list->AddText(caption,
                          definition != nullptr ? IM_COL32(220, 222, 230, 255) : IM_COL32(115, 118, 128, 255),
                          label.c_str());
            list->PopClipRect();
            ImGui::Dummy({kSlotSize, ImGui::GetTextLineHeight()});

            ImGui::EndGroup();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Tab closes.  G drops the selected item.");
    }
    ImGui::End();
}

void PredationGame::OnImGui()
{
    // The editor is a tool, not part of the game's own interface, so it is drawn whether or not the
    // debug overlay is showing and it takes the screen to itself.
    if (m_editor.IsOpen())
    {
        m_editor.DrawUi(m_scene, m_app->GetMeshes());
        return;
    }

    if (m_screen == Screen::Title)
    {
        DrawTitleScreen();
        return;
    }

    // The HUD is part of the game, not the debug overlay, so it is always drawn.
    if (m_cameraMode != CameraMode::Fly)
    {
        DrawHud();
    }

    if (!m_app->IsOverlayVisible())
    {
        return;
    }

    DrawNetworkPanel();

    ImGui::SetNextWindowPos(ImVec2(8.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Player"))
    {
        if (m_cameraMode == CameraMode::Fly)
        {
            ImGui::TextUnformatted("Free camera. P cycles first person, third person and this.");
            ImGui::Text("Camera %.2f %.2f %.2f", m_camera.position.x, m_camera.position.y,
                        m_camera.position.z);
            ImGui::Separator();
        }
        DrawPlayerPanel();
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(420.0f, 470.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("World", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const SceneRenderer::Stats& stats = m_app->GetSceneRenderer().LastStats();
        ImGui::Text("Entities %zu   submitted %zu   triangles %zu", m_scene.EntityCount(),
                    stats.meshesSubmitted, stats.trianglesSubmitted);
        ImGui::Text("Props %zu", m_props.size());

        float fov = cv_fov.Get();
        if (ImGui::SliderFloat("FOV (horizontal)", &fov, 60.0f, 120.0f, "%.0f"))
        {
            cv_fov.Set(fov);
        }
        float sensitivity = cv_mouseSensitivity.Get();
        if (ImGui::SliderFloat("Sensitivity", &sensitivity, 0.02f, 0.5f, "%.3f"))
        {
            cv_mouseSensitivity.Set(sensitivity);
        }
        bool invert = cv_invertY.Get();
        if (ImGui::Checkbox("Invert Y", &invert))
        {
            cv_invertY.Set(invert);
        }
        bool crouchToggle = cv_crouchToggle.Get();
        if (ImGui::Checkbox("Crouch toggles", &crouchToggle))
        {
            cv_crouchToggle.Set(crouchToggle);
        }
        bool sprintToggle = cv_sprintToggle.Get();
        if (ImGui::Checkbox("Sprint toggles", &sprintToggle))
        {
            cv_sprintToggle.Set(sprintToggle);
        }

        ImGui::Separator();
        Environment& environment = m_scene.GetEnvironment();
        float sunIntensity = cv_sunIntensity.Get();
        if (ImGui::SliderFloat("Sun intensity", &sunIntensity, 0.0f, 8.0f, "%.2f"))
        {
            cv_sunIntensity.Set(sunIntensity);
        }
        ImGui::ColorEdit3("Fog colour", &environment.fogColor.r);
        float fogStart = cv_fogStart.Get();
        if (ImGui::SliderFloat("Fog start", &fogStart, 0.0f, 100.0f, "%.1f m"))
        {
            cv_fogStart.Set(fogStart);
        }
        float fogEnd = cv_fogEnd.Get();
        if (ImGui::SliderFloat("Fog end", &fogEnd, 1.0f, 400.0f, "%.1f m"))
        {
            cv_fogEnd.Set(fogEnd);
        }

        bool grid = cv_showGrid.Get();
        if (ImGui::Checkbox("Reference grid", &grid))
        {
            cv_showGrid.Set(grid);
        }
        ImGui::SameLine();
        bool wireframe = cv_wireframe.Get();
        if (ImGui::Checkbox("Wireframe", &wireframe))
        {
            cv_wireframe.Set(wireframe);
        }
    }
    ImGui::End();
}

} // namespace pred
