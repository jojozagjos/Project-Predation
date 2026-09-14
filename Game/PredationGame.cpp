#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Assets/GltfImport.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/World/TestMap.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
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
CVar<std::string> cv_playerName{"net.name", "operator", "What other players see you called",
                                CVarFlags::Archive};
// On by default: a four-player extraction game where rounds pass through your team is a different
// game, and a quieter one.
CVar<bool> cv_friendlyFire{"game.friendly_fire", true, "Rounds hurt other players", CVarFlags::Archive};
CVar<float> cv_migrationSeconds{"net.migration_seconds", 2.5f,
                              "How long to wait after losing the host before taking over",
                              CVarFlags::Archive};
CVar<float> cv_respawnSeconds{"game.respawn_seconds", 6.0f, "How long you lie there before coming back",
                             CVarFlags::Archive};
CVar<bool> cv_showGrid{"debug.show_grid", false, "Draw the reference grid"};
CVar<bool> cv_wireframe{"r.wireframe", false, "Draw scene meshes as wireframe"};
CVar<float> cv_fogStart{"r.fog_start", 12.0f, "Fog start distance in meters"};
CVar<float> cv_fogEnd{"r.fog_end", 90.0f, "Fog end distance in meters"};
CVar<float> cv_sunIntensity{"r.sun_intensity", 2.2f, "Directional light intensity"};

// How a round is drawn. It travels rather than appearing as a whole lit line, because a line from
// the muzzle to the wall is a diagram of a shot rather than a shot. Fast enough to be over almost
// at once, slow enough that the eye catches the direction it went.
constexpr float kTracerSpeed = 260.0f;   // metres a second
constexpr float kTracerLength = 2.2f;    // how much of it is lit at any moment
constexpr float kSparkSeconds = 0.18f;   // how long the mark at the far end lasts
// Long enough for the longest shot to arrive and its impact to fade.
constexpr float kTracerSeconds = 0.75f;

// The wire carries "no particular state" as a sentinel, because nine bits cannot hold a negative.
int LoadFromWire(uint16_t value)
{
    return value >= kDefaultLoad ? -1 : static_cast<int>(value);
}

uint16_t LoadToWire(int value)
{
    return value < 0 ? kDefaultLoad : static_cast<uint16_t>(std::min(value, kDefaultLoad - 1));
}

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
    m_body.SetTextureLibrary(app.GetTextures());

    m_items.LoadFromFile(Paths::AssetsRoot() / "Data" / "items.json");
    // Weapons load before the icons, because a weapon item draws its icon from the weapon's own
    // model and would otherwise fall back to the placeholder block.
    m_weaponData.LoadFromFile(Paths::AssetsRoot() / "Data" / "weapons.json");
    m_itemIcons.Build(m_items, app.GetMeshes(), app.GetRenderer(), &m_weaponData);
    m_world.SetTextures(app.GetTextures());
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
    std::snprintf(m_playerName, sizeof(m_playerName), "%s", cv_playerName.Get().c_str());
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
                            [this](const std::vector<std::string>&) { RespawnLocalPlayer(m_spawnPoint); });

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

#if PRED_DEV_TOOLS
    console.RegisterCommand(
        "editor", "Open or close the model and animation editor: editor [model name]",
        [this](const std::vector<std::string>& args)
        {
            EnterEditor(args.size() >= 2 ? args[1] : std::string());
        },
        "editor [model]");

    console.RegisterCommand(
        "bench_weapon",
        "Point a weapon at the model open in the editor: bench_weapon <key>",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("usage: bench_weapon <key>");
                return;
            }
            AssignModelToWeapon(m_weaponData.IdOf(args[1]));
        });

    console.RegisterCommand(
        "hold_report",
        "Print where the weapon sits relative to the eye, in the view's own frame: hold_report [frames]",
        [this](const std::vector<std::string>& args)
        {
            // Optionally after a wait. Commands from --exec all run before the first frame, when
            // nothing has been equipped and no body has been posed, so a report taken then is a
            // report about an empty hand.
            if (args.size() >= 2)
            {
                m_holdReportIn = std::max(std::atoi(args[1].c_str()), 1);
                return;
            }
            ReportHold();
        });

    console.RegisterCommand("solo", "Leave the title screen and start a game on your own",
                            [this](const std::vector<std::string>&)
                            {
                                if (m_screen == Screen::Title)
                                {
                                    StopSession();
                                    EnterWorld();
                                }
                            });

    console.RegisterCommand(
        "model_import",
        "Import a glTF binary as an editable model: model_import <file.glb> <name> [size] [turn x y z]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 3)
            {
                m_app->GetConsole().PrintError(
                    "usage: model_import <file.glb> <name> [size] [turn x y z]");
                return;
            }
            GltfImportOptions options;
            // Images inside the file are written out beside the model, because a model file is meant
            // to stay something a person can open and a base colour image is two megabytes of it.
            options.textureDirectory = ModelDirectory() / "Textures";
            options.texturePrefix = args[2];
            if (args.size() >= 4)
            {
                options.targetSize = static_cast<float>(std::atof(args[3].c_str()));
            }
            if (args.size() >= 7)
            {
                options.rotationDegrees = {static_cast<float>(std::atof(args[4].c_str())),
                                           static_cast<float>(std::atof(args[5].c_str())),
                                           static_cast<float>(std::atof(args[6].c_str()))};
            }
            ModelAsset imported;
            std::string error;
            if (!LoadGlbModel(args[1], options, imported, &error))
            {
                m_app->GetConsole().PrintError("import failed: " + error);
                return;
            }
            imported.name = args[2];
            if (!imported.SaveToFile(ModelDirectory() / (args[2] + ".json")))
            {
                m_app->GetConsole().PrintError("could not write the model");
                return;
            }
            ForgetWeaponModels();
            m_app->GetConsole().Print("Imported " + std::to_string(imported.parts.size()) +
                                      " parts as " + args[2] +
                                      ". Open it with: editor " + args[2]);
        },
        "model_import <file.glb> <name> [size] [turn x y z]");

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
#endif // PRED_DEV_TOOLS

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
        // The host has the eye this was traced from, because the client sent it.
        tracer.from = event.position;
        tracer.origin = shot.origin;
        tracer.to = event.direction;
        tracer.hit = event.flag;
        m_tracers.push_back(tracer);

        // The host draws the shooter too, so their weapon has to kick here as well. The event goes
        // out to everybody else; nobody sends it back to the machine that made it.
        if (RemoteAvatar* avatar = AvatarFor(request.player))
        {
            avatar->weaponKick = 1.0f;
        }
    }

    for (const NetHost::DropRequest& request : m_host.TakeDropRequests())
    {
        // Only what the host actually handed them. Otherwise dropping is a way to make items out of
        // nothing, which is what let one get duplicated.
        if (!m_host.TakeCarried(request.player, request.drop.item, request.drop.count))
        {
            continue;
        }
        const int index = m_world.SpawnPickup(
            m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
            static_cast<ItemId>(request.drop.item), request.drop.count, request.drop.position,
            request.drop.velocity, LoadFromWire(request.drop.rounds),
            LoadFromWire(request.drop.reserve));
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

    // Whatever somebody walked out with goes back on the floor where they were standing. It came
    // out of the world and it belongs to the world: a player who quits holding the keycard
    // otherwise takes it with them, and everybody else is left looking at an item hanging in the
    // air where their body used to be.
    for (const NetHost::Departure& departure : m_host.TakeDeparted())
    {
        for (const auto& [item, count] : departure.carried)
        {
            const int index = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                                  m_interactions, m_items, static_cast<ItemId>(item),
                                                  count, departure.position + glm::vec3(0.0f, 0.4f, 0.0f),
                                                  glm::vec3(0.0f));
            if (index < 0)
            {
                continue;
            }
            WorldEventMessage event;
            event.kind = WorldEventKind::PickupSpawned;
            event.index = static_cast<uint8_t>(index);
            event.item = item;
            event.other = static_cast<uint8_t>(count);
            event.position = departure.position + glm::vec3(0.0f, 0.4f, 0.0f);
            m_host.Broadcast(event);
        }

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

    // Every slot, in both directions. Telling them only what has been taken leaves out everything
    // that has been dropped since, and leaves a slot that has been taken and then filled with
    // something else showing whatever the map originally put there. Taking first and then spawning
    // is also how a slot gets corrected: a spawn at an occupied index replaces what is there.
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

    for (size_t i = 0; i < m_world.Pickups().size(); ++i)
    {
        const WorldObjects::Pickup& pickup = m_world.Pickups()[i];
        if (!pickup.alive)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupSpawned;
        event.index = static_cast<uint8_t>(i);
        event.item = static_cast<uint16_t>(pickup.item);
        event.other = static_cast<uint8_t>(pickup.count);
        event.rounds = LoadToWire(pickup.rounds);
        event.reserve = LoadToWire(pickup.reserve);
        event.position = m_app->GetPhysics().IsValid(pickup.body)
                             ? m_app->GetPhysics().GetTransform(pickup.body).position
                             : pickup.netPosition;
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

PredationGame::RemoteAvatar* PredationGame::AvatarFor(uint8_t id)
{
    for (auto& candidate : m_avatars)
    {
        if (candidate->id == id)
        {
            return candidate.get();
        }
    }
    return nullptr;
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
            // With whatever it was carrying, which is the whole point of a dropped weapon keeping
            // its magazine. This asked for the item and the count and nothing else, so a client
            // picking up a rifle with three rounds left in it got a full one, and a client picking
            // up a full one that the host had emptied got an empty one: the ammunition was the one
            // thing about a pickup that only ever crossed the wire in one direction.
            const int stored = m_inventory.Add(m_items, pickup->item, pickup->count, pickup->rounds,
                                               pickup->reserve);
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
        // At the index the host chose, not one of this machine's own choosing. Everything after
        // this refers to the pickup by that number.
        m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                            static_cast<ItemId>(event.item), static_cast<int>(event.other),
                            event.position, event.direction, LoadFromWire(event.rounds),
                            LoadFromWire(event.reserve), static_cast<int>(event.index));
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
            // Their eye never crosses the wire, so the muzzle is the best origin there is.
            tracer.origin = event.position;
            tracer.to = event.direction;
            tracer.hit = event.flag;
            m_tracers.push_back(tracer);

            // And their weapon kicks and flashes. A snapshot cannot carry this: firing happens on
            // one frame and snapshots go out on others, so the moment would be missed most times.
            // It rides the shot message instead, which is the one thing that is sent exactly when
            // a round leaves the barrel.
            if (RemoteAvatar* avatar = AvatarFor(event.player))
            {
                avatar->weaponKick = 1.0f;
            }
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
            RespawnLocalPlayer(event.position);
        }
        break;

    case WorldEventKind::Count:
        break;
    }
}

