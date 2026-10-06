#pragma once

#include "Engine/Render/PlanetLook.h"

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

class ShaderLibrary;

// Planets, stars and rings, drawn as they look from space (Shaders/planet): for the system map, which is a picture
// of a solar system rather than a scene in the world. Everything is lit by the star at the origin and developed for
// the screen in the shader, so what it draws into is a finished picture.
class PlanetRenderer
{
public:
    bool Init(ShaderLibrary& shaders);
    void Shutdown();
    bool IsValid() const { return bgfx::isValid(m_program); }

    // Where the camera is and how bright the picture is, for everything drawn after.
    void SetCamera(const glm::vec3& eye, float exposure);
    // What it draws into: a finished picture of its own (the system map: developed for the screen, depth less), or the
    // world's picture, which is developed after (linear light) and whose depth runs as DepthConvention has it.
    void SetOutput(bool linear, uint64_t depthTest);
    // A planet or moon: `model` places, sizes and turns the unit sphere; `towardsLight` is the direction of its star.
    // `highlight` (0 to 1) rims it, for one picked out.
    void Body(bgfx::ViewId view, const glm::mat4& model, const PlanetLook& look, const glm::vec3& towardsLight,
              const glm::vec3& lightColor, float highlight, float drift);
    // A star, and the glow round it (drawn into `glowView`, after the bodies, added on).
    void Star(bgfx::ViewId view, bgfx::ViewId glowView, const glm::vec3& at, float radius, const glm::vec3& color,
              const glm::vec3& cameraRight, const glm::vec3& cameraUp, float time);
    // A planet's rings: flat round it in its own equator, from `inner` to `outer` times `radius`.
    void Rings(bgfx::ViewId view, const glm::mat4& model, const PlanetLook& look, const glm::vec3& towardsLight,
               const glm::vec3& lightColor);
    // Lines: an orbit, a route. Collected, then drawn in one go, over the bodies they are not behind.
    void Line(const glm::vec3& a, const glm::vec3& b, uint32_t abgr);
    void FlushLines(bgfx::ViewId view);

private:
    void SetLook(const PlanetLook& look, const glm::vec3& towardsLight, const glm::vec3& lightColor, float mode, float highlight,
                 float drift);

    struct Vertex
    {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
    };
    struct LineVertex
    {
        float x, y, z;
        uint32_t abgr;
    };

    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_lineProgram = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout m_layout;
    bgfx::VertexLayout m_lineLayout;
    bgfx::VertexBufferHandle m_sphereVertices = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle m_sphereIndices = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uA = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uB = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uC = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uD = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uE = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLight = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLightColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uMode = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uEye = BGFX_INVALID_HANDLE;
    glm::vec3 m_eye{0.0f};
    float m_exposure = 1.0f;
    bool m_linear = false;
    uint64_t m_depthTest = BGFX_STATE_DEPTH_TEST_LESS;
    std::vector<LineVertex> m_lines;
};

} // namespace pred
