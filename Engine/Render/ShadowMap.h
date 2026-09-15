#pragma once

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>

namespace pred
{

// An orthographic depth render of the world from one direction, so the shading pass can ask whether
// anything stands between a surface and a light.
//
// Two of these do all the occlusion this renderer has. One faces along the sun, and answers "is this
// in shadow". One faces straight down, and answers "is there a roof over this", which is the same
// question asked of the sky: a room is dark because it has a roof, not because something declared it
// dark. That second use is why this is a class rather than a member of the sun's shadow: the only
// difference between the two is the direction, the size of the texture, and what the answer is
// multiplied into.
//
// What it stores is distance from the light in metres, in a single-channel float target, rather than
// the hardware depth buffer. Depth buffers are stored differently on every backend and crowd their
// precision near the near plane; metres are metres, and the bias that goes with them is a distance
// anybody can reason about rather than a number found by trial.
class ShadowMap
{
public:
    // `resolution` is the square texture's size. The area it covers is set by Fit, so the two
    // together decide how much world each texel is responsible for.
    bool Init(uint16_t resolution, bgfx::ProgramHandle program);
    void Shutdown();
    bool IsValid() const { return bgfx::isValid(m_frameBuffer); }

    // Aims the map along `direction` at a box `radius` metres across, centred on `centre`.
    //
    // The centre is snapped to whole texels first. Without that, walking moves the sampling grid
    // under every shadow edge in the world and they crawl and sparkle; snapped, the grid moves in
    // whole texels and the edges stay put.
    void Fit(const glm::vec3& centre, const glm::vec3& direction, float radius, float depth);

    // Points a view at this map, ready for meshes to be submitted to it.
    void Begin(bgfx::ViewId view) const;

    bgfx::ProgramHandle Program() const { return m_program; }
    bgfx::TextureHandle Texture() const { return m_texture; }

    // World position -> this map's texture coordinates, with the backend's texture origin and clip
    // depth already folded in.
    const glm::mat4& TextureMatrix() const { return m_textureMatrix; }

    // Distance in metres from the light along its axis, for a world position P, is
    // dot(P, axis.xyz) + axis.w. The shading pass compares that against what this map stored.
    const glm::vec4& Axis() const { return m_axis; }

    // How much world one texel covers, so the shading pass can spread its samples in world terms
    // rather than in texture ones.
    float TexelSize() const { return m_texelSize; }
    uint16_t Resolution() const { return m_resolution; }

private:
    bgfx::FrameBufferHandle m_frameBuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_texture = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE; // owned by the ShaderLibrary
    uint16_t m_resolution = 0;
    float m_texelSize = 0.0f;
    float m_depthRange = 0.0f;
    bgfx::UniformHandle m_uRange = BGFX_INVALID_HANDLE;

    glm::mat4 m_view{1.0f};
    glm::mat4 m_projection{1.0f};
    glm::mat4 m_textureMatrix{1.0f};
    glm::vec4 m_axis{0.0f, -1.0f, 0.0f, 0.0f};
};

} // namespace pred
