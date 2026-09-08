#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/TestMap.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

    // Editing player.json on disk applies immediately, without a rebuild or a restart.
    app.GetFileWatcher().Watch(PlayerConfigPath(),
                               [this](const std::filesystem::path&) { ReloadPlayerConfig(); });

    // Yaw 0 looks down -Z, which is where the test map is from the spawn point.
    m_lookYaw = 0.0f;
    m_lookPitch = 0.0f;
    m_camera.position = {0.0f, 3.2f, 22.0f};

    RegisterCommands();
    UpdateMouseCapture();

    PRED_LOG_INFO(Gameplay,
                  "Milestone 3 ready. WASD move, Space jump, Ctrl crouch, Z prone, Shift sprint, "
                  "Alt walk. F toggles the fly camera, R respawns, Escape releases the mouse.");
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
                m_cameraMode = CameraMode::FirstPerson;
            }
            else if (which == "third")
            {
                m_cameraMode = CameraMode::ThirdPerson;
            }
            else if (which == "fly")
            {
                m_cameraMode = CameraMode::Fly;
            }
            else
            {
                m_app->GetConsole().PrintError("usage: camera <first|third|fly>");
                return;
            }
            m_app->GetConsole().Print("Camera: " + which);
        },
        "camera <first|third|fly>");

    console.RegisterCommand("fly", "Toggle the free-flying inspection camera",
                            [this](const std::vector<std::string>&)
                            {
                                m_cameraMode = m_cameraMode == CameraMode::Fly ? CameraMode::FirstPerson
                                                                               : CameraMode::Fly;
                            });

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

void PredationGame::OnShutdown()
{
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
    const bool shouldCapture = m_wantMouseCaptured && m_windowFocused && !m_app->IsConsoleOpen();

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
    m_lookYaw += delta.x * sensitivity;
    m_lookPitch += (cv_invertY.Get() ? delta.y : -delta.y) * sensitivity;

    const float limit = glm::radians(m_player.Config().maxPitchDegrees);
    m_lookPitch = std::clamp(m_lookPitch, -limit, limit);
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

    // The latch carries a press that landed between two fixed ticks.
    result.jump = m_jumpLatch;
    m_jumpLatch = false;

    result.sprint = cv_sprintToggle.Get() ? m_sprintToggleState : input.IsActionDown("sprint");
    result.crouchHeld = cv_crouchToggle.Get() ? m_crouchToggleState : input.IsActionDown("crouch");
    result.proneHeld = cv_crouchToggle.Get() ? m_proneToggleState : input.IsActionDown("prone");
    result.walk = input.IsActionDown("walk");

    // Debug override from the `stance` console command.
    result.crouchHeld = result.crouchHeld || m_forceCrouch;
    result.proneHeld = result.proneHeld || m_forceProne;
    return result;
}

