// Contracts, leads, the debrief and the ship's terminal (Phase 4). The host offers contracts -- the set ones of the home system
// first, then others every few minutes -- and, when the shuttle has gone from a site, writes the debrief: the contract there paid,
// sometimes a lead to somewhere else, all in the ship's log. Everybody sees them on the terminal in the ops room, worked up close
// with the pointer (the camera brought to it as to a site's devices), and on the map. What they say and pay is data
// (Assets/Data/contracts.json); the rules are Game/Campaign/Contracts, the screen Game/Campaign/ShipTerminal.

#include "Game/PredationGame.h"
#include "Game/PredationGameShared.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

// A flat picture facing -z, its top up and its left the viewer's (as MissionProps' screens).
MeshData TerminalQuad(float w, float h)
{
    MeshData mesh;
    const glm::vec3 normal{0.0f, 0.0f, -1.0f};
    mesh.vertices.push_back(MeshVertex{{w * 0.5f, h * 0.5f, 0.0f}, normal, {0.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{-w * 0.5f, h * 0.5f, 0.0f}, normal, {1.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{-w * 0.5f, -h * 0.5f, 0.0f}, normal, {1.0f, 1.0f}});
    mesh.vertices.push_back(MeshVertex{{w * 0.5f, -h * 0.5f, 0.0f}, normal, {0.0f, 1.0f}});
    mesh.indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    return mesh;
}

// How far back from the screen the camera stands to work it.
constexpr float kTerminalStandOff = 1.15f;

// The terminal's own frame, its screen's face along -z: the ship's poses face along +z (CinePose), so turned about.
glm::quat TerminalTurn(const CinePose& at)
{
    return at.rotation * glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
}

} // namespace

// --- Places, by system, body and area ------------------------------------------------------------------------------------

const LandingRegion* PredationGame::FindArea(uint64_t system, int body, int region)
{
    const StarSystem* found = m_universe.System(system);
    const Body* world = found != nullptr ? found->Find(body) : nullptr;
    return world != nullptr && region >= 0 && region < static_cast<int>(world->regions.size()) ? &world->regions[static_cast<size_t>(region)]
                                                                                                : nullptr;
}

std::string PredationGame::AreaName(uint64_t system, int body, int region)
{
    const LandingRegion* area = FindArea(system, body, region);
    return area != nullptr ? area->designation : std::string("UNKNOWN AREA");
}

std::string PredationGame::WorldName(uint64_t system, int body)
{
    const StarSystem* found = m_universe.System(system);
    const Body* world = found != nullptr ? found->Find(body) : nullptr;
    if (world == nullptr)
    {
        return std::string();
    }
    return system == m_campaign.system ? world->name : world->name + ", " + found->name;
}

std::string PredationGame::MissionTemplateOf(const LandingRegion& region)
{
    // What the place's mission will be, from what kind of place it is (MissionPlan::Generate): a station is restored, a facility's
    // data recovered, survey ground read, a signal shut down. A wreck has none yet (Phase 11B).
    switch (SiteKindOf(region))
    {
    case SitePlan::SiteKind::Station: return Objectives::TemplateFor(MissionPlan::Kind::Restore);
    case SitePlan::SiteKind::Survey: return Objectives::TemplateFor(MissionPlan::Kind::Survey);
    case SitePlan::SiteKind::Signal: return Objectives::TemplateFor(MissionPlan::Kind::Signal);
    case SitePlan::SiteKind::Facility: return Objectives::TemplateFor(MissionPlan::Kind::Download);
    case SitePlan::SiteKind::Wreck: break;
    }
    return std::string();
}

const CampaignState::Contract* PredationGame::ContractAt(uint64_t system, int body, int region) const
{
    for (const CampaignState::Contract& contract : m_campaign.contracts)
    {
        if (contract.system == system && contract.body == body && contract.region == region && contract.status != CampaignState::Contract::Status::Done)
        {
            return &contract;
        }
    }
    return nullptr;
}

const CampaignState::LogEntry* PredationGame::LeadAt(uint64_t system, int body, int region) const
{
    const std::string key = CampaignState::RegionKey(system, body, region);
    const auto visited = m_campaign.regionChanges.find(key);
    if (visited != m_campaign.regionChanges.end() && visited->second.is_object() && visited->second.value("visited", false))
    {
        return nullptr;
    }
    for (const CampaignState::LogEntry& entry : m_campaign.log)
    {
        if (entry.kind == "lead" && entry.subject == key)
        {
            return &entry;
        }
    }
    return nullptr;
}

// --- Offering them ----------------------------------------------------------------------------------------------------------

void PredationGame::LoadContractRules()
{
    const std::filesystem::path file = Paths::AssetsRoot() / "Data" / "contracts.json";
    std::string error;
    if (!m_contractRules.LoadFromFile(file, &error))
    {
        m_contractRules = Contracts::Rules::Default();
        PRED_LOG_WARN(Gameplay, "Contracts: {} -- none set, the rewards built in", error);
    }
    m_app->GetFileWatcher().Watch(file, [this](const std::filesystem::path& path)
                                  {
                                      std::string why;
                                      if (!m_contractRules.LoadFromFile(path, &why))
                                      {
                                          PRED_LOG_WARN(Gameplay, "Contracts: {}", why);
                                      }
                                  });
}

float PredationGame::ContractDice()
{
    // The host's own throw for what to offer: what it comes to is in the campaign, sent to everybody.
    m_contractDice = m_contractDice * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<float>(m_contractDice >> 40) / static_cast<float>(1ull << 24);
}

bool PredationGame::PickArea(uint64_t& system, int& body, int& region, bool forLead)
{
    // Somewhere there is work: an area with a mission (not an outpost, not a wreck), not been to, with nothing on it already --
    // in the ship's system, or any the drive reaches across to.
    struct Candidate
    {
        uint64_t system;
        int body;
        int region;
    };
    std::vector<Candidate> candidates;
    std::vector<uint64_t> systems{m_campaign.system};
    if (DriveTier() >= 1)
    {
        for (const SystemId& id : m_universe.Near(Travel::GalaxyPosition(m_campaign, m_universe), Travel::CrossingRange(DriveTier())))
        {
            if (id.Packed() != m_campaign.system)
            {
                systems.push_back(id.Packed());
            }
        }
    }
    for (const uint64_t id : systems)
    {
        const StarSystem* at = m_universe.System(id);
        if (at == nullptr)
        {
            continue;
        }
        for (const Body& world : at->bodies)
        {
            if (!world.Landable())
            {
                continue;
            }
            for (int r = 0; r < static_cast<int>(world.regions.size()); ++r)
            {
                const LandingRegion& area = world.regions[static_cast<size_t>(r)];
                const std::string key = CampaignState::RegionKey(id, world.index, r);
                const auto changes = m_campaign.regionChanges.find(key);
                const bool visited = changes != m_campaign.regionChanges.end() && changes->second.is_object() && changes->second.value("visited", false);
                if (RegionIsPort(world, r) || MissionTemplateOf(area).empty() || visited || ContractAt(id, world.index, r) != nullptr ||
                    (forLead && LeadAt(id, world.index, r) != nullptr) ||
                    (id == m_campaign.system && world.index == m_campaign.body && r == m_campaign.travel.region))
                {
                    continue;
                }
                candidates.push_back({id, world.index, r});
            }
        }
    }
    if (candidates.empty())
    {
        return false;
    }
    const Candidate& chosen = candidates[std::min(static_cast<size_t>(ContractDice() * static_cast<float>(candidates.size())), candidates.size() - 1)];
    system = chosen.system;
    body = chosen.body;
    region = chosen.region;
    return true;
}

void PredationGame::OfferContract(uint64_t system, int body, int region, int set, int64_t reward)
{
    const LandingRegion* area = FindArea(system, body, region);
    if (area == nullptr)
    {
        return;
    }
    const std::string kind = MissionTemplateOf(*area);
    const CampaignState::Contract& contract = Contracts::Offer(m_campaign, system, body, region, kind, reward, set);
    CampaignState::LogEntry& entry = m_campaign.AddLog("contract", CampaignState::RegionKey(system, body, region), area->designation,
                                                       "Offered: " + m_contractRules.Wanted(kind) + " " + WorldName(system, body) + ". Pays " +
                                                           std::to_string(contract.reward) + " credits.");
    entry.time = m_campaign.clock;
    PRED_LOG_INFO(Gameplay, "Contract {} offered: {} on {}, {} credits", contract.id, area->designation, WorldName(system, body), contract.reward);
    CampaignChanged();
    AnnounceContract();
}

void PredationGame::AnnounceContract()
{
    // Heard aboard: the chime, and the intercom saying there is work on the terminal.
    if (m_screen == Screen::Playing && m_map == MapChoice::Ship)
    {
        PlayNamed("World/transmission", m_renderEye, 0.7f, 1.0f, false);
        Say("contract", 1.4f);
    }
}

void PredationGame::UpdateContracts(float dt)
{
    (void)dt;
    if (!m_campaignOpen || !IsAuthority())
    {
        return;
    }
    // The set ones first, one at a time: the next as soon as the last is done, and nothing else until they all are.
    if (m_campaign.setContractsDone < static_cast<int>(m_contractRules.set.size()))
    {
        const int next = m_campaign.setContractsDone;
        const bool open = std::any_of(m_campaign.contracts.begin(), m_campaign.contracts.end(), [next](const CampaignState::Contract& contract)
                                      { return contract.set == next && contract.status != CampaignState::Contract::Status::Done; });
        if (open)
        {
            return;
        }
        const StarSystem* home = m_universe.System(m_universe.Home());
        const Contracts::Rules::Set& wanted = m_contractRules.set[static_cast<size_t>(next)];
        for (const Body& world : home != nullptr ? home->bodies : std::vector<Body>{})
        {
            for (int r = 0; r < static_cast<int>(world.regions.size()); ++r)
            {
                if (world.regions[static_cast<size_t>(r)].designation == wanted.area)
                {
                    OfferContract(home->id.Packed(), world.index, r, next, wanted.reward);
                    return;
                }
            }
        }
        // (Not there: passed over rather than holding everything else up for good.)
        PRED_LOG_WARN(Gameplay, "Contracts: the set one '{}' is not in the home system -- passed over", wanted.area);
        m_campaign.setContractsDone = next + 1;
        CampaignChanged();
        return;
    }
    // Then others, every few minutes, a few waiting at most.
    if (m_campaign.nextOfferAt <= 0.0)
    {
        m_campaign.nextOfferAt = m_campaign.clock + 20.0;
        CampaignChanged();
    }
    if (m_campaign.clock < m_campaign.nextOfferAt)
    {
        return;
    }
    m_campaign.nextOfferAt = m_campaign.clock + static_cast<double>(m_contractRules.offerEvery[0] +
                                                                    (m_contractRules.offerEvery[1] - m_contractRules.offerEvery[0]) * ContractDice());
    uint64_t system = 0;
    int body = -1;
    int region = -1;
    if (Contracts::Offered(m_campaign) < m_contractRules.maxOffered && PickArea(system, body, region, false))
    {
        const LandingRegion* area = FindArea(system, body, region);
        const float fromHome = glm::distance(m_universe.SystemPosition(SystemId::Unpack(system)), m_universe.SystemPosition(m_universe.Home()));
        OfferContract(system, body, region, -1, m_contractRules.Reward(MissionTemplateOf(*area), fromHome));
    }
    CampaignChanged();
}

// --- When the shuttle has gone: the debrief ----------------------------------------------------------------------------------

void PredationGame::RecordExpedition()
{
    if (!m_campaignOpen || !IsAuthority() || m_campaign.travel.region < 0 || m_campaign.body < 0)
    {
        return;
    }
    const uint64_t system = m_campaign.system;
    const int body = m_campaign.body;
    const int region = m_campaign.travel.region;
    const LandingRegion* area = FindArea(system, body, region);
    if (area == nullptr)
    {
        return;
    }
    const std::string key = CampaignState::RegionKey(system, body, region);
    nlohmann::json& changes = m_campaign.regionChanges[key];
    if (!changes.is_object())
    {
        changes = nlohmann::json::object();
    }
    changes["visited"] = true;

    CampaignState::Debrief debrief;
    debrief.any = true;
    debrief.system = system;
    debrief.body = body;
    debrief.region = region;
    debrief.kind = Objectives::TemplateFor(m_missionPlan.kind);
    debrief.success = m_mission.recovered;
    debrief.crew = static_cast<uint8_t>(1u << LocalPlayerId());
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        debrief.crew = static_cast<uint8_t>(debrief.crew | (1u << remote.id));
    }
    debrief.aboard = m_mission.aboard;
    debrief.time = m_campaign.clock;

    const Objectives::Template* kind = m_objectiveLibrary.Find(debrief.kind);
    const std::string result = kind != nullptr ? (debrief.success ? kind->done : kind->failed) : (debrief.success ? "COMPLETE" : "NOT COMPLETED");
    const int crew = std::popcount(static_cast<unsigned>(debrief.crew));
    const int back = std::popcount(static_cast<unsigned>(debrief.aboard & debrief.crew));
    CampaignState::LogEntry& report = m_campaign.AddLog("expedition", key, area->designation,
                                                        result + ". " + std::to_string(back) + " of " + std::to_string(crew) + " back aboard.");
    report.time = m_campaign.clock;
    debrief.entries.push_back(report.id);

    if (debrief.success)
    {
        changes["done"] = true;
        const int setsBefore = m_campaign.setContractsDone;
        if (const CampaignState::Contract* contract = Contracts::Complete(m_campaign, system, body, region))
        {
            debrief.paid = contract->reward;
            debrief.contract = contract->id;
            CampaignState::LogEntry& paid = m_campaign.AddLog("contract", key, area->designation,
                                                              "Done. Paid " + std::to_string(contract->reward) + " credits.");
            paid.time = m_campaign.clock;
            debrief.entries.push_back(paid.id);
            PRED_LOG_INFO(Gameplay, "Contract {} done: {} credits", contract->id, contract->reward);
        }
        // A lead now and then -- and always at the end of the set ones, so there is somewhere to go next.
        const bool lastSet = setsBefore < static_cast<int>(m_contractRules.set.size()) &&
                             m_campaign.setContractsDone >= static_cast<int>(m_contractRules.set.size());
        uint64_t leadSystem = 0;
        int leadBody = -1;
        int leadRegion = -1;
        if ((lastSet || ContractDice() < m_contractRules.leadChance) && PickArea(leadSystem, leadBody, leadRegion, true))
        {
            const std::string place = AreaName(leadSystem, leadBody, leadRegion);
            CampaignState::LogEntry& lead = m_campaign.AddLog("lead", CampaignState::RegionKey(leadSystem, leadBody, leadRegion), place,
                                                              m_contractRules.LeadNote(debrief.kind, place, WorldName(leadSystem, leadBody)));
            lead.time = m_campaign.clock;
            lead.important = true;
            lead.links.push_back(report.id);
            debrief.entries.push_back(lead.id);
            PRED_LOG_INFO(Gameplay, "A lead: {} on {}", place, WorldName(leadSystem, leadBody));
        }
    }
    m_campaign.debrief = debrief;
    // The terminal opens on it, for whoever looks next.
    m_terminalView = {ShipTerminal::Page::Debrief, 0, 0};
    BroadcastTerminal();
    m_terminalDirty = true;
    CampaignChanged();
}