void PredationGame::SendDynamicBodies()
{
    // Nobody to send to is the common case for a game somebody opened and then played alone, and
    // walking every loose body in the world to build a packet for an empty room is the most
    // expensive thing hosting does.
    if (m_sessionMode != SessionMode::Host || m_host.ConnectedCount() == 0)
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
    // Recorded, not applied. A client running its own physics for a dropped rifle would disagree
    // with everyone else within a second, so the host is the authority; but the host speaks thirty
    // times a second and the screen draws at least twice that, so setting the transform on arrival
    // drew a falling item in visible steps. WorldObjects::FollowNetworkState eases towards these
    // every frame instead.
    for (uint8_t i = 0; i < state.count; ++i)
    {
        const DynamicBodyState& entry = state.bodies[i];
        m_world.SetNetworkState(entry.id, entry.position, entry.rotation);
    }
}

// --- Damage ------------------------------------------------------------------------------------

void PredationGame::UpdateHostMigration(float dt)
{
    // The host has gone. Rather than the game ending for everybody left, the lowest surviving
    // player number takes over and the rest connect to it. Everyone was given the same roster while
    // the old host was alive, so everyone reaches the same answer without having to agree on one,
    // which matters because the machine they would have agreed through is the one that left.
    //
    // The world survives the handover because every machine already has a complete copy of it: that
    // is what applying the same events to the same starting state buys. The new host's copy becomes
    // the authoritative one.
    if (m_sessionMode != SessionMode::Client || !m_client.HostLost())
    {
        return;
    }

    m_migrationTimer += dt;
    // A moment of quiet first. A host that drops off for a second and comes back would otherwise
    // race with the successor for the same port, and the settling time is also long enough that
    // everybody has noticed rather than only the client with the best connection.
    if (m_migrationTimer < cv_migrationSeconds.Get())
    {
        return;
    }

    const bool takeOver = m_client.ShouldBecomeHost();
    const std::string successor = m_client.SuccessorAddress();
    // Where everyone will look for the new host: the port they were already on.
    const uint16_t port = m_client.SessionPort();
    m_migrationTimer = 0.0f;

    if (takeOver)
    {
        PRED_LOG_INFO(Network, "Host went; taking over on port {}", port);
        m_client.Disconnect();
        m_sessionMode = SessionMode::Offline;

        NetHost::Config config;
        config.port = port;
        auto transport = CreateUdpTransport();
        transport->SetConditions(m_simulatedConditions);
        if (m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(),
                         m_spawnPoint))
        {
            m_sessionMode = SessionMode::Host;
            m_app->GetConsole().Print("The host left. You are hosting now.");
        }
        else
        {
            m_app->GetConsole().PrintError("The host left and this machine could not take over.");
            ReturnToTitle();
        }
        return;
    }

    if (successor.empty())
    {
        // Nobody left to play with.
        m_app->GetConsole().Print("The host left and there is nobody else here.");
        ReturnToTitle();
        return;
    }

    PRED_LOG_INFO(Network, "Host went; following {}", successor);
    auto transport = CreateUdpTransport();
    transport->SetConditions(m_simulatedConditions);
    NetClient::Config config;
    // The address carries its own port, because it is where that machine was seen from, not where
    // it listens. The successor listens on the game's port.
    const std::string host = successor.substr(0, successor.find(':'));
    if (m_client.Connect(std::move(transport), host, port, PlayerName(), config))
    {
        m_app->GetConsole().Print("The host left. Reconnecting to " + host);
    }
    else
    {
        m_app->GetConsole().PrintError("The host left and " + host + " could not be reached.");
        ReturnToTitle();
    }
}

void PredationGame::UpdateRespawns(float dt)
{
    // Death is a pause, not an end. Offline and as a host the clock is run here; a client is told
    // when it comes back, because whether it is alive is not its own decision to make.
    if (m_sessionMode == SessionMode::Client)
    {
        return;
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
        // Into the controller the host is simulating for them. The published view is rebuilt from
        // that controller every tick, so subtracting from the view changed nothing and their health
        // came back the moment it was read again: nobody could be killed by anything short of a
        // single fatal round, and the two machines then disagreed about who was alive.
        m_host.ApplyDamageTo(player, amount);
        remaining = m_host.HealthOf(player);
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
        m_respawnTimer = cv_respawnSeconds.Get();
        m_spectating = -1;
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        // The host runs the clock for everybody, because the host is what decides they are dead.
        m_remoteRespawnTimers[player] = cv_respawnSeconds.Get();
    }
    PRED_LOG_INFO(Gameplay, "Player {} died", player);
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
    m_world.Clear(m_scene, m_app->GetPhysics(), m_interactions);
    m_world.Build(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                  &m_weaponData);

    m_inventory.Clear();
    m_weapon = WeaponState{};
    m_heldItem = kInvalidItem;
    m_body.ClearHeldItem(m_scene);
    m_hidingSpot = -1;
    m_tracers.clear();
    m_remoteRespawnTimers.clear();
    m_respawnTimer = 0.0f;
    m_spectating = -1;
    m_deathImpulse = glm::vec3(0.0f);
    m_localCollapsed = false;
    m_body.Revive();
    RespawnLocalPlayer(m_spawnPoint);
    PRED_LOG_INFO(Gameplay, "World reset");
}

void PredationGame::EnterWorld()
{
    PRED_LOG_INFO(Gameplay, "Entering the world");
    // A new game starts from the beginning. The world has been simulating behind the menu, and
    // whatever was done to it last time is still done.
    ResetWorld();
    m_screen = Screen::Playing;
    m_paused = false;
    m_titleStatus.clear();
    SetCameraMode(CameraMode::FirstPerson);
    m_wantMouseCaptured = true;
}

float PredationGame::ReloadProgress() const
{
    // 0 at the start of a reload, 1 at the end. One function, because there were two: the pose used
    // the fraction and the wire sent one minus the seconds remaining, which is the same number only
    // for a reload that happens to take a second. At 2.2 seconds it stayed at zero until the last
    // one and then ran the whole movement inside it, so everybody else saw a reload begin as it
    // ended and play at more than twice the speed.
    if (!m_weapon.IsReloading())
    {
        return 0.0f;
    }
    const WeaponDefinition* weapon = EquippedWeapon();
    const float seconds = weapon != nullptr ? std::max(weapon->reloadSeconds, 0.01f) : 1.0f;
    return glm::clamp(1.0f - m_weapon.reloadRemaining / seconds, 0.0f, 1.0f);
}

// Where the weapon sits relative to the eye, in the view's own frame. The same numbers from the
// editor and from the game, so the two can be compared rather than argued about.
void PredationGame::ReportHold()
{
    // The same numbers from the editor and from the game, so the two can be compared rather
    // than argued about. Everything is in the view frame: across, up and out from the eye.
    const bool inEditor = m_screen == Screen::Editor && m_editorBodyBuilt;
    const PlayerBody& body = inEditor ? m_editorBody : m_body;
    const PlayerView& view = inEditor ? m_editorView : m_player.View();
    const glm::vec3 forward = view.Forward();
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, forward);
    const auto say = [&](const char* what, const glm::vec3& at)
    {
        const glm::vec3 local = at - view.eyePosition;
        char line[160];
        std::snprintf(line, sizeof(line), "%-8s across %+.3f  up %+.3f  out %+.3f", what,
                      glm::dot(local, right), glm::dot(local, up), glm::dot(local, forward));
        m_app->GetConsole().Print(line);
        PRED_LOG_INFO(Gameplay, "{}", line);
    };
    m_app->GetConsole().Print(inEditor ? "The editor's body:" : "The player:");
    say("hold", body.HoldPoint());
    say("origin", body.WeaponOrigin());
    say("muzzle", body.MuzzlePoint());
    say("sight", body.SightPoint());
    char eye[96];
    std::snprintf(eye, sizeof(eye), "eye at %.3f m, pitch %.1f deg", view.eyeHeight,
                  glm::degrees(view.pitch));
    m_app->GetConsole().Print(eye);
    PRED_LOG_INFO(Gameplay, "{}", eye);
}


