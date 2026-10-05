// The mission, as the game runs it: planned from the site whenever the site is built, run by the host -- the terminal,
// the breaker, the download, the drive, the launch -- and shown on every machine. The plan and the rules are in
// Game/Mission/Mission.h; the terminal, the panels and the console in the world are MissionProps.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/TestMap.h"
#include "Game/World/Vehicles.h"

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace pred
{

namespace
{

using Stage = MissionState::Stage;

// The deployment console aboard, across, high and deep.
constexpr glm::vec3 kDeployConsoleSize{1.2f, 1.05f, 0.6f};

// Where the shuttle's console is, to hear its alarm from.
glm::vec3 ConsoleOf(const VehicleProp& shuttle)
{
    CinePose console;
    return shuttle.Socket("console", console) ? console.position + glm::vec3(0.0f, 0.9f, 0.0f) : shuttle.Home().position;
}

// While the download goes on, the terminal is heard working now and then: not loud, but a room with something in it
// hears it.
constexpr float kHumEvery = 7.0f;
constexpr float kHumReach = 12.0f;
// Power coming back to a building is heard well beyond it.
constexpr float kBreakerReach = 22.0f;

WorldEventMessage MissionEvent(const MissionState& state)
{
    WorldEventMessage event;
    event.kind = WorldEventKind::Mission;
    event.index = static_cast<uint8_t>(state.stage);
    event.flag = state.powered;
    event.flag2 = state.attended;
    event.amount = state.progress;
    event.item = state.Launching() ? static_cast<uint16_t>(std::ceil(state.launchIn * 10.0f)) + 1u : 0u;
    event.rounds = state.recovered ? 1 : 0;
    event.other = state.aboard;
    return event;
}

MissionState MissionFrom(const WorldEventMessage& event)
{
    MissionState state;
    state.stage = static_cast<Stage>(std::min<uint8_t>(event.index, static_cast<uint8_t>(Stage::Over)));
    state.powered = event.flag;
    state.attended = event.flag2;
    state.progress = event.amount;
    state.launchIn = event.item == 0 ? -1.0f : static_cast<float>(event.item - 1u) * 0.1f;
    state.recovered = event.rounds != 0;
    state.aboard = event.other;
    return state;
}

// Which way, and how far, as a briefing would put it: "140 m north-east". North is -z.
std::string Bearing(const glm::vec3& from, const glm::vec3& to)
{
    const glm::vec2 offset{to.x - from.x, to.z - from.z};
    const float distance = glm::length(offset);
    if (distance < 6.0f)
    {
        return "close by";
    }
    static const char* const kNames[] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
    const float degrees = glm::degrees(std::atan2(offset.x, -offset.y)) + 360.0f + 22.5f;
    const int sector = static_cast<int>(degrees / 45.0f) % 8;
    const int rounded = static_cast<int>(std::round(distance / 10.0f)) * 10;
    return std::to_string(std::max(rounded, 10)) + " m " + kNames[sector];
}

std::string FloorName(int floor)
{
    return floor == 0 ? "on the ground floor" : floor == 1 ? "one floor up" : std::to_string(floor) + " floors up";
}

} // namespace

void PredationGame::RegisterMissionCommands()
{
    Console& console = m_app->GetConsole();
    console.RegisterCommand("mission", "Say where the mission's terminal is and how the mission stands",
                            [this](const std::vector<std::string>&)
                            {
                                if (!m_missionPlan.Valid())
                                {
                                    m_app->GetConsole().Print("No mission: no site, or nowhere on it for a terminal.");
                                    return;
                                }
                                static const char* const kStages[] = {"none", "find the terminal", "downloading", "carry the drive", "over"};
                                char line[256];
                                std::snprintf(line, sizeof(line), "Terminal in building %d, %s; power %s; map %s; %.0f s to copy. Now: %s, %.0f%%%s",
                                              m_missionPlan.building, FloorName(m_missionPlan.floor).c_str(), m_mission.powered ? "on" : "OUT",
                                              m_missionPlan.mapGiven ? "given" : "NOT given", m_missionPlan.downloadSeconds,
                                              kStages[static_cast<int>(m_mission.stage)], m_mission.progress * 100.0f,
                                              m_mission.Launching() ? ", launching" : "");
                                m_app->GetConsole().Print(line);
                            });
#if PRED_DEV_TOOLS
    console.RegisterCommand("mission_goto", "Stand in front of the mission's terminal, its building's breaker, or the shuttle's console: "
                            "mission_goto <terminal|breaker|shuttle>",
                            [this](const std::vector<std::string>& args)
                            {
                                const std::string where = args.size() >= 2 ? args[1] : "terminal";
                                glm::vec3 at{0.0f};
                                float facing = 0.0f; // as a thing is turned: it faces (-sin, 0, -cos)
                                float height = 0.0f; // how far its middle is over the floor
                                if (where == "terminal" && m_missionPlan.Valid())
                                {
                                    at = m_missionPlan.terminal;
                                    facing = m_missionPlan.terminalYaw;
                                    height = 0.92f + MissionSpec::kTerminalSize.y * 0.5f;
                                }
                                else if (where == "breaker" && m_missionPlan.Valid() &&
                                         BreakerPanel(m_facility.Plan().buildings[static_cast<size_t>(m_missionPlan.building)], at, facing))
                                {
                                    height = MissionSpec::kBreakerHeight;
                                }
                                else if (where == "shuttle")
                                {
                                    // In the shuttle, facing its console, which faces down the cabin.
                                    CinePose console;
                                    if (!m_facility.Shuttle().Socket("console", console))
                                    {
                                        return;
                                    }
                                    at = console.position + glm::vec3(0.0f, 0.475f, 0.0f);
                                    const glm::vec3 faces = console.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
                                    facing = std::atan2(-faces.x, -faces.z);
                                    height = 0.475f;
                                }
                                else
                                {
                                    m_app->GetConsole().PrintError("usage: mission_goto <terminal|breaker|shuttle>");
                                    return;
                                }
                                const glm::vec3 out{-std::sin(facing), 0.0f, -std::cos(facing)};
                                m_player.Teleport(at + out * 1.1f - glm::vec3(0.0f, height - 0.1f, 0.0f));
                                // Looking back at it (the way (sin, -cos) is, for a look), from about eye height.
                                m_lookYaw = std::atan2(-out.x, out.z);
                                m_lookPitch = std::atan2(height - 1.6f, 1.1f);
                            });
    console.RegisterCommand("briefing", "As the host, have orders come in now and stand at the console: briefing [site seed | play]",
                            [this](const std::vector<std::string>& args)
                            {
                                const Transform* console = m_scene.GetTransform(m_deployConsole);
                                if (console == nullptr || !IsAuthority())
                                {
                                    return;
                                }
                                const glm::vec3 front = console->rotation * glm::vec3(0.0f, 0.0f, -1.0f);
                                // On the deck the console stands on.
                                m_player.Teleport(glm::vec3(console->position.x, console->position.y - kDeployConsoleSize.y * 0.5f + 0.1f, console->position.z) + front * 1.2f);
                                m_lookYaw = std::atan2(-front.x, front.z);
                                if (args.size() >= 2 && args[1] == "play")
                                {
                                    StartBriefing();
                                    return;
                                }
                                const int seed = args.size() >= 2 ? std::atoi(args[1].c_str()) : 0;
                                ClearOrders();
                                IssueOrder(static_cast<uint16_t>(std::clamp(seed, 0, 65535)));
                            });
    console.RegisterCommand("deploy", "As the host, press Deploy on a site without the briefing: deploy [site seed]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!IsAuthority() || m_screen != Screen::Playing)
                                {
                                    return;
                                }
                                const int seed = args.size() >= 2 ? std::atoi(args[1].c_str()) : 0;
                                DeployTo(static_cast<uint16_t>(std::clamp(seed > 0 ? seed : static_cast<int>(m_facility.Seed()) + 1, 1, 65535)));
                            });
    console.RegisterCommand("mission_finish", "As the host, end the deployment with the drive aboard the shuttle: mission_finish [all|host] (who is aboard)",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!IsAuthority() || m_map != MapChoice::Facility || m_mission.stage == Stage::Over || m_mission.stage == Stage::None)
                                {
                                    return;
                                }
                                uint8_t aboard = static_cast<uint8_t>(1u << LocalPlayerId());
                                if (args.size() < 2 || args[1] != "host")
                                {
                                    for (const RemotePlayerView& remote : RemotePlayers())
                                    {
                                        aboard = static_cast<uint8_t>(aboard | (1u << remote.id));
                                    }
                                }
                                MissionRules::Finish(m_mission, true, aboard);
                                OnMissionOver();
                                BroadcastMission();
                            });
    console.RegisterCommand("mission_skip", "As the host, finish the download at once", [this](const std::vector<std::string>&)
                            {
                                if (IsAuthority() && m_mission.stage == Stage::Downloading)
                                {
                                    m_mission.progress = 0.999f;
                                }
                            });
