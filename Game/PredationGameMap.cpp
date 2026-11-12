// The system map and the ship's travel: looking at the system from the navigation console, pointing things out to the
// others, choosing where to go and where to go down, and the ship flying there. The map's picture is Game/Campaign/
// SystemMap and Engine/Render/PlanetRenderer; travel is Game/Campaign/Travel.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/Renderer.h"
#include "Engine/Render/TextureLibrary.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/Outpost.h"
#include "Game/World/ScreenCanvas.h"

#include <imgui.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace pred
{

namespace
{

// How often the host tells everybody how the ship stands and who is pointing at what.
constexpr float kTravelSendEvery = 0.1f;

// Colder than this (degrees Celsius), a site's water is ice and its rain falls as snow.
constexpr float kFreezes = -2.0f;

// A region's seed as the site builder takes it.
uint16_t SiteSeed(uint32_t seed)
{
    const auto folded = static_cast<uint16_t>((seed ^ (seed >> 16)) & 0xFFFFu);
    return folded == 0 ? uint16_t{1} : folded;
}


} // namespace

PlanetLook PredationGame::LookOf(const Body& body)
{
    PlanetLook look;
    look.groundA = body.groundA;
    look.groundB = body.groundB;
    look.ocean = body.ocean;
    look.oceanAmount = body.oceanAmount;
    look.ice = body.ice;
    look.clouds = body.clouds;
    look.cloudColor = body.cloudColor;
    look.airColor = body.airColor;
    look.air = body.air;
    look.gas = body.gas;
    look.seed = static_cast<float>(body.seed % 997u) * 0.37f;
    look.rings = body.rings;
    look.ringColor = body.ringColor;
    // Its weather, seen from space as it is felt on the ground: more cloud and storms turning in it where it rains or snows,
    // grey sheets of fog, tan veils of dust over a dusty world, dark plumes over an ash-choked one.
    if (!body.gas && body.air >= 0.015f)
    {
        const std::string& w = body.weather;
        // (A sea world under clear skies -- Universe -- keeps them: its weather in a few storms, its sea seen.)
        const float clear = body.oceanAmount > 0.8f && body.clouds < 0.3f ? 0.4f : 1.0f;
        const auto more = [&](float clouds, float storms) {
            look.clouds = std::max(look.clouds, clouds * clear);
            look.storms = std::max(look.storms, storms * clear);
        };
        if (w == "rain")
        {
            more(0.6f, 0.25f);
        }
        else if (w == "storm")
        {
            more(0.75f, 1.0f);
            look.cloudColor *= 0.88f;
        }
        else if (w == "snow" || w == "heavy_snow" || w == "blizzard")
        {
            more(w == "snow" ? 0.6f : 0.75f, w == "blizzard" ? 0.58f : w == "heavy_snow" ? 0.45f : 0.2f);
        }
        else if (w == "fog")
        {
            more(0.65f, 0.0f);
            look.cloudColor = glm::mix(look.cloudColor, glm::vec3(0.78f, 0.8f, 0.82f), 0.5f);
        }
        else if (w == "dust" || w == "sandstorm")
        {
            more(w == "dust" ? 0.25f : 0.38f, w == "sandstorm" ? 0.55f : 0.2f);
            look.cloudColor = glm::mix(body.groundA, glm::vec3(0.85f, 0.72f, 0.55f), 0.4f);
        }
        else if (w == "ash" || w == "ash_storm")
        {
            more(w == "ash" ? 0.55f : 0.75f, w == "ash_storm" ? 0.85f : 0.3f);
            look.cloudColor = glm::vec3(0.32f, 0.29f, 0.27f);
        }
    }
    return look;
}

const StarSystem* PredationGame::CurrentSystem()
{
    return m_campaignOpen ? m_universe.System(m_campaign.system) : nullptr;
}

int PredationGame::DriveTier() const
{
    return m_campaign.Upgrade("travel");
}

int PredationGame::SensorTier() const
{
    return m_campaign.Upgrade("sensors");
}

bool PredationGame::RegionKnown(const Body& body, int region) const
{
    return region >= 0 && region < static_cast<int>(body.regions.size());
}

bool PredationGame::RegionLandable(const Body& body, int region) const
{
    return RegionKnown(body, region) && body.Landable();
}

// --- What is asked of the campaign ----------------------------------------------------------------------------------

void PredationGame::AskCampaign(CampaignAction action, int a, int b)
{
    if (m_sessionMode == SessionMode::Client)
    {
        CampaignRequest request;
        request.action = static_cast<uint8_t>(action);
        request.a = a;
        request.b = b;
        m_client.SendCampaignRequest(request);
        return;
    }
    DoCampaignAction(LocalPlayerId(), action, a, b);
}

bool PredationGame::DoCampaignAction(uint8_t player, CampaignAction action, int a, int b)
{
    // The ship's terminal's, which are not the map's.
    switch (action)
    {
    case CampaignAction::TerminalView:
    case CampaignAction::AcceptContract:
    case CampaignAction::PlotContract:
    case CampaignAction::PlotLead:
    case CampaignAction::SellCargo: return DoTerminalAction(player, action, a, b);
    default: break;
    }
    const StarSystem* system = CurrentSystem();
    if (system == nullptr)
    {
        return false;
    }
    switch (action)
    {
    case CampaignAction::SetCourse:
    {
        // Only aboard, and not while the ship is already doing something that has the picture.
        if (m_map != MapChoice::Ship || m_cine.Active() || system->Find(a) == nullptr)
        {
            return false;
        }
        if (!m_campaign.travel.underway && a == m_campaign.body)
        {
            ChooseLandingRegion(b);
            return true;
        }
        const bool leaving = !m_campaign.travel.underway;
        const bool fromGround = ShipLanded();
        if (!Travel::SetCourse(m_campaign, *system, a))
        {
            return false;
        }
        m_campaign.travel.region = b;
        m_shipReady = false;
        m_shipOrbiting = 0;
        PRED_LOG_INFO(Gameplay, "Player {} set a course for {}", player, system->bodies[static_cast<size_t>(a)].name);
        if (leaving)
        {
            PlayLeaving(fromGround);
        }
        CampaignChanged();
        return true;
    }

    case CampaignAction::SetSystemCourse:
    {
        if (m_map != MapChoice::Ship || m_cine.Active())
        {
            return false;
        }
        const uint64_t to = static_cast<uint64_t>(static_cast<uint32_t>(a)) | (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32);
        const bool leaving = !m_campaign.travel.underway;
        const bool fromGround = ShipLanded();
        if (!Travel::SetSystemCourse(m_campaign, m_universe, to, DriveTier()))
        {
            return false;
        }
        m_shipReady = false;
        m_shipOrbiting = 0;
        PRED_LOG_INFO(Gameplay, "Player {} set a course for {}", player, m_universe.Glance(SystemId::Unpack(to)).name);
        if (leaving)
        {
            PlayLeaving(fromGround);
        }
        CampaignChanged();
        return true;
    }

    case CampaignAction::PlotCourse:
        if (system->Find(a) == nullptr || m_campaign.travel.interstellar)
        {
            return false;
        }
        m_campaign.plan = {true, false, 0, a, b};
        CampaignChanged();
        return true;

    case CampaignAction::PlotSystem:
    {
        const uint64_t to = static_cast<uint64_t>(static_cast<uint32_t>(a)) | (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32);
        const float distance = glm::length(m_universe.Glance(SystemId::Unpack(to)).position - Travel::GalaxyPosition(m_campaign, m_universe));
        if (m_universe.System(to) == nullptr || to == m_campaign.system || distance > Travel::CrossingRange(DriveTier()))
        {
            return false;
        }
        m_campaign.plan = {true, true, to, -1, -1};
        CampaignChanged();
        return true;
    }

    case CampaignAction::Door:
        // Only on the ground does it open; it can always be shut -- and, shutting, it waits open while anybody stands in it
        // (ShipMap::UpdateAirlock).
        if (a != 0 && !ShipLanded())
        {
            return false;
        }
        m_campaign.doorOpen = a != 0;
        CampaignChanged();
        return true;

    case CampaignAction::PlotOnward:
    {
        // A world of the system just plotted for, to fly on to once there.
        const StarSystem* to = m_campaign.plan.set && m_campaign.plan.toSystem ? m_universe.System(m_campaign.plan.system) : nullptr;
        if (to == nullptr || to->Find(a) == nullptr)
        {
            return false;
        }
        m_campaign.plan.body = a;
        CampaignChanged();
        return true;
    }

    case CampaignAction::HoldMap:
        // The first to ask has it, until they give it up; anybody else asking is told who has it.
        if (a != 0 && m_mapHolder != kNobody && m_mapHolder != player)
        {
            SendMapHolder();
            return false;
        }
        if (a != 0)
        {
            m_mapHolder = player;
        }
        else if (m_mapHolder == player)
        {
            m_mapHolder = kNobody;
        }
        SendMapHolder();
        return true;

    case CampaignAction::Depart:
    {
        // Off the ground only with everybody aboard: nobody is left on the pad as the station is left behind.
        if (!m_campaign.plan.set || !LeavingBlocked().empty())
        {
            return false;
        }
        const CampaignState::Plan plan = m_campaign.plan;
        m_campaign.plan = {};
        const bool done = plan.toSystem ? DoCampaignAction(player, CampaignAction::SetSystemCourse, static_cast<int>(static_cast<uint32_t>(plan.system & 0xFFFFFFFFu)),
                                                           static_cast<int>(static_cast<uint32_t>(plan.system >> 32)))
                                        : DoCampaignAction(player, CampaignAction::SetCourse, plan.body, plan.region);
        // Across the stars and on to a world there, if one was picked: the crossing knows where it goes on to.
        if (done && plan.toSystem)
        {
            m_campaign.travel.onward = plan.body;
        }
        if (!done)
        {
            m_campaign.plan = plan;
        }
        CampaignChanged();
        return done;
    }

    case CampaignAction::CancelCourse:
        if (!m_campaign.travel.underway || m_campaign.travel.target < 0)
        {
            return false;
        }
        Travel::SetCourse(m_campaign, *system, -1);
        PRED_LOG_INFO(Gameplay, "Player {} called off the course: coming to a stop", player);
        CampaignChanged();
        return true;

    case CampaignAction::SetRegion:
        if (m_campaign.travel.underway)
        {
            m_campaign.travel.region = a;
            CampaignChanged();
            return true;
        }
        if (m_map != MapChoice::Ship || m_cine.Active())
        {
            return false;
        }
        ChooseLandingRegion(a);
        return true;

    case CampaignAction::None:
        break;
    }
    return false;
}

void PredationGame::ChooseLandingRegion(int region)
{
    const StarSystem* system = CurrentSystem();
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    if (body == nullptr || m_campaign.travel.underway)
    {
        m_shipReady = false;
        return;
    }
    if (!RegionLandable(*body, region))
    {
        // Nowhere chosen: the ship stays in orbit until somebody picks where to go down on the map -- never a place picked
        // for them.
        region = -1;
    }
    // A hub's field: the ship itself sets down there, and nobody needs the shuttle.
    if (RegionIsPort(*body, region))
    {
        const bool already = m_campaign.landed;
        m_campaign.travel.region = region;
        m_campaign.landed = true;
        m_shipReady = false;
        m_shipOrbiting = 0;
        m_groundBody = body->index;
        m_groundRegion = region;
        if (!already && m_map == MapChoice::Ship)
        {
            // Seen from aboard, coming down onto it.
            StartDescent(true);
        }
        if (!already)
        {
            m_campaign.Learn(m_campaign.system, body->index, CampaignState::kKnownVisited);
            m_campaign.AddLog("region", CampaignState::RegionKey(m_campaign.system, body->index, region),
                              body->regions[static_cast<size_t>(region)].designation, "The ship set down on " + body->name + ".");
            PRED_LOG_INFO(Gameplay, "Landed at {} on {}", body->regions[static_cast<size_t>(region)].designation, body->name);
        }
        CampaignChanged();
        return;
    }
    // Anywhere else the ship stays up, in orbit, and the shuttle goes down: up off the pad first, if it was on one.
    if (m_campaign.landed && m_map == MapChoice::Ship && !m_cine.Active())
    {
        Travel::Lift(m_campaign);
        PlayLeaving(true);
    }
    Travel::Lift(m_campaign);
    m_campaign.travel.region = region;
    CampaignChanged();
    if (region < 0)
    {
        m_shipReady = false;
        m_shipOrbiting = 0;
        return;
    }
    // The kind of place it is decides what is there (SitePlan::SiteKind), carried in the seed.
    const uint16_t seed = SitePlan::SeedFor(SiteSeed(body->regions[static_cast<size_t>(region)].seed), SiteKindOf(body->regions[static_cast<size_t>(region)]));
    // Made once the ship is over it, before anybody launches for it (OnUpdate) -- not now, while it may yet go somewhere else.
    m_siteToBuild = m_facility.Seed() != seed || m_mission.stage == MissionState::Stage::Over ? seed : 0;
    m_shipOrbiting = seed;
    m_shipReady = true;
    // And round the orbit to be over it -- the shuttle goes down from right above it, not from wherever the ship happened to
    // be (LaunchFromShip waits for it). Not begun again for the place it is already going to or over.
    const glm::vec3 over = AreaOnGlobe(body->regions[static_cast<size_t>(region)]);
    if (glm::length(m_campaign.travel.overAt) < 0.5f || glm::dot(glm::normalize(m_campaign.travel.overAt), glm::normalize(over)) < 0.9999f)
    {
        Travel::MoveOver(m_campaign, *system, body->index, over, DriveTier());
        CampaignChanged();
    }
    PRED_LOG_INFO(Gameplay, "Going down to {} on {}", body->regions[static_cast<size_t>(region)].designation, body->name);
}

std::string PredationGame::SummaryOf(const Body& body) const
{
    // What is known of it, in a line: for the log.
    std::string text;
    const auto add = [&text](const std::string& part)
    {
        if (!part.empty())
        {
            text += (text.empty() ? "" : ", ") + part;
        }
    };
    if (const BiomeDef* biome = m_universeData.Biome(body.biome))
    {
        add(biome->name);
    }
    if (const AtmosphereDef* air = m_universeData.Atmosphere(body.atmosphere))
    {
        add(air->name + " atmosphere");
    }
    if (!body.gas)
    {
        add(std::to_string(static_cast<int>(std::round(body.temperature))) + " C");
    }
    return text.empty() ? std::string("Nothing known") : text + ".";
}

SiteTitle PredationGame::PlaceTitle()
{
    if (const StarSystem* system = CurrentSystem())
    {
        const Body* body = system->Find(m_campaign.travel.underway ? m_campaign.travel.target : m_campaign.body);
        if (body != nullptr)
        {
            SiteTitle title;
            title.planet = body->name;
            // On the site, the one stood on; aboard, the one chosen to go down to (the shuttle's screen, the briefing).
            const int region = m_map == MapChoice::Facility && m_campaign.region >= 0 ? m_campaign.region
                               : m_campaign.travel.region >= 0                       ? m_campaign.travel.region
                                                                                     : m_campaign.region;
            if (region >= 0 && region < static_cast<int>(body->regions.size()))
            {
                title.site = body->regions[static_cast<size_t>(region)].designation;
            }
            return title;
        }
    }
    return m_siteNames.For(m_facility.Seed());
}

SiteConditions PredationGame::PlaceConditions(float fogEnd)
{
    const StarSystem* system = CurrentSystem();
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    if (body == nullptr)
    {
        return ConditionsFor(m_facility.Seed(), fogEnd);
    }
    // The planet's own: how warm, how windy its weather is (somewhere in its range, the same each time for the region),
    // and how far its weather and its air let anybody see.
    SiteConditions conditions;
    conditions.temperature = static_cast<int>(std::round(body->temperature));
    const WeatherDef* weather = m_universeData.Weather(body->weather);
    const AtmosphereDef* air = m_universeData.Atmosphere(body->atmosphere);
    const float spread = static_cast<float>(m_facility.Seed() % 101u) / 100.0f;
    conditions.wind = weather != nullptr ? static_cast<int>(std::round(weather->wind.x + (weather->wind.y - weather->wind.x) * spread)) : 0;
    const float seeing = (weather != nullptr ? weather->visibility : 1.0f) * (air != nullptr ? air->visibility : 1.0f);
    conditions.visibility = seeing < 0.45f ? "POOR" : seeing < 0.8f ? "LOW" : seeing < 1.15f ? "FAIR" : "GOOD";
    // No air: nothing to blow, nothing in the way.
    conditions.air = body->air > 0.015f;
    if (!conditions.air)
    {
        conditions.wind = 0;
        conditions.visibility = "GOOD";
    }
    return conditions;
}

void PredationGame::ArriveAtBody()
{
    const StarSystem* system = CurrentSystem();
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    if (body == nullptr || !IsAuthority())
    {
        return;
    }
    m_campaign.AddLog("planet", CampaignState::BodyKey(m_campaign.system, body->index), body->name, "In orbit. " + SummaryOf(*body));
    ChooseLandingRegion(m_campaign.travel.region);
    CampaignChanged();
    PRED_LOG_INFO(Gameplay, "In orbit of {}", body->name);
}

// --- Under way ------------------------------------------------------------------------------------------------------

void PredationGame::UpdateTravel(float dt)
{
    const StarSystem* system = CurrentSystem();
    if (system == nullptr || m_screen != Screen::Playing)
    {
        return;
    }
    // The map, shared with everybody.
    UpdateSharedMap(dt);
    if (m_sessionMode == SessionMode::Client)
    {
        if (m_client.TravelsReceived() != m_appliedTravels)
        {
            m_appliedTravels = m_client.TravelsReceived();
            const TravelMessage& travel = m_client.LatestTravel();
            m_campaign.clock = travel.clock;
            m_campaign.system = travel.system;
            m_campaign.travel.underway = travel.underway;
            m_campaign.landed = travel.landed;
            m_campaign.travel.target = travel.target;
            m_campaign.travel.position = travel.position;
            m_campaign.travel.velocity = travel.velocity;
            m_campaign.travel.region = travel.region;
            m_campaign.travel.from = travel.from;
            m_campaign.travel.setOut = travel.setOut;
            m_campaign.travel.departWay = travel.departWay;
            m_campaign.travel.orbitOut = travel.orbitOut;
            m_campaign.travel.orbitSince = travel.orbitSince;
            m_campaign.travel.overFrom = travel.overFrom;
            m_campaign.travel.overAt = travel.overAt;
            m_campaign.travel.overVia = travel.overVia;
            m_campaign.travel.overSince = travel.overSince;
            m_campaign.travel.overSeconds = travel.overSeconds;
            m_campaign.body = travel.body;
        }
        else if (m_campaign.travel.underway && !m_campaign.travel.interstellar)
        {
            // Between the host's words, flown the same way here, so the map moves smoothly.
            Travel::Step(m_campaign, *system, dt, DriveTier());
        }
    }
    else
    {
        if (m_campaign.travel.interstellar)
        {
            // Between the stars: there when the time is up, at the edge of the new system, shown arriving.
            if (!CinematicHoldsWorld() && Travel::StepInterstellar(m_campaign, m_universe))
            {
                const StarSystem* arrived = CurrentSystem();
                m_campaign.Learn(m_campaign.system, -1, CampaignState::kKnownVisited);
                if (arrived != nullptr)
                {
                    m_campaign.AddLog("system", CampaignState::BodyKey(m_campaign.system, -1), arrived->name,
                                      "Arrived from across the stars. " + std::to_string(arrived->bodies.size()) + " bodies.");
                    PRED_LOG_INFO(Gameplay, "Arrived in {}", arrived->name);
                }
                CampaignChanged();
                // At the system's edge there is nothing to be shown arriving at (the arrival over a planet is for reaching
                // one): straight on to the world picked across the stars, if one was, or waiting here to be told where.
                const int onward = m_campaign.travel.onward;
                m_campaign.travel.onward = -1;
                if (arrived != nullptr && onward >= 0 && arrived->Find(onward) != nullptr)
                {
                    DoCampaignAction(LocalPlayerId(), CampaignAction::SetCourse, onward, -1);
                }
            }
        }
        else if (m_campaign.travel.underway && !CinematicHoldsWorld())
        {
            if (Travel::Step(m_campaign, *system, dt, DriveTier()))
            {
                CampaignChanged();
                if (m_campaign.body >= 0)
                {
                    // There, in orbit of it, or going straight down onto its outpost (StartDescent): seen from aboard, out of
                    // the windows, by everybody where they are -- nobody is taken outside the ship to watch it come in.
                    ArriveAtBody();
                }
                else
                {
                    PRED_LOG_INFO(Gameplay, "The ship has come to rest between the planets");
                }
            }
        }
        if (m_sessionMode == SessionMode::Host)
        {
            m_travelSendIn -= dt;
            if (m_travelSendIn <= 0.0f)
            {
                m_travelSendIn = kTravelSendEvery;
                TravelMessage travel;
                travel.clock = m_campaign.clock;
                travel.system = m_campaign.system;
                travel.underway = m_campaign.travel.underway;
                travel.landed = m_campaign.landed;
                travel.target = static_cast<int8_t>(std::clamp(m_campaign.travel.target, -1, 127));
                travel.body = static_cast<int8_t>(std::clamp(m_campaign.body, -1, 127));
                travel.region = static_cast<int8_t>(std::clamp(m_campaign.travel.region, -1, 127));
                travel.position = m_campaign.travel.position;
                travel.velocity = m_campaign.travel.velocity;
                travel.from = static_cast<int8_t>(std::clamp(m_campaign.travel.from, -1, 127));
                travel.setOut = m_campaign.travel.setOut;
                travel.departWay = m_campaign.travel.departWay;
                travel.orbitOut = m_campaign.travel.orbitOut;
                travel.orbitSince = m_campaign.travel.orbitSince;
                travel.overFrom = m_campaign.travel.overFrom;
                travel.overAt = m_campaign.travel.overAt;
                travel.overVia = m_campaign.travel.overVia;
                travel.overSince = m_campaign.travel.overSince;
                travel.overSeconds = m_campaign.travel.overSeconds;
                m_host.SendTravel(travel);
            }
        }
    }
    // The intercom, on every machine alike, from what it sees change: a course set, changed or called off.
    {
        const bool underway = m_campaign.travel.underway;
        const int target = m_campaign.travel.target;
        if (m_travelSeen)
        {
            if (underway && !m_travelSeenUnderway)
            {
                Say("course_set", 1.0f);
            }
            else if (underway && target != m_travelSeenTarget)
            {
                Say(target >= 0 ? "course_changed" : "course_stopped", 0.3f);
            }
            else if (const Body* reached = !underway && m_travelSeenUnderway && !m_campaign.landed ? system->Find(m_campaign.body) : nullptr)
            {
                // Come into orbit of a world: its name on the picture, and the intercom says so.
                ShowPlaceCard({reached->name}, 0.6f);
                Say("orbit", 1.6f);
            }
        }
        m_travelSeenUnderway = underway;
        m_travelSeenTarget = target;
        m_travelSeen = true;
    }
    UpdateShipGround();
    m_ship.UpdateAirlock(m_scene, m_app->GetMeshes(), dt, DoorwayBlocked());
    m_ship.UpdateFittings(m_scene, m_app->GetMeshes(), dt);
    for (const Entity control : m_airlockControls)
    {
        if (Interactable* door = m_interactions.Find(control))
        {
            door->enabled = m_map == MapChoice::Ship && !m_cine.Active() && !Descending() && (ShipLanded() || m_campaign.doorOpen);
            door->verb = m_campaign.doorOpen ? "Close" : "Open";
        }
    }
    if (Interactable* helm = m_interactions.Find(m_helm))
    {
        helm->enabled = m_campaign.plan.set && m_map == MapChoice::Ship && !m_cine.Active();
        helm->verb = m_campaign.travel.underway ? "Change course" : "Set out";
        helm->name = PlanName();
    }
    // The ship's outside as the campaign has it: its colours, and its drive and sensors.
    ShipHullLook look;
    look.primary = m_campaign.colors.primary;
    look.secondary = m_campaign.colors.secondary;
    look.accent = m_campaign.colors.accent;
    look.drive = DriveTier();
    look.sensors = SensorTier();
    m_ship.SetLook(m_scene, m_app->GetMeshes(), look);
    // What the ship's own systems go by: under way or not.
    m_shipTravel = m_campaign.travel.underway ? 1.0f : 0.0f;
    m_shipTravelTotal = m_shipTravel;
}

// --- Space out of the windows ------------------------------------------------------------------------------------

void PredationGame::ClearOutpostInteriors()
{
    if (m_interiorDoorsFrom != SIZE_MAX)
    {
        m_world.RemoveDoorsFrom(m_scene, m_app->GetPhysics(), m_interactions, m_interiorDoorsFrom);
    }
    m_interiorDoorsFrom = SIZE_MAX;
    m_outpostInteriors.Clear(m_scene, m_app->GetPhysics(), &m_levelLights);
}

void PredationGame::BuildOutpostGround(const FieldLook& field)
{
    // As a site's ground (SiteMap::BuildFrom), round a plan with nothing on it but what the outpost pours, levelled under it and
    // the land blending into it round about -- the world's own ground between its yards: a square round all of it, in the
    // ship's frame; no cliffs round it, the land running on; the way the ship comes in -- low over the pad's front, from far
    // out ahead of it -- kept low.
    // (Kestrel Station's the same way, its fence's square levelled and the road out of its gate: KestrelStation::Footprint.)
    Outpost::Layout layout;
    if (field.kestrel)
    {
        KestrelStation::Footprint(layout.slab, layout.levelled, layout.roads);
    }
    else
    {
        layout = Outpost::Generate(field.seed, field.ground, field.rock, field.style);
    }
    const glm::vec3 origin = ShipSpec::kOrigin;
    const float ground = origin.y + ShipSpec::kFieldGround;
    const glm::vec2 lo{layout.slab.x, layout.slab.y};
    const glm::vec2 hi{layout.slab.z, layout.slab.w};
    const glm::vec2 middle = (lo + hi) * 0.5f;
    SitePlan plan;
    // (Kestrel's, whose field has no seed of its own, the same in every campaign: the home world is.)
    constexpr uint32_t kKestrelGround = 0x4B35u;
    plan.seed = field.kestrel ? kKestrelGround : field.seed;
    plan.size = std::max(hi.x - lo.x, hi.y - lo.y) + 20.0f;
    plan.origin = {origin.x + middle.x - plan.size * 0.5f, ground, origin.z + middle.y - plan.size * 0.5f};
    plan.landing = {origin.x, ground + 0.5f, origin.z};
    for (const glm::vec4& rect : layout.levelled)
    {
        plan.levelled.push_back(rect + glm::vec4(origin.x, origin.z, origin.x, origin.z));
    }
    // Its road in, graded into the land, and what stands out on the land round it, stood where the ground is.
    for (SitePlan::Road road : layout.roads)
    {
        for (glm::vec2& point : road.line)
        {
            point += glm::vec2(origin.x, origin.z);
        }
        plan.roads.push_back(std::move(road));
    }
    if (glm::length(layout.windsock) > 0.0f)
    {
        plan.windsock = layout.windsock + origin;
    }
    for (SitePlan::Outlying piece : layout.outlying)
    {
        piece.at += glm::vec2(origin.x, origin.z);
        plan.outlying.push_back(std::move(piece));
    }
    plan.keepLow.push_back({origin.x - 80.0f, origin.z - 3000.0f, origin.x + 80.0f, origin.z});
    plan.openEdge = true;
    // Its world's own ground, weather and all; no water, none of the organism's signs.
    SiteMap::Look look = SiteLookHere();
    look.water = SiteTerrain::Water::None;
    look.signs = 0.0f;
    m_outpostGround.SetLook(look);
    m_outpostGround.BuildFrom(plan, static_cast<uint16_t>(plan.seed & 0xFFFFu), true, m_scene, m_app->GetMeshes(), m_app->GetPhysics(), nullptr);
    m_outpostGroundSeed = field.seed;
    // Its smoke and steam, by where they rise.
    m_outpostSmoke.clear();
    m_outpostSteam.clear();
    for (const glm::vec4& plume : layout.plumes)
    {
        (plume.w <= 0.0f ? m_outpostSmoke : m_outpostSteam).push_back({glm::vec3(plume) + origin, plume.w <= 0.0f ? 0.8f : plume.w});
    }
    // Its roofs, where no weather falls.
    m_outpostRoofs.clear();
    for (const Outpost::Layout::Roof& roof : layout.roofs)
    {
        m_outpostRoofs.push_back({roof.rect + glm::vec4(origin.x, origin.z, origin.x, origin.z), origin.y + roof.top});
    }
    PRED_LOG_INFO(Gameplay, "Outpost ground made round outpost {}: {:.0f} m across", field.seed, plan.size);
}

void PredationGame::UpdateWater()
{
    // A site's water when it is water (lava and acid are not swum in; ice is walked on), its surface where the site has it --
    // and the water standing in its flooded buildings (SiteMap::Flood), wherever there is a flood under the roof.
    const auto siteWater = [this]
    {
        return m_facility.GetLook().liquid == SiteMap::Look::Liquid::Water && (m_facility.Terrain().HasWater() || m_facility.Terrain().HasStreams());
    };
    const bool swimmable = m_facility.Built() && m_map == MapChoice::Facility && (siteWater() || m_facility.Flooded());
    if (swimmable != m_swimWaterSet)
    {
        m_swimWaterSet = swimmable;
        m_swimWater = WaterQuery{};
        if (swimmable)
        {
            m_swimWater.depth = [this, siteWater](const glm::vec3& at)
            {
                float surface = 0.0f;
                bool standing = false;
                const float depth = m_facility.WaterAt(at, surface, &standing);
                return standing || siteWater() ? depth : -1.0f;
            };
            // (A stream's surface where one runs: it falls as it goes, and is no one level. A flood's, its own.)
            m_swimWater.surface = [this](const glm::vec3& at)
            {
                float surface = m_facility.Terrain().WaterLevel();
                m_facility.WaterAt(at, surface);
                return surface;
            };
            // (The waves only on the sea or a lake, not a stream -- nor a flood, still under its roof.)
            m_swimWater.rise = [this](float x, float z)
            {
                const SiteTerrain& terrain = m_facility.Terrain();
                float flooded = 0.0f;
                // (Nor in a moon pool, under a building on the sea's floor.)
                if (!terrain.HasWater() || terrain.WaterSurface(x, z) > terrain.WaterLevel() + 0.01f || m_facility.Flood(x, z, flooded) > 0.0f ||
                    m_facility.UnderBuilding(x, z))
                {
                    return 0.0f;
                }
                return m_seaWaves.Rise({x, z}, m_waveClock, terrain.WaterDepth(x, z), terrain.WaterLineSpacing(x, z));
            };
        }
    }
    if (swimmable)
    {
        m_swimWater.level = m_facility.Terrain().WaterLevel();
        // The waves its wind raises -- higher in a storm, and on the open sea far higher than a lake has room for -- by the
        // campaign's clock, so everybody's are the same.
        const SiteMap::Look& look = m_facility.GetLook();
        const Weather::Kind kind = Weather::KindOf(look.weather);
        const float storm = kind == Weather::Kind::Rain || kind == Weather::Kind::Snow ? glm::smoothstep(0.55f, 1.0f, look.weatherAmount) : 0.0f;
        const bool openSea = look.water == SiteTerrain::Water::Coast || look.water == SiteTerrain::Water::Island || look.water == SiteTerrain::Water::Seabed;
        m_seaWaves = Waves::Raised(static_cast<float>(PlaceConditions(m_facility.Plan().sky.fogEnd).wind), SiteMap::WindAngle(m_facility.Seed()),
                                   storm, openSea, m_facility.Seed(), look.water == SiteTerrain::Water::Seabed);
        m_waveClock = static_cast<float>(std::fmod(m_campaign.clock, 3600.0));
    }
    else
    {
        m_seaWaves = Waves{};
    }
}

void PredationGame::UpdateLockouts()
{
    // Each lockout as full of the sea as its doors have it (SiteMap::Lockout): its door into the sea open, full; its door into the
    // building open, empty; while one waits to open, the air cycling (WorldObjects::kAirlockCycle) -- the sea let in as the far one
    // waits, let out as the near one does. The same on every machine: the doors are.
    const std::vector<SiteMap::Lockout>& lockouts = m_facility.Lockouts();
    const std::vector<WorldObjects::Door>& doors = m_world.Doors();
    for (size_t k = 0; k < lockouts.size(); ++k)
    {
        const SiteMap::Lockout& lockout = lockouts[k];
        const size_t inner = m_facilityDoorsFirst + lockout.inner;
        const size_t outer = m_facilityDoorsFirst + lockout.outer;
        if (inner >= doors.size() || outer >= doors.size())
        {
            continue;
        }
        const WorldObjects::Door& in = doors[inner];
        const WorldObjects::Door& out = doors[outer];
        float level = lockout.level;
        if (out.angle != out.closedYaw)
        {
            level = 1.0f;
        }
        else if (in.angle != in.closedYaw)
        {
            level = 0.0f;
        }
        else if (out.openIn >= 0.0f)
        {
            level = std::max(level, 1.0f - out.openIn / WorldObjects::kAirlockCycle);
        }
        else if (in.openIn >= 0.0f)
        {
            level = std::min(level, in.openIn / WorldObjects::kAirlockCycle);
        }
        m_facility.SetLockoutLevel(k, level);
    }
}

void PredationGame::UpdateAirlocks(float dt)
{
    // An airlock shuts the door behind whoever has come into it by it: once everybody in it is well clear of it (out of its
    // swing), two seconds on, that door closes by itself -- so the far door is ready to be cycled. Never the door opened from inside to
    // leave by, never with
    // anybody in the doorway or about it, nor while the air cycles. The host decides, as for every door; what it does is shared.
    const std::vector<SiteMap::Lockout>& airlocks = m_facility.Airlocks();
    m_airlockHeld.resize(airlocks.size(), 0.0f);
    m_airlockOpen.resize(airlocks.size(), 0);
    if (m_sessionMode == SessionMode::Client || !m_facility.Built())
    {
        return;
    }
    std::vector<glm::vec3> people;
    if (m_player.State().alive)
    {
        people.push_back(m_player.State().position);
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.alive)
        {
            people.push_back(remote.position);
        }
    }
    const std::vector<WorldObjects::Door>& doors = m_world.Doors();
    for (size_t k = 0; k < airlocks.size(); ++k)
    {
        const SiteMap::Lockout& airlock = airlocks[k];
        const size_t inner = m_facilityDoorsFirst + airlock.inner;
        const size_t outer = m_facilityDoorsFirst + airlock.outer;
        if (inner >= doors.size() || outer >= doors.size())
        {
            continue;
        }
        const WorldObjects::Door& in = doors[inner];
        const WorldObjects::Door& out = doors[outer];
        const bool inOpen = std::abs(in.target - in.closedYaw) > 1e-3f;
        const bool outOpen = std::abs(out.target - out.closedYaw) > 1e-3f;
        // (Exactly one open and nothing cycling: the door somebody came in by.)
        if (inOpen == outOpen || in.openIn >= 0.0f || out.openIn >= 0.0f)
        {
            m_airlockHeld[k] = 0.0f;
            m_airlockOpen[k] = 0;
            continue;
        }
        const WorldObjects::Door& open = inOpen ? in : out;
        bool inside = false;
        bool near = false;
        for (const glm::vec3& at : people)
        {
            const bool within = at.x > airlock.min.x + 0.2f && at.x < airlock.max.x - 0.2f && at.z > airlock.min.z + 0.2f && at.z < airlock.max.z - 0.2f &&
                                at.y > airlock.min.y - 0.5f && at.y < airlock.max.y;
            inside = inside || within;
            near = near || glm::distance(glm::vec2(at.x, at.z), glm::vec2(open.hinge.x, open.hinge.z)) < 1.7f;
        }
        // Which way it was opened: from inside (on the way out -- never shut on whoever opened it) or from outside (on the way in).
        if (m_airlockOpen[k] == 0)
        {
            m_airlockOpen[k] = inside ? 2 : 1;
        }
        m_airlockHeld[k] = m_airlockOpen[k] == 1 && inside && !near ? m_airlockHeld[k] + dt : 0.0f;
        if (m_airlockHeld[k] < 2.0f)
        {
            continue;
        }
        m_airlockHeld[k] = 0.0f;
        const int index = static_cast<int>(inOpen ? inner : outer);
        m_world.SetDoorOpen(index, false, m_interactions);
        PlaySound(m_sounds.door.Pick(), open.hinge, 0.8f);
        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::DoorMoved;
            event.index = static_cast<uint8_t>(index);
            event.flag = false;
            m_host.Broadcast(event);
        }
    }
}

