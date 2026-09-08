#include "Engine/Platform/Input.h"

#include "Engine/Core/Log.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace pred
{
namespace
{

std::string ToLower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

const std::unordered_map<std::string, SDL_Scancode>& KeyAliases()
{
    static const std::unordered_map<std::string, SDL_Scancode> aliases = {
        {"grave", SDL_SCANCODE_GRAVE},   {"tilde", SDL_SCANCODE_GRAVE},     {"`", SDL_SCANCODE_GRAVE},
        {"esc", SDL_SCANCODE_ESCAPE},    {"enter", SDL_SCANCODE_RETURN},    {"lshift", SDL_SCANCODE_LSHIFT},
        {"rshift", SDL_SCANCODE_RSHIFT}, {"shift", SDL_SCANCODE_LSHIFT},    {"lctrl", SDL_SCANCODE_LCTRL},
        {"rctrl", SDL_SCANCODE_RCTRL},   {"ctrl", SDL_SCANCODE_LCTRL},      {"lalt", SDL_SCANCODE_LALT},
        {"ralt", SDL_SCANCODE_RALT},     {"alt", SDL_SCANCODE_LALT},        {"space", SDL_SCANCODE_SPACE},
        {"tab", SDL_SCANCODE_TAB},       {"backspace", SDL_SCANCODE_BACKSPACE},
    };
    return aliases;
}

const std::unordered_map<std::string, MouseButton>& MouseAliases()
{
    static const std::unordered_map<std::string, MouseButton> aliases = {
        {"mouse1", MouseButton::Left},  {"mouse2", MouseButton::Right}, {"mouse3", MouseButton::Middle},
        {"mouse4", MouseButton::X1},    {"mouse5", MouseButton::X2},    {"lmb", MouseButton::Left},
        {"rmb", MouseButton::Right},    {"mmb", MouseButton::Middle},   {"leftmouse", MouseButton::Left},
        {"rightmouse", MouseButton::Right},
    };
    return aliases;
}

MouseButton FromSdlButton(uint8_t button)
{
    switch (button)
    {
    case SDL_BUTTON_LEFT:
        return MouseButton::Left;
    case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
    case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
    case SDL_BUTTON_X1:
        return MouseButton::X1;
    case SDL_BUTTON_X2:
        return MouseButton::X2;
    default:
        return MouseButton::Count;
    }
}

} // namespace

void Input::BeginFrame()
{
    m_keyPressed.fill(false);
    m_keyReleased.fill(false);
    m_mousePressed.fill(false);
    m_mouseReleased.fill(false);
    m_mouseDelta = glm::vec2(0.0f);
    m_wheelDelta = 0.0f;
}

void Input::HandleEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    {
        const SDL_Scancode code = event.key.scancode;
        if (code > SDL_SCANCODE_UNKNOWN && code < SDL_SCANCODE_COUNT && !event.key.repeat)
        {
            m_keyDown[code] = true;
            m_keyPressed[code] = true;
        }
        break;
    }
    case SDL_EVENT_KEY_UP:
    {
        const SDL_Scancode code = event.key.scancode;
        if (code > SDL_SCANCODE_UNKNOWN && code < SDL_SCANCODE_COUNT)
        {
            m_keyDown[code] = false;
            m_keyReleased[code] = true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        m_mouseDelta += glm::vec2(event.motion.xrel, event.motion.yrel);
        m_mousePosition = glm::vec2(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        const MouseButton button = FromSdlButton(event.button.button);
        if (button != MouseButton::Count)
        {
            m_mouseDown[static_cast<size_t>(button)] = true;
            m_mousePressed[static_cast<size_t>(button)] = true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const MouseButton button = FromSdlButton(event.button.button);
        if (button != MouseButton::Count)
        {
            m_mouseDown[static_cast<size_t>(button)] = false;
            m_mouseReleased[static_cast<size_t>(button)] = true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        m_wheelDelta += event.wheel.y;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Keys released while unfocused never arrive; drop everything.
        m_keyDown.fill(false);
        m_mouseDown.fill(false);
        break;
    default:
        break;
    }
}

void Input::SetBlocked(bool keyboard, bool mouse)
{
    m_keyboardBlocked = keyboard;
    m_mouseBlocked = mouse;
}

bool Input::IsKeyDownRaw(SDL_Scancode key) const
{
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT && m_keyDown[key];
}

bool Input::WasKeyPressedRaw(SDL_Scancode key) const
{
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT && m_keyPressed[key];
}

bool Input::WasKeyReleasedRaw(SDL_Scancode key) const
{
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT && m_keyReleased[key];
}

bool Input::IsKeyDown(SDL_Scancode key) const
{
    return !m_keyboardBlocked && IsKeyDownRaw(key);
}

bool Input::WasKeyPressed(SDL_Scancode key) const
{
    return !m_keyboardBlocked && WasKeyPressedRaw(key);
}

bool Input::WasKeyReleased(SDL_Scancode key) const
{
    return !m_keyboardBlocked && WasKeyReleasedRaw(key);
}

bool Input::IsMouseDown(MouseButton button) const
{
    return !m_mouseBlocked && button != MouseButton::Count && m_mouseDown[static_cast<size_t>(button)];
}

bool Input::WasMousePressed(MouseButton button) const
{
    return !m_mouseBlocked && button != MouseButton::Count && m_mousePressed[static_cast<size_t>(button)];
}

bool Input::WasMouseReleased(MouseButton button) const
{
    return !m_mouseBlocked && button != MouseButton::Count && m_mouseReleased[static_cast<size_t>(button)];
}

glm::vec2 Input::MouseDelta() const
{
    return m_mouseBlocked ? glm::vec2(0.0f) : m_mouseDelta;
}

glm::vec2 Input::MousePosition() const
{
    return m_mousePosition;
}

float Input::WheelDelta() const
{
    return m_mouseBlocked ? 0.0f : m_wheelDelta;
}

bool Input::LoadBindings(const std::filesystem::path& file)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_ERROR(Platform, "Input bindings file not found: {}", file.string());
        return false;
    }
    const nlohmann::json json = nlohmann::json::parse(stream, nullptr, false, true);
    if (json.is_discarded() || !json.is_object() || !json.contains("actions") || !json["actions"].is_object())
    {
        PRED_LOG_ERROR(Platform, "Input bindings file is invalid (expected {{\"actions\": {{...}}}}): {}",
                       file.string());
        return false;
    }

    ClearBindings();
    int bound = 0;
    int failed = 0;
    for (const auto& [action, keys] : json["actions"].items())
    {
        if (!keys.is_array())
        {
            PRED_LOG_WARN(Platform, "Input action '{}' must be an array of key names", action);
            ++failed;
            continue;
        }
        for (const auto& keyName : keys)
        {
            Binding binding;
            if (keyName.is_string() && ParseBinding(keyName.get<std::string>(), binding))
            {
                BindAction(action, binding);
                ++bound;
            }
            else
            {
                PRED_LOG_WARN(Platform, "Input action '{}': unknown key name '{}'", action, keyName.dump());
                ++failed;
            }
        }
    }
    PRED_LOG_INFO(Platform, "Loaded input bindings from {} ({} bindings, {} problems)", file.string(), bound,
                  failed);
    return failed == 0;
}

void Input::ClearBindings()
{
    m_actions.clear();
}

void Input::BindAction(std::string_view action, const Binding& binding)
{
    m_actions[std::string(action)].push_back(binding);
}

bool Input::IsActionDown(std::string_view action) const
{
    const auto it = m_actions.find(std::string(action));
    if (it == m_actions.end())
    {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [this](const Binding& b) { return BindingDown(b); });
}

bool Input::WasActionPressed(std::string_view action) const
{
    const auto it = m_actions.find(std::string(action));
    if (it == m_actions.end())
    {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [this](const Binding& b) { return BindingPressed(b); });
}

bool Input::WasActionReleased(std::string_view action) const
{
    const auto it = m_actions.find(std::string(action));
    if (it == m_actions.end())
    {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(),
                       [this](const Binding& b) { return BindingReleased(b); });
}

bool Input::ActionHasKey(std::string_view action, SDL_Scancode key) const
{
    const auto it = m_actions.find(std::string(action));
    if (it == m_actions.end())
    {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(),
                       [key](const Binding& b) { return b.kind == Binding::Kind::Key && b.code == key; });
}

bool Input::ParseBinding(std::string_view name, Binding& out)
{
    const std::string lower = ToLower(name);
    if (lower.empty())
    {
        return false;
    }

    if (const auto mouse = MouseAliases().find(lower); mouse != MouseAliases().end())
    {
        out.kind = Binding::Kind::Mouse;
        out.code = static_cast<int>(mouse->second);
        return true;
    }
    if (const auto alias = KeyAliases().find(lower); alias != KeyAliases().end())
    {
        out.kind = Binding::Kind::Key;
        out.code = alias->second;
        return true;
    }

    const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
    if (code == SDL_SCANCODE_UNKNOWN)
    {
        return false;
    }
    out.kind = Binding::Kind::Key;
    out.code = code;
    return true;
}

std::string Input::BindingName(const Binding& binding)
{
    if (binding.kind == Binding::Kind::Mouse)
    {
        return "Mouse" + std::to_string(binding.code + 1);
    }
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.code));
    return (name != nullptr && *name != '\0') ? name : "Unknown";
}

bool Input::BindingDown(const Binding& binding) const
{
    return binding.kind == Binding::Kind::Key ? IsKeyDown(static_cast<SDL_Scancode>(binding.code))
                                              : IsMouseDown(static_cast<MouseButton>(binding.code));
}

bool Input::BindingPressed(const Binding& binding) const
{
    return binding.kind == Binding::Kind::Key ? WasKeyPressed(static_cast<SDL_Scancode>(binding.code))
                                              : WasMousePressed(static_cast<MouseButton>(binding.code));
}

bool Input::BindingReleased(const Binding& binding) const
{
    return binding.kind == Binding::Kind::Key ? WasKeyReleased(static_cast<SDL_Scancode>(binding.code))
                                              : WasMouseReleased(static_cast<MouseButton>(binding.code));
}

} // namespace pred
