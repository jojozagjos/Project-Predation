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


// The console commands: what can be typed, and the reports a developer asks for.

namespace pred
{

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
    console.RegisterCommand("where", "Print where the player's feet are, and whether they are standing on something",
                            [this](const std::vector<std::string>&)
                            {
                                const PlayerState& state = m_player.State();
                                char line[128];
                                const int surface = SurfaceIndexAt(state.position);
                                const char* underfoot = surface >= 0 && surface < static_cast<int>(m_footsteps.size()) ? m_footsteps[static_cast<size_t>(surface)].name.c_str() : "?";
                                std::snprintf(line, sizeof(line), "at %.2f %.2f %.2f, %s, on %s", state.position.x, state.position.y,
                                              state.position.z, state.swimming ? "swimming" : state.grounded ? "standing" : "in the air", underfoot);
                                m_app->GetConsole().Print(line);
                                PRED_LOG_INFO(Gameplay, "Player {}; the eye at {:.2f}, {:.0f}% under water; the water's surface there {:.2f} ({} waves, the highest {:.2f} m, at {:.1f} s; {:.1f} m deep, lines {:.1f} m apart)", line,
                                              m_renderEye.y, m_underwater * 100.0f, m_swimWaterSet ? m_swimWater.SurfaceAt(m_renderEye.x, m_renderEye.z) : 0.0f,
                                              m_seaWaves.count, m_seaWaves.Highest(), m_waveClock, m_facility.Built() ? m_facility.Terrain().WaterDepth(m_renderEye.x, m_renderEye.z) : 0.0f,
                                              m_facility.Built() ? m_facility.Terrain().WaterLineSpacing(m_renderEye.x, m_renderEye.z) : 0.0f);
                            });

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
        "lean", "Hold a lean for testing (-1 left, 1 right, 'off' to let go), or report it: lean [amount|off]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() > 1)
            {
                m_forceLeanOn = args[1] != "off";
                m_forceLean = m_forceLeanOn ? std::clamp(std::strtof(args[1].c_str(), nullptr), -1.0f, 1.0f) : 0.0f;
            }
            // How far the eye actually sits out to the side of the body, which is what a lean is for.
            const PlayerView& view = m_player.View();
            const glm::vec3 fromBody = view.eyePosition - m_player.View().renderPosition;
            const float sideways = glm::dot(fromBody, view.Right());
            char line[200];
            std::snprintf(line, sizeof(line),
                          "lean %.2f  roll %.1f deg  eye %.3f m to the side%s  keys to the game: %s  mouse captured: %s",
                          m_player.State().leanAmount, glm::degrees(view.leanRoll), sideways,
                          m_forceLeanOn ? "  (held)" : "", m_app->IsUiCapturingKeyboard() ? "no" : "yes",
                          m_app->GetWindow().IsRelativeMouse() ? "yes" : "no");
            m_app->GetConsole().Print(line);
            PRED_LOG_INFO(Gameplay, "{}", line);
        },
        "lean [amount|off]");

#if PRED_DEV_TOOLS
    // Real input events, put on the queue exactly as the operating system would, so a problem that
    // only happens with a key and a mouse button held together can be reproduced without hands.
    // Everything downstream -- the UI, the input system, the game -- sees them as the real thing.
    console.RegisterCommand(
        "sim_input",
        "Inject input: sim_input key <name> <down|up> | mouse <x> <y> | button <left|right> <down|up> | focus",
        [this](const std::vector<std::string>& args)
        {
            const SDL_WindowID window = SDL_GetWindowID(m_app->GetWindow().Handle());
            SDL_Event event{};
            if (args.size() >= 4 && args[1] == "key")
            {
                const SDL_Scancode code = SDL_GetScancodeFromName(args[2].c_str());
                if (code == SDL_SCANCODE_UNKNOWN)
                {
                    m_app->GetConsole().PrintError("No key called " + args[2]);
                    return;
                }
                const bool down = args[3] == "down";
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.windowID = window;
                event.key.scancode = code;
                event.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
                event.key.down = down;
            }
            else if (args.size() >= 2 && args[1] == "focus")
            {
                // The window being clicked into, which is what lets the game capture the mouse.
                event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
                event.window.windowID = window;
            }
            else if (args.size() >= 4 && args[1] == "mouse")
            {
                event.type = SDL_EVENT_MOUSE_MOTION;
                event.motion.windowID = window;
                event.motion.x = std::strtof(args[2].c_str(), nullptr);
                event.motion.y = std::strtof(args[3].c_str(), nullptr);
            }
            else if (args.size() >= 4 && args[1] == "button")
            {
                const bool down = args[3] == "down";
                event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                event.button.windowID = window;
                event.button.button = args[2] == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                event.button.down = down;
                event.button.clicks = 1;
                SDL_GetMouseState(&event.button.x, &event.button.y);
            }
            else
            {
                m_app->GetConsole().PrintError(
                    "usage: sim_input key <name> <down|up> | mouse <x> <y> | button <left|right> <down|up>");
                return;
            }
            SDL_PushEvent(&event);
        },
        "sim_input key <name> <down|up> | mouse <x> <y> | button <left|right> <down|up>");
