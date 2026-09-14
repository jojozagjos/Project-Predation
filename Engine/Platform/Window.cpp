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
    SDL_PropertiesID properties = SDL_GetWindowProperties(m_window);
#if defined(_WIN32)
    return SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
    return SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#else
    // Wayland first, because a session that has both is a Wayland session with X11 available for
    // programs that cannot do better. The window under Wayland is a surface; under X11 it is a
    // number that has to be carried as though it were a pointer, which is what every renderer on
    // this platform expects and what bgfx documents.
    if (void* surface =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        surface != nullptr)
    {
        return surface;
    }
    const Uint64 x11 = SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    return x11 != 0 ? reinterpret_cast<void*>(static_cast<uintptr_t>(x11)) : nullptr;
#endif
}

void* Window::NativeDisplay() const
{
    if (m_window == nullptr)
    {
        return nullptr;
    }
#if defined(_WIN32) || defined(__APPLE__)
    // Nothing to name. There is one display connection and the system knows where it is.
    return nullptr;
#else
    // And on X11 and Wayland there is not: a window means nothing without the connection it was
    // made on, and a renderer handed only the window has no way to talk to it. Leaving this null is
    // why a first port comes up to a black screen and no error.
    SDL_PropertiesID properties = SDL_GetWindowProperties(m_window);
    if (void* display =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        display != nullptr)
    {
        return display;
    }
    return SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
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