// --- Plotting a course to one -------------------------------------------------------------------------------------------------

bool PredationGame::PlotTo(uint64_t system, int body, int region)
{
    if (m_campaign.travel.interstellar)
    {
        return false;
    }
    if (system == m_campaign.system)
    {
        const StarSystem* here = CurrentSystem();
        if (here == nullptr || here->Find(body) == nullptr)
        {
            return false;
        }
        m_campaign.plan = {true, false, 0, body, region};
    }
    else
    {
        // Across to another system: only as far as the drive reaches, and on to the world there.
        const float distance = glm::length(m_universe.Glance(SystemId::Unpack(system)).position - Travel::GalaxyPosition(m_campaign, m_universe));
        if (m_universe.System(system) == nullptr || distance > Travel::CrossingRange(DriveTier()))
        {
            return false;
        }
        m_campaign.plan = {true, true, system, body, region};
    }
    CampaignChanged();
    return true;
}

bool PredationGame::Plotted(uint64_t system, int body, int region) const
{
    const CampaignState::Plan& plan = m_campaign.plan;
    if (!plan.set)
    {
        return false;
    }
    return plan.toSystem ? plan.system == system && plan.body == body : system == m_campaign.system && plan.body == body && plan.region == region;
}

// --- The terminal ------------------------------------------------------------------------------------------------------------

