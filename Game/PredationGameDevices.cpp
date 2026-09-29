// The things carried for finding the way, used by holding them: the map, which sweeps the walls near you like a
// motion tracker sweeps for movement -- only what is close, only walls, never a creature or the objective -- and the
// objective tracker, which points at what is to be done next and beeps faster the closer it is.

#include "Game/PredationGame.h"

#include <imgui.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pred
{

namespace
{

constexpr int kScanSamples = 240;
constexpr float kScanSweepSeconds = 1.6f; // once round
constexpr float kScanFade = 1.5f;         // how long a wall stays lit after the sweep has passed it
// How far it reaches: further on a site that came with map data.
constexpr float kScanReachMapped = 20.0f;
constexpr float kScanReachUnmapped = 12.0f;

// A device's round screen, down on the right: its middle and radius.
void DeviceScreen(ImVec2& centre, float& radius)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    radius = std::min(viewport->Size.y * 0.13f, 120.0f);
    centre = {viewport->Pos.x + viewport->Size.x - radius - 36.0f, viewport->Pos.y + viewport->Size.y - radius - 110.0f};
}

// A world direction on the screen, the way you face being up.
ImVec2 OnScreen(const ImVec2& centre, float radius, float worldAngle, float facing, float fraction)
{
    const float relative = worldAngle - facing;
    return {centre.x + std::sin(relative) * radius * fraction, centre.y - std::cos(relative) * radius * fraction};
}

} // namespace

const std::string& PredationGame::HeldDevice() const
{
    static const std::string none;
    const ItemDefinition* held = m_items.Get(m_heldItem);
    return held != nullptr && m_player.State().alive ? held->device : none;
}

bool PredationGame::ObjectiveTarget(glm::vec3& at) const
{
    const glm::vec3 here = m_player.State().position;
    // Aboard: the shuttle when it is waiting to go, the console where the next site is chosen otherwise.
    if (m_ship.Contains(here))
    {
        at = m_shipReady ? m_ship.Shuttle().Home().position : m_ship.BriefingConsole().position;
        return true;
    }
    if (!AtSite() || !m_missionPlan.Valid())
    {
        return false;
    }
    switch (m_mission.stage)
    {
    case MissionState::Stage::Find:
    {
        // Found dead, the terminal waits on its building's breaker.
        glm::vec3 panel;
        float yaw = 0.0f;
        if (!m_mission.powered && m_missionFoundNoPower &&
            BreakerPanel(m_facility.Plan().buildings[static_cast<size_t>(m_missionPlan.building)], panel, yaw))
        {
            at = panel;
            return true;
        }
        at = m_missionPlan.terminal;
        return true;
    }
    case MissionState::Stage::Downloading:
        at = m_missionPlan.terminal;
        return true;
    case MissionState::Stage::Carry:
        at = m_missionProps.Crawler().Home().position;
        return true;
    case MissionState::Stage::None:
    case MissionState::Stage::Over:
        break;
    }
    return false;
}

void PredationGame::UpdateDevices(float dt)
{
    const std::string& device = HeldDevice();
    if (m_screen != Screen::Playing || CinematicHoldsPlayers())
    {
        return;
    }
    m_deviceClock += dt;
    if (device == "map")
    {
        if (m_scanDistance.size() != kScanSamples)
        {
            m_scanDistance.assign(kScanSamples, -1.0f);
            m_scanSeen.assign(kScanSamples, -100.0f);
        }
        // Round it goes, a few rays each frame along the edge of the sweep, at chest height: walls on this floor only.
        const float reach = m_missionPlan.Valid() && AtSite() && !m_missionPlan.mapGiven ? kScanReachUnmapped : kScanReachMapped;
        const glm::vec3 eye = m_player.State().position + glm::vec3(0.0f, 1.2f, 0.0f);
        const float from = m_scanAngle;
        m_scanAngle += glm::two_pi<float>() * dt / kScanSweepSeconds;
        const int first = static_cast<int>(std::floor(from / glm::two_pi<float>() * kScanSamples));
        const int last = static_cast<int>(std::floor(m_scanAngle / glm::two_pi<float>() * kScanSamples));
        for (int step = first + 1; step <= last; ++step)
        {
            const int i = ((step % kScanSamples) + kScanSamples) % kScanSamples;
            const float angle = static_cast<float>(i) / kScanSamples * glm::two_pi<float>();
            const RayHit hit = m_app->GetPhysics().RayCastStatic(eye, {std::sin(angle), 0.0f, -std::cos(angle)}, reach);
            m_scanDistance[static_cast<size_t>(i)] = hit.hit ? hit.distance / reach : -1.0f;
            m_scanSeen[static_cast<size_t>(i)] = m_deviceClock;
        }
        m_scanAngle = std::fmod(m_scanAngle, glm::two_pi<float>());
        m_scanReach = reach;
    }
    else if (device == "tracker")
    {
        glm::vec3 target;
        if (!ObjectiveTarget(target))
        {
            return;
        }
        // Faster and higher the closer it is.
        const float distance = glm::length(glm::vec2(target.x - m_player.State().position.x, target.z - m_player.State().position.z));
        m_trackerBeepIn -= dt;
        if (m_trackerBeepIn <= 0.0f)
        {
            m_trackerBeepIn = std::clamp(0.15f + distance / 55.0f, 0.15f, 2.0f);
            m_trackerBeepAt = m_deviceClock;
            PlayNamed("Items/tracker_beep", m_player.State().position, 0.45f, 1.0f + 0.4f * std::clamp(1.0f - distance / 40.0f, 0.0f, 1.0f), false);
        }
    }
}

