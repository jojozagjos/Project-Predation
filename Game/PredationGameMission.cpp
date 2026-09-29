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

// Where the crawler's console is, to hear its alarm from.
glm::vec3 ConsoleOf(const VehicleProp& crawler)
{
    CinePose console;
    return crawler.Socket("console", console) ? console.position + glm::vec3(0.0f, 0.9f, 0.0f) : crawler.Home().position;
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
    console.RegisterCommand("site_map", "Open or close the site map, as its key does", [this](const std::vector<std::string>&)
                            { m_mapOpen = !m_mapOpen; });
#if PRED_DEV_TOOLS
    console.RegisterCommand("mission_goto", "Stand in front of the mission's terminal, its building's breaker, or the crawler's console: "
                            "mission_goto <terminal|breaker|crawler>",
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
                                else if (where == "crawler" || where == "shuttle")
                                {
                                    // In the crawler, facing its console, which faces down the cabin.
                                    CinePose console;
                                    if (!m_missionProps.Crawler().Socket("console", console))
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
                                    m_app->GetConsole().PrintError("usage: mission_goto <terminal|breaker|crawler>");
                                    return;
                                }
                                const glm::vec3 out{-std::sin(facing), 0.0f, -std::cos(facing)};
                                m_player.Teleport(at + out * 1.1f - glm::vec3(0.0f, height - 0.1f, 0.0f));
                                // Looking back at it (the way (sin, -cos) is, for a look), from about eye height.
                                m_lookYaw = std::atan2(-out.x, out.z);
                                m_lookPitch = std::atan2(height - 1.6f, 1.1f);
                            });
    console.RegisterCommand("briefing", "Stand at the deployment console and open its briefing: briefing [site seed]",
                            [this](const std::vector<std::string>& args)
                            {
                                const Transform* console = m_scene.GetTransform(m_deployConsole);
                                if (console == nullptr || !IsAuthority())
                                {
                                    return;
                                }
                                const glm::vec3 front = console->rotation * glm::vec3(0.0f, 0.0f, -1.0f);
                                m_player.Teleport(glm::vec3(console->position.x, 0.1f, console->position.z) + front * 1.2f);
                                m_lookYaw = std::atan2(-front.x, front.z);
                                const int seed = args.size() >= 2 ? std::atoi(args[1].c_str()) : 0;
                                OpenBriefing(static_cast<uint16_t>(seed > 0 ? seed : 1 + std::chrono::steady_clock::now().time_since_epoch().count() % 65535));
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
    m_missionProps.Build(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_facility.Plan(), m_missionPlan);
    m_driveItem = m_items.IdOf("data_drive");
    m_missionSendIn = 0.0f;
    m_missionHumIn = kHumEvery;
    m_missionOverFor = 0.0f;
    m_missionFoundNoPower = false;
    m_missionSeen = m_mission;
    m_foundNoPowerSeen = false;
    m_missionLeaving = false;
    // The crawler's ways: from the pad round everything to beside where it parks, and on past it outwards; then backing
    // straight in, its ramp to the door; and, leaving, straight out and round everything back to the pad. Every corner
    // rounded off, because a vehicle does not turn on a point.
    m_missionRoute.clear();
    m_missionReverse.clear();
    m_missionRouteBack.clear();
    if (m_missionPlan.Valid() && m_missionProps.Crawler().Built())
    {
        const SitePlan& site = m_facility.Plan();
        const CinePose park = m_missionProps.Crawler().Home();
        const glm::vec3 facing = park.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 away = glm::normalize(glm::vec3(facing.x, 0.0f, facing.z));
        glm::vec3 side{-away.z, 0.0f, away.x};
        // Beside the spot on the side the pad is.
        if (glm::dot(site.crawlerStart.position - park.position, side) < 0.0f)
        {
            side = -side;
        }
        const glm::vec3 beside = park.position + side * 6.0f + away * 1.0f;
        const glm::vec3 past = park.position + away * 6.0f;
        const glm::vec3 pulled = park.position + away * 12.0f;
        std::vector<glm::vec3> out = site.Route(site.crawlerStart.position, beside);
        out.push_back(past);
        out.push_back(pulled);
        m_missionRoute = SitePlan::Smoothed(out);
        m_missionReverse = {pulled, park.position};
        std::vector<glm::vec3> back{park.position};
        for (const glm::vec3& point : site.Route(pulled, site.crawlerStart.position))
        {
            back.push_back(point);
        }
        m_missionRouteBack = SitePlan::Smoothed(back);
    }
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
    if (!m_missionProps.Crawler().Socket("arrival", arrival))
    {
        return m_facility.Spawn();
    }
    // Side by side across the cabin, so four people do not arrive inside one another.
    const glm::vec3 across = arrival.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    return arrival.position + across * ((static_cast<float>(player) - 1.5f) * 0.55f) + glm::vec3(0.0f, 0.1f, 0.0f);
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
            PlayNamed("World/shuttle_alarm", ConsoleOf(m_missionProps.Crawler()), 0.9f);
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
    PRED_LOG_INFO(Gameplay, "The crawler has gone{}, {} aboard", m_mission.recovered ? " with the data" : " without the data",
                  std::popcount(static_cast<unsigned>(m_mission.aboard)));
    // And it leaves: the crawler away from the building, back to the shuttle, and the shuttle up. The host starts it, for
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
        // Choosing where everybody goes is the host's, and it happens on the host's screen.
        if (player != LocalPlayerId())
        {
            return false;
        }
        OpenBriefing(static_cast<uint16_t>(1 + std::chrono::steady_clock::now().time_since_epoch().count() % 65535));
        return true;

    case InteractionKind::Board:
        return LaunchFromShip(player);

    case InteractionKind::Launch:
        if (!MissionRules::ToggleLaunch(m_mission))
        {
            return false;
        }
        if (m_mission.Launching())
        {
            PlayNamed("World/shuttle_alarm", ConsoleOf(m_missionProps.Crawler()), 0.9f);
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
            if (m_missionProps.Crawler().Aboard(at))
            {
                aboard = static_cast<uint8_t>(aboard | (1u << id));
                drive = drive || CarriesDrive(id);
            }
        }
        for (const WorldObjects::Pickup& pickup : m_world.Pickups())
        {
            if (pickup.alive && pickup.item == m_driveItem)
            {
                if (const Transform* where = m_scene.GetTransform(pickup.entity); where != nullptr && m_missionProps.Crawler().Aboard(where->position))
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

void PredationGame::DrawSiteMap()
{
    if (!m_mapOpen || m_screen != Screen::Playing || m_paused || !AtSite() || m_mission.stage == Stage::None)
    {
        return;
    }
    const SitePlan& site = m_facility.Plan();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float side = std::min(viewport->Size.x, viewport->Size.y) * 0.84f;
    const ImVec2 min{viewport->Pos.x + (viewport->Size.x - side) * 0.5f, viewport->Pos.y + (viewport->Size.y - side) * 0.5f};
    const ImVec2 max{min.x + side, min.y + side};
    draw->AddRectFilled(min, max, IM_COL32(8, 10, 12, 215), 4.0f);
    draw->AddRect(min, max, IM_COL32(120, 130, 140, 160), 4.0f);
    const ImU32 label = IM_COL32(170, 178, 186, 230);
    draw->AddText({min.x + 12.0f, max.y - 24.0f}, IM_COL32(120, 128, 136, 200),
                  ("[" + KeyFor(m_app->GetInput(), "map") + "] close").c_str());

    if (!m_missionPlan.mapGiven)
    {
        // Nothing was given: nothing to show but that.
        const char* text = "NO MAP DATA FOR THIS SITE";
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText({(min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f}, label, text);
        return;
    }

    // North up; the open ground and a margin round it.
    constexpr float kMargin = 10.0f;
    const float scale = side / (site.size + kMargin * 2.0f);
    const auto at = [&](float x, float z)
    {
        return ImVec2{min.x + (x - site.origin.x + kMargin) * scale, min.y + (z - site.origin.z + kMargin) * scale};
    };
    draw->AddRect(at(site.origin.x, site.origin.z), at(site.origin.x + site.size, site.origin.z + site.size), IM_COL32(60, 66, 72, 160));
    // North, at the top.
    draw->AddTriangleFilled({(min.x + max.x) * 0.5f, min.y + 6.0f}, {(min.x + max.x) * 0.5f - 5.0f, min.y + 15.0f},
                            {(min.x + max.x) * 0.5f + 5.0f, min.y + 15.0f}, label);
    draw->AddText({(min.x + max.x) * 0.5f + 9.0f, min.y + 4.0f}, label, "N");

    // Each building, a floor of it: the one you are on in the building you are in, the ground floor of the rest.
    const glm::vec3 you = m_player.State().position;
    for (size_t b = 0; b < site.buildings.size(); ++b)
    {
        const FacilityLayout& building = site.buildings[b];
        int floor = 0;
        glm::vec2 lo;
        glm::vec2 hi;
        SitePlan::Footprint(building, 0.0f, lo, hi);
        if (you.x > lo.x && you.x < hi.x && you.z > lo.y && you.z < hi.y)
        {
            floor = std::clamp(static_cast<int>(std::floor((you.y - building.origin.y + 0.5f) / FacilityLayout::kStorey)), 0, building.floors - 1);
        }
        for (int z = 0; z < building.depth; ++z)
        {
            for (int x = 0; x < building.width; ++x)
            {
                ImU32 colour = 0;
                switch (building.At(floor, x, z))
                {
                case FacilityLayout::Cell::Room: colour = IM_COL32(74, 80, 88, 255); break;
                case FacilityLayout::Cell::Corridor: colour = IM_COL32(52, 56, 62, 255); break;
                case FacilityLayout::Cell::Stair: colour = IM_COL32(96, 92, 70, 255); break;
                case FacilityLayout::Cell::Solid:
                case FacilityLayout::Cell::Duct: break;
                }
                if (colour == 0)
                {
                    continue;
                }
                const float cx = building.origin.x + static_cast<float>(x) * FacilityLayout::kCell;
                const float cz = building.origin.z + static_cast<float>(z) * FacilityLayout::kCell;
                draw->AddRectFilled(at(cx, cz), at(cx + FacilityLayout::kCell, cz + FacilityLayout::kCell), colour);
            }
        }
        draw->AddRect(at(lo.x, lo.y), at(hi.x, hi.y), IM_COL32(140, 148, 156, 200), 0.0f, 0, 1.5f);
        const std::string name = floor == 0 ? "ground floor" : FloorName(floor);
        draw->AddText({at(lo.x, lo.y).x + 3.0f, at(lo.x, lo.y).y - 16.0f}, IM_COL32(120, 128, 136, 220), name.c_str());
        // The data, where it is, and which floor when it is not the one shown.
        if (static_cast<int>(b) == m_missionPlan.building && m_mission.stage != Stage::Carry)
        {
            const ImVec2 mark = at(m_missionPlan.terminal.x, m_missionPlan.terminal.z);
            draw->AddRectFilled({mark.x - 4.0f, mark.y - 4.0f}, {mark.x + 4.0f, mark.y + 4.0f}, IM_COL32(120, 220, 160, 255));
            const std::string text = m_missionPlan.floor == floor ? "DATA" : "DATA, " + FloorName(m_missionPlan.floor);
            draw->AddText({mark.x + 7.0f, mark.y - 7.0f}, IM_COL32(120, 220, 160, 255), text.c_str());
        }
    }

    // The crawler, where it is parked.
    if (m_missionProps.Crawler().Built())
    {
        const glm::vec3 parked = m_missionProps.Crawler().Home().position;
        const ImVec2 mark = at(parked.x, parked.z);
        draw->AddRectFilled({mark.x - 4.0f, mark.y - 4.0f}, {mark.x + 4.0f, mark.y + 4.0f}, IM_COL32(220, 150, 90, 255));
        draw->AddText({mark.x + 7.0f, mark.y - 7.0f}, IM_COL32(220, 150, 90, 240), "CRAWLER");
    }
    // The shuttle.
    const glm::vec3 base = site.ShuttleBase();
    const ImVec2 pad = at(base.x, base.z);
    draw->AddCircle(pad, 7.0f * scale + 3.0f, IM_COL32(150, 170, 200, 220), 24, 1.5f);
    draw->AddText({pad.x + 7.0f * scale + 6.0f, pad.y - 7.0f}, IM_COL32(150, 170, 200, 230), "SHUTTLE");

    // Everybody else, and you, pointing the way you look.
    for (const RemotePlayerView& other : RemotePlayers())
    {
        if (other.alive)
        {
            draw->AddCircleFilled(at(other.position.x, other.position.z), 4.0f, IM_COL32(210, 190, 110, 255));
        }
    }
    if (m_player.State().alive)
    {
        const ImVec2 centre = at(you.x, you.z);
        const glm::vec2 ahead{std::sin(m_lookYaw), -std::cos(m_lookYaw)};
        const glm::vec2 right{-ahead.y, ahead.x};
        const auto point = [&](glm::vec2 offset) { return ImVec2{centre.x + offset.x, centre.y + offset.y}; };
        draw->AddTriangleFilled(point(ahead * 9.0f), point(-ahead * 5.0f + right * 5.0f), point(-ahead * 5.0f - right * 5.0f),
                                IM_COL32(235, 240, 245, 255));
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
            ImGui::TextColored(text, "%s", mine ? "You made it out." : "You were left behind.");
            ImGui::TextDisabled("%d of %d aboard", std::popcount(static_cast<unsigned>(m_mission.aboard)), everybody);
            const float left = MissionSpec::kResultSeconds - m_missionOverFor;
            if (left > 0.0f && !m_missionLeaving)
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
            if (m_missionPlan.mapGiven)
            {
                ImGui::TextDisabled("The terminal: %s, %s.", Bearing(m_player.State().position, m_missionPlan.terminal).c_str(),
                                    FloorName(m_missionPlan.floor).c_str());
                if (!m_mapOpen)
                {
                    ImGui::TextDisabled("[%s] site map", KeyFor(m_app->GetInput(), "map").c_str());
                }
            }
            else
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
            ImGui::TextColored(text, "Take the drive to the crawler.");
            ImGui::TextDisabled("The crawler: %s.", Bearing(m_player.State().position, m_missionProps.Crawler().Home().position).c_str());
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
            ImGui::TextColored(warning, "The crawler leaves in %d. Anybody not aboard is left behind.",
                               static_cast<int>(std::ceil(m_mission.launchIn)));
        }
    }
    ImGui::End();
}


// --- Arriving, the intercom, and deploying -------------------------------------------------------

namespace
{

constexpr glm::vec3 kDeployConsoleSize{1.2f, 1.05f, 0.6f};

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
    m_deployBody = m_app->GetPhysics().CreateBox(kDeployConsoleSize * 0.5f, transform, BodyMotion::Static);
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
    interactable.focusOffset = transform.rotation * glm::vec3(0.0f, kDeployConsoleSize.y * 0.5f, -0.1f);
    interactable.range = 2.2f;
    m_interactions.Register(interactable);
}

void PredationGame::OpenBriefing(uint16_t seed)
{
    m_nextSite = seed == 0 ? 1 : seed;
    m_nextTitle = m_siteNames.For(m_nextSite);
    m_nextMapGiven = MissionPlan::Generate(SitePlan::Generate(m_nextSite), m_nextSite).mapGiven;
    m_briefingOpen = true;
    m_wantMouseCaptured = false;
    PlayNamed("UI/click", m_renderEye, 0.5f, 1.0f, false);
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
    const SiteTitle title = m_siteNames.For(m_facility.Seed());
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

void PredationGame::DrawBriefing()
{
    if (!m_briefingOpen)
    {
        return;
    }
    // Walked away from the console, or not in a game any more: closed.
    const Transform* console = m_scene.GetTransform(m_deployConsole);
    if (m_screen != Screen::Playing || !IsAuthority() || !m_player.State().alive || console == nullptr ||
        glm::distance(console->position, m_player.State().position) > 4.0f)
    {
        m_briefingOpen = false;
        m_wantMouseCaptured = m_screen == Screen::Playing;
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.45f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.88f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##Briefing", nullptr, kFlags))
    {
        ImGui::TextColored({0.62f, 0.66f, 0.7f, 1.0f}, "NEXT DEPLOYMENT");
        ImGui::Spacing();
        ImGui::TextDisabled("%s", m_nextTitle.planet.c_str());
        ImGui::SetWindowFontScale(1.3f);
        ImGui::TextColored({0.9f, 0.92f, 0.94f, 1.0f}, "%s", m_nextTitle.site.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::TextColored({0.86f, 0.88f, 0.9f, 1.0f}, "Objective: download the data from a terminal on the site,");
        ImGui::TextColored({0.86f, 0.88f, 0.9f, 1.0f}, "and bring the drive back aboard the crawler.");
        ImGui::TextDisabled("Site map: %s", m_nextMapGiven ? "on file" : "none on file");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Deploy", {120.0f, 0.0f}))
        {
            m_briefingOpen = false;
            m_wantMouseCaptured = true;
            // A site that has been done is gone back to as it was; another is planned.
            if (m_nextSite != m_facility.Seed() || m_mission.stage == MissionState::Stage::Over)
            {
                ChangeFacility(m_nextSite);
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
        ImGui::SameLine();
        if (ImGui::Button("Another site", {120.0f, 0.0f}))
        {
            OpenBriefing(static_cast<uint16_t>(1 + (m_nextSite * 40503u + 7919u) % 65535u));
        }
        ImGui::SameLine();
        if (ImGui::Button("Not yet", {120.0f, 0.0f}))
        {
            m_briefingOpen = false;
            m_wantMouseCaptured = true;
        }
    }
    ImGui::End();
}

} // namespace pred