void PredationGame::BuildShipTerminal()
{
    MeshLibrary& meshes = m_app->GetMeshes();
    const CinePose at = m_ship.OpsTerminal();
    if (!m_terminalTexture.IsValid())
    {
        m_terminalTexture = m_app->GetTextures().CreateDynamic(ShipTerminal::kWide, ShipTerminal::kHigh, "ship_terminal");
    }
    // Its frame, a hand's breadth bigger than the screen, standing off the wall; the screen on its face.
    Transform frame;
    frame.position = at.position;
    frame.rotation = TerminalTurn(at);
    m_terminalFrame = m_scene.CreateMeshEntity("ship_terminal_frame", frame,
                                               meshes.Upload(Primitives::Box({ShipMap::kTerminalWide + 0.1f, ShipMap::kTerminalHigh + 0.1f, 0.05f}),
                                                             "ship_terminal_frame"),
                                               Material::Metal({0.12f, 0.125f, 0.13f}, 0.5f));
    Transform screen = frame;
    screen.position = at.position + TerminalTurn(at) * glm::vec3(0.0f, 0.0f, -0.027f);
    Material picture = Material::Diffuse(glm::vec3(1.0f), 0.35f);
    picture.baseColorTexture = m_terminalTexture;
    picture.emissive = glm::vec3(1.1f);
    picture.emissiveTextured = true;
    m_terminalScreen = m_scene.CreateMeshEntity("ship_terminal_screen", screen,
                                                meshes.Upload(TerminalQuad(ShipMap::kTerminalWide, ShipMap::kTerminalHigh), "ship_terminal_screen"), picture);
    if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_terminalScreen))
    {
        renderer->castsShadow = false;
    }
    Interactable interactable;
    interactable.entity = m_terminalScreen;
    interactable.kind = InteractionKind::Device;
    interactable.payload = static_cast<int>(MissionProps::Device::ShipTerminal);
    interactable.verb = "Use";
    interactable.name = "terminal";
    interactable.focusOffset = glm::vec3(0.0f, -0.25f, 0.0f);
    interactable.range = 2.6f;
    m_interactions.Register(interactable);
    m_terminalDirty = true;
}

