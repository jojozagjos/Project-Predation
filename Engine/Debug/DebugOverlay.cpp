#include "Engine/Debug/DebugOverlay.h"

#include "Engine/Core/SystemInfo.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Debug/FrameStats.h"
#include "Engine/Render/Renderer.h"

#include <imgui.h>

namespace pred
{

void DebugOverlay::Draw(const Info& info)
{
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("##DebugOverlay", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    const FrameStats* stats = info.stats;
    const Renderer* renderer = info.renderer;

    ImGui::TextUnformatted("PROJECT PREDATION  " PRED_VERSION_STRING "  [F3 overlay, ` console]");
    ImGui::Separator();

    if (stats != nullptr)
    {
        ImGui::Text("FPS %6.1f   frame %6.2f ms (avg)  %6.2f ms (last)", stats->Fps(), stats->AverageFrameMs(),
                    stats->FrameMs());
        ImGui::Text("CPU main %6.2f ms", stats->CpuFrameMs());
        if (renderer != nullptr)
        {
            ImGui::SameLine();
            ImGui::Text("   GPU %6.2f ms   bgfx cpu %6.2f ms", renderer->GpuFrameMs(), renderer->BgfxCpuFrameMs());
        }
        ImGui::PlotLines("##FrameHistory", stats->History().data(), static_cast<int>(stats->History().size()),
                         stats->HistoryOffset(), nullptr, 0.0f, 33.3f, ImVec2(360.0f, 48.0f));

        if (!stats->Timings().empty())
        {
            for (const FrameStats::Timing& timing : stats->Timings())
            {
                ImGui::Text("  %-14s %6.2f ms", timing.name, timing.milliseconds);
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Fixed step %.0f Hz   steps this frame %d%s", info.fixedStepHz, info.fixedStepsLastFrame,
                info.droppedFixedTime ? "   (dropped time)" : "");

    if (renderer != nullptr)
    {
        const bgfx::Stats* bgfxStats = renderer->Stats();
        ImGui::Text("Renderer %s   %dx%d   vsync %s", renderer->BackendName(), renderer->Width(), renderer->Height(),
                    renderer->VSync() ? "on" : "off");
        if (bgfxStats != nullptr)
        {
            ImGui::Text("Draw calls %u   views %u   GPU mem %.1f MB", bgfxStats->numDraw, bgfxStats->numViews,
                        static_cast<double>(bgfxStats->gpuMemoryUsed) / (1024.0 * 1024.0));
        }
    }

    const ProcessMemoryInfo memory = SystemInfo::QueryProcessMemory();
    ImGui::Text("Memory working set %.1f MB   private %.1f MB",
                static_cast<double>(memory.workingSetBytes) / (1024.0 * 1024.0),
                static_cast<double>(memory.privateBytes) / (1024.0 * 1024.0));
    ImGui::Text("Entities %zu   meshes %zu   triangles %zu   debug lines %zu", info.entityCount,
                info.meshesDrawn, info.trianglesDrawn, info.debugLineCount);

    ImGui::Separator();
    ImGui::TextUnformatted("Debug categories");
    DebugCategories::DrawImGui();

    ImGui::End();
}

} // namespace pred
