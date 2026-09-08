#include "Engine/Render/Renderer.h"

#include "Engine/Core/Log.h"

#include <bgfx/platform.h>
#include <bx/bx.h>
#include <bx/debug.h>
#include <glm/gtc/type_ptr.hpp>
#include <stb_image_write.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace pred
{
namespace
{

bgfx::RendererType::Enum ParseBackend(const std::string& name)
{
    if (name == "dx11" || name == "d3d11")
    {
        return bgfx::RendererType::Direct3D11;
    }
    if (name == "dx12" || name == "d3d12")
    {
        return bgfx::RendererType::Direct3D12;
    }
    if (name == "vulkan" || name == "vk")
    {
        return bgfx::RendererType::Vulkan;
    }
    if (name == "opengl" || name == "gl")
    {
        return bgfx::RendererType::OpenGL;
    }
    if (name != "auto")
    {
        PRED_LOG_WARN(Render, "Unknown backend '{}', using automatic selection", name);
    }
    return bgfx::RendererType::Count;
}

uint32_t MsaaFlag(int samples)
{
    switch (samples)
    {
    case 2:
        return BGFX_RESET_MSAA_X2;
    case 4:
        return BGFX_RESET_MSAA_X4;
    case 8:
        return BGFX_RESET_MSAA_X8;
    case 16:
        return BGFX_RESET_MSAA_X16;
    default:
        return 0;
    }
}

// Routes bgfx diagnostics into our log and writes screenshots as PNG.
class BgfxCallback final : public bgfx::CallbackI
{
public:
    void fatal(const char* filePath, uint16_t line, bgfx::Fatal::Enum code, const char* str) override
    {
        PRED_LOG_CRITICAL(Render, "bgfx fatal ({}:{}) code {}: {}", filePath ? filePath : "?", line,
                          static_cast<int>(code), str ? str : "");
        Log::Flush();
        if (code == bgfx::Fatal::DebugCheck)
        {
            bx::debugBreak();
        }
        else
        {
            std::abort();
        }
    }

    void traceVargs(const char* filePath, uint16_t line, const char* format, va_list argList) override
    {
        char buffer[2048];
        std::vsnprintf(buffer, sizeof(buffer), format, argList);
        std::string text(buffer);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        {
            text.pop_back();
        }
        PRED_LOG_DEBUG(Render, "bgfx: {}", text);
        BX_UNUSED(filePath, line);
    }

    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}

    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}

    // bgfx changed this callback's signature (a TextureFormat parameter was added) between releases.
    // Both forms are provided without `override` so whichever one the installed bgfx declares as pure
    // virtual is implemented; the other is an unused member function.
    void screenShot(const char* filePath, uint32_t width, uint32_t height, uint32_t pitch, const void* data,
                    uint32_t size, bool yflip)
    {
        WriteScreenshotPng(filePath, width, height, pitch, data, size, yflip);
    }

    void screenShot(const char* filePath, uint32_t width, uint32_t height, uint32_t pitch,
                    bgfx::TextureFormat::Enum format, const void* data, uint32_t size, bool yflip)
    {
        if (format != bgfx::TextureFormat::BGRA8 && format != bgfx::TextureFormat::RGBA8)
        {
            PRED_LOG_WARN(Render, "Screenshot format {} is not BGRA8/RGBA8; colors may be wrong",
                          static_cast<int>(format));
        }
        WriteScreenshotPng(filePath, width, height, pitch, data, size, yflip);
    }

    static void WriteScreenshotPng(const char* filePath, uint32_t width, uint32_t height, uint32_t pitch,
                                   const void* data, uint32_t size, bool yflip)
    {
        BX_UNUSED(size);
        // bgfx hands us BGRA8; convert to RGBA8 for PNG.
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        const auto* src = static_cast<const uint8_t*>(data);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t srcRow = yflip ? (height - 1 - y) : y;
            const uint8_t* row = src + static_cast<size_t>(srcRow) * pitch;
            uint8_t* dst = rgba.data() + static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x)
            {
                dst[x * 4 + 0] = row[x * 4 + 2];
                dst[x * 4 + 1] = row[x * 4 + 1];
                dst[x * 4 + 2] = row[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
        if (stbi_write_png(filePath, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                           static_cast<int>(width * 4)) != 0)
        {
            PRED_LOG_INFO(Render, "Screenshot saved: {} ({}x{})", filePath, width, height);
        }
        else
        {
            PRED_LOG_ERROR(Render, "Failed to write screenshot: {}", filePath);
        }
    }

    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}
};

} // namespace

