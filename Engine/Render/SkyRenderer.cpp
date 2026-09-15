#include "Engine/Render/SkyRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/ShaderLibrary.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace pred
{

bool SkyRenderer::Init(ShaderLibrary& shaders)
{
    m_program = shaders.LoadProgram("vs_sky", "fs_sky");
    if (!bgfx::isValid(m_program))
    {
        PRED_LOG_WARN(Render, "SkyRenderer: no sky program; the background stays flat");
        return false;
    }

    m_layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    // One triangle rather than two, covering the screen twice over. A quad has a seam down its
    // diagonal where the two triangles meet, and every pixel along it is shaded twice.
    static const float kCorners[] = {-1.0f, -1.0f, 0.0f, 3.0f, -1.0f, 0.0f, -1.0f, 3.0f, 0.0f};
    m_triangle = bgfx::createVertexBuffer(bgfx::makeRef(kCorners, sizeof(kCorners)), m_layout);

    m_uRays = bgfx::createUniform("u_skyRays", bgfx::UniformType::Mat4);
    m_uZenith = bgfx::createUniform("u_skyZenith", bgfx::UniformType::Vec4);
    m_uHorizon = bgfx::createUniform("u_skyHorizon", bgfx::UniformType::Vec4);
    m_uGround = bgfx::createUniform("u_skyGround", bgfx::UniformType::Vec4);
    m_uSun = bgfx::createUniform("u_skySun", bgfx::UniformType::Vec4);
    m_uSunColor = bgfx::createUniform("u_skySunColor", bgfx::UniformType::Vec4);
    m_uGrade = bgfx::createUniform("u_skyGrade", bgfx::UniformType::Vec4);
    return true;
}

void SkyRenderer::Shutdown()
{
    const bgfx::UniformHandle uniforms[] = {m_uRays, m_uZenith, m_uHorizon,
                                            m_uGround, m_uSun, m_uSunColor, m_uGrade};
    for (const bgfx::UniformHandle handle : uniforms)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    m_uRays = m_uZenith = m_uHorizon = m_uGround = BGFX_INVALID_HANDLE;
    m_uSun = m_uSunColor = m_uGrade = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(m_triangle))
    {
        bgfx::destroy(m_triangle);
    }
    m_triangle = BGFX_INVALID_HANDLE;
    m_program = BGFX_INVALID_HANDLE; // owned by the ShaderLibrary
}

void SkyRenderer::Draw(bgfx::ViewId view, const Environment& environment, const glm::mat4& viewMatrix,
                       const glm::mat4& projection)
{
    if (!IsValid())
    {
        return;
    }

    // Clip space back to a world direction, with the camera's position taken out of the view first.
    // Leaving it in makes the sky a thing at the origin that the player can walk towards.
    glm::mat4 rotationOnly = viewMatrix;
    rotationOnly[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::mat4 rays = glm::inverse(projection * rotationOnly);
    bgfx::setUniform(m_uRays, glm::value_ptr(rays));

    const glm::vec3 towardsSun = glm::normalize(-environment.sunDirection);
    // The sky is lit by the same numbers the surfaces are, so the two cannot drift apart: the zenith
    // is the ambient sky colour opened up, the horizon sits between that and the fog the distance
    // fades into, and below it is the ground's own bounce.
    const float zenith[4] = {environment.ambientSky.r * 1.9f, environment.ambientSky.g * 1.9f,
                             environment.ambientSky.b * 2.1f, 0.0f};
    const float horizon[4] = {environment.fogColor.r * 2.6f + 0.06f,
                              environment.fogColor.g * 2.6f + 0.07f,
                              environment.fogColor.b * 2.6f + 0.08f, 0.0f};
    const float ground[4] = {environment.ambientGround.r * 1.2f, environment.ambientGround.g * 1.2f,
                             environment.ambientGround.b * 1.2f, 0.0f};
    const float sun[4] = {towardsSun.x, towardsSun.y, towardsSun.z, 220.0f};
    const float sunColor[4] = {environment.sunColor.r, environment.sunColor.g, environment.sunColor.b,
                               0.55f};
    bgfx::setUniform(m_uZenith, zenith);
    bgfx::setUniform(m_uHorizon, horizon);
    bgfx::setUniform(m_uGround, ground);
    bgfx::setUniform(m_uSun, sun);
    bgfx::setUniform(m_uSunColor, sunColor);
    const float grade[4] = {environment.exposure, environment.contrast, 0.0f, 0.0f};
    bgfx::setUniform(m_uGrade, grade);

    bgfx::setVertexBuffer(0, m_triangle);
    // No depth write and no depth test: it is drawn first and everything else covers it. Writing
    // depth at the far plane would be harmless and testing against it costs a comparison per pixel
    // for an answer that is always the same.
    bgfx::setState(BGFX_STATE_WRITE_RGB);
    bgfx::submit(view, m_program);
}

} // namespace pred
