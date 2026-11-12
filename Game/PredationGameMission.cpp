// The mission, as the game runs it: planned from the site whenever the site is built, run by the host -- the terminal,
// the breaker, the download, the drive, the launch -- and shown on every machine. The plan and the rules are in
// Game/Mission/Mission.h; the terminal, the panels and the console in the world are MissionProps.

#include "Game/PredationGame.h"
#include "Game/PredationGameShared.h"

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

const char* WorkedName(MissionRules::Worked worked)
{
    switch (worked)
    {
    case MissionRules::Worked::Opened: return "opened";
    case MissionRules::Worked::Switched: return "switched";
    case MissionRules::Worked::Tripped: return "tripped";
    case MissionRules::Worked::Done: return "done";
    case MissionRules::Worked::Nothing: break;
    }
    return "nothing";
}

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
    const unsigned hands = (state.boardOpen ? 1u : 0u) | (state.boardMain ? 2u : 0u) | (static_cast<unsigned>(state.boardOn & MissionSpec::kBoardAll) << 2) |
                           (static_cast<unsigned>(state.device & 0xFu) << 8);
    event.direction = glm::vec3(static_cast<float>(state.readings), static_cast<float>(state.reading), static_cast<float>(hands));
    event.reserve = state.found ? 1 : 0; // (and a cut feed's cabinet over it: below)
    // (A secured terminal's keypad: what is typed, how many figures, and whether it has let them in.)
    // (And with it, a faulted terminal's supply and its box, packed.)
    const unsigned box = (state.unlocked ? 1u : 0u) | (state.supplied ? 2u : 0u) | (state.boxIsolator ? 4u : 0u) | (state.boxSpared ? 8u : 0u) |
                         (static_cast<unsigned>(state.boxScrews & 0xFu) << 4) | (static_cast<unsigned>(state.boxFuses & MissionSpec::kFusesAll) << 8);
    event.position = glm::vec3(static_cast<float>(state.typed), static_cast<float>(state.typedCount), static_cast<float>(box));
    // (And a cut feed's cabinet, over the terminal found.)
    event.reserve |= static_cast<uint16_t>(((state.fed ? 1u : 0u) | (state.cabinetOpen ? 2u : 0u) | (state.feedMain ? 4u : 0u) | (state.feedStandby ? 8u : 0u) |
                                        (static_cast<unsigned>(std::min<uint8_t>(state.changeover, 2)) << 4) | (state.changeoverBack ? 64u : 0u)) << 1);
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
    state.readings = static_cast<uint8_t>(event.direction.x);
    state.reading = static_cast<int>(event.direction.y);
    const unsigned hands = static_cast<unsigned>(event.direction.z);
    state.boardOpen = (hands & 1u) != 0u;
    state.boardMain = (hands & 2u) != 0u;
    state.boardOn = static_cast<uint8_t>((hands >> 2) & MissionSpec::kBoardAll);
    state.device = static_cast<uint8_t>((hands >> 8) & 0xFu);
    state.found = (event.reserve & 1u) != 0u;
    const unsigned cabinet = static_cast<unsigned>(event.reserve >> 1);
    state.typed = static_cast<uint16_t>(std::clamp(event.position.x, 0.0f, 9999.0f));
    state.typedCount = static_cast<uint8_t>(std::clamp(event.position.y, 0.0f, 4.0f));
    const unsigned box = static_cast<unsigned>(std::clamp(event.position.z, 0.0f, 65535.0f));
    state.unlocked = (box & 1u) != 0u;
    state.supplied = (box & 2u) != 0u;
    state.boxIsolator = (box & 4u) != 0u;
    state.boxSpared = (box & 8u) != 0u;
    state.boxScrews = static_cast<uint8_t>((box >> 4) & 0xFu);
    state.boxFuses = static_cast<uint8_t>((box >> 8) & MissionSpec::kFusesAll);
    state.fed = (cabinet & 1u) != 0u;
    state.cabinetOpen = (cabinet & 2u) != 0u;
    state.feedMain = (cabinet & 4u) != 0u;
    state.feedStandby = (cabinet & 8u) != 0u;
    state.changeover = static_cast<uint8_t>(std::min((cabinet >> 4) & 3u, 2u));
    state.changeoverBack = (cabinet & 64u) != 0u;
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

