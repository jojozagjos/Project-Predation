#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Render/DebugDraw.h"
#include "Game/World/TestMap.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

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
CVar<float> cv_flySpeed{"cam.fly_speed", 6.0f, "Fly camera speed in meters per second"};
CVar<bool> cv_showGrid{"debug.show_grid", true, "Draw the reference grid"};
CVar<bool> cv_wireframe{"r.wireframe", false, "Draw scene meshes as wireframe"};
CVar<float> cv_fogStart{"r.fog_start", 12.0f, "Fog start distance in meters"};
CVar<float> cv_fogEnd{"r.fog_end", 90.0f, "Fog end distance in meters"};
CVar<float> cv_sunIntensity{"r.sun_intensity", 2.2f, "Directional light intensity"};

} // namespace

bool PredationGame::OnInit(Application& app)
{
    m_app = &app;

    BuildTestMap(m_scene, app.GetMeshes());

    // Start looking at the test map from a comfortable height.
    m_camera.position = {0.0f, 3.2f, 14.0f};
    m_camera.yaw = 0.0f;
    m_camera.pitch = glm::radians(-8.0f);

    RegisterCommands();

    PRED_LOG_INFO(Gameplay, "Milestone 2 scene ready. Hold RMB to look, WASD to move, F3 for the overlay.");
    return true;
}

void PredationGame::RegisterCommands()
{
    Console& console = m_app->GetConsole();

    console.RegisterCommand("cam_reset", "Reset the fly camera to its starting position",
                            [this](const std::vector<std::string>&)
                            {
                                m_camera = FlyCamera{};
                                m_camera.position = {0.0f, 3.2f, 14.0f};
                            });

    console.RegisterCommand(
        "cam_pos", "Print or set the fly camera position",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() >= 4)
            {
                m_camera.position = glm::vec3(std::strtof(args[1].c_str(), nullptr),
                                              std::strtof(args[2].c_str(), nullptr),
                                              std::strtof(args[3].c_str(), nullptr));
            }
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "camera %.2f %.2f %.2f  yaw %.1f pitch %.1f",
                          m_camera.position.x, m_camera.position.y, m_camera.position.z,
                          glm::degrees(m_camera.yaw), glm::degrees(m_camera.pitch));
            m_app->GetConsole().Print(buffer);
        },
        "cam_pos [x y z]");

    console.RegisterCommand("scene_stats", "Print scene and mesh statistics",
                            [this](const std::vector<std::string>&)
                            {
                                const SceneRenderer::Stats& stats = m_app->GetSceneRenderer().LastStats();
                                char buffer[200];
                                std::snprintf(buffer, sizeof(buffer),
                                              "entities %zu   meshes in library %zu   submitted %zu   "
                                              "triangles %zu",
                                              m_scene.EntityCount(), m_app->GetMeshes().Count(),
                                              stats.meshesSubmitted, stats.trianglesSubmitted);
                                m_app->GetConsole().Print(buffer);
                            });

    console.RegisterCommand("scene_rebuild", "Clear and rebuild the test map",
                            [this](const std::vector<std::string>&)
                            {
                                m_scene.Clear();
                                BuildTestMap(m_scene, m_app->GetMeshes());
                                m_app->GetConsole().Print("Test map rebuilt");
                            });
}

void PredationGame::OnShutdown()
{
    m_scene.Clear();
    PRED_LOG_INFO(Gameplay, "Game shutdown");
}

void PredationGame::OnUpdate(double dt, double /*alpha*/)
{
    m_time += dt;

    Application& app = *m_app;
    Input& input = app.GetInput();
    Window& window = app.GetWindow();
    Renderer& renderer = app.GetRenderer();

    const bool wantLook = input.IsActionDown("look") && !app.IsConsoleOpen();
    if (wantLook != m_looking)
    {
        m_looking = wantLook;
        window.SetRelativeMouse(m_looking);
    }

    m_camera.mouseSensitivity = cv_mouseSensitivity.Get();
    m_camera.moveSpeed = cv_flySpeed.Get();
    m_camera.Update(input, static_cast<float>(dt), m_looking);

    const float aspect = renderer.Height() > 0
                             ? static_cast<float>(renderer.Width()) / static_cast<float>(renderer.Height())
                             : 16.0f / 9.0f;
    // r.fov is horizontal; convert to the vertical angle the projection wants.
    const float horizontal = glm::radians(cv_fov.Get());
    m_camera.fovDegrees = glm::degrees(2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect));
    renderer.SetCamera(m_camera.View(), m_camera.Projection(aspect, renderer.HomogeneousDepth()));

    // Live-tunable environment, so lighting can be dialled in without a rebuild.
    Environment& environment = m_scene.GetEnvironment();
    environment.fogStart = cv_fogStart.Get();
    environment.fogEnd = cv_fogEnd.Get();
    environment.sunIntensity = cv_sunIntensity.Get();
    renderer.SetClearColor(0x11131aff);

    app.GetSceneRenderer().SetWireframe(cv_wireframe.Get());
    app.SetEntityCount(m_scene.EntityCount());
}

