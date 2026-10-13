#include "Engine/Render/PostProcess.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/DepthConvention.h"
#include "Engine/Render/Renderer.h"
#include "Engine/Render/ShaderLibrary.h"

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

uint64_t MsaaTextureFlag(int msaa)
{
    switch (msaa)
    {
    case 2:
        return BGFX_TEXTURE_RT_MSAA_X2;
    case 4:
        return BGFX_TEXTURE_RT_MSAA_X4;
    case 8:
        return BGFX_TEXTURE_RT_MSAA_X8;
    case 16:
        return BGFX_TEXTURE_RT_MSAA_X16;
    default:
        return BGFX_TEXTURE_RT;
    }
}

} // namespace

bool PostProcess::Init(ShaderLibrary& shaders)
{
    m_bright = shaders.LoadProgram("vs_post", "fs_post_bright");
    m_down = shaders.LoadProgram("vs_post", "fs_post_down");
    m_up = shaders.LoadProgram("vs_post", "fs_post_up");
    m_final = shaders.LoadProgram("vs_post", "fs_post_final");
    if (!bgfx::isValid(m_bright) || !bgfx::isValid(m_down) || !bgfx::isValid(m_up) || !bgfx::isValid(m_final))
    {
        PRED_LOG_WARN(Render, "PostProcess: its shaders are missing; the scene goes straight to the screen");
        return false;
    }
    if ((bgfx::getCaps()->formats[bgfx::TextureFormat::RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) == 0)
    {
        PRED_LOG_WARN(Render, "PostProcess: this device cannot render to half floats; the scene goes straight to the screen");
        return false;
    }
    m_layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    static const float kCorners[] = {-1.0f, -1.0f, 0.0f, 3.0f, -1.0f, 0.0f, -1.0f, 3.0f, 0.0f};
    m_triangle = bgfx::createVertexBuffer(bgfx::makeRef(kCorners, sizeof(kCorners)), m_layout);
    m_sScene = bgfx::createUniform("s_scene", bgfx::UniformType::Sampler);
    m_sBloom = bgfx::createUniform("s_bloom", bgfx::UniformType::Sampler);
    m_uTexel = bgfx::createUniform("u_postTexel", bgfx::UniformType::Vec4);
    m_uTone = bgfx::createUniform("u_postTone", bgfx::UniformType::Vec4);
    m_uLens = bgfx::createUniform("u_postLens", bgfx::UniformType::Vec4);
    m_uMood = bgfx::createUniform("u_postMood", bgfx::UniformType::Vec4);
    m_uFlip = bgfx::createUniform("u_postFlip", bgfx::UniformType::Vec4);
    for (size_t i = 0; i < m_bloom.size(); ++i)
    {
        m_bloom[i] = BGFX_INVALID_HANDLE;
        m_bloomColor[i] = BGFX_INVALID_HANDLE;
    }

    // The eye: its own small targets, the same whatever the screen. Without them (or its shaders) the picture is simply not
    // adapted.
    for (size_t i = 0; i < m_adapt.size(); ++i)
    {
        m_adapt[i] = BGFX_INVALID_HANDLE;
        m_adaptColor[i] = BGFX_INVALID_HANDLE;
    }
    m_lumProgram = shaders.LoadProgram("vs_post", "fs_post_lum");
    m_avgProgram = shaders.LoadProgram("vs_post", "fs_post_avg");
    m_adaptProgram = shaders.LoadProgram("vs_post", "fs_post_adapt");
    m_sAdapt = bgfx::createUniform("s_adapt", bgfx::UniformType::Sampler);
    m_uAdapt = bgfx::createUniform("u_postAdapt", bgfx::UniformType::Vec4);
    m_uAdaptRate = bgfx::createUniform("u_postAdaptRate", bgfx::UniformType::Vec4);
    const bool floats = (bgfx::getCaps()->formats[bgfx::TextureFormat::R32F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) != 0;
    if (floats && bgfx::isValid(m_lumProgram) && bgfx::isValid(m_avgProgram) && bgfx::isValid(m_adaptProgram))
    {
        const uint64_t point = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        m_lumColor = bgfx::createTexture2D(64, 64, false, 1, bgfx::TextureFormat::R32F, point);
        m_lum = bgfx::createFrameBuffer(1, &m_lumColor, true);
        m_avgColor = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::R32F, point | BGFX_SAMPLER_POINT);
        m_avg = bgfx::createFrameBuffer(1, &m_avgColor, true);
        for (size_t i = 0; i < m_adapt.size(); ++i)
        {
            m_adaptColor[i] = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::R32F, point | BGFX_SAMPLER_POINT);
            m_adapt[i] = bgfx::createFrameBuffer(1, &m_adaptColor[i], true);
        }
        if ((bgfx::getCaps()->supported & (BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK)) == (BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK))
        {
            m_readback = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::R32F, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        }
    }
    return true;
}