bool PredationGame::TerminalPose(glm::vec3& eye, glm::vec3& target) const
{
    if (!m_terminalScreen.IsValid())
    {
        return false;
    }
    const CinePose at = m_ship.OpsTerminal();
    target = at.position;
    eye = at.position + TerminalTurn(at) * glm::vec3(0.0f, 0.0f, -kTerminalStandOff);
    return true;
}

ShipTerminal::Words PredationGame::TerminalWords()
{
    ShipTerminal::Words words;
    words.area = [this](uint64_t system, int body, int region) { return AreaName(system, body, region); };
    words.world = [this](uint64_t system, int body) { return WorldName(system, body); };
    words.wanted = [this](const std::string& kind) { return m_contractRules.Wanted(kind); };
    words.read = [this](uint64_t system, int body, int region)
    {
        const LandingRegion* area = FindArea(system, body, region);
        return area != nullptr ? RegionRead(*area) : std::string();
    };
    words.result = [this](const std::string& kind, bool success)
    {
        const Objectives::Template* found = m_objectiveLibrary.Find(kind);
        return found != nullptr ? (success ? found->done : found->failed) : std::string(success ? "COMPLETE" : "NOT COMPLETED");
    };
    words.player = [this](int id)
    {
        if (id == LocalPlayerId())
        {
            return PlayerName();
        }
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (remote.id == id)
            {
                return remote.name;
            }
        }
        return "Crew " + std::to_string(id + 1);
    };
    words.plotted = [this](uint64_t system, int body, int region) { return Plotted(system, body, region); };
    return words;
}

