#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>

union SDL_Event;
struct ImDrawData;
struct ImTextureData;

namespace pred
{

class Window;
class Renderer;
class ShaderLibrary;

// Dear ImGui integration: SDL3 platform backend (input, clipboard, cursors) from
// the imgui package plus our own bgfx renderer backend.
class ImGuiLayer
{
public:
    bool Init(Window& window, Renderer& renderer, ShaderLibrary& shaders);
    void Shutdown();

    void ProcessEvent(const SDL_Event& event);
    void BeginFrame();
    void EndFrame();

    bool WantCaptureKeyboard() const;
    bool WantCaptureMouse() const;
    bool WantTextInput() const;

    // How this backend packs a bgfx texture into an ImTextureID, so game UI can draw its own render
    // targets without knowing the encoding. Zero is ImGui's "no texture", hence the offset. Typed
    // as uint64_t so imgui.h stays out of the engine headers; it is ImTextureID's own type.
    static uint64_t TextureId(bgfx::TextureHandle handle);

private:
    void RenderDrawData(ImDrawData* drawData);
    void UpdateTexture(ImTextureData* texture);

    Renderer* m_renderer = nullptr;
    bgfx::VertexLayout m_layout;
    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_textureUniform = BGFX_INVALID_HANDLE;
    std::string m_iniPath;
    bool m_initialized = false;
    bool m_warnedOverflow = false;
};

} // namespace pred
