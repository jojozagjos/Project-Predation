#include "Engine/Platform/Window.h"

#include "Engine/Core/Log.h"

#include <SDL3/SDL.h>

namespace pred
{

Window::~Window()
{
    Destroy();
}

bool Window::Create(const WindowDesc& desc)
{
    Destroy();

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable)
    {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (desc.fullscreen)
    {
        flags |= SDL_WINDOW_FULLSCREEN;
    }

    m_window = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
    if (m_window == nullptr)
    {
        PRED_LOG_ERROR(Platform, "SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    int pixelWidth = 0;
    int pixelHeight = 0;
    GetSizeInPixels(pixelWidth, pixelHeight);
    PRED_LOG_INFO(Platform, "Window created: {}x{} logical, {}x{} pixels", desc.width, desc.height, pixelWidth,
                  pixelHeight);
    return true;
}

void Window::Destroy()
{
    if (m_window != nullptr)
    {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

void* Window::NativeHandle() const
{
    if (m_window == nullptr)
    {
        return nullptr;
    }
#ifdef _WIN32
    return SDL_GetPointerProperty(SDL_GetWindowProperties(m_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    return nullptr;
#endif
}

void Window::GetSize(int& width, int& height) const
{
    width = 0;
    height = 0;
    if (m_window != nullptr)
    {
        SDL_GetWindowSize(m_window, &width, &height);
    }
}

void Window::GetSizeInPixels(int& width, int& height) const
{
    width = 0;
    height = 0;
    if (m_window != nullptr)
    {
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
    }
}

void Window::SetTitle(const std::string& title)
{
    if (m_window != nullptr)
    {
        SDL_SetWindowTitle(m_window, title.c_str());
    }
}

void Window::SetRelativeMouse(bool enabled)
{
    if (m_window != nullptr)
    {
        if (!SDL_SetWindowRelativeMouseMode(m_window, enabled))
        {
            PRED_LOG_WARN(Platform, "SDL_SetWindowRelativeMouseMode failed: {}", SDL_GetError());
        }
    }
}

bool Window::IsRelativeMouse() const
{
    return m_window != nullptr && SDL_GetWindowRelativeMouseMode(m_window);
}

} // namespace pred