#endif

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
                                // In the log too, and where the picture is taken from and which map this machine thinks it is on, so a
                                // scripted run of two machines can say where each one is.
                                PRED_LOG_INFO(Gameplay, "{}; eye {:.1f} {:.1f} {:.1f}; map {}; aboard {}; camera mode {}", buffer, m_renderEye.x, m_renderEye.y, m_renderEye.z,
                                              static_cast<int>(m_map), m_ship.Contains(state.position), static_cast<int>(m_cameraMode));
                            });

    console.RegisterCommand("eye_measure", "Log how bright the picture has been lately, as the eye adapted to it (for r.adapt_reference): measured now, logged a few frames on",
                            [this](const std::vector<std::string>&)
                            {
                                m_app->GetPostProcess().MeasureLuminance();
                                const Environment& air = m_scene.GetEnvironment();
                                PRED_LOG_INFO(Render, "eye_measure: last measured {:.4f} (r.adapt_reference {:.4f}); air haze {:.3f}, fog {:.0f} to {:.0f} m",
                                              m_app->GetPostProcess().MeasuredLuminance(), cv_eyeReference.Get(), air.airHaze, air.fogStart, air.fogEnd);
                            });
    console.RegisterCommand("interact_report", "List what can be used near the view, and why each is or is not offered",
                            [this](const std::vector<std::string>&)
                            {
                                for (const std::string& line : m_interactions.Report(m_scene, m_app->GetPhysics(), m_player.View().eyePosition,
                                                                                      m_player.View().Forward()))
                                {
                                    m_app->GetConsole().Print(line);
                                    PRED_LOG_INFO(Gameplay, "interact: {}", line);
                                }
                            });
    console.RegisterCommand("boot", "Show the boot screen again, and then the title's press-any-key",
                            [this](const std::vector<std::string>&)
                            {
                                m_bootDone = false;
                                m_bootForced = true;
                                m_bootClock = 0.0f;
                                m_bootCard = 0;
                                m_titleAwake = false;
                                m_titleShownFor = 0.0f;
                                m_titleClock = 0.0f;
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
                // In the editor, its camera is the one the view is taken from.
                m_editor.Camera().position = m_camera.position;
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
        "look_sun", "Turn the fly camera to the sun as the sky draws it, or so many degrees right and up of it: look_sun [right up]",
        [this](const std::vector<std::string>& args)
        {
            const Environment& environment = m_scene.GetEnvironment();
            const glm::vec3 to = -glm::normalize(glm::length(environment.skySunDirection) > 1.0e-6f ? environment.skySunDirection : environment.sunDirection);
            m_cameraMode = CameraMode::Fly;
            m_camera.position = m_cameraMode == CameraMode::Fly ? m_camera.position : m_renderEye;
            m_lookYaw = std::atan2(to.x, -to.z);
            m_lookPitch = std::asin(std::clamp(to.y, -1.0f, 1.0f));
            if (args.size() >= 3)
            {
                m_lookYaw += glm::radians(std::strtof(args[1].c_str(), nullptr));
                m_lookPitch += glm::radians(std::strtof(args[2].c_str(), nullptr));
            }
            PRED_LOG_INFO(Gameplay, "sun towards {:.3f} {:.3f} {:.3f}; light from {:.3f} {:.3f} {:.3f}", to.x, to.y, to.z, -environment.sunDirection.x,
                          -environment.sunDirection.y, -environment.sunDirection.z);
            // And what else is up there, and how far from it.
            for (size_t i = 0; i + 1 < std::size(environment.skyBodies); i += 2)
            {
                const glm::vec4 place = environment.skyBodies[i];
                if (place.w <= 0.0f)
                {
                    continue;
                }
                const float apart = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(glm::vec3(place)), to), -1.0f, 1.0f)));
                PRED_LOG_INFO(Gameplay, "  body {}: {:.1f} deg from the sun, {:.3f} deg across, brightness {:.2f}", i / 2, apart, glm::degrees(place.w),
                              glm::length(glm::vec3(place)));
            }
        });

    console.RegisterCommand(
        "what_is","Name the drawn things along the middle of the view, nearest first: what_is [how many]",
        [this](const std::vector<std::string>& args)
        {
            // Along the view from the eye, through every drawn thing's box (its mesh's bounds, turned and scaled as it is).
            const glm::vec3 from = m_cameraMode == CameraMode::Fly ? m_camera.position : m_player.View().eyePosition;
            const glm::vec3 way = glm::normalize(m_cameraMode == CameraMode::Fly ? m_camera.Forward() : m_player.View().Forward());
            std::vector<std::pair<float, std::string>> hits;
            m_scene.ForEachMeshRenderer(
                [&](Entity entity, const Transform& transform, const MeshRenderer& renderer)
                {
                    const Mesh* mesh = m_app->GetMeshes().Get(renderer.mesh);
                    if (mesh == nullptr || !mesh->bounds.IsValid())
                    {
                        return;
                    }
                    // Into the thing's own frame, where its box is square to the axes.
                    const glm::mat4 toThing = glm::inverse(transform.Matrix());
                    const glm::vec3 o = glm::vec3(toThing * glm::vec4(from, 1.0f));
                    const glm::vec3 d = glm::vec3(toThing * glm::vec4(way, 0.0f));
                    float near = 0.0f;
                    float far = 1.0e9f;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        if (std::abs(d[axis]) < 1.0e-9f)
                        {
                            if (o[axis] < mesh->bounds.min[axis] || o[axis] > mesh->bounds.max[axis])
                            {
                                return;
                            }
                            continue;
                        }
                        float t0 = (mesh->bounds.min[axis] - o[axis]) / d[axis];
                        float t1 = (mesh->bounds.max[axis] - o[axis]) / d[axis];
                        if (t0 > t1)
                        {
                            std::swap(t0, t1);
                        }
                        near = std::max(near, t0);
                        far = std::min(far, t1);
                    }
                    if (near <= far)
                    {
                        // Back in metres along the view.
                        const glm::vec3 at = glm::vec3(transform.Matrix() * glm::vec4(o + d * near, 1.0f));
                        hits.emplace_back(glm::length(at - from), m_scene.Name(entity) + " (" + mesh->name + ")");
                    }
                });
            std::sort(hits.begin(), hits.end());
            const size_t count = args.size() >= 2 ? static_cast<size_t>(std::max(std::atoi(args[1].c_str()), 1)) : 6;
            for (size_t i = 0; i < hits.size() && i < count; ++i)
            {
                char line[256];
                std::snprintf(line, sizeof(line), "%6.2f m  %s", hits[i].first, hits[i].second.c_str());
                m_app->GetConsole().Print(line);
                PRED_LOG_INFO(Gameplay, "what_is: {}", line);
            }
            if (hits.empty())
            {
                m_app->GetConsole().Print("Nothing drawn along the view.");
            }
        },
        "what_is [how many]");

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
        "hold", "Take an item from the inventory into the hand: hold <key>",
        [this](const std::vector<std::string>& args)
        {
            const ItemId id = args.size() >= 2 ? m_items.IdOf(args[1]) : kInvalidItem;
            for (int slot = 0; slot < m_inventory.SlotCount(); ++slot)
            {
                if (!m_inventory.At(slot).IsEmpty() && m_inventory.At(slot).item == id)
                {
                    m_inventory.SelectSlot(slot);
                    return;
                }
            }
            m_app->GetConsole().PrintError("not carrying that");
        },
        "hold <key>");
    console.RegisterCommand(
        "use_item", "Use what is in the hand, as fire would: use_item",
        [this](const std::vector<std::string>&)
        {
            const ItemDefinition* held = m_items.Get(m_heldItem);
            if (held == nullptr || held->use.kind == ItemUseKind::None || m_itemUse.active)
            {
                m_app->GetConsole().PrintError("nothing in the hand to use");
                return;
            }
            StartItemUse(*held);
        });
    console.RegisterCommand(
        "item_state", "What the hands and the torch are doing: item_state",
        [this](const std::vector<std::string>&)
        {
            char line[200];
            std::snprintf(line, sizeof(line), "held %s, using %s %.2f/%.2f s, torch %.0f%%, flare %.1f s, %zu burning",
                          m_items.Get(m_heldItem) != nullptr ? m_items.Get(m_heldItem)->key.c_str() : "nothing",
                          m_itemUse.active ? "yes" : "no", m_itemUse.time, m_itemUse.seconds, m_torchCharge * 100.0f,
                          m_flareBurn, m_burningFlares.size());
            m_app->GetConsole().Print(line);
            PRED_LOG_INFO(Gameplay, "{} (health {:.0f})", line, m_player.State().health);
        });
    console.RegisterCommand(
        "hurt_me", "Take some damage, to try something that mends it: hurt_me [amount]",
        [this](const std::vector<std::string>& args)
        {
            const float amount = args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : 40.0f;
            ApplyPlayerDamage(LocalPlayerId(), amount, kNoKiller, glm::vec3(0.0f, 0.0f, 1.0f), "console");
        });
    console.RegisterCommand(
        "torch_charge", "Set the torch's cell, 0 to 1: torch_charge <fraction>",
        [this](const std::vector<std::string>& args)
        {
            m_torchCharge = args.size() >= 2 ? std::clamp(std::strtof(args[1].c_str(), nullptr), 0.0f, 1.0f) : 1.0f;
        });

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
        "light_report", "List the level's lamps near you: what kind, how lit, how far",
        [this](const std::vector<std::string>&)
        {
            Console& out = m_app->GetConsole();
            const glm::vec3 me = m_player.State().position;
            int shown = 0;
            for (const LevelLights::Light& light : m_levelLights.Lights())
            {
                const float away = glm::distance(light.position, me);
                if (away > 25.0f)
                {
                    continue;
                }
                char line[160];
                std::snprintf(line, sizeof(line), "%-9s %-8s circuit %d  lit %.2f  %.1f m  at %.1f %.1f %.1f",
                              LightKindName(light.kind), LightMoodName(light.mood), light.circuit, light.level, away,
                              light.position.x, light.position.y, light.position.z);
                out.Print(line);
                PRED_LOG_INFO(Render, "{}", line);
                ++shown;
            }
            out.Print(std::to_string(shown) + " lamp(s) within 25 m, " +
                      std::to_string(m_scene.GetEnvironment().sceneLights.size()) + " lit this frame in all");
            PRED_LOG_INFO(Render, "{} lamp(s) within 25 m, {} lit this frame", shown, m_scene.GetEnvironment().sceneLights.size());
        });

    console.RegisterCommand(
        "weapon_rounds", "Set how many rounds are in the magazine, for trying the empty states: weapon_rounds <n> [reserve]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() >= 2)
            {
                m_weapon.rounds = std::max(std::atoi(args[1].c_str()), 0);
            }
            if (args.size() >= 3)
            {
                m_weapon.reserve = std::max(std::atoi(args[2].c_str()), 0);
            }
            m_app->GetConsole().Print("rounds " + std::to_string(m_weapon.rounds) + ", reserve " + std::to_string(m_weapon.reserve));
        },
        "weapon_rounds <n> [reserve]");

    console.RegisterCommand(
        "fire", "Hold the trigger for a number of ticks, for testing without a mouse: fire [ticks]",
        [this](const std::vector<std::string>& args)
        { m_debugTriggerTicks = args.size() >= 2 ? std::atoi(args[1].c_str()) : 1; }, "fire [ticks]");

    console.RegisterCommand(
        "tracer_report", "Where the last tracer was drawn from, against the eye and the drawn barrel",
        [this](const std::vector<std::string>&)
        {
            if (m_tracers.empty())
            {
                m_app->GetConsole().Print("No tracers");
                return;
            }
            const Tracer& last = m_tracers.back();
            const glm::vec3 eye = m_player.View().eyePosition;
            const glm::vec3 barrel = m_body.HasWeapon() ? m_body.MuzzlePoint() : eye;
            char line[200];
            std::snprintf(line, sizeof(line),
                          "tracer starts %.3f m from the eye and %.3f m from the drawn barrel; the round "
                          "was traced from %.3f m from the eye",
                          glm::distance(last.from, eye), glm::distance(last.from, barrel),
                          glm::distance(last.origin, eye));
            m_app->GetConsole().Print(line);
            PRED_LOG_INFO(Gameplay, "{}", line);
        });

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
        "editor_reload",
        "Make a reload from a template in the open model, and put the playhead somewhere: "
        "editor_reload <reload|empty|double> [seconds]",
        [this](const std::vector<std::string>& args)
        {
            if (!m_editor.IsOpen() || args.size() < 2)
            {
                m_app->GetConsole().PrintError("Open a model in the editor first: editor_reload <reload|empty|double> [seconds]");
                return;
            }
            const ModelEditor::ReloadTemplate kind = args[1] == "empty"    ? ModelEditor::ReloadTemplate::Empty
                                                     : args[1] == "double" ? ModelEditor::ReloadTemplate::DoubleMagazine
                                                                           : ModelEditor::ReloadTemplate::Tactical;
            m_editor.MakeReloadTemplate(kind);
            if (args.size() > 2)
            {
                m_editor.SetPlayhead(static_cast<float>(std::atof(args[2].c_str())));
            }
            PRED_LOG_INFO(Asset, "editor_reload {}: {}", args[1], m_editor.Status());
        },
        "editor_reload <reload|empty|double> [seconds]");

    console.RegisterCommand("editor_save", "Save the model open in the editor, as its Save button does",
                            [this](const std::vector<std::string>&)
                            {
                                const bool saved = m_editor.IsOpen() && m_editor.Save();
                                PRED_LOG_INFO(Asset, "editor_save: {}", saved ? "saved" : "nothing saved");
                            });

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

    console.RegisterCommand("lab", "Go to the creature lab: everybody in the game, and the creatures from its nest",
                            [this](const std::vector<std::string>&) { GoToMap(MapChoice::Lab); });
    console.RegisterCommand("testmap", "Go to the testing area: everybody in the game",
                            [this](const std::vector<std::string>&) { GoToMap(MapChoice::TestMap); });
    console.RegisterCommand("ship", "Go aboard the ship: everybody in the game",
                            [this](const std::vector<std::string>&) { GoToMap(MapChoice::Ship); });
