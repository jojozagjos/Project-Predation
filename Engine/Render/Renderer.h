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
    // bgfx runs views in the order of their ids, so anything the main pass reads has to be drawn
    // into a lower one. The depth maps are therefore first, and the main view is not 0.
    //
    // The sun gets two, covering different amounts of world at the same resolution. A single map
    // wide enough to hold a building has texels several centimetres across, which is invisible on a
    // wall thirty metres away and very visible indeed on the shadow of your own head two metres in
    // front of you. The near one covers a few metres at a centimetre a texel and the far one picks
    // up where it stops.
    static constexpr bgfx::ViewId kViewSunNearShadow = 0;
    static constexpr bgfx::ViewId kViewSunShadow = 1;
    static constexpr bgfx::ViewId kViewSkyShadow = 2;
    static constexpr bgfx::ViewId kViewMain = 3;
    static constexpr bgfx::ViewId kViewDebug = 4;
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
    void SetClearColor(uint32_t rgba);
    void SetCamera(const glm::mat4& view, const glm::mat4& projection);
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
