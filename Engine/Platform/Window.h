#pragma once

#include <string>

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
    void* NativeHandle() const; // HWND on Windows

    void GetSize(int& width, int& height) const;
    void GetSizeInPixels(int& width, int& height) const;

    void SetTitle(const std::string& title);
    void SetRelativeMouse(bool enabled);
    bool IsRelativeMouse() const;

private:
    SDL_Window* m_window = nullptr;
};

} // namespace pred
