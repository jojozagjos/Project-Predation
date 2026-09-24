#pragma once

#include <string>
#include <utility>
#include <vector>

struct SDL_Window;

namespace pred
{

struct WindowDesc
{
    std::string title = "Project Predation";
    int width = 1600;
    int height = 900;
    bool resizable = true;
    bool fullscreen = false;
};

class Window
{
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const WindowDesc& desc);
    void Destroy();

    SDL_Window* Handle() const { return m_window; }
    // What the renderer needs to draw into this window. A HWND on Windows, an NSWindow on macOS, a
    // Wayland surface or an X11 window elsewhere.
    void* NativeHandle() const;
    // And the connection that window was made on, which on X11 and Wayland is a separate thing the
    // renderer cannot do without. Null on Windows and macOS, where there is only one and the system
    // already knows where it is.
    void* NativeDisplay() const;

    void GetSize(int& width, int& height) const;
    void GetSizeInPixels(int& width, int& height) const;

    // Fullscreen at the desktop's own resolution, without a border: what "fullscreen" means for almost
    // every game now, and the only kind that switches without the screen going black for a second.
    void SetFullscreen(bool fullscreen);
    bool IsFullscreen() const;
    // The window's size while it is a window, kept on screen.
    void SetSize(int width, int height);
    // The sizes the main display offers, largest first, without repeats.
    static std::vector<std::pair<int, int>> DisplaySizes();

    void SetTitle(const std::string& title);
    void SetRelativeMouse(bool enabled);
    bool IsRelativeMouse() const;

private:
    SDL_Window* m_window = nullptr;
};

} // namespace pred
