#pragma once

#include "Engine/Render/Mesh.h"
#include "Engine/Render/Waves.h"

#include <bgfx/bgfx.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <vector>

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
    static constexpr int kMostRipples = 16;
    static constexpr int kMostLamps = 8; // the lamps given back by it (the scene's, but for the torch)

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
        Waves waves;                         // what moves its surface (water only); none: flat
        // Where its mesh is finest (SiteTerrain::WaterMesh): its middle (x, z), and how far out from it every way the lines
        // stay a few metres apart -- past that a wave too short for the lines is let go of, rather than drawn jagged.
        glm::vec2 fineMiddle{0.0f};
        float fineReach = 0.0f;
        // Under a roof (a flooded building's): no sky over it to give back nor sun on it -- the dark of the room in it, and seen
        // from under it, not the bright of the day coming down through it.
        bool roofed = false;
        // Rings going out on it from where something moved in it -- somebody wading, swimming, stepping in: where (x, z), how
        // long ago (seconds) and how hard (0 to 1). The nearest kMostRipples.
        std::vector<glm::vec4> ripples;
        // How far its mesh is raised as it is drawn, metres: a lockout's sheet, raised to as full of the sea as it is now.
        float lift = 0.0f;
        // What stands up through it -- a platform's legs: where (x, z) and how wide (radius), the sea breaking white round each.
        std::vector<glm::vec3> posts;
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
    bgfx::UniformHandle m_uRim = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uHaze = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTint = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTorchAt = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTorchAim = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTorchLight = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uKind = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLift = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uWaves = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uWaveParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uRipples = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uPosts = BGFX_INVALID_HANDLE;
    static constexpr int kMostPosts = 8;
    bgfx::UniformHandle m_uLamps = BGFX_INVALID_HANDLE;
};

} // namespace pred