void PredationGame::OnRender()
{
    Application& app = *m_app;
    app.GetSceneRenderer().Draw(Renderer::kViewMain, m_scene, app.GetMeshes(), m_camera.position);
    DrawDebugOverlays();
}

void PredationGame::DrawDebugOverlays()
{
    DebugDraw& draw = m_app->GetDebugDraw();

    if (cv_showGrid.Get())
    {
        // Lifted off the floor so it does not fight with the ground plane for depth.
        draw.Grid(24.0f, 1.0f, 0.02f);
        draw.Axes(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.02f, 0.0f)), 1.5f);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Rendering))
    {
        // Bounding boxes of everything the scene renderer will submit.
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

void PredationGame::OnImGui()
{
    if (!m_app->IsOverlayVisible())
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(8.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Camera %.2f %.2f %.2f", m_camera.position.x, m_camera.position.y, m_camera.position.z);
        ImGui::Text("Yaw %.1f  Pitch %.1f", glm::degrees(m_camera.yaw), glm::degrees(m_camera.pitch));
        ImGui::TextUnformatted(m_looking ? "Looking (release RMB to stop)"
                                         : "Hold RMB to look. WASD move, E/Q up/down, Shift fast, Ctrl slow.");
        ImGui::Separator();

        const SceneRenderer::Stats& stats = m_app->GetSceneRenderer().LastStats();
        ImGui::Text("Entities %zu   submitted %zu   triangles %zu", m_scene.EntityCount(),
                    stats.meshesSubmitted, stats.trianglesSubmitted);

        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            float fov = cv_fov.Get();
            if (ImGui::SliderFloat("FOV (horizontal)", &fov, 50.0f, 120.0f, "%.0f"))
            {
                cv_fov.Set(fov);
            }
            float sensitivity = cv_mouseSensitivity.Get();
            if (ImGui::SliderFloat("Sensitivity", &sensitivity, 0.02f, 0.5f, "%.3f"))
            {
                cv_mouseSensitivity.Set(sensitivity);
            }
            float speed = cv_flySpeed.Get();
            if (ImGui::SliderFloat("Fly speed", &speed, 0.5f, 30.0f, "%.1f m/s"))
            {
                cv_flySpeed.Set(speed);
            }
        }

        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
        {
            Environment& environment = m_scene.GetEnvironment();

            float sunIntensity = cv_sunIntensity.Get();
            if (ImGui::SliderFloat("Sun intensity", &sunIntensity, 0.0f, 8.0f, "%.2f"))
            {
                cv_sunIntensity.Set(sunIntensity);
            }
            ImGui::SliderFloat3("Sun direction", &environment.sunDirection.x, -1.0f, 1.0f, "%.2f");
            ImGui::ColorEdit3("Sun colour", &environment.sunColor.r);
            ImGui::ColorEdit3("Ambient sky", &environment.ambientSky.r);
            ImGui::ColorEdit3("Ambient ground", &environment.ambientGround.r);
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
        }

        if (ImGui::CollapsingHeader("Debug draw"))
        {
            bool grid = cv_showGrid.Get();
            if (ImGui::Checkbox("Reference grid", &grid))
            {
                cv_showGrid.Set(grid);
            }
            bool wireframe = cv_wireframe.Get();
            if (ImGui::Checkbox("Wireframe", &wireframe))
            {
                cv_wireframe.Set(wireframe);
            }
            ImGui::TextUnformatted("Mesh bounds: enable the RENDERING debug category.");
        }
    }
    ImGui::End();
}

} // namespace pred
