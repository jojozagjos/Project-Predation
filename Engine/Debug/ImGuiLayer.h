#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Window;
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

    // While the game has the mouse captured, the UI gets none of it: no pointer, no buttons, no
    // wheel. The pointer is hidden then, but the UI went on tracking an invisible one that drifted
    // as the player turned, and a shot fired while it happened to rest over a window was a press
    // on that window -- which hands the UI the keyboard for as long as the button is held, so every
    // held key, a lean among them, let go mid-burst.
    void SetMouseIgnored(bool ignored);

    bool WantCaptureKeyboard() const;
    bool WantCaptureMouse() const;
    bool WantTextInput() const;

    // A pointer and keys driven by console commands (ui_move, ui_down, ui_key...) rather than by anybody at the
    // machine, for testing the panels from a script. From the first of them on, the real mouse is ignored: the UI
    // sees only the scripted pointer.
    void ScriptPointer(float x, float y);
    void ScriptButton(int button, bool down);
    void ScriptWheel(float x, float y);
    // A key pressed and let go, by its name in the UI ("Space", "K", "LeftArrow"), with Ctrl, Shift or Alt held
    // over it when asked. False when there is no such key.
    bool ScriptKey(const std::string& name, bool ctrl, bool shift, bool alt);
    // Text typed into whatever field has the keyboard.
    void ScriptText(const std::string& text);

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
    SDL_Window* m_window = nullptr;
    bool m_initialized = false;
    bool m_mouseIgnored = false;
    bool m_warnedOverflow = false;
    // The scripted pointer, once scripting has begun, and what it has still to press or let go of: each happens a
    // frame after the one before, so the UI sees a press and a release as two things.
    bool m_scripted = false;
    float m_scriptX = 0.0f;
    float m_scriptY = 0.0f;
    struct ScriptedInput
    {
        int kind = 0; // 0 a button, 1 a key, 2 the wheel, 3 text
        int code = 0;
        bool down = false;
        float x = 0.0f;
        float y = 0.0f;
        std::string text;
    };
    std::vector<ScriptedInput> m_scriptQueue;
};

} // namespace pred
