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

    // Where the camera is, how bright the picture is, and how many pixels a radian of view is across (for how big a body
    // is on the screen), for everything drawn after.
    void SetCamera(const glm::vec3& eye, float exposure, float pixelsPerRadian);
    // Once a frame, before anything is drawn: a body's face is baked a strip a frame, and only one body's.
    void NewFrame();
    // What it draws into: a finished picture of its own (the system map: developed for the screen, depth less), or the
    // world's picture, which is developed after (linear light) and whose depth runs as DepthConvention has it.
    void SetOutput(bool linear, uint64_t depthTest);
    // A planet or moon: `model` places, sizes and turns the unit sphere; `towardsLight` is the direction of its star.
    // Big on the screen, it is drawn from maps of its face baked once (a few frames after it first is) rather than worked out
    // again for every pixel of it every frame, which for a world filling the screen cost more than everything else drawn.
    void Body(bgfx::ViewId view, const glm::mat4& model, const PlanetLook& look, const glm::vec3& towardsLight,
              const glm::vec3& lightColor, float drift);
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
    void SetLook(const PlanetLook& look, const glm::vec3& towardsLight, const glm::vec3& lightColor, float mode, float drift);
    // A body's baked face: its two maps, the target they are drawn into, how many rows of them are done, and when it was
    // last wanted (the least wanted goes first when there are too many).
    struct Baked
    {
        uint64_t key = 0;
        bgfx::TextureHandle surface = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle detail = BGFX_INVALID_HANDLE;
        bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE;
        int rows = 0;
        uint64_t doneFrame = 0;
        uint64_t wanted = 0;
    };
    // The baked face of a body that looks like this: made if it is not there; a strip more of it baked if this frame has
    // not baked one yet; null when there are no maps to be had.
    Baked* BakedFor(const PlanetLook& look);
    void BakeStrip(Baked& baked, const PlanetLook& look);
    void Forget(Baked& baked);

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
    bgfx::ProgramHandle m_bakeProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sSurface = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sDetail = BGFX_INVALID_HANDLE;
    std::vector<Baked> m_baked;
    uint64_t m_frame = 1;
    bool m_bakedThisFrame = false;
    float m_pixelsPerRadian = 0.0f;
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
