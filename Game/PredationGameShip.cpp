// The ship as the game plays it: the part of a deployment spent aboard -- the burn to the site, arriving over it, and
// the shuttle dropping out of the hangar -- the shuttle's launch controls, and what everybody aboard is to do next. The
// ship itself is Game/World/ShipMap.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"

#include <imgui.h>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <string>

namespace pred
{

void PredationGame::RegisterShipCommands()
{
#if PRED_DEV_TOOLS
    // Somewhere aboard at once, for trying things: ship_goto shuttle|hangar|briefing|gear|cockpit.
    m_app->GetConsole().RegisterCommand(
        "ship_goto", "Go straight to somewhere aboard: ship_goto <shuttle|hangar|briefing|gear|cockpit>",
        [this](const std::vector<std::string>& args)
        {
            const std::string where = args.size() >= 2 ? args[1] : "briefing";
            glm::vec3 at = m_ship.Spawn(LocalPlayerId());
            float yaw = 0.0f;
            if (where == "shuttle")
            {
                // In its cabin, at the front, facing the controls.
                at = m_ship.Shuttle().Home().position + m_ship.Shuttle().Home().rotation * glm::vec3(0.0f, 0.8f, -3.6f);
            }
            else if (where == "hangar")
            {
                at = ShipMap::ToWorld({0.0f, 0.1f, 8.0f});
                yaw = glm::pi<float>();
            }
            else if (where == "gear")
            {
                at = ShipMap::ToWorld({-6.0f, 0.1f, 0.0f});
            }
            else if (where == "cockpit")
            {
                at = ShipMap::ToWorld({0.0f, ShipSpec::kUpperDeck + 0.1f, -34.0f});
            }
            if (m_map != MapChoice::Ship)
            {
                m_app->GetConsole().PrintError("Only aboard: go aboard with ship first.");
                return;
            }
            m_player.Teleport(at);
            m_lookYaw = yaw;
            m_lookPitch = 0.0f;
            m_player.State().yaw = yaw;
        });
#endif
}

void PredationGame::BuildShipControls()
{
    // The shuttle's controls: its cabin's front wall, used from inside.
    const Entity wall = m_ship.Shuttle().Part("front_wall");
    if (!m_scene.IsAlive(wall))
    {
        return;
    }
    Interactable interactable;
    interactable.entity = wall;
    interactable.kind = InteractionKind::Board;
    interactable.verb = "Launch";
    interactable.name = "the shuttle";
    interactable.range = 2.6f;
    interactable.enabled = false;
    m_interactions.Register(interactable);
    m_shipControls = wall;
}

void PredationGame::ShuttleAboard(int& aboard, int& everybody) const
{
    // Everybody who is up, and how many of them are in the ship's shuttle.
    aboard = 0;
    everybody = 0;
    const auto count = [&](bool alive, const glm::vec3& at)
    {
        if (!alive)
        {
            return;
        }
        ++everybody;
        aboard += m_ship.Shuttle().Aboard(at) ? 1 : 0;
    };
    count(m_player.State().alive, m_player.State().position);
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        count(remote.alive, remote.position);
    }
}

glm::vec3 PredationGame::ShipArrival(uint8_t player, float& yaw) const
{
    CinePose cabin;
    const bool cameBack = m_dockingReturn && ((m_mission.aboard >> player) & 1u) != 0;
    if (cameBack && m_ship.Shuttle().Socket("arrival", cabin))
    {
        // Side by side across the cabin, facing its ramp.
        const glm::vec3 faces = cabin.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        yaw = std::atan2(faces.x, -faces.z);
        const glm::vec3 across = cabin.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
        return cabin.position + across * ((static_cast<float>(player % 6) - 2.5f) * 0.55f) + glm::vec3(0.0f, 0.1f, 0.0f);
    }
    yaw = m_ship.SpawnYaw();
    return m_ship.Spawn(player);
}

void PredationGame::BeginTransit()
{
    // The site is chosen: the ship burns for it, and arrives over its planet (the transit's "arrive" marker). Without the
    // cinematic, it is simply there.
    m_shipReady = false;
    if (m_map == MapChoice::Ship && HasCinematic("ship_transit"))
    {
        PlayCinematic("ship_transit");
        return;
    }
    ArriveOverSite();
}