std::string FloorName(int floor, int ground)
{
    const int up = floor - ground;
    return up == 0 ? "on the ground floor" : up == 1 ? "one floor up" : up > 1 ? std::to_string(up) + " floors up" : up == -1 ? "in the basement" : std::to_string(-up) + " floors down";
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
                                              m_missionPlan.building, FloorName(m_missionPlan.floor, (m_missionPlan.building >= 0 && m_missionPlan.building < static_cast<int>(m_facility.Plan().buildings.size()) ? m_facility.Plan().buildings[static_cast<size_t>(m_missionPlan.building)].ground : 0)).c_str(), m_mission.powered ? "on" : "OUT",
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
                                else if (where == "breaker" && MissionBreakerAt(at, facing))
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
                                SeatAboard(aboard);
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
    console.RegisterCommand("mission_closeup", "Bring the camera up to a mission device, as pressing it does: mission_closeup <board|console|transmitter|terminal|keypad|note|fusebox|cabinet>",
                            [this](const std::vector<std::string>& args)
                            {
                                const std::string which = args.size() >= 2 ? args[1] : "";
                                const MissionProps::Device device = which == "board"         ? MissionProps::Device::Board
                                                                    : which == "console"     ? MissionProps::Device::Console
                                                                    : which == "transmitter" ? MissionProps::Device::Transmitter
                                                                    : which == "terminal"    ? MissionProps::Device::ShipTerminal
                                                                    : which == "keypad"      ? MissionProps::Device::Keypad
                                                                    : which == "note"        ? MissionProps::Device::Note
                                                                    : which == "fusebox"     ? MissionProps::Device::FuseBox
                                                                    : which == "cabinet"     ? MissionProps::Device::Cabinet
                                                                                             : MissionProps::Device::None;
                                glm::vec3 eye;
                                glm::vec3 target;
                                if (device == MissionProps::Device::None || !DeviceView(device, eye, target))
                                {
                                    m_app->GetConsole().PrintError("usage: mission_closeup <board|console|transmitter|terminal|keypad|note|fusebox|cabinet> (one this site has)");
                                    return;
                                }
                                EnterDevice(device);
                            });
    console.RegisterCommand("terminal_press", "Press a point on the ship's terminal's screen, as a click there does: terminal_press <x> <y> (0-767, 0-455)",
                            [this](const std::vector<std::string>& args)
                            {
                                if (args.size() < 3)
                                {
                                    m_app->GetConsole().PrintError("usage: terminal_press <x> <y>");
                                    return;
                                }
                                PressTerminal(m_shipTerminal.Hit(std::atoi(args[1].c_str()), std::atoi(args[2].c_str())));
                            });
    console.RegisterCommand("mission_work",
                            "As the host, work a mission thing as if pressed: mission_work <board 0-7|console 0-3|transmitter 0-2|marker n|keypad 0-11|fusebox 0-8|cabinet 0-3>",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!IsAuthority() || args.size() < 3)
                                {
                                    m_app->GetConsole().PrintError("usage: mission_work <board|console|transmitter|marker> <control>");
                                    return;
                                }
                                const int control = std::atoi(args[2].c_str());
                                bool done = false;
                                if (args[1] == "board")
                                {
                                    done = PerformMissionInteraction(InteractionKind::Breaker, control, LocalPlayerId());
                                }
                                else if (args[1] == "console" || args[1] == "transmitter")
                                {
                                    done = PerformMissionInteraction(InteractionKind::Terminal, control, LocalPlayerId());
                                }
                                else if (args[1] == "cabinet")
                                {
                                    done = PerformMissionInteraction(InteractionKind::Terminal, MissionRules::kCabinetPayload + control, LocalPlayerId());
                                }
                                else if (args[1] == "fusebox")
                                {
                                    done = PerformMissionInteraction(InteractionKind::Terminal, MissionRules::kBoxPayload + control, LocalPlayerId());
                                }
                                else if (args[1] == "keypad")
                                {
                                    done = PerformMissionInteraction(InteractionKind::Terminal, MissionRules::kKeyPayload + control, LocalPlayerId());
                                }
                                else if (args[1] == "marker")
                                {
                                    done = PerformMissionInteraction(InteractionKind::Terminal, control * 4, LocalPlayerId());
                                }
                                if (!done)
                                {
                                    m_app->GetConsole().Print("Nothing came of it");
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
    // (A survey's markers stood on the ground where they are: the same ground on every machine.)
    for (glm::vec3& marker : m_missionPlan.markers)
    {
        marker.y = m_facility.Terrain().Height(marker.x, marker.z);
    }
    if (m_missionPlan.feedCut)
    {
        m_missionPlan.cabinet.y = m_facility.Terrain().Height(m_missionPlan.cabinet.x, m_missionPlan.cabinet.z);
    }
    // Nobody is left up close to a device that is gone.
    if (DeviceOpen())
    {
        m_device = MissionProps::Device::None;
        m_deviceBlend = 0.0f;
        m_deviceLeaving = false;
        m_wantMouseCaptured = true;
        UpdateMouseCapture();
    }
    m_mission = MissionRules::Start(m_missionPlan);
    LayObjectives();
    m_missionProps.Build(m_scene, m_app->GetMeshes(), &m_app->GetTextures(), m_app->GetPhysics(), m_interactions, m_facility.Plan(), m_missionPlan, m_facility.Shuttle());
    // And what can be taken out of the ground there (Resources).
    BuildDeposits();
    RegisterSeats(1);
    BuildLifts();
    m_driveItem = m_items.IdOf("data_drive");
    // The shuttle's tool case (Phase 5): a multitool and a circuit tester on its right-hand bench, for a crew that came without.
    if (IsAuthority() && m_missionPlan.Valid())
    {
        PutToolsInShuttle();
    }
    m_missionSendIn = 0.0f;
    m_missionHumIn = kHumEvery;
    m_missionOverFor = 0.0f;
    m_missionDocked = false;
    m_missionFoundNoPower = false;
    m_missionSeen = m_mission;
    m_foundNoPowerSeen = false;
    m_missionLeaving = false;
    AttachVehicleLamps();
    ShowMission();
    if (m_missionPlan.kind == MissionPlan::Kind::Survey)
    {
        PRED_LOG_INFO(Gameplay, "Mission: a survey, {} markers", m_missionPlan.markers.size());
    }
    else if (m_missionPlan.AtTransmitter())
    {
        PRED_LOG_INFO(Gameplay, "Mission: a signal{}, its transmitter at {:.1f} {:.1f} {:.1f}", m_missionPlan.kind == MissionPlan::Kind::Revive ? " gone silent, to bring back" : ", to shut down", m_missionPlan.markers.front().x, m_missionPlan.markers.front().y,
                      m_missionPlan.markers.front().z);
    }
    else if (m_missionPlan.Valid())
    {
        PRED_LOG_INFO(Gameplay, "Mission: {} a terminal in building {}, {}, at {:.1f} {:.1f} {:.1f}{}{}; {:.0f} s at it",
                      m_missionPlan.kind == MissionPlan::Kind::Restore ? "the station is restarted at" : m_missionPlan.kind == MissionPlan::Kind::Activate ? "the plant is started at" : "the data is on",
                      m_missionPlan.building, FloorName(m_missionPlan.floor, (m_missionPlan.building >= 0 && m_missionPlan.building < static_cast<int>(m_facility.Plan().buildings.size()) ? m_facility.Plan().buildings[static_cast<size_t>(m_missionPlan.building)].ground : 0)), m_missionPlan.terminal.x, m_missionPlan.terminal.y,
                      m_missionPlan.terminal.z, m_missionPlan.powerOut ? ", its power out" : "", m_missionPlan.mapGiven ? "" : ", no map",
                      m_missionPlan.downloadSeconds);
        if (m_missionPlan.Secured())
        {
            PRED_LOG_INFO(Gameplay, "Mission: the terminal is secured, its code on a note at {:.1f} {:.1f} {:.1f}", m_missionPlan.clue.x, m_missionPlan.clue.y, m_missionPlan.clue.z);
        }
        if (m_missionPlan.feedCut)
        {
            PRED_LOG_INFO(Gameplay, "Mission: its feed is cut, the feeder cabinet at {:.1f} {:.1f} {:.1f}", m_missionPlan.cabinet.x, m_missionPlan.cabinet.y, m_missionPlan.cabinet.z);
        }
        if (m_missionPlan.Faulted())
        {
            PRED_LOG_INFO(Gameplay, "Mission: its supply is faulted, fuse F{} blown in the box at {:.1f} {:.1f} {:.1f}", m_missionPlan.blownFuse + 1, m_missionPlan.fuseBox.x, m_missionPlan.fuseBox.y, m_missionPlan.fuseBox.z);
        }
    }
}

void PredationGame::LayObjectives()
{
    const Objectives::Template* source = m_objectiveLibrary.Find(Objectives::TemplateFor(m_missionPlan.kind));
    m_objectives = source != nullptr && m_missionPlan.Valid() ? Objectives::Lay(*source, m_missionPlan) : Objectives::Graph{};
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
    m_missionProps.SetColourSafe(cv_colourSafe.Get());
    m_missionProps.Show(m_scene, m_interactions, &m_app->GetTextures(), m_mission, static_cast<float>(m_lightClock));
    // The building's lamps, each circuit through its breaker on the board: lit as each comes back on, all of them lit without a
    // mission.
    for (size_t c = 0; c < m_missionPlan.circuits.size(); ++c)
    {
        const bool powered = m_mission.stage == Stage::None || MissionRules::BreakerLive(m_mission, MissionRules::BreakerOf(m_missionPlan, c));
        const int circuit = m_missionPlan.circuits[c];
        if (m_levelLights.Powered(circuit) != powered)
        {
            m_levelLights.SetPowered(circuit, powered);
        }
    }
}

void PredationGame::PutToolsInShuttle()
{
    // On the right-hand bench in its cabin, a little above it, to settle there: one of each tool the work on a site wants.
    const Entity bench = m_facility.Shuttle().Part("bench_right");
    const Transform* at = bench.IsValid() ? m_scene.GetTransform(bench) : nullptr;
    if (at == nullptr)
    {
        return;
    }
    const glm::vec3 along = at->rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    float offset = -0.2f;
    for (const char* key : {"multitool", "circuit_tester"})
    {
        const ItemId item = m_items.IdOf(key);
        if (item == kInvalidItem)
        {
            continue;
        }
        const glm::vec3 where = at->position + glm::vec3(0.0f, 0.35f, 0.0f) + along * offset;
        offset += 0.4f;
        const int index = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, item, 1, where, glm::vec3(0.0f), -1, -1);
        if (m_sessionMode == SessionMode::Host && index >= 0)
        {
            m_host.Broadcast(PickupSpawnedEvent(static_cast<uint8_t>(index), static_cast<uint16_t>(item), 1, LoadToWire(-1), LoadToWire(-1), where, glm::vec3(0.0f)));
        }
    }
    PRED_LOG_INFO(Gameplay, "The shuttle's tool case: on its bench at {:.1f} {:.1f} {:.1f}", at->position.x, at->position.y, at->position.z);
}