void PredationGame::MarineSnow(std::vector<Sprite>& out) const
{
    // Under water, what the water carries: flecks of it close round the eye, each drifting slowly down and along with the current
    // and swaying a little, seen faintly by the murk's own light -- and bright where the torch's beam catches them, as a diver's
    // light lights up the water in front of it. Each in a cell of a grid carried along with the drift, so they stay put as the eye
    // moves among them. Fogged as the world is.
    const Environment& lit = m_scene.GetEnvironment();
    const glm::vec3 eye = m_renderEye;
    constexpr float kCell = 1.25f;
    constexpr int kReach = 4;
    const float t = static_cast<float>(std::fmod(m_campaign.clock, 3600.0));
    const glm::vec3 shift = glm::vec3(0.05f, -0.03f, 0.035f) * t;
    const glm::ivec3 centre = glm::ivec3(glm::floor((eye - shift) / kCell));
    const PunctualLight& torch = lit.lights[0];
    const bool torchOn = torch.intensity > 0.0f && torch.range > 0.0f && torch.outerAngle < 89.0f;
    const glm::vec3 aim = torchOn && glm::length(torch.direction) > 1.0e-4f ? glm::normalize(torch.direction) : glm::vec3(0.0f, -1.0f, 0.0f);
    const float outer = std::cos(glm::radians(std::max(torch.outerAngle, 1.0f)));
    const float inner = std::cos(glm::radians(std::max(torch.innerAngle, 0.5f)));
    const float soak = std::max(SiteLightReach() * 0.5f, 0.8f);
    const auto hash = [](glm::ivec3 c, uint32_t salt)
    {
        uint32_t h = static_cast<uint32_t>(c.x) * 0x8DA6B343u ^ static_cast<uint32_t>(c.y) * 0xD8163841u ^ static_cast<uint32_t>(c.z) * 0xCB1AB31Fu ^ salt * 0x9E3779B9u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        return static_cast<float>(h & 0xFFFFu) / 65535.0f;
    };
    const float most = kCell * static_cast<float>(kReach);
    for (int z = -kReach; z <= kReach; ++z)
    {
        for (int y = -kReach; y <= kReach; ++y)
        {
            for (int x = -kReach; x <= kReach; ++x)
            {
                const glm::ivec3 cell = centre + glm::ivec3(x, y, z);
                if (hash(cell, 1u) > 0.55f)
                {
                    continue;
                }
                glm::vec3 at = (glm::vec3(cell) + glm::vec3(hash(cell, 2u), hash(cell, 3u), hash(cell, 4u))) * kCell + shift;
                const float phase = hash(cell, 5u) * 6.2831853f;
                at += glm::vec3(std::sin(t * 0.37f + phase), std::sin(t * 0.23f + phase * 1.7f) * 0.5f, std::cos(t * 0.31f + phase)) * 0.07f;
                const float away = glm::length(at - eye);
                if (away > most || away < 0.2f || m_swimWater.depth(at) <= 0.0f || at.y > m_swimWater.SurfaceAt(at) - 0.05f)
                {
                    continue;
                }
                glm::vec3 colour = lit.fogColor * 1.7f;
                float alpha = 0.22f;
                if (torchOn)
                {
                    const glm::vec3 from = at - torch.position;
                    const float d = glm::length(from);
                    const float cone = glm::smoothstep(outer, inner, glm::dot(from / std::max(d, 1.0e-3f), aim));
                    const float light = torch.intensity * cone / std::max(d * d, 0.6f) * std::exp(-2.0f * d / soak) * 0.04f;
                    colour += torch.color * light * glm::vec3(0.6f, 0.85f, 1.0f);
                    alpha += std::min(cone * 0.5f, 0.45f) * std::exp(-d / soak);
                }
                const float fade = 1.0f - glm::smoothstep(most * 0.6f, most, away);
                const float fogged = std::clamp((away - lit.fogStart) / std::max(lit.fogEnd - lit.fogStart, 1.0e-3f), 0.0f, 1.0f);
                Sprite fleck;
                fleck.at = at;
                fleck.colour = glm::mix(colour, lit.fogColor, fogged);
                fleck.alpha = alpha * fade;
                fleck.size = 0.012f + 0.022f * hash(cell, 6u);
                out.push_back(fleck);
            }
        }
    }
}