void PredationGame::UpdateShipTerminal(float dt)
{
    if (!m_terminalScreen.IsValid())
    {
        return;
    }
    if (const Interactable* interactable = m_interactions.Find(m_terminalScreen))
    {
        const bool usable = m_campaignOpen && m_map == MapChoice::Ship && !m_cine.Active();
        if (interactable->enabled != usable)
        {
            m_interactions.SetEnabled(m_terminalScreen, usable);
        }
    }
    // Drawn a few times a second while anybody here could see it (the campaign changing under it), at once when its page or
    // what is under the pointer changes.
    m_terminalRedrawIn -= dt;
    const bool near = m_map == MapChoice::Ship && glm::distance(m_renderEye, m_ship.OpsTerminal().position) < 25.0f;
    if (!m_campaignOpen || !near || (!m_terminalDirty && m_terminalRedrawIn > 0.0f))
    {
        return;
    }
    m_terminalDirty = false;
    m_terminalRedrawIn = 0.5f;
    ScreenCanvas canvas(ShipTerminal::kWide, ShipTerminal::kHigh);
    const bool mine = m_device == MissionProps::Device::ShipTerminal && !m_deviceLeaving;
    m_shipTerminal.Draw(canvas, m_campaign, m_terminalView, TerminalWords(), mine ? m_terminalHover.x : -1, mine ? m_terminalHover.y : -1);
    m_app->GetTextures().Update(m_terminalTexture, canvas.image);
}

void PredationGame::TerminalPointer(const glm::vec3& eye, const glm::vec3& direction, bool clicked, float wheel)
{
    // Where the pointer's line meets the screen, as a point on its picture.
    const CinePose at = m_ship.OpsTerminal();
    const glm::vec3 out = TerminalTurn(at) * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 face = at.position + out * 0.028f;
    const float facing = glm::dot(direction, out);
    glm::ivec2 hover{-1, -1};
    if (facing < -1e-4f)
    {
        const float along = glm::dot(face - eye, out) / facing;
        const glm::vec3 local = glm::inverse(TerminalTurn(at)) * (eye + direction * along - face);
        const float u = (ShipMap::kTerminalWide * 0.5f - local.x) / ShipMap::kTerminalWide;
        const float v = (ShipMap::kTerminalHigh * 0.5f - local.y) / ShipMap::kTerminalHigh;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)
        {
            hover = {static_cast<int>(u * static_cast<float>(ShipTerminal::kWide)), static_cast<int>(v * static_cast<float>(ShipTerminal::kHigh))};
        }
    }
    if (hover != m_terminalHover)
    {
        m_terminalHover = hover;
        m_terminalDirty = true;
    }
    if (clicked && hover.x >= 0)
    {
        PressTerminal(m_shipTerminal.Hit(hover.x, hover.y));
    }
    if (std::abs(wheel) > 0.01f)
    {
        PressTerminal({ShipTerminal::Press::Kind::Scroll, wheel > 0.0f ? -1 : 1});
    }
}