struct Renderer::Impl
{
    BgfxCallback callback;
    bool initialized = false;
    int width = 0;
    int height = 0;
    bool vsync = true;
    int msaa = 0;
    uint32_t clearColor = 0x1a1a20ff;
    uint32_t debugFlags = BGFX_DEBUG_NONE;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    std::string pendingScreenshot;
    uint32_t frameNumber = 0;

    uint32_t ResetFlags() const { return (vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE) | MsaaFlag(msaa); }
};

Renderer::Renderer() : m_impl(std::make_unique<Impl>()) {}

Renderer::~Renderer()
{
    Shutdown();
}

bool Renderer::Init(const RendererDesc& desc)
{
    Impl& impl = *m_impl;
    impl.width = desc.width;
    impl.height = desc.height;
    impl.vsync = desc.vsync;
    impl.msaa = desc.msaa;

    // Calling renderFrame before init keeps bgfx single-threaded: simpler to debug,
    // and the multithreaded encoder path can be enabled later without API changes.
    bgfx::renderFrame();

    bgfx::Init init;
    init.type = ParseBackend(desc.backend);
    init.vendorId = BGFX_PCI_ID_NONE;
    init.platformData.nwh = desc.nativeWindowHandle;
    init.platformData.ndt = nullptr;
    init.resolution.width = static_cast<uint32_t>(desc.width);
    init.resolution.height = static_cast<uint32_t>(desc.height);
    init.resolution.reset = impl.ResetFlags();
    init.callback = &impl.callback;
    init.debug = desc.debug;
    init.profile = true;

    if (!bgfx::init(init))
    {
        PRED_LOG_ERROR(Render, "bgfx::init failed (backend '{}')", desc.backend);
        return false;
    }
    impl.initialized = true;

    const bgfx::Caps* caps = bgfx::getCaps();
    PRED_LOG_INFO(Render, "Renderer: {} | {}x{} | vsync {} | msaa {} | homogeneousDepth {} | maxTexture {}",
                  bgfx::getRendererName(caps->rendererType), desc.width, desc.height, desc.vsync, desc.msaa,
                  caps->homogeneousDepth, caps->limits.maxTextureSize);

    bgfx::setDebug(impl.debugFlags);
    bgfx::setViewName(kViewMain, "Main");
    bgfx::setViewName(kViewDebug, "DebugDraw");
    return true;
}

void Renderer::Shutdown()
{
    if (m_impl && m_impl->initialized)
    {
        bgfx::shutdown();
        m_impl->initialized = false;
        PRED_LOG_INFO(Render, "Renderer shut down");
    }
}

bool Renderer::IsInitialized() const
{
    return m_impl->initialized;
}

void Renderer::Resize(int width, int height)
{
    Impl& impl = *m_impl;
    if (width <= 0 || height <= 0 || (width == impl.width && height == impl.height))
    {
        return;
    }
    impl.width = width;
    impl.height = height;
    if (impl.initialized)
    {
        bgfx::reset(static_cast<uint32_t>(width), static_cast<uint32_t>(height), impl.ResetFlags());
        PRED_LOG_DEBUG(Render, "Renderer resized to {}x{}", width, height);
    }
}

void Renderer::SetVSync(bool enabled)
{
    Impl& impl = *m_impl;
    if (impl.vsync == enabled)
    {
        return;
    }
    impl.vsync = enabled;
    if (impl.initialized)
    {
        bgfx::reset(static_cast<uint32_t>(impl.width), static_cast<uint32_t>(impl.height), impl.ResetFlags());
    }
}

