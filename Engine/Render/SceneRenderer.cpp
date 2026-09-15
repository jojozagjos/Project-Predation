#include "Engine/Render/SceneRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/TextureLibrary.h"
#include "Engine/Render/ShaderLibrary.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{
// The sun's map is the one whose edges are looked at directly, so it gets the resolution. The sky's
// is only ever asked "is there a roof over this", an answer that changes over metres rather than
// centimetres, and a coarse map is also a softer one: its texels are 18 cm of world, which is the
// width of the fade at a doorway rather than a hard line across the floor.
// The near map does not need to be as big as the far one: it covers a fifth of the distance, so even
// at half the resolution its texels are a quarter the size.
constexpr uint16_t kSunNearShadowSize = 2048;
constexpr uint16_t kSunShadowSize = 2048;
constexpr uint16_t kSkyShadowSize = 512;
// How far the maps reach along their own axis. Deep enough that nothing in a level stands outside
// it and gets quietly clipped out of its own shadow.
constexpr float kShadowDepthRange = 220.0f;
} // namespace

bool SceneRenderer::Init(ShaderLibrary& shaders)
{
    m_program = shaders.LoadProgram("vs_mesh", "fs_mesh");
    if (!bgfx::isValid(m_program))
    {
        PRED_LOG_ERROR(Render, "SceneRenderer: failed to load the mesh program");
        return false;
    }

    m_uBaseColor = bgfx::createUniform("u_baseColor", bgfx::UniformType::Vec4);
    m_uMaterialParams = bgfx::createUniform("u_materialParams", bgfx::UniformType::Vec4);
    m_uEmissive = bgfx::createUniform("u_emissive", bgfx::UniformType::Vec4);
    m_uLightDirection = bgfx::createUniform("u_lightDirection", bgfx::UniformType::Vec4);
    m_uLightColor = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
    m_uAmbientSky = bgfx::createUniform("u_ambientSky", bgfx::UniformType::Vec4);
    m_uAmbientGround = bgfx::createUniform("u_ambientGround", bgfx::UniformType::Vec4);
    m_uFogColor = bgfx::createUniform("u_fogColor", bgfx::UniformType::Vec4);
    m_uFogParams = bgfx::createUniform("u_fogParams", bgfx::UniformType::Vec4);
    m_uCameraPosition = bgfx::createUniform("u_cameraPosition", bgfx::UniformType::Vec4);
    m_uGrade = bgfx::createUniform("u_grade", bgfx::UniformType::Vec4);
    m_uLights = bgfx::createUniform("u_lights", bgfx::UniformType::Vec4,
                                    static_cast<uint16_t>(kMaxPunctualLights * 4));
    m_uSunNearMtx = bgfx::createUniform("u_sunNearMtx", bgfx::UniformType::Mat4);
    m_uSunNearAxis = bgfx::createUniform("u_sunNearAxis", bgfx::UniformType::Vec4);
    m_uSunNearParams = bgfx::createUniform("u_sunNearParams", bgfx::UniformType::Vec4);
    m_sSunNear = bgfx::createUniform("s_sunNear", bgfx::UniformType::Sampler);
    m_uSunShadowMtx = bgfx::createUniform("u_sunShadowMtx", bgfx::UniformType::Mat4);
    m_uSunShadowAxis = bgfx::createUniform("u_sunShadowAxis", bgfx::UniformType::Vec4);
    m_uSunShadowParams = bgfx::createUniform("u_sunShadowParams", bgfx::UniformType::Vec4);
    m_uSkyShadowMtx = bgfx::createUniform("u_skyShadowMtx", bgfx::UniformType::Mat4);
    m_uSkyShadowAxis = bgfx::createUniform("u_skyShadowAxis", bgfx::UniformType::Vec4);
    m_uSkyShadowParams = bgfx::createUniform("u_skyShadowParams", bgfx::UniformType::Vec4);
    m_sBaseColor = bgfx::createUniform("s_baseColor", bgfx::UniformType::Sampler);
    m_sSunShadow = bgfx::createUniform("s_sunShadow", bgfx::UniformType::Sampler);
    m_sSkyShadow = bgfx::createUniform("s_skyShadow", bgfx::UniformType::Sampler);

    // Occlusion is not required for a picture. If the depth program or the float target is missing
    // the game still runs, unshadowed, and says so once rather than every frame.
    const bgfx::ProgramHandle depthProgram = shaders.LoadProgram("vs_shadow", "fs_shadow");
    m_shadowsReady = bgfx::isValid(depthProgram) &&
                     m_sunNearShadow.Init(kSunNearShadowSize, depthProgram) &&
                     m_sunShadow.Init(kSunShadowSize, depthProgram) &&
                     m_skyShadow.Init(kSkyShadowSize, depthProgram);
    if (!m_shadowsReady)
    {
        PRED_LOG_WARN(Render, "SceneRenderer: no shadow maps; the sun and sky reach everywhere");
    }

    PRED_LOG_INFO(Render, "SceneRenderer initialized");
    return true;
}