#endif
}

void PredationGame::ResetMission()
{
    // The last plan's building lit again first: a circuit left off stays off, whatever is built next.
    for (const int circuit : m_missionPlan.circuits)
    {
        m_levelLights.SetPowered(circuit, true);
    }
    m_missionPlan = m_facility.Built() ? MissionPlan::Generate(m_facility.Plan(), m_facility.Seed()) : MissionPlan{};
    m_mission = MissionRules::Start(m_missionPlan);
    m_missionProps.Build(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_facility.Plan(), m_missionPlan, m_facility.Shuttle());
    m_driveItem = m_items.IdOf("data_drive");
    m_missionSendIn = 0.0f;
    m_missionHumIn = kHumEvery;
    m_missionOverFor = 0.0f;
    m_missionFoundNoPower = false;
    m_missionSeen = m_mission;
    m_foundNoPowerSeen = false;
    m_missionLeaving = false;
    AttachVehicleLamps();
    ShowMission();
    if (m_missionPlan.Valid())
    {
        PRED_LOG_INFO(Gameplay, "Mission: the data is on a terminal in building {}, {}, at {:.1f} {:.1f} {:.1f}{}{}; {:.0f} s to copy",
                      m_missionPlan.building, FloorName(m_missionPlan.floor), m_missionPlan.terminal.x, m_missionPlan.terminal.y,
                      m_missionPlan.terminal.z, m_missionPlan.powerOut ? ", its power out" : "", m_missionPlan.mapGiven ? "" : ", no map",
                      m_missionPlan.downloadSeconds);
    }
}

