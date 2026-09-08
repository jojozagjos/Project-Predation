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
    static constexpr bgfx::ViewId kViewMain = 0;
    static constexpr bgfx::ViewId kViewDebug = 1;
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
