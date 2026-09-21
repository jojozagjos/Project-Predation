#include "Engine/Debug/ImGuiLayer.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Platform/Window.h"
#include "Engine/Render/Renderer.h"
#include "Engine/Render/ShaderLibrary.h"

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace pred
{
namespace
{

static_assert(sizeof(ImDrawIdx) == 2, "ImGui bgfx backend expects 16-bit indices");
static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert layout changed; update the bgfx vertex layout");

ImTextureID ToTextureId(bgfx::TextureHandle handle)
{
    return static_cast<ImTextureID>(ImGuiLayer::TextureId(handle));
}

bgfx::TextureHandle FromTextureId(ImTextureID id)
{
    bgfx::TextureHandle handle;
    handle.idx = id == ImTextureID_Invalid ? bgfx::kInvalidHandle : static_cast<uint16_t>(id - 1);
    return handle;
}

} // namespace

bool ImGuiLayer::Init(Window& window, Renderer& renderer, ShaderLibrary& shaders)
{
    m_renderer = &renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Keyboard navigation is deliberately off. With it on, ImGui reports that it wants the
    // keyboard whenever any window has focus, and the application honours that by blocking game
    // input; opening the inventory then froze the player where they stood. Every panel here is
    // driven with the mouse, and the console still receives keys because its text field is an
    // active item, which claims the keyboard on its own.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    io.BackendRendererName = "predation_bgfx";

    m_iniPath = (Paths::UserDataDir() / "imgui.ini").string();
    io.IniFilename = m_iniPath.c_str();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;

    if (!ImGui_ImplSDL3_InitForOther(window.Handle()))
    {
        PRED_LOG_ERROR(Debug, "ImGui_ImplSDL3_InitForOther failed");
        return false;
    }
    m_window = window.Handle();

    m_layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    m_program = shaders.LoadProgram("vs_imgui", "fs_imgui");
    if (!bgfx::isValid(m_program))
    {
        PRED_LOG_ERROR(Debug, "ImGui: failed to load shader program");
        return false;
    }
    m_textureUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

    m_initialized = true;
    PRED_LOG_INFO(Debug, "ImGui {} initialized", IMGUI_VERSION);
    return true;
}

void ImGuiLayer::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }
    // Release textures ImGui still references (font atlas). Mirrors the official backends:
    // only textures used solely by this context are destroyed here.
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
    {
        if (texture->RefCount == 1)
        {
            const bgfx::TextureHandle handle = FromTextureId(texture->GetTexID());
            if (bgfx::isValid(handle))
            {
                bgfx::destroy(handle);
            }
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    if (bgfx::isValid(m_textureUniform))
    {
        bgfx::destroy(m_textureUniform);
        m_textureUniform = BGFX_INVALID_HANDLE;
    }
    m_program = BGFX_INVALID_HANDLE;
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

void ImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    if (!m_initialized)
    {
        return;
    }
    if (m_mouseIgnored)
    {
        switch (event.type)
        {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            return;
        default:
            break;
        }
    }
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::SetMouseIgnored(bool ignored)
{
    if (!m_initialized || ignored == m_mouseIgnored)
    {
        return;
    }
    if (ignored && m_window != nullptr)
    {
        // Let go of every button first, through the backend so its own record of what is held
        // agrees. The click that captured the mouse went down while the UI could still see it, and
        // its release will now never arrive: without this the UI would hold that button, and the
        // keyboard with it, until the mouse was next freed.
        for (const int button : {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2})
        {
            SDL_Event release{};
            release.type = SDL_EVENT_MOUSE_BUTTON_UP;
            release.button.windowID = SDL_GetWindowID(m_window);
            release.button.button = static_cast<Uint8>(button);
            release.button.down = false;
            ImGui_ImplSDL3_ProcessEvent(&release);
        }
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    m_mouseIgnored = ignored;
}

void ImGuiLayer::BeginFrame()
{
    ImGui_ImplSDL3_NewFrame();
    // The backend reads the operating system's pointer position each frame on its own, which would
    // put the invisible pointer back over whatever window it is under. Last word goes to this.
    if (m_mouseIgnored)
    {
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    ImGui::NewFrame();
}

void ImGuiLayer::EndFrame()
{
    ImGui::Render();
    RenderDrawData(ImGui::GetDrawData());
}

uint64_t ImGuiLayer::TextureId(bgfx::TextureHandle handle)
{
    return static_cast<uint64_t>(handle.idx) + 1;
}

bool ImGuiLayer::WantCaptureKeyboard() const
{
    return m_initialized && ImGui::GetIO().WantCaptureKeyboard;
}

bool ImGuiLayer::WantCaptureMouse() const
{
    return m_initialized && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::WantTextInput() const
{
    return m_initialized && ImGui::GetIO().WantTextInput;
}

void ImGuiLayer::UpdateTexture(ImTextureData* texture)
{
    if (texture->Status == ImTextureStatus_WantCreate)
    {
        IM_ASSERT(texture->Format == ImTextureFormat_RGBA32);

        // Create the texture WITHOUT initial data, then fill it with an update.
        //
        // Passing pixels to createTexture2D makes bgfx allocate an immutable resource, and every
        // later updateTexture2D against it is silently discarded. ImGui 1.92 rasterizes glyphs on
        // demand and streams them into the atlas as they are first used, so an immutable atlas
        // freezes at whatever was baked on the first frame: most text then renders as blanks with
        // only a few scattered letters visible.
        const bgfx::TextureHandle handle =
            bgfx::createTexture2D(static_cast<uint16_t>(texture->Width), static_cast<uint16_t>(texture->Height),
                                  false, 1, bgfx::TextureFormat::RGBA8,
                                  BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, nullptr);
        if (!bgfx::isValid(handle))
        {
            PRED_LOG_ERROR(Debug, "ImGui: could not create a {}x{} atlas texture", texture->Width,
                           texture->Height);
            return;
        }
        bgfx::setName(handle, "ImGuiTexture");

        const uint32_t size = static_cast<uint32_t>(texture->Width * texture->Height * texture->BytesPerPixel);
        const bgfx::Memory* memory = bgfx::copy(texture->GetPixels(), size);
        bgfx::updateTexture2D(handle, 0, 0, 0, 0, static_cast<uint16_t>(texture->Width),
                              static_cast<uint16_t>(texture->Height), memory,
                              static_cast<uint16_t>(texture->GetPitch()));

        texture->SetTexID(ToTextureId(handle));
        texture->SetStatus(ImTextureStatus_OK);
    }
    else if (texture->Status == ImTextureStatus_WantUpdates)
    {
        const bgfx::TextureHandle handle = FromTextureId(texture->GetTexID());
        for (const ImTextureRect& rect : texture->Updates)
        {
            const uint32_t pitch = static_cast<uint32_t>(rect.w) * texture->BytesPerPixel;
            const bgfx::Memory* memory = bgfx::alloc(pitch * rect.h);
            for (int y = 0; y < rect.h; ++y)
            {
                std::memcpy(memory->data + static_cast<size_t>(y) * pitch, texture->GetPixelsAt(rect.x, rect.y + y),
                            pitch);
            }
            bgfx::updateTexture2D(handle, 0, 0, rect.x, rect.y, rect.w, rect.h, memory, static_cast<uint16_t>(pitch));
        }
        texture->SetStatus(ImTextureStatus_OK);
    }
    else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0)
    {
        const bgfx::TextureHandle handle = FromTextureId(texture->GetTexID());
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
    }
}

void ImGuiLayer::RenderDrawData(ImDrawData* drawData)
{
    if (drawData == nullptr)
    {
        return;
    }
    if (drawData->Textures != nullptr)
    {
        for (ImTextureData* texture : *drawData->Textures)
        {
            if (texture->Status != ImTextureStatus_OK)
            {
                UpdateTexture(texture);
            }
        }
    }

    const int fbWidth = static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int fbHeight = static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (fbWidth <= 0 || fbHeight <= 0 || drawData->CmdListsCount == 0)
    {
        return;
    }

    const bgfx::ViewId view = Renderer::kViewUI;
    bgfx::setViewName(view, "ImGui");
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(fbWidth), static_cast<uint16_t>(fbHeight));

    const float left = drawData->DisplayPos.x;
    const float right = left + drawData->DisplaySize.x;
    const float top = drawData->DisplayPos.y;
    const float bottom = top + drawData->DisplaySize.y;
    const glm::mat4 ortho = m_renderer->HomogeneousDepth() ? glm::orthoRH_NO(left, right, bottom, top, -1.0f, 1.0f)
                                                           : glm::orthoRH_ZO(left, right, bottom, top, 0.0f, 1.0f);
    bgfx::setViewTransform(view, nullptr, glm::value_ptr(ortho));

    const ImVec2 clipOffset = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    constexpr uint64_t kState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
                                BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);

    for (int n = 0; n < drawData->CmdListsCount; ++n)
    {
        const ImDrawList* cmdList = drawData->CmdLists[n];
        const auto numVertices = static_cast<uint32_t>(cmdList->VtxBuffer.Size);
        const auto numIndices = static_cast<uint32_t>(cmdList->IdxBuffer.Size);
        if (numVertices == 0 || numIndices == 0)
        {
            continue;
        }
        if (bgfx::getAvailTransientVertexBuffer(numVertices, m_layout) < numVertices ||
            bgfx::getAvailTransientIndexBuffer(numIndices, false) < numIndices)
        {
            if (!m_warnedOverflow)
            {
                PRED_LOG_WARN(Debug, "ImGui: transient buffers exhausted, UI partially drawn");
                m_warnedOverflow = true;
            }
            break;
        }

        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, numVertices, m_layout);
        bgfx::allocTransientIndexBuffer(&tib, numIndices, false);
        std::memcpy(tvb.data, cmdList->VtxBuffer.Data, numVertices * sizeof(ImDrawVert));
        std::memcpy(tib.data, cmdList->IdxBuffer.Data, numIndices * sizeof(ImDrawIdx));

        for (const ImDrawCmd& cmd : cmdList->CmdBuffer)
        {
            if (cmd.UserCallback != nullptr)
            {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                {
                    cmd.UserCallback(cmdList, &cmd);
                }
                continue;
            }
            if (cmd.ElemCount == 0)
            {
                continue;
            }

            const float clipMinX = std::max((cmd.ClipRect.x - clipOffset.x) * clipScale.x, 0.0f);
            const float clipMinY = std::max((cmd.ClipRect.y - clipOffset.y) * clipScale.y, 0.0f);
            const float clipMaxX = std::min((cmd.ClipRect.z - clipOffset.x) * clipScale.x, static_cast<float>(fbWidth));
            const float clipMaxY =
                std::min((cmd.ClipRect.w - clipOffset.y) * clipScale.y, static_cast<float>(fbHeight));
            if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
            {
                continue;
            }

            bgfx::setScissor(static_cast<uint16_t>(clipMinX), static_cast<uint16_t>(clipMinY),
                             static_cast<uint16_t>(clipMaxX - clipMinX), static_cast<uint16_t>(clipMaxY - clipMinY));
            bgfx::setState(kState);
            bgfx::setTexture(0, m_textureUniform, FromTextureId(cmd.GetTexID()));
            bgfx::setVertexBuffer(0, &tvb, cmd.VtxOffset, numVertices - cmd.VtxOffset);
            bgfx::setIndexBuffer(&tib, cmd.IdxOffset, cmd.ElemCount);
            bgfx::submit(view, m_program);
        }
    }
}

} // namespace pred