glm::vec3 PredationGame::MissionArrival(uint8_t player) const
{
    CinePose arrival;
    if (!m_facility.Shuttle().Socket("arrival", arrival))
    {
        return m_facility.Spawn();
    }
    // Side by side across the cabin, so four people do not arrive inside one another: a body is 0.64 m across, and
    // four of them this far apart fit the cabin's 3.6 m.
    const glm::vec3 across = arrival.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    return arrival.position + across * ((static_cast<float>(player) - 1.5f) * 0.7f) + glm::vec3(0.0f, 0.1f, 0.0f);
}

void PredationGame::ShowMission()
{
    m_missionProps.Show(m_scene, m_interactions, m_mission, static_cast<float>(m_lightClock));
    const bool powered = m_mission.powered || m_mission.stage == Stage::None;
    for (const int circuit : m_missionPlan.circuits)
    {
        if (m_levelLights.Powered(circuit) != powered)
        {
            m_levelLights.SetPowered(circuit, powered);
        }
    }
}

bool PredationGame::CarriesDrive(uint8_t player) const
{
    if (m_driveItem == kInvalidItem)
    {
        return false;
    }
    if (player == LocalPlayerId())
    {
        return m_inventory.CountOf(m_driveItem) > 0;
    }
    return m_sessionMode == SessionMode::Host && m_host.CarriedCount(player, static_cast<uint16_t>(m_driveItem)) > 0;
}

void PredationGame::BroadcastMission(bool quiet, int player)
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }
    WorldEventMessage event = MissionEvent(m_mission);
    event.quiet = quiet;
    if (player >= 0)
    {
        m_host.SendTo(static_cast<uint8_t>(player), event);
    }
    else
    {
        m_host.Broadcast(event);
    }
}