float PredationGame::SiteLightReach() const
{
    // The open sea, clear, lets the day down tens of metres; a lake, a stream or a flood only a couple.
    const SiteTerrain::Water water = m_facility.GetLook().water;
    const bool sea = water == SiteTerrain::Water::Coast || water == SiteTerrain::Water::Island || water == SiteTerrain::Water::Seabed;
    glm::vec3 tint;
    float clarity = 1.0f;
    SiteWaterLook(tint, clarity);
    return sea && m_facility.GetLook().liquid == SiteMap::Look::Liquid::Water ? clarity * 3.0f : 2.0f;
}

void PredationGame::SiteWaterLook(glm::vec3& tint, float& clarity) const
{
    const SiteMap::Look& look = m_facility.GetLook();
    if (look.liquid == SiteMap::Look::Liquid::Acid)
    {
        tint = {0.16f, 0.21f, 0.02f};
        clarity = 0.6f;
    }
    else if (look.biome == "jungle")
    {
        tint = {0.035f, 0.045f, 0.02f};
        clarity = 1.0f;
    }
    else if (look.water == SiteTerrain::Water::Lakes)
    {
        tint = {0.03f, 0.045f, 0.035f};
        clarity = 2.0f;
    }
    else
    {
        tint = {0.02f, 0.055f, 0.07f};
        clarity = 4.0f;
    }
}