void PostProcess::DestroyTargets()
{
    if (bgfx::isValid(m_scene))
    {
        bgfx::destroy(m_scene);
    }
    m_scene = BGFX_INVALID_HANDLE;
    m_sceneColor = BGFX_INVALID_HANDLE;
    for (size_t i = 0; i < m_bloom.size(); ++i)
    {
        if (bgfx::isValid(m_bloom[i]))
        {
            bgfx::destroy(m_bloom[i]);
        }
        m_bloom[i] = BGFX_INVALID_HANDLE;
        m_bloomColor[i] = BGFX_INVALID_HANDLE;
    }
    m_width = 0;
    m_height = 0;
}

void PostProcess::Shutdown()
{
    DestroyTargets();
    const bgfx::FrameBufferHandle eye[] = {m_lum, m_avg, m_adapt[0], m_adapt[1]};
    for (const bgfx::FrameBufferHandle handle : eye)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle); // its texture with it
        }
    }
    m_lum = m_avg = m_adapt[0] = m_adapt[1] = BGFX_INVALID_HANDLE;
    m_lumColor = m_avgColor = m_adaptColor[0] = m_adaptColor[1] = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(m_readback))
    {
        bgfx::destroy(m_readback);
    }
    m_readback = BGFX_INVALID_HANDLE;
    const bgfx::UniformHandle eyeUniforms[] = {m_sAdapt, m_uAdapt, m_uAdaptRate};
    for (const bgfx::UniformHandle handle : eyeUniforms)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    m_sAdapt = m_uAdapt = m_uAdaptRate = BGFX_INVALID_HANDLE;
    m_lumProgram = m_avgProgram = m_adaptProgram = BGFX_INVALID_HANDLE;
    const bgfx::UniformHandle uniforms[] = {m_sScene, m_sBloom, m_uTexel, m_uTone, m_uLens, m_uMood, m_uFlip};
    for (const bgfx::UniformHandle handle : uniforms)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    m_sScene = m_sBloom = m_uTexel = m_uTone = m_uLens = m_uMood = m_uFlip = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(m_triangle))
    {
        bgfx::destroy(m_triangle);
    }
    m_triangle = BGFX_INVALID_HANDLE;
    // The programs belong to the shader library.
    m_bright = m_down = m_up = m_final = BGFX_INVALID_HANDLE;
}

bool PostProcess::Active() const
{
    return m_settings.enabled && bgfx::isValid(m_final) && bgfx::isValid(m_scene);
}

bgfx::FrameBufferHandle PostProcess::Prepare(int width, int height, int msaa)
{
    if (!m_settings.enabled || !bgfx::isValid(m_final) || width <= 0 || height <= 0)
    {
        DestroyTargets();
        return BGFX_INVALID_HANDLE;
    }
    if (width == m_width && height == m_height && msaa == m_msaa && bgfx::isValid(m_scene))
    {
        return m_scene;
    }
    DestroyTargets();
    m_width = width;
    m_height = height;
    m_msaa = msaa;
    const auto w = static_cast<uint16_t>(width);
    const auto h = static_cast<uint16_t>(height);
    const uint64_t sampling = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    m_sceneColor = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA16F, MsaaTextureFlag(msaa) | sampling);
    const bgfx::TextureHandle depth =
        bgfx::createTexture2D(w, h, false, 1, Depth::Format(), MsaaTextureFlag(msaa) | BGFX_TEXTURE_RT_WRITE_ONLY);
    const bgfx::TextureHandle attachments[] = {m_sceneColor, depth};
    m_scene = bgfx::createFrameBuffer(2, attachments, true);
    int levelWidth = width;
    int levelHeight = height;
    for (int i = 0; i < kBloomLevels; ++i)
    {
        levelWidth = std::max(levelWidth / 2, 1);
        levelHeight = std::max(levelHeight / 2, 1);
        m_bloomWidth[static_cast<size_t>(i)] = levelWidth;
        m_bloomHeight[static_cast<size_t>(i)] = levelHeight;
        m_bloomColor[static_cast<size_t>(i)] =
            bgfx::createTexture2D(static_cast<uint16_t>(levelWidth), static_cast<uint16_t>(levelHeight), false, 1,
                                  bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | sampling);
        m_bloom[static_cast<size_t>(i)] = bgfx::createFrameBuffer(1, &m_bloomColor[static_cast<size_t>(i)], true);
    }
    if (!bgfx::isValid(m_scene))
    {
        PRED_LOG_WARN(Render, "PostProcess: could not make a {}x{} target; the scene goes straight to the screen", width, height);
        DestroyTargets();
        return BGFX_INVALID_HANDLE;
    }
    PRED_LOG_INFO(Render, "PostProcess: {}x{} target, msaa {}, {} glow levels", width, height, msaa, kBloomLevels);
    return m_scene;
}