void PredationGame::ApplyMissionEvent(const WorldEventMessage& event)
{
    const MissionState was = m_mission;
    m_mission = MissionFrom(event);
    if (!event.quiet)
    {
        if (!was.powered && m_mission.powered && m_missionPlan.Valid())
        {
            glm::vec3 panel;
            float yaw = 0.0f;
            if (BreakerPanel(m_facility.Plan().buildings[static_cast<size_t>(m_missionPlan.building)], panel, yaw))
            {
                PlayNamed("World/breaker", panel, 1.0f);
            }
        }
        if (was.stage == Stage::Find && m_mission.stage == Stage::Downloading)
        {
            PlayNamed("World/terminal_beep", m_missionPlan.terminal, 0.7f);
        }
        if (was.stage == Stage::Downloading && m_mission.stage == Stage::Carry)
        {
            PlayNamed("World/terminal_done", m_missionPlan.terminal, 0.8f);
        }
        if (!was.Launching() && m_mission.Launching())
        {
            PlayNamed("World/shuttle_alarm", ConsoleOf(m_facility.Shuttle()), 0.9f);
        }
    }
    if (was.stage != Stage::Over && m_mission.stage == Stage::Over)
    {
        OnMissionOver();
    }
    ShowMission();
}

void PredationGame::OnMissionOver()
{
    m_missionOverFor = 0.0f;
    // The drive is the Company's now, or lost with the site: nobody still has it.
    for (int slot = 0; slot < m_inventory.SlotCount(); ++slot)
    {
        if (m_inventory.At(slot).item == m_driveItem)
        {
            m_inventory.RemoveFromSlot(slot, m_inventory.At(slot).count);
        }
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            const int carried = m_host.CarriedCount(remote.id, static_cast<uint16_t>(m_driveItem));
            if (carried > 0)
            {
                m_host.TakeCarried(remote.id, static_cast<uint16_t>(m_driveItem), carried);
            }
        }
    }
    PRED_LOG_INFO(Gameplay, "The shuttle has gone{}, {} aboard", m_mission.recovered ? " with the data" : " without the data",
                  std::popcount(static_cast<unsigned>(m_mission.aboard)));
    // And it leaves: the ramp up and the shuttle off the pad. The host starts it, for
    // everybody; its last marker takes everybody back aboard the ship.
    if (IsAuthority() && m_map == MapChoice::Facility && HasCinematic("surface_extraction") && !m_missionLeaving)
    {
        m_missionLeaving = true;
        PlayCinematic("surface_extraction");
    }
}

bool PredationGame::PerformMissionInteraction(InteractionKind kind, int index, uint8_t player)
{
    if (!IsAuthority())
    {
        return false;
    }
    switch (kind)
    {
    case InteractionKind::Terminal:
        if (!MissionRules::StartDownload(m_mission))
        {
            return false;
        }
        PlayNamed("World/terminal_beep", m_missionPlan.terminal, 0.7f);
        MakeNoise(NoiseKind::Item, m_missionPlan.terminal, kHumReach, player);
        m_missionHumIn = kHumEvery;
        PRED_LOG_INFO(Gameplay, "Player {} started the download", player);
        break;

    case InteractionKind::Breaker:
    {
        if (index != m_missionPlan.building || !MissionRules::RestorePower(m_mission))
        {
            return false;
        }
        glm::vec3 panel;
        float yaw = 0.0f;
        if (BreakerPanel(m_facility.Plan().buildings[static_cast<size_t>(index)], panel, yaw))
        {
            PlayNamed("World/breaker", panel, 1.0f);
            MakeNoise(NoiseKind::Door, panel, kBreakerReach, player);
        }
        PRED_LOG_INFO(Gameplay, "Player {} reset the breaker: building {} has power", player, index);
        break;
    }

    case InteractionKind::Deploy:
        // Anybody: orders in are played, and once briefed, everybody goes. Not while under way or over a site already.
        (void)player;
        if (m_shipTravel > 0.0f || m_shipReady)
        {
            return false;
        }
        if (m_order == OrderState::Incoming)
        {
            StartBriefing();
            return true;
        }
        if (m_order == OrderState::Ready)
        {
            const uint16_t site = m_orderSite;
            ClearOrders();
            DeployTo(site);
            return true;
        }
        return false;

    case InteractionKind::Board:
        return LaunchFromShip(player);

    case InteractionKind::Launch:
        if (!MissionRules::ToggleLaunch(m_mission))
        {
            return false;
        }
        if (m_mission.Launching())
        {
            PlayNamed("World/shuttle_alarm", ConsoleOf(m_facility.Shuttle()), 0.9f);
        }
        PRED_LOG_INFO(Gameplay, "Player {} {} the launch", player, m_mission.Launching() ? "started" : "held");
        break;

    default:
        return false;
    }
    ShowMission();
    BroadcastMission();
    return true;
}