void SceneRenderer::Shutdown()
{
    m_sunNearShadow.Shutdown();
    m_sunShadow.Shutdown();
    m_skyShadow.Shutdown();
    m_shadowsReady = false;

    const bgfx::UniformHandle uniforms[] = {
        m_uBaseColor,      m_uMaterialParams,  m_uEmissive,        m_uLightDirection,
        m_uLightColor,     m_uAmbientSky,      m_uAmbientGround,   m_uFogColor,
        m_uFogParams,      m_uCameraPosition,  m_uGrade,           m_uLights,
        m_uSunShadowMtx,   m_uSunShadowAxis,   m_uSunShadowParams, m_uSkyShadowMtx,
        m_uSunNearMtx,     m_uSunNearAxis,     m_uSunNearParams,   m_sSunNear,
        m_uSkyShadowAxis,  m_uSkyShadowParams, m_sBaseColor,       m_sSunShadow,
        m_sSkyShadow};
    for (const bgfx::UniformHandle handle : uniforms)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    m_uBaseColor = m_uMaterialParams = m_uEmissive = BGFX_INVALID_HANDLE;
    m_uLightDirection = m_uLightColor = m_uAmbientSky = m_uAmbientGround = BGFX_INVALID_HANDLE;
    m_uFogColor = m_uFogParams = m_uCameraPosition = BGFX_INVALID_HANDLE;
    m_uGrade = m_uLights = BGFX_INVALID_HANDLE;
    m_uSunShadowMtx = m_uSunShadowAxis = m_uSunShadowParams = BGFX_INVALID_HANDLE;
    m_uSunNearMtx = m_uSunNearAxis = m_uSunNearParams = m_sSunNear = BGFX_INVALID_HANDLE;
    m_uSkyShadowMtx = m_uSkyShadowAxis = m_uSkyShadowParams = BGFX_INVALID_HANDLE;
    m_sBaseColor = m_sSunShadow = m_sSkyShadow = BGFX_INVALID_HANDLE;
    // The programs themselves are owned by the ShaderLibrary.
    m_program = BGFX_INVALID_HANDLE;
}

