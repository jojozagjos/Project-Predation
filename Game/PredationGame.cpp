#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Render/DebugDraw.h"

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
CVar<float> cv_flySpeed{"cam.fly_speed", 4.0f, "Fly camera speed in meters per second"};
CVar<bool> cv_showTestScene{"scene.show_test", true, "Draw the placeholder reference scene"};

} // namespace

bool PredationGame::OnInit(Application& app)
{
    m_app = &app;

    app.GetConsole().RegisterCommand("cam_reset", "Reset the fly camera",
                                     [this](const std::vector<std::string>&) { m_camera = FlyCamera{}; });
    app.GetConsole().RegisterCommand(
        "cam_pos", "Print or set the fly camera position",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() >= 4)
            {
                m_camera.position = glm::vec3(std::strtof(args[1].c_str(), nullptr),
                                              std::strtof(args[2].c_str(), nullptr),
                                              std::strtof(args[3].c_str(), nullptr));
            }
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "camera %.2f %.2f %.2f  yaw %.1f pitch %.1f", m_camera.position.x,
                          m_camera.position.y, m_camera.position.z, glm::degrees(m_camera.yaw),
                          glm::degrees(m_camera.pitch));
            m_app->GetConsole().Print(buffer);
        },
        "cam_pos [x y z]");

    PRED_LOG_INFO(Gameplay, "Game initialized: Milestone 1 reference scene. Hold RMB to look, WASD to move.");
    return true;
}

void PredationGame::OnShutdown()
{
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
    // r.fov is horizontal; convert to the vertical angle glm expects.
    const float horizontal = glm::radians(cv_fov.Get());
    m_camera.fovDegrees = glm::degrees(2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect));
    renderer.SetCamera(m_camera.View(), m_camera.Projection(aspect, renderer.HomogeneousDepth()));
}

void PredationGame::OnRender()
{
    if (!cv_showTestScene.Get())
    {
        return;
    }
    DebugDraw& draw = m_app->GetDebugDraw();

    draw.Grid(20.0f, 1.0f);
    draw.Axes(glm::mat4(1.0f), 1.0f);

    // Reference volumes: a 1 m cube, a 2 m doorway, a 0.5 m step.
    draw.Box({2.0f, 0.0f, -2.0f}, {3.0f, 1.0f, -1.0f}, Color::kYellow);
    draw.Box({-3.0f, 0.0f, -4.0f}, {-2.0f, 2.0f, -3.8f}, Color::kCyan);
    draw.Box({4.0f, 0.0f, 1.0f}, {6.0f, 0.5f, 3.0f}, Color::kGreen);

    // A moving marker proves the frame loop and timing are alive.
    const auto t = static_cast<float>(m_time);
    const glm::vec3 marker(std::sin(t) * 4.0f, 1.0f + 0.25f * std::sin(t * 3.0f), std::cos(t) * 4.0f);
    draw.Sphere(marker, 0.3f, Color::kMagenta);
    draw.Line(marker, {marker.x, 0.0f, marker.z}, Color::kGrey);
}

void PredationGame::OnImGui()
{
    if (!m_app->IsOverlayVisible())
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(8.0f, 440.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Camera %.2f %.2f %.2f", m_camera.position.x, m_camera.position.y, m_camera.position.z);
        ImGui::Text("Yaw %.1f  Pitch %.1f", glm::degrees(m_camera.yaw), glm::degrees(m_camera.pitch));
        ImGui::TextUnformatted(m_looking ? "Looking (release RMB to stop)"
                                         : "Hold RMB to look. WASD move, E/Q up/down, Shift fast, Ctrl slow.");

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
        bool showScene = cv_showTestScene.Get();
        if (ImGui::Checkbox("Show reference scene", &showScene))
        {
            cv_showTestScene.Set(showScene);
        }
    }
    ImGui::End();
}

} // namespace pred