#if PRED_DEV_TOOLS
    console.RegisterCommand("ship_orbit", "Put the ship over a site's planet, or out in space with 0: ship_orbit <seed>",
                            [this](const std::vector<std::string>& args)
                            { m_shipOrbiting = static_cast<uint16_t>(args.size() >= 2 ? std::clamp(std::atoi(args[1].c_str()), 0, 65535) : m_facility.Seed()); });
#endif
    console.RegisterCommand(
        "facility",
        "Go to the generated facility. facility <seed> builds the one planned from that seed first (1 to 65535); "
        "facility new, a new one",
        [this](const std::vector<std::string>& args)
        {
            if (m_screen == Screen::Playing && !IsAuthority())
            {
                m_app->GetConsole().PrintError("Only the host can change the facility.");
                return;
            }
            uint16_t seed = m_facility.Seed();
            if (args.size() >= 2)
            {
                if (args[1] == "new")
                {
                    seed = static_cast<uint16_t>(
                        1 + std::chrono::steady_clock::now().time_since_epoch().count() % 65535);
                }
                else
                {
                    seed = static_cast<uint16_t>(std::clamp(std::atoi(args[1].c_str()), 1, 65535));
                }
            }
            if (seed != m_facility.Seed())
            {
                ChangeFacility(seed);
            }
            GoToMap(MapChoice::Facility);
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
        "Import a glTF binary as an editable model: model_import <file.glb> <name> [size] [turn x y z] "
        "[skip=part,part]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 3)
            {
                m_app->GetConsole().PrintError(
                    "usage: model_import <file.glb> <name> [size] [turn x y z] [skip=part,part]");
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
            // Display props to leave behind -- the spare magazine and loose rounds a download often
            // comes with. Anywhere on the line, so it does not care which of the numbers were given.
            for (const std::string& arg : args)
            {
                if (arg.rfind("skip=", 0) != 0)
                {
                    continue;
                }
                std::string list = arg.substr(5);
                size_t start = 0;
                while (start <= list.size())
                {
                    const size_t comma = list.find(',', start);
                    const std::string name = list.substr(start, comma - start);
                    if (!name.empty())
                    {
                        options.skipParts.push_back(name);
                    }
                    if (comma == std::string::npos)
                    {
                        break;
                    }
                    start = comma + 1;
                }
            }
            ModelAsset imported;
            std::string error;
            if (!LoadGlbModel(args[1], options, imported, &error))
            {
                m_app->GetConsole().PrintError("import failed: " + error);
                return;
            }
            imported.name = args[2];
            if (!imported.SaveToFile(ModelPathFor(args[2], "Weapons")))
            {
                m_app->GetConsole().PrintError("could not write the model");
                return;
            }
            ForgetWeaponModels();
            RescanModels();
            m_app->GetConsole().Print("Imported " + std::to_string(imported.parts.size()) +
                                      " parts as " + args[2] +
                                      ". Open it with: editor " + args[2]);
        },
        "model_import <file.glb> <name> [size] [turn x y z]");

    console.RegisterCommand(
        "model_info", "Print a model's extents and sockets: model_info <name>",
        [this](const std::vector<std::string>& args)
        {
            // Placing a socket needs to know where the geometry actually is, and an imported model
            // arrives in whatever frame its author used. This says: how big it is, which way the
            // long axis runs, and what sockets it already has.
            Console& out = m_app->GetConsole();
            if (args.size() < 2)
            {
                out.PrintError("usage: model_info <name>");
                return;
            }
            ModelAsset model;
            const std::filesystem::path file = ModelPath(args[1]);
            if (file.empty() || !model.LoadFromFile(file))
            {
                out.PrintError("no model called " + args[1]);
                return;
            }
            glm::vec3 low{std::numeric_limits<float>::max()};
            glm::vec3 high{std::numeric_limits<float>::lowest()};
            size_t vertices = 0;
            for (const ModelPart& part : model.parts)
            {
                const glm::mat4 rest = part.LocalMatrix();
                for (const MeshVertex& vertex : part.mesh.vertices)
                {
                    const glm::vec3 at = glm::vec3(rest * glm::vec4(vertex.position, 1.0f));
                    low = glm::min(low, at);
                    high = glm::max(high, at);
                    ++vertices;
                }
            }
            char buffer[220];
            if (vertices == 0)
            {
                out.Print("no geometry");
            }
            else
            {
                std::snprintf(buffer, sizeof(buffer),
                              "%zu parts, %zu vertices, extents x %.3f..%.3f  y %.3f..%.3f  z "
                              "%.3f..%.3f  (size %.3f x %.3f x %.3f)",
                              model.parts.size(), vertices, low.x, high.x, low.y, high.y, low.z,
                              high.z, high.x - low.x, high.y - low.y, high.z - low.z);
                out.Print(buffer);
                PRED_LOG_INFO(Asset, "{}: {}", args[1], buffer);
            }
            for (const ModelSocket& socket : model.sockets)
            {
                std::snprintf(buffer, sizeof(buffer), "  socket %-10s %7.3f %7.3f %7.3f",
                              socket.name.c_str(), socket.position.x, socket.position.y,
                              socket.position.z);
                out.Print(buffer);
                PRED_LOG_INFO(Asset, "{}", buffer);
            }
        },
        "model_info <name>");

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

    console.RegisterCommand("look_sun", "Point the view at the sun (its flare, its glow), or so many degrees off it: look_sun [yaw] [pitch]",
                            [this](const std::vector<std::string>& args)
                            {
                                const glm::vec3 towards = glm::normalize(-m_scene.GetEnvironment().sunDirection);
                                m_lookYaw = std::atan2(towards.x, -towards.z) + glm::radians(args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : 0.0f);
                                m_lookPitch = std::asin(std::clamp(towards.y, -0.99f, 0.99f)) + glm::radians(args.size() >= 3 ? std::strtof(args[2].c_str(), nullptr) : 0.0f);
                                m_app->GetConsole().Print("The sun is " + std::to_string(static_cast<int>(glm::degrees(m_lookPitch))) + " degrees up");
                            });

    console.RegisterCommand("interact", "Press the interact key: use whatever the player is looking at",
                            [this](const std::vector<std::string>&) { PressInteract(); });

    console.RegisterCommand("drop", "Drop the selected item in front of the player",
                            [this](const std::vector<std::string>&) { DropSelected(); });

    console.RegisterCommand("inventory", "Open or close the inventory panel",
                            [this](const std::vector<std::string>&)
                            {
                                m_inventoryOpen = !m_inventoryOpen;
                                UpdateMouseCapture();
                            });

    console.RegisterCommand(
        "sound_bake",
        "Render the placeholder sound library (Assets/Data/Sounds) into wavs in Assets/Audio: sound_bake [name]",
        [this](const std::vector<std::string>& args)
        {
            // Turns the designed sounds into files somebody can replace.
            //
            // Nothing in the game reads the design: it plays wavs. This renders each sound into the
            // folder the game loads it from, several takes of each so nothing repeats, and replacing
            // one is dropping a recording on top. Only files named <sound>_<take>.wav are touched, so
            // a recording somebody has put in the folder under any other name is left alone.
            //
            // A command rather than a build step because it is run when the design changes, which is
            // rarely, and baking on every build would rewrite a hundred files nobody asked about.
            Console& out = m_app->GetConsole();
            const std::filesystem::path design = Paths::AssetsRoot() / "Data" / "Sounds";
            std::vector<std::string> problems;
            const std::vector<SoundPatch> patches = LoadSoundLibrary(design.string(), &problems);
            for (const std::string& problem : problems)
            {
                out.PrintError(problem);
            }
            const std::string only = args.size() >= 2 ? args[1] : std::string();

            int sounds = 0;
            int takes = 0;
            for (const SoundPatch& patch : patches)
            {
                if (!only.empty() && patch.name != only)
                {
                    continue;
                }
                const std::filesystem::path folder = Paths::AssetsRoot() / "Audio" / patch.name;
                std::error_code ec;
                std::filesystem::create_directories(folder, ec);
                for (int take = 0; take < 12; ++take)
                {
                    // Named for the sound itself, not its category: Creature/growl/growl_1.wav.
                    const std::string leaf = patch.name.substr(patch.name.find_last_of('/') + 1);
                    const std::filesystem::path target = folder / (leaf + "_" + std::to_string(take + 1) + ".wav");
                    if (take >= patch.variants)
                    {
                        // A take the design no longer asks for, from a bake when it asked for more.
                        std::filesystem::remove(target, ec);
                        continue;
                    }
                    const std::vector<uint8_t> bytes = SaveWav(RenderPatch(patch, take, 48000));
                    std::ofstream wav(target, std::ios::binary | std::ios::trunc);
                    if (!wav)
                    {
                        out.PrintError("Could not write " + target.string());
                        continue;
                    }
                    wav.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    ++takes;
                }
                ++sounds;
            }
            out.Print(std::to_string(sounds) + " sound(s), " + std::to_string(takes) +
                      " take(s) written to Assets/Audio. They are loaded at the next start.");
        });

    console.RegisterCommand(
        "mic_probe", "List the recording devices and try to open the chosen one",
        [this](const std::vector<std::string>&)
        {
            // The settings panel says "microphone shut" when the device will not open, and that is
            // all it can say: it has one bool to work with. This says why, which is the difference
            // between a setting somebody can fix and one they can only stare at.
            Console& out = m_app->GetConsole();
            const std::vector<VoiceCapture::Device> devices = VoiceCapture::Devices();
            if (devices.empty())
            {
                out.PrintError("No recording devices at all. Windows may be denying microphone "
                               "access to this application: Settings > Privacy > Microphone.");
            }
            for (const VoiceCapture::Device& device : devices)
            {
                out.Print("  " + std::to_string(device.id) + "  " + device.name +
                          (static_cast<int>(device.id) == cv_voiceDevice.Get() ? "   <- chosen"
                                                                              : ""));
            }

            // And actually open it, because a device that lists and will not open is the common
            // case and the list alone looks like everything is fine.
            VoiceCapture probe;
            VoiceCapture::Settings settings;
            settings.sampleRate = 48000;
            settings.deviceId = static_cast<uint32_t>(std::max(cv_voiceDevice.Get(), 0));
            if (!probe.Start(settings))
            {
                out.PrintError("Could not open it: " + probe.Message());
                return;
            }
            out.Print("Opened. Say something and run this again to see whether it heard you.");
            probe.Stop();
        });

    console.RegisterCommand(
        "lobby_host", "Host a game and open its lobby, as the Start button does: lobby_host [name]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() >= 2)
            {
                std::string name = args[1];
                for (size_t i = 2; i < args.size(); ++i)
                {
                    name += " " + args[i];
                }
                std::snprintf(m_lobbyName, sizeof(m_lobbyName), "%s", name.c_str());
            }
            StartHosting();
            PRED_LOG_INFO(Network, "lobby_host: {}", m_sessionMode == SessionMode::Host ? "hosting" : m_titleStatus);
        },
        "lobby_host [name]");

    console.RegisterCommand(
        "lobby_join", "Join a game by its code: lobby_join <code>",
        [this](const std::vector<std::string>& args)
        {
            uint32_t code = 0;
            if (args.size() < 2 || !EncodeLobbyCode(args[1], code))
            {
                m_app->GetConsole().PrintError("usage: lobby_join <six character code>");
                return;
            }
            JoinCode(code);
            PRED_LOG_INFO(Network, "lobby_join: {}", args[1]);
        },
        "lobby_join <code>");

    console.RegisterCommand(
        "lobby_start", "Leave the lobby for the game, as the host's Start button does",
        [this](const std::vector<std::string>&)
        {
            if (m_sessionMode == SessionMode::Host && m_inLobby)
            {
                StartTheGame();
            }
        });

    console.RegisterCommand(
        "lobby_state", "Say what the lobby is doing",
        [this](const std::vector<std::string>&)
        {
            const char* role = "none";
            switch (m_lobby.GetRole())
            {
            case LobbyClient::Role::Host: role = "host"; break;
            case LobbyClient::Role::Guest: role = "guest"; break;
            case LobbyClient::Role::None: break;
            }
            const char* state = "idle";
            switch (m_lobby.Status())
            {
            case LobbyClient::State::Resolving: state = "looking up the server"; break;
            case LobbyClient::State::Contacting: state = "asking the server"; break;
            case LobbyClient::State::Open: state = "open"; break;
            case LobbyClient::State::Punching: state = "punching through"; break;
            case LobbyClient::State::Reached: state = "reached the host"; break;
            case LobbyClient::State::Failed: state = "failed"; break;
            case LobbyClient::State::Idle: break;
            }
            const std::string line =
                std::string("lobby: ") + role + ", " + state + ", code " +
                (m_lobby.CodeText().empty() ? std::string("-") : m_lobby.CodeText()) + ", seen as " +
                m_lobby.SeenAs().ToString() + ", in lobby " + (m_inLobby ? "yes" : "no") + ", started " +
                (m_sessionMode == SessionMode::Host ? (m_host.Started() ? "yes" : "no")
                                                    : (m_client.HostStarted() ? "yes" : "no")) +
                (m_lobby.Message().empty() ? std::string() : " -- " + m_lobby.Message());
            m_app->GetConsole().Print(line);
            PRED_LOG_INFO(Network, "{}", line);
        });

    // A lobby server, and a STUN server, for this run only and without saving them. Testing on one PC
    // runs Tools/LobbyWorker/local-server.js, which is both, and points each copy of the game at it:
    // lobby_use http://127.0.0.1:8787 127.0.0.1:3478
    console.RegisterCommand(
        "lobby_use", "Use a lobby server for this run only, without saving it: lobby_use <web address> [stun host:port]",
        [](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                return;
            }
            g_lobbyOverride = args[1];
            g_stunOverride = args.size() > 2 ? args[2] : std::string();
            PRED_LOG_INFO(Network, "lobby_use: {} (STUN {})", g_lobbyOverride,
                          g_stunOverride.empty() ? std::string("the public ones") : g_stunOverride);
        },
        "lobby_use <web address> [stun host:port]");

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
        "phys_drop",
        "Spawn a dynamic prop in front of the view: phys_drop [sphere|box] [count] [speed]",
        [this](const std::vector<std::string>& args)
        {
            const bool sphere = args.size() < 2 || args[1] != "box";
            const int count = args.size() > 2 ? std::atoi(args[2].c_str()) : 1;
            // Thrown by default, which is what a person at the console wants. Zero sets it down
            // where it is, which is what a scripted run wants: a ball that has been thrown is
            // somewhere out of shot by the time anything is aimed at it.
            const float speed = args.size() > 3 ? std::strtof(args[3].c_str(), nullptr) : 6.0f;
            for (int i = 0; i < std::clamp(count, 1, 64); ++i)
            {
                SpawnProp(sphere, speed);
            }
            m_app->GetConsole().Print("Props in world: " + std::to_string(m_props.size()));
        },
        "phys_drop [sphere|box] [count] [speed]");

    console.RegisterCommand("phys_clear", "Remove every dynamic prop",
                            [this](const std::vector<std::string>&)
                            {
                                const size_t removed = m_props.size();
                                ClearProps();
                                m_app->GetConsole().Print("Removed " + std::to_string(removed) + " props");
                            });
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