// Uniforms that are the same for every mesh in a pass. Split out so the item icon atlas can draw
// single meshes through the same shader the world uses, rather than approximating them in 2D.
void SceneRenderer::SetEnvironmentUniforms(const Environment& environment,
                                           const glm::vec3& cameraPosition, bool withShadows)
{
    // Shader wants the direction *towards* the light, which is the opposite of travel direction.
    const glm::vec3 towardsLight = glm::normalize(-environment.sunDirection);
    const float lightDirection[4] = {towardsLight.x, towardsLight.y, towardsLight.z, 0.0f};
    const float lightColor[4] = {environment.sunColor.r, environment.sunColor.g, environment.sunColor.b,
                                 environment.sunIntensity};
    const float ambientSky[4] = {environment.ambientSky.r, environment.ambientSky.g, environment.ambientSky.b,
                                 0.0f};
    const float ambientGround[4] = {environment.ambientGround.r, environment.ambientGround.g,
                                    environment.ambientGround.b, 0.0f};
    const float fogColor[4] = {environment.fogColor.r, environment.fogColor.g, environment.fogColor.b, 0.0f};
    const float fogParams[4] = {environment.fogStart, environment.fogEnd, 0.0f, 0.0f};
    const float cameraPos[4] = {cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f};

    bgfx::setUniform(m_uLightDirection, lightDirection);
    bgfx::setUniform(m_uLightColor, lightColor);
    bgfx::setUniform(m_uAmbientSky, ambientSky);
    bgfx::setUniform(m_uAmbientGround, ambientGround);
    bgfx::setUniform(m_uFogColor, fogColor);
    bgfx::setUniform(m_uFogParams, fogParams);
    bgfx::setUniform(m_uCameraPosition, cameraPos);

    const float grade[4] = {environment.exposure, environment.contrast, m_shadowSettings.indoorLight,
                            withShadows ? static_cast<float>(m_shadowSettings.debugView) : 0.0f};
    bgfx::setUniform(m_uGrade, grade);

    // Four vec4 per light, uploaded as one array. An unlit slot still costs its place in the
    // shader's loop, so there is nothing to gain by sending fewer, and a partial upload would leave
    // whatever the last frame happened to put there.
    float lights[kMaxPunctualLights * 4 * 4] = {};
    for (size_t i = 0; i < kMaxPunctualLights; ++i)
    {
        const PunctualLight& light = environment.lights[i];
        const bool on = light.intensity > 0.0f && light.range > 0.0f;
        const glm::vec3 direction = glm::length(light.direction) > 1e-4f
                                        ? glm::normalize(light.direction)
                                        : glm::vec3(0.0f, -1.0f, 0.0f);
        // Cones are authored in degrees from the axis and compared as cosines, so the conversion
        // happens once here rather than per pixel. The outer is forced a shade wider than the
        // inner, or the divide between them in the shader is by zero.
        const float inner = std::cos(glm::radians(std::clamp(light.innerAngle, 0.0f, 180.0f)));
        const float outer = std::cos(glm::radians(
            std::clamp(std::max(light.outerAngle, light.innerAngle + 0.5f), 0.0f, 180.0f)));

        float* entry = lights + i * 16;
        entry[0] = light.position.x;
        entry[1] = light.position.y;
        entry[2] = light.position.z;
        entry[3] = light.range;
        entry[4] = light.color.r;
        entry[5] = light.color.g;
        entry[6] = light.color.b;
        entry[7] = light.intensity;
        entry[8] = direction.x;
        entry[9] = direction.y;
        entry[10] = direction.z;
        entry[11] = inner;
        entry[12] = outer;
        entry[13] = on ? 1.0f : 0.0f;
        entry[14] = std::max(light.sourceRadius, 0.01f);
    }
    bgfx::setUniform(m_uLights, lights, static_cast<uint16_t>(kMaxPunctualLights * 4));

    // The occlusion maps. Both samplers are bound whatever happens: a sampler a shader declares and
    // nobody fills reads whatever was last in that slot, which is a picture that changes depending
    // on what else drew this frame. When a map is off, the flag below stops the shader looking at
    // it, and what is bound there does not matter.
    const bool sun = withShadows && m_shadowsReady && m_shadowSettings.sunEnabled;
    const bool sky = withShadows && m_shadowsReady && m_shadowSettings.skyEnabled;

    bgfx::setUniform(m_uSunShadowMtx, glm::value_ptr(m_sunShadow.TextureMatrix()));
    bgfx::setUniform(m_uSkyShadowMtx, glm::value_ptr(m_skyShadow.TextureMatrix()));
    bgfx::setUniform(m_uSunShadowAxis, glm::value_ptr(m_sunShadow.Axis()));
    bgfx::setUniform(m_uSkyShadowAxis, glm::value_ptr(m_skyShadow.Axis()));

    const float sunParams[4] = {1.0f / static_cast<float>(std::max<uint16_t>(m_sunShadow.Resolution(), 1)),
                                m_shadowSettings.sunBias, sun ? 1.0f : 0.0f,
                                m_shadowSettings.sunNormalOffset};
    const float skyParams[4] = {1.0f / static_cast<float>(std::max<uint16_t>(m_skyShadow.Resolution(), 1)),
                                m_shadowSettings.skyBias, sky ? 1.0f : 0.0f,
                                m_shadowSettings.skyNormalOffset};
    const float nearParams[4] = {
        1.0f / static_cast<float>(std::max<uint16_t>(m_sunNearShadow.Resolution(), 1)),
        m_shadowSettings.sunBias, sun ? 1.0f : 0.0f, m_shadowSettings.sunNormalOffset};
    bgfx::setUniform(m_uSunNearMtx, glm::value_ptr(m_sunNearShadow.TextureMatrix()));
    bgfx::setUniform(m_uSunNearAxis, glm::value_ptr(m_sunNearShadow.Axis()));
    bgfx::setUniform(m_uSunNearParams, nearParams);
    bgfx::setUniform(m_uSunShadowParams, sunParams);
    bgfx::setUniform(m_uSkyShadowParams, skyParams);
}