void PredationGame::UpdateShipGround()
{
    const StarSystem* system = CurrentSystem();
    if (system == nullptr)
    {
        return;
    }
    // Standing at a hub: its legs down, the station round it, the world's ground in its own colours, the door open.
    {
        // (Or showing an outpost to look at it, wherever the ship is: outpost_preview.)
        const bool down = (ShipLanded() || m_outpostPreview != 0) && system->Find(m_campaign.body) != nullptr;
        if (down)
        {
            m_groundBody = m_campaign.body;
            m_groundRegion = m_campaign.travel.region;
        }
        // (And while the ship lifts off it, still there below it, falling away: StartDescent.)
        const bool landed = down || (Descending() && !m_descentLanding && system->Find(m_groundBody) != nullptr);
        const Body* at = system->Find(m_groundBody);
        glm::vec3 ground{0.4f};
        glm::vec3 rock{0.3f};
        if (const BiomeDef* biome = at != nullptr ? m_universeData.Biome(at->biome) : nullptr)
        {
            ground = biome->siteGround;
            rock = biome->siteRock;
        }
        // Kestrel Station on the home world, built by hand; any other world's outpost planned from its own seed, its name on it.
        FieldLook field;
        field.ground = ground;
        field.rock = rock;
        const StarSystem* home = m_universe.System(m_universe.Home());
        field.kestrel = home != nullptr && m_campaign.system == home->id.Packed() && m_groundBody == home->hub;
        // Its name over the operations block's door, and its operator's number under it where its owner goes by one.
        std::vector<std::string> name;
        if (at != nullptr && m_groundRegion >= 0 && m_groundRegion < static_cast<int>(at->regions.size()))
        {
            const LandingRegion& region = at->regions[static_cast<size_t>(m_groundRegion)];
            field.seed = region.seed;
            field.style = StyleOf(region);
            name = {region.designation};
            if (const std::string number = OperatorNumber(region); !number.empty())
            {
                name.push_back("OPERATOR " + number);
            }
        }
        if (field.kestrel)
        {
            field.seed = 0;
            field.style = OutpostStyle{};
        }
        if (m_outpostPreview != 0)
        {
            LandingRegion preview;
            preview.seed = m_outpostPreview;
            preview.designation = "OUTPOST " + std::to_string(10 + m_outpostPreview % 90u);
            preview.operatorNumber = static_cast<int>(100 + m_outpostPreview % 9900u);
            // Its owner as asked, or one by its seed.
            preview.owner = m_outpostPreviewOwner;
            if (preview.owner.empty() && !m_universeData.owners.empty())
            {
                preview.owner = m_universeData.owners[m_outpostPreview % m_universeData.owners.size()].id;
            }
            field.kestrel = false;
            field.seed = m_outpostPreview;
            field.style = StyleOf(preview);
            name = {preview.designation};
            if (const std::string number = OperatorNumber(preview); !number.empty())
            {
                name.push_back("OPERATOR " + number);
            }
        }
        m_ship.SetField(m_scene, m_app->GetMeshes(), landed, field);
        // And the world's own ground round it, at an outpost and at Kestrel Station.
        const bool outpostGround = landed;
        if (outpostGround && (!m_outpostGround.Built() || m_outpostGroundSeed != field.seed))
        {
            BuildOutpostGround(field);
        }
        else if (!outpostGround && m_outpostGround.Built())
        {
            m_outpostGround.Clear(m_scene, m_app->GetPhysics(), nullptr);
        }
        // Its buildings to go into (OutpostInteriors) -- another world's outpost, not Kestrel's -- stood in for at once, then built inside
        // one every so many frames, in their order: the same on every machine.
        // (Not while the ship is coming down onto it or going up off it: nobody goes into them then.)
        const bool interiors = landed && !Descending() && !field.kestrel && m_outpostGround.Built();
        if (m_outpostInteriors.Planned() && (!interiors || !(m_interiorsLook == field)))
        {
            ClearOutpostInteriors();
        }
        if (interiors && !m_outpostInteriors.Planned())
        {
            m_interiorsLook = field;
            m_outpostInteriors.Set(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), &m_levelLights,
                                   Outpost::Generate(field.seed, field.ground, field.rock, field.style).buildings, ShipSpec::kOrigin, m_ship.FieldGroup());
            m_interiorWait = 30;
        }
        else if (interiors && !m_outpostInteriors.Done() && --m_interiorWait <= 0)
        {
            WorldObjects::Placements doors;
            if (m_outpostInteriors.BuildNext(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), &m_levelLights, doors))
            {
                if (m_interiorDoorsFrom == SIZE_MAX)
                {
                    m_interiorDoorsFrom = m_world.DoorCount();
                }
                m_world.AddFacility(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, doors);
            }
            m_interiorWait = 20;
        }
        m_ship.SetGear(landed, false);
        m_ship.SetStair(landed, false);
        m_ship.SetAirlockOpen(m_scene, m_app->GetMeshes(), landed && m_campaign.doorOpen && !Descending());
        UpdateHubSigns(landed, field, name);
    }
}

