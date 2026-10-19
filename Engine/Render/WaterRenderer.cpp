#include "Engine/Render/WaterRenderer.h"

#include "Engine/Render/ShaderLibrary.h"
#include "Engine/Scene/Scene.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

bool WaterRenderer::Init(ShaderLibrary& shaders)
{
    m_program = shaders.LoadProgram("vs_water", "fs_water");
    m_uEye = bgfx::createUniform("u_waterEye", bgfx::UniformType::Vec4);
    m_uSky = bgfx::createUniform("u_waterSky", bgfx::UniformType::Vec4);
    m_uHorizon = bgfx::createUniform("u_waterHorizon", bgfx::UniformType::Vec4);
    m_uSun = bgfx::createUniform("u_waterSun", bgfx::UniformType::Vec4);
    m_uSunColor = bgfx::createUniform("u_waterSunColor", bgfx::UniformType::Vec4);
    m_uAmbient = bgfx::createUniform("u_waterAmbient", bgfx::UniformType::Vec4);
    m_uFog = bgfx::createUniform("u_waterFog", bgfx::UniformType::Vec4);
    m_uFogParams = bgfx::createUniform("u_waterFogParams", bgfx::UniformType::Vec4);
    m_uTint = bgfx::createUniform("u_waterTint", bgfx::UniformType::Vec4);
    m_uTorchAt = bgfx::createUniform("u_waterTorchAt", bgfx::UniformType::Vec4);
    m_uTorchAim = bgfx::createUniform("u_waterTorchAim", bgfx::UniformType::Vec4);
    m_uTorchLight = bgfx::createUniform("u_waterTorchLight", bgfx::UniformType::Vec4);
    return bgfx::isValid(m_program);
}

void WaterRenderer::Shutdown()
{
    for (bgfx::UniformHandle* handle : {&m_uEye, &m_uSky, &m_uHorizon, &m_uSun, &m_uSunColor, &m_uAmbient, &m_uFog, &m_uFogParams, &m_uTint, &m_uTorchAt, &m_uTorchAim, &m_uTorchLight})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
        }
        *handle = BGFX_INVALID_HANDLE;
    }
    // (The program belongs to the shader library.)
    m_program = BGFX_INVALID_HANDLE;
}

void WaterRenderer::Draw(bgfx::ViewId view, uint64_t depthTest, const Mesh& mesh, const Environment& environment, const glm::vec3& eye,
                         const Look& look)
{
    if (!bgfx::isValid(m_program) || !mesh.IsValid() || !bgfx::isValid(mesh.vertexBuffer))
    {
        return;
    }
    // The sky as SkyRenderer draws it, so what the water shows of it is what is over it: the zenith from the light from the
    // sky, drawn into the haze as the air thickens; the horizon from the fog.
    const float haze = std::clamp(environment.skyHaze, 0.0f, 1.0f);
    const glm::vec3 open{environment.ambientSky.r * 1.9f, environment.ambientSky.g * 1.9f, environment.ambientSky.b * 2.1f};
    const glm::vec3 hazed = environment.fogColor * 2.6f + glm::vec3(0.06f, 0.07f, 0.08f);
    const glm::vec3 zenith = glm::mix(open, hazed * 0.92f, haze) * look.skyBrightness;
    const glm::vec3 horizon = hazed * look.skyBrightness;
    const glm::vec3 towardsSun = glm::normalize(-environment.sunDirection);
    // How much of the sun reaches the water: none below the horizon, little under a heavy sky.
    const float sunReach = std::clamp(environment.sunIntensity, 0.0f, 2.0f) * glm::smoothstep(-0.02f, 0.08f, towardsSun.y) *
                           (1.0f - 0.8f * environment.cloudCover * std::clamp(environment.cloudDark + 0.4f, 0.0f, 1.0f));
    const float eyeAt[4] = {eye.x, eye.y, eye.z, look.seconds};
    const float sky[4] = {zenith.r, zenith.g, zenith.b, std::clamp(look.rain, 0.0f, 1.0f)};
    const float horizonUniform[4] = {horizon.r, horizon.g, horizon.b, std::max(look.wind, 0.0f)};
    const float sun[4] = {towardsSun.x, towardsSun.y, towardsSun.z, sunReach};
    const float sunColor[4] = {environment.sunColor.r, environment.sunColor.g, environment.sunColor.b, look.linear ? 1.0f : 0.0f};
    const glm::vec3 ambient = environment.ambientSky * 0.8f + environment.ambientGround * 0.2f;
    const float ambientUniform[4] = {ambient.r, ambient.g, ambient.b, environment.exposure};
    const float fog[4] = {environment.fogColor.r, environment.fogColor.g, environment.fogColor.b, environment.eyeOutdoors};
    const float fogParams[4] = {environment.fogStart, environment.fogEnd, std::max(environment.lightning, 0.0f), 0.0f};
    const float tint[4] = {look.tint.r, look.tint.g, look.tint.b, look.clarity};
    bgfx::setUniform(m_uEye, eyeAt);
    bgfx::setUniform(m_uSky, sky);
    bgfx::setUniform(m_uHorizon, horizonUniform);
    bgfx::setUniform(m_uSun, sun);
    bgfx::setUniform(m_uSunColor, sunColor);
    bgfx::setUniform(m_uAmbient, ambientUniform);
    bgfx::setUniform(m_uFog, fog);
    bgfx::setUniform(m_uFogParams, fogParams);
    bgfx::setUniform(m_uTint, tint);
    // The torch, when it is one (the first light, a cone), as SceneRenderer packs it.
    {
        float at[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float aim[4] = {0.0f, -1.0f, 0.0f, 1.0f};
        float light[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const PunctualLight& torch = environment.lights[0];
        if (torch.intensity > 0.0f && torch.range > 0.0f && torch.outerAngle < 89.0f)
        {
            const glm::vec3 way = glm::length(torch.direction) > 1.0e-4f ? glm::normalize(torch.direction) : glm::vec3(0.0f, -1.0f, 0.0f);
            at[0] = torch.position.x;
            at[1] = torch.position.y;
            at[2] = torch.position.z;
            at[3] = torch.range;
            aim[0] = way.x;
            aim[1] = way.y;
            aim[2] = way.z;
            aim[3] = std::cos(glm::radians(std::clamp(std::max(torch.outerAngle, torch.innerAngle + 0.5f), 0.0f, 180.0f)));
            light[0] = torch.color.r * torch.intensity;
            light[1] = torch.color.g * torch.intensity;
            light[2] = torch.color.b * torch.intensity;
            light[3] = std::cos(glm::radians(std::clamp(torch.innerAngle, 0.0f, 180.0f)));
        }
        bgfx::setUniform(m_uTorchAt, at);
        bgfx::setUniform(m_uTorchAim, aim);
        bgfx::setUniform(m_uTorchLight, light);
    }
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(mesh.indexBuffer);
    bgfx::setState(BGFX_STATE_WRITE_RGB | depthTest | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    bgfx::submit(view, m_program);
}

} // namespace pred