uint64_t SceneRenderer::DrawState() const
{
    // Primitives are wound counter-clockwise when seen from outside (the glTF convention). A
    // right-handed projection flips that to clockwise in screen space, so the winding to discard is
    // CW. Getting this backwards renders the *insides* of objects: surfaces lit by ambient only and
    // single-sided geometry such as the ground plane disappearing entirely.
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                     
                   BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
    if (m_wireframe)
    {
        state |= BGFX_STATE_PT_LINES;
    }
    return state;
}

void SceneRenderer::SubmitMesh(bgfx::ViewId view, const Mesh& mesh, const Material& material,
                               const glm::mat4& model, uint64_t state)
{
    const float baseColor[4] = {material.baseColor.r, material.baseColor.g, material.baseColor.b, 1.0f};
    const float materialParams[4] = {material.metallic, material.roughness, 0.0f, 0.0f};
    const float emissive[4] = {material.emissive.r, material.emissive.g, material.emissive.b, 0.0f};

    bgfx::setUniform(m_uBaseColor, baseColor);
    bgfx::setUniform(m_uMaterialParams, materialParams);
    bgfx::setUniform(m_uEmissive, emissive);

    // Always bound. A material with no texture of its own gets the library's white pixel, so the
    // shader multiplies by one and there is no second program and no branch per fragment.
    if (bgfx::isValid(m_sBaseColor) && m_textures != nullptr)
    {
        bgfx::setTexture(0, m_sBaseColor, m_textures->Get(material.baseColorTexture));
    }

    // And the two occlusion maps, here rather than once per pass with the rest of the environment.
    //
    // bgfx keeps uniform values between draws but does not keep texture bindings: a submit consumes
    // them, and the next draw starts with those sampler slots empty. Bound once before the loop, the
    // first mesh submitted reads the maps correctly and every mesh after it samples nothing -- which
    // is a bug that looks exactly like the maps being wrong rather than like a binding being lost,
    // because the first mesh is usually the ground and the ground is the one thing that came out
    // right.
    if (m_textures != nullptr)
    {
        const bgfx::TextureHandle white = m_textures->Get({});
        bgfx::setTexture(1, m_sSunShadow, m_shadowsReady ? m_sunShadow.Texture() : white);
        bgfx::setTexture(3, m_sSunNear, m_shadowsReady ? m_sunNearShadow.Texture() : white);
        bgfx::setTexture(2, m_sSkyShadow, m_shadowsReady ? m_skyShadow.Texture() : white);
    }

    bgfx::setTransform(glm::value_ptr(model));
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(mesh.indexBuffer);
    bgfx::setState(state);
    bgfx::submit(view, m_program);

    ++m_stats.meshesSubmitted;
    m_stats.trianglesSubmitted += mesh.indexCount / 3;
}