void PredationGame::UpdateMission(float dt)
{
    UpdateArrivalAndIntercom(dt);
    ShowMission();
    if (m_mission.stage == Stage::Over)
    {
        // The result on screen for a while, and then everybody is back aboard the ship -- the testing area, until
        // there is a ship.
        const bool wasShowing = m_missionOverFor < MissionSpec::kResultSeconds;
        m_missionOverFor += dt;
        if (wasShowing && m_missionOverFor >= MissionSpec::kResultSeconds && IsAuthority() && m_screen == Screen::Playing &&
            m_map == MapChoice::Facility && !m_missionLeaving && !m_cine.Active())
        {
            GoToMap(MapChoice::Ship);
        }
        return;
    }
    if (!IsAuthority() || m_mission.stage == Stage::None)
    {
        return;
    }

    // Everybody who is up, and where.
    std::vector<std::pair<uint8_t, glm::vec3>> living;
    if (m_player.State().alive)
    {
        living.emplace_back(LocalPlayerId(), m_player.State().position);
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.alive)
        {
            living.emplace_back(remote.id, remote.position);
        }
    }

    bool attended = false;
    for (const auto& [id, at] : living)
    {
        const glm::vec2 apart{at.x - m_missionPlan.terminal.x, at.z - m_missionPlan.terminal.z};
        attended = attended || (glm::length(apart) < MissionSpec::kAttendReach && std::abs(at.y - m_missionPlan.terminal.y) < 2.2f);
    }
    const MissionState was = m_mission;
    const MissionRules::Ticked ticked = MissionRules::Tick(m_mission, m_missionPlan, dt, attended);
    bool changed = was.attended != m_mission.attended || was.stage != m_mission.stage;

    if (m_mission.stage == Stage::Downloading && m_mission.attended)
    {
        m_missionHumIn -= dt;
        if (m_missionHumIn <= 0.0f)
        {
            m_missionHumIn = kHumEvery;
            ShareSound("World/terminal_working", m_missionPlan.terminal, 0.6f);
            MakeNoise(NoiseKind::Item, m_missionPlan.terminal, kHumReach, -1);
        }
    }
    if (ticked.downloaded)
    {
        // Out onto the bench in front of it, for whoever is to carry it.
        PlayNamed("World/terminal_done", m_missionPlan.terminal, 0.8f);
        DropIntoWorld(m_driveItem, 1, -1, -1, m_missionPlan.driveAt, glm::vec3(0.0f));
        PRED_LOG_INFO(Gameplay, "The download is done: the drive is on the bench");
    }
    if (ticked.launched)
    {
        // Who is aboard, and whether the drive is: carried by somebody aboard, or lying in the cabin.
        uint8_t aboard = 0;
        bool drive = false;
        for (const auto& [id, at] : living)
        {
            if (m_facility.Shuttle().Aboard(at))
            {
                aboard = static_cast<uint8_t>(aboard | (1u << id));
                drive = drive || CarriesDrive(id);
            }
        }
        for (const WorldObjects::Pickup& pickup : m_world.Pickups())
        {
            if (pickup.alive && pickup.item == m_driveItem)
            {
                if (const Transform* where = m_scene.GetTransform(pickup.entity); where != nullptr && m_facility.Shuttle().Aboard(where->position))
                {
                    drive = true;
                }
            }
        }
        MissionRules::Finish(m_mission, drive, aboard);
        changed = true;
        OnMissionOver();
    }

    m_missionSendIn -= dt;
    const bool counting = m_mission.stage == Stage::Downloading || m_mission.Launching();
    if (changed || (counting && m_missionSendIn <= 0.0f))
    {
        BroadcastMission();
        m_missionSendIn = 0.5f;
    }
}

void PredationGame::DeployTo(uint16_t site)
{
    if (m_shipTravel > 0.0f)
    {
        return;
    }
    // A site that has been done is gone back to as it was; another is planned.
    if (site != m_facility.Seed() || m_mission.stage == MissionState::Stage::Over)
    {
        ChangeFacility(site);
    }
    // Aboard, the ship burns for it; anywhere else (the testing area), straight there.
    if (m_map == MapChoice::Ship)
    {
        BeginTransit();
    }
    else
    {
        GoToMap(MapChoice::Facility);
    }
}