void PredationGame::PressTerminal(const ShipTerminal::Press& press)
{
    using Kind = ShipTerminal::Press::Kind;
    ShipTerminal::View view = m_terminalView;
    switch (press.kind)
    {
    case Kind::None: return;
    case Kind::Page: view = {static_cast<ShipTerminal::Page>(press.value), 0, 0}; break;
    case Kind::Select: view.selected = press.value; break;
    case Kind::Scroll:
        view.scroll += press.value * (ShipTerminal::kRows / 2);
        view.selected = std::clamp(view.selected, view.scroll, view.scroll + ShipTerminal::kRows - 1);
        break;
    case Kind::Accept: AskCampaign(CampaignAction::AcceptContract, press.value); break;
    case Kind::Plot: AskCampaign(press.lead ? CampaignAction::PlotLead : CampaignAction::PlotContract, press.value); break;
    }
    PlayNamed("UI/confirm", m_renderEye, 0.45f, press.kind == Kind::Accept || press.kind == Kind::Plot ? 1.0f : 1.25f, false);
    view = ShipTerminal::Clamped(m_campaign, view);
    if (!(view == m_terminalView))
    {
        AskCampaign(CampaignAction::TerminalView, static_cast<int>(view.page) | (view.selected << 4), view.scroll);
    }
}

bool PredationGame::DoTerminalAction(uint8_t player, CampaignAction action, int a, int b)
{
    // The host: the terminal's page for everybody, a contract taken on, a course plotted from it.
    switch (action)
    {
    case CampaignAction::TerminalView:
        m_terminalView = ShipTerminal::Clamped(m_campaign, {static_cast<ShipTerminal::Page>(a & 0xF), a >> 4, b});
        BroadcastTerminal();
        m_terminalDirty = true;
        return true;

    case CampaignAction::AcceptContract:
    {
        if (!Contracts::Accept(m_campaign, static_cast<uint32_t>(a)))
        {
            return false;
        }
        const CampaignState::Contract* contract = m_campaign.FindContract(static_cast<uint32_t>(a));
        PRED_LOG_INFO(Gameplay, "Player {} took on contract {}", player, a);
        PlotTo(contract->system, contract->body, contract->region);
        m_terminalDirty = true;
        CampaignChanged();
        return true;
    }

    case CampaignAction::PlotContract:
    {
        const CampaignState::Contract* contract = m_campaign.FindContract(static_cast<uint32_t>(a));
        m_terminalDirty = true;
        return contract != nullptr && PlotTo(contract->system, contract->body, contract->region);
    }

    case CampaignAction::PlotLead:
    {
        const CampaignState::LogEntry* entry = m_campaign.FindLog(static_cast<uint32_t>(a));
        unsigned long long system = 0;
        int body = -1;
        int region = -1;
        m_terminalDirty = true;
        return entry != nullptr && std::sscanf(entry->subject.c_str(), "%llu:%d:%d", &system, &body, &region) == 3 && PlotTo(system, body, region);
    }

    default: return false;
    }
}

void PredationGame::BroadcastTerminal(int player)
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }
    WorldEventMessage event;
    event.kind = WorldEventKind::Terminal;
    event.index = static_cast<uint8_t>(m_terminalView.page);
    event.item = static_cast<uint16_t>(std::clamp(m_terminalView.selected, 0, 4095));
    event.rounds = static_cast<uint16_t>(std::clamp(m_terminalView.scroll, 0, 4095));
    event.quiet = player >= 0;
    if (player >= 0)
    {
        m_host.SendTo(static_cast<uint8_t>(player), event);
    }
    else
    {
        m_host.Broadcast(event);
    }
}

void PredationGame::ApplyTerminalEvent(const WorldEventMessage& event)
{
    m_terminalView = {static_cast<ShipTerminal::Page>(std::min<int>(event.index, static_cast<int>(ShipTerminal::Page::Count) - 1)), event.item, event.rounds};
    m_terminalDirty = true;
}

// --- The shuttle's screen ---------------------------------------------------------------------------------------------------

