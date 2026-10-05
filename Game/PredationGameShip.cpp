// The ship as the game plays it: the part of a deployment spent aboard -- the burn to the site, arriving over it, and
// the shuttle dropping out of the hangar -- the shuttle's launch controls, and what everybody aboard is to do next. The
// ship itself is Game/World/ShipMap.

#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"

#include <imgui.h>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <string>

namespace pred
{

// How long the burn to a site takes, walking about the ship, between the ship leaving and it arriving.
CVar<float> cv_shipTravelSeconds{"game.ship_travel_seconds", 90.0f, "Seconds the ship takes to reach a site once it has left, aboard"};
// How fast the dust goes past the windows, under way.
constexpr float kDustSpeed = 140.0f;

void PredationGame::RegisterShipCommands()
{
#if PRED_DEV_TOOLS
    // Somewhere aboard at once, for trying things: ship_goto shuttle|hangar|briefing|gear|cockpit.
    m_app->GetConsole().RegisterCommand(
        "ship_goto", "Go straight to somewhere aboard: ship_goto <shuttle|hangar|briefing|gear|loadout|cockpit>",
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
            else if (where == "hangar" || where == "bay")
            {
                at = ShipMap::ToWorld({-4.9f, 0.1f, 5.0f});
                yaw = glm::pi<float>();
            }
            else if (where == "gear")
            {
                at = ShipMap::ToWorld({2.0f, 0.1f, 1.0f});
            }
            else if (where == "loadout")
            {
                at = ShipMap::ToWorld({1.6f, 0.1f, -1.0f});
                yaw = glm::half_pi<float>();
            }
            else if (where == "cockpit")
            {
                at = ShipMap::ToWorld({0.0f, ShipSpec::kDeck + 0.1f, -13.8f});
            }
            else if (where == "engines")
            {
                at = ShipMap::ToWorld({0.0f, 0.1f, 20.4f});
                yaw = glm::pi<float>();
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
            // "all": the host takes everybody, side by side -- a player's place is the host's to say.
            if (args.size() >= 3 && args[2] == "all" && m_sessionMode == SessionMode::Host)
            {
                float side = 0.0f;
                for (const RemotePlayerView& remote : m_host.Remotes())
                {
                    side += 0.8f;
                    const glm::vec3 place = at + glm::vec3(std::cos(yaw), 0.0f, std::sin(yaw)) * side;
                    m_host.RespawnPlayer(remote.id, place);
                    WorldEventMessage event;
                    event.kind = WorldEventKind::PlayerRespawned;
                    event.player = remote.id;
                    event.position = place;
                    m_host.Broadcast(event);
                }
            }
        });
    m_app->GetConsole().RegisterCommand("ship_travel", "As the host, under way: this many seconds left of the journey: ship_travel <seconds>",
                                        [this](const std::vector<std::string>& args)
                                        {
                                            if (IsAuthority() && m_shipTravel > 0.0f && args.size() >= 2)
                                            {
                                                m_shipTravel = std::max(static_cast<float>(std::atof(args[1].c_str())), 0.01f);
                                                m_shipStateSent = ~0ull;
                                            }
                                        });
    m_app->GetConsole().RegisterCommand("shuttle_launch", "As the host, work the shuttle's controls, as boarding does",
                                        [this](const std::vector<std::string>&)
                                        {
                                            if (!LaunchFromShip(LocalPlayerId()))
                                            {
                                                m_app->GetConsole().PrintError("Not now: the ship is not over a site, or not everybody is aboard.");
                                            }
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
        return cabin.position + across * ((static_cast<float>(player % kMaxPlayers) - 1.5f) * 0.7f) + glm::vec3(0.0f, 0.1f, 0.0f);
    }
    yaw = m_ship.SpawnYaw();
    return m_ship.Spawn(player);
}

void PredationGame::BeginTransit()
{
    // The site is chosen: the ship leaves where it was (ship_depart), and is under way to it for the length of the burn,
    // aboard, everybody free to walk about it; at the end of that it arrives (UpdateShipTravel). Not aboard, it is simply
    // there.
    m_shipReady = false;
    if (m_map != MapChoice::Ship)
    {
        ArriveOverSite();
        return;
    }
    m_shipOrbiting = 0;
    m_shipTravelTotal = std::max(cv_shipTravelSeconds.Get(), 1.0f);
    m_shipTravel = m_shipTravelTotal;
    PRED_LOG_INFO(Gameplay, "Under way to site {}: {:.0f} s", m_facility.Seed(), m_shipTravelTotal);
    if (HasCinematic("ship_depart"))
    {
        PlayCinematic("ship_depart");
    }
}

void PredationGame::UpdateShipTravel(float dt)
{
    // Only aboard, and only while playing: the menu's backdrop goes nowhere.
    const bool underWay = m_shipTravel > 0.0f && m_screen == Screen::Playing && m_map == MapChoice::Ship;
    if (m_shipTravel > 0.0f && m_map != MapChoice::Ship)
    {
        m_shipTravel = 0.0f;
    }
    // The dust past the windows, and the engines burning -- the picture, not the cinematic's, while one has it.
    m_ship.UpdateDust(m_scene, m_app->GetMeshes(), underWay ? kDustSpeed : 0.0f, dt);
    if (!m_cine.Active())
    {
        const float burn = underWay ? 0.85f : 0.0f;
        if (std::abs(m_ship.Engines() - burn) > 1e-3f)
        {
            m_ship.SetEngines(m_scene, burn);
        }
    }
    // The clock runs once the leaving has been seen, and the arriving is shown when it runs out -- the host's call. In a
    // campaign the ship is flown instead (UpdateTravel), and arrives when it gets there.
    if (!underWay || m_cine.Active() || m_campaignOpen)
    {
        return;
    }
    m_shipTravel = std::max(m_shipTravel - dt, 0.0f);
    if (m_shipTravel > 0.0f)
    {
        return;
    }
    if (!IsAuthority())
    {
        // Held at the last moment until the host says it is there.
        m_shipTravel = 0.01f;
        return;
    }
    if (HasCinematic("ship_arrive"))
    {
        PlayCinematic("ship_arrive");
    }
    else
    {
        ArriveOverSite();
    }
}

void PredationGame::ArriveOverSite()
{
    if (m_campaignOpen)
    {
        ArriveAtBody();
        return;
    }
    m_shipOrbiting = m_facility.Seed();
    m_shipReady = true;
    m_shipTravel = 0.0f;
    m_shipTravelTotal = 0.0f;
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

glm::vec3 PredationGame::ArrivalFor(uint8_t player) const
{
    float ignored = 0.0f;
    return m_map == MapChoice::Facility ? MissionArrival(player) : m_map == MapChoice::Ship ? ShipArrival(player, ignored) : m_spawnPoint;
}

void PredationGame::SendShipState(int player)
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }
    WorldEventMessage event;
    event.kind = WorldEventKind::ShipState;
    event.index = static_cast<uint8_t>(m_map);
    event.item = m_shipOrbiting;
    event.flag = m_shipReady;
    event.flag2 = m_shipTravel > 0.0f;
    event.amount = m_shipTravel;
    event.direction.x = m_shipTravelTotal;
    event.other = static_cast<uint8_t>(m_order);
    event.rounds = m_orderSite;
    event.direction.y = m_briefingAt;
    event.quiet = true;
    if (player >= 0)
    {
        m_host.SendTo(static_cast<uint8_t>(player), event);
        return;
    }
    m_host.Broadcast(event);
}

void PredationGame::UpdateShip()
{
    RecoverFallen();
    // The host says so whenever where everybody is, or how the ship stands, changes.
    if (m_sessionMode == SessionMode::Host && m_screen == Screen::Playing)
    {
        // Under way counts as a change of its own; the seconds left are counted down on each machine from there.
        const uint64_t now = static_cast<uint64_t>(m_map) | (static_cast<uint64_t>(m_shipOrbiting) << 2) | (m_shipReady ? 1ull << 18 : 0ull) |
                             (m_shipTravel > 0.0f ? 1ull << 19 : 0ull) | (static_cast<uint64_t>(m_order) << 20) |
                             (static_cast<uint64_t>(m_orderSite) << 22);
        if (now != m_shipStateSent)
        {
            m_shipStateSent = now;
            SendShipState();
        }
    }
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
    // The console in the briefing room is the navigation console in a campaign.
    if (Interactable* console = m_interactions.Find(m_deployConsole))
    {
        console->verb = m_campaignOpen ? "Open" : "Use";
        console->name = m_campaignOpen ? "the navigation map" : "deployment console";
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
        const StarSystem* system = CurrentSystem();
        const Body* heading_ = system != nullptr ? system->Find(m_campaign.travel.target) : nullptr;
        if (system != nullptr && m_campaign.travel.interstellar)
        {
            const std::string to = m_universe.Glance(SystemId::Unpack(m_campaign.travel.toSystem)).name;
            ImGui::TextColored(text, "%s", ("Crossing to " + to + ".").c_str());
            ImGui::TextDisabled("The navigation map is at the table behind the cockpit.");
        }
        else if (system != nullptr && m_campaign.travel.underway)
        {
            ImGui::TextColored(text, "%s", heading_ != nullptr ? ("Under way to " + heading_->name + ".").c_str() : "Coming to a stop.");
            ImGui::TextDisabled("The navigation map is at the table behind the cockpit.");
        }
        else if (m_shipTravel > 0.0f)
        {
            ImGui::TextColored(text, "Under way to the site.");
        }
        else if (m_shipReady)
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
        else if (m_order == OrderState::Incoming)
        {
            ImGui::TextColored(text, "Orders have come in.");
            ImGui::TextDisabled("Play the briefing at the console in the briefing room.");
        }
        else if (m_order == OrderState::Briefing)
        {
            ImGui::TextColored(text, "Briefing in the briefing room.");
        }
        else if (m_order == OrderState::Ready)
        {
            ImGui::TextColored(text, "Deploy from the console in the briefing room.");
        }
        else if (system != nullptr)
        {
            ImGui::TextColored(text, "Choose where to go.");
            ImGui::TextDisabled("The navigation map is at the table behind the cockpit.");
        }
        else
        {
            ImGui::TextColored(text, "Waiting for orders.");
        }
    }
    ImGui::End();
}

} // namespace pred