void PredationGame::DrawMissionHud()
{
    if (m_screen != Screen::Playing || m_mission.stage == Stage::None)
    {
        return;
    }
    const bool over = m_mission.stage == Stage::Over;
    // Over, the result stays up until a little after everybody is back aboard, and then it is gone.
    if ((!over && !AtSite()) || (over && m_missionOverFor > MissionSpec::kResultSeconds + 5.0f))
    {
        return;
    }
    constexpr ImGuiWindowFlags kHud = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBackground;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec4 heading{0.62f, 0.66f, 0.7f, 1.0f};
    const ImVec4 text{0.86f, 0.88f, 0.9f, 1.0f};
    const ImVec4 warning{0.92f, 0.52f, 0.36f, 1.0f};

    if (over)
    {
        const bool mine = (m_mission.aboard >> LocalPlayerId()) & 1u;
        const int everybody = 1 + static_cast<int>(RemotePlayers().size());
        ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.3f}, ImGuiCond_Always,
                                {0.5f, 0.5f});
        if (ImGui::Begin("##MissionOver", nullptr, kHud))
        {
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextColored(m_mission.recovered ? ImVec4{0.5f, 0.85f, 0.6f, 1.0f} : warning,
                               m_mission.recovered ? "DATA RECOVERED" : "DATA NOT RECOVERED");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextColored(text, "%s", mine ? "You made it out." : m_mission.aboard == 0 ? "Nobody made it out." : "You were left behind.");
            ImGui::TextDisabled("%d of %d aboard", std::popcount(static_cast<unsigned>(m_mission.aboard)), everybody);
            const float left = MissionSpec::kResultSeconds - m_missionOverFor;
            // Only while still at the site: leaving is known on the host alone, but where everybody is, everybody knows.
            if (left > 0.0f && !m_missionLeaving && m_map == MapChoice::Facility && !m_cine.Active())
            {
                ImGui::TextDisabled("Back aboard the ship in %d", static_cast<int>(std::ceil(left)));
            }
        }
        ImGui::End();
        return;
    }

    // The objective, top left, under the microphone meter when there is one.
    ImGui::SetNextWindowPos({viewport->Pos.x + 16.0f, viewport->Pos.y + 44.0f}, ImGuiCond_Always);
    if (ImGui::Begin("##Mission", nullptr, kHud))
    {
        ImGui::TextColored(heading, "OBJECTIVE");
        switch (m_mission.stage)
        {
        case Stage::Find:
            // Found without power, the terminal waits on the breaker: that is what there is to do.
            if (!m_mission.powered && m_missionFoundNoPower)
            {
                ImGui::TextColored(text, "Restore the power: reset the building's breaker.");
                ImGui::TextDisabled("The terminal is dead until then.");
                break;
            }
            ImGui::TextColored(text, "Download the data from the terminal.");
            // Not where: finding it is the searching, the map and the tracker's.
            if (!m_missionPlan.mapGiven)
            {
                ImGui::TextDisabled("No map data for this site: search the buildings.");
            }
            if (!m_mission.powered && m_missionFoundNoPower)
            {
                ImGui::TextColored(warning, "No power at the terminal. Reset its building's breaker.");
            }
            break;
        case Stage::Downloading:
            ImGui::TextColored(text, "Downloading the data: %d%%", static_cast<int>(m_mission.progress * 100.0f));
            if (m_mission.attended)
            {
                ImGui::TextDisabled("Stay with the terminal.");
            }
            else
            {
                ImGui::TextColored(warning, "Paused: nobody is at the terminal.");
            }
            break;
        case Stage::Carry:
            ImGui::TextColored(text, "Take the drive to the shuttle.");
            ImGui::TextDisabled("It is waiting on the pad.");
            if (m_driveItem != kInvalidItem && m_inventory.CountOf(m_driveItem) > 0)
            {
                ImGui::TextDisabled("You have the drive.");
            }
            break;
        case Stage::None:
        case Stage::Over:
            break;
        }
        if (m_mission.Launching())
        {
            ImGui::TextColored(warning, "The shuttle leaves in %d. Anybody not aboard is left behind.",
                               static_cast<int>(std::ceil(m_mission.launchIn)));
        }
    }
    ImGui::End();
}


// --- Arriving, the intercom, and deploying -------------------------------------------------------