void PredationGame::DrawPauseMenu()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f};
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({320.0f, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("##pause", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("Paused");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();

    const ImVec2 wide{-1.0f, 32.0f};
    if (ImGui::Button("Resume", wide))
    {
        m_paused = false;
        m_wantMouseCaptured = true;
    }
    // In a session the world carries on without you, and saying so is better than letting somebody
    // believe they have stopped the game everyone else is in.
    if (m_sessionMode != SessionMode::Offline)
    {
        ImGui::TextDisabled("The others are still playing.");
    }

    ImGui::Spacing();
    if (ImGui::Button(m_sessionMode == SessionMode::Offline ? "Leave to menu" : "Leave the game", wide))
    {
        m_paused = false;
        ReturnToTitle();
        ImGui::End();
        return;
    }

    ImGui::Spacing();
    if (ImGui::Button("Quit to desktop", wide))
    {
        m_app->RequestQuit();
    }

    ImGui::End();
}

void PredationGame::ReturnToTitle()
{
    PRED_LOG_INFO(Gameplay, "Back to the title screen");
    StopSession();
    if (m_editor.IsOpen())
    {
        m_editor.SetOpen(m_editorScene, false);
    }
    m_screen = Screen::Title;
    m_paused = false;
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

    // The menu stands on its own rather than in front of the developer test map, which is a grid of
    // grey boxes and says nothing about the game. A slow vertical fall of light over black: enough
    // to not be a flat void, quiet enough to read type against.
    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    const ImVec2 topLeft = viewport->WorkPos;
    const ImVec2 bottomRight{topLeft.x + size.x, topLeft.y + size.y};
    backdrop->AddRectFilledMultiColor(topLeft, bottomRight, IM_COL32(10, 12, 15, 255),
                                      IM_COL32(10, 12, 15, 255), IM_COL32(22, 26, 30, 255),
                                      IM_COL32(16, 18, 22, 255));

    // A few slow motes drifting down it, so the screen is alive without being a scene.
    for (int i = 0; i < 40; ++i)
    {
        const float seed = static_cast<float>(i) * 12.9898f;
        const float column = std::fmod(std::sin(seed) * 43758.5f, 1.0f);
        const float speed = 6.0f + std::fmod(std::abs(std::cos(seed)) * 91.0f, 14.0f);
        const float y = std::fmod(m_titleClock * speed + static_cast<float>(i) * 37.0f, size.y);
        const float alpha = 18.0f + 26.0f * std::abs(std::sin(seed * 2.0f));
        backdrop->AddCircleFilled({topLeft.x + std::abs(column) * size.x, topLeft.y + y}, 1.4f,
                                  IM_COL32(150, 170, 190, static_cast<int>(alpha)), 6);
    }

    ImGui::SetNextWindowPos({viewport->WorkPos.x + size.x * 0.5f, viewport->WorkPos.y + size.y * 0.5f},
                            ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460.0f, 0.0f}, ImGuiCond_Always);

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

    // What everyone else sees on the player list. Kept in the archived config, so it is typed once.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(86.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##playername", m_playerName, sizeof(m_playerName)))
    {
        cv_playerName.Set(m_playerName);
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
    ImGui::TextDisabled("others join on one of these");
    // The actual addresses, not the advice to go and find one. Told to look up "your address", the
    // obvious answer is the public one, which belongs to the router and not to this machine, and a
    // friend on the same network cannot reach it.
    {
        static const std::vector<std::string> addresses = LocalNetworkAddresses();
        if (addresses.empty())
        {
            ImGui::TextDisabled("  no network address found");
        }
        for (const std::string& address : addresses)
        {
            ImGui::TextColored({0.70f, 0.80f, 0.95f, 1.0f}, "  %s:%d", address.c_str(), m_hostPort);
            if (ImGui::IsItemClicked())
            {
                ImGui::SetClipboardText((address + ":" + std::to_string(m_hostPort)).c_str());
            }
        }
        // And the address from outside the house, once the router has agreed to forward the port.
        // Local addresses only reach people on the same network, which is most of what "I gave
        // someone my IP and they could not join" turns out to be.
        switch (m_ports.Status())
        {
        case PortMapper::State::Working:
            ImGui::TextDisabled("  asking the router about the outside world...");
            break;
        case PortMapper::State::Open:
        {
            const std::string outside = m_ports.ExternalAddress();
            if (!outside.empty())
            {
                ImGui::TextColored({0.70f, 0.95f, 0.75f, 1.0f}, "  %s:%d  (from anywhere)",
                                   outside.c_str(), m_hostPort);
                if (ImGui::IsItemClicked())
                {
                    ImGui::SetClipboardText((outside + ":" + std::to_string(m_hostPort)).c_str());
                }
            }
            break;
        }
        case PortMapper::State::Failed:
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(m_ports.Message().c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            break;
        }
        case PortMapper::State::Idle:
        default:
            break;
        }
        ImGui::TextDisabled("  click to copy");
    }
    if (ImGui::Button("Open a game", wide))
    {
        StopSession();
        NetHost::Config config;
        config.port = static_cast<uint16_t>(m_hostPort);
        config.name = PlayerName();
        auto transport = CreateUdpTransport();
        transport->SetConditions(m_simulatedConditions);
        if (m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(), m_spawnPoint))
        {
            m_sessionMode = SessionMode::Host;
            // And ask the router to let people in from outside, which takes a second or two and
            // happens on its own thread. It often works and sometimes cannot; the panel says which,
            // and the local addresses are still there either way.
            m_ports.Open(static_cast<uint16_t>(m_hostPort));
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
                             PlayerName(), config))
        {
            m_sessionMode = SessionMode::Client;
            // A client predicts where it will be, never whether it is alive.
            m_player.SetDecidesDamage(false);
            m_titleStatus.clear();
        }
        else
        {
            // Naming the two things that are almost always wrong, because "could not reach" on its
            // own leaves a player with nothing to try.
            m_titleStatus = std::string("Could not reach ") + m_joinAddress + ":" +
                            std::to_string(m_joinPort) +
                            ". Check they are hosting, that this is their address on this network "
                            "rather than their public one, and that their firewall is letting the "
                            "game through.";
        }
    }

    if (!m_titleStatus.empty())
    {
        ImGui::TextColored({0.90f, 0.55f, 0.35f, 1.0f}, "%s", m_titleStatus.c_str());
    }

#if PRED_DEV_TOOLS
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Model editor", wide))
    {
        EnterEditor(std::string());
    }
#endif
    ImGui::Spacing();
    if (ImGui::Button("Quit", wide))
    {
        m_app->RequestQuit();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("by jojozagjos");

    ImGui::End();
}

// --- Multiplayer -----------------------------------------------------------------------------
//
// The host runs the real simulation for everyone. A client predicts its own movement from local
// input and interpolates everybody else. Nothing a client sends is written into the world: the host
// runs the same movement code against its own physics and what comes out is what happened.

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
    m_localCollapsed = false;
    m_body.Revive();
    m_deathImpulse = glm::vec3(0.0f);
    m_spectating = -1;
}

std::string PredationGame::PlayerName() const
{
    // Trimmed and never empty. A blank name on somebody else's player list is a row with nothing in
    // it, and a name of pure spaces is the same thing with more effort.
    std::string name = m_playerName;
    const size_t first = name.find_first_not_of(" \t");
    const size_t last = name.find_last_not_of(" \t");
    name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    return name.empty() ? std::string("operator") : name;
}

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
        // And take the forwarding down. A door left open onto a machine that is no longer
        // listening is worse than no door.
        m_ports.Close();
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
    // On its own again, so the player is once more the authority on its own health.
    m_player.SetDecidesDamage(true);
    m_networkTick = 0;
}

