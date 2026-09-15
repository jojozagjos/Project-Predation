#pragma once

#include "Engine/Render/ShadowMap.h"
#include "Engine/Render/TextureLibrary.h"

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>

namespace pred
{

class Scene;
class MeshLibrary;
class ShaderLibrary;
struct Environment;
struct Material;
struct Mesh;

// How much occlusion costs and how forgiving it is. Everything here is in metres, because the maps
// hold metres: a bias somebody has to tune by watching for stripes is a bias nobody can reason
// about, and these are all distances with a physical meaning.
struct ShadowSettings
{
    bool sunEnabled = true;
    bool skyEnabled = true;
    // The radius around the player that the maps cover. Past it there is no occlusion at all, so
    // this is the distance at which a building stops being dark inside, and it trades directly
    // against how much world each texel is responsible for.
    float distance = 32.0f;
    // Slack in the comparison, and how far along the surface normal to take the reading. The sky
    // map needs far more of the second: it has to move the lookup clear of the wall the surface
    // belongs to, or every outside wall stands in the shade of its own roof.
    float sunBias = 0.05f;
    float sunNormalOffset = 0.06f;
    float skyBias = 0.12f;
    float skyNormalOffset = 0.40f;
    // What is left of the ambient where the sky cannot reach, standing in for light that bounced
    // its way in. Zero is a void rather than a dark room: geometry outside the torch beam stops
    // existing rather than being hard to see.
    float indoorLight = 0.06f;
    // 0 draws the scene, 1 draws the sun's occlusion on its own, 2 the sky's. A debug view rather
    // than a setting: when the lighting is wrong the first question is which map is saying what.
    int debugView = 0;
};

// Draws the scene's mesh renderers with a single forward lit pass.
//
// One directional light, four punctual ones, hemispheric ambient and distance fog, with the sun and
// the sky each occluded by a depth map rendered beforehand. Post-processing beyond a tone curve
// arrives in a later milestone.
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

    // Renders both depth maps, fitted around `focus`. Has to run before Draw, into lower view ids,
    // because bgfx submits views in the order of their ids and Draw reads what this writes.
    void RenderShadows(bgfx::ViewId sunView, bgfx::ViewId skyView, const Scene& scene,
                       const MeshLibrary& meshes, const glm::vec3& focus);

    void Draw(bgfx::ViewId view, const Scene& scene, const MeshLibrary& meshes, const glm::vec3& cameraPosition);

    // Draws one mesh on its own, with an environment supplied by the caller rather than a scene.
    // The item icon atlas uses this so inventory icons are the real geometry under the real shader,
    // instead of a hand-drawn 2D stand-in that has to be redrawn for every new item.
    void DrawOne(bgfx::ViewId view, const Mesh& mesh, const Material& material, const glm::mat4& model,
                 const Environment& environment, const glm::vec3& cameraPosition);

    const Stats& LastStats() const { return m_stats; }
    bool WireframeEnabled() const { return m_wireframe; }
    void SetWireframe(bool enabled) { m_wireframe = enabled; }
    // Where the textures materials name actually live. Held rather than passed to every draw call,
    // because it is one object for the life of the renderer and threading it through would touch
    // every call site to say the same thing.
    void SetTextures(const TextureLibrary& textures) { m_textures = &textures; }

    // Live, because the graphics settings reach these and the player is meant to be able to see
    // what moving them does.
    ShadowSettings& Shadows() { return m_shadowSettings; }
    const ShadowSettings& Shadows() const { return m_shadowSettings; }
    bool ShadowsAvailable() const { return m_shadowsReady; }

private:
    // `withShadows` is false for the icon atlas, which draws one mesh with an environment of its
    // own and has no maps fitted to it: leaving them on would light icons through the world.
    void SetEnvironmentUniforms(const Environment& environment, const glm::vec3& cameraPosition,
                                bool withShadows);
    void SubmitDepth(bgfx::ViewId view, const Mesh& mesh, const glm::mat4& model,
                     bgfx::ProgramHandle program);
    uint64_t DrawState() const;
    void SubmitMesh(bgfx::ViewId view, const Mesh& mesh, const Material& material, const glm::mat4& model,
                    uint64_t state);

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
    bgfx::UniformHandle m_uGrade = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLights = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSunShadowMtx = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSunShadowAxis = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSunShadowParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSkyShadowMtx = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSkyShadowAxis = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSkyShadowParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sSunShadow = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sSkyShadow = BGFX_INVALID_HANDLE;
    // The base colour texture. Always bound, because a material with none samples the library's
    // single white pixel and the shader then needs no branch.
    bgfx::UniformHandle m_sBaseColor = BGFX_INVALID_HANDLE;
    const TextureLibrary* m_textures = nullptr;

    Stats m_stats;
    bool m_wireframe = false;

    // The two depth maps, and whether they could be created at all. A machine that cannot render to
    // a float target still gets a picture; it gets one with no occlusion in it.
    ShadowMap m_sunShadow;
    ShadowMap m_skyShadow;
    ShadowSettings m_shadowSettings;
    bool m_shadowsReady = false;
};

} // namespace pred
