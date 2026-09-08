#pragma once

#include "Engine/Application.h"
#include "Engine/Render/Camera.h"

namespace pred
{

// Milestone 1 game: a fly camera over a debug-drawn reference scene.
// Real gameplay systems arrive in later milestones.
class PredationGame final : public Game
{
public:
    bool OnInit(Application& app) override;
    void OnShutdown() override;
    void OnUpdate(double dt, double alpha) override;
    void OnRender() override;
    void OnImGui() override;

private:
    Application* m_app = nullptr;
    FlyCamera m_camera;
    bool m_looking = false;
    double m_time = 0.0;
};

} // namespace pred