Entity PredationGame::MakeSign(const KestrelStation::Sign& sign, const std::string& meshName, TextureHandle texture, int wide, int high, int textureWide,
                               int textureHigh, MeshHandle& mesh)
{
    // A flat panel facing +z, both faces, its picture the part of the texture drawn into (wide by high of it; half a texel in
    // from the edge, where what is beyond would be blended in).
    MeshData quad;
    const float w = sign.width * 0.5f;
    const float h = sign.height * 0.5f;
    const float u = (static_cast<float>(wide) - 0.5f) / static_cast<float>(textureWide);
    const float v = (static_cast<float>(high) - 0.5f) / static_cast<float>(textureHigh);
    const glm::vec3 normal{0.0f, 0.0f, 1.0f};
    quad.vertices.push_back(MeshVertex{{-w, h, 0.0f}, normal, {0.0f, 0.0f}});
    quad.vertices.push_back(MeshVertex{{w, h, 0.0f}, normal, {u, 0.0f}});
    quad.vertices.push_back(MeshVertex{{w, -h, 0.0f}, normal, {u, v}});
    quad.vertices.push_back(MeshVertex{{-w, -h, 0.0f}, normal, {0.0f, v}});
    quad.indices = {0, 3, 2, 0, 2, 1, 0, 1, 2, 0, 2, 3};
    mesh = m_app->GetMeshes().Upload(quad, meshName);
    Transform at;
    at.position = ShipMap::ToWorld(sign.at);
    at.rotation = glm::angleAxis(glm::radians(sign.yaw), glm::vec3(0.0f, 1.0f, 0.0f));
    // Paint on the ground lies flat, its top where it faces.
    const bool floor = sign.style == KestrelStation::Sign::Style::Floor;
    if (floor)
    {
        at.rotation = at.rotation * glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
    }
    const bool logo = sign.style == KestrelStation::Sign::Style::Logo;
    // Painted, or lit no longer: only as light falls on it.
    const bool painted = sign.style == KestrelStation::Sign::Style::Painted || floor || sign.dark;
    Material material = Material::Diffuse(sign.dark ? glm::vec3(0.45f) : glm::vec3(1.0f), painted ? 0.9f : 0.4f);
    material.baseColorTexture = texture;
    material.emissive = painted ? glm::vec3(0.0f) : glm::vec3(logo ? 1.4f : 1.1f);
    material.emissiveTextured = !painted;
    const Entity entity = m_scene.CreateMeshEntity("sign_" + sign.id, at, mesh, material);
    if (MeshRenderer* renderer = m_scene.GetMeshRenderer(entity))
    {
        renderer->castsShadow = false;
    }
    return entity;
}

OutpostStyle PredationGame::StyleOf(const LandingRegion& region) const
{
    OutpostStyle style;
    const OwnerDef* owner = m_universeData.Owner(region.owner);
    if (owner == nullptr)
    {
        return style;
    }
    style.mark = owner->mark;
    style.standard = owner->standard;
    style.abandoned = owner->abandoned;
    style.lit = owner->lit;
    style.operations = owner->operations;
    style.blocks = owner->blocks;
    style.sheds = owner->sheds;
    style.habitats = owner->habitats;
    style.tanks = owner->tanks;
    style.comms = owner->comms;
    style.uses = owner->uses;
    return style;
}

std::string PredationGame::OperatorNumber(const LandingRegion& region) const
{
    const OwnerDef* owner = m_universeData.Owner(region.owner);
    if (owner == nullptr || !owner->operatorNumber)
    {
        return {};
    }
    std::string number = std::to_string(std::clamp(region.operatorNumber, 0, 9999));
    return std::string(4 - std::min<size_t>(number.size(), 4), '0') + number;
}

std::string PredationGame::OperatorOf(const LandingRegion& region) const
{
    const OwnerDef* owner = m_universeData.Owner(region.owner);
    if (owner == nullptr)
    {
        return {};
    }
    std::string text = owner->map;
    if (const size_t at = text.find("{operator}"); at != std::string::npos)
    {
        text.replace(at, 10, OperatorNumber(region));
    }
    return text;
}

std::string PredationGame::OutpostTag(const LandingRegion& region) const
{
    std::string tag = "OUTPOST";
    if (const OwnerDef* owner = m_universeData.Owner(region.owner))
    {
        std::string name = owner->name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        tag += ", " + name;
    }
    return tag + "   ";
}

void PredationGame::UpdateHubSigns(bool shown, const FieldLook& field, const std::vector<std::string>& name)
{
    TextureLibrary& textures = m_app->GetTextures();
    // Kestrel's, made once, the first time the ship stands there; shown and hidden after.
    if (shown && field.kestrel && m_hubSigns.empty())
    {
        for (const KestrelStation::Sign& sign : KestrelStation::Signs(true))
        {
            const bool logo = sign.style == KestrelStation::Sign::Style::Logo;
            const int high = logo ? 256 : 128;
            const int wide = std::clamp(static_cast<int>(static_cast<float>(high) * sign.width / std::max(sign.height, 0.1f)), 64, 2048);
            ScreenCanvas canvas(wide, high);
            KestrelStation::Draw(canvas, sign);
            const TextureHandle texture = textures.CreateDynamic(wide, high, "sign_" + sign.id);
            textures.Update(texture, canvas.image);
            MeshHandle mesh;
            m_hubSigns.push_back(MakeSign(sign, "sign_" + sign.id, texture, wide, high, wide, high, mesh));
        }
    }
    // Another outpost's, made again whenever it is another outpost: what its plan puts up, and its name on its operations block.
    // Each sign drawn into a texture kept for its place in the list, as wide as any sign is, so going from outpost to outpost
    // makes no more of them.
    if (shown && !field.kestrel && (!m_outpostSignsBuilt || m_outpostSignsSeed != field.seed || m_outpostSignsName != name || !(m_outpostSignsStyle == field.style)))
    {
        for (const Entity entity : m_outpostSigns)
        {
            m_scene.Destroy(entity);
        }
        for (const MeshHandle mesh : m_outpostSignMeshes)
        {
            m_app->GetMeshes().Release(mesh);
        }
        m_outpostSigns.clear();
        m_outpostSignMeshes.clear();
        m_outpostSignsBuilt = true;
        // (Made where they stand: as far down as the field is drawn is put on them from here.)
        m_signsShift = glm::vec3(0.0f);
        m_outpostSignsSeed = field.seed;
        m_outpostSignsName = name;
        m_outpostSignsStyle = field.style;
        constexpr int kTextureWide = 1024;
        constexpr int kHigh = 128;
        std::vector<KestrelStation::Sign> signs = Outpost::Generate(field.seed, field.ground, field.rock, field.style).signs;
        for (size_t i = 0; i < signs.size(); ++i)
        {
            KestrelStation::Sign& sign = signs[i];
            if (sign.id == "outpost_name")
            {
                sign.lines = name;
            }
            const int wide = std::clamp(static_cast<int>(static_cast<float>(kHigh) * sign.width / std::max(sign.height, 0.1f)), 64, kTextureWide);
            ScreenCanvas canvas(wide, kHigh);
            KestrelStation::Draw(canvas, sign);
            const TextureHandle texture = textures.CreateDynamic(kTextureWide, kHigh, "outpost_sign_" + std::to_string(i));
            textures.Update(texture, canvas.image);
            MeshHandle mesh;
            m_outpostSigns.push_back(MakeSign(sign, "outpost_sign_" + std::to_string(field.seed) + "_" + sign.id, texture, wide, kHigh, kTextureWide, kHigh, mesh));
            m_outpostSignMeshes.push_back(mesh);
        }
    }
    for (const Entity entity : m_hubSigns)
    {
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(entity))
        {
            renderer->visible = shown && field.kestrel;
        }
    }
    for (const Entity entity : m_outpostSigns)
    {
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(entity))
        {
            renderer->visible = shown && !field.kestrel;
        }
    }
}

std::string PredationGame::LeavingBlocked() const
{
    if (!ShipLanded())
    {
        return {};
    }
    // Nobody is left behind on the pad: everybody up is aboard.
    int outside = m_player.State().alive && !ShipMap::Aboard(m_player.State().position) ? 1 : 0;
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        outside += remote.alive && !ShipMap::Aboard(remote.position) ? 1 : 0;
    }
    if (outside == 0)
    {
        return {};
    }
    return outside == 1 ? "Not everybody is aboard: one of the crew is still outside" : "Not everybody is aboard: " + std::to_string(outside) + " of the crew are outside";
}

bool PredationGame::DoorwayBlocked() const
{
    if (m_player.State().alive && ShipMap::InDoorway(m_player.State().position))
    {
        return true;
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.alive && ShipMap::InDoorway(remote.position))
        {
            return true;
        }
    }
    return false;
}

bool PredationGame::ShipLanded() const
{
    return m_campaignOpen && m_campaign.landed && !m_campaign.travel.underway && m_campaign.body >= 0;
}

float PredationGame::LaunchDive() const
{
    if (!m_cine.Active())
    {
        return 0.0f;
    }
    // From the nose going down at it, after the drop out of the bay, to the heat at its height (the track's go_to_site); and
    // back up from it to the ship, out of the heat with the world filling everything behind, falling away below.
    const std::string& name = m_cine.Playing().name;
    if (name == "ship_launch")
    {
        return glm::smoothstep(11.5f, std::max(m_cine.Playing().duration - 0.3f, 12.0f), m_cine.Time());
    }
    if (name == "ship_docking")
    {
        return 1.0f - glm::smoothstep(0.0f, 10.0f, m_cine.Time());
    }
    return 0.0f;
}

void PredationGame::PlayLeaving(bool fromGround)
{
    // Up off a hub's pad: seen from aboard, the field falling away under it (StartDescent). Out of orbit, nobody is taken out
    // of the ship to watch it go: the engines light, the dust starts past the windows, the intercom says so -- however good
    // the drive.
    if (fromGround && m_map == MapChoice::Ship)
    {
        StartDescent(false);
    }
    else if (m_map == MapChoice::Ship)
    {
        PlayNamed("World/shuttle_launch", ShipMap::ToWorld({0.0f, 1.0f, 22.0f}), 0.8f, 0.55f, false);
    }
}

namespace
{
// Coming down onto a field and lifting off it: how long, and from how high it is first seen.
constexpr float kLandSeconds = 30.0f;
constexpr float kLiftSeconds = 26.0f;
// And how far along its way the ship is for every metre it is up: coming in to the field from ahead on a long slope, and going
// on out ahead over it climbing away (it was straight up and down, as on a lift).
constexpr float kDescentAlong = 2.2f;
constexpr float kDescentHeight = 3500.0f;
} // namespace

float PredationGame::DescentDrop() const
{
    if (!Descending())
    {
        return 0.0f;
    }
    // Fast from high up, slowing all the way down to the ground (and the other way going up), never a jolt at either end
    // but the gear's.
    if (m_descentLanding)
    {
        const float s = std::clamp(m_descentTime / kLandSeconds, 0.0f, 1.0f);
        return kDescentHeight * std::pow(1.0f - s, 2.6f);
    }
    const float s = std::clamp(m_descentTime / kLiftSeconds, 0.0f, 1.0f);
    return kDescentHeight * std::pow(s, 2.6f);
}

float PredationGame::DescentDive() const
{
    if (!Descending())
    {
        return 0.0f;
    }
    // The world below growing from as it is from orbit to the horizon, over the first part of the way down (the last of the
    // way up), done by the time the burning air round the nose is at its height and the sky turns from space to its own under
    // it (DescentHeat).
    if (m_descentLanding)
    {
        return glm::smoothstep(0.0f, 0.17f, m_descentTime / kLandSeconds);
    }
    return 1.0f - glm::smoothstep(0.83f, 1.0f, m_descentTime / kLiftSeconds);
}

float PredationGame::DescentHeat()
{
    if (!Descending())
    {
        return 0.0f;
    }
    // Into the air from orbit, the air burning over the ship's nose -- at its height as the world under it turns from the globe
    // seen from space into the land it is coming down to (the sky from space to its own, about 2 km up); going up, a lighter glow
    // of it as it leaves the air the same way. None on a world without any.
    const StarSystem* system = CurrentSystem();
    const Body* below = system != nullptr ? system->Find(m_groundBody) : nullptr;
    const float air = below != nullptr ? std::clamp((below->air - 0.015f) * 6.0f, 0.0f, 1.0f) : 0.0f;
    if (m_descentLanding)
    {
        const float s = m_descentTime / kLandSeconds;
        return air * glm::smoothstep(0.03f, 0.16f, s) * (1.0f - glm::smoothstep(0.21f, 0.34f, s));
    }
    const float s = m_descentTime / kLiftSeconds;
    return air * 0.75f * glm::smoothstep(0.7f, 0.8f, s) * (1.0f - glm::smoothstep(0.86f, 0.97f, s));
}