void PredationGame::OnFixedUpdate(double fixedDt)
{
    PlayerInput input = BuildPlayerInput();
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
    m_player.Step(input, static_cast<float>(fixedDt));
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

    if (!app.IsConsoleOpen())
    {
        if (input.WasActionPressed("jump"))
        {
            m_jumpLatch = true;
        }
        if (input.WasActionPressed("crouch") && cv_crouchToggle.Get())
        {
            m_crouchToggleState = !m_crouchToggleState;
            m_proneToggleState = false;
        }
        if (input.WasActionPressed("prone") && cv_crouchToggle.Get())
        {
            m_proneToggleState = !m_proneToggleState;
            m_crouchToggleState = false;
        }
        if (input.WasActionPressed("sprint") && cv_sprintToggle.Get())
        {
            m_sprintToggleState = !m_sprintToggleState;
        }
        if (input.WasActionPressed("toggle_fly"))
        {
            // Cycles first person, third person, fly. Third person exists so the body animation can
            // actually be watched, which is impossible from inside the head.
            m_cameraMode = m_cameraMode == CameraMode::FirstPerson  ? CameraMode::ThirdPerson
                           : m_cameraMode == CameraMode::ThirdPerson ? CameraMode::Fly
                                                                     : CameraMode::FirstPerson;
        }
        if (input.WasActionPressed("respawn"))
        {
            m_player.Respawn(m_spawnPoint);
        }
        if (input.WasActionPressed("quit_capture"))
        {
            // Escape toggles, so the same key both frees the pointer for the debug UI and puts it
            // back. Only releasing it left no way back except clicking, which was easy to miss.
            m_wantMouseCaptured = !m_wantMouseCaptured;
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
        m_camera.yaw = m_lookYaw;
        m_camera.pitch = m_lookPitch;
        m_camera.moveSpeed = cv_flySpeed.Get();
        m_camera.Update(input, deltaSeconds, false); // look is applied above, this only moves
        view = m_camera.View();
        viewPosition = m_camera.position;
        break;

    case CameraMode::ThirdPerson:
    {
        // Orbits the player at a fixed offset along the look direction. It follows the interpolated
        // position for the same reason the body does: anything using the raw simulation state
        // steps at the tick rate and judders.
        const PlayerView& playerView = m_player.View();
        const glm::vec3 focus =
            playerView.renderPosition + glm::vec3(0.0f, m_thirdPersonHeight, 0.0f);
        const float cp = std::cos(m_lookPitch);
        const glm::vec3 lookDirection{std::sin(m_lookYaw) * cp, std::sin(m_lookPitch),
                                      -std::cos(m_lookYaw) * cp};

        // Pull the camera in when something is in the way, so it does not end up inside a wall or a
        // pillar with the player hidden behind it.
        constexpr float kCameraSkin = 0.25f;
        float distance = m_thirdPersonDistance;
        const RayHit blocked =
            app.GetPhysics().RayCast(focus, -lookDirection, m_thirdPersonDistance + kCameraSkin);
        if (blocked)
        {
            distance = std::max(0.35f, blocked.distance - kCameraSkin);
        }

        viewPosition = focus - lookDirection * distance;
        view = glm::lookAtRH(viewPosition, focus, glm::vec3(0.0f, 1.0f, 0.0f));
        break;
    }

    case CameraMode::FirstPerson:
    default:
        view = m_player.View().ViewMatrix();
        viewPosition = m_player.View().eyePosition;
        break;
    }

    // The body follows the simulation every frame. Its head is only drawn from the fly camera,
    // because in first person the camera sits inside it.
    m_body.Tuning().hideHead = m_cameraMode == CameraMode::FirstPerson;
    m_body.Update(m_scene, m_player.State(), m_player.View(), m_player.Config(), app.GetPhysics(),
                  deltaSeconds);

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
    const glm::vec3 viewPosition = m_cameraMode == CameraMode::Fly ? m_camera.position : m_player.View().eyePosition;
    app.GetSceneRenderer().Draw(Renderer::kViewMain, m_scene, app.GetMeshes(), viewPosition);
    DrawDebugOverlays();
}

void PredationGame::DrawDebugOverlays()
{
    DebugDraw& draw = m_app->GetDebugDraw();

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
                const glm::vec3 center = transform.position + mesh->bounds.Center() * transform.scale;
                const glm::vec3 extents = mesh->bounds.HalfExtents() * transform.scale;
                draw.Box(center - extents, center + extents, Color::kCyan);
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
        ImGui::SliderFloat("Bob amount", &config.bobAmount, 0.0f, 0.12f, "%.3f m");
        ImGui::SliderFloat("Bob stride", &config.bobStrideLength, 0.5f, 4.0f, "%.2f m");
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
        ImGui::SliderFloat("Stride length", &body.strideLength, 0.6f, 3.0f, "%.2f m");
        ImGui::SliderFloat("Step height", &body.stepHeight, 0.0f, 0.4f, "%.3f m");
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

void PredationGame::OnImGui()
{
    if (!m_app->IsOverlayVisible())
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(8.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Player"))
    {
        if (m_cameraMode == CameraMode::Fly)
        {
            ImGui::TextUnformatted("Fly camera active. Press F for the player camera.");
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
