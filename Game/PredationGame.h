#pragma once

#include "Engine/Application.h"
#include "Engine/Render/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Game/Player/PlayerBody.h"
#include "Game/Player/PlayerController.h"

#include <glm/vec3.hpp>

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
    void UpdateMouseCapture();
    void ReloadPlayerConfig();
    void DrawPlayerPanel();

    Application* m_app = nullptr;
    Scene m_scene;

    PlayerController m_player;
    PlayerBody m_body;
    glm::vec3 m_spawnPoint{0.0f, 0.5f, 18.0f};

    FlyCamera m_camera;
    bool m_flyMode = false;

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
    bool m_crouchToggleState = false;
    bool m_proneToggleState = false;
    bool m_sprintToggleState = false;

    std::vector<DynamicProp> m_props;
    MeshHandle m_propSphereMesh;
    MeshHandle m_propBoxMesh;
    double m_time = 0.0;
};

} // namespace pred