void PostProcess::FullScreen(bgfx::ViewId view, bgfx::ProgramHandle program, uint64_t state)
{
    bgfx::setVertexBuffer(0, m_triangle);
    bgfx::setState(state);
    bgfx::submit(view, program);
}

void PostProcess::Apply(float seconds, bool originBottomLeft)
{
    if (!Active())
    {
        return;
    }
    const Settings& s = m_settings;
    const float flip[4] = {originBottomLeft ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    const float tone[4] = {s.exposure, s.contrast, s.bloom, s.bloomThreshold};
    const uint64_t write = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    const auto texel = [&](int w, int h)
    {
        const float value[4] = {1.0f / static_cast<float>(std::max(w, 1)), 1.0f / static_cast<float>(std::max(h, 1)),
                                static_cast<float>(m_width), static_cast<float>(m_height)};
        bgfx::setUniform(m_uTexel, value);
    };
    const auto target = [&](bgfx::ViewId view, int level, bool clear)
    {
        bgfx::setViewFrameBuffer(view, m_bloom[static_cast<size_t>(level)]);
        bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(m_bloomWidth[static_cast<size_t>(level)]),
                          static_cast<uint16_t>(m_bloomHeight[static_cast<size_t>(level)]));
        bgfx::setViewClear(view, clear ? BGFX_CLEAR_COLOR : BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
    };

    // The glow: the bright parts at half size, then smaller and smaller, then back up, each level
    // added onto the one above.
    bgfx::ViewId view = Renderer::kViewPostFirst;
    target(view, 0, true);
    bgfx::setUniform(m_uFlip, flip);
    bgfx::setUniform(m_uTone, tone);
    texel(m_width, m_height);
    bgfx::setTexture(0, m_sScene, m_sceneColor);
    FullScreen(view, m_bright, write);
    ++view;
    for (int level = 1; level < kBloomLevels; ++level, ++view)
    {
        target(view, level, true);
        bgfx::setUniform(m_uFlip, flip);
        texel(m_bloomWidth[static_cast<size_t>(level - 1)], m_bloomHeight[static_cast<size_t>(level - 1)]);
        bgfx::setTexture(0, m_sScene, m_bloomColor[static_cast<size_t>(level - 1)]);
        FullScreen(view, m_down, write);
    }
    for (int level = kBloomLevels - 2; level >= 0; --level, ++view)
    {
        target(view, level, false);
        bgfx::setUniform(m_uFlip, flip);
        texel(m_bloomWidth[static_cast<size_t>(level + 1)], m_bloomHeight[static_cast<size_t>(level + 1)]);
        bgfx::setTexture(0, m_sScene, m_bloomColor[static_cast<size_t>(level + 1)]);
        FullScreen(view, m_up, write | BGFX_STATE_BLEND_ADD);
    }

    // The eye: how bright the picture is, in one number, and what it has adapted to by now -- the one from last frame
    // carried on towards it.
    // (Measured whether or not it is used, so it can be measured with it off.)
    const bool eye = bgfx::isValid(m_lum) && bgfx::isValid(m_avg) && bgfx::isValid(m_adapt[0]) && bgfx::isValid(m_adapt[1]);
    if (eye)
    {
        const auto small = [&](bgfx::ViewId at, bgfx::FrameBufferHandle into, int size)
        {
            bgfx::setViewFrameBuffer(at, into);
            bgfx::setViewRect(at, 0, 0, static_cast<uint16_t>(size), static_cast<uint16_t>(size));
            bgfx::setViewClear(at, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
        };
        small(Renderer::kViewAdaptFirst, m_lum, 64);
        bgfx::setUniform(m_uFlip, flip);
        texel(m_width, m_height);
        bgfx::setTexture(0, m_sScene, m_sceneColor);
        FullScreen(Renderer::kViewAdaptFirst, m_lumProgram, write);
        small(Renderer::kViewAdaptFirst + 1, m_avg, 1);
        bgfx::setUniform(m_uFlip, flip);
        bgfx::setTexture(0, m_sScene, m_lumColor);
        FullScreen(Renderer::kViewAdaptFirst + 1, m_avgProgram, write);
        const int before = m_adaptCurrent;
        m_adaptCurrent = 1 - m_adaptCurrent;
        small(Renderer::kViewAdaptFirst + 2, m_adapt[static_cast<size_t>(m_adaptCurrent)], 1);
        // To brighter in about half a second, to darker over three or four.
        const float rate[4] = {std::clamp(s.deltaSeconds, 0.0f, 0.25f), 4.0f, 0.7f, m_adaptFresh ? 1.0f : 0.0f};
        bgfx::setUniform(m_uFlip, flip);
        bgfx::setUniform(m_uAdaptRate, rate);
        bgfx::setTexture(0, m_sScene, m_avgColor);
        bgfx::setTexture(1, m_sBloom, m_adaptColor[static_cast<size_t>(before)]);
        FullScreen(Renderer::kViewAdaptFirst + 2, m_adaptProgram, write);
        m_adaptFresh = false;
        // Now and then, read back what it came to, for tuning by.
        if (bgfx::isValid(m_readback))
        {
            if (m_reading && --m_readIn <= 0)
            {
                m_measured = std::exp(m_readValue);
                m_reading = false;
            }
            if (!m_reading)
            {
                bgfx::blit(Renderer::kViewAdaptFirst + 3, m_readback, 0, 0, m_adaptColor[static_cast<size_t>(m_adaptCurrent)]);
                bgfx::readTexture(m_readback, &m_readValue);
                m_reading = true;
                m_readIn = 30;
            }
        }
    }

    // And the picture, onto the screen.
    const bgfx::ViewId last = Renderer::kViewPost;
    bgfx::setViewFrameBuffer(last, BGFX_INVALID_HANDLE);
    if (s.outWidth > 0 && s.outHeight > 0)
    {
        bgfx::setViewRect(last, static_cast<uint16_t>(std::max(s.outX, 0)), static_cast<uint16_t>(std::max(s.outY, 0)), static_cast<uint16_t>(s.outWidth),
                          static_cast<uint16_t>(s.outHeight));
    }
    else
    {
        bgfx::setViewRect(last, 0, 0, static_cast<uint16_t>(m_width), static_cast<uint16_t>(m_height));
    }
    bgfx::setViewClear(last, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
    const float lens[4] = {s.vignette, s.grain, s.fringe, seconds};
    const float mood[4] = {s.fear, s.coldShadows, s.warmHighlights, s.flash};
    const float finalTone[4] = {s.exposure, s.contrast, s.bloom, s.saturation};
    bgfx::setUniform(m_uFlip, flip);
    texel(m_width, m_height);
    bgfx::setUniform(m_uTone, finalTone);
    bgfx::setUniform(m_uLens, lens);
    bgfx::setUniform(m_uMood, mood);
    bgfx::setTexture(0, m_sScene, m_sceneColor);
    bgfx::setTexture(1, m_sBloom, m_bloomColor[0]);
    const float adapt[4] = {std::max(s.adaptReference, 1.0e-4f), eye ? s.adapt : 0.0f, s.adaptDarker, s.adaptBrighter};
    bgfx::setUniform(m_uAdapt, adapt);
    if (eye)
    {
        bgfx::setTexture(2, m_sAdapt, m_adaptColor[static_cast<size_t>(m_adaptCurrent)]);
    }
    FullScreen(last, m_final, write);
}

} // namespace pred