bool PredationGame::StepSession(const PlayerInput& input, float dt)
{
    ++m_networkTick;

    // What is in the local player's hands, so everyone else sees it.
    const ItemDefinition* held = m_items.Get(m_inventory.Selected().item);
    const uint8_t heldId = held != nullptr ? static_cast<uint8_t>(held->id) : 0;
    // A fraction of the reload, not the seconds left of it. See the note on the client's copy of
    // this below: the two were wrong in the same way and for the same reason.
    const float reloadPlay = ReloadProgress();

    if (m_sessionMode == SessionMode::Host)
    {
        m_host.SetPlayerHeld(0, heldId, m_weapon.aim, m_weapon.IsReloading(), reloadPlay);

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
        // Told to the host with the next input, or nobody else ever sees this player holding
        // anything: the host cannot see inside another machine.
        // How far through the reload is, as a fraction. It used to send one minus the seconds left,
        // which is a fraction only for a reload that takes exactly one second: with a 2.2 second
        // one it stayed at zero until the last second and then ran the whole movement in it, so
        // everyone else saw the reload start when it was nearly over and play at twice the speed.
        m_client.SetHeld(heldId, m_weapon.aim, m_weapon.IsReloading(), reloadPlay);

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
            fresh->body.SetTextureLibrary(m_app->GetTextures());
            // Other people have heads. The body hides its own by default because in first person
            // the camera lives inside it, which is true of exactly one body on this machine at a
            // time, and while spectating it is theirs rather than yours. Set below, every frame.
            fresh->body.Tuning().hideHead = false;
            fresh->view.eyeHeight = config.EyeHeightForStance(remote.stance);
            fresh->built = true;
            m_avatars.push_back(std::move(fresh));
            avatar = m_avatars.back().get();
        }

        // Only where they are and what they are doing crosses the wire. The walk cycle, the lean,
        // the arms and the head all come out of the same procedural body the local player uses, run
        // here from replicated state. A gait is expensive to send and cheap to reproduce.
        // Eased towards where they are rather than set to it. The host rebuilds what it publishes
        // about everyone else once per simulation tick and the screen draws whenever it likes, so
        // taking that position straight moves a body in steps; from inside their head it is the
        // camera jumping back and forth, and the body it is attached to smears with it. A long way
        // out means a respawn or a teleport, where arriving beats sliding across the room.
        if (!avatar->drawnValid || glm::distance(avatar->drawn, remote.position) > 2.0f)
        {
            avatar->drawn = remote.position;
            avatar->drawnValid = true;
        }
        else
        {
            const float blend = 1.0f - std::exp(-26.0f * frameDeltaSeconds);
            avatar->drawn = glm::mix(avatar->drawn, remote.position, blend);
        }

        avatar->state.position = avatar->drawn;
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
        // The climb, so a remote player is seen hauling themselves over rather than sliding up a
        // wall. The duration is nominal here: only the phase matters for the pose.
        //
        // The edge and the phase are remembered rather than read fresh every frame. The snapshot
        // carries them only while somebody is climbing, and the hands go on fading off the ledge
        // for a moment after they stop; taking the wire's zero during that fade dragged their
        // weapon towards the world origin at full strength and then let it snap back.
        if (remote.mantling)
        {
            avatar->mantleEdge = remote.mantleEdge;
            avatar->mantlePhase = remote.mantlePhase;
        }
        else
        {
            avatar->mantlePhase = 1.0f;
        }
        avatar->state.mantling = remote.mantling;
        avatar->state.mantleDuration = 1.0f;
        avatar->state.mantleTime = avatar->mantlePhase;
        avatar->state.mantleEdge = avatar->mantleEdge;
        avatar->state.mantleFrom = avatar->drawn;
        avatar->state.mantleTo = avatar->drawn;

        // The eye eases towards the stance's height rather than being set to it, exactly as the
        // local player's own view does. The body is anchored to the eye, so setting it outright
        // dropped a remote player straight into a crouch in one frame while their own screen
        // showed them sinking into it.
        const float targetEye = config.EyeHeightForStance(remote.stance);
        avatar->view.eyeHeight +=
            (targetEye - avatar->view.eyeHeight) *
            (1.0f - std::exp(-config.eyeTransitionSpeed * frameDeltaSeconds));

        avatar->view.renderPosition = avatar->drawn;
        avatar->view.eyePosition = avatar->drawn + glm::vec3(0.0f, avatar->view.eyeHeight, 0.0f);

        // Leaning moves the eye out sideways, and the body is anchored to the eye, so a remote
        // player who is leaning has to have their eye moved the same way here. Without it they saw
        // round a corner while their body stayed squarely behind it: they could see you and you
        // could not see them, which is the worst thing a peek can be.
        const glm::vec3 leanRight{std::cos(remote.yaw), 0.0f, std::sin(remote.yaw)};
        avatar->view.eyePosition += leanRight * (remote.leanAmount * config.leanSideOffset);
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
                avatar->body.SetHeldItemPlacement(item->holdOffset, item->holdRotation);
            }
            else
            {
                avatar->body.ClearHeldItem(m_scene);
            }
            // Brought up rather than appearing already shouldered.
            avatar->weaponDraw = 0.0f;
        }

        // Decayed per frame, exactly as the local one is. Firing is an event rather than a state,
        // so it arrives through the shot message and fades from there; without it, everybody else's
        // weapon fired with no flash and no recoil, which is why shots from other players read as
        // coming from nowhere.
        avatar->weaponKick = std::max(avatar->weaponKick - frameDeltaSeconds * 7.0f, 0.0f);
        avatar->weaponDraw = std::min(avatar->weaponDraw + frameDeltaSeconds * 3.2f, 1.0f);

        PlayerBody::WeaponPose weaponPose;
        weaponPose.aim = remote.aim;
        weaponPose.reloading = remote.reloading;
        weaponPose.reload = remote.reloadProgress;
        weaponPose.kick = avatar->weaponKick;
        weaponPose.draw = avatar->weaponDraw;
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

        // Spectating puts the camera in their head, so their head has to come off, exactly as your
        // own does when you are alive. Without it the view spends the whole time inside a skull.
        avatar->body.Tuning().hideHead = !m_player.State().alive &&
                                         m_cameraMode == CameraMode::FirstPerson &&
                                         static_cast<int>(avatar->id) == m_spectating;

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
            config.name = PlayerName();
            auto transport = CreateUdpTransport();
            transport->SetConditions(m_simulatedConditions);
            if (!m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(),
                              m_spawnPoint))
            {
                m_app->GetConsole().PrintError("Could not open UDP port " + std::to_string(config.port));
                return;
            }
            m_sessionMode = SessionMode::Host;
            m_ports.Open(config.port);
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
            if (!m_client.Connect(std::move(transport), address, port, PlayerName(), config))
            {
                m_app->GetConsole().PrintError("Could not reach " + address);
                return;
            }
            m_sessionMode = SessionMode::Client;
            m_player.SetDecidesDamage(false);
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
    DestroyEditorFirstPerson();
    m_editor.Shutdown(m_editorScene);
    if (m_editorBodyBuilt)
    {
        m_editorBody.Destroy(m_editorScene);
        m_editorBodyBuilt = false;
    }
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
    const int slotNow = m_inventory.SelectedSlot();
    const Inventory::Slot slot = m_inventory.Selected();
    const ItemDefinition* item = m_items.Get(slot.item);
    const WeaponId wanted = item != nullptr ? m_weaponData.ForItem(item->key) : kInvalidWeapon;

    const ItemId heldNow = (item != nullptr && wanted == kInvalidWeapon) ? item->id : kInvalidItem;

    if (slotNow == m_ammoSlot && wanted == m_weapon.weapon && heldNow == m_heldItem)
    {
        // Nothing is changing hands, so whatever was being put away is back.
        m_weaponHolster = 1.0f;
        return;
    }

    // A weapon already in the hands goes away before the next thing comes out. Two tenths of a
    // second is enough to see it leave, and it is what a model's "unequip" clip runs on: without it
    // a swap is one weapon vanishing and another appearing in the same frame, and there is no
    // moment for an authored put-away to happen in.
    //
    // Everything waits on it, the next weapon and the next item alike. The item used to be put in
    // the hand at the top of this function, before the wait, so selecting a medical kit while
    // holding a rifle put the kit in the hand and left the rifle beside it for a fifth of a second.
    if (m_weapon.HasWeapon() && m_weaponHolster > 0.0f)
    {
        m_weaponHolster = std::max(m_weaponHolster - m_lastFrameSeconds / 0.20f, 0.0f);
        if (m_weaponHolster > 0.0f)
        {
            return;
        }
    }

    // Anything that is not a weapon is still carried in a hand. Selecting a medical kit should put
    // the medical kit in your hand, not leave you empty-handed holding an inventory entry.
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
            m_body.SetHeldItemPlacement(item->holdOffset, item->holdRotation);
        }
    }
    m_weaponHolster = 1.0f;

    // The magazine goes back in the slot it came out of before anything else changes hands.
    // Without this, putting a weapon away and taking it out again refilled it, which the key that
    // now holsters what is already out made trivial to do.
    if (m_ammoSlot != Inventory::kNoSlot && m_weapon.HasWeapon())
    {
        m_inventory.SetSlotAmmo(m_ammoSlot, m_weapon.rounds, m_weapon.reserve);
    }
    m_ammoSlot = slotNow;

    // Anything arriving in the hands is brought up rather than appearing already held.
    m_weaponDraw = 0.0f;

    if (wanted == kInvalidWeapon)
    {
        m_weapon = WeaponState{};
        return;
    }
    if (const WeaponDefinition* definition = m_weaponData.Get(wanted))
    {
        // Rounds do not carry between weapons; each comes with its own magazine and reserve.
        // Swapping back and forth would otherwise be a free reload.
        WeaponSim::Equip(*definition, m_weapon);
        // Unless this particular one has been carried before, in which case it is however the
        // player left it.
        if (slot.rounds >= 0)
        {
            m_weapon.rounds = slot.rounds;
            m_weapon.reserve = slot.reserve;
        }
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
            // Traced from the eye so that what is under the crosshair is hit, drawn from the muzzle
            // so it looks like it came out of the gun. Those are different points and the round is
            // entitled to both.
            tracer.from = MuzzlePosition();
            tracer.origin = shot.origin;
            tracer.to = predicted ? predicted.position : shot.origin + shot.direction * shot.range;
            tracer.hit = predicted.hit;
            m_tracers.push_back(tracer);
            continue;
        }

        ShotResult result = ResolveShot(physics, shot);
        ResolvePlayerHits(shot, LocalPlayerId(), result, PosesNow());

        Tracer tracer;
        tracer.from = MuzzlePosition();
        tracer.origin = shot.origin;
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