void PredationGame::DrawDevice()
{
    const std::string& device = HeldDevice();
    if (device.empty() || m_screen != Screen::Playing || CinematicHoldsPlayers())
    {
        return;
    }
    ImVec2 centre;
    float radius = 0.0f;
    DeviceScreen(centre, radius);
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 glass = IM_COL32(6, 14, 10, 215);
    const ImU32 line = IM_COL32(70, 170, 110, 120);
    const ImU32 bright = IM_COL32(120, 240, 160, 255);
    const float facing = m_lookYaw;
    draw->AddCircleFilled(centre, radius + 6.0f, IM_COL32(20, 22, 24, 230), 48);
    draw->AddCircleFilled(centre, radius, glass, 48);
    draw->AddCircle(centre, radius, line, 48, 1.5f);
    draw->AddCircle(centre, radius * 0.5f, IM_COL32(70, 170, 110, 60), 48, 1.0f);
    // You, in the middle, facing up.
    draw->AddTriangleFilled({centre.x, centre.y - 7.0f}, {centre.x - 5.0f, centre.y + 5.0f}, {centre.x + 5.0f, centre.y + 5.0f}, bright);

    char label[48];
    if (device == "map")
    {
        // The sweep, and the walls it has found, fading after it passes.
        if (m_scanDistance.size() == kScanSamples)
        {
            ImVec2 previous{};
            bool had = false;
            for (int i = 0; i <= kScanSamples; ++i)
            {
                const int index = i % kScanSamples;
                const float d = m_scanDistance[static_cast<size_t>(index)];
                const float age = m_deviceClock - m_scanSeen[static_cast<size_t>(index)];
                if (d < 0.0f || age > kScanFade)
                {
                    had = false;
                    continue;
                }
                const float angle = static_cast<float>(index) / kScanSamples * glm::two_pi<float>();
                const ImVec2 p = OnScreen(centre, radius, angle, facing, d);
                const int alpha = static_cast<int>(255.0f * (1.0f - age / kScanFade));
                const ImU32 colour = IM_COL32(120, 240, 160, alpha);
                // Joined up where the wall runs on, so it reads as a wall and not a scatter of dots.
                const float before = m_scanDistance[static_cast<size_t>((index + kScanSamples - 1) % kScanSamples)];
                if (had && before >= 0.0f && std::abs(before - d) * m_scanReach < 1.2f)
                {
                    draw->AddLine(previous, p, colour, 2.0f);
                }
                else
                {
                    draw->AddCircleFilled(p, 1.6f, colour, 6);
                }
                previous = p;
                had = true;
            }
        }
        const ImVec2 edge = OnScreen(centre, radius, m_scanAngle, facing, 1.0f);
        draw->AddLine(centre, edge, IM_COL32(120, 240, 160, 150), 1.5f);
        std::snprintf(label, sizeof(label), "MAP  %.0f M", m_scanReach);
    }
    else if (device == "tracker")
    {
        glm::vec3 target;
        if (ObjectiveTarget(target))
        {
            // Where it is, on a scale that keeps a long way off inside the screen and a short way readable.
            const glm::vec2 to{target.x - m_player.State().position.x, target.z - m_player.State().position.z};
            const float distance = glm::length(to);
            const float angle = std::atan2(to.x, -to.y);
            const float fraction = std::clamp(std::log(1.0f + distance) / std::log(1.0f + 150.0f), 0.08f, 1.0f);
            const ImVec2 blip = OnScreen(centre, radius, angle, facing, fraction);
            const float flash = std::clamp(1.0f - (m_deviceClock - m_trackerBeepAt) / 0.25f, 0.0f, 1.0f);
            draw->AddCircleFilled(blip, 5.0f + 4.0f * flash, IM_COL32(120, 240, 160, static_cast<int>(140 + 115 * flash)), 16);
            // An arrow round the rim the way it lies, for when the blip is lost in the middle.
            draw->AddCircleFilled(OnScreen(centre, radius - 8.0f, angle, facing, 1.0f), 3.0f, bright, 8);
            std::snprintf(label, sizeof(label), "OBJ  %.0f M", distance);
        }
        else
        {
            std::snprintf(label, sizeof(label), "NO SIGNAL");
        }
    }
    else
    {
        return;
    }
    const ImVec2 size = ImGui::CalcTextSize(label);
    draw->AddText({centre.x - size.x * 0.5f, centre.y + radius + 10.0f}, bright, label);
}

void PredationGame::TakeOutMap()
{
    // The map's key takes the map out of the bag, or puts it away again: there is no map without one.
    const ItemId map = m_items.IdOf("site_map");
    for (int slot = 0; slot < m_inventory.SlotCount(); ++slot)
    {
        if (map != kInvalidItem && m_inventory.At(slot).item == map)
        {
            m_inventory.SelectSlot(m_inventory.SelectedSlot() == slot ? Inventory::kNoSlot : slot);
            return;
        }
    }
    m_app->GetConsole().Print("No map on you: there are some in the gear room aboard.");
}

} // namespace pred