void Renderer::SetClearColor(uint32_t rgba)
{
    m_impl->clearColor = rgba;
}

void Renderer::SetCamera(const glm::mat4& view, const glm::mat4& projection)
{
    m_impl->view = view;
    m_impl->projection = projection;
}

void Renderer::SetBgfxStatsOverlay(bool enabled)
{
    m_impl->debugFlags = enabled ? BGFX_DEBUG_STATS : BGFX_DEBUG_NONE;
    if (m_impl->initialized)
    {
        bgfx::setDebug(m_impl->debugFlags);
    }
}

void Renderer::BeginFrame()
{
    Impl& impl = *m_impl;
    const auto w = static_cast<uint16_t>(impl.width);
    const auto h = static_cast<uint16_t>(impl.height);

    bgfx::setViewRect(kViewMain, 0, 0, w, h);
    bgfx::setViewClear(kViewMain, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, impl.clearColor, 1.0f, 0);
    bgfx::setViewTransform(kViewMain, glm::value_ptr(impl.view), glm::value_ptr(impl.projection));
    bgfx::touch(kViewMain);

    bgfx::setViewRect(kViewDebug, 0, 0, w, h);
    bgfx::setViewTransform(kViewDebug, glm::value_ptr(impl.view), glm::value_ptr(impl.projection));
}

void Renderer::EndFrame()
{
    Impl& impl = *m_impl;
    if (!impl.pendingScreenshot.empty())
    {
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, impl.pendingScreenshot.c_str());
        impl.pendingScreenshot.clear();
    }
    impl.frameNumber = bgfx::frame();
}

void Renderer::RequestScreenshot(const std::filesystem::path& pngPath)
{
    std::error_code ec;
    std::filesystem::create_directories(pngPath.parent_path(), ec);
    m_impl->pendingScreenshot = pngPath.string();
}

int Renderer::Width() const
{
    return m_impl->width;
}

int Renderer::Height() const
{
    return m_impl->height;
}

bool Renderer::VSync() const
{
    return m_impl->vsync;
}

bool Renderer::HomogeneousDepth() const
{
    return m_impl->initialized ? bgfx::getCaps()->homogeneousDepth : false;
}

bool Renderer::OriginBottomLeft() const
{
    return m_impl->initialized ? bgfx::getCaps()->originBottomLeft : false;
}

const char* Renderer::BackendName() const
{
    return m_impl->initialized ? bgfx::getRendererName(bgfx::getRendererType()) : "none";
}

std::string Renderer::ShaderProfileDir() const
{
    if (!m_impl->initialized)
    {
        return "dx11";
    }
    switch (bgfx::getRendererType())
    {
    case bgfx::RendererType::Direct3D11:
    case bgfx::RendererType::Direct3D12:
        return "dx11";
    case bgfx::RendererType::Vulkan:
        return "spirv";
    case bgfx::RendererType::Metal:
        return "metal";
    case bgfx::RendererType::OpenGL:
    case bgfx::RendererType::OpenGLES:
        return "glsl";
    default:
        return "dx11";
    }
}

const bgfx::Stats* Renderer::Stats() const
{
    return m_impl->initialized ? bgfx::getStats() : nullptr;
}

double Renderer::GpuFrameMs() const
{
    const bgfx::Stats* stats = Stats();
    if (stats == nullptr || stats->gpuTimerFreq == 0)
    {
        return 0.0;
    }
    return static_cast<double>(stats->gpuTimeEnd - stats->gpuTimeBegin) * 1000.0 /
           static_cast<double>(stats->gpuTimerFreq);
}

double Renderer::BgfxCpuFrameMs() const
{
    const bgfx::Stats* stats = Stats();
    if (stats == nullptr || stats->cpuTimerFreq == 0)
    {
        return 0.0;
    }
    return static_cast<double>(stats->cpuTimeFrame) * 1000.0 / static_cast<double>(stats->cpuTimerFreq);
}

uint32_t Renderer::FrameNumber() const
{
    return m_impl->frameNumber;
}

} // namespace pred
