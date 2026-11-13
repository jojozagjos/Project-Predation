// The campaign as the game plays it: choosing or beginning one on the title, the host keeping it and saving it, and
// everybody else being sent it. What a campaign is, is Game/Campaign.

#include "Game/PredationGame.h"
#include "Game/OverlayLayer.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"

#include <imgui.h>

#include <glm/geometric.hpp>

#include <chrono>
#include <cmath>
#include <ctime>
#include <optional>
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

// test_site's kinds of place: what each word asks for.
const char* TestSiteKindName(SitePlan::SiteKind kind)
{
    switch (kind)
    {
    case SitePlan::SiteKind::Facility: return "facility";
    case SitePlan::SiteKind::Station: return "station";
    case SitePlan::SiteKind::Survey: return "survey";
    case SitePlan::SiteKind::Wreck: return "wreck";
    case SitePlan::SiteKind::Signal: return "signal";
    }
    return "facility";
}

struct TestSiteType
{
    const char* word;
    SitePlan::FacilityType type;
};
constexpr TestSiteType kTestSiteTypes[] = {
    {"lab", SitePlan::FacilityType::ResearchLab},    {"mining", SitePlan::FacilityType::Mining},     {"refinery", SitePlan::FacilityType::Refinery},
    {"depot", SitePlan::FacilityType::CargoDepot},   {"habitat", SitePlan::FacilityType::Habitat},   {"relay", SitePlan::FacilityType::CommsRelay},
    {"observatory", SitePlan::FacilityType::Observatory}, {"camp", SitePlan::FacilityType::SurveyCamp}};
constexpr SitePlan::SiteKind kTestSiteKinds[] = {SitePlan::SiteKind::Facility, SitePlan::SiteKind::Station, SitePlan::SiteKind::Survey,
                                                 SitePlan::SiteKind::Wreck, SitePlan::SiteKind::Signal};

std::vector<std::string> TestSiteWords()
{
    std::vector<std::string> words{"any"};
    for (const SitePlan::SiteKind kind : kTestSiteKinds)
    {
        words.emplace_back(TestSiteKindName(kind));
    }
    for (const TestSiteType& type : kTestSiteTypes)
    {
        words.emplace_back(type.word);
    }
    for (const char* feature : {"cave", "ruin", "bunker"})
    {
        words.emplace_back(feature);
    }
    return words;
}

std::string TestSiteKinds()
{
    std::string all;
    for (const std::string& word : TestSiteWords())
    {
        all += (all.empty() ? "" : " ") + word;
    }
    return all;
}