void PredationGame::ArriveOverSite()
{
    m_shipOrbiting = m_facility.Seed();
    m_shipReady = true;
    PRED_LOG_INFO(Gameplay, "The ship is over site {}", m_shipOrbiting);
}

void PredationGame::RecoverFallen()
{
    // Out of the bottom of wherever this is -- through a gap, off an edge -- and falling for ever: put back where
    // everybody arrives here. Nothing is below any floor by forty metres.
    if (m_screen == Screen::Playing && m_player.State().alive && !CinematicHoldsPlayers() && m_player.State().position.y < -40.0f)
    {
        PRED_LOG_INFO(Gameplay, "Fell out of the world at {:.1f} {:.1f} {:.1f}: back to where this place is arrived at", m_player.State().position.x,
                      m_player.State().position.y, m_player.State().position.z);
        RespawnLocalPlayer(m_map == MapChoice::Ship ? m_ship.Spawn(LocalPlayerId()) : m_spawnPoint);
        m_player.State().velocity = glm::vec3(0.0f);
    }
}

void PredationGame::UpdateShip()
{
    RecoverFallen();
    if (!m_ship.Built())
    {
        return;
    }
    // Aboard, nobody tires and no torch runs down.
    if (m_ship.Contains(m_player.State().position))
    {
        m_player.State().stamina = 1.0f;
        m_player.State().winded = false;
        m_torchCharge = 1.0f;
    }
    // The controls offer to launch only when there is somewhere to go, and say who is not aboard yet.
    if (Interactable* controls = m_interactions.Find(m_shipControls))
    {
        int aboard = 0;
        int everybody = 0;
        ShuttleAboard(aboard, everybody);
        controls->enabled = m_shipReady && !m_cine.Active() && m_map == MapChoice::Ship;
        controls->verb = aboard >= everybody ? "Launch" : "Launch (" + std::to_string(aboard) + " of " + std::to_string(everybody) + " aboard)";
    }
}

bool PredationGame::LaunchFromShip(uint8_t player)
{
    // Everybody goes down together: nobody is left aboard the ship while the rest are on a site.
    int aboard = 0;
    int everybody = 0;
    ShuttleAboard(aboard, everybody);
    if (!IsAuthority() || !m_shipReady || m_cine.Active() || aboard < everybody || !HasCinematic("ship_launch"))
    {
        return false;
    }
    PRED_LOG_INFO(Gameplay, "Player {} launched the shuttle for site {}, {} aboard", player, m_facility.Seed(), aboard);
    PlayCinematic("ship_launch");
    return true;
}

void PredationGame::DrawShipHud()
{
    if (m_screen != Screen::Playing || !m_ship.Contains(m_player.State().position) || !m_player.State().alive)
    {
        return;
    }
    // The last deployment's result has the screen for a while first.
    if (m_mission.stage == MissionState::Stage::Over && m_missionOverFor <= MissionSpec::kResultSeconds + 5.0f)
    {
        return;
    }
    constexpr ImGuiWindowFlags kHud = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBackground;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + 16.0f, viewport->Pos.y + 44.0f}, ImGuiCond_Always);
    if (ImGui::Begin("##Ship", nullptr, kHud))
    {
        const ImVec4 heading{0.62f, 0.66f, 0.7f, 1.0f};
        const ImVec4 text{0.86f, 0.88f, 0.9f, 1.0f};
        ImGui::TextColored(heading, "OBJECTIVE");
        if (m_shipReady)
        {
            int aboard = 0;
            int everybody = 0;
            ShuttleAboard(aboard, everybody);
            ImGui::TextColored(text, "Board the shuttle in the hangar.");
            if (everybody > 1)
            {
                ImGui::TextDisabled("%d of %d aboard. Launch from its controls when everybody is.", aboard, everybody);
            }
            else
            {
                ImGui::TextDisabled("Launch from its controls, at the front of its cabin.");
            }
        }
        else
        {
            ImGui::TextColored(text, IsAuthority() ? "Choose the next deployment at the console in the briefing room."
                                                   : "The next deployment is chosen at the console in the briefing room.");
        }
    }
    ImGui::End();
}

} // namespace pred