float PredationGame::DescentShake() const
{
    if (!Descending())
    {
        return 0.0f;
    }
    // A steady rumble from the engines; hardest coming in to the ground and pulling off it, and the bump of the gear taking
    // the weight.
    if (m_descentLanding)
    {
        const float s = std::clamp(m_descentTime / kLandSeconds, 0.0f, 1.0f);
        const float bump = std::exp(-std::pow((m_descentTime - kLandSeconds) * 5.0f, 2.0f));
        return 0.2f + 0.45f * glm::smoothstep(0.75f, 0.97f, s) * (1.0f - glm::smoothstep(0.985f, 1.0f, s)) + 0.8f * bump;
    }
    const float s = std::clamp(m_descentTime / kLiftSeconds, 0.0f, 1.0f);
    return 0.2f + 0.55f * (1.0f - glm::smoothstep(0.0f, 0.3f, s)) * glm::smoothstep(0.0f, 0.02f, s);
}

void PredationGame::StartDescent(bool landing)
{
    m_descentTime = 0.0f;
    m_descentLanding = landing;
    m_descentSounds = 0;
    PRED_LOG_INFO(Gameplay, "The ship {} the field", landing ? "comes down onto" : "lifts off");
    // Anybody still out on the field when it goes up is brought aboard first: nobody is left standing in the air.
    if (!landing && m_player.State().alive && !ShipMap::Aboard(m_player.State().position))
    {
        m_player.Teleport(m_ship.Spawn(LocalPlayerId()));
    }
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::Cinematic;
        event.item = SoundKey(landing ? "ship_land" : "ship_takeoff");
        event.flag = true;
        m_host.Broadcast(event);
    }
}

void PredationGame::UpdateDescent(float dt)
{
    if (!Descending())
    {
        return;
    }
    m_descentTime += dt;
    // (Gone from the world it was lifting off before it was clear of it -- under way and arrived somewhere else: over.)
    if (!m_descentLanding && !m_campaign.travel.underway && !ShipLanded() && m_campaign.body != m_groundBody)
    {
        m_descentTime = kLiftSeconds;
    }
    const float total = m_descentLanding ? kLandSeconds : kLiftSeconds;
    // The engines: hard at the start of either, eased at the ground; and their roar, again as it comes in.
    const float s = std::clamp(m_descentTime / total, 0.0f, 1.0f);
    m_ship.SetEngines(m_scene, m_descentLanding ? glm::mix(0.5f, 1.0f, glm::smoothstep(0.6f, 0.95f, s)) * (1.0f - glm::smoothstep(0.98f, 1.0f, s))
                                                : glm::mix(1.0f, 0.4f, s));
    // (Their running is a loop, turned up and down with them: UpdateEngineSounds. Lifting off, the roar of them lighting up first.)
    const glm::vec3 engines = ShipMap::ToWorld({0.0f, 1.0f, 22.0f});
    if (m_descentSounds == 0 && !m_descentLanding)
    {
        PlayNamed("World/shuttle_launch", engines, 0.9f, 0.55f, false);
        ++m_descentSounds;
    }
    if (m_descentTime >= total)
    {
        if (m_descentLanding)
        {
            PlayNamed("World/door_slam", engines, 0.8f, 0.5f, false);
        }
        m_ship.SetEngines(m_scene, 0.0f);
        m_descentTime = -1.0f;
    }
    // What is below: as far down as the ship is up, and ahead of it coming in (behind it going): the ship's bow is the way of its -z.
    const float drop = DescentDrop();
    const glm::vec3 shift{0.0f, -drop, (m_descentLanding ? -1.0f : 1.0f) * kDescentAlong * drop};
    m_ship.SetFieldDrop(m_scene, shift);
    if (m_outpostGround.Built())
    {
        m_outpostGround.SetDrop(m_scene, shift);
    }
    for (std::vector<Entity>* signs : {&m_hubSigns, &m_outpostSigns})
    {
        for (const Entity sign : *signs)
        {
            if (Transform* transform = m_scene.GetTransform(sign))
            {
                transform->position += shift - m_signsShift;
            }
        }
    }
    m_signsShift = shift;
}

void PredationGame::SendMapHolder(int player)
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }
    WorldEventMessage event;
    event.kind = WorldEventKind::MapHeld;
    event.player = m_mapHolder;
    if (player < 0)
    {
        m_host.Broadcast(event);
    }
    else
    {
        m_host.SendTo(static_cast<uint8_t>(player), event);
    }
}

std::string PredationGame::NameOfPlayer(uint8_t player) const
{
    if (player == LocalPlayerId())
    {
        return PlayerName();
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& other : m_host.Remotes())
        {
            if (other.id == player)
            {
                return other.name;
            }
        }
    }
    else
    {
        for (const NetClient::KnownPeer& peer : m_client.Peers())
        {
            if (peer.id == player)
            {
                return peer.name;
            }
        }
    }
    return "Somebody";
}

float PredationGame::TravelTimeLeft()
{
    // Flown ahead twice a second of the campaign's clock, counted down between.
    const StarSystem* system = CurrentSystem();
    if (system == nullptr || !m_campaign.travel.underway)
    {
        return 0.0f;
    }
    if (m_timeLeftAt < 0.0 || m_campaign.clock < m_timeLeftAt || m_campaign.clock - m_timeLeftAt > 0.5)
    {
        m_timeLeft = Travel::TimeLeft(m_campaign, *system, DriveTier());
        m_timeLeftAt = m_campaign.clock;
    }
    return std::max(m_timeLeft - static_cast<float>(m_campaign.clock - m_timeLeftAt), 0.0f);
}

std::string PredationGame::PlanName()
{
    const CampaignState::Plan& plan = m_campaign.plan;
    if (!plan.set)
    {
        return {};
    }
    if (plan.toSystem)
    {
        // Its world, if one there was picked to fly on to; otherwise the system.
        const StarSystem* to = m_universe.System(plan.system);
        const Body* onward = to != nullptr ? to->Find(plan.body) : nullptr;
        return onward != nullptr ? onward->name : m_universe.Glance(SystemId::Unpack(plan.system)).name;
    }
    const StarSystem* system = CurrentSystem();
    const Body* body = system != nullptr ? system->Find(plan.body) : nullptr;
    if (body == nullptr)
    {
        return {};
    }
    if (plan.region >= 0 && plan.region < static_cast<int>(body->regions.size()))
    {
        return body->name + ", " + body->regions[static_cast<size_t>(plan.region)].designation;
    }
    return body->name;
}

bool PredationGame::RegionIsPort(const Body& body, int region) const
{
    if (region < 0 || region >= static_cast<int>(body.regions.size()))
    {
        return false;
    }
    const RegionKindDef* kind = m_universeData.RegionKind(body.regions[static_cast<size_t>(region)].kind);
    return kind != nullptr && kind->ship;
}

PredationGame::SkyState PredationGame::SkyState::Of(const Environment& environment)
{
    SkyState state;
    state.sunDirection = environment.sunDirection;
    state.skySunDirection = glm::length(environment.skySunDirection) > 1.0e-6f ? environment.skySunDirection : environment.sunDirection;
    state.sunColor = environment.sunColor;
    state.sunIntensity = environment.sunIntensity;
    state.ambientSky = environment.ambientSky;
    state.ambientGround = environment.ambientGround;
    state.fogColor = environment.fogColor;
    state.fogStart = environment.fogStart;
    state.fogEnd = environment.fogEnd;
    state.stars = environment.stars;
    state.skyAir = environment.skyAir;
    state.skyAirColor = environment.skyAirColor;
    state.sunDisc = environment.sunDisc;
    state.skySun = environment.skySun;
    state.planetRadius = environment.planetRadius;
    state.planetDirection = environment.planetDirection;
    return state;
}

PredationGame::SkyState PredationGame::SkyState::Toward(const SkyState& other, float amount) const
{
    const float t = std::clamp(amount, 0.0f, 1.0f);
    const auto way = [&](const glm::vec3& a, const glm::vec3& b)
    {
        const glm::vec3 between = glm::mix(a, b, t);
        return glm::length(between) > 1.0e-4f ? glm::normalize(between) : (t < 0.5f ? a : b);
    };
    SkyState state;
    state.sunDirection = way(sunDirection, other.sunDirection);
    // The sun in the sky does not slide across it from where it was to where it will be (two frames that do not line
    // up: over the ground and in space) -- it goes out, and comes back where it now is, while the sky changes round it.
    state.skySunDirection = t < 0.5f ? skySunDirection : other.skySunDirection;
    state.skySun = t < 0.5f ? skySun * (1.0f - glm::smoothstep(0.3f, 0.5f, t)) : other.skySun * glm::smoothstep(0.5f, 0.7f, t);
    state.sunColor = glm::mix(sunColor, other.sunColor, t);
    state.sunIntensity = glm::mix(sunIntensity, other.sunIntensity, t);
    state.ambientSky = glm::mix(ambientSky, other.ambientSky, t);
    state.ambientGround = glm::mix(ambientGround, other.ambientGround, t);
    state.fogColor = glm::mix(fogColor, other.fogColor, t);
    // Fog thins away as it is left behind: by how far, which a mix of distances does not do evenly.
    state.fogStart = std::exp(glm::mix(std::log(std::max(fogStart, 1.0f)), std::log(std::max(other.fogStart, 1.0f)), t));
    state.fogEnd = std::exp(glm::mix(std::log(std::max(fogEnd, 1.0f)), std::log(std::max(other.fogEnd, 1.0f)), t));
    state.stars = glm::mix(stars, other.stars, t);
    state.skyAir = glm::mix(skyAir, other.skyAir, t);
    state.skyAirColor = glm::mix(skyAirColor, other.skyAirColor, t);
    state.sunDisc = glm::mix(sunDisc, other.sunDisc, t);
    // What hangs in the sky is the one being gone to's.
    state.planetRadius = t < 0.5f ? planetRadius : other.planetRadius;
    state.planetDirection = t < 0.5f ? planetDirection : other.planetDirection;
    return state;
}

void PredationGame::SkyState::Into(Environment& environment) const
{
    environment.sunDirection = sunDirection;
    environment.skySunDirection = skySunDirection;
    environment.sunColor = sunColor;
    environment.sunIntensity = sunIntensity;
    environment.ambientSky = ambientSky;
    environment.ambientGround = ambientGround;
    environment.fogColor = fogColor;
    environment.fogStart = fogStart;
    environment.fogEnd = fogEnd;
    environment.stars = stars;
    environment.skyAir = skyAir;
    environment.skyAirColor = skyAirColor;
    environment.sunDisc = sunDisc;
    environment.skySun = skySun;
    environment.planetRadius = planetRadius;
    environment.planetDirection = planetDirection;
}

void PredationGame::SetGroundSky(Environment& environment)
{
    // Under a sky, not in space: no bodies hung in it.
    m_spaceBodies.clear();
    const StarSystem* system = CurrentSystem();
    if (const Body* body = system != nullptr ? system->Find(m_groundBody) : nullptr)
    {
        SetSkyOver(*system, *body, m_groundRegion, environment);
    }
}

float PredationGame::RegionTemperature(const Body& body, const LandingRegion& region)
{
    // (Far towards a pole, a good deal colder.)
    return body.temperature + (std::abs(region.latLon.x) > 55.0f ? -25.0f : 0.0f);
}

