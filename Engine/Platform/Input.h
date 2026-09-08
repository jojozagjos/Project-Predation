#pragma once

#include <SDL3/SDL_scancode.h>
#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

union SDL_Event;

namespace pred
{

enum class MouseButton : uint8_t
{
    Left,
    Right,
    Middle,
    X1,
    X2,
    Count
};

// Raw keyboard/mouse state plus named actions bound from Assets/Config/input.json.
//
// "Blocked" input: when the console or an ImGui widget captures the keyboard or
// mouse, action queries return false so the game does not react. Raw queries
// (IsKeyDownRaw etc.) ignore blocking and are used for global hotkeys.
class Input
{
public:
    struct Binding
    {
        enum class Kind : uint8_t
        {
            Key,
            Mouse
        };
        Kind kind = Kind::Key;
        int code = 0; // SDL_Scancode or MouseButton
    };

    void BeginFrame();
    void HandleEvent(const SDL_Event& event);
    void SetBlocked(bool keyboard, bool mouse);

    // Raw state (not affected by blocking).
    bool IsKeyDownRaw(SDL_Scancode key) const;
    bool WasKeyPressedRaw(SDL_Scancode key) const;
    bool WasKeyReleasedRaw(SDL_Scancode key) const;

    // Filtered state.
    bool IsKeyDown(SDL_Scancode key) const;
    bool WasKeyPressed(SDL_Scancode key) const;
    bool WasKeyReleased(SDL_Scancode key) const;
    bool IsMouseDown(MouseButton button) const;
    bool WasMousePressed(MouseButton button) const;
    bool WasMouseReleased(MouseButton button) const;
    glm::vec2 MouseDelta() const;
    glm::vec2 MousePosition() const;
    float WheelDelta() const;

    // Actions.
    bool LoadBindings(const std::filesystem::path& file);
    void ClearBindings();
    void BindAction(std::string_view action, const Binding& binding);
    bool IsActionDown(std::string_view action) const;
    bool WasActionPressed(std::string_view action) const;
    bool WasActionReleased(std::string_view action) const;
    bool ActionHasKey(std::string_view action, SDL_Scancode key) const;
    const std::unordered_map<std::string, std::vector<Binding>>& Bindings() const { return m_actions; }

    // Parses names such as "W", "F3", "Space", "Left Shift", "Grave", "Mouse1", "LMB".
    static bool ParseBinding(std::string_view name, Binding& out);
    static std::string BindingName(const Binding& binding);

private:
    std::array<bool, SDL_SCANCODE_COUNT> m_keyDown{};
    std::array<bool, SDL_SCANCODE_COUNT> m_keyPressed{};
    std::array<bool, SDL_SCANCODE_COUNT> m_keyReleased{};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_mouseDown{};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_mousePressed{};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_mouseReleased{};
    glm::vec2 m_mouseDelta{0.0f};
    glm::vec2 m_mousePosition{0.0f};
    float m_wheelDelta = 0.0f;
    bool m_keyboardBlocked = false;
    bool m_mouseBlocked = false;

    std::unordered_map<std::string, std::vector<Binding>> m_actions;

    bool BindingDown(const Binding& binding) const;
    bool BindingPressed(const Binding& binding) const;
    bool BindingReleased(const Binding& binding) const;
};

} // namespace pred
