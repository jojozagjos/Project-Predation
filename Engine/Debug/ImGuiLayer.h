#pragma once

#include <bgfx/bgfx.h>

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