SiteMap::Look PredationGame::SiteLookHere()
{
    // The site the shuttle goes down to is on the body the ship is over: its own ground and rock, and its own weather --
    // rain freezing to snow where it is cold enough and snow melting to rain where it is not, and nothing at all without
    // air to carry it.
    SiteMap::Look look;
    const StarSystem* system = m_campaignOpen ? CurrentSystem() : nullptr;
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    const std::string terrain = !m_siteTerrainPreview.empty() ? m_siteTerrainPreview : body != nullptr ? body->terrain : std::string();
    look.terrain = SiteTerrain::ShapeOf(terrain);
    look.dunes = terrain == "dunes";
    // (With no world under the ship, only what a test asked for: test_site.)
    const std::string biomeId = !m_siteBiomePreview.empty() ? m_siteBiomePreview : body != nullptr ? body->biome : std::string();
    if (biomeId.empty())
    {
        return look;
    }
    if (const BiomeDef* biome = m_universeData.Biome(biomeId))
    {
        look.ground = biome->siteGround;
        look.rock = biome->siteRock;
        look.surface = biome->surface;
        look.steep = biome->steepSurface;
        look.blend = biome->blendSurface;
        look.blendColour = biome->blendColour;
        look.blendShare = biome->blendShare;
        look.biome = biomeId;
        // (A test's site, by its biome: airless if that kind of world never has air.)
        const bool neverAir = std::all_of(biome->atmospheres.begin(), biome->atmospheres.end(), [](const auto& kind) { return kind.first == "none"; });
        look.airless = m_siteBiomePreview.empty() ? body != nullptr && body->air <= 0.0f : neverAir;
    }
    // How warm it is there: the world's at the place it is going down to; a test's biome's usual (site_biome, test_site),
    // so a volcanic test site is not snowed on by the world it was built on; or what a test asked for (site_temperature).
    float temperature = body != nullptr ? body->temperature : 15.0f;
    if (body != nullptr && m_campaign.travel.region >= 0 && m_campaign.travel.region < static_cast<int>(body->regions.size()))
    {
        temperature = RegionTemperature(*body, body->regions[static_cast<size_t>(m_campaign.travel.region)]);
    }
    if (!m_siteBiomePreview.empty())
    {
        if (const BiomeDef* biome = m_universeData.Biome(m_siteBiomePreview))
        {
            temperature = std::clamp((biome->temperature.x + biome->temperature.y) * 0.5f, -80.0f, 80.0f);
        }
    }
    if (!std::isnan(m_siteTemperaturePreview))
    {
        temperature = m_siteTemperaturePreview;
    }
    // Its water: the sea over all of it on an ocean world (you, 2026-11-03: its facilities on the sea's floor, not over the water),
    // all round on an island world, along one side at a coast; lakes in the low ground where things grow -- and pools of lava in a
    // volcanic world's, of acid in a toxic one's, frozen lakes on an icy one; none in a desert or on bare rock.
    {
        const bool coast = body != nullptr && m_campaign.travel.region >= 0 && m_campaign.travel.region < static_cast<int>(body->regions.size()) &&
                           body->regions[static_cast<size_t>(m_campaign.travel.region)].designation.find("COAST") != std::string::npos;
        look.water = look.biome == "oceanic"                 ? SiteTerrain::Water::Seabed
                     : terrain == "islands"                  ? SiteTerrain::Water::Island
                     : coast                                 ? SiteTerrain::Water::Coast
                     : look.biome == "temperate" || look.biome == "jungle" || look.biome == "volcanic" || look.biome == "toxic" || look.biome == "frozen"
                         ? SiteTerrain::Water::Lakes
                                                             : SiteTerrain::Water::None;
        // (No lakes nor sea on an airless world -- a moon, a bare rock: nothing liquid stands without air over it.)
        if (look.airless)
        {
            look.water = SiteTerrain::Water::None;
        }
        // (Or what a test asked for: site_water, test_site.)
        if (m_siteWaterPreview == "none" || m_siteWaterPreview == "lakes" || m_siteWaterPreview == "coast" || m_siteWaterPreview == "island" ||
            m_siteWaterPreview == "seabed")
        {
            look.water = m_siteWaterPreview == "lakes"    ? SiteTerrain::Water::Lakes
                         : m_siteWaterPreview == "coast"  ? SiteTerrain::Water::Coast
                         : m_siteWaterPreview == "island" ? SiteTerrain::Water::Island
                         : m_siteWaterPreview == "seabed" ? SiteTerrain::Water::Seabed
                                                          : SiteTerrain::Water::None;
        }
        // What it is: lava and acid in their worlds' low ground (the sea is the sea); ice wherever it is cold enough to freeze --
        // sea, lake or stream -- as cold as the rain falls as snow there. (Never a sea with a site on its floor: frozen over, nobody
        // could get down to it.)
        const bool lakes = look.water == SiteTerrain::Water::Lakes;
        const bool seabed = look.water == SiteTerrain::Water::Seabed;
        look.liquid = lakes && look.biome == "volcanic" ? SiteMap::Look::Liquid::Lava
                      : lakes && look.biome == "toxic"  ? SiteMap::Look::Liquid::Acid
                      : !seabed && (look.biome == "frozen" || temperature < kFreezes) ? SiteMap::Look::Liquid::Ice
                                                                                       : SiteMap::Look::Liquid::Water;
        // Along the sea, a beach: sand up from the water (a sand world's beach is its own sand; an icy one's, its snow) --
        // and bare sandy patches inland, the grass thin on them. Round a lake, its second ground a little way up from the water.
        // (The sea's floor sand too, in patches.)
        const bool sea = look.water == SiteTerrain::Water::Coast || look.water == SiteTerrain::Water::Island || seabed;
        if (sea && look.surface != "sand" && look.surface != "snow_02")
        {
            if (look.blend != "sand")
            {
                look.blend = "sand";
                // (Black sand on a volcanic world's shore.)
                look.blendColour = look.biome == "volcanic" ? glm::vec3(0.07f, 0.065f, 0.06f) : glm::vec3(0.36f, 0.31f, 0.22f);
                look.blendShare = 0.14f;
            }
            look.blendShore = 1.4f;
        }
        else if (look.water == SiteTerrain::Water::Lakes)
        {
            look.blendShore = 1.0f;
        }
    }
    // Signs of the organism wherever the creatures are -- at every site -- faint near home, plainer the further out it is
    // (until a site has a difficulty of its own, the further out the harder): by the place, so the same for everyone and every
    // visit. (Or as a test asked for: site_signs.)
    look.signTissue = m_universeData.signTissue;
    if (system != nullptr && body != nullptr)
    {
        look.signs = RegionSigns(*system, m_campaign.body, m_campaign.travel.region);
    }
    if (m_siteSignsPreview >= 0.0f)
    {
        look.signs = m_siteSignsPreview;
    }
    // (A test's main building flooded: site_flood.)
    look.flood = m_siteFloodPreview;
    look.floodLevel = m_siteFloodLevelPreview;
    look.weather = "none";
    look.weatherAmount = 0.0f;
    // Its weather: the world's -- or, for a site built as on another biome (site_biome, test_site), that biome's usual
    // weather -- at how warm it is there (above).
    std::string weatherId = body != nullptr ? body->weather : std::string();
    bool air = body != nullptr && body->air > 0.015f;
    if (!m_siteBiomePreview.empty())
    {
        if (const BiomeDef* biome = m_universeData.Biome(m_siteBiomePreview))
        {
            float most = 0.0f;
            for (const auto& [id, weight] : biome->weathers)
            {
                if (weight > most)
                {
                    most = weight;
                    weatherId = id;
                }
            }
            air = air || !biome->weathers.empty();
        }
    }
    if (const WeatherDef* weather = m_universeData.Weather(weatherId); weather != nullptr && air)
    {
        look.weather = weather->particles;
        look.weatherAmount = weather->intensity;
        if (look.weather == "rain" && temperature < kFreezes)
        {
            look.weather = "snow";
        }
        else if (look.weather == "snow" && temperature > 3.0f)
        {
            look.weather = "rain";
        }
    }
    return look;
}

uint16_t PredationGame::SiteSeedOf(const LandingRegion& region)
{
    return SitePlan::SeedFor(SiteSeed(region.seed), SiteKindOf(region));
}

std::string PredationGame::RegionRead(const LandingRegion& region) const
{
    const RegionKindDef* kind = m_universeData.RegionKind(region.kind);
    // (The other way round where its mission is: a facility's plant to start, a signal gone silent -- MissionPlan::KindFor.)
    const uint16_t seed = SiteSeedOf(region);
    const MissionPlan::Kind mission = MissionPlan::KindFor(seed);
    const bool other = mission == MissionPlan::Kind::Activate || mission == MissionPlan::Kind::Revive;
    std::string read = kind != nullptr ? (other ? kind->readOther : kind->read) : std::string();
    // What the place is for, from its seed alone: known from orbit by what is built there.
    const std::string key = "{type}";
    if (const size_t at = read.find(key); at != std::string::npos)
    {
        read.replace(at, key.size(), SitePlan::TypeName(SitePlan::TypeOf(seed, SitePlan::KindOf(seed))));
    }
    return read;
}

float PredationGame::RegionSigns(const StarSystem& system, int body, int region) const
{
    // By the place, so the same for everyone and every visit: further from home plainer, give or take a little.
    uint64_t h = system.id.Packed() * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(body + 1) * 0xBF58476D1CE4E5B9ull ^
                 static_cast<uint64_t>(region + 7) * 0x94D049BB133111EBull;
    h ^= h >> 31;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 29;
    const float spread = static_cast<float>((h >> 24) & 0xFFFFull) / 65536.0f;
    const float out = glm::distance(m_universe.SystemPosition(system.id), m_universe.SystemPosition(m_universe.Home()));
    return std::clamp(0.12f + 0.8f * out / std::max(m_universeData.signFullDistance, 1.0f) + (spread - 0.5f) * 0.15f, 0.08f, 1.0f);
}

Difficulty::Read PredationGame::RegionDifficulty(const StarSystem& system, int body, int region) const
{
    const Body* world = system.Find(body);
    if (world == nullptr || region < 0 || region >= static_cast<int>(world->regions.size()))
    {
        return {};
    }
    const LandingRegion& area = world->regions[static_cast<size_t>(region)];
    Difficulty::Inputs in;
    in.kind = SiteKindOf(area);
    in.charted = area.charted;
    in.threatKnown = SensorTier() >= 1;
    in.signs = RegionSigns(system, body, region);
    in.air = world->air > 0.015f;
    // How rough its weather is: by how much falls and how little can be seen in it, the storms the worst.
    if (const WeatherDef* weather = in.air ? m_universeData.Weather(world->weather) : nullptr)
    {
        const std::string& w = world->weather;
        const bool storm = w == "storm" || w == "blizzard" || w == "sandstorm" || w == "ash_storm";
        in.weather = std::clamp(std::max({weather->intensity, 1.0f - weather->visibility, storm ? 0.8f : 0.0f}), 0.0f, 1.0f);
    }
    in.temperature = RegionTemperature(*world, area);
    in.night = system.SunHeight(body, area.latLon, m_campaign.clock) < 0.0f;
    return Difficulty::Rate(in);
}

SitePlan::SiteKind PredationGame::SiteKindOf(const LandingRegion& region)
{
    return region.kind == "outpost"   ? SitePlan::SiteKind::Station
           : region.kind == "survey"  ? SitePlan::SiteKind::Survey
           : region.kind == "wreck"   ? SitePlan::SiteKind::Wreck
           : region.kind == "signal"  ? SitePlan::SiteKind::Signal
                                      : SitePlan::SiteKind::Facility;
}

float PredationGame::LocalHour(const StarSystem& system, const Body& body, int region)
{
    if (region < 0 || region >= static_cast<int>(body.regions.size()))
    {
        return 12.0f;
    }
    // As SetSkyOver: the star's way over the place, in its east and up; noon with it overhead, sunrise with it to the east.
    const glm::vec3 up = AreaOnGlobe(body.regions[static_cast<size_t>(region)]);
    glm::vec3 east = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up);
    east = glm::length(east) > 1.0e-4f ? glm::normalize(east) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 sun = SunOverBody(system, body);
    const float angle = std::atan2(glm::dot(sun, east), glm::dot(sun, up));
    return std::fmod(12.0f - angle / glm::two_pi<float>() * 24.0f + 24.0f, 24.0f);
}