// The weapon bench.
//
// Placing a grip socket is not something that can be done by looking at the model on its own: the
// only question that matters is where the hand ends up, and the only way to answer it is to hold
// the thing. So the bench points a weapon at a model without a restart, drives everything the
// weapon does, and draws the sockets on the weapon as it is held, with the distance from each one
// to the hand that is supposed to be at it.
// The editor's try-it panel: someone holding what is being built.
//
// Placing a grip cannot be done by looking at the model. The only question is where the hand ends
// up, so the panel shows the distance from each socket to the hand meant to be at it, draws the line
// between them in the world, and plays every movement the weapon has on demand. Move a socket, watch
// the number come down.
void PredationGame::DrawWeaponBench()
{
    if (m_screen != Screen::Editor || !m_editorBodyBuilt)
    {
        return;
    }

    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 372.0f, 24.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({350.0f, 520.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Hold it"))
    {
        ImGui::End();
        return;
    }

    // Everything in here wraps to the panel. The explanations are the useful part of this window
    // and a sentence that runs off the right edge is worse than no sentence: it reads as a bug.
    ImGui::PushTextWrapPos(0.0f);

    ImGui::TextDisabled("Whatever is open in the editor, in someone's hands.");
    ImGui::Separator();

    // What is in the hands: the model being edited, or one of the items that is not a weapon.
    //
    // One list rather than a list and a checkbox and a second list. They were separate because a
    // weapon and an item are placed by different machinery, and a person holding one thing at a
    // time does not care about that. Choosing an item also takes the weapon out of the hands, which
    // it has to: a weapon takes both of them and the one-handed carry never runs while one is held,
    // so the item was placed and then never posed and never appeared.
    {
        std::vector<const ItemDefinition*> holdable;
        for (const ItemDefinition& item : m_items.All())
        {
            if (item.id != kInvalidItem && !item.key.empty() &&
                m_weaponData.ForItem(item.key) == kInvalidWeapon)
            {
                holdable.push_back(&item);
            }
        }
        m_benchItem = std::clamp(m_benchItem, -1, static_cast<int>(holdable.size()) - 1);

        const auto label = [](const ItemDefinition* item)
        { return item->name.empty() ? item->key.c_str() : item->name.c_str(); };
        const char* current = m_benchItem < 0 ? "the model in the editor"
                                              : label(holdable[static_cast<size_t>(m_benchItem)]);
        if (ImGui::BeginCombo("In the hands", current))
        {
            if (ImGui::Selectable("the model in the editor", m_benchItem < 0))
            {
                m_benchItem = -1;
                m_benchItemHeld = false;
            }
            for (int i = 0; i < static_cast<int>(holdable.size()); ++i)
            {
                ImGui::PushID(i);
                if (ImGui::Selectable(label(holdable[static_cast<size_t>(i)]), i == m_benchItem))
                {
                    m_benchItem = i;
                    m_benchItemHeld = false;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        if (m_benchItem >= 0)
        {
            const ItemDefinition* chosen = holdable[static_cast<size_t>(m_benchItem)];
            if (ItemDefinition* editable = m_items.Mutable(chosen->id))
            {
                bool moved =
                    ImGui::DragFloat3("Offset", &editable->holdOffset.x, 0.002f, -0.5f, 0.5f, "%.3f m");
                moved |= ImGui::DragFloat3("Turn", &editable->holdRotation.x, 1.0f, -180.0f, 180.0f,
                                           "%.0f deg");
                if (moved)
                {
                    m_editorBody.SetHeldItemPlacement(editable->holdOffset, editable->holdRotation);
                }
                if (ImGui::Button("Write to items.json"))
                {
                    const std::filesystem::path file = Paths::AssetsRoot() / "Data" / "items.json";
                    m_app->GetConsole().Print(m_items.SaveHoldPlacements(file)
                                                  ? "Hold placements written to items.json"
                                                  : "Could not write items.json");
                }
                ImGui::TextDisabled("The offset is from the fingers and the turn is in the hand's "
                                    "own frame, so both follow the arm wherever it goes.");
            }
        }
    }
    ImGui::Separator();

    // What the body is doing. Nothing simulates in here, so every movement is a button: the point
    // is to watch one thing at a time and as often as you like.
    const char* stances[] = {"Standing", "Crouching", "Prone"};
    ImGui::Combo("Stance", &m_editorStance, stances, 3);
    ImGui::Checkbox("Walk on the spot", &m_editorWalking);
    ImGui::SliderFloat("Sights", &m_editorAim, 0.0f, 1.0f, "%.2f");

    if (ImGui::Button("Reload"))
    {
        m_editorReload = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Draw"))
    {
        m_editorDraw = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Put away"))
    {
        m_editorHolster = 1.0f;
        m_editorHolstering = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Fire"))
    {
        m_editorKick = 1.0f;
    }
    if (m_editorReload >= 0.0f)
    {
        ImGui::ProgressBar(m_editorReload, {-1.0f, 0.0f}, "reloading");
    }

    ImGui::Separator();

    // The clips the model carries, each playable on demand. A clip that only runs when it happens
    // to be named after something the weapon already does is most of a clip system nobody can use.
    const ModelAsset& model = m_editor.Model();
    if (model.clips.empty())
    {
        ImGui::TextDisabled("No clips on this model, so nothing moves on it: reloading and drawing "
                            "are the model's own now, not something written into the game. Add one "
                            "under Animation and name it reload, equip, unequip or fire. A fire "
                            "clip plays on top of the recoil rather than instead of it.");
    }
    else
    {
        int clipIndex = 0;
        for (const AnimationClip& clip : model.clips)
        {
            // By position, not by name. A clip whose name has been cleared would push an empty id.
            ImGui::PushID(clipIndex++);
            const bool playing = m_editorClip == clip.name;
            if (ImGui::Button(playing ? "Stop" : "Play"))
            {
                m_editorClip = playing ? std::string() : clip.name;
                m_editorClipTime = 0.0f;
            }
            ImGui::SameLine();
            ImGui::Text("%s  %.2fs  %d tracks", clip.name.c_str(), clip.duration,
                        static_cast<int>(clip.tracks.size()));
            if (playing)
            {
                ImGui::SliderFloat("##scrub", &m_editorClipTime, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                ImGui::Checkbox("Run", &m_editorClipPlaying);
            }
            ImGui::PopID();
        }
        if (model.FindPart("magazine") == nullptr)
        {
            ImGui::TextColored({0.90f, 0.70f, 0.45f, 1.0f},
                               "No part called 'magazine', so the built-in reload has nothing to "
                               "take out. Rename the part that is one and it will move.");
        }
    }

    ImGui::Separator();
    if (m_editorBody.HasWeapon())
        {
        ImGui::Checkbox("Draw sockets and the hands they belong to", &m_benchSockets);
        ImGui::TextDisabled("The carry socket is where the weapon sits: move it and the gun moves on "
                            "the screen. The grip is where the trigger hand closes: move that and the "
                            "hand moves along the weapon. Neither drags the other any more.");
        const WeaponVisual& visual = m_editorBody.Weapon();
        const glm::quat hold = m_editorBody.WeaponRotation();
        const glm::vec3 origin = m_editorBody.WeaponOrigin();
        // Two separate questions, and they used to be answered by one number that could not tell them
        // apart. A wrist sits a hand's length behind whatever the palm closes on, so a hand properly on
        // a grip still reads several centimetres away from it, and a support hand that slid back down
        // the barrel because the socket was out of reach read as a much larger miss than a hand that
        // simply could not get there. The useful readings are whether the fingers close on the socket,
        // and how hard the arm is working to hold that.
        const auto report = [&](const char* label, const glm::vec3& socket, int side)
        {
            const glm::vec3 world = origin + hold * socket;
            const glm::vec3 wrist = m_editorBody.GetPose().GlobalPosition(m_editorBody.Rig().hand[side]);
            const glm::vec3 shoulder =
                m_editorBody.GetPose().GlobalPosition(m_editorBody.Rig().shoulder[side]);
            const float gap = glm::distance(world, wrist);
            const float span =
                m_editorBody.Rig().upperArmLength + m_editorBody.Rig().lowerArmLength;
            const float working = glm::distance(wrist, shoulder) / std::max(span, 1e-3f);
    
            // A hand is about eleven centimetres from wrist to closed fingers, so anything inside that
            // is a hand on the grip. Beyond it the arm went somewhere else.
            const bool held = gap < 0.11f;
            ImGui::TextColored(held ? ImVec4(0.65f, 0.85f, 0.65f, 1.0f) : ImVec4(0.90f, 0.70f, 0.45f, 1.0f),
                               held ? "%s hand is on its socket, wrist %.1f cm behind it"
                                    : "%s hand ended up %.1f cm from its socket",
                               label, gap * 100.0f);
            ImGui::TextColored(working < 0.95f ? ImVec4(0.65f, 0.85f, 0.65f, 1.0f)
                                               : ImVec4(0.90f, 0.70f, 0.45f, 1.0f),
                               "   arm at %.0f%% of its reach", working * 100.0f);
        };
        report("Trigger", visual.triggerGrip, 1);
        report("Support", visual.supportGrip, 0);
    
        // What is wrong with the hold, said where the hold is being looked at. The same faults are
        // reported in the editor's Model panel, which is collapsed most of the time and is not where
        // anybody is looking while they place a grip.
        {
            const ImVec4 warn{0.95f, 0.55f, 0.45f, 1.0f};
            if (visual.muzzle.z <= visual.triggerGrip.z)
            {
                ImGui::TextColored(warn, "The muzzle socket is behind the grip, so this is being held "
                                         "by the barrel and pointed at its own stock. Turn the model "
                                         "round under Model, or set the grip's Turn to 0, 180, 0.");
            }
            const float across = std::max(std::abs(visual.triggerGrip.x), std::abs(visual.supportGrip.x));
            if (across > 0.08f)
            {
                ImGui::TextColored(warn,
                                   "A grip sits %.0f cm off the middle of the weapon. Hands close on the "
                                   "centre line of a gun, and an offset there twists the whole hold.",
                                   across * 100.0f);
            }
        }
        ImGui::TextDisabled("The support hand slides back down the barrel when its socket is out of "
                            "reach, so a socket out past the end of the handguard shows as a hand that "
                            "is not on it rather than as an arm stretched to nothing. An arm at its "
                            "full reach is the one to move: that is where the elbow locks straight and "
                            "the hold stops looking like a hold.");
    }


    ImGui::Separator();
    ImGui::TextDisabled("The weapon this model is worn by, for the game:");
    // Index zero is the database's "no weapon" placeholder: no key, no name. It has no business in
    // a list of weapons to assign a model to, and offering it crashed the game outright, because a
    // row with an empty name is a row with an empty id, and an empty id at the root of a popup is
    // the one thing ImGui refuses outright. Every row also carries its own id, so two weapons that
    // happen to share a name cannot collide either.
    const std::vector<WeaponDefinition>& weapons = m_weaponData.All();
    if (weapons.size() > 1)
    {
        m_benchWeapon = std::clamp(m_benchWeapon, 1, static_cast<int>(weapons.size()) - 1);
        const auto label = [&](int index)
        {
            const WeaponDefinition& weapon = weapons[static_cast<size_t>(index)];
            return weapon.name.empty() ? weapon.key : weapon.name;
        };
        if (ImGui::BeginCombo("Weapon", label(m_benchWeapon).c_str()))
        {
            for (int i = 1; i < static_cast<int>(weapons.size()); ++i)
            {
                ImGui::PushID(i);
                if (ImGui::Selectable(label(i).c_str(), i == m_benchWeapon))
                {
                    m_benchWeapon = i;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Assign this model to it"))
        {
            AssignModelToWeapon(weapons[static_cast<size_t>(m_benchWeapon)].id);
        }
        ImGui::TextDisabled("Not written to weapons.json: set it there to keep it.");
    }

    ImGui::PopTextWrapPos();
    ImGui::End();
}

namespace
{

// The panel's own resolution. Small on purpose: it is redrawn every frame, and what it is for is
// the shape and placement of a hold, which reads at this size.
constexpr uint16_t kEditorEyeWidth = 640;
// The height is the window's, worked out per frame: see RenderEditorFirstPerson.

} // namespace

void PredationGame::RenderEditorFirstPerson()
{
    if (m_screen != Screen::Editor || !m_editorFirstPerson || !m_editorBodyBuilt)
    {
        return;
    }

    // The panel is the shape of the game's window, not a fixed 16:9. How much of a weapon is on
    // screen depends entirely on how wide the screen is, so a hold lined up against a panel of the
    // wrong shape comes out somewhere else in the game. The target is remade when the window
    // changes shape, which is rare enough to cost nothing.
    const Renderer& renderer = m_app->GetRenderer();
    const float windowAspect = renderer.Height() > 0 ? static_cast<float>(renderer.Width()) /
                                                           static_cast<float>(renderer.Height())
                                                     : 16.0f / 9.0f;
    const auto wantedHeight = static_cast<uint16_t>(
        std::clamp(static_cast<int>(std::lround(kEditorEyeWidth / std::max(windowAspect, 0.2f))), 180,
                   1200));
    if (wantedHeight != m_editorEyeHeightPixels)
    {
        DestroyEditorFirstPerson();
        m_editorEyeHeightPixels = wantedHeight;
    }

    if (!bgfx::isValid(m_editorEyeBuffer))
    {
        const uint64_t targetFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureHandle attachments[2] = {
            bgfx::createTexture2D(kEditorEyeWidth, m_editorEyeHeightPixels, false, 1,
                                  bgfx::TextureFormat::BGRA8, targetFlags),
            bgfx::createTexture2D(kEditorEyeWidth, m_editorEyeHeightPixels, false, 1,
                                  bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY)};
        if (!bgfx::isValid(attachments[0]) || !bgfx::isValid(attachments[1]))
        {
            PRED_LOG_ERROR(Render, "Editor first-person panel: could not create its render target");
            m_editorFirstPerson = false;
            return;
        }
        // The framebuffer takes ownership of both, so destroying it destroys them.
        m_editorEyeBuffer = bgfx::createFrameBuffer(2, attachments, true);
        m_editorEyeTexture = attachments[0];
        if (!bgfx::isValid(m_editorEyeBuffer))
        {
            PRED_LOG_ERROR(Render, "Editor first-person panel: could not create its framebuffer");
            m_editorFirstPerson = false;
            return;
        }
    }

    const PlayerView& view = m_editorView;
    const glm::vec3 forward = view.Forward();
    const glm::mat4 viewMatrix =
        glm::lookAtRH(view.eyePosition, view.eyePosition + forward, glm::vec3(0.0f, 1.0f, 0.0f));
    // The game's own field of view, and the near plane it uses, because that is what decides how
    // much of a receiver the camera cuts through.
    const float aspect =
        static_cast<float>(kEditorEyeWidth) / static_cast<float>(m_editorEyeHeightPixels);
    const float vertical = 2.0f * std::atan(std::tan(glm::radians(cv_fov.Get()) * 0.5f) / aspect);
    const bool homogeneous = bgfx::getCaps()->homogeneousDepth;
    const glm::mat4 projection = homogeneous
                                     ? glm::perspectiveRH_NO(vertical, aspect, 0.05f, 200.0f)
                                     : glm::perspectiveRH_ZO(vertical, aspect, 0.05f, 200.0f);

    bgfx::setViewFrameBuffer(Renderer::kViewOffscreenLive, m_editorEyeBuffer);
    bgfx::setViewRect(Renderer::kViewOffscreenLive, 0, 0, kEditorEyeWidth, m_editorEyeHeightPixels);
    bgfx::setViewClear(Renderer::kViewOffscreenLive, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x14181cff,
                       1.0f, 0);
    bgfx::setViewTransform(Renderer::kViewOffscreenLive, glm::value_ptr(viewMatrix),
                           glm::value_ptr(projection));
    m_app->GetSceneRenderer().Draw(Renderer::kViewOffscreenLive, m_editorScene, m_app->GetMeshes(),
                                   view.eyePosition);
}

void PredationGame::DrawEditorFirstPerson()
{
    if (m_screen != Screen::Editor || !m_editorBodyBuilt)
    {
        return;
    }

    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 492.0f, 470.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({470.0f, 350.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("First person"))
    {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Draw it", &m_editorFirstPerson);
    ImGui::SameLine();
    ImGui::Checkbox("Centre lines", &m_editorEyeReticle);
    if (m_editorFirstPerson && bgfx::isValid(m_editorEyeTexture))
    {
        // Fitted to the panel, keeping the shape of the frame it is standing in for. A stretched
        // one would answer the on-screen question wrongly, which is the question it exists for.
        const float aspect =
            static_cast<float>(kEditorEyeWidth) / static_cast<float>(m_editorEyeHeightPixels);
        const ImVec2 room = ImGui::GetContentRegionAvail();
        const float width = std::min(room.x, std::max(room.y, 1.0f) * aspect);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, width / aspect);
        ImGui::Image(static_cast<ImTextureID>(ImGuiLayer::TextureId(m_editorEyeTexture)), size);

        // Where the view axis is, drawn over the picture. Lining a sight up means putting it on
        // that axis, and by eye alone the middle of a small panel is a guess.
        if (m_editorEyeReticle)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 centre(at.x + size.x * 0.5f, at.y + size.y * 0.5f);
            constexpr ImU32 kLine = IM_COL32(120, 230, 140, 150);
            draw->AddLine({at.x, centre.y}, {at.x + size.x, centre.y}, kLine);
            draw->AddLine({centre.x, at.y}, {centre.x, at.y + size.y}, kLine);
            draw->AddCircle(centre, size.y * 0.06f, kLine, 0, 1.0f);
        }
    }
    else if (!m_editorFirstPerson)
    {
        ImGui::TextDisabled("Off. The panel is redrawn every frame, so leave it off while working "
                            "on something else.");
    }

    ImGui::End();
}

void PredationGame::DestroyEditorFirstPerson()
{
    if (bgfx::isValid(m_editorEyeBuffer))
    {
        bgfx::destroy(m_editorEyeBuffer);
    }
    m_editorEyeBuffer = BGFX_INVALID_HANDLE;
    m_editorEyeTexture = BGFX_INVALID_HANDLE;
}

void PredationGame::AssignModelToWeapon(WeaponId weapon)
{
    WeaponDefinition* chosen = m_weaponData.Mutable(weapon);
    if (chosen == nullptr)
    {
        m_app->GetConsole().PrintError("No such weapon to assign a model to");
        return;
    }

    const std::string name = m_editor.Model().name;
    if (name.empty())
    {
        m_app->GetConsole().PrintError("The model has no name yet: give it one and save it first");
        return;
    }

    chosen->model = name;
    // Anything already built from the old model file is now wrong, so the cache goes and the next
    // weapon built reads the file again.
    ForgetWeaponModels();
    m_app->GetConsole().Print(chosen->name + " now wears " + name);
}



// The preview body in the editor: someone to hold what is being built.
//
// Driven from numbers rather than from a controller, because there is nothing to control. The body
// is the game's own, posed by the same code that poses the player, so a grip that looks held here is
// held in the game. A separate mannequin drawn by separate code would answer a different question.
void PredationGame::UpdateEditorBody(float frameDeltaSeconds)
{
    if (!m_editorBodyBuilt)
    {
        return;
    }

    m_editorClock += frameDeltaSeconds;

    const PlayerConfig& config = m_player.Config();
    const PlayerStance stance = m_editorStance == 2   ? PlayerStance::Prone
                                : m_editorStance == 1 ? PlayerStance::Crouching
                                                      : PlayerStance::Standing;
    m_editorState.stance = stance;
    m_editorState.grounded = true;
    m_editorState.alive = true;
    // Aside from the origin, where the model itself is laid out. Standing the two in the same place
    // means the body is always in front of the thing being edited.
    m_editorState.position = {1.1f, 0.0f, 0.0f};

    // Walking on the spot. The stride advances but the body does not travel, so the gait can be
    // watched from one place rather than chased across the floor.
    const float speed = m_editorWalking ? config.SpeedForStance(stance, false, false) : 0.0f;
    m_editorState.velocity = glm::vec3(0.0f, 0.0f, -speed);
    if (m_editorWalking)
    {
        m_editorState.strideDistance += speed * frameDeltaSeconds;
        m_editorState.stridePhase = glm::fract(
            m_editorState.stridePhase +
            speed * frameDeltaSeconds / std::max(config.StrideLength(speed, stance), 0.05f));
    }

    m_editorView.eyeHeight = config.EyeHeightForStance(stance);
    m_editorView.renderPosition = m_editorState.position;
    m_editorView.eyePosition = m_editorState.position + glm::vec3(0.0f, m_editorView.eyeHeight, 0.0f);
    // Facing straight ahead. It used to be turned a little towards the model, on the idea that the
    // weapon reads better presented than seen end-on, and what it actually looked like was a body
    // standing crooked for no reason anyone could see.
    m_editorView.yaw = 0.0f;
    m_editorState.yaw = 0.0f;
    m_editorView.pitch = 0.0f;
    m_editorView.leanRoll = 0.0f;

    // What it is holding is whatever the editor has open, rebuilt whenever the editor says the model
    // changed. That is the whole loop: move a socket, see the hand move.
    const ModelAsset& model = m_editor.Model();
    const bool geometryChanged = m_editor.TakeGeometryChanged();
    const bool anythingChanged = m_editor.TakePreviewChanged();

    // One thing in the hands at a time.
    //
    // A weapon takes both of them, and the one-handed carry never runs while one is held, so an
    // item put in the hand alongside a weapon was placed and then never posed: it stayed at the
    // origin and nobody ever saw it. Choosing an item takes the weapon away and choosing the model
    // brings it back.
    std::vector<const ItemDefinition*> holdable;
    for (const ItemDefinition& item : m_items.All())
    {
        if (item.id != kInvalidItem && !item.key.empty() &&
            m_weaponData.ForItem(item.key) == kInvalidWeapon)
        {
            holdable.push_back(&item);
        }
    }
    const int itemIndex =
        m_benchItem >= 0 && m_benchItem < static_cast<int>(holdable.size()) ? m_benchItem : -1;

    if (itemIndex >= 0)
    {
        if (m_editorBody.HasWeapon())
        {
            m_editorBody.SetWeapon(m_editorScene, m_app->GetMeshes(), nullptr);
        }
        // Built once rather than every frame: making a mesh per frame is how an editor comes to
        // feel slow, and dragging an offset does not change the mesh.
        if (!m_benchItemHeld)
        {
            const ItemDefinition* item = holdable[static_cast<size_t>(itemIndex)];
            m_editorBody.SetHeldItem(m_editorScene, m_app->GetMeshes(), item->key,
                                     ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
            m_editorBody.SetHeldItemPlacement(item->holdOffset, item->holdRotation);
            m_benchItemHeld = true;
        }
    }
    else
    {
        if (m_benchItemHeld)
        {
            m_editorBody.ClearHeldItem(m_editorScene);
            m_benchItemHeld = false;
        }
        if (geometryChanged || !m_editorBody.HasWeapon())
        {
            m_editorPreviewWeapon = WeaponDefinition{};
            m_editorPreviewWeapon.id = static_cast<WeaponId>(1);
            m_editorPreviewWeapon.key = model.name.empty() ? "preview" : model.name;
            m_editorPreviewWeapon.name = m_editorPreviewWeapon.key;
            m_editorPreviewWeapon.model = model.name;
            m_editorPreviewWeapon.reloadSeconds = 2.2f;
            m_editorPreviewWeapon.magazineSize = 30;
            ForgetWeaponModels();
            m_editorBody.SetWeaponFromModel(m_editorScene, m_app->GetMeshes(), m_editorPreviewWeapon,
                                            model);
        }
    }

    if (anythingChanged && !geometryChanged)
    {
        // A socket or a clip. Nothing that is drawn has changed, so nothing that is drawn is
        // rebuilt: only the named points the hands are placed by are read again. Rebuilding the
        // meshes for a socket drag copied a quarter of a megabyte per part and destroyed and remade
        // every entity, on every frame of the drag.
        m_editorBody.RefreshWeaponSockets(m_editorPreviewWeapon, model);
    }

    // A clip being watched runs on its own clock, so it can be scrubbed or left running.
    if (!m_editorClip.empty() && m_editorClipPlaying)
    {
        const AnimationClip* watching = model.FindClip(m_editorClip);
        const float duration = watching != nullptr ? std::max(watching->duration, 0.05f) : 1.0f;
        m_editorClipTime = glm::fract(m_editorClipTime + frameDeltaSeconds / duration);
    }

    PlayerBody::WeaponPose pose;
    pose.aim = m_editorAim;
    pose.draw = m_editorDraw;
    pose.kick = m_editorKick;
    pose.reloading = m_editorReload >= 0.0f;
    pose.reload = m_editorReload;
    pose.clip = m_editorClip;
    pose.clipProgress = m_editorClipTime;
    pose.holster = m_editorHolster;
    m_editorBody.SetWeaponPose(pose);

    m_editorDraw = std::min(m_editorDraw + frameDeltaSeconds * 1.6f, 1.0f);
    if (m_editorHolstering)
    {
        m_editorHolster -= frameDeltaSeconds / 0.6f;
        if (m_editorHolster <= 0.0f)
        {
            // Held at nothing for a moment and then brought back out, so the put-away can be
            // watched over and over from one button.
            m_editorHolster = 1.0f;
            m_editorHolstering = false;
            m_editorDraw = 0.0f;
        }
    }
    m_editorKick = std::max(m_editorKick - frameDeltaSeconds * 7.0f, 0.0f);
    if (m_editorReload >= 0.0f)
    {
        m_editorReload += frameDeltaSeconds / 2.2f;
        if (m_editorReload > 1.0f)
        {
            m_editorReload = -1.0f;
        }
    }

    m_editorBody.Update(m_editorScene, m_editorState, m_editorView, config, m_app->GetPhysics(),
                        frameDeltaSeconds);
}

void PredationGame::EnterEditor(const std::string& modelName)
{
    // A place of its own rather than a mode switched on in the middle of a game. It has its own
    // scene, so what is being built stands against an empty floor instead of against whatever
    // happens to be at the spawn point, and nothing in the world simulates behind it.
    StopSession();
    m_screen = Screen::Editor;
    m_titleStatus.clear();
    m_inventoryOpen = false;

    m_editor.SetOpen(m_editorScene, true);
    if (!modelName.empty())
    {
        m_editor.Load(modelName);
    }

    if (!m_editorBodyBuilt)
    {
        // Someone to hold it. Built once and kept, because building a body is not free and the
        // editor is opened and closed a great deal.
        m_editorBody.Build(m_editorScene, m_app->GetMeshes(), m_player.Config());
        m_editorBody.SetTextureLibrary(m_app->GetTextures());
        m_editorBody.Tuning().hideHead = false;
        m_editorBodyBuilt = true;
    }
    m_editorState = PlayerState{};
    m_editorState.alive = true;
    m_editorState.grounded = true;
    m_editorClock = 0.0f;

    // Everything in an editor is done with the pointer, so the mouse is free and it is only taken
    // while the right button is held to look around.
    SetCameraMode(CameraMode::Fly);
    // Framed on the model, which sits at the origin, with the body it will be held by off to one
    // side and in shot.
    //
    // Written to the editor's camera, not the game's. The game's is copied from the editor's every
    // frame while the editor is open, so setting it here was undone one frame later and the view
    // jumped back to wherever the editor had last left it.
    m_editor.Camera().position = {0.30f, 0.62f, 1.15f};
    m_camera.position = m_editor.Camera().position;
    m_lookYaw = glm::radians(12.0f);
    m_lookPitch = glm::radians(-22.0f);
    m_wantMouseCaptured = false;
    UpdateMouseCapture();
    m_app->GetConsole().Print(
        "Model editor. WASD to move, Q and E for down and up, right mouse to look, Escape to leave.");
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
    const glm::vec3 origin = view.eyePosition + view.Forward() * 0.6f;
    const glm::vec3 throwVelocity = view.Forward() * 2.5f;

    DropIntoWorld(slot.item, removed, rounds, reserve, origin, throwVelocity);
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

    // Everyone else has to see it land, and it has to be there to pick up. The throw is sent too,
    // so it arcs on their screen rather than appearing on the floor.
    if (m_sessionMode == SessionMode::Host && index >= 0)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupSpawned;
        event.index = static_cast<uint8_t>(index);
        event.item = static_cast<uint16_t>(item);
        event.other = static_cast<uint8_t>(count);
        event.rounds = LoadToWire(rounds);
        event.reserve = LoadToWire(reserve);
        event.position = origin;
        event.direction = velocity;
        m_host.Broadcast(event);
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
    // Dead counts as restrained: a body on the floor does not fire, aim or reload.
    const bool restrained =
        m_hidingSpot >= 0 || m_cameraMode == CameraMode::Fly || !m_player.State().alive;

    // The weapon runs before the movement, because aiming down the sights slows the player and the
    // controller needs that this tick rather than next.
    WeaponInput weaponInput;
    if (!restrained && !m_inventoryOpen)
    {
        Input& raw = m_app->GetInput();
        weaponInput.trigger = raw.IsActionDown("fire") || m_debugTriggerTicks > 0;
        // And only where the sights would mean anything. Against a wall the body refuses to raise
        // them, so the simulation refuses too: otherwise the player would be walking at aiming
        // pace and shooting at aiming accuracy while looking at a weapon held at their hip.
        weaponInput.aim = (raw.IsActionDown("aim") || m_debugAim) && m_body.AimHasRoom();
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

    UpdateHostMigration(dt);
    UpdateRespawns(dt);

    // The toggles mirror the stance the body is actually in, every tick, not just when a change is
    // refused. They are a request, and the body is the answer; a request that has been answered is
    // spent. Anything else lets the two drift apart, and then the next press asks for the stance you
    // are already in and nothing happens.
    //
    // On a client this matters twice over. The host owns the stance, and reconciliation writes its
    // answer over the prediction: with the toggle left alone, a client could hold "standing" while
    // the host held "prone", stand up locally every tick and be pulled back down by the next
    // snapshot. Spamming the key was the quickest way to get there.
    if (cv_crouchToggle.Get())
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
            const Renderer& renderer = app.GetRenderer();
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

    // Nothing bound to a game key does anything at the menu. The menu is pointed at and typed into,
    // and a stray W while filling in an address must not make the character walk.
    //
    // Nor does anything while dead. A body on the floor was still picking things up, opening doors,
    // getting into lockers, dropping its inventory and changing what it had in hand: none of the
    // actions asked whether the player was alive, because the camera moves to a teammate and it
    // looked as though nothing was being driven any more. Movement is already refused by the
    // controller; this is everything else.
    if (!app.IsConsoleOpen() && m_screen == Screen::Playing && m_player.State().alive)
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
                // Pressing the number for what is already out puts it away again. One key for both,
                // because a separate holster key is one nobody finds.
                m_inventory.SelectSlot(m_inventory.SelectedSlot() == slot ? Inventory::kNoSlot : slot);
            }
        }
        if (const float wheel = input.WheelDelta(); std::abs(wheel) > 0.1f)
        {
            m_inventory.SelectNext(wheel > 0.0f ? -1 : 1);
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
            RespawnLocalPlayer(m_spawnPoint);
        }
        if (input.WasActionPressed("quit_capture"))
        {
            // Escape opens the pause menu and frees the pointer; Escape again closes it and takes
            // the pointer back. It used to leave the game outright on the second press, which meant
            // there was no way to let go of the mouse for a moment without ending up at the title
            // screen, and no way to leave deliberately either: the same key did both.
            m_paused = !m_paused;
            m_wantMouseCaptured = !m_paused;
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
    {
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
            spectated.yaw = watched->yaw;
            spectated.pitch = watched->pitch;
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
            view = m_player.View().ViewMatrix();
            viewPosition = m_player.View().eyePosition;
        }
        break;
    }
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
    // because in first person the camera sits inside it.
    m_body.Tuning().hideHead = m_cameraMode == CameraMode::FirstPerson && m_player.State().alive;

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
        m_body.Revive();
        m_localCollapsed = false;
    }

    m_body.Update(m_scene, m_player.State(), m_player.View(), m_player.Config(), app.GetPhysics(),
                  deltaSeconds);

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

    // A join finishes when the host answers, which can be a moment after the button was pressed.
    if (m_screen == Screen::Title && m_sessionMode == SessionMode::Client && m_client.Connected())
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

    // The menu draws its own backdrop, so the world is not drawn behind it. Nothing else changes:
    // it is still built and still simulating, so starting a game is still instant.
    if (m_screen == Screen::Title)
    {
        return;
    }

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

    // A round is drawn as something that travels, with a short lit section and a mark where it
    // arrives. Lighting the whole line at once, which is what this used to do, draws a diagram of
    // a shot rather than a shot, and it stayed on screen long enough to read as a laser.
    for (const Tracer& tracer : m_tracers)
    {
        const glm::vec3 travel = tracer.to - tracer.from;
        const float distance = glm::length(travel);
        if (distance < 1e-3f)
        {
            continue;
        }
        const glm::vec3 direction = travel / distance;
        const float head = tracer.age * kTracerSpeed;

        if (head < distance)
        {
            const float tail = std::max(head - kTracerLength, 0.0f);
            // Dimmer the further it has gone, so a long shot thins out down range.
            const float bright = 1.0f - 0.40f * (head / distance);
            const auto level = [bright](float channel)
            { return static_cast<uint8_t>(std::clamp(channel * bright, 0.0f, 255.0f)); };
            draw.Line(tracer.from + direction * tail, tracer.from + direction * head,
                      Color::RGBA(level(255.0f), level(226.0f), level(150.0f)));
        }
        else if (tracer.hit)
        {
            // Arrived, and found something. A small burst that shrinks away, so an impact is
            // visible for a moment after the round itself has gone.
            const float left =
                1.0f - std::clamp((tracer.age - distance / kTracerSpeed) / kSparkSeconds, 0.0f, 1.0f);
            if (left > 0.0f)
            {
                const float size = 0.16f * left;
                const auto level = [left](float channel)
                { return static_cast<uint8_t>(std::clamp(channel * left, 0.0f, 255.0f)); };
                const uint32_t colour = Color::RGBA(level(255.0f), level(190.0f), level(90.0f));
                draw.Line(tracer.to - glm::vec3(size, 0.0f, 0.0f),
                          tracer.to + glm::vec3(size, 0.0f, 0.0f), colour);
                draw.Line(tracer.to - glm::vec3(0.0f, size, 0.0f),
                          tracer.to + glm::vec3(0.0f, size, 0.0f), colour);
                draw.Line(tracer.to - glm::vec3(0.0f, 0.0f, size),
                          tracer.to + glm::vec3(0.0f, 0.0f, size), colour);
            }
        }
    }

    // The line a round was actually traced along, which is not the line it is drawn along: rounds
    // are traced from the eye so the crosshair tells the truth, and drawn from the muzzle so they
    // look right. Seeing both at once is the only way to check that difference has not become a
    // lie, and it is a developer's question, so it lives behind a toggle rather than on screen.
    if (DebugCategories::IsEnabled(DebugCategory::Combat))
    {
        for (const Tracer& tracer : m_tracers)
        {
            draw.Line(tracer.origin, tracer.to, Color::kMagenta);
            draw.Line(tracer.from, tracer.to, tracer.hit ? Color::kYellow : Color::kGrey);
            if (tracer.hit)
            {
                draw.Sphere(tracer.to, 0.045f, Color::kRed, 8);
            }
        }
    }

    // The sockets on the weapon as it is actually held, with a line to the hand that is meant to be
    // at each. Placing a grip is a matter of looking at where the hand lands, and there is nothing
    // else that shows both at once.
    if (m_screen == Screen::Editor && m_benchSockets && m_editorBodyBuilt)
    {
        const glm::quat hold = m_editorBody.WeaponRotation();
        const glm::vec3 origin = m_editorBody.WeaponOrigin();
        const auto mark = [&](const glm::vec3& socket, BoneIndex bone, uint32_t colour)
        {
            const glm::vec3 world = origin + hold * socket;
            draw.Sphere(world, 0.018f, colour, 8);
            draw.Line(world, m_editorBody.GetPose().GlobalPosition(bone), colour);
        };
        mark(m_editorBody.Weapon().triggerGrip, m_editorBody.Rig().hand[1], Color::kGreen);
        mark(m_editorBody.Weapon().supportGrip, m_editorBody.Rig().hand[0], Color::kCyan);
        draw.Sphere(origin + hold * m_editorBody.Weapon().muzzle, 0.014f, Color::kYellow, 8);
        draw.Axes(glm::translate(glm::mat4(1.0f), origin) * glm::mat4_cast(hold), 0.12f);
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
        RespawnLocalPlayer(m_spawnPoint);
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
        ImGui::SliderFloat("Step up", &config.stepHeight, 0.05f, 0.8f, "%.2f m");
        ImGui::SliderFloat("Max slope", &config.maxSlopeAngle, 20.0f, 70.0f, "%.0f deg");
        ImGui::TextUnformatted("Step up and slope apply on respawn.");
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
        ImGui::SliderFloat("Foot lift", &body.stepHeight, 0.0f, 0.4f, "%.3f m");
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

void PredationGame::DrawCondition()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    // What shape the player is in, bottom left. Two bars and no numbers: how hurt you are and how
    // much running is left are things to glance at, and a number invites arithmetic in a game that
    // is meant to be making you panic.
    const float left = viewport->Pos.x + 26.0f;
    const float bottom = viewport->Pos.y + viewport->Size.y - 26.0f;
    constexpr float kWidth = 168.0f;
    constexpr float kHeight = 7.0f;
    constexpr float kGap = 9.0f;

    const auto bar = [&](float y, float fill, ImU32 colour)
    {
        const ImVec2 from{left, y};
        const ImVec2 to{left + kWidth, y + kHeight};
        draw->AddRectFilled(from, to, IM_COL32(14, 16, 20, 150), 2.0f);
        if (fill > 0.0f)
        {
            draw->AddRectFilled(from, {left + kWidth * std::clamp(fill, 0.0f, 1.0f), to.y}, colour, 2.0f);
        }
        draw->AddRect(from, to, IM_COL32(120, 126, 138, 130), 2.0f);
    };

    const PlayerState& state = m_player.State();
    const float health = std::clamp(state.health / 100.0f, 0.0f, 1.0f);
    // Reddens as it empties, so the colour says the same thing as the length.
    const ImU32 healthColour = health > 0.6f   ? IM_COL32(150, 190, 160, 220)
                               : health > 0.3f ? IM_COL32(210, 180, 110, 225)
                                               : IM_COL32(205, 90, 80, 235);
    bar(bottom - kHeight, health, healthColour);

    // Stamina is only worth the space when it is not full, or when it has run out, which is exactly
    // when the player needs to know about it.
    if (state.stamina < 0.999f)
    {
        const ImU32 staminaColour =
            state.winded ? IM_COL32(200, 120, 90, 220) : IM_COL32(140, 165, 195, 200);
        bar(bottom - kHeight * 2.0f - kGap, state.stamina, staminaColour);
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

    // How the player is doing. Nothing to do with what is in their hands, which is where this was
    // and why it only appeared when a weapon was out.
    DrawCondition();

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

    DrawPlayerList();

    if (m_inventoryOpen)
    {
        DrawInventoryPanel();
    }
}

void PredationGame::DrawPlayerList()
{
    // Only in a game with other people in it. On your own it is a list of one, which is a panel
    // asking the player to look at their own name.
    if (m_sessionMode == SessionMode::Offline)
    {
        return;
    }
    const std::vector<RemotePlayerView>& others = RemotePlayers();
    if (others.empty())
    {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x - 16.0f, viewport->Pos.y + 16.0f},
                            ImGuiCond_Always, {1.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.35f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (!ImGui::Begin("##Players", nullptr, kFlags))
    {
        ImGui::End();
        return;
    }

    // One row per player, this machine first. Health as a short bar rather than a number: from
    // across the screen a bar says "hurt" at a glance and a number has to be read.
    const auto row = [](const char* name, float health, bool alive, int pingMs, bool self,
                        bool spectated)
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr float kBarWidth = 54.0f;
        constexpr float kBarHeight = 4.0f;

        const ImVec4 nameColour = !alive     ? ImVec4(0.62f, 0.40f, 0.38f, 1.0f)
                                 : spectated ? ImVec4(0.95f, 0.85f, 0.55f, 1.0f)
                                 : self      ? ImVec4(0.88f, 0.92f, 0.96f, 1.0f)
                                             : ImVec4(0.74f, 0.78f, 0.84f, 1.0f);
        ImGui::TextColored(nameColour, "%s%s", name, self ? " (you)" : "");
        ImGui::SameLine(132.0f);

        if (alive)
        {
            const ImVec2 barAt{origin.x + 132.0f, origin.y + ImGui::GetTextLineHeight() * 0.5f - 1.0f};
            const float fraction = glm::clamp(health / 100.0f, 0.0f, 1.0f);
            list->AddRectFilled(barAt, {barAt.x + kBarWidth, barAt.y + kBarHeight},
                                IM_COL32(60, 64, 72, 180));
            const ImU32 fill = fraction > 0.6f   ? IM_COL32(120, 190, 130, 220)
                               : fraction > 0.3f ? IM_COL32(210, 185, 110, 220)
                                                 : IM_COL32(205, 100, 90, 230);
            list->AddRectFilled(barAt, {barAt.x + kBarWidth * fraction, barAt.y + kBarHeight}, fill);
            ImGui::Dummy({kBarWidth, ImGui::GetTextLineHeight()});
        }
        else
        {
            ImGui::TextColored({0.62f, 0.40f, 0.38f, 1.0f}, "down");
        }

        ImGui::SameLine(200.0f);
        if (self && pingMs < 0)
        {
            // The host is not waiting on anybody, so it has no round trip worth printing.
            ImGui::TextDisabled("host");
        }
        else
        {
            const ImVec4 colour = pingMs < 80    ? ImVec4(0.55f, 0.75f, 0.58f, 1.0f)
                                  : pingMs < 180 ? ImVec4(0.82f, 0.76f, 0.50f, 1.0f)
                                                 : ImVec4(0.85f, 0.50f, 0.45f, 1.0f);
            ImGui::TextColored(colour, "%d ms", pingMs);
        }
    };

    const bool hosting = m_sessionMode == SessionMode::Host;
    row(PlayerName().c_str(), m_player.State().health, m_player.State().alive,
        hosting ? -1 : static_cast<int>(m_client.PingMs()), true, false);
    for (const RemotePlayerView& other : others)
    {
        const bool watched = m_spectating >= 0 && static_cast<int>(other.id) == m_spectating;
        row(other.name.c_str(), other.health, other.alive, static_cast<int>(other.pingMs), false,
            watched);
    }

    ImGui::End();
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
                // Clicking what is already out puts it away, the same as pressing its number.
                m_inventory.SelectSlot(m_inventory.SelectedSlot() == i ? Inventory::kNoSlot : i);
            }
            if (definition != nullptr && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", definition->name.c_str());
            }

            // Up to two lines, broken between words, with anything that still will not fit cut short
            // and marked as cut. Clipping one line at the slot edge, which is what this did, reads
            // as a rendering fault rather than as an abbreviation: "Access Keycard" came out as
            // "Access Keyc" with the c half drawn.
            const std::string label = definition != nullptr ? definition->name : std::string("Empty");
            const float lineHeight = ImGui::GetTextLineHeight();
            const ImU32 colour =
                definition != nullptr ? IM_COL32(220, 222, 230, 255) : IM_COL32(115, 118, 128, 255);
            const ImVec2 caption = ImGui::GetCursorScreenPos();

            std::vector<std::string> lines;
            {
                std::string current;
                size_t at = 0;
                while (at <= label.size() && lines.size() < 2)
                {
                    const size_t space = label.find(' ', at);
                    const std::string word = label.substr(at, space - at);
                    const std::string candidate = current.empty() ? word : current + " " + word;
                    if (!current.empty() && ImGui::CalcTextSize(candidate.c_str()).x > kSlotSize)
                    {
                        lines.push_back(current);
                        current = word;
                    }
                    else
                    {
                        current = candidate;
                    }
                    if (space == std::string::npos)
                    {
                        break;
                    }
                    at = space + 1;
                }
                if (lines.size() < 2 && !current.empty())
                {
                    lines.push_back(current);
                }
            }
            for (std::string& line : lines)
            {
                // A single word longer than the slot has nowhere to break, so it is shortened.
                while (line.size() > 1 && ImGui::CalcTextSize((line + ".").c_str()).x > kSlotSize)
                {
                    line.pop_back();
                }
                if (ImGui::CalcTextSize(line.c_str()).x > kSlotSize)
                {
                    line += ".";
                }
            }
            for (size_t line = 0; line < lines.size(); ++line)
            {
                list->AddText({caption.x, caption.y + lineHeight * static_cast<float>(line)}, colour,
                              lines[line].c_str());
            }
            // Always two lines' worth, so a one-word name and a two-word one leave the grid level.
            ImGui::Dummy({kSlotSize, lineHeight * 2.0f});

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
        m_editor.DrawUi(m_editorScene, m_app->GetMeshes());
        // Beside it, because it answers the one question the editor cannot: where does the hand end
        // up.
        DrawWeaponBench();
        DrawEditorFirstPerson();
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

    if (m_paused)
    {
        DrawPauseMenu();
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
