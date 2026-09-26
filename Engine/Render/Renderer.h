#pragma once

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace pred
{

struct RendererDesc
{
    void* nativeWindowHandle = nullptr;
    // The display connection the window belongs to. Null on Windows and macOS; on X11 and Wayland
    // a window on its own is not enough to draw into.
    void* nativeDisplay = nullptr;
    int width = 0;
    int height = 0;
    bool vsync = true;
    int msaa = 0;                 // 0, 2, 4, 8, 16
    std::string backend = "auto"; // auto | dx11 | dx12 | vulkan | opengl
    bool debug = false;           // backend validation layers where available
};

// Thin owner of the bgfx device. The renderer proper (materials, lights,
// shadows, post-processing) is built on top of this in later milestones.
// Game code never talks to bgfx directly.
class Renderer
{
public:
    // bgfx runs views in the order of their ids, so anything a later pass reads has to be drawn into
    // an earlier one. The two depth maps come first, then the sky, then the world on top of it.
    static constexpr bgfx::ViewId kViewSunShadow = 0;
    // The sun again, over only the few metres round the player, so the shadows you look at closely
    // are sharp while the far map reaches out to the shadow distance.
    static constexpr bgfx::ViewId kViewSunNearShadow = 1;
    static constexpr bgfx::ViewId kViewSkyShadow = 2;
    // The torch's own depth map, so a cone light is stopped by walls like everything else.
    static constexpr bgfx::ViewId kViewSpotShadow = 3;
    // The sky gets a view of its own rather than being the first thing submitted to the main one.
    // bgfx sorts the draws inside a view to save state changes, so "submitted first" is not
    // "drawn first" -- the sky came out over the top of the world. Views run in id order, and that
    // order is a promise.
    // The planar reflection: the world again from a mirrored camera, into a texture the main pass
    // samples. Two views into the one target for the same reason the world has two -- the sky writes
    // no depth, so sorted after the world inside a single view it would paint over it.
    //
    // After the depth maps, because the reflection is lit and wants the same occlusion the world
    // does, and before the world, because the world reads what it writes.
    static constexpr bgfx::ViewId kViewReflectionSky = 4;
    static constexpr bgfx::ViewId kViewReflection = 5;
    static constexpr bgfx::ViewId kViewSky = 6;
    static constexpr bgfx::ViewId kViewMain = 7;
    static constexpr bgfx::ViewId kViewDebug = 8;
    // Post-processing (PostProcess): the glow's chain of smaller and smaller copies and back, then the
    // finished picture onto the screen, before the interface.
    static constexpr bgfx::ViewId kViewPostFirst = 9;
    static constexpr bgfx::ViewId kViewPost = 18;
    // The lamps' shadows, a few faces a frame into their atlas. After the picture, not before it: what is
    // drawn here is used from the next frame, which is what a shadow drawn once and kept wants anyway.
    static constexpr bgfx::ViewId kViewLampShadowFirst = 120;
    // A block reserved for rendering into offscreen targets, such as the inventory icon atlas.
    // bgfx runs views in id order, so these are finished long before the UI that samples them.
    static constexpr bgfx::ViewId kViewOffscreenFirst = 200;
    static constexpr bgfx::ViewId kViewOffscreenCount = 48;
    // One more offscreen view, redrawn every frame rather than once: the editor's first-person
    // panel. Kept out of the block above so a long item list can never grow into it.
    static constexpr bgfx::ViewId kViewOffscreenLive = 249;
    static constexpr bgfx::ViewId kViewUI = 250;

    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init(const RendererDesc& desc);
    void Shutdown();
    bool IsInitialized() const;

    void Resize(int width, int height);
    void SetVSync(bool enabled);
    // Multisampling, 0 for none: 2, 4, 8 or 16. Applied at once.
    void SetMsaa(int samples);
    int Msaa() const;
    // Where the sky, the world and the debug lines are drawn: a target of post-processing's, or the
    // screen when there is none. With a target, the screen itself needs no multisampling -- the target
    // has it.
    void SetSceneTarget(bgfx::FrameBufferHandle target);
    void SetClearColor(uint32_t rgba);
    void SetCamera(const glm::mat4& view, const glm::mat4& projection);
    // What was last set, for passes that need the camera again after it was handed over -- the sky
    // is drawn from the render step and the matrices are built in the update one.
    const glm::mat4& ViewMatrix() const;
    const glm::mat4& ProjectionMatrix() const;
    void SetBgfxStatsOverlay(bool enabled);

    void BeginFrame();
    void EndFrame();

    // The screenshot is written as PNG by the bgfx callback when the frame completes.
    void RequestScreenshot(const std::filesystem::path& pngPath);

    int Width() const;
    int Height() const;
    bool VSync() const;
    bool HomogeneousDepth() const; // true for OpenGL-style [-1,1] clip depth
    bool OriginBottomLeft() const;
    const char* BackendName() const;
    std::string ShaderProfileDir() const; // "dx11", "spirv", ...
    const bgfx::Stats* Stats() const;
    double GpuFrameMs() const;
    double BgfxCpuFrameMs() const;
    uint32_t FrameNumber() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace pred
