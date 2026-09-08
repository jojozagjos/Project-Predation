#pragma once

#include <bgfx/bgfx.h>
#include <glm/vec3.hpp>

#include <cstddef>

namespace pred
{

class Scene;
class MeshLibrary;
class ShaderLibrary;

// Draws the scene's mesh renderers with a single forward lit pass.
//
// One directional light plus hemispheric ambient and distance fog. Punctual lights, shadow maps and
// post-processing arrive in later milestones; this exists so geometry reads correctly while the
// player controller is built.
class SceneRenderer
{
public:
    struct Stats
    {
        size_t meshesSubmitted = 0;
        size_t trianglesSubmitted = 0;
    };

    bool Init(ShaderLibrary& shaders);
    void Shutdown();

    void Draw(bgfx::ViewId view, const Scene& scene, const MeshLibrary& meshes, const glm::vec3& cameraPosition);

    const Stats& LastStats() const { return m_stats; }
    bool WireframeEnabled() const { return m_wireframe; }
    void SetWireframe(bool enabled) { m_wireframe = enabled; }

private:
    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE;

    bgfx::UniformHandle m_uBaseColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uMaterialParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uEmissive = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLightDirection = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLightColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAmbientSky = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAmbientGround = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uFogColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uFogParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uCameraPosition = BGFX_INVALID_HANDLE;

    Stats m_stats;
    bool m_wireframe = false;
};

} // namespace pred
