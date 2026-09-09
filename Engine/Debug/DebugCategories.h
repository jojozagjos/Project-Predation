#pragma once

#include <cstdint>
#include <string_view>

namespace pred
{

// Toggleable debug visualization categories. Each is backed by a "debug.<name>"
// cvar so it can be set from config, the console, or the F3 overlay.
enum class DebugCategory : uint8_t
{
    AI,
    Navigation,
    Perception,
    Animation,
    Physics,
    Network,
    Rendering,
    Audio,
    Player,
    // Shots: where a round was traced from, where it went, and what it found. Separate from Player
    // because the answer to "is my aim lying to me" is a different question from "where is my
    // capsule", and they are wanted at different times.
    Combat,
    Count
};

class DebugCategories
{
public:
    static const char* Name(DebugCategory category);
    static bool Parse(std::string_view name, DebugCategory& out);
    static bool IsEnabled(DebugCategory category);
    static void SetEnabled(DebugCategory category, bool enabled);
    static void DrawImGui();
};

} // namespace pred
