#pragma once

#include <bgfx/bgfx.h>

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
        float coldShadows = 1.0f;
        float warmHighlights = 1.0f;
    };

    bool Init(ShaderLibrary& shaders);
    void Shutdown();

    // Makes sure the targets match the window and the multisampling, and says where the scene should
    // be drawn: the target, or the screen when post-processing is off or unavailable.
    bgfx::FrameBufferHandle Prepare(int width, int height, int msaa);

    // Adds the passes that finish the frame onto the screen. After the scene and before the interface.
    void Apply(float seconds, bool originBottomLeft);

    Settings& GetSettings() { return m_settings; }
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
};

} // namespace pred