void PredationGame::SetSkyOver(const StarSystem& system, const Body& body, int region, Environment& environment)
{
    if (region < 0 || region >= static_cast<int>(body.regions.size()))
    {
        return;
    }
    // Where the star is from the place: its height and bearing over the ground there, as the world turns. In the
    // ship's frame, up is up, north is ahead of the bow and east to starboard.
    const glm::vec3 up = AreaOnGlobe(body.regions[static_cast<size_t>(region)]);
    glm::vec3 east = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up);
    east = glm::length(east) > 1.0e-4f ? glm::normalize(east) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 north = glm::cross(up, east);
    // A direction in the body's frame, over the place.
    const auto overHere = [&](const glm::vec3& d) { return glm::normalize(glm::vec3(glm::dot(d, east), glm::dot(d, up), -glm::dot(d, north))); };
    const glm::vec3 towardsSun = overHere(SunOverBody(system, body));
    const float height = towardsSun.y;
    const float day = glm::smoothstep(-0.12f, 0.2f, height);
    const float low = 1.0f - glm::smoothstep(0.04f, 0.4f, height);
    const float air = std::clamp(body.air, 0.0f, 1.0f);
    glm::vec3 ground{0.4f};
    if (const BiomeDef* biome = m_universeData.Biome(body.biome))
    {
        ground = biome->siteGround;
    }

    for (glm::vec4& other : environment.skyBodies)
    {
        other = glm::vec4(0.0f);
    }
    environment.planetRadius = 0.0f;
    environment.skySun = 1.0f;
    // Its own star's light: a red dwarf's orange, a white star's cold white -- the eye adjusting a little towards white, not
    // all the way.
    const glm::vec3 starLight = glm::mix(system.starColor, glm::vec3(1.0f), 0.08f);

    // A ringed world's own rings, over its ground: their plane through its middle about its pole (the globe's y), seen from
    // here (the sky shader draws them from that; their size and colour as a near planet's are).
    environment.ownRings = glm::vec4(0.0f);
    if (body.rings.y > 0.0f && body.parent < 0)
    {
        environment.planet = LookOf(body);
        environment.ownRings = glm::vec4(overHere(glm::vec3(0.0f, 1.0f, 0.0f)), 1.0f);
    }

    // --- The other worlds in the sky, and what lights the night ------------------------------------------------------
    // A moon's planet hangs huge over it; a planet's moons cross its sky. Each where it really is from here, as big as it
    // really looks, lit by the star as it really is -- and the brightest of them above the horizon is the light at night
    // (a planet over its moon a great deal of it, a moon over its planet some), or, with none up, the stars.
    constexpr float kEarthRadiusAu = 4.26e-5f;
    const double clock = m_campaign.clock;
    const glm::dvec3 here = system.PositionD(body.index, clock);
    // A second sun, where the system has a companion star: where it is from here, as big and as bright against the first as it
    // really is -- a close pair's two together, a far one's a brilliant point (seen, however faint against the first: a star a few
    // dozen AU off is brighter than any moon).
    environment.secondSun = glm::vec4(0.0f);
    environment.secondSunColor = glm::vec4(0.0f);
    if (system.HasCompanion() && m_debugCompanion == 0)
    {
        const glm::dvec3 apart = glm::dvec3(system.CompanionAt(clock)) - here;
        const double distance = std::max(glm::length(apart), 1.0e-4);
        const glm::vec3 way = overHere(system.Over(body.index, glm::vec3(apart / distance), clock));
        constexpr float kSunRadiusAu = 0.00465f;
        const bool close = system.companionApart < 1.0f;
        const float first = std::max(system.luminosity - (close ? system.companionLuminosity : 0.0f), 0.001f);
        const double fromFirst = std::max(glm::length(here), 1.0e-4);
        const float ratio = system.companionLuminosity / first * static_cast<float>((fromFirst * fromFirst) / (distance * distance));
        environment.secondSun = glm::vec4(way, std::max(system.companionRadius * kSunRadiusAu / static_cast<float>(distance), 0.0006f));
        environment.secondSunColor = glm::vec4(glm::mix(system.companionColor, glm::vec3(1.0f), 0.08f), 0.55f * std::clamp(ratio, 0.012f, 1.0f));
    }
    // (Or, to look at: sky_companion -- a close pair's second sun beside the first, or a far one's off across the sky.)
    if (m_debugCompanion != 0)
    {
        const glm::vec3 first = overHere(SunOverBody(system, body));
        const glm::vec3 side = glm::normalize(glm::cross(first, glm::vec3(0.0f, 1.0f, 0.0f)) + glm::vec3(1.0e-4f, 0.0f, 0.0f));
        const bool close = m_debugCompanion == 1;
        environment.secondSun = glm::vec4(glm::normalize(first + side * (close ? 0.09f : 1.4f) + glm::vec3(0.0f, close ? 0.02f : 0.3f, 0.0f)),
                                          close ? 0.0035f : 0.0008f);
        environment.secondSunColor = glm::vec4(1.0f, 0.62f, 0.42f, close ? 0.35f : 0.03f);
    }
    glm::vec3 nightWay = glm::normalize(glm::vec3(0.25f, 1.0f, 0.15f));
    float nightLight = 0.05f;
    glm::vec3 nightColour = glm::vec3(0.62f, 0.72f, 1.0f);
    int discs = 0;
    for (const Body& other : system.bodies)
    {
        if (other.index == body.index || (other.parent != body.index && body.parent != other.index &&
                                          !(body.parent >= 0 && other.parent == body.parent)))
        {
            continue; // only the worlds of the same planet's family are near enough to be more than points
        }
        const glm::dvec3 there = system.PositionD(other.index, clock);
        const glm::dvec3 apart = there - here;
        const double distance = glm::length(apart);
        if (distance < 1.0e-12)
        {
            continue;
        }
        const float angle = static_cast<float>(std::asin(std::min(static_cast<double>(std::max(other.radius, 0.02f) * kEarthRadiusAu) / distance, 0.95)));
        const glm::vec3 way = overHere(system.Over(body.index, glm::vec3(apart / distance), clock));
        // How much of its face is lit, seen from here: all of it with the star behind us, none with it behind the world.
        const glm::vec3 fromItToStar = glm::normalize(glm::vec3(-there));
        const float lit = 0.5f * (1.0f + glm::dot(fromItToStar, glm::vec3(-apart / distance)));
        const glm::vec3 colour = glm::mix(other.groundA, other.cloudColor, other.clouds * 0.6f);
        if (other.index == body.parent && angle > 0.01f)
        {
            environment.planetDirection = way;
            environment.planetRadius = angle;
            environment.planet = LookOf(other);
            environment.planetAirWarm = 0.0f;
        }
        else if (discs < Environment::kSkyBodies)
        {
            environment.skyBodies[discs * 2] = glm::vec4(way, angle);
            environment.skyBodies[discs * 2 + 1] = glm::vec4(colour, other.air);
            ++discs;
        }
        const float light = 0.9f * lit * glm::smoothstep(0.002f, 0.14f, angle) * glm::smoothstep(-0.02f, 0.12f, way.y);
        if (light > nightLight)
        {
            nightLight = light;
            nightWay = way;
            nightColour = glm::mix(glm::vec3(0.62f, 0.72f, 1.0f), colour, 0.35f);
        }
    }
    nightLight = std::min(nightLight, 0.5f) * (1.0f - 0.7f * body.clouds);

    // And the ship, in orbit of this world: a point of sunlit light going over, as a satellite is -- brighter the nearer it
    // is (the orbit is so many of the world's radii up, so over a small moon it is close), seen once the sky is dark enough
    // for the stars.
    if (discs < Environment::kSkyBodies && m_campaign.body == body.index && !m_campaign.travel.underway && !m_campaign.landed)
    {
        glm::vec3 out;
        glm::vec3 along;
        Travel::OrbitFrame(m_campaign, system, body.index, clock, out, along);
        const float orbitUp = Travel::OrbitRadius(body) / (std::max(body.radius, 0.02f) * kEarthRadiusAu);
        const glm::vec3 apart = system.Over(body.index, out, clock) * orbitUp - up;
        const glm::vec3 way = overHere(apart);
        if (way.y > 0.0f)
        {
            // Its distance in kilometres: the world's radius (6371 km an Earth radius) times how far off it is in them.
            const float distance = glm::length(apart) * std::max(body.radius, 0.02f) * 6371.0f;
            const float bright = std::clamp(1500.0f / std::max(distance, 1.0f), 0.25f, 2.0f);
            environment.skyBodies[discs * 2] = glm::vec4(way * bright, 1.0e-6f);
            environment.skyBodies[discs * 2 + 1] = glm::vec4(0.9f, 0.92f, 0.95f, 0.0f);
            ++discs;
        }
    }

    // The light on the ground: the star by day; by night what is up; between, the one fading into the other as the light
    // swings from the one to the other. The sky still draws its star where it is.
    const float sunUp = glm::smoothstep(-0.1f, 0.12f, height);
    const glm::vec3 sunColour = glm::mix(starLight, starLight * glm::vec3(1.0f, 0.6f, 0.34f), low * air); // reddened low through air
    // Bright, not blinding; and dimmer through thick air, which spreads some of it over the sky instead.
    // How much of its star reaches here (its light over the square of how far off it is, against the Sun's at Earth): a
    // little brighter or dimmer for it, the eye taking up most of the difference. And the star as big as it really looks from
    // here -- a red dwarf's near worlds under a big dim sun, a blue-white star's a small fierce one.
    const double fromStar = std::max(glm::length(here), 1.0e-4);
    const float reaching = static_cast<float>(static_cast<double>(std::max(system.luminosity, 0.001f)) / (fromStar * fromStar));
    const float sunLight = 1.6f * day * (1.0f - 0.35f * body.clouds) * (1.0f - 0.2f * glm::smoothstep(0.2f, 1.0f, air)) *
                           std::clamp(std::pow(reaching, 0.2f), 0.7f, 1.35f);
    environment.sunDisc = std::clamp(static_cast<float>(0.00465 * static_cast<double>(std::max(system.starRadius, 0.05f)) / fromStar), 0.0012f, 0.06f);
    const glm::vec3 between = nightWay * (1.0f - sunUp) + towardsSun * sunUp + glm::vec3(0.0f, 1.2f * sunUp * (1.0f - sunUp), 0.0f);
    environment.sunDirection = -glm::normalize(between);
    environment.skySunDirection = -towardsSun;
    environment.sunColor = glm::mix(nightColour * starLight, sunColour, sunUp);
    environment.sunIntensity = glm::mix(nightLight, sunLight, sunUp);
    const float nightGlow = nightLight * (1.0f - sunUp);

    if (air < 0.015f)
    {
        // No air at all: a black sky with the stars in it whatever the hour, and a hard sun -- the shadows lit only by what
        // the sunlit ground round them throws back.
        environment.stars = 1.0f;
        environment.ambientSky = glm::vec3(0.022f, 0.025f, 0.034f) + ground * sunLight * 0.045f + nightColour * nightGlow * 0.12f;
        environment.ambientGround = ground * (0.08f * sunLight / 1.6f + 0.1f * nightGlow) + glm::vec3(0.01f);
        environment.fogColor = glm::vec3(0.0f);
        environment.fogStart = 3000.0f;
        environment.fogEnd = 9000.0f;
        return;
    }
    // Any air at all lights the sky by day -- a thin one less blue and less bright, its stars showing through as the sun goes
    // low -- and the stars come out at night, as much as the cloud lets them; and at night it is still air, not space.
    const float thick = glm::smoothstep(0.015f, 0.2f, air);
    environment.skyAir = std::max(thick, 0.25f);
    environment.skyAirColor = body.airColor;
    environment.stars = std::clamp((1.0f - day * (0.6f + 0.4f * thick)) * (1.0f - 0.8f * body.clouds), 0.0f, 1.0f);
    // Night not black: the sky's own glow, and whatever is up, are enough to see the shapes of things by.
    const glm::vec3 night = glm::vec3(0.045f, 0.054f, 0.078f) + nightColour * nightGlow * 0.12f;
    const glm::vec3 daySky = body.airColor * (0.3f + 0.25f * air) * glm::mix(0.55f, 1.0f, thick);
    environment.ambientSky = glm::mix(night, daySky, day);
    const glm::vec3 haze = glm::mix(body.airColor, ground, 0.25f) * 0.42f;
    environment.fogColor = glm::mix(night * 0.8f, glm::mix(haze, haze * glm::vec3(1.25f, 0.8f, 0.6f), low), day);
    environment.ambientGround = ground * (0.12f * day + 0.1f * nightGlow) + glm::vec3(0.018f, 0.02f, 0.026f);
    // How far anybody can see: by the air and the weather.
    float visibility = 1.0f;
    if (const WeatherDef* weather = m_universeData.Weather(body.weather))
    {
        visibility *= weather->visibility;
    }
    if (const AtmosphereDef* atmosphere = m_universeData.Atmosphere(body.atmosphere))
    {
        visibility *= atmosphere->visibility;
    }
    // As far as a site's: the land round an outpost is made out to the horizon (SiteMap::BuildFrom), so it is seen a long way.
    environment.fogStart = 60.0f * visibility;
    environment.fogEnd = std::clamp(4500.0f * visibility, 800.0f, 6500.0f);
}

} // namespace pred
