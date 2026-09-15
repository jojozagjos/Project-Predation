#pragma once

#include "Engine/Render/ShadowMap.h"
#include "Engine/Render/TextureLibrary.h"

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>

namespace pred
{

class Scene;
class MeshLibrary;
class ShaderLibrary;
class SkyRenderer;
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
    float distance = 24.0f;
    // And the radius of the near one, which is where the shadows anybody looks closely at are: your
    // own, and whatever you are standing next to.
    // belongs to, or every outside wall stands in the shade of its own roof.
    float sunBias = 0.05f;
    float sunNormalOffset = 0.06f;
    float skyBias = 0.16f;
    float skyNormalOffset = 0.05f;
    // The torch. A cone light gets its own map, because it is the light the player actually aims
    // and the one whose leaking through a wall is most obvious.
    bool spotEnabled = true;
    float spotBias = 0.035f;
    float spotNormalOffset = 0.03f;
    // How wide the torch.s cone is at its far end, for working out what one of its texels covers.
    // Set when the map is fitted.
    float spotRange = 14.0f;
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
    void RenderShadows(bgfx::ViewId sunView, bgfx::ViewId skyView, bgfx::ViewId spotView,
                       const Scene& scene, const MeshLibrary& meshes, const glm::vec3& focus);

    // Renders the world again from a camera reflected across `plane`, into a texture the mirror
    // samples. `plane` is xyz = normal, w = offset, in world space, pointing out of the mirror.
    //
    // One plane, not one per mirror. Several mirrors lying in the same plane share a reflection for
    // nothing, which is how the three panels in the test map work; mirrors in different planes need
    // a pass each and this renderer has room for one. That is a deliberate limit rather than an
    // oversight: a second pass over the whole scene is the most expensive thing in this renderer.
    void RenderReflection(bgfx::ViewId skyView, bgfx::ViewId worldView, const Scene& scene,
                          const MeshLibrary& meshes, const glm::vec4& plane,
                          const glm::mat4& cameraView, const glm::mat4& projection,
                          const glm::vec3& cameraPosition);
    // Turns it off for a frame, so nothing samples a stale or absent reflection.
    void NoReflection() { m_reflectionReady = false; }
    bool ReflectionAvailable() const { return m_reflectionReady; }
    // Live, so the graphics page can turn the second pass off on a machine that cannot afford it.
    bool ReflectionsEnabled() const { return m_reflections; }
    void SetReflectionsEnabled(bool enabled) { m_reflections = enabled; }
    // The sky, so the reflection pass can draw the same backdrop behind itself that the world has.
    // Held rather than passed, for the same reason the texture library is.
    void SetSky(SkyRenderer& sky) { m_sky = &sky; }

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
                     const ShadowMap& map);
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
    bgfx::UniformHandle m_uSpotShadowMtx = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSpotShadowAxis = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSpotShadowParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uShadowTexelWorld = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sSunShadow = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sSkyShadow = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sSpotShadow = BGFX_INVALID_HANDLE;
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
    // And one for the brightest cone light there is, which in this game is the torch. Without it a
    // punctual light has no occlusion at all and shines through walls.
    ShadowMap m_spotShadow;
    bool m_spotShadowLit = false;

    // The planar reflection target, and whether this frame has one in it.
    bgfx::FrameBufferHandle m_reflectionTarget = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_reflectionTexture = BGFX_INVALID_HANDLE;
    uint16_t m_reflectionWidth = 0;
    uint16_t m_reflectionHeight = 0;
    bool m_reflectionReady = false;
    bgfx::UniformHandle m_uClipPlane = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uReflectParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sReflection = BGFX_INVALID_HANDLE;
    bool m_reflections = true;
    SkyRenderer* m_sky = nullptr;
    ShadowSettings m_shadowSettings;
    bool m_shadowsReady = false;
};

} // namespace pred