void PredationGame::UpdateShuttleScreens(float dt)
{
    constexpr int kWide = 512;
    constexpr int kHigh = 232;
    constexpr float kScreenWide = 0.92f;
    constexpr float kScreenHigh = kScreenWide * static_cast<float>(kHigh) / static_cast<float>(kWide);
    if (!m_shuttleScreenTexture.IsValid())
    {
        m_shuttleScreenTexture = m_app->GetTextures().CreateDynamic(kWide, kHigh, "shuttle_site_screen");
    }
    // In the bezel on each shuttle's dash, facing the seats: followed as the shuttle moves.
    bool near = false;
    for (int i = 0; i < 2; ++i)
    {
        const VehicleProp& shuttle = i == 0 ? m_ship.Shuttle() : m_facility.Shuttle();
        const Entity console = shuttle.Part("dash_top");
        const Transform* at = console.IsValid() ? m_scene.GetTransform(console) : nullptr;
        Entity& screen = m_shuttleScreens[i];
        if (at == nullptr)
        {
            if (MeshRenderer* renderer = screen.IsValid() ? m_scene.GetMeshRenderer(screen) : nullptr)
            {
                renderer->visible = false;
            }
            continue;
        }
        if (!screen.IsValid() || m_scene.GetTransform(screen) == nullptr)
        {
            Material picture = Material::Diffuse(glm::vec3(1.0f), 0.35f);
            picture.baseColorTexture = m_shuttleScreenTexture;
            picture.emissive = glm::vec3(1.05f);
            picture.emissiveTextured = true;
            screen = m_scene.CreateMeshEntity("shuttle_site_screen", Transform{}, m_app->GetMeshes().Upload(TerminalQuad(kScreenWide, kScreenHigh), "shuttle_site_screen"),
                                              picture);
            if (MeshRenderer* renderer = m_scene.GetMeshRenderer(screen))
            {
                renderer->castsShadow = false;
            }
        }
        if (Transform* placed = m_scene.GetTransform(screen))
        {
            placed->rotation = at->rotation * glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)) * glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
            placed->position = at->position + at->rotation * glm::vec3(0.0f, 0.044f, 0.02f);
        }
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(screen))
        {
            renderer->visible = true;
        }
        near = near || glm::distance(m_renderEye, at->position) < 30.0f;
    }
    m_shuttleScreenIn -= dt;
    if (!near || m_shuttleScreenIn > 0.0f)
    {
        return;
    }
    m_shuttleScreenIn = 1.0f;

    // What it shows: the site gone to, or about to be.
    ScreenCanvas canvas(kWide, kHigh);
    const Rgb back{6, 12, 16};
    const Rgb ink{214, 226, 232};
    const Rgb dim{104, 124, 134};
    const Rgb amber{255, 176, 64};
    const Rgb line{36, 60, 72};
    canvas.Clear(back);
    canvas.Fill(0.0f, 0.0f, static_cast<float>(kWide), 26.0f, {16, 30, 38});
    canvas.Text(10, 7, "SHUTTLE NAVIGATION - SITE DATA", ink, 2);
    const auto up = [](std::string text)
    {
        std::string out;
        for (const char c : text)
        {
            const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (Glyph(u) != nullptr || u == ' ')
            {
                out.push_back(u);
            }
        }
        return out;
    };
    const bool going = m_facility.Built() && (m_map == MapChoice::Facility || m_shipReady || m_shipOrbiting != 0);
    if (!going)
    {
        canvas.Text(10, 44, "NO DESTINATION SET", dim, 2);
        canvas.Text(10, 70, "CHOOSE ONE ON THE NAVIGATION MAP", dim, 1);
    }
    else
    {
        const SiteStats stats = SiteStatsNow();
        canvas.Text(10, 36, up(stats.site).substr(0, 40).c_str(), amber, 2);
        canvas.Text(10, 56, up(stats.planet).c_str(), dim, 1);
        canvas.Line(10.0f, 70.0f, static_cast<float>(kWide - 10), 70.0f, line);
        int y = 80;
        for (const std::string& row : {stats.localTime, stats.temperature, stats.wind, stats.visibility})
        {
            canvas.Text(10, y, up(row).c_str(), ink, 2);
            y += 20;
        }
        canvas.Text(10, y + 2, m_missionPlan.mapGiven ? "MAP DATA ON FILE" : "NO MAP DATA", m_missionPlan.mapGiven ? dim : amber, 1);
        // What is wanted there, wrapped to the screen.
        int wy = y + 18;
        for (const std::string& brief : LandingBrief())
        {
            std::string rest = up(brief);
            while (!rest.empty() && wy < kHigh - 10)
            {
                size_t cut = rest.size() <= 80 ? rest.size() : rest.rfind(' ', 80);
                cut = cut == std::string::npos || cut == 0 ? std::min<size_t>(80, rest.size()) : cut;
                canvas.Text(10, wy, rest.substr(0, cut).c_str(), amber, 1);
                rest = cut < rest.size() ? rest.substr(cut + 1) : std::string();
                wy += 10;
            }
        }
    }
    canvas.Lines();
    m_app->GetTextures().Update(m_shuttleScreenTexture, canvas.image);
}