void PredationGame::RegisterNetCommands()
{
    Console& console = m_app->GetConsole();

    console.RegisterCommand(
        "play", "Enter the world on your own, without a session",
        [this](const std::vector<std::string>&)
        {
            // The menu has no solo button any more, on purpose: this is a game about a team. But
            // walking into the map to look at something is most of what development is, and doing
            // that through a lobby is three steps for no reason.
            if (m_screen != Screen::Playing)
            {
                m_sessionMode = SessionMode::Offline;
                EnterWorld();
            }
        });

    console.RegisterCommand(
        "torch", "Switch the flashlight on or off",
        [this](const std::vector<std::string>&)
        {
            // Or, dead, the lamp on the drone.
            if (m_supportDrone.Active())
            {
                m_supportDrone.lightOn = !m_supportDrone.lightOn;
                return;
            }
            m_torchOn = !m_torchOn;
        });

    console.RegisterCommand(
        "menu", "Show a page of the title screen: menu <root|browse|host|settings>",
        [this](const std::vector<std::string>& args)
        {
            // So a menu page can be looked at without clicking through to it, which is what makes
            // it possible to take a screenshot of one from a headless run and see what a change to
            // it actually did.
            const std::string page = args.size() >= 2 ? args[1] : "root";
            // The pause menu is over the world rather than a page of the title screen, so it is
            // opened where it is -- and with the settings on it when asked, the other way that
            // panel is reached.
            if (page == "pause")
            {
                m_paused = true;
                m_wantMouseCaptured = false;
                m_settingsOpen = args.size() >= 3;
                m_settingsTab = args.size() >= 3 ? args[2] : std::string();
                return;
            }
            if (page == "root")
            {
                m_titlePage = TitlePage::Root;
            }
            else if (page == "browse")
            {
                m_titlePage = TitlePage::Browse;
            }
            else if (page == "host" || page == "new")
            {
                m_titlePage = TitlePage::NewCampaign;
            }
            else if (page == "load")
            {
                m_titlePage = TitlePage::LoadCampaign;
                m_campaignSlotsRead = false;
            }
            else if (page == "settings")
            {
                m_settingsOpen = true;
                // And which tab, capitalised the way the tab reads: menu settings Graphics.
                m_settingsTab = args.size() >= 3 ? args[2] : std::string();
            }
            else
            {
                m_app->GetConsole().PrintError("Unknown page. Try root, browse, host or settings.");
                return;
            }
            m_screen = Screen::Title;
            m_titleStatus.clear();
        },
        "menu <root|browse|host|settings [tab]|pause [settings tab]>");

    console.RegisterCommand("lobby_open", "Open a game and wait in its lobby, as the host page's Host button does",
                            [this](const std::vector<std::string>&) { StartHosting(); });

    console.RegisterCommand(
        "host_lan", "Open a game on this network and go in, as the host page's Start button does",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() >= 2)
            {
                // Everything after the command, not just the first word: a game called "kitchen pc"
                // is two arguments and one name.
                std::string name = args[1];
                for (size_t i = 2; i < args.size(); ++i)
                {
                    name += " " + args[i];
                }
                std::snprintf(m_lobbyName, sizeof(m_lobbyName), "%s", name.c_str());
            }
            StartHosting();
            if (m_sessionMode != SessionMode::Host)
            {
                m_app->GetConsole().PrintError(m_titleStatus);
                return;
            }
            StartTheGame();
            m_app->GetConsole().Print("Hosting \"" + LobbyName() + "\" on port " +
                                      std::to_string(m_hostPort) +
                                      (m_beacon.Running() ? ", announced on this network"
                                                          : ", NOT announced: " + m_beacon.Message()));
            // The console's own output does not reach the log file, and this is a command whose
            // whole purpose is to be checked from a headless run.
            PRED_LOG_INFO(Network, "host_lan: hosting \"{}\" on {}, announcing {}", LobbyName(),
                          m_hostPort, m_beacon.Running());
        },
        "host_lan [name]");

    console.RegisterCommand(
        "games", "List the games this machine can currently see",
        [this](const std::vector<std::string>&)
        {
            StartBrowsing();
            Console& out = m_app->GetConsole();
            const std::vector<LanLobby>& local = m_browser.Lobbies();
            out.Print("On this network: " + std::to_string(local.size()));
            for (const LanLobby& lobby : local)
            {
                const std::string line = "  " + lobby.name + "  " + lobby.address + ":" +
                                         std::to_string(lobby.port) + "  " +
                                         std::to_string(lobby.players) + "/" +
                                         std::to_string(lobby.maxPlayers);
                out.Print(line);
                PRED_LOG_INFO(Network, "games lan:{}", line);
            }
        });

    console.RegisterCommand(
        "snd", "Play a sound by name, to hear one without making it happen: snd <name> [gain]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                m_app->GetConsole().PrintError("snd <name> [gain]");
                return;
            }
            AudioEngine& audio = m_app->GetAudio();
            const SoundId sound = audio.Find(args[1]);
            if (sound == kInvalidSound)
            {
                m_app->GetConsole().PrintError("No sound called '" + args[1] + "'");
                return;
            }
            const float gain = args.size() > 2 ? std::strtof(args[2].c_str(), nullptr) : 1.0f;
            PlaySound(sound, m_camera.position, gain, 1.0f, false);
            m_app->GetConsole().Print("Played " + args[1] +
                                      (audio.HasDevice() ? "" : " (no audio device, so silently)"));
        },
        "snd <name> [gain]");


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
            // Hosting from the console at the menu should put you in the game, the same as the
            // button does. Joining does not, because it is not a game until the host answers.
            // Started, then, and said so: without it anybody joining waited in a lobby for a start
            // that had already happened.
            m_host.SetStarted(true);
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


} // namespace pred
