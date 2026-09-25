#include "Engine/Render/SceneRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/TextureLibrary.h"
#include "Engine/Render/Renderer.h"
#include "Engine/Render/ShaderLibrary.h"
#include "Engine/Render/SkyRenderer.h"
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
// Four thousand and ninety-six, not two, and the reason is arithmetic rather than taste.
//
// The map covers a circle of `distance` around the player, so a texel is (2 * distance) / size of
// world. At 2048 over a 16 m radius that is 1.56 cm, and a shadow's silhouette is a staircase at
// texel scale whatever the filter does -- each texel either holds the occluder or does not. At the
// distance a player looks at their own shadow, 1.56 cm is about three pixels a step, which is
// exactly the "my shadow is pixelated" that keeps being reported.
//
// Doubling the side halves the step to 0.78 cm and, just as usefully, buys the range back: the map
// now reaches 24 m instead of 16 and the texel is still 1.17 cm, finer than it used to be at
// two-thirds the distance. 64 MB of the card, which is a fair price for the one effect in this game
// that is looked at closely from a metre away.
constexpr uint16_t kSunShadowSize = 4096;
// The sky map is read close up on walls, where a coarse texel shows as a square patch of dimmer
// ambient a hand span across -- reported as "big pixelated squares" next to a wall. It is not the
// resolution the occlusion needs, it is the resolution the eye needs at arm.s length.
// Deliberately very coarse, and the coarseness is the whole design.
//
// The sky term is not asking "what is directly over this point" -- that question has no good answer
// for the side of anything, whose own top is directly over it -- but "how much of the neighbourhood
// can see sky". So what matters is the size of the neighbourhood, and it has to sit between the two
// scales the answer differs at: wider than the things a surface belongs to, narrower than a room.
//
// At a quarter of a metre it was too narrow, and the way that showed was crates with one side dark
// and the top lit: a face of a metre-wide box never found open sky within reach of itself. Sixty
// centimetres clears a crate, a bench and a wall, and is still nothing against a room four metres
// across. Over a forty metre map that is a hundred and twenty-eight texels, which sounds absurd for
// a shadow map and is exactly right for this one.
constexpr uint16_t kSkyShadowSize = 512;
// The near sun map: the same sun over only the ground round the player, twelve metres out, which is
// where a shadow is looked at closely -- your own, the creature's across a room. 2048 texels over
// twenty-four metres is about a centimetre each, whatever the shadow distance is set to, so turning
// that up for the far shadows no longer coarsens the near ones. That is what made them pixelated again.
constexpr uint16_t kSunNearShadowSize = 2048;
constexpr float kSunNearDistance = 12.0f;
// The torch's. Smaller than the sun's, because a cone covers far less world: a 70 degree beam at 14
// metres is about 20 m across at the far end, so 1024 texels is 2 cm there and finer everywhere
// nearer. It is also redrawn every frame from a light that moves with the player's head, so it is
// the one map whose cost is paid continuously.
constexpr uint16_t kSpotShadowSize = 1024;
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
    m_uOutput = bgfx::createUniform("u_output", bgfx::UniformType::Vec4);
    m_uLights = bgfx::createUniform("u_lights", bgfx::UniformType::Vec4,
                                    static_cast<uint16_t>(kMaxPunctualLights * 4));
    m_uSunShadowMtx = bgfx::createUniform("u_sunShadowMtx", bgfx::UniformType::Mat4);
    m_uSunShadowAxis = bgfx::createUniform("u_sunShadowAxis", bgfx::UniformType::Vec4);
    m_uSunShadowParams = bgfx::createUniform("u_sunShadowParams", bgfx::UniformType::Vec4);
    m_uSkyShadowMtx = bgfx::createUniform("u_skyShadowMtx", bgfx::UniformType::Mat4);
    m_uSkyShadowAxis = bgfx::createUniform("u_skyShadowAxis", bgfx::UniformType::Vec4);
    m_uSkyShadowParams = bgfx::createUniform("u_skyShadowParams", bgfx::UniformType::Vec4);
    m_sBaseColor = bgfx::createUniform("s_baseColor", bgfx::UniformType::Sampler);
    m_sSunShadow = bgfx::createUniform("s_sunShadow", bgfx::UniformType::Sampler);
    m_uSunNearShadowMtx = bgfx::createUniform("u_sunNearShadowMtx", bgfx::UniformType::Mat4);
    m_uSunNearShadowAxis = bgfx::createUniform("u_sunNearShadowAxis", bgfx::UniformType::Vec4);
    m_uSunNearShadowParams = bgfx::createUniform("u_sunNearShadowParams", bgfx::UniformType::Vec4);
    m_sSunNearShadow = bgfx::createUniform("s_sunNearShadow", bgfx::UniformType::Sampler);
    m_sSkyShadow = bgfx::createUniform("s_skyShadow", bgfx::UniformType::Sampler);
    m_uSpotShadowMtx = bgfx::createUniform("u_spotShadowMtx", bgfx::UniformType::Mat4);
    m_uSpotShadowAxis = bgfx::createUniform("u_spotShadowAxis", bgfx::UniformType::Vec4);
    m_uSpotShadowParams = bgfx::createUniform("u_spotShadowParams", bgfx::UniformType::Vec4);
    m_uShadowTexelWorld = bgfx::createUniform("u_shadowTexelWorld", bgfx::UniformType::Vec4);
    m_sSpotShadow = bgfx::createUniform("s_spotShadow", bgfx::UniformType::Sampler);
    m_uClipPlane = bgfx::createUniform("u_clipPlane", bgfx::UniformType::Vec4);
    m_uReflectParams = bgfx::createUniform("u_reflectParams", bgfx::UniformType::Vec4);
    m_sReflection = bgfx::createUniform("s_reflection", bgfx::UniformType::Sampler);

    // Occlusion is not required for a picture. If the depth program or the float target is missing
    // the game still runs, unshadowed, and says so once rather than every frame.
    const bgfx::ProgramHandle depthProgram = shaders.LoadProgram("vs_shadow", "fs_shadow");
    m_shadowsReady = bgfx::isValid(depthProgram) &&
                     m_sunShadow.Init(kSunShadowSize, depthProgram) &&
                     m_sunNearShadow.Init(kSunNearShadowSize, depthProgram) &&
                     m_skyShadow.Init(kSkyShadowSize, depthProgram) &&
                     m_spotShadow.Init(kSpotShadowSize, depthProgram);
    if (!m_shadowsReady)
    {
        PRED_LOG_WARN(Render, "SceneRenderer: no shadow maps; the sun and sky reach everywhere");
    }

    PRED_LOG_INFO(Render, "SceneRenderer initialized");
    return true;
}