// --- F3: the Mission window ---------------------------------------------------------------------------------------------------

void PredationGame::DrawMissionPanel()
{
    ImGui::SetNextWindowPos(ImVec2(830.0f, 470.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mission", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    static const char* const kStages[] = {"none", "find", "working", "carry", "over"};
    static const char* const kKinds[] = {"download", "restore", "survey", "signal"};
    static const char* const kSiteKinds[] = {"facility", "station", "survey", "wreck", "signal"};
    static const char* const kDevices[] = {"none", "board", "console", "transmitter", "ship terminal"};
    if (m_facility.Built())
    {
        const uint32_t seed = m_facility.Seed();
        const SitePlan::SiteKind kind = SitePlan::KindOf(seed);
        ImGui::Text("Site %u: %s, %s", seed, kSiteKinds[static_cast<int>(kind)], SitePlan::TypeName(SitePlan::TypeOf(seed, kind)));
    }
    if (m_missionPlan.Valid())
    {
        const MissionState& s = m_mission;
        ImGui::Text("Mission %s   stage %s   %s   map %s", kKinds[static_cast<int>(m_missionPlan.kind)], kStages[static_cast<int>(s.stage)],
                    s.powered ? "powered" : "POWER OUT", m_missionPlan.mapGiven ? "given" : (s.found ? "none, terminal found" : "none"));
        ImGui::Text("Progress %.0f%%   %s   launch %s", s.progress * 100.0f, s.attended ? "attended" : "unattended",
                    s.Launching() ? std::to_string(static_cast<int>(s.launchIn)).c_str() : "-");
        if (m_missionPlan.powerOut)
        {
            ImGui::Text("Board: door %s, main %s, on %02X (tripped %02X, fault %d)", s.boardOpen ? "open" : "shut", s.boardMain ? "on" : "off",
                        s.boardOn, m_missionPlan.boardTripped, m_missionPlan.boardFault);
        }
        if (m_missionPlan.AtMarkers())
        {
            ImGui::Text("Readings %02X of %zu   reading %d", s.readings, m_missionPlan.markers.size(), s.reading);
        }
        ImGui::Text("Device bits %X", s.device);
        for (const int open : m_objectives.Open())
        {
            ImGui::BulletText("%s", m_objectives.steps[static_cast<size_t>(open)].def->text.c_str());
        }
        if (IsAuthority())
        {
            if (ImGui::Button("Finish (all aboard)") && s.stage != MissionState::Stage::Over)
            {
                m_app->GetConsole().Execute("mission_finish all");
            }
            ImGui::SameLine();
            if (ImGui::Button("Skip work") && s.stage == MissionState::Stage::Downloading)
            {
                m_mission.progress = 0.999f;
            }
        }
    }
    else
    {
        ImGui::TextDisabled("No mission here.");
    }
    if (DeviceOpen())
    {
        ImGui::Text("Up close: %s (%.0f%%)%s", kDevices[static_cast<int>(m_device)], m_deviceBlend * 100.0f, m_deviceLeaving ? ", leaving" : "");
    }
    if (m_campaignOpen)
    {
        ImGui::Separator();
        int offered = 0;
        int taken = 0;
        int done = 0;
        for (const CampaignState::Contract& contract : m_campaign.contracts)
        {
            offered += contract.status == CampaignState::Contract::Status::Offered ? 1 : 0;
            taken += contract.status == CampaignState::Contract::Status::Accepted ? 1 : 0;
            done += contract.status == CampaignState::Contract::Status::Done ? 1 : 0;
        }
        const int leads = static_cast<int>(ShipTerminal::LeadRows(m_campaign).size());
        ImGui::Text("Credits %lld   contracts %d offered, %d taken on, %d done   leads %d", static_cast<long long>(m_campaign.credits), offered, taken,
                    done, leads);
        ImGui::Text("Set contracts done %d of %zu   next offer in %.0f s", m_campaign.setContractsDone, m_contractRules.set.size(),
                    std::max(m_campaign.nextOfferAt - m_campaign.clock, 0.0));
        static const char* const kPages[] = {"contracts", "leads", "log", "debrief"};
        ImGui::Text("Terminal page %s, row %d, scroll %d", kPages[static_cast<int>(m_terminalView.page) % 4], m_terminalView.selected,
                    m_terminalView.scroll);
        if (IsAuthority() && ImGui::Button("Offer a contract now"))
        {
            m_campaign.nextOfferAt = m_campaign.clock;
        }
    }
    ImGui::End();
}

} // namespace pred