bool PredationGame::Carries(uint8_t player, ItemId item) const
{
    if (item == kInvalidItem)
    {
        return false;
    }
    if (player == LocalPlayerId())
    {
        return m_inventory.CountOf(item) > 0;
    }
    return m_sessionMode == SessionMode::Host && m_host.CarriedCount(player, static_cast<uint16_t>(item)) > 0;
}

std::string PredationGame::TesterReading(InteractionKind kind, int payload) const
{
    // What a circuit tester held to it reads (Phase 5, diagnosis): a breaker's circuit tested for a short to earth -- the faulted one
    // found before it is thrown, not by its bang -- and a supply box's fuses for continuity, its supply for voltage.
    char line[64];
    if (kind == InteractionKind::Breaker)
    {
        const int breaker = payload - MissionRules::kBoardBreaker;
        if (payload == MissionRules::kBoardMain)
        {
            if (!m_mission.fed)
            {
                return "INCOMING 0 V  NO SUPPLY";
            }
            return m_mission.boardMain ? "INCOMING 400 V  MAIN ON" : "INCOMING 400 V  MAIN OFF";
        }
        if (breaker >= 0 && breaker < MissionSpec::kBoardBreakers)
        {
            std::snprintf(line, sizeof(line), "CIRCUIT %d  %s", breaker + 1, breaker == m_missionPlan.boardFault ? "SHORT TO EARTH" : "INSULATION OK");
            return line;
        }
        return {};
    }
    if (kind == InteractionKind::Terminal && payload >= MissionRules::kCabinetPayload)
    {
        switch (payload - MissionRules::kCabinetPayload)
        {
        case MissionRules::kCabinetMain: return "FEED A  0 V  CABLE FAULT";
        case MissionRules::kCabinetStandby: return "FEED B  400 V";
        case MissionRules::kCabinetChangeover: return m_mission.fed ? "BUS  400 V" : "BUS  0 V";
        default: return {};
        }
    }
    if (kind == InteractionKind::Terminal && payload >= MissionRules::kBoxPayload)
    {
        const int control = payload - MissionRules::kBoxPayload;
        if (control < MissionRules::kBoxFuse)
        {
            return m_mission.boxIsolator ? "SUPPLY LIVE  230 V" : "ISOLATED  0 V";
        }
        const int fuse = control - MissionRules::kBoxFuse;
        const bool in = ((m_mission.boxFuses >> fuse) & 1u) != 0u;
        const bool blown = fuse == m_missionPlan.blownFuse && !m_mission.boxSpared;
        std::snprintf(line, sizeof(line), "F%d  %s", fuse + 1, !in ? "HOLDER OPEN" : blown ? "OPEN CIRCUIT - BLOWN" : "CONTINUITY OK");
        return line;
    }
    return {};
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

bool PredationGame::MissionBreakerAt(glm::vec3& at, float& yaw) const
{
    if (!m_missionPlan.Valid())
    {
        return false;
    }
    return BreakerPanel(m_facility.Plan().buildings[static_cast<size_t>(m_missionPlan.building)], at, yaw);
}

void PredationGame::ApplyMissionEvent(const WorldEventMessage& event)
{
    const MissionState was = m_mission;
    m_mission = MissionFrom(event);
    if (!event.quiet)
    {
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
            PlayNamed("World/shuttle_alarm", ConsoleOf(m_facility.Shuttle()), 0.25f); // (a square wave: loud for its peak)
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
    m_missionDocked = false;
    // The host: what it came to, in the ship's log -- the debrief, the contract there paid, sometimes a lead.
    RecordExpedition();
    // What was taken out of the ground, carried aboard: into the hold (PredationGameResources.cpp).
    StowSamples(m_mission.aboard);
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

void PredationGame::EnterDevice(MissionProps::Device device)
{
    glm::vec3 eye;
    glm::vec3 target;
    if (!DeviceView(device, eye, target))
    {
        return;
    }
    m_device = device;
    m_deviceLeaving = false;
    m_deviceDoneFor = 0.0f;
    // The pointer, to work it with by hand.
    m_wantMouseCaptured = false;
    UpdateMouseCapture();
}

void PredationGame::LeaveDevice()
{
    if (!DeviceOpen() || m_deviceLeaving)
    {
        return;
    }
    m_deviceLeaving = true;
    m_missionProps.Hover(m_scene, Entity{});
    m_terminalHover = {-1, -1};
    m_terminalDirty = true;
    m_wantMouseCaptured = true;
    UpdateMouseCapture();
}

void PredationGame::UpdateDevice(float dt)
{
    if (!DeviceOpen() || m_deviceBlend < 1.0f)
    {
        m_testerReading.clear();
    }
    if (!DeviceOpen())
    {
        return;
    }
    // Up to it in half a second, and back as easily.
    m_deviceBlend = m_deviceLeaving ? std::max(m_deviceBlend - dt / 0.5f, 0.0f) : std::min(m_deviceBlend + dt / 0.5f, 1.0f);
    if (m_deviceLeaving)
    {
        if (m_deviceBlend <= 0.0f)
        {
            m_device = MissionProps::Device::None;
            m_deviceLeaving = false;
        }
        return;
    }
    glm::vec3 eye;
    glm::vec3 target;
    if (!DeviceView(m_device, eye, target) || !m_player.State().alive || HeldLocally() || m_hidingSpot >= 0 || m_screen != Screen::Playing)
    {
        LeaveDevice();
        return;
    }
    // Stepping away, or looking away (the sights' button), is leaving it.
    Input& input = m_app->GetInput();
    for (const char* action : {"move_forward", "move_back", "move_left", "move_right", "jump", "aim"})
    {
        if (input.WasActionPressed(action))
        {
            LeaveDevice();
            return;
        }
    }
    // Nothing left to do at it -- the power back, the transmitter down, the station running: a moment to see it, and back.
    const bool terminal = m_device == MissionProps::Device::ShipTerminal;
    if (terminal ? !(m_campaignOpen && m_map == MapChoice::Ship) : !m_missionProps.Workable(m_device, m_mission))
    {
        m_missionProps.Hover(m_scene, Entity{});
        m_deviceDoneFor += dt;
        if (m_deviceDoneFor > 1.4f)
        {
            LeaveDevice();
        }
        return;
    }
    m_deviceDoneFor = 0.0f;
    if (m_deviceBlend < 1.0f || m_paused || m_app->IsConsoleOpen() || ImGui::GetIO().WantCaptureMouse)
    {
        m_missionProps.Hover(m_scene, Entity{});
        return;
    }
    // From the eye through the pointer, in the lens the frame is drawn with: whatever of it is nearest that line is under the pointer.
    Renderer& renderer = m_app->GetRenderer();
    const float width = static_cast<float>(std::max<int>(renderer.Width(), 1));
    const float height = static_cast<float>(std::max<int>(renderer.Height(), 1));
    const glm::vec2 pointer = input.MousePosition();
    const float halfHorizontal = std::tan(glm::radians(cv_fov.Get()) * 0.5f);
    const float halfVertical = halfHorizontal / (width / height);
    const float x = (pointer.x / width) * 2.0f - 1.0f;
    const float y = 1.0f - (pointer.y / height) * 2.0f;
    const glm::quat turn = glm::quatLookAtRH(glm::normalize(target - eye), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 direction = glm::normalize(turn * glm::vec3(x * halfHorizontal, y * halfVertical, -1.0f));
    // The ship's terminal: a point on its screen, not a thing on it.
    if (terminal)
    {
        TerminalPointer(eye, direction, input.WasActionPressed("fire"), input.WheelDelta());
        return;
    }
    const InteractionSystem::Focus& focus = m_interactions.UpdateFocus(m_scene, m_app->GetPhysics(), eye, direction, 5.0f, true);
    const bool on = focus.valid && m_missionProps.DeviceOf(focus.entity) == m_device;
    m_missionProps.Hover(m_scene, on ? focus.entity : Entity{});
    m_testerReading = on && Carries(LocalPlayerId(), m_items.IdOf("circuit_tester")) ? TesterReading(focus.kind, focus.payload) : std::string();
    if (on && input.WasActionPressed("fire"))
    {
        WorkControl(focus.kind, focus.payload);
    }
}

bool PredationGame::DeviceView(MissionProps::Device device, glm::vec3& eye, glm::vec3& target) const
{
    return device == MissionProps::Device::ShipTerminal ? TerminalPose(eye, target) : m_missionProps.View(device, eye, target);
}

void PredationGame::DeviceCamera(glm::mat4& view, glm::vec3& eye) const
{
    glm::vec3 to;
    glm::vec3 target;
    if (!DeviceOpen() || m_deviceBlend <= 0.0f || !DeviceView(m_device, to, target))
    {
        return;
    }
    // Eased from wherever the eye is to the device and back, turning as it goes.
    const float blend = m_deviceBlend * m_deviceBlend * (3.0f - 2.0f * m_deviceBlend);
    const glm::mat4 world = glm::inverse(view);
    const glm::quat from = glm::normalize(glm::quat_cast(glm::mat3(world)));
    const glm::quat toTurn = glm::quatLookAtRH(glm::normalize(target - to), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 at = glm::mix(glm::vec3(world[3]), to, blend);
    const glm::quat turn = glm::slerp(from, toTurn, blend);
    view = glm::inverse(glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(turn));
    eye = at;
}

void PredationGame::WorkControl(InteractionKind kind, int payload)
{
    // A supply box's screws, with no multitool to take them out with: nothing to ask the host.
    const int boxControl = payload - MissionRules::kBoxPayload;
    if (kind == InteractionKind::Terminal && boxControl >= MissionRules::kBoxScrew && boxControl < MissionRules::kBoxIsolator &&
        !Carries(LocalPlayerId(), m_items.IdOf("multitool")))
    {
        m_deviceNote = "The cover's screws need a multitool.";
        m_deviceNoteFor = 3.0f;
        PlayNamed("World/door_locked", m_missionPlan.fuseBox, 0.5f);
        return;
    }
    // A station's console with no power: its switches only click, and the objective says what to do about it. (Not the supply
    // box's controls, or the keypad's: they are worked with the terminal dead.)
    if (kind == InteractionKind::Terminal && payload < MissionRules::kKeyPayload && !(m_mission.powered && m_mission.supplied) && !m_missionPlan.AtMarkers())
    {
        m_missionFoundNoPower = true;
        PlayNamed("World/terminal_dead", m_missionPlan.terminal, 0.7f);
        return;
    }
    if (m_sessionMode == SessionMode::Client)
    {
        m_client.SendInteract(static_cast<uint8_t>(kind), static_cast<uint8_t>(payload));
        return;
    }
    PerformInteraction(kind, payload, LocalPlayerId());
}

bool PredationGame::SoundWorked(MissionRules::Worked worked, const glm::vec3& at, uint8_t player, const char* opened)
{
    // Heard by everybody near it (ShareSound), and by whatever listens: a switch is a click, a door or a lid a creak, and
    // something thrown wrongly a bang and a crack heard well beyond the room.
    switch (worked)
    {
    case MissionRules::Worked::Nothing: return false;
    case MissionRules::Worked::Opened:
        ShareSound(opened, at, 0.8f);
        MakeNoise(NoiseKind::Item, at, kHumReach, player);
        break;
    case MissionRules::Worked::Switched: ShareSound("World/switch", at, 0.7f); break;
    case MissionRules::Worked::Tripped:
        ShareSound("World/spark", at, 1.0f);
        ShareSound("World/breaker", at, 0.8f);
        MakeNoise(NoiseKind::Door, at, kBreakerReach, player);
        break;
    case MissionRules::Worked::Done:
        ShareSound("World/switch", at, 0.7f);
        ShareSound("World/breaker", at, 1.0f);
        MakeNoise(NoiseKind::Door, at, kBreakerReach, player);
        break;
    }
    return true;
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
        if (m_missionPlan.AtMarkers())
        {
            // A marker (its payload: which, four to a marker, and which of its controls): a survey's reading started, or a signal's
            // transmitter worked by hand.
            const int marker = index / 4;
            const int control = index % 4;
            if (marker >= static_cast<int>(m_missionPlan.markers.size()))
            {
                return false;
            }
            const glm::vec3 foot = m_missionPlan.markers[static_cast<size_t>(marker)];
            if (m_missionPlan.AtTransmitter())
            {
                const MissionRules::Worked worked = MissionRules::WorkTransmitter(m_mission, m_missionPlan, control);
                if (!SoundWorked(worked, foot + glm::vec3(0.0f, 0.35f, 0.0f), player, "World/crate_open"))
                {
                    return false;
                }
                PRED_LOG_INFO(Gameplay, "Player {} worked the transmitter ({}): {}", player, control, WorkedName(worked));
                break;
            }
            if (!MissionRules::StartReading(m_mission, m_missionPlan, marker))
            {
                return false;
            }
            PlayNamed("World/terminal_beep", foot + glm::vec3(0.0f, 1.2f, 0.0f), 0.7f);
            MakeNoise(NoiseKind::Item, foot, kHumReach, player);
            PRED_LOG_INFO(Gameplay, "Player {} started the reading at marker {}", player, marker);
            break;
        }
        if (index >= MissionRules::kCabinetPayload)
        {
            // A cut feed's cabinet: its door, its two feeds' switches and its changeover.
            const MissionRules::Worked worked = MissionRules::WorkCabinet(m_mission, m_missionPlan, index - MissionRules::kCabinetPayload);
            if (!SoundWorked(worked, m_missionPlan.cabinet + glm::vec3(0.0f, 0.9f, 0.0f), player, "World/locker"))
            {
                return false;
            }
            PRED_LOG_INFO(Gameplay, "Player {} worked the feeder cabinet ({}): {}{}", player, index - MissionRules::kCabinetPayload, WorkedName(worked),
                          worked == MissionRules::Worked::Done ? (m_mission.powered ? ", the building is fed and its power back" : ", the building is fed") : "");
            break;
        }
        if (index >= MissionRules::kBoxPayload)
        {
            // A faulted terminal's supply box: its cover's screws (only with a multitool), its isolator, its fuse holders.
            const bool tool = Carries(player, m_items.IdOf("multitool"));
            const MissionRules::Worked worked = MissionRules::WorkBox(m_mission, m_missionPlan, index - MissionRules::kBoxPayload, tool);
            if (!SoundWorked(worked, m_missionPlan.fuseBox, player, "World/crate_open"))
            {
                return false;
            }
            PRED_LOG_INFO(Gameplay, "Player {} worked the supply box ({}): {}{}", player, index - MissionRules::kBoxPayload, WorkedName(worked),
                          worked == MissionRules::Worked::Done ? ", the terminal has its supply" : "");
            break;
        }
        if (index >= MissionRules::kKeyPayload)
        {
            // A secured terminal's keypad: a key's beep, a refusal's buzz, and the chime of being let in.
            const MissionRules::Worked worked = MissionRules::WorkKeypad(m_mission, m_missionPlan, index - MissionRules::kKeyPayload);
            switch (worked)
            {
            case MissionRules::Worked::Nothing: return false;
            case MissionRules::Worked::Tripped: ShareSound("World/terminal_dead", m_missionPlan.terminal, 0.8f); break;
            case MissionRules::Worked::Done: ShareSound("World/terminal_done", m_missionPlan.terminal, 0.8f); break;
            case MissionRules::Worked::Opened:
            case MissionRules::Worked::Switched: ShareSound("World/terminal_beep", m_missionPlan.terminal, 0.35f); break;
            }
            PRED_LOG_INFO(Gameplay, "Player {} pressed the keypad's {}: {}", player, index - MissionRules::kKeyPayload,
                          worked == MissionRules::Worked::Done ? "let in" : worked == MissionRules::Worked::Tripped ? "refused" : "typed");
            break;
        }
        if (index > 0)
        {
            // A station's console: one of its life-support switches (the payload, from 1).
            const MissionRules::Worked worked = MissionRules::WorkConsole(m_mission, m_missionPlan, index - 1);
            if (!SoundWorked(worked, m_missionPlan.terminal, player, ""))
            {
                return false;
            }
            m_missionHumIn = kHumEvery;
            PRED_LOG_INFO(Gameplay, "Player {} worked the console's switch {}: {}", player, index, WorkedName(worked));
            break;
        }
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
        // The board of the building whose power is out: its door, its main switch, or one of its breakers (the payload).
        glm::vec3 panel;
        float yaw = 0.0f;
        if (!MissionBreakerAt(panel, yaw))
        {
            return false;
        }
        const MissionRules::Worked worked = MissionRules::WorkBoard(m_mission, m_missionPlan, index);
        if (!SoundWorked(worked, panel, player, "World/locker"))
        {
            return false;
        }
        PRED_LOG_INFO(Gameplay, "Player {} worked the breaker board ({}): {}{}", player, index, WorkedName(worked),
                      worked == MissionRules::Worked::Done ? ", the power is back" : "");
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

    case InteractionKind::Launch:
        if (!MissionRules::ToggleLaunch(m_mission))
        {
            return false;
        }
        if (m_mission.Launching())
        {
            PlayNamed("World/shuttle_alarm", ConsoleOf(m_facility.Shuttle()), 0.25f); // (a square wave: loud for its peak)
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
    // A signal's transmitter, while it sends: a beep with each flash of its lamp, to be heard from near it (on every machine, off the
    // same clock as the lamp).
    if (m_missionPlan.AtTransmitter() && m_mission.stage != Stage::Over && (m_mission.device & MissionState::kSenderOff) == 0 && !m_missionPlan.markers.empty() && AtSite() &&
        std::fmod(m_lightClock, 1.6) < static_cast<double>(dt))
    {
        PlayNamed("World/terminal_beep", m_missionPlan.markers.front() + glm::vec3(0.0f, 0.5f, 0.0f), 0.3f);
    }
    if (m_mission.stage == Stage::Over)
    {
        // The result on screen for a while, and then everybody is back aboard the ship -- the testing area, until
        // there is a ship.
        // (Back aboard -- docked, or brought there with no shuttle to fly: the result is up from now.)
        if (!m_missionDocked && m_map == MapChoice::Ship && !m_cine.Active())
        {
            m_missionDocked = true;
            m_missionOverFor = 0.0f;
            // (That site is done with: the shuttle does not set out for it again because somebody is still sitting in it.)
            m_shipReady = false;
        }
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
    // At it: the terminal -- or, a survey's, the marker whose reading is being taken -- somebody within reach of it.
    const bool survey = m_missionPlan.AtMarkers();
    const glm::vec3 attendAt = survey && m_mission.reading >= 0 && m_mission.reading < static_cast<int>(m_missionPlan.markers.size())
                                   ? m_missionPlan.markers[static_cast<size_t>(m_mission.reading)] + glm::vec3(0.0f, 1.0f, 0.0f)
                                   : m_missionPlan.terminal;
    const float reach = survey ? MissionSpec::kReadingReach : MissionSpec::kAttendReach;
    for (const auto& [id, at] : living)
    {
        const glm::vec2 apart{at.x - attendAt.x, at.z - attendAt.z};
        attended = attended || (glm::length(apart) < reach && std::abs(at.y - attendAt.y) < 2.2f);
    }
    const MissionState was = m_mission;
    const MissionRules::Ticked ticked = MissionRules::Tick(m_mission, m_missionPlan, dt, attended);
    bool changed = was.attended != m_mission.attended || was.stage != m_mission.stage;
    // Where there was no map, the terminal is found once somebody has been in the room with it: the tracker points at it after.
    if (!m_mission.found && !m_missionPlan.AtMarkers() && m_missionPlan.building >= 0)
    {
        for (const auto& [id, at] : living)
        {
            const glm::vec2 apart{at.x - m_missionPlan.terminal.x, at.z - m_missionPlan.terminal.z};
            if (glm::length(apart) < 6.0f && std::abs(at.y - m_missionPlan.terminal.y) < 2.5f)
            {
                m_mission.found = true;
                changed = true;
            }
        }
    }

    if (m_mission.stage == Stage::Downloading && m_mission.attended)
    {
        m_missionHumIn -= dt;
        if (m_missionHumIn <= 0.0f)
        {
            m_missionHumIn = kHumEvery;
            ShareSound("World/terminal_working", attendAt, 0.6f);
            MakeNoise(NoiseKind::Item, attendAt, kHumReach, -1);
        }
    }
    if (ticked.read)
    {
        // (Where it was taken: the marker's lamp gone green -- ShowMission.)
        ShareSound("World/terminal_done", attendAt, 0.8f);
        PRED_LOG_INFO(Gameplay, "A reading is taken: {} of {}", std::popcount(static_cast<unsigned>(m_mission.readings)), m_missionPlan.markers.size());
    }
    if (ticked.downloaded)
    {
        PlayNamed("World/terminal_done", m_missionPlan.terminal, 0.8f);
        if (m_missionPlan.AtConsole())
        {
            PRED_LOG_INFO(Gameplay, "{}", m_missionPlan.kind == MissionPlan::Kind::Restore ? "The station's life support is restarted" : "The facility's plant is running");
        }
        else
        {
            // Out onto the bench in front of it, for whoever is to carry it.
            DropIntoWorld(m_driveItem, 1, -1, -1, m_missionPlan.driveAt, glm::vec3(0.0f));
            PRED_LOG_INFO(Gameplay, "The download is done: the drive is on the bench");
        }
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
        // (Nothing to carry but the download's drive: a survey's readings, a station restored or a signal shut down are done once
        // done, and go with whoever is aboard.)
        if (m_missionPlan.kind != MissionPlan::Kind::Download)
        {
            drive = m_mission.stage == Stage::Carry && aboard != 0;
        }
        // Everybody aboard into a seat for the flight up.
        SeatAboard(aboard);
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
    if ((!over && !AtSite()) || (over && (!m_missionDocked || m_missionOverFor > MissionSpec::kResultSeconds + 5.0f)))
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
            // (Headed as the mission kind's template has it: missions.json.)
            const Objectives::Template* kind = m_objectives.source;
            const std::string title = kind == nullptr ? std::string(m_mission.recovered ? "MISSION COMPLETE" : "MISSION FAILED") : m_mission.recovered ? kind->done : kind->failed;
            ImGui::TextColored(m_mission.recovered ? ImVec4{0.5f, 0.85f, 0.6f, 1.0f} : warning, "%s", title.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextColored(text, "%s", mine ? "You made it out." : m_mission.aboard == 0 ? "Nobody made it out." : "You were left behind.");
            ImGui::TextDisabled("%d of %d aboard", std::popcount(static_cast<unsigned>(m_mission.aboard)), everybody);
        }
        ImGui::End();
        return;
    }

    // Up close to a device: what a circuit tester reads off the part under the pointer, and anything wanted to work it.
    m_deviceNoteFor = std::max(m_deviceNoteFor - ImGui::GetIO().DeltaTime, 0.0f);
    if (DeviceOpen() && (!m_testerReading.empty() || m_deviceNoteFor > 0.0f))
    {
        ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.8f}, ImGuiCond_Always, {0.5f, 0.5f});
        if (ImGui::Begin("##DeviceReading", nullptr, kHud & ~ImGuiWindowFlags_NoBackground))
        {
            if (!m_testerReading.empty())
            {
                ImGui::TextColored(heading, "CIRCUIT TESTER");
                ImGui::SetWindowFontScale(1.4f);
                ImGui::TextColored({1.0f, 0.78f, 0.3f, 1.0f}, "%s", m_testerReading.c_str());
                ImGui::SetWindowFontScale(1.0f);
            }
            if (m_deviceNoteFor > 0.0f)
            {
                ImGui::TextColored(warning, "%s", m_deviceNote.c_str());
            }
        }
        ImGui::End();
    }

    // The objective, top left, under the microphone meter when there is one.
    ImGui::SetNextWindowPos({viewport->Pos.x + 16.0f, viewport->Pos.y + 44.0f}, ImGuiCond_Always);
    if (ImGui::Begin("##Mission", nullptr, kHud))
    {
        ImGui::TextColored(heading, "OBJECTIVE");
        // What there is to do now, as the mission's steps have it (Objectives): those open, each with the line under it -- and
        // how a step under way is going (a download's progress, whether anybody is at it; whether this player has the drive).
        // Not where: finding it is the searching, the map and the tracker's.
        Objectives::Update(m_objectives, m_mission, m_missionFoundNoPower);
        for (const int index : m_objectives.Open())
        {
            const Objectives::Step& step = m_objectives.steps[static_cast<size_t>(index)];
            // One worked at something while somebody stays there: a download or a restart at the terminal, a reading at a marker,
            // a transmitter shut down. (Counted where there is more than one of them: a survey's markers.)
            const bool readings = step.def->kind == Objectives::Kind::Readings;
            const bool worked = readings || step.def->kind == Objectives::Kind::Download;
            const int all = static_cast<int>(m_missionPlan.markers.size());
            const std::string count =
                readings && all > 1 ? "  (" + std::to_string(std::popcount(static_cast<unsigned>(m_mission.readings))) + " of " + std::to_string(all) + " taken)"
                                    : std::string();
            if (worked && m_mission.stage == Stage::Downloading)
            {
                const std::string& working = step.def->working.empty() ? step.def->text : step.def->working;
                ImGui::TextColored(text, "%s: %d%%%s", working.c_str(), static_cast<int>(m_mission.progress * 100.0f), count.c_str());
                if (m_mission.attended && !step.def->stay.empty())
                {
                    ImGui::TextDisabled("%s", step.def->stay.c_str());
                }
                else if (!m_mission.attended && !step.def->paused.empty())
                {
                    ImGui::TextColored(warning, "%s", step.def->paused.c_str());
                }
                continue;
            }
            ImGui::TextColored(text, "%s%s", Objectives::TextOf(step).c_str(), count.c_str());
            if (const std::string hint = Objectives::HintOf(step, m_missionPlan); !hint.empty())
            {
                ImGui::TextDisabled("%s", hint.c_str());
            }
            if (step.def->kind == Objectives::Kind::Carry && m_driveItem != kInvalidItem && m_inventory.CountOf(m_driveItem) > 0)
            {
                ImGui::TextDisabled("You have the drive.");
            }
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
    // What missions are for, step by step: laid over the mission there is again whenever it is read (its steps point into it).
    const std::filesystem::path missions = Paths::AssetsRoot() / "Data" / "missions.json";
    if (!m_objectiveLibrary.LoadFromFile(missions, &error))
    {
        PRED_LOG_WARN(Gameplay, "Missions: {}", error);
    }
    m_app->GetFileWatcher().Watch(missions, [this](const std::filesystem::path& path)
                                  {
                                      std::string why;
                                      if (m_objectiveLibrary.LoadFromFile(path, &why))
                                      {
                                          LayObjectives();
                                      }
                                      else
                                      {
                                          PRED_LOG_WARN(Gameplay, "Missions: {}", why);
                                      }
                                  });
    // What contracts there are and pay (Assets/Data/contracts.json).
    LoadContractRules();
    LoadResourceRules();
    // All of them are written by hand while the game is running, so all are read again when they change.
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
    interactable.name = "";
    interactable.focusOffset = glm::vec3(0.0f, kDeployConsoleSize.y * 0.5f, -0.1f);
    interactable.range = 2.2f;
    m_interactions.Register(interactable);
}

void PredationGame::BuildAirlockControls()
{
    // A small lit panel either side of the boarding door: inside, on the wall aft of it; outside, on the hull.
    for (int i = 0; i < 2; ++i)
    {
        const CinePose at = m_ship.AirlockControl(i == 0);
        Transform transform;
        transform.position = at.position;
        transform.rotation = at.rotation;
        Material panel = Material::Diffuse({0.08f, 0.08f, 0.09f}, 0.4f);
        panel.emissive = {0.5f, 0.3f, 0.06f};
        m_airlockControls[i] = m_scene.CreateMeshEntity(i == 0 ? "airlock_control_in" : "airlock_control_out", transform,
                                                        m_app->GetMeshes().Upload(Primitives::Box({0.22f, 0.32f, 0.05f}), "airlock_control"), panel);
        Interactable interactable;
        interactable.entity = m_airlockControls[i];
        interactable.kind = InteractionKind::Airlock;
        interactable.verb = "Open";
        interactable.name = "";
        interactable.range = 1.8f;
        interactable.enabled = false;
        m_interactions.Register(interactable);
    }
}

void PredationGame::BuildHelm()
{
    // A panel on the helm's top, between the two seats, where the course is set out on: its screen lying in it, the top of
    // its picture away from the seats (DrawHelmScreen).
    const CinePose at = m_ship.Helm();
    Transform transform;
    transform.position = at.position;
    transform.rotation = at.rotation;
    MeshLibrary& meshes = m_app->GetMeshes();
    m_helm = m_scene.CreateMeshEntity("helm_panel", transform, meshes.Upload(Primitives::Box({1.24f, 0.03f, 0.5f}), "helm_panel"),
                                      Material::Metal({0.1f, 0.1f, 0.11f}, 0.5f));
    m_helmTexture = m_app->GetTextures().CreateDynamic(512, 192, "helm_screen");
    {
        MeshData quad;
        const float w = 0.56f;
        const float h = 0.21f;
        const glm::vec3 normal{0.0f, 0.0f, 1.0f};
        quad.vertices.push_back(MeshVertex{{-w, h, 0.0f}, normal, {0.0f, 0.0f}});
        quad.vertices.push_back(MeshVertex{{w, h, 0.0f}, normal, {1.0f, 0.0f}});
        quad.vertices.push_back(MeshVertex{{w, -h, 0.0f}, normal, {1.0f, 1.0f}});
        quad.vertices.push_back(MeshVertex{{-w, -h, 0.0f}, normal, {0.0f, 1.0f}});
        quad.indices = {0, 3, 2, 0, 2, 1};
        Transform screen = transform;
        screen.position += glm::vec3(0.0f, 0.017f, 0.0f);
        // Laid flat, facing up, the picture's top towards the bow.
        screen.rotation = transform.rotation * glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        Material glass = Material::Diffuse(glm::vec3(1.0f), 0.3f);
        glass.baseColorTexture = m_helmTexture;
        glass.emissive = glm::vec3(1.2f);
        glass.emissiveTextured = true;
        m_helmScreen = m_scene.CreateMeshEntity("helm_screen", screen, meshes.Upload(quad, "helm_screen"), glass);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_helmScreen))
        {
            renderer->castsShadow = false;
        }
    }
    Interactable interactable;
    interactable.entity = m_helm;
    interactable.kind = InteractionKind::Helm;
    interactable.verb = "Set out";
    interactable.name = "";
    interactable.range = 2.0f;
    interactable.enabled = false;
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
    // Arriving without a cinematic to show it (one says its own first line). (What the site is, is on the shuttle's screen.)
    if (atSite && !m_wasAtSite && !m_cine.Active())
    {
        Say(m_missionPlan.mapGiven ? "arrival" : "arrival_no_map", 2.0f);
    }
    m_wasAtSite = atSite;

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

std::vector<std::string> PredationGame::LandingBrief() const
{
    std::vector<std::string> lines;
    const CampaignState::Contract* contract =
        m_campaignOpen && m_campaign.travel.region >= 0 ? ContractAt(m_campaign.system, m_campaign.body, m_campaign.travel.region) : nullptr;
    if (contract != nullptr)
    {
        lines.push_back("CONTRACT: " + m_contractRules.Wanted(contract->kind) + " Pays " + std::to_string(contract->reward) + " credits.");
    }
    else if (const std::vector<int> open = m_objectives.Open(); !open.empty())
    {
        lines.push_back(m_objectives.steps[static_cast<size_t>(open.front())].def->text);
    }
    if (!m_missionPlan.mapGiven)
    {
        lines.push_back("No map data for this site.");
    }
    return lines;
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
