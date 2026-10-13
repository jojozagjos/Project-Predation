#pragma once

#include <imgui.h>

namespace pred
{

// A layer the size of the screen to draw on -- a cinematic's bars and fade, a title, names over heads -- that takes no
// input and is drawn in its place among the windows. Not ImGui's foreground list, which is over everything: the developer's
// windows are kept over this (PredationGame::KeepDebugOnTop), and nothing can be kept over the foreground. Open while it
// is in scope.
class OverlayLayer
{
public:
    explicit OverlayLayer(const char* name)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
        m_draw = ImGui::GetWindowDrawList();
    }
    ~OverlayLayer()
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    OverlayLayer(const OverlayLayer&) = delete;
    OverlayLayer& operator=(const OverlayLayer&) = delete;

    ImDrawList* Draw() const { return m_draw; }

private:
    ImDrawList* m_draw = nullptr;
};

} // namespace pred