namespace
{


// The title card: in after a moment, up for a while, and out slowly.
constexpr float kCardIn = 1.0f;
constexpr float kCardFade = 1.2f;
constexpr float kCardHold = 5.0f;

} // namespace

void PredationGame::LoadMissionData()
{
    std::string error;
    const std::filesystem::path sites = Paths::AssetsRoot() / "Data" / "sites.json";
    if (!m_siteNames.LoadFromFile(sites, &error))
    {
        PRED_LOG_WARN(Gameplay, "Site names: {} -- using the examples built in", error);
    }
    const std::filesystem::path intercom = Paths::AssetsRoot() / "Data" / "intercom.json";
    if (!m_intercom.LoadFromFile(intercom, &error))
    {
        PRED_LOG_WARN(Gameplay, "Intercom lines: {}", error);
    }
    PRED_LOG_INFO(Gameplay, "The intercom has {} line(s)", m_intercom.Count());
    // Both are written by hand while the game is running, so both are read again when they change.
    m_app->GetFileWatcher().Watch(sites, [this](const std::filesystem::path& path) { m_siteNames.LoadFromFile(path); });
    m_app->GetFileWatcher().Watch(intercom, [this](const std::filesystem::path& path)
                                  {
                                      m_intercom.LoadFromFile(path);
                                      PRED_LOG_INFO(Gameplay, "The intercom has {} line(s)", m_intercom.Count());
                                  });
}

void PredationGame::BuildDeployConsole()
{
    MeshLibrary& meshes = m_app->GetMeshes();
    // In the ship's briefing room, before its screen, its front towards the room.
    const CinePose at = m_ship.BriefingConsole();
    Transform transform;
    transform.position = at.position + glm::vec3(0.0f, kDeployConsoleSize.y * 0.5f, 0.0f);
    transform.rotation = at.rotation;
    m_deployConsole = m_scene.CreateMeshEntity("deploy_console", transform, meshes.Upload(Primitives::Box(kDeployConsoleSize), "deploy_console"),
                                               Material::Metal({0.22f, 0.23f, 0.25f}, 0.5f));
    m_app->GetPhysics().CreateBox(kDeployConsoleSize * 0.5f, transform, BodyMotion::Static);
    Transform screen = transform;
    screen.position += transform.rotation * glm::vec3(0.0f, kDeployConsoleSize.y * 0.5f + 0.006f, -0.03f);
    Material glass = Material::Diffuse({0.03f, 0.035f, 0.04f}, 0.25f);
    glass.emissive = {0.12f, 0.25f, 0.4f};
    m_deployScreen = m_scene.CreateMeshEntity("deploy_console_screen", screen,
                                              meshes.Upload(Primitives::Box({0.9f, 0.01f, 0.4f}), "deploy_console_screen"), glass);
    Interactable interactable;
    interactable.entity = m_deployConsole;
    interactable.kind = InteractionKind::Deploy;
    interactable.verb = "Use";
    interactable.name = "deployment console";
    interactable.focusOffset = glm::vec3(0.0f, kDeployConsoleSize.y * 0.5f, -0.1f);
    interactable.range = 2.2f;
    m_interactions.Register(interactable);
}

void PredationGame::Say(const std::string& moment, float delay)
{
    m_intercomQueue.emplace_back(moment, delay);
}

