#pragma once

#include "Engine/Render/Mesh.h"

#include <bgfx/bgfx.h>
#include <glm/vec3.hpp>

namespace pred
{

class ShaderLibrary;
struct Environment;

// Open water -- a sea, a lake; or lava, acid or ice in its place -- drawn over the world's picture, blended (fs_water): the sky in it, a glint of the sun, waves
// with the wind and rings in the rain, clear in the shallows and darker with depth, foam at the shore, fogged. Its surface
// is a mesh laid out in the world already, each corner's uv.x how deep the water is there (SiteTerrain::WaterMesh).
class WaterRenderer
{
public:
    bool Init(ShaderLibrary& shaders);
    void Shutdown();

    // How it looks this frame.
    struct Look
    {
        glm::vec3 tint{0.03f, 0.06f, 0.07f}; // its own colour, deep
        float clarity = 2.5f;                // metres down to see a third of the way to the ground
        float rain = 0.0f;                   // how hard it rains on it, 0 to 1
        float wind = 3.0f;                   // metres a second
        float seconds = 0.0f;                // the waves' clock
        float skyBrightness = 1.0f;          // as the sky is drawn (SkyRenderer::SetBrightness), so what it shows matches
        bool linear = true;                  // drawing linear light for post-processing to finish
        int kind = 0;                        // what it is: 0 water, 1 lava, 2 acid, 3 ice (fs_water)
        bool below = false;                  // seen from under it: its underside, bright and murky
    };

    // Into `view` (one drawn after the sky, into the world's picture, with its camera), tested against the world's depth in
    // `depthTest`'s convention and writing none.
    void Draw(bgfx::ViewId view, uint64_t depthTest, const Mesh& mesh, const Environment& environment, const glm::vec3& eye, const Look& look);

private:
    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uEye = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSky = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uHorizon = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSun = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSunColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAmbient = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uFog = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uFogParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTint = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTorchAt = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTorchAim = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTorchLight = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uKind = BGFX_INVALID_HANDLE;
};

} // namespace pred
