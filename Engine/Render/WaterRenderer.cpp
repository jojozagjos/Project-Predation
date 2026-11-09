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
    m_uKind = bgfx::createUniform("u_waterKind", bgfx::UniformType::Vec4);
    m_uLift = bgfx::createUniform("u_waterLift", bgfx::UniformType::Vec4);
    m_uWaves = bgfx::createUniform("u_waves", bgfx::UniformType::Vec4, Waves::kMost);
    m_uWaveParams = bgfx::createUniform("u_waveParams", bgfx::UniformType::Vec4);
    m_uRipples = bgfx::createUniform("u_waterRipples", bgfx::UniformType::Vec4, kMostRipples);
    m_uLamps = bgfx::createUniform("u_waterLamps", bgfx::UniformType::Vec4, kMostLamps * 2);
    return bgfx::isValid(m_program);
}

void WaterRenderer::Shutdown()
{
    for (bgfx::UniformHandle* handle : {&m_uEye, &m_uSky, &m_uHorizon, &m_uSun, &m_uSunColor, &m_uAmbient, &m_uFog, &m_uFogParams, &m_uTint, &m_uTorchAt, &m_uTorchAim, &m_uTorchLight, &m_uKind, &m_uLift, &m_uWaves, &m_uWaveParams, &m_uRipples, &m_uLamps})
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
    // (Under a roof, none of it: the room's dark.)
    const float roof = look.roofed ? 0.08f : 1.0f;
    const glm::vec3 zenith = glm::mix(open, hazed * 0.92f, haze) * look.skyBrightness * roof;
    const glm::vec3 horizon = hazed * look.skyBrightness * roof;
    const glm::vec3 towardsSun = glm::normalize(-environment.sunDirection);
    // How much of the sun reaches the water: none below the horizon, little under a heavy sky.
    const float sunReach = std::clamp(environment.sunIntensity, 0.0f, 2.0f) * glm::smoothstep(-0.02f, 0.08f, towardsSun.y) *
                           (1.0f - 0.8f * environment.cloudCover * std::clamp(environment.cloudDark + 0.4f, 0.0f, 1.0f)) * (look.roofed ? 0.0f : 1.0f);
    const float eyeAt[4] = {eye.x, eye.y, eye.z, look.seconds};
    const float sky[4] = {zenith.r, zenith.g, zenith.b, std::clamp(look.rain, 0.0f, 1.0f)};
    const float horizonUniform[4] = {horizon.r, horizon.g, horizon.b, std::max(look.wind, 0.0f)};
    const float sun[4] = {towardsSun.x, towardsSun.y, towardsSun.z, sunReach};
    const float sunColor[4] = {environment.sunColor.r, environment.sunColor.g, environment.sunColor.b, look.linear ? 1.0f : 0.0f};
    const glm::vec3 ambient = (environment.ambientSky * 0.8f + environment.ambientGround * 0.2f) * (look.roofed ? 0.3f : 1.0f);
    const float ambientUniform[4] = {ambient.r, ambient.g, ambient.b, environment.exposure};
    const float fog[4] = {environment.fogColor.r, environment.fogColor.g, environment.fogColor.b, environment.eyeOutdoors};
    const int ripples = std::min(static_cast<int>(look.ripples.size()), kMostRipples);
    const float fogParams[4] = {environment.fogStart, environment.fogEnd, look.roofed ? 0.0f : std::max(environment.lightning, 0.0f), static_cast<float>(ripples)};
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
    const float kind[4] = {static_cast<float>(look.kind), look.below ? 1.0f : 0.0f, look.fineReach, look.roofed ? 1.0f : 0.0f};
    bgfx::setUniform(m_uKind, kind);
    const float lift[4] = {look.lift, environment.waterReach, std::clamp(environment.airHaze, 0.0f, 1.5f), environment.eyeInMedium};
    bgfx::setUniform(m_uLift, lift);
    // The waves (vs_water), only on water.
    const int waves = look.kind == 0 ? std::clamp(look.waves.count, 0, Waves::kMost) : 0;
    bgfx::setUniform(m_uWaves, look.waves.waves.data(), static_cast<uint16_t>(Waves::kMost));
    const float waveParams[4] = {static_cast<float>(waves), look.waves.steepness, look.fineMiddle.x, look.fineMiddle.y};
    bgfx::setUniform(m_uWaveParams, waveParams);
    // The lamps near by, given back by it as a mirror gives them back (fs_water) -- every light of the scene's but the torch,
    // which has its own: where and how far it reaches (none: 0); its light, and how big it is.
    {
        // From the lights that move (flares, drones: Environment::lights) and the level's own lamps (sceneLights) both -- the
        // brightest here, by how much each gives out over how far it is, within a few times its reach (a mirror shows a lamp
        // further than its light goes). With the moving ones only, a building's ceiling lamps were never in it.
        const PunctualLight& first = environment.lights[0];
        const bool torchFirst = first.intensity > 0.0f && first.range > 0.0f && first.outerAngle < 89.0f;
        std::vector<std::pair<float, const PunctualLight*>> near;
        const auto consider = [&](const PunctualLight& lamp)
        {
            if (lamp.intensity <= 0.0f || lamp.range <= 0.0f)
            {
                return;
            }
            const float away = glm::length(lamp.position - eye);
            if (away < lamp.range * 4.0f)
            {
                near.emplace_back(lamp.intensity / std::max(away * away, 1.0f), &lamp);
            }
        };
        for (size_t i = torchFirst ? 1 : 0; i < kMaxPunctualLights; ++i)
        {
            consider(environment.lights[i]);
        }
        for (const PunctualLight& lamp : environment.sceneLights)
        {
            consider(lamp);
        }
        const size_t kept = std::min(near.size(), static_cast<size_t>(kMostLamps));
        std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(kept), near.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        glm::vec4 lamps[kMostLamps * 2] = {};
        for (size_t count = 0; count < kept; ++count)
        {
            const PunctualLight& lamp = *near[count].second;
            lamps[count * 2] = glm::vec4(lamp.position, lamp.range);
            lamps[count * 2 + 1] = glm::vec4(lamp.color * lamp.intensity, std::max(lamp.sourceRadius, 0.05f));
        }
        bgfx::setUniform(m_uLamps, lamps, static_cast<uint16_t>(kMostLamps * 2));
    }
    // The rings on it (fs_water), as many as there are.
    {
        glm::vec4 rings[kMostRipples] = {};
        std::copy(look.ripples.begin(), look.ripples.begin() + ripples, rings);
        bgfx::setUniform(m_uRipples, rings, static_cast<uint16_t>(kMostRipples));
    }
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
