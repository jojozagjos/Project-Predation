#pragma once

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>

#include <array>

namespace pred
{

class ShaderLibrary;

// What happens to the picture after the scene is drawn and before it reaches the screen.
//
// The scene is drawn into a target of its own in linear light, unexposed -- values above one allowed,
// which is what a lamp is -- and this finishes it: the bright parts glow (bloom, from a chain of ever
// smaller copies), then exposure, the filmic curve, gamma and contrast, a colour grade, a vignette, a
// lens's colour fringe at the edges, film grain, and, driven by the game, fear: the edges closing in
// and the colour draining, in time with a pulse.
//
// Before this existed all of that but the glow was done at the end of every surface's own shader,
// which left nothing to glow from and no screen to vignette.
class PostProcess
{
public:
    struct Settings
    {
        bool enabled = true;
        float exposure = 1.0f;
        float contrast = 1.0f;
        float bloom = 0.6f;          // how much of the glow is added
        float bloomThreshold = 0.9f; // how bright, once exposed, something has to be to glow
        float saturation = 0.85f;
        float vignette = 0.35f;
        float grain = 0.035f;
        float fringe = 0.004f;
        float fear = 0.0f;           // 0 to 1, set by the game
        float flash = 0.0f;          // 0 to 1: the picture washed red, for a moment
        // The eye adapting to how bright the picture is (fs_post_adapt): how far it brings the picture back towards the
        // brightness it is set for (0 not at all, 1 all the way), that brightness, and how much darker and brighter that can
        // make it at most; and the seconds since last frame, which it adapts over.
        float adapt = 0.25f;
        float adaptReference = 0.11f;
        float adaptDarker = 0.7f;
        float adaptBrighter = 1.4f;
        float deltaSeconds = 1.0f / 60.0f;
        float coldShadows = 1.0f;
        float warmHighlights = 1.0f;
        // How much the shut-in parts of the picture are darkened -- corners, under things, the ground at a boot -- by how much
        // of the open world round them other surfaces take up (ambient occlusion, fs_post_ao; 0 off), out to how far round
        // (metres); and the picture's projection, to find where each of its points is. Only without multisampling.
        float ambientOcclusion = 0.7f;
        float occlusionRadius = 0.6f;
        glm::mat4 projection{1.0f};
        // Where the sun is in the picture (x, y as the projection has it, -1 to 1; z = 1 when it is ahead of the eye) and how
        // much it flares in the lens (fs_post_final; 0 none).
        glm::vec3 sunAhead{0.0f};
        float lensFlare = 0.35f;
        // Where on the screen the finished picture goes, in pixels from the top left: all of it when the width is
        // 0, or a smaller window of it, the whole picture scaled down into it -- an editor's preview, framed exactly
        // as the game frames it.
        int outX = 0;
        int outY = 0;
        int outWidth = 0;
        int outHeight = 0;
    };

    bool Init(ShaderLibrary& shaders);
    void Shutdown();

    // Makes sure the targets match the window and the multisampling, and says where the scene should
    // be drawn: the target, or the screen when post-processing is off or unavailable.
    bgfx::FrameBufferHandle Prepare(int width, int height, int msaa);

    // Adds the passes that finish the frame onto the screen. After the scene and before the interface.
    void Apply(float seconds, bool originBottomLeft);

    Settings& GetSettings() { return m_settings; }
    // How bright the picture has been lately, as the eye has adapted to it (linear light): read back from the graphics card
    // every so often, for tuning `adaptReference` by; nought until the first reading.
    float MeasuredLuminance() const { return m_measured; }
    bool Active() const;

private:
    static constexpr int kBloomLevels = 5;
    void DestroyTargets();
    void FullScreen(bgfx::ViewId view, bgfx::ProgramHandle program, uint64_t state);

    Settings m_settings;
    bgfx::ProgramHandle m_bright = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_down = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_up = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_final = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_lumProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_avgProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_adaptProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sAdapt = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAdapt = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAdaptRate = BGFX_INVALID_HANDLE;
    // Ambient occlusion: its passes, the scene's depth to work it from, and what it makes, then that smoothed.
    bgfx::ProgramHandle m_aoProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_aoBlurProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sAo = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAoProj = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAoInvProj = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAoParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSun = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_sceneDepth = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_ao = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_aoColor = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_aoBlur = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_aoBlurColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sScene = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sBloom = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTexel = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uTone = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLens = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uMood = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uFlip = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle m_triangle = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout m_layout;

    bgfx::FrameBufferHandle m_scene = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_sceneColor = BGFX_INVALID_HANDLE;
    std::array<bgfx::FrameBufferHandle, kBloomLevels> m_bloom{};
    std::array<bgfx::TextureHandle, kBloomLevels> m_bloomColor{};
    std::array<int, kBloomLevels> m_bloomWidth{};
    std::array<int, kBloomLevels> m_bloomHeight{};
    int m_width = 0;
    int m_height = 0;
    int m_msaa = -1;
    // The eye: the picture's brightness 64 across, then in one, then adapted to (two, last frame's and this one's, turn and
    // turn about), and somewhere to read it back from.
    bgfx::FrameBufferHandle m_lum = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_lumColor = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_avg = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_avgColor = BGFX_INVALID_HANDLE;
    std::array<bgfx::FrameBufferHandle, 2> m_adapt{};
    std::array<bgfx::TextureHandle, 2> m_adaptColor{};
    int m_adaptCurrent = 0;
    bool m_adaptFresh = true;
    bgfx::TextureHandle m_readback = BGFX_INVALID_HANDLE;
    float m_readValue = 0.0f;
    int m_readIn = 0;
    bool m_reading = false;
    float m_measured = 0.0f;
};

} // namespace pred
