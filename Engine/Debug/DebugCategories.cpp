#include "Engine/Debug/DebugCategories.h"

#include "Engine/Core/CVar.h"

#include <imgui.h>

#include <array>
#include <cctype>

namespace pred
{
namespace
{

constexpr std::array<const char*, static_cast<size_t>(DebugCategory::Count)> kNames = {
    "AI", "NAVIGATION", "PERCEPTION", "ANIMATION", "PHYSICS", "NETWORK", "RENDERING", "AUDIO", "PLAYER"};

CVar<bool> cv_ai{"debug.ai", false, "Show AI debug visualization"};
CVar<bool> cv_navigation{"debug.navigation", false, "Show navigation debug visualization"};
CVar<bool> cv_perception{"debug.perception", false, "Show perception debug visualization"};
CVar<bool> cv_animation{"debug.animation", false, "Show animation debug visualization"};
CVar<bool> cv_physics{"debug.physics", false, "Show physics debug visualization"};
CVar<bool> cv_network{"debug.network", false, "Show network debug information"};
CVar<bool> cv_rendering{"debug.rendering", false, "Show rendering debug information"};
CVar<bool> cv_audio{"debug.audio", false, "Show audio debug information"};
CVar<bool> cv_player{"debug.player", false, "Show player debug visualization"};

std::array<CVar<bool>*, static_cast<size_t>(DebugCategory::Count)>& Vars()
{
    static std::array<CVar<bool>*, static_cast<size_t>(DebugCategory::Count)> vars = {
        &cv_ai,      &cv_navigation, &cv_perception, &cv_animation, &cv_physics,
        &cv_network, &cv_rendering,  &cv_audio,      &cv_player};
    return vars;
}

} // namespace

const char* DebugCategories::Name(DebugCategory category)
{
    const auto index = static_cast<size_t>(category);
    return index < kNames.size() ? kNames[index] : "UNKNOWN";
}

bool DebugCategories::Parse(std::string_view name, DebugCategory& out)
{
    for (size_t i = 0; i < kNames.size(); ++i)
    {
        const std::string_view candidate = kNames[i];
        if (candidate.size() != name.size())
        {
            continue;
        }
        bool equal = true;
        for (size_t c = 0; c < name.size(); ++c)
        {
            if (std::toupper(static_cast<unsigned char>(name[c])) != candidate[c])
            {
                equal = false;
                break;
            }
        }
        if (equal)
        {
            out = static_cast<DebugCategory>(i);
            return true;
        }
    }
    return false;
}

bool DebugCategories::IsEnabled(DebugCategory category)
{
    const auto index = static_cast<size_t>(category);
    return index < Vars().size() && Vars()[index]->Get();
}

void DebugCategories::SetEnabled(DebugCategory category, bool enabled)
{
    const auto index = static_cast<size_t>(category);
    if (index < Vars().size())
    {
        Vars()[index]->Set(enabled);
    }
}

void DebugCategories::DrawImGui()
{
    for (size_t i = 0; i < kNames.size(); ++i)
    {
        bool value = Vars()[i]->Get();
        if (ImGui::Checkbox(kNames[i], &value))
        {
            Vars()[i]->Set(value);
        }
        if ((i % 3) != 2 && i + 1 < kNames.size())
        {
            ImGui::SameLine(0.0f, 12.0f);
        }
    }
}

} // namespace pred