// The seed of a site of that kind, from `base` on: a kind of place keeps the seed's own layout (its low bits) and sets its kind
// (the top ones); what it is for, or something it has (a cave, a half-fallen building, a basement), is looked for in the seeds
// after it. "any": the seed as given, or a kind picked at random.
std::optional<uint16_t> TestSiteSeed(const std::string& word, uint16_t base, bool seedGiven)
{
    if (word == "any")
    {
        return seedGiven ? base : SitePlan::SeedFor(base, kTestSiteKinds[base % std::size(kTestSiteKinds)]);
    }
    for (const SitePlan::SiteKind kind : kTestSiteKinds)
    {
        if (word == TestSiteKindName(kind))
        {
            return SitePlan::SeedFor(base, kind);
        }
    }
    for (const TestSiteType& type : kTestSiteTypes)
    {
        if (word == type.word)
        {
            for (uint32_t step = 0; step < 8192u; ++step)
            {
                const auto layout = static_cast<uint16_t>((base + step) & 0x1FFFu);
                for (const SitePlan::SiteKind kind : kTestSiteKinds)
                {
                    const uint16_t seed = SitePlan::SeedFor(layout, kind);
                    if (SitePlan::TypeOf(seed, kind) == type.type)
                    {
                        return seed;
                    }
                }
            }
            return std::nullopt;
        }
    }
    if (word == "cave" || word == "ruin" || word == "bunker")
    {
        // (A few hundred plans at most: the planning alone, nothing built.)
        for (uint32_t step = 0; step < 400u; ++step)
        {
            const uint16_t seed = SitePlan::SeedFor(static_cast<uint16_t>((base + step) & 0x1FFFu), SitePlan::SiteKind::Facility);
            const SitePlan plan = SitePlan::Generate(seed);
            bool has = word == "cave" && !plan.caves.empty();
            for (const FacilityLayout& building : plan.buildings)
            {
                has = has || (word == "ruin" && building.ruin.Has()) || (word == "bunker" && building.ground > 0);
            }
            if (has)
            {
                return seed;
            }
        }
    }
    return std::nullopt;
}

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
    const OverlayLayer layer("##notice");
    ImDrawList* draw = layer.Draw();
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
    // The site test commands change only the site this machine builds: in a game with others theirs would not match it, so
    // they are for a game on your own.
    const auto soloOnly = [this]
    {
        if (m_sessionMode != SessionMode::Offline)
        {
            m_app->GetConsole().PrintError("Site test commands change only this machine's site: use them in a game on your own.");
            return false;
        }
        return true;
    };
    console.RegisterCommand("site_terrain", "Build the next site on this ground rather than its world's: site_terrain [flat|rolling|mountainous|canyons|cratered|dunes|icesheet|glacial|lavaplain|karst|highlands|mesas|pillars|crystalfields], none for the world's own",
                            [this, soloOnly](const std::vector<std::string>& args)
                            {
                                if (soloOnly())
                                {
                                    m_siteTerrainPreview = args.size() >= 2 && args[1] != "none" ? args[1] : std::string();
                                }
                            });
    console.RegisterCommand("site_biome", "Build the next site as on another biome: what lies on its ground, its ground and rock: site_biome <id> (universe.json), none for the world's own",
                            [this, soloOnly](const std::vector<std::string>& args)
                            {
                                if (soloOnly())
                                {
                                    m_siteBiomePreview = args.size() >= 2 && args[1] != "none" ? args[1] : std::string();
                                }
                            });
    console.RegisterCommand("site_signs", "Build the next site with the organism's signs this plain: site_signs <0 to 1>, world for the place's own",
                            [this, soloOnly](const std::vector<std::string>& args)
                            {
                                if (soloOnly())
                                {
                                    m_siteSignsPreview = args.size() >= 2 && args[1] != "world" ? std::clamp(std::stof(args[1]), 0.0f, 1.0f) : -1.0f;
                                }
                            });
    console.SetCompleter("site_signs", [](size_t argument) { return argument == 1 ? std::vector<std::string>{"0", "0.2", "0.5", "0.8", "1", "world"} : std::vector<std::string>{}; });
    console.RegisterCommand("site_water", "Build the next site with this water rather than its world's: site_water <none|lakes|coast|island|seabed>, world for the world's own",
                            [this, soloOnly](const std::vector<std::string>& args)
                            {
                                if (soloOnly())
                                {
                                    m_siteWaterPreview = args.size() >= 2 && args[1] != "world" ? args[1] : std::string();
                                }
                            });
    console.RegisterCommand("site_temperature", "Build the next site as this warm, degrees Celsius -- its water ice below -2: site_temperature <degrees>, world for the place's own",
                            [this, soloOnly](const std::vector<std::string>& args)
                            {
                                if (soloOnly())
                                {
                                    m_siteTemperaturePreview = args.size() >= 2 && args[1] != "world" ? std::strtof(args[1].c_str(), nullptr)
                                                                                                      : std::numeric_limits<float>::quiet_NaN();
                                }
                            });
    console.RegisterCommand("site_flood", "Build the next site with a building flooded -- the first with a basement, the water standing somewhere in its basements, or at so many metres over its lowest floor: site_flood <on|off> [metres]",
                            [this, soloOnly](const std::vector<std::string>& args)
                            {
                                if (soloOnly())
                                {
                                    m_siteFloodPreview = args.size() < 2 || args[1] == "on";
                                    m_siteFloodLevelPreview = args.size() >= 3 ? std::strtof(args[2].c_str(), nullptr) : -1.0f;
                                    m_app->GetConsole().Print(std::string("The next site's first building with a basement ") + (m_siteFloodPreview ? "flooded." : "as it comes."));
                                }
                            });
    // Straight to a new site of any kind, from anywhere (the title screen too): no world to find first.
    console.RegisterCommand(
        "test_site",
        "Go straight to a new site of any kind, no world needed: test_site <biome> [flat|rolling|mountainous|canyons|cratered|dunes|islands|icesheet|glacial|lavaplain|karst|highlands|mesas|pillars|crystalfields|any] "
        "[none|lakes|coast|island|seabed|any] [any|facility|station|survey|wreck|signal|lab|mining|refinery|depot|habitat|relay|observatory|camp|cave|ruin|bunker] [seed]",
        [this, soloOnly](const std::vector<std::string>& args)
        {
            std::string biomes;
            for (const BiomeDef& biome : m_universeData.biomes)
            {
                if (!biome.surface.empty())
                {
                    biomes += (biomes.empty() ? "" : " ") + biome.id;
                }
            }
            if (args.size() < 2 || m_universeData.Biome(args[1]) == nullptr)
            {
                m_app->GetConsole().PrintError("Usage: test_site <biome> [ground] [water] [kind] [seed]. Biomes: " + biomes);
                return;
            }
            if (!soloOnly())
            {
                return;
            }
            m_siteBiomePreview = args[1];
            m_siteTerrainPreview = args.size() >= 3 && args[2] != "any" ? args[2] : std::string();
            m_siteWaterPreview = args.size() >= 4 && args[3] != "any" ? args[3] : std::string();
            // What kind of place (its fourth word, if not a number): a kind, what it is for, or something it has; a number there
            // is the seed, as before there was a kind to give.
            const auto numeric = [](const std::string& word) { return !word.empty() && word.find_first_not_of("0123456789") == std::string::npos; };
            const bool kindGiven = args.size() >= 5 && !numeric(args[4]);
            const std::string kind = kindGiven ? args[4] : std::string("any");
            const size_t seedAt = kindGiven ? 5 : 4;
            // A new site each time unless a seed is given, so the same command again shows another.
            const bool seedGiven = args.size() > seedAt;
            const auto base = static_cast<uint16_t>(seedGiven ? std::clamp(std::atoi(args[seedAt].c_str()), 1, 65535)
                                                              : 1 + std::chrono::steady_clock::now().time_since_epoch().count() % 65535);
            const std::optional<uint16_t> found = TestSiteSeed(kind, base, seedGiven);
            if (!found)
            {
                m_app->GetConsole().PrintError("No such kind of site, or none found near that seed: " + kind + ". Kinds: " + TestSiteKinds());
                return;
            }
            const uint16_t seed = *found;
            if (m_screen == Screen::Title)
            {
                StopSession();
                EnterWorld();
            }
            // With no campaign open, a world to stand on all the same -- its sun, its sky and its weather, its clock -- kept
            // in memory only (no folder: nothing is saved, nothing left in the list), and the site at midday on it.
            const bool scratch = !m_campaignOpen;
            if (scratch)
            {
                const uint64_t world = 0x5173u;
                m_universe.Reset(world, &m_universeData);
                m_campaign = CampaignState::Begin("Site test", world, m_universe);
                m_campaignFolder.clear();
                m_campaignOpen = true;
            }
            ChangeFacility(seed);
            GoToMap(MapChoice::Facility);
            if (scratch)
            {
                m_app->GetConsole().Execute("day_hour 11");
            }
            // What was made, and how to get to what it has.
            const SitePlan& plan = m_facility.Plan();
            const SitePlan::SiteKind siteKind = SitePlan::KindOf(seed);
            int ruins = 0;
            int bunkers = 0;
            int flooded = 0;
            for (const FacilityLayout& building : plan.buildings)
            {
                ruins += building.ruin.Has() ? 1 : 0;
                bunkers += building.ground > 0 ? 1 : 0;
                flooded += building.flooded >= 0 ? 1 : 0;
            }
            std::string has;
            const auto count = [&has](int n, const char* one, const char* many, const char* reach)
            {
                if (n > 0)
                {
                    has += (has.empty() ? "" : ", ") + std::to_string(n) + " " + (n > 1 ? many : one) + (*reach != 0 ? std::string(" (") + reach + ")" : std::string());
                }
            };
            count(static_cast<int>(plan.caves.size()), "cave", "caves", "site_cave");
            {
                int joined = 0;
                for (const SitePlan::Cave& cave : plan.caves)
                {
                    joined += static_cast<int>(cave.joins.size());
                }
                count(joined, "building joined to a cave", "buildings joined to caves", "site_cave 0 1");
            }
            count(ruins, "half-fallen building", "half-fallen buildings", "");
            count(bunkers, "building with basements", "buildings with basements", "site_stairs");
            count(flooded, "flooded building", "flooded buildings", "site_flooded");
            const SitePlan::FacilityType siteType = SitePlan::TypeOf(seed, siteKind);
            m_app->GetConsole().Print("Site " + std::to_string(seed) + ": " + TestSiteKindName(siteKind) + (siteType == SitePlan::FacilityType::None ? std::string() : std::string(", ") + SitePlan::TypeName(siteType)) +
                                      ", " + args[1] + (m_siteTerrainPreview.empty() ? "" : ", " + m_siteTerrainPreview) +
                                      (m_siteWaterPreview.empty() ? "" : ", water " + m_siteWaterPreview) + (has.empty() ? "" : "; " + has) +
                                      ". site_weather and day_hour set the weather and the time; test_site again for another.");
            PRED_LOG_INFO(Gameplay, "test_site: seed {} {} {}; {}", seed, TestSiteKindName(siteKind), SitePlan::TypeName(siteType), has);
        });
    // What each of them can be, offered under the console's input as it is typed.
    {
        const auto biomes = [this]
        {
            std::vector<std::string> ids;
            for (const BiomeDef& biome : m_universeData.biomes)
            {
                if (!biome.surface.empty())
                {
                    ids.push_back(biome.id);
                }
            }
            return ids;
        };
        const std::vector<std::string> grounds{"flat", "rolling", "mountainous", "canyons", "cratered", "dunes", "islands", "icesheet", "glacial", "lavaplain", "karst", "highlands", "mesas", "pillars", "crystalfields"};
        const std::vector<std::string> waters{"none", "lakes", "coast", "island", "seabed"};
        const auto with = [](std::vector<std::string> values, const char* extra)
        {
            values.emplace_back(extra);
            return values;
        };
        console.SetCompleter("test_site",
                             [=](size_t argument)
                             {
                                 return argument == 1   ? biomes()
                                        : argument == 2 ? with(grounds, "any")
                                        : argument == 3 ? with(waters, "any")
                                        : argument == 4 ? TestSiteWords()
                                                        : std::vector<std::string>{};
                             });
        console.SetCompleter("site_biome", [=](size_t argument) { return argument == 1 ? with(biomes(), "none") : std::vector<std::string>{}; });
        console.SetCompleter("site_terrain", [=](size_t argument) { return argument == 1 ? with(grounds, "none") : std::vector<std::string>{}; });
        console.SetCompleter("site_water", [=](size_t argument) { return argument == 1 ? with(waters, "world") : std::vector<std::string>{}; });
        console.SetCompleter("site_weather",
                             [](size_t argument)
                             {
                                 return argument == 1   ? std::vector<std::string>{"none", "snow", "rain", "dust", "ash"}
                                        : argument == 2 ? std::vector<std::string>{"0.3", "0.6", "1"}
                                                        : std::vector<std::string>{};
                             });
        console.SetCompleter("day_hour", [](size_t argument)
                             { return argument == 1 ? std::vector<std::string>{"6", "9", "12", "15", "18", "21", "0"} : std::vector<std::string>{}; });
    }
    console.RegisterCommand("site_weather", "What falls at the site, or round the outpost, you are at, on this machine, to see it: site_weather <none|snow|rain|dust|ash> [amount 0-1]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (args.size() < 2)
                                {
                                    m_app->GetConsole().PrintError("Usage: site_weather <none|snow|rain|dust|ash> [amount 0-1]");
                                    return;
                                }
                                for (SiteMap* place : {&m_facility, &m_outpostGround})
                                {
                                    SiteMap::Look look = place->GetLook();
                                    look.weather = args[1];
                                    look.weatherAmount = args.size() >= 3 ? std::clamp(std::strtof(args[2].c_str(), nullptr), 0.0f, 1.0f) : 0.7f;
                                    place->SetLook(look);
                                }
                                m_weatherHeld = true;
                                // The ground as wet or snowed on as that weather would leave it, at once.
                                m_surfaceWeatherSite = 0;
                            });
    console.RegisterCommand("footprint_trail", "Lay a few steps' prints on the ground ahead, as if walked there: footprint_trail [steps]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!m_facility.Built())
                                {
                                    return;
                                }
                                const int steps = args.size() >= 2 ? std::clamp(std::atoi(args[1].c_str()), 1, 40) : 10;
                                const glm::vec3 ahead = glm::normalize(glm::vec3(std::sin(m_lookYaw), 0.0f, -std::cos(m_lookYaw)));
                                for (int i = 0; i < steps; ++i)
                                {
                                    const glm::vec3 at = m_player.State().position + ahead * (1.0f + 0.7f * static_cast<float>(i));
                                    m_footprints.Leave(m_facility, at, m_lookYaw, i % 2 == 0, m_campaign.clock);
                                    if (i == 0)
                                    {
                                        PRED_LOG_INFO(Gameplay, "footprint_trail: from {:.1f} {:.2f} {:.1f}, the ground there {:.2f}", at.x, at.y, at.z,
                                                      m_facility.Terrain().Height(at.x, at.z));
                                    }
                                }
                            });
    console.RegisterCommand("ripple_trail", "Leave rings on the water ahead of the eye, as if somebody had just waded across it: ripple_trail [steps]",
                            [this](const std::vector<std::string>& args)
                            {
                                const int steps = args.size() >= 2 ? std::clamp(std::atoi(args[1].c_str()), 1, 16) : 8;
                                const glm::vec3 ahead = glm::normalize(glm::vec3(std::sin(m_lookYaw), 0.0f, -std::cos(m_lookYaw)));
                                for (int i = 0; i < steps; ++i)
                                {
                                    // (The furthest first and so the oldest: the way they would be, walked towards the eye.)
                                    AddRipple(m_renderEye + ahead * (1.5f + 0.6f * static_cast<float>(steps - i)), 0.7f,
                                              -0.3f * static_cast<float>(steps - i));
                                }
                            });
    console.RegisterCommand("env", "Log the light and the air as they are drawn now: the sun, the sky and ground light, the fog, the cloud",
                            [this](const std::vector<std::string>&)
                            {
                                const Environment& e = m_scene.GetEnvironment();
                                PRED_LOG_INFO(Gameplay, "env: sun {:.3f} x ({:.2f} {:.2f} {:.2f}) from {:.2f} {:.2f} {:.2f}; sky light {:.3f} {:.3f} {:.3f}; ground light {:.3f} {:.3f} {:.3f}",
                                              e.sunIntensity, e.sunColor.x, e.sunColor.y, e.sunColor.z, -e.sunDirection.x, -e.sunDirection.y, -e.sunDirection.z,
                                              e.ambientSky.x, e.ambientSky.y, e.ambientSky.z, e.ambientGround.x, e.ambientGround.y, e.ambientGround.z);
                                PRED_LOG_INFO(Gameplay, "env: fog {:.3f} {:.3f} {:.3f} from {:.0f} to {:.0f} m; cloud {:.2f} deck {:.2f} dark {:.2f}; haze {:.2f}", e.fogColor.x,
                                              e.fogColor.y, e.fogColor.z, e.fogStart, e.fogEnd, e.cloudCover, e.deckCover, e.cloudDark, e.skyHaze);
                                PRED_LOG_INFO(Gameplay, "env: weather {} climate {:.2f} spell {:.2f} at clock {:.0f}; turns bad in {:.0f} s", m_facility.GetLook().weather,
                                              m_climateAmount, Weather::SpellAt(m_facility.Seed(), m_campaign.clock), m_campaign.clock,
                                              Weather::TurnsIn(m_facility.Seed(), m_campaign.clock, Weather::kBadSpell, 3600.0f));
                            });
    console.RegisterCommand("far_things", "Log what is drawn further than this from the eye, by name, with how many and the nearest: far_things [metres]",
                            [this](const std::vector<std::string>& args)
                            {
                                const float beyond = args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : 300.0f;
                                std::map<std::string, std::pair<int, glm::vec3>> found;
                                m_scene.ForEachMeshRenderer([&](Entity entity, const Transform& transform, const MeshRenderer& renderer)
                                {
                                    if (!renderer.visible || glm::distance(transform.position, m_renderEye) < beyond)
                                    {
                                        return;
                                    }
                                    auto& [count, at] = found[m_scene.Name(entity)];
                                    if (count == 0 || glm::distance(transform.position, m_renderEye) < glm::distance(at, m_renderEye))
                                    {
                                        at = transform.position;
                                    }
                                    ++count;
                                });
                                for (const auto& [name, seen] : found)
                                {
                                    PRED_LOG_INFO(Gameplay, "far: {} x{}, nearest at {:.0f} {:.0f} {:.0f}", name, seen.first, seen.second.x, seen.second.y, seen.second.z);
                                }
                            });
    console.RegisterCommand("lightning_now", "In a storm, lightning now, to see it: lightning_now [yaw degrees] [metres off]",
                            [this](const std::vector<std::string>& args)
                            {
                                m_lightningIn = 0.0f;
                                m_lightningAim = {args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : -1.0f,
                                                  args.size() >= 3 ? std::strtof(args[2].c_str(), nullptr) : -1.0f};
                            });
    console.RegisterCommand("land_region","Pick where the shuttle goes down on the world the ship is over, as the map does: land_region <region>",
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
    console.RegisterCommand("day_hour", "Move the campaign's clock on to an hour of the day where the ship is (the host): day_hour <0 to 24>",
                            [this](const std::vector<std::string>& args)
                            {
                                const StarSystem* system = CurrentSystem();
                                const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
                                if (!m_campaignOpen || m_sessionMode == SessionMode::Client || args.size() < 2 || body == nullptr)
                                {
                                    m_app->GetConsole().PrintError("Usage, on the host at a body: day_hour <hour>");
                                    return;
                                }
                                const float now = LocalHour(*system, *body, m_campaign.travel.region);
                                const float ahead = std::fmod(std::stof(args[1]) - now + 48.0f, 24.0f);
                                m_campaign.clock += static_cast<double>(body->day) * static_cast<double>(ahead / 24.0f);
                                PRED_LOG_INFO(Gameplay, "day_hour: local time was {:.1f}, now {:.1f}", now, LocalHour(*system, *body, m_campaign.travel.region));
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
    console.RegisterCommand("depart", "Set out on the course plotted, as the helm does: depart",
                            [this](const std::vector<std::string>&) { AskCampaign(CampaignAction::Depart); });
    console.RegisterCommand("course_system", "Set a course for another system, the nth nearest the ship, and a world there to fly on to: course_system [n] [body]",
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
                                        if (args.size() >= 3)
                                        {
                                            AskCampaign(CampaignAction::PlotOnward, std::atoi(args[2].c_str()), 0);
                                        }
                                        return;
                                    }
                                }
                            });
    console.RegisterCommand("system_areas", "List the landing areas of the system the ship is in: each body's, with its kind and scan read",
                            [this](const std::vector<std::string>&)
                            {
                                const StarSystem* system = CurrentSystem();
                                if (system == nullptr)
                                {
                                    return;
                                }
                                for (const Body& body : system->bodies)
                                {
                                    for (size_t r = 0; r < body.regions.size(); ++r)
                                    {
                                        const LandingRegion& region = body.regions[r];
                                        PRED_LOG_INFO(Gameplay, "Area: body {} ({}, {} {:.0f} C, air {:.2f}) area {}: {} [{}] {}", body.index, body.name, body.biome, body.temperature, body.air, r, region.designation, region.kind,
                                                      RegionRead(region));
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
    console.RegisterCommand("travel_now", "Under way, arrive at once: in orbit of where it is going, or at the end of a crossing; or over the site it is going round to (the host)",
                            [this](const std::vector<std::string>&)
                            {
                                const StarSystem* system = CurrentSystem();
                                if (system != nullptr && IsAuthority() && m_campaign.travel.interstellar)
                                {
                                    m_campaign.travel.departed = m_campaign.clock - static_cast<double>(m_campaign.travel.duration);
                                    return;
                                }
                                const Body* target = system != nullptr ? system->Find(m_campaign.travel.target) : nullptr;
                                // Or going round the orbit to be over a site: over it now.
                                if (IsAuthority() && !m_campaign.travel.underway && Travel::SecondsToOver(m_campaign) > 0.0f)
                                {
                                    m_campaign.travel.overSince = m_campaign.clock - static_cast<double>(m_campaign.travel.overSeconds);
                                    return;
                                }
                                if (target == nullptr || !IsAuthority() || !m_campaign.travel.underway)
                                {
                                    m_app->GetConsole().PrintError("Only on the host, under way to somewhere");
                                    return;
                                }
                                // At the orbit's height on the side it is coming from: the next step takes it into orbit there.
                                const glm::dvec3 there = system->PositionD(target->index, m_campaign.clock);
                                glm::vec3 out = glm::vec3(m_campaign.travel.position - there);
                                out = glm::length(out) > 1.0e-12f ? glm::normalize(out) : glm::vec3(1.0f, 0.0f, 0.0f);
                                m_campaign.travel.position = there + glm::dvec3(out * Travel::OrbitRadius(*target));
                                m_campaign.travel.from = -1;
                            });
    // The ship's upgrades, as if bought: upgrade <category> <tier>, or on its own what it has.
    console.RegisterCommand("upgrade", "Set one of the ship's upgrades to a tier (the host): upgrade <travel|sensors|...> <tier>; on its own, list them",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!m_campaignOpen || m_sessionMode == SessionMode::Client)
                                {
                                    m_app->GetConsole().PrintError("Only on the host, with a campaign open");
                                    return;
                                }
                                if (args.size() < 3)
                                {
                                    m_app->GetConsole().Print("travel " + std::to_string(m_campaign.Upgrade("travel")) + ", sensors " +
                                                              std::to_string(m_campaign.Upgrade("sensors")));
                                    for (const auto& [category, tier] : m_campaign.upgrades)
                                    {
                                        if (category != "travel" && category != "sensors")
                                        {
                                            m_app->GetConsole().Print(category + " " + std::to_string(tier));
                                        }
                                    }
                                    return;
                                }
                                m_campaign.upgrades[args[1]] = std::clamp(std::atoi(args[2].c_str()), 0, 12);
                                CampaignChanged();
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
                                    return;
                                }
                                // And into it, from the title: not left there for the title's Solo to carry on another -- the most recent, somebody's
                                // own -- in its place.
                                if (m_screen == Screen::Title)
                                {
                                    m_titleMode = TitleMode::Solo;
                                    EnterWorld();
                                }
                            });
#endif
}

} // namespace pred
