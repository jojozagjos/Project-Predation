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
