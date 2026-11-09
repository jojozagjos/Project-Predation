// Pointing something out (Phase 4): the ping key marks where you are looking, for everybody, for a few seconds -- a mark in your
// colour with your name and how far it is, and a chime in everybody's helmet. Asked of the host like everything else, and told to
// everybody from there.

#include "Game/PredationGame.h"
#include "Game/PredationGameShared.h"

#include "Engine/Core/Log.h"

#include <imgui.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

// How long a mark stays, the last of it fading; and how far a ping reaches.
constexpr float kPingSeconds = 7.0f;
constexpr float kPingFade = 1.5f;
constexpr float kPingReach = 120.0f;

// Each player's colour, by their number: told apart at a glance, and none of them the HUD's amber.
ImU32 PingColour(uint8_t player, float alpha)
{
    static constexpr ImU32 kColours[] = {IM_COL32(90, 210, 255, 255), IM_COL32(120, 235, 140, 255), IM_COL32(245, 120, 200, 255),
                                         IM_COL32(250, 236, 110, 255), IM_COL32(170, 150, 255, 255), IM_COL32(255, 160, 110, 255),
                                         IM_COL32(110, 240, 220, 255), IM_COL32(230, 230, 230, 255)};
    const ImU32 colour = kColours[player % std::size(kColours)];
    return (colour & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << IM_COL32_A_SHIFT);
}

} // namespace

void PredationGame::PackPing(const glm::vec3& at, int& a, int& b)
{
    // Across in quarter metres (to eight kilometres either way, which every place is within), up in sixty-fourths.
    const auto quarter = [](float v) { return static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(std::clamp(std::lround(v * 4.0f), -32767L, 32767L)))); };
    a = static_cast<int>((quarter(at.x) << 16) | quarter(at.z));
    b = static_cast<int>(std::lround(at.y * 64.0f));
}

glm::vec3 PredationGame::UnpackPing(int a, int b)
{
    const auto packed = static_cast<uint32_t>(a);
    const auto x = static_cast<int16_t>(static_cast<uint16_t>(packed >> 16));
    const auto z = static_cast<int16_t>(static_cast<uint16_t>(packed & 0xFFFFu));
    return {static_cast<float>(x) / 4.0f, static_cast<float>(b) / 64.0f, static_cast<float>(z) / 4.0f};
}

void PredationGame::PressPing()
{
    if (m_pingCooldown > 0.0f || !m_player.State().alive || m_screen != Screen::Playing)
    {
        return;
    }
    m_pingCooldown = 0.6f;
    // Whatever is in the middle of the view: the first thing it meets, or a point well ahead when it meets nothing.
    const PlayerView& view = m_player.View();
    glm::vec3 at = view.eyePosition + view.Forward() * 40.0f;
    if (const RayHit hit = m_app->GetPhysics().RayCast(view.eyePosition, view.Forward(), kPingReach))
    {
        at = hit.position;
    }
    if (m_sessionMode == SessionMode::Client)
    {
        // Shown here at once, and asked of the host, which tells everybody (this one too, which only puts it back where it is).
        AddPing(LocalPlayerId(), at);
        int a = 0;
        int b = 0;
        PackPing(at, a, b);
        AskCampaign(CampaignAction::Ping, a, b);
        return;
    }
    Ping(LocalPlayerId(), at);
}

void PredationGame::Ping(uint8_t player, const glm::vec3& at)
{
    AddPing(player, at);
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::Ping;
        event.player = player;
        event.position = at;
        m_host.Broadcast(event);
    }
}

void PredationGame::AddPing(uint8_t player, const glm::vec3& at)
{
    // One each: a new one moves it. (The host telling a client of its own ping, already shown, only puts it back where it is.)
    const bool echo = std::any_of(m_pings.begin(), m_pings.end(), [&](const PingMark& mark)
                                  { return mark.player == player && mark.age < 2.0f && glm::distance(mark.at, at) < 0.5f; });
    m_pings.erase(std::remove_if(m_pings.begin(), m_pings.end(), [player](const PingMark& mark) { return mark.player == player; }), m_pings.end());
    m_pings.push_back({player, at, 0.0f});
    if (!echo)
    {
        PlayNamed("UI/ping", m_renderEye, 0.35f, 1.0f, false);
    }
}

void PredationGame::UpdatePings(float dt)
{
    m_pingCooldown = std::max(m_pingCooldown - dt, 0.0f);
    for (PingMark& mark : m_pings)
    {
        mark.age += dt;
    }
    m_pings.erase(std::remove_if(m_pings.begin(), m_pings.end(), [](const PingMark& mark) { return mark.age > kPingSeconds; }), m_pings.end());
}

void PredationGame::DrawPings(ImDrawList* draw, const ImGuiViewport* viewport)
{
    for (const PingMark& mark : m_pings)
    {
        const float alpha = std::clamp((kPingSeconds - mark.age) / kPingFade, 0.0f, 1.0f);
        // Where it is on the screen -- kept to the edge, pointing, when it is off it or behind.
        const glm::vec4 clip = m_viewProjection * glm::vec4(mark.at, 1.0f);
        glm::vec2 ndc = clip.w > 0.05f ? glm::vec2(clip) / clip.w : -glm::vec2(clip.x, clip.y) * 1000.0f;
        const bool off = clip.w <= 0.05f || std::abs(ndc.x) > 0.92f || std::abs(ndc.y) > 0.88f;
        if (off)
        {
            const float scale = std::max(std::abs(ndc.x) / 0.92f, std::abs(ndc.y) / 0.88f);
            ndc /= std::max(scale, 1e-3f);
        }
        const ImVec2 at{viewport->Pos.x + (ndc.x * 0.5f + 0.5f) * viewport->Size.x, viewport->Pos.y + (0.5f - ndc.y * 0.5f) * viewport->Size.y};
        const ImU32 colour = PingColour(mark.player, alpha);
        const ImU32 shade = IM_COL32(0, 0, 0, static_cast<int>(150.0f * alpha));
        // A diamond with a ring that grows out of it the moment it is made.
        const float r = 7.0f;
        const ImVec2 points[4] = {{at.x, at.y - r}, {at.x + r, at.y}, {at.x, at.y + r}, {at.x - r, at.y}};
        draw->AddPolyline(points, 4, shade, ImDrawFlags_Closed, 4.0f);
        draw->AddPolyline(points, 4, colour, ImDrawFlags_Closed, 2.0f);
        draw->AddCircleFilled(at, 2.5f, colour);
        if (mark.age < 0.6f)
        {
            draw->AddCircle(at, r + mark.age * 40.0f, PingColour(mark.player, alpha * (1.0f - mark.age / 0.6f)), 0, 1.5f);
        }
        // Who, and how far.
        std::string who = mark.player == LocalPlayerId() ? PlayerName() : "Crew " + std::to_string(mark.player + 1);
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            who = remote.id == mark.player ? remote.name : who;
        }
        const int metres = static_cast<int>(std::round(glm::distance(m_renderEye, mark.at)));
        const std::string line = who + "  " + std::to_string(metres) + " m";
        const ImVec2 size = ImGui::CalcTextSize(line.c_str());
        const ImVec2 text{at.x - size.x * 0.5f, at.y + r + 4.0f};
        draw->AddText({text.x + 1.0f, text.y + 1.0f}, shade, line.c_str());
        draw->AddText(text, colour, line.c_str());
    }
}

} // namespace pred