void PredationGame::UpdateArrivalAndIntercom(float dt)
{
    // Arriving: the site the moment this player is on it, whoever brought them.
    const bool atSite = m_screen == Screen::Playing && m_mission.stage != Stage::None && m_mission.stage != Stage::Over && AtSite();
    // Arriving without a cinematic to show it (one shows its own title card, and says its own first line).
    if (atSite && !m_wasAtSite && !m_cine.Active())
    {
        m_titleCardFor = 0.0f;
        Say(m_missionPlan.mapGiven ? "arrival" : "arrival_no_map", kCardIn + kCardFade + 1.0f);
    }
    m_wasAtSite = atSite;
    if (m_titleCardFor >= 0.0f)
    {
        m_titleCardFor += dt;
        if (m_titleCardFor > kCardIn + kCardFade * 2.0f + kCardHold)
        {
            m_titleCardFor = -1.0f;
        }
    }

    // What has just happened, which the intercom has something to say about. Every machine sees the same changes,
    // so every machine says the same things.
    const MissionState& now = m_mission;
    const MissionState& was = m_missionSeen;
    if (now.stage != was.stage || now.Launching() != was.Launching())
    {
        if (was.stage == Stage::Find && now.stage == Stage::Downloading)
        {
            Say("download_started");
        }
        if (was.stage == Stage::Downloading && now.stage == Stage::Carry)
        {
            Say("download_done", 1.0f);
        }
        if (!was.Launching() && now.Launching())
        {
            Say("launch", 0.5f);
        }
        if (was.stage != Stage::Over && now.stage == Stage::Over)
        {
            const bool mine = (now.aboard >> LocalPlayerId()) & 1u;
            Say(now.recovered ? "recovered" : "not_recovered", 1.5f);
            if (!mine)
            {
                Say("left_behind");
            }
        }
    }
    if (m_missionFoundNoPower && !m_foundNoPowerSeen)
    {
        Say("power_out", 0.5f);
    }
    m_foundNoPowerSeen = m_missionFoundNoPower;
    m_missionSeen = m_mission;

    // One line at a time: the next waits for the last to finish.
    m_subtitleLeft = std::max(m_subtitleLeft - dt, 0.0f);
    for (auto& [moment, delay] : m_intercomQueue)
    {
        delay -= dt;
    }
    if (m_subtitleLeft <= 0.0f && !m_intercomQueue.empty() && m_intercomQueue.front().second <= 0.0f)
    {
        const std::string moment = m_intercomQueue.front().first;
        m_intercomQueue.erase(m_intercomQueue.begin());
        const IntercomLine* line = m_intercom.Pick(moment, m_missionPlan.seed);
        if (line == nullptr && moment == "arrival_no_map")
        {
            line = m_intercom.Pick("arrival", m_missionPlan.seed);
        }
        if (line != nullptr)
        {
            if (!line->sound.empty())
            {
                PlayNamed(line->sound, m_renderEye, 1.0f, 1.0f, false);
            }
            m_subtitle = line->subtitle;
            m_subtitleLeft = IntercomLines::SecondsFor(*line);
        }
    }
    if (m_screen != Screen::Playing)
    {
        m_intercomQueue.clear();
        m_subtitleLeft = 0.0f;
    }
}

void PredationGame::DrawTitleCard()
{
    if (m_titleCardFor < 0.0f || m_screen != Screen::Playing)
    {
        return;
    }
    const float t = m_titleCardFor - kCardIn;
    const float alpha = t < 0.0f ? 0.0f : t < kCardFade ? t / kCardFade : t < kCardFade + kCardHold ? 1.0f : 1.0f - (t - kCardFade - kCardHold) / kCardFade;
    if (alpha <= 0.0f)
    {
        return;
    }
    const SiteTitle title = PlaceTitle();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.62f}, ImGuiCond_Always, {0.5f, 0.5f});
    // A faint band behind it, so it reads over whatever the site looks like.
    ImGui::SetNextWindowBgAlpha(0.35f * alpha);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##TitleCard", nullptr, kFlags))
    {
        const auto centred = [&](const std::string& text, float scale, const ImVec4& colour)
        {
            ImGui::SetWindowFontScale(scale);
            const float width = ImGui::CalcTextSize(text.c_str()).x;
            ImGui::SetCursorPosX(std::max((ImGui::GetWindowSize().x - width) * 0.5f, 0.0f));
            ImGui::TextColored(colour, "%s", text.c_str());
        };
        centred(title.planet, 1.1f, {0.62f, 0.66f, 0.7f, alpha});
        centred(title.site, 1.7f, {0.9f, 0.92f, 0.94f, alpha});
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
}

void PredationGame::DrawSubtitle()
{
    if (m_subtitleLeft <= 0.0f || m_subtitle.empty() || m_screen != Screen::Playing)
    {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y - 150.0f}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::SetNextWindowSizeConstraints({0.0f, 0.0f}, {viewport->Size.x * 0.6f, viewport->Size.y});
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##Subtitle", nullptr, kFlags))
    {
        ImGui::PushTextWrapPos(viewport->Size.x * 0.58f);
        ImGui::TextColored({0.88f, 0.9f, 0.92f, std::min(m_subtitleLeft * 2.0f, 1.0f)}, "%s", m_subtitle.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

} // namespace pred
