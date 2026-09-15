#include "Engine/Render/ShadowMap.h"

#include "Engine/Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/geometric.hpp>

#include <cmath>

namespace pred
{

bool ShadowMap::Init(uint16_t resolution, bgfx::ProgramHandle program)
{
    if (!bgfx::isValid(program))
    {
        PRED_LOG_ERROR(Render, "ShadowMap: no depth program");
        return false;
    }
    const uint32_t caps = bgfx::getCaps()->formats[bgfx::TextureFormat::R32F];
    if ((caps & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) == 0)
    {
        PRED_LOG_WARN(Render, "ShadowMap: this GPU cannot render to R32F; shadows are off");
        return false;
    }

    m_program = program;
    m_resolution = resolution;
    m_uRange = bgfx::createUniform("u_shadowRange", bgfx::UniformType::Vec4);

    // Point sampling and clamped edges. Filtering between two distances gives a distance that
    // belongs to neither surface, which reads as a band of shadow along every silhouette; softening
    // is done by taking several samples and averaging the *answers*, not the distances. Clamping
    // means a lookup that falls outside the map repeats the edge, which the shading pass rejects
    // anyway by checking the coordinates.
    m_texture = bgfx::createTexture2D(resolution, resolution, false, 1, bgfx::TextureFormat::R32F,
                                      BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                          BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP |
                                          BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(m_texture))
    {
        PRED_LOG_ERROR(Render, "ShadowMap: could not create a {}x{} target", resolution, resolution);
        return false;
    }

    const bgfx::TextureHandle depth =
        bgfx::createTexture2D(resolution, resolution, false, 1, bgfx::TextureFormat::D24S8,
                              BGFX_TEXTURE_RT_WRITE_ONLY);
    const bgfx::TextureHandle attachments[] = {m_texture, depth};
    // The frame buffer owns both textures from here: destroying it destroys them.
    m_frameBuffer = bgfx::createFrameBuffer(2, attachments, true);
    if (!bgfx::isValid(m_frameBuffer))
    {
        PRED_LOG_ERROR(Render, "ShadowMap: could not create the frame buffer");
        return false;
    }

    PRED_LOG_INFO(Render, "Shadow map ready: {}x{}", resolution, resolution);
    return true;
}

void ShadowMap::Shutdown()
{
    if (bgfx::isValid(m_frameBuffer))
    {
        bgfx::destroy(m_frameBuffer); // takes its attachments with it
    }
    m_frameBuffer = BGFX_INVALID_HANDLE;
    m_texture = BGFX_INVALID_HANDLE;
    m_program = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(m_uRange))
    {
        bgfx::destroy(m_uRange);
    }
    m_uRange = BGFX_INVALID_HANDLE;
}

void ShadowMap::Fit(const glm::vec3& centre, const glm::vec3& direction, float radius, float depth)
{
    if (m_resolution == 0)
    {
        return;
    }
    radius = std::max(radius, 1.0f);
    m_texelSize = (radius * 2.0f) / static_cast<float>(m_resolution);

    glm::vec3 forward = direction;
    if (glm::length(forward) < 1e-4f)
    {
        forward = glm::vec3(0.0f, -1.0f, 0.0f);
    }
    forward = glm::normalize(forward);
    // Any up vector will do as long as it is not the direction itself, which is the one case where
    // a look-at matrix has no answer. Straight down is exactly that case and it is the sky map's
    // normal state, so it is picked deliberately rather than guarded against.
    const glm::vec3 up =
        std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

    // Stand back far enough that everything between the light and the area is in front of the near
    // plane. Half the depth range, so the box is centred on what it is looking at.
    const glm::vec3 eye = centre - forward * (depth * 0.5f);
    glm::mat4 view = glm::lookAtRH(eye, eye + forward, up);

    // Snap to whole texels, in the light's own space rather than the world's. A shadow edge lives
    // at a texel boundary; if the boundaries slide as the player walks, every edge in the world
    // shimmers. Moving the map in whole texels keeps them still.
    const glm::vec3 viewCentre = glm::vec3(view * glm::vec4(centre, 1.0f));
    const glm::vec2 snapped{std::round(viewCentre.x / m_texelSize) * m_texelSize,
                            std::round(viewCentre.y / m_texelSize) * m_texelSize};
    view = glm::translate(glm::mat4(1.0f),
                          glm::vec3(snapped.x - viewCentre.x, snapped.y - viewCentre.y, 0.0f)) *
           view;

    const bgfx::Caps* caps = bgfx::getCaps();
    m_projection = caps->homogeneousDepth
                       ? glm::orthoRH_NO(-radius, radius, -radius, radius, 0.0f, depth)
                       : glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, depth);
    m_view = view;

    // World -> texture coordinates. The clip cube runs -1..1 across the screen and the texture runs
    // 0..1, so the extra step is a halving and a shift; the vertical is flipped on the backends
    // whose textures start at the top. Doing it here rather than in the shader means the shader is
    // the same on every backend and this is written down once.
    const float flipY = caps->originBottomLeft ? 0.5f : -0.5f;
    glm::mat4 toTexture(1.0f);
    toTexture[0][0] = 0.5f;
    toTexture[1][1] = flipY;
    toTexture[3][0] = 0.5f;
    toTexture[3][1] = 0.5f;
    m_textureMatrix = toTexture * m_projection * m_view;

    // Distance from the *back* of the map for a world point, as a plane equation, matching what the
    // depth shader stored: see fs_shadow.sc for why it is that way up rather than distance from the
    // light. The third row of the view matrix is the light's axis, and the sign flips twice -- once
    // because a right-handed view looks down its own -Z, and once for the reversal.
    m_depthRange = depth;
    m_axis = glm::vec4(m_view[0][2], m_view[1][2], m_view[2][2], depth + m_view[3][2]);
}

void ShadowMap::Begin(bgfx::ViewId view) const
{
    if (!IsValid())
    {
        return;
    }
    const float range[4] = {m_depthRange, 0.0f, 0.0f, 0.0f};
    bgfx::setUniform(m_uRange, range);
    bgfx::setViewFrameBuffer(view, m_frameBuffer);
    bgfx::setViewRect(view, 0, 0, m_resolution, m_resolution);
    // Cleared to plain black, which in this encoding means "as far away as this map can see".
    bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
    bgfx::setViewTransform(view, glm::value_ptr(m_view), glm::value_ptr(m_projection));
    bgfx::touch(view);
}

} // namespace pred
