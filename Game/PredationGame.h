#pragma once

#include "Engine/Application.h"
#include "Engine/Render/Camera.h"
#include "Engine/Scene/Scene.h"

namespace pred
{

// Milestone 2 game: a lit scene built from procedural geometry, viewed with the developer fly
// camera. The player controller replaces the fly camera in Milestone 3.
class PredationGame final : public Game
{
public:
    bool OnInit(Application& app) override;
    void OnShutdown() override;
    void OnUpdate(double dt, double alpha) override;
    void OnRender() override;
    void OnImGui() override;

private:
    // A prop whose scene transform is driven by its physics body each frame.
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

    Application* m_app = nullptr;
    Scene m_scene;
    FlyCamera m_camera;
    std::vector<DynamicProp> m_props;
    MeshHandle m_propSphereMesh;
    MeshHandle m_propBoxMesh;
    bool m_looking = false;
    double m_time = 0.0;
};

} // namespace pred