void SceneRenderer::Shutdown()
{
    m_sunShadow.Shutdown();
    m_sunNearShadow.Shutdown();
    m_skyShadow.Shutdown();
    m_spotShadow.Shutdown();
    m_shadowsReady = false;

    const bgfx::UniformHandle uniforms[] = {
        m_uBaseColor,      m_uMaterialParams,  m_uEmissive,        m_uLightDirection,
        m_uLightColor,     m_uAmbientSky,      m_uAmbientGround,   m_uFogColor,
        m_uFogParams,      m_uCameraPosition,  m_uGrade,           m_uLights,
        m_uSunShadowMtx,   m_uSunShadowAxis,   m_uSunShadowParams, m_uSkyShadowMtx,
        m_uSkyShadowAxis,  m_uSkyShadowParams, m_sBaseColor,       m_sSunShadow,
        m_sSkyShadow,      m_uClipPlane,       m_uReflectParams,   m_sReflection,
        m_uSpotShadowMtx,  m_uSpotShadowAxis,  m_uSpotShadowParams, m_sSpotShadow,
        m_uShadowTexelWorld, m_uSunNearShadowMtx, m_uSunNearShadowAxis, m_uSunNearShadowParams,
        m_sSunNearShadow};
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
    if (bgfx::isValid(m_uOutput))
    {
        bgfx::destroy(m_uOutput);
    }
    m_uOutput = BGFX_INVALID_HANDLE;
    m_uSunShadowMtx = m_uSunShadowAxis = m_uSunShadowParams = BGFX_INVALID_HANDLE;
    m_uSkyShadowMtx = m_uSkyShadowAxis = m_uSkyShadowParams = BGFX_INVALID_HANDLE;
    m_sBaseColor = m_sSunShadow = m_sSkyShadow = BGFX_INVALID_HANDLE;
    m_uClipPlane = m_uReflectParams = m_sReflection = BGFX_INVALID_HANDLE;
    m_uSpotShadowMtx = m_uSpotShadowAxis = m_uSpotShadowParams = m_sSpotShadow = BGFX_INVALID_HANDLE;
    m_uShadowTexelWorld = BGFX_INVALID_HANDLE;
    m_uSunNearShadowMtx = m_uSunNearShadowAxis = m_uSunNearShadowParams = m_sSunNearShadow = BGFX_INVALID_HANDLE;

    if (bgfx::isValid(m_reflectionTarget))
    {
        bgfx::destroy(m_reflectionTarget); // takes its colour and depth attachments with it
    }
    m_reflectionTarget = BGFX_INVALID_HANDLE;
    m_reflectionTexture = BGFX_INVALID_HANDLE;
    m_reflectionWidth = m_reflectionHeight = 0;
    m_reflectionReady = false;

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

    // The lights are chosen per surface rather than here: see UploadLightsFor.
    PackLights(environment);

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
    bgfx::setUniform(m_uSunNearShadowMtx, glm::value_ptr(m_sunNearShadow.TextureMatrix()));
    bgfx::setUniform(m_uSunNearShadowAxis, glm::value_ptr(m_sunNearShadow.Axis()));
    const float nearParams[4] = {1.0f / static_cast<float>(std::max<uint16_t>(m_sunNearShadow.Resolution(), 1)),
                                 m_shadowSettings.sunBias, sun ? 1.0f : 0.0f, m_shadowSettings.sunNormalOffset};
    bgfx::setUniform(m_uSunNearShadowParams, nearParams);
    bgfx::setUniform(m_uSunShadowParams, sunParams);
    bgfx::setUniform(m_uSkyShadowParams, skyParams);

    const bool spot = withShadows && m_shadowsReady && m_spotShadowLit;
    bgfx::setUniform(m_uSpotShadowMtx, glm::value_ptr(m_spotShadow.TextureMatrix()));
    bgfx::setUniform(m_uSpotShadowAxis, glm::value_ptr(m_spotShadow.Axis()));
    const float spotParams[4] = {
        1.0f / static_cast<float>(std::max<uint16_t>(m_spotShadow.Resolution(), 1)),
        m_shadowSettings.spotBias, spot ? 1.0f : 0.0f, m_shadowSettings.spotNormalOffset};
    bgfx::setUniform(m_uSpotShadowParams, spotParams);

    // How much world one texel of each map covers. The slack every one of them needs is a distance
    // in the world, so it is worked out from this rather than being a constant somebody has to
    // remember to re-tune: raising the shadow distance from 16 m to 24 m made a sky texel half again
    // as wide and put the banding back on the ramps, and nothing in the code said it would.
    //
    // The torch's map is a cone, so its texels grow with distance and there is no single figure. The
    // one here is what a texel covers at the far end of the beam, which is the worst case and the
    // only place it matters.
    const float spotFar = m_spotShadow.Resolution() > 0
                              ? (2.0f * m_shadowSettings.spotRange) /
                                    static_cast<float>(m_spotShadow.Resolution())
                              : 0.0f;
    const float texelWorld[4] = {m_sunShadow.TexelSize(), m_skyShadow.TexelSize(), spotFar,
                                 m_sunNearShadow.TexelSize()};
    bgfx::setUniform(m_uShadowTexelWorld, texelWorld);

    // No clip, and the mirror on if there is one to sample. Both are overridden immediately after
    // this by the reflection pass itself, which needs the opposite of each.
    //
    // These have to be set for every pass rather than once, because a uniform bgfx never sees set
    // holds whatever the last pass put there. Left alone, the icon atlas would inherit the world's
    // reflection and paint a slice of the level onto every metal item in the inventory.
    const float noClip[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float reflect[4] = {(withShadows && m_reflectionReady) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    bgfx::setUniform(m_uClipPlane, noClip);
    bgfx::setUniform(m_uReflectParams, reflect);
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
    const float materialParams[4] = {material.metallic, material.roughness, material.reflectivity,
                                     0.0f};
    const float emissive[4] = {material.emissive.r, material.emissive.g, material.emissive.b, 0.0f};

    bgfx::setUniform(m_uBaseColor, baseColor);
    bgfx::setUniform(m_uMaterialParams, materialParams);
    bgfx::setUniform(m_uEmissive, emissive);
    const float output[4] = {m_linearOutput && view < Renderer::kViewOffscreenFirst ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    bgfx::setUniform(m_uOutput, output);

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
        bgfx::setTexture(5, m_sSunNearShadow, m_shadowsReady ? m_sunNearShadow.Texture() : white);
        bgfx::setTexture(2, m_sSkyShadow, m_shadowsReady ? m_skyShadow.Texture() : white);
        bgfx::setTexture(3, m_sSpotShadow, m_shadowsReady ? m_spotShadow.Texture() : white);
        // And the mirror, for the same reason: a sampler the shader declares and nobody fills reads
        // whatever was last in that slot.
        bgfx::setTexture(4, m_sReflection,
                         bgfx::isValid(m_reflectionTexture) ? m_reflectionTexture : white);
    }

    UploadLightsFor(mesh, model);
    bgfx::setTransform(glm::value_ptr(model));
    if (mesh.IsDynamic())
    {
        bgfx::setVertexBuffer(0, mesh.dynamicVertexBuffer);
    }
    else
    {
        bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    }
    bgfx::setIndexBuffer(mesh.indexBuffer);
    bgfx::setState(state);
    bgfx::submit(view, m_program);

    ++m_stats.meshesSubmitted;
    m_stats.trianglesSubmitted += mesh.indexCount / 3;
}

void SceneRenderer::SubmitDepth(bgfx::ViewId view, const Mesh& mesh, const glm::mat4& model,
                                const ShadowMap& map)
{
    // This map's range, immediately before this draw. The uniform is shared between all three maps
    // and captured at submit, so it cannot be set once when the view starts: see ShadowMap::BindRange.
    map.BindRange();
    bgfx::setTransform(glm::value_ptr(model));
    if (mesh.IsDynamic())
    {
        bgfx::setVertexBuffer(0, mesh.dynamicVertexBuffer);
    }
    else
    {
        bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    }
    bgfx::setIndexBuffer(mesh.indexBuffer);
    // Red and depth only: no colour to blend, no alpha, and the same winding discarded as the main
    // pass so a light sees the same faces the camera does. Wireframe is deliberately not honoured
    // here -- a wireframe world should still cast solid shadows, or turning it on to look at
    // geometry changes the lighting of everything.
    bgfx::setState(BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                   BGFX_STATE_CULL_CW);
    bgfx::submit(view, map.Program());
}

void SceneRenderer::RenderShadows(bgfx::ViewId sunView, bgfx::ViewId sunNearView, bgfx::ViewId skyView,
                                  bgfx::ViewId spotView, const Scene& scene, const MeshLibrary& meshes,
                                  const glm::vec3& focus)
{
    m_spotShadowLit = false;
    if (!m_shadowsReady)
    {
        return;
    }
    const Environment& environment = scene.GetEnvironment();
    const ShadowSettings& settings = m_shadowSettings;

    // Both maps are fitted around the player rather than around the level. A level is bigger than a
    // map can be at any useful resolution, and the only part of it whose shadows anybody can see is
    m_sunShadow.Fit(focus, environment.sunDirection, settings.distance, kShadowDepthRange);
    m_sunNearShadow.Fit(focus, environment.sunDirection, std::min(kSunNearDistance, settings.distance),
                        kShadowDepthRange);
    // Straight down. The sky is not a direction but a hemisphere, and the honest version of this
    // would gather visibility over all of it; one look straight down is the cheapest approximation
    // that gets the thing that matters right, which is that a roof is between the room and the sky.
    m_skyShadow.Fit(focus, glm::vec3(0.0f, -1.0f, 0.0f), settings.distance, kShadowDepthRange);

    if (settings.sunEnabled)
    {
        m_sunShadow.Begin(sunView);
        m_sunNearShadow.Begin(sunNearView);
    }
    if (settings.skyEnabled)
    {
        m_skyShadow.Begin(skyView);
    }

    // And the torch, which is whatever is in the first light slot if it is a cone.
    //
    // One shadowed punctual light rather than all four. Each one is a whole extra pass over the
    // scene, and the game already sorts the slots by what each light actually contributes at the
    // eye -- so slot zero is the brightest thing near the player, which is the one whose leaking
    // through a wall anybody would notice. A muzzle flash lands there for the frames it exists and
    // gets shadowed too, which is a bonus rather than the point.
    //
    // Only a cone. A bulb throws light in every direction and one square map cannot see all of it;
    // that wants a cube map and a much larger change.
    const PunctualLight& first = environment.lights[0];
    const bool coneLight = first.intensity > 0.0f && first.range > 0.0f && first.outerAngle < 89.0f;
    if (settings.spotEnabled && coneLight)
    {
        // Remembered so the shading pass can work out what one of the cone.s texels covers at the far
        // end of the beam, which is the worst case for its slack.
        m_shadowSettings.spotRange = first.range;
        m_spotShadow.FitSpot(first.position, first.direction, first.outerAngle, first.range);
        m_spotShadow.Begin(spotView);
        m_spotShadowLit = true;
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
                SubmitDepth(sunView, *mesh, model, m_sunShadow);
                SubmitDepth(sunNearView, *mesh, model, m_sunNearShadow);
            }
            if (settings.skyEnabled && renderer.blocksSky)
            {
                SubmitDepth(skyView, *mesh, model, m_skyShadow);
            }
            if (m_spotShadowLit)
            {
                SubmitDepth(spotView, *mesh, model, m_spotShadow);
            }
        });
}

