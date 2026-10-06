// The campaign as the game plays it: choosing or beginning one on the title, the host keeping it and saving it, and
// everybody else being sent it. What a campaign is, is Game/Campaign.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"

#include <imgui.h>

#include <glm/geometric.hpp>

#include <chrono>
#include <cmath>
#include <ctime>
#include <random>
#include <string>

namespace pred
{

namespace
{

// The game sends what everybody else has of the campaign this long after it last changed, so a burst of changes is
// one sending.
constexpr float kCampaignSendDelay = 0.25f;
// And keeps the clock in step this often in between, which is all that changes on its own.
constexpr float kCampaignClockEvery = 10.0f;

std::string PlayedFor(double seconds)
{
    const int minutes = static_cast<int>(seconds / 60.0);
    if (minutes < 60)
    {
        return std::to_string(minutes) + " min";
    }
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min";
}

std::string SavedAgo(int64_t savedAt)
{
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t ago = std::max<int64_t>(now - savedAt, 0);
    if (ago < 60)
    {
        return "just now";
    }
    if (ago < 3600)
    {
        return std::to_string(ago / 60) + " min ago";
    }
    if (ago < 86400)
    {
        return std::to_string(ago / 3600) + " h ago";
    }
    return std::to_string(ago / 86400) + " days ago";
}

uint64_t FreshSeed()
{
    std::random_device device;
    const uint64_t high = static_cast<uint64_t>(device()) << 32;
    const uint64_t low = static_cast<uint64_t>(device());
    const uint64_t time = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    // Kept to a number a person can read out and type: nine digits.
    return (MixSeed(high | low, time) % 900000000ull) + 100000000ull;
}

} // namespace

void PredationGame::LoadUniverseData()
{
    m_campaignStore = std::make_unique<CampaignStore>();
    std::string error;
    const auto file = Paths::Resolve("Data/universe.json");
    if (!file || !m_universeData.LoadFromFile(*file, &error))
    {
        PRED_LOG_WARN(Gameplay, "Universe data not read ({}): using the built-in set", file ? error : std::string("no Data/universe.json"));
        m_universeData.UseDefaults();
    }
    else
    {
        PRED_LOG_INFO(Gameplay, "Universe data: {} biomes, {} atmospheres, {} weathers, {} stars", m_universeData.biomes.size(),
                      m_universeData.atmospheres.size(), m_universeData.weathers.size(), m_universeData.stars.size());
    }
    m_universe.Reset(0, &m_universeData);
}

bool PredationGame::OpenCampaign(const std::string& folder, std::string& error)
{
    CampaignState loaded;
    if (!m_campaignStore->Load(folder, loaded, &error))
    {
        return false;
    }
    m_campaign = std::move(loaded);
    m_campaignFolder = folder;
    m_campaignOpen = true;
    m_universe.Reset(m_campaign.universeSeed, &m_universeData);
    m_campaignDirty = true;
    PRED_LOG_INFO(Gameplay, "Campaign '{}' opened from {} (seed {}, {} played)", m_campaign.name, folder, m_campaign.universeSeed,
                  PlayedFor(m_campaign.played));
    return true;
}

bool PredationGame::BeginCampaign(const std::string& name, uint64_t seed, std::string& error)
{
    const std::string title = name.empty() ? std::string("Campaign") : name;
    m_universe.Reset(seed, &m_universeData);
    m_campaign = CampaignState::Begin(title, seed, m_universe);
    if (m_universe.System(m_universe.Home()) == nullptr)
    {
        error = "the universe could not be made";
        return false;
    }
    m_campaignFolder = m_campaignStore->NewFolder(title);
    m_campaignOpen = true;
    m_campaignDirty = true;
    // On disk at once, so it is in the list even if the game goes no further.
    m_campaignStore->Save(m_campaignFolder, m_campaign, false);
    m_campaignSlotsRead = false;
    PRED_LOG_INFO(Gameplay, "Campaign '{}' begun in {} from seed {}", title, m_campaignFolder, seed);
    return true;
}

void PredationGame::CloseCampaign()
{
    if (m_campaignOpen && m_sessionMode != SessionMode::Client && !m_campaignFolder.empty())
    {
        SaveCampaign(true, "leaving");
        m_campaignStore->Finish();
    }
    m_campaignOpen = false;
    m_campaignFolder.clear();
    m_campaign = CampaignState{};
    m_mapOpen = false;
    m_mapFramed = false;
    m_mapSelected = -1;
    m_mapSharedSet = false;
    m_mapSerialSeen = {-1, -1, -1, -1};
    m_mapMovedBy = -1;
    m_appliedTravels = 0;
    m_travelSeen = false;
    m_campaignSlotsRead = false;
}

void PredationGame::SaveCampaign(bool autosave, const char* why)
{
    if (!m_campaignOpen || m_sessionMode == SessionMode::Client || m_campaignFolder.empty())
    {
        return;
    }
    m_campaignStore->Save(m_campaignFolder, m_campaign, autosave);
    m_campaignNotice = autosave ? "Autosaved" : "Saved";
    m_campaignNoticeFor = 2.5f;
    m_campaignNoticeCentred = false;
    PRED_LOG_INFO(Gameplay, "Campaign {} ({})", autosave ? "autosaved" : "saved", why);
}

void PredationGame::CampaignChanged()
{
    if (!m_campaignDirty)
    {
        m_campaignDirty = true;
        m_campaignSendIn = kCampaignSendDelay;
    }
}

void PredationGame::SendCampaign(int player)
{
    if (m_sessionMode != SessionMode::Host || !m_campaignOpen)
    {
        return;
    }
    m_host.SendDocument(player, DocumentKind::Campaign, m_campaign.ToJson().dump());
}

void PredationGame::ApplyCampaignDocument(const std::string& text)
{
    CampaignState state;
    std::string error;
    try
    {
        if (!CampaignState::FromJson(nlohmann::json::parse(text), state, &error))
        {
            PRED_LOG_WARN(Network, "The host's campaign could not be read: {}", error);
            return;
        }
    }
    catch (const std::exception& exception)
    {
        PRED_LOG_WARN(Network, "The host's campaign could not be read: {}", exception.what());
        return;
    }
    if (!m_campaignOpen || state.universeSeed != m_campaign.universeSeed || m_universe.Data() == nullptr)
    {
        m_universe.Reset(state.universeSeed, &m_universeData);
    }
    const bool first = !m_campaignOpen;
    m_campaign = std::move(state);
    m_campaignOpen = true;
    // Joining while the ship stands at an outpost: the host has put this player outside by the stair (ShipArrival), but which
    // way to face was chosen before the campaign came, not knowing the ship was landed -- turned now to face the ship.
    if (first && m_screen == Screen::Playing && m_map == MapChoice::Ship && ShipLanded())
    {
        m_lookYaw = KestrelStation::SpawnYaw();
        m_lookPitch = 0.0f;
        m_player.State().yaw = m_lookYaw;
    }
}

void PredationGame::ServeCampaignRequests()
{
    for (const NetHost::CampaignAsk& ask : m_host.TakeCampaignRequests())
    {
        DoCampaignAction(ask.player, static_cast<CampaignAction>(ask.request.action), ask.request.a, ask.request.b);
    }
}

void PredationGame::UpdateCampaign(float dt)
{
    m_campaignNoticeFor = std::max(m_campaignNoticeFor - dt, 0.0f);
    if (m_sessionMode == SessionMode::Client)
    {
        for (NetClient::Document& document : m_client.TakeDocuments())
        {
            if (document.kind == DocumentKind::Campaign)
            {
                ApplyCampaignDocument(document.text);
            }
        }
    }
    if (!m_campaignOpen || m_screen != Screen::Playing)
    {
        return;
    }
    // Every machine keeps the clock going -- the planets turn by it -- and the host's is the one that counts.
    m_campaign.clock += dt;
    m_campaign.played += dt;
    if (m_sessionMode == SessionMode::Client)
    {
        return;
    }
    if (m_sessionMode != SessionMode::Host)
    {
        m_campaignDirty = false;
        return;
    }
    ServeCampaignRequests();
    m_campaignSendIn -= dt;
    m_campaignClockIn -= dt;
    if ((m_campaignDirty && m_campaignSendIn <= 0.0f) || m_campaignClockIn <= 0.0f)
    {
        SendCampaign();
        m_campaignDirty = false;
        m_campaignClockIn = kCampaignClockEvery;
    }
}

void PredationGame::ShowCampaignNotice(const std::string& text)
{
    m_campaignNotice = text;
    m_campaignNoticeFor = 3.0f;
    m_campaignNoticeCentred = true;
}

void PredationGame::SetOut()
{
    if (const std::string why = LeavingBlocked(); !why.empty())
    {
        ShowCampaignNotice(why);
        PlayNamed("UI/back", m_renderEye, 0.5f, 0.8f, false);
        return;
    }
    AskCampaign(CampaignAction::Depart);
    PlayNamed("UI/confirm", m_renderEye, 0.6f, 1.0f, false);
}

void PredationGame::DrawCampaignNotice()
{
    if (m_campaignNoticeFor <= 0.0f || m_campaignNotice.empty())
    {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float alpha = std::min(m_campaignNoticeFor / 0.6f, 1.0f);
    const ImVec2 size = ImGui::CalcTextSize(m_campaignNotice.c_str());
    const ImVec2 at = m_campaignNoticeCentred ? ImVec2{viewport->WorkPos.x + (viewport->WorkSize.x - size.x) * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.62f}
                                              : ImVec2{viewport->WorkPos.x + viewport->WorkSize.x - size.x - 24.0f, viewport->WorkPos.y + 20.0f};
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddText({at.x + 1.0f, at.y + 1.0f}, IM_COL32(0, 0, 0, static_cast<int>(180.0f * alpha)), m_campaignNotice.c_str());
    draw->AddText(at, IM_COL32(220, 226, 230, static_cast<int>(230.0f * alpha)), m_campaignNotice.c_str());
}

bool PredationGame::OpenChosenCampaign(std::string& error)
{
    if (m_campaignChoice >= 0 && m_campaignChoice < static_cast<int>(m_campaignSlots.size()))
    {
        return OpenCampaign(m_campaignSlots[static_cast<size_t>(m_campaignChoice)].folder, error);
    }
    uint64_t seed = 0;
    if (m_newCampaignSeed[0] != '\0')
    {
        try
        {
            seed = std::stoull(m_newCampaignSeed);
        }
        catch (const std::exception&)
        {
            seed = 0;
        }
    }
    if (seed == 0)
    {
        seed = FreshSeed();
    }
    return BeginCampaign(m_newCampaignName, seed, error);
}

void PredationGame::RegisterCampaignCommands()
{
    Console& console = m_app->GetConsole();
    console.RegisterCommand("campaign", "How the campaign stands: its name, seed, credits and where the ship is",
                            [this](const std::vector<std::string>&)
                            {
                                Console& out = m_app->GetConsole();
                                if (!m_campaignOpen)
                                {
                                    out.Print("No campaign is open.");
                                    return;
                                }
                                const StarSystem* system = m_universe.System(m_campaign.system);
                                const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
                                out.Print("'" + m_campaign.name + "', seed " + std::to_string(m_campaign.universeSeed) + ", " +
                                          std::to_string(m_campaign.credits) + " credits, " + PlayedFor(m_campaign.played) + " played");
                                out.Print("At " + (body != nullptr ? body->name : system != nullptr ? system->name : std::string("nowhere")) +
                                          (m_campaignFolder.empty() ? std::string(" (the host's)") : ", saved in " + m_campaignFolder));
                                out.Print(std::to_string(m_campaign.log.size()) + " log entries, " + std::to_string(m_campaign.known.size()) +
                                          " bodies known about");
                            });
    console.RegisterCommand("campaign_save", "Save the campaign now (the host)",
                            [this](const std::vector<std::string>&)
                            {
                                if (!m_campaignOpen || m_campaignFolder.empty())
                                {
                                    m_app->GetConsole().PrintError("There is no campaign of this machine's to save.");
                                    return;
                                }
                                SaveCampaign(false, "asked for");
                                const std::string error = m_campaignStore->Finish();
                                m_app->GetConsole().Print(error.empty() ? "Saved." : "Not saved: " + error);
                            });
    console.RegisterCommand("universe", "The system the ship is in (or home, or 'universe <seed>' for another universe's home)",
                            [this](const std::vector<std::string>& args)
                            {
                                Console& out = m_app->GetConsole();
                                // In the log as well, so a run without a window can be read.
                                const auto say = [&out](const std::string& line)
                                {
                                    out.Print(line);
                                    PRED_LOG_INFO(Gameplay, "{}", line);
                                };
                                Universe scratch;
                                Universe* universe = &m_universe;
                                uint64_t systemId = m_campaignOpen ? m_campaign.system : 0;
                                if (args.size() >= 2)
                                {
                                    scratch.Reset(std::stoull(args[1]), &m_universeData);
                                    universe = &scratch;
                                    systemId = 0;
                                }
                                const StarSystem* system = universe->System(systemId);
                                if (system == nullptr)
                                {
                                    out.PrintError("No system there.");
                                    return;
                                }
                                const StarDef* star = nullptr;
                                for (const StarDef& def : m_universeData.stars)
                                {
                                    star = def.id == system->star ? &def : star;
                                }
                                say(system->name + ": " + (star != nullptr ? star->name : system->star) + ", " +
                                          std::to_string(system->bodies.size()) + " bodies");
                                for (const Body& body : system->bodies)
                                {
                                    std::string line = (body.kind == BodyKind::Moon ? "    " : "  ") + body.name + "  " + body.biome;
                                    if (!body.gas)
                                    {
                                        line += ", " + body.atmosphere + " air, " + body.terrain + ", " + body.weather + ", " + body.civilization;
                                    }
                                    line += ", " + std::to_string(static_cast<int>(body.temperature)) + " C";
                                    for (const std::string& special : body.specials)
                                    {
                                        line += ", " + special;
                                    }
                                    if (body.index == system->hub)
                                    {
                                        line += "  [settled, its hub is where the ship lands]";
                                    }
                                    say(line);
                                    for (const LandingRegion& region : body.regions)
                                    {
                                        say("        " + region.designation + " (" + region.kind + (region.charted ? ", charted" : "") + ")");
                                    }
                                }
                            });
#if PRED_DEV_TOOLS
    console.RegisterCommand("credits", "Set the campaign's credits (the host): credits <amount>",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!m_campaignOpen || m_sessionMode == SessionMode::Client || args.size() < 2)
                                {
                                    m_app->GetConsole().PrintError("Usage, on the host with a campaign open: credits <amount>");
                                    return;
                                }
                                m_campaign.credits = std::stoll(args[1]);
                                CampaignChanged();
                            });
    // Another world's outpost round the ship where it stands, for looking at without crossing to it: outpost_preview <seed>, or
    // with no seed back to the one that is really there.
    console.RegisterCommand("outpost_preview", "Show the outpost planned from a seed round the landed ship: outpost_preview [seed] [owner]",
                            [this](const std::vector<std::string>& args)
                            {
                                m_outpostPreview = args.size() >= 2 ? static_cast<uint32_t>(std::stoul(args[1])) : 0u;
                                m_outpostPreviewOwner = args.size() >= 3 ? args[2] : std::string();
                            });
    console.RegisterCommand("site_terrain", "Build the next site on this ground rather than its world's: site_terrain [flat|rolling|mountainous|canyons|cratered|dunes], none for the world's own",
                            [this](const std::vector<std::string>& args) { m_siteTerrainPreview = args.size() >= 2 && args[1] != "none" ? args[1] : std::string(); });
    console.RegisterCommand("land_region", "Pick where the shuttle goes down on the world the ship is over, as the map does: land_region <region>",
                            [this](const std::vector<std::string>& args) { ChooseLandingRegion(args.size() >= 2 ? std::atoi(args[1].c_str()) : 0); });
    console.RegisterCommand("orbit_here", "Put the ship in orbit of the body it is landed on, without the cinematic (the host)",
                            [this](const std::vector<std::string>&)
                            {
                                if (!m_campaignOpen || m_sessionMode == SessionMode::Client)
                                {
                                    return;
                                }
                                Travel::Lift(m_campaign);
                                m_campaign.doorOpen = false;
                                CampaignChanged();
                            });
    console.RegisterCommand("day_skip", "Move the campaign's clock on by part of the day where the ship is (the host): day_skip <fraction>",
                            [this](const std::vector<std::string>& args)
                            {
                                const StarSystem* system = CurrentSystem();
                                const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
                                if (!m_campaignOpen || m_sessionMode == SessionMode::Client || args.size() < 2 || body == nullptr)
                                {
                                    m_app->GetConsole().PrintError("Usage, on the host at a body: day_skip <fraction of its day>");
                                    return;
                                }
                                m_campaign.clock += static_cast<double>(body->day) * std::stod(args[1]);
                                CampaignChanged();
                            });
    console.RegisterCommand("map_open",
                            "Open the navigation map, as the console does: map_open [galaxy | <body to pick out> | body <body> [area]]",
                            [this](const std::vector<std::string>& args)
                            {
                                OpenSystemMap();
                                if (!m_mapOpen || args.size() < 2)
                                {
                                    return;
                                }
                                if (args[1] == "galaxy")
                                {
                                    ApplyMapGalaxy(Travel::GalaxyPosition(m_campaign, m_universe), args.size() >= 3 ? std::stof(args[2]) : 60.0f);
                                }
                                else if (args[1] == "body" && args.size() >= 3)
                                {
                                    ApplyMapSystem(m_campaign.system, -1);
                                    ApplyMapBody(std::atoi(args[2].c_str()));
                                    if (args.size() >= 4)
                                    {
                                        m_mapRegion = std::atoi(args[3].c_str());
                                    }
                                }
                                else
                                {
                                    ApplyMapSystem(m_campaign.system, std::atoi(args[1].c_str()));
                                }
                            });
    console.RegisterCommand("course_system", "Set a course for another system, the nth nearest the ship: course_system [n]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!m_campaignOpen)
                                {
                                    return;
                                }
                                const int n = args.size() >= 2 ? std::max(std::atoi(args[1].c_str()), 1) : 1;
                                int seen = 0;
                                for (const SystemId& id : m_universe.Near(Travel::GalaxyPosition(m_campaign, m_universe), 80.0f))
                                {
                                    if (id.Packed() != m_campaign.system && ++seen == n)
                                    {
                                        AskSystemCourse(id.Packed());
                                        return;
                                    }
                                }
                            });
    console.RegisterCommand("map_close", "Close the system map", [this](const std::vector<std::string>&) { CloseSystemMap(); });
    console.RegisterCommand("course", "Set a course for a body of this system, as the map does: course <body> [region]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (args.size() >= 2)
                                {
                                    AskCampaign(CampaignAction::SetCourse, std::atoi(args[1].c_str()), args.size() >= 3 ? std::atoi(args[2].c_str()) : -1);
                                }
                            });
    // Most of the way there at once, for trying arriving: travel_skip.
    console.RegisterCommand("travel_skip", "Under way, put the ship almost at its destination (the host)",
                            [this](const std::vector<std::string>&)
                            {
                                const StarSystem* system = CurrentSystem();
                                if (system != nullptr && IsAuthority() && m_campaign.travel.interstellar)
                                {
                                    // A crossing: all but the last few seconds of it.
                                    m_campaign.travel.departed = m_campaign.clock - std::max(m_campaign.travel.duration - 3.0f, 0.0f);
                                    return;
                                }
                                if (system == nullptr || !IsAuthority() || !m_campaign.travel.underway || m_campaign.travel.target < 0)
                                {
                                    return;
                                }
                                const glm::vec3 there = system->Position(m_campaign.travel.target, m_campaign.clock);
                                m_campaign.travel.position =
                                    glm::dvec3(there) + glm::dvec3(glm::normalize(glm::vec3(m_campaign.travel.position - glm::dvec3(there)) + glm::vec3(1e-4f)) * 0.004f);
                                m_campaign.travel.velocity = glm::vec3(0.0f);
                            });
    // A campaign to try things in, without the title: campaign_new [name] [seed].
    console.RegisterCommand("campaign_new", "Begin a campaign here, without the title: campaign_new [name] [seed]",
                            [this](const std::vector<std::string>& args)
                            {
                                std::string error;
                                const uint64_t seed = args.size() >= 3 ? std::stoull(args[2]) : FreshSeed();
                                if (!BeginCampaign(args.size() >= 2 ? args[1] : std::string("Test"), seed, error))
                                {
                                    m_app->GetConsole().PrintError(error);
                                }
                            });
#endif
}

} // namespace pred