void SceneRenderer::SubmitDepth(bgfx::ViewId view, const Mesh& mesh, const glm::mat4& model,
                                bgfx::ProgramHandle program)
{
    bgfx::setTransform(glm::value_ptr(model));
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(mesh.indexBuffer);
    // Red and depth only: no colour to blend, no alpha, and the same winding discarded as the main
    // pass so a light sees the same faces the camera does. Wireframe is deliberately not honoured
    // here -- a wireframe world should still cast solid shadows, or turning it on to look at
    // geometry changes the lighting of everything.
    bgfx::setState(BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                   BGFX_STATE_CULL_CW);
    bgfx::submit(view, program);
}

void SceneRenderer::RenderShadows(bgfx::ViewId sunNearView, bgfx::ViewId sunView,
                                  bgfx::ViewId skyView, const Scene& scene,
                                  const MeshLibrary& meshes, const glm::vec3& focus)
{
    if (!m_shadowsReady)
    {
        return;
    }
    const Environment& environment = scene.GetEnvironment();
    const ShadowSettings& settings = m_shadowSettings;

    // Both maps are fitted around the player rather than around the level. A level is bigger than a
    // map can be at any useful resolution, and the only part of it whose shadows anybody can see is
    // the part they are standing in.
    m_sunNearShadow.Fit(focus, environment.sunDirection,
                        std::min(settings.nearDistance, settings.distance), kShadowDepthRange);
    m_sunShadow.Fit(focus, environment.sunDirection, settings.distance, kShadowDepthRange);
    // Straight down. The sky is not a direction but a hemisphere, and the honest version of this
    // would gather visibility over all of it; one look straight down is the cheapest approximation
    // that gets the thing that matters right, which is that a roof is between the room and the sky.
    m_skyShadow.Fit(focus, glm::vec3(0.0f, -1.0f, 0.0f), settings.distance, kShadowDepthRange);

    if (settings.sunEnabled)
    {
        m_sunNearShadow.Begin(sunNearView);
        m_sunShadow.Begin(sunView);
    }
    if (settings.skyEnabled)
    {
        m_skyShadow.Begin(skyView);
    }

    scene.ForEachShadowCaster(
        [&](Entity, const Transform& transform, const MeshRenderer& renderer)
        {
            if (!renderer.castsShadow)
            {
                return;
            }
            const Mesh* mesh = meshes.Get(renderer.mesh);
            if (mesh == nullptr || !mesh->IsValid())
            {
                return;
            }
            const glm::mat4 model = transform.Matrix();
            if (settings.sunEnabled)
            {
                SubmitDepth(sunNearView, *mesh, model, m_sunNearShadow.Program());
                SubmitDepth(sunView, *mesh, model, m_sunShadow.Program());
            }
            if (settings.skyEnabled)
            {
                SubmitDepth(skyView, *mesh, model, m_skyShadow.Program());
            }
        });
}

void SceneRenderer::Draw(bgfx::ViewId view, const Scene& scene, const MeshLibrary& meshes,
                         const glm::vec3& cameraPosition)
{
    m_stats = Stats{};
    if (!bgfx::isValid(m_program))
    {
        return;
    }

    SetEnvironmentUniforms(scene.GetEnvironment(), cameraPosition, true);
    const uint64_t state = DrawState();

    scene.ForEachMeshRenderer(
        [&](Entity, const Transform& transform, const MeshRenderer& renderer)
        {
            const Mesh* mesh = meshes.Get(renderer.mesh);
            if (mesh == nullptr || !mesh->IsValid())
            {
                return;
            }
            SubmitMesh(view, *mesh, renderer.material, transform.Matrix(), state);
        });
}

void SceneRenderer::DrawOne(bgfx::ViewId view, const Mesh& mesh, const Material& material,
                            const glm::mat4& model, const Environment& environment,
                            const glm::vec3& cameraPosition)
{
    if (!bgfx::isValid(m_program) || !mesh.IsValid())
    {
        return;
    }
    SetEnvironmentUniforms(environment, cameraPosition, false);
    SubmitMesh(view, mesh, material, model, DrawState());
}

} // namespace pred