void SceneRenderer::RenderReflection(bgfx::ViewId skyView, bgfx::ViewId worldView, const Scene& scene,
                                     const MeshLibrary& meshes, const glm::vec4& plane,
                                     const glm::mat4& cameraView, const glm::mat4& projection,
                                     const glm::vec3& cameraPosition)
{
    m_reflectionReady = false;
    if (!m_reflections || !bgfx::isValid(m_program))
    {
        return;
    }
    const glm::vec3 normal = glm::vec3(plane);
    if (glm::length(normal) < 0.9f)
    {
        return; // not a plane
    }

    // The target follows the window at half its width and height.
    //
    // A mirror is looked at through a surface, from a distance, and at an angle; half resolution
    // there is very hard to see and is a quarter of the pixels of a second full pass. The cost of
    // this feature is a whole extra draw of the scene, so the place to spend care is on not making
    // it two full ones.
    const bgfx::Stats* stats = bgfx::getStats();
    const auto wantWidth = static_cast<uint16_t>(std::max<uint32_t>(stats->width / 2, 64));
    const auto wantHeight = static_cast<uint16_t>(std::max<uint32_t>(stats->height / 2, 64));
    if (wantWidth != m_reflectionWidth || wantHeight != m_reflectionHeight ||
        !bgfx::isValid(m_reflectionTarget) || m_reflectionLinear != m_linearOutput)
    {
        m_reflectionLinear = m_linearOutput;
        if (bgfx::isValid(m_reflectionTarget))
        {
            bgfx::destroy(m_reflectionTarget);
        }
        m_reflectionTexture =
            bgfx::createTexture2D(wantWidth, wantHeight, false, 1,
                                  m_linearOutput ? bgfx::TextureFormat::RGBA16F : bgfx::TextureFormat::RGBA8,
                                  BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        const bgfx::TextureHandle depth = bgfx::createTexture2D(
            wantWidth, wantHeight, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
        const bgfx::TextureHandle attachments[] = {m_reflectionTexture, depth};
        // The frame buffer owns both from here: destroying it destroys them.
        m_reflectionTarget = bgfx::createFrameBuffer(2, attachments, true);
        m_reflectionWidth = wantWidth;
        m_reflectionHeight = wantHeight;
        if (!bgfx::isValid(m_reflectionTarget))
        {
            PRED_LOG_WARN(Render, "SceneRenderer: no reflection target; mirrors stay flat");
            m_reflections = false;
            return;
        }
    }

    // The camera, reflected across the mirror's plane.
    //
    // Reflecting the world instead would do equally well and would mean moving every object;
    // reflecting the camera moves one matrix. Householder: P' = P - 2(N.P + d)N, written out as a
    // matrix so it can be composed with the view in one multiply.
    //
    // What this costs is that the reflection turns every triangle inside out, so the winding to
    // discard is the opposite one for this pass. Miss that and the mirror shows the insides of
    // everything -- ambient-lit shells with the ground missing, which reads as the reflection being
    // broken rather than as a culling mistake.
    glm::mat4 mirror(1.0f);
    mirror[0][0] = 1.0f - 2.0f * normal.x * normal.x;
    mirror[1][0] = -2.0f * normal.x * normal.y;
    mirror[2][0] = -2.0f * normal.x * normal.z;
    mirror[3][0] = -2.0f * normal.x * plane.w;
    mirror[0][1] = -2.0f * normal.y * normal.x;
    mirror[1][1] = 1.0f - 2.0f * normal.y * normal.y;
    mirror[2][1] = -2.0f * normal.y * normal.z;
    mirror[3][1] = -2.0f * normal.y * plane.w;
    mirror[0][2] = -2.0f * normal.z * normal.x;
    mirror[1][2] = -2.0f * normal.z * normal.y;
    mirror[2][2] = 1.0f - 2.0f * normal.z * normal.z;
    mirror[3][2] = -2.0f * normal.z * plane.w;
    const glm::mat4 reflectedView = cameraView * mirror;
    const glm::vec3 reflectedEye = glm::vec3(mirror * glm::vec4(cameraPosition, 1.0f));

    // Two views into the one target, the same way the world is drawn: the sky clears the colour and
    // the world clears only the depth over the top of it. One view would not do, because bgfx sorts
    // the draws inside a view and the sky writes no depth -- sorted after the world it would paint
    // straight over it.
    bgfx::setViewFrameBuffer(skyView, m_reflectionTarget);
    bgfx::setViewRect(skyView, 0, 0, m_reflectionWidth, m_reflectionHeight);
    bgfx::setViewClear(skyView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
    bgfx::setViewTransform(skyView, glm::value_ptr(reflectedView), glm::value_ptr(projection));
    bgfx::touch(skyView);
    if (m_sky != nullptr)
    {
        m_sky->Draw(skyView, scene.GetEnvironment(), reflectedView, projection);
    }

    bgfx::setViewFrameBuffer(worldView, m_reflectionTarget);
    bgfx::setViewRect(worldView, 0, 0, m_reflectionWidth, m_reflectionHeight);
    bgfx::setViewClear(worldView, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
    bgfx::setViewTransform(worldView, glm::value_ptr(reflectedView), glm::value_ptr(projection));
    bgfx::touch(worldView);

    // Lit as the world is, from the reflected eye, with the mirror term off so a mirror cannot
    // appear inside its own reflection, and with everything behind the plane clipped away.
    SetEnvironmentUniforms(scene.GetEnvironment(), reflectedEye, true);
    const float noReflection[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bgfx::setUniform(m_uReflectParams, noReflection);
    bgfx::setUniform(m_uClipPlane, glm::value_ptr(plane));

    const uint64_t state = (DrawState() & ~BGFX_STATE_CULL_MASK) | BGFX_STATE_CULL_CCW;
    scene.ForEachReflected(
        [&](Entity, const Transform& transform, const MeshRenderer& renderer)
        {
            const Mesh* mesh = meshes.Get(renderer.mesh);
            if (mesh == nullptr || !mesh->IsValid())
            {
                return;
            }
            SubmitMesh(worldView, *mesh, renderer.material, transform.Matrix(), state);
        });

    // Whatever this pass drew is not part of the frame's count: those are the meshes the player can
    // see, and doubling them because the scene was drawn twice makes the number mean nothing.
    m_stats = Stats{};
    m_reflectionReady = true;
}

void SceneRenderer::PackLights(const Environment& environment)
{
    m_packed.clear();
    const auto pack = [this](const PunctualLight& light, bool pinned)
    {
        if (light.intensity <= 0.0f || light.range <= 0.0f)
        {
            return;
        }
        PackedLight packed;
        const glm::vec3 direction = glm::length(light.direction) > 1e-4f ? glm::normalize(light.direction)
                                                                         : glm::vec3(0.0f, -1.0f, 0.0f);
        // Cones are authored in degrees from the axis and compared as cosines, so the conversion happens
        // once here rather than per pixel. The outer is forced a shade wider than the inner, or the divide
        // between them in the shader is by zero.
        const float inner = std::cos(glm::radians(std::clamp(light.innerAngle, 0.0f, 180.0f)));
        const float outer =
            std::cos(glm::radians(std::clamp(std::max(light.outerAngle, light.innerAngle + 0.5f), 0.0f, 180.0f)));
        float* entry = packed.data;
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
        entry[13] = 1.0f;
        entry[14] = std::max(light.sourceRadius, 0.01f);
        packed.position = light.position;
        packed.range = light.range;
        packed.intensity = light.intensity;
        packed.pinned = pinned;
        m_packed.push_back(packed);
    };
    for (size_t i = 0; i < kMaxPunctualLights; ++i)
    {
        pack(environment.lights[i], i == 0);
    }
    for (const PunctualLight& light : environment.sceneLights)
    {
        pack(light, false);
    }
}

void SceneRenderer::UploadLightsFor(const Mesh& mesh, const glm::mat4& model)
{
    // The surface as a sphere in the world: its box's middle and the distance to its farthest corner,
    // grown by however much the transform scales it.
    glm::vec3 centre = glm::vec3(model[3]);
    float radius = 1.0e6f;
    if (mesh.bounds.IsValid())
    {
        const glm::vec3 local = (mesh.bounds.min + mesh.bounds.max) * 0.5f;
        centre = glm::vec3(model * glm::vec4(local, 1.0f));
        const float scale = std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                                      glm::length(glm::vec3(model[2]))});
        radius = glm::length(mesh.bounds.max - mesh.bounds.min) * 0.5f * scale;
    }

    // Whatever reaches it, strongest at its nearest point first. The shadowed slot keeps its place: its
    // shadow map was drawn for it, and only slot nought reads one.
    m_choice.clear();
    int pinned = -1;
    for (size_t i = 0; i < m_packed.size(); ++i)
    {
        const PackedLight& light = m_packed[i];
        const float gap = glm::length(light.position - centre) - radius;
        if (gap > light.range)
        {
            continue;
        }
        if (light.pinned)
        {
            pinned = static_cast<int>(i);
            continue;
        }
        const float near = std::max(gap, 0.5f);
        m_choice.emplace_back(light.intensity / (near * near), i);
    }
    const size_t room = kMaxPunctualLights - 1;
    if (m_choice.size() > room)
    {
        std::partial_sort(m_choice.begin(), m_choice.begin() + static_cast<ptrdiff_t>(room), m_choice.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        m_choice.resize(room);
    }

    float lights[kMaxPunctualLights * 16] = {};
    if (pinned >= 0)
    {
        std::copy(m_packed[static_cast<size_t>(pinned)].data, m_packed[static_cast<size_t>(pinned)].data + 16, lights);
    }
    for (size_t slot = 0; slot < m_choice.size(); ++slot)
    {
        const float* data = m_packed[m_choice[slot].second].data;
        std::copy(data, data + 16, lights + (slot + 1) * 16);
    }
    bgfx::setUniform(m_uLights, lights, static_cast<uint16_t>(kMaxPunctualLights * 4));
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
