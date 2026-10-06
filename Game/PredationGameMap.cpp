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

// How often the host tells everybody how the ship stands and who is pointing at what, and how often the sensors look.
constexpr float kTravelSendEvery = 0.1f;
constexpr float kSensorEvery = 1.0f;

// How far the ship's sensors see while under way, in astronomical units, by their tier.
float SensorRange(int tier)
{
    return 0.05f * std::pow(3.0f, static_cast<float>(std::clamp(tier, 0, 6)));
}

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
    return region >= 0 && region < static_cast<int>(body.regions.size()) &&
           m_campaign.RegionFound(m_campaign.system, body.index, region, body.regions[static_cast<size_t>(region)].charted);
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
        // Only on the ground does it open; it can always be shut -- but never on anybody standing in it.
        if ((a != 0 && !ShipLanded()) || (a == 0 && DoorwayBlocked()))
        {
            return false;
        }
        m_campaign.doorOpen = a != 0;
        CampaignChanged();
        return true;

    case CampaignAction::ClearPlot:
        m_campaign.plan = {};
        CampaignChanged();
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
        // The first place the shuttle can go down to -- not a hub, which the ship sets down at only when asked.
        region = -1;
        for (int i = 0; i < static_cast<int>(body->regions.size()) && region < 0; ++i)
        {
            region = RegionLandable(*body, i) && !RegionIsPort(*body, i) ? i : -1;
        }
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
        if (!already && m_map == MapChoice::Ship && HasCinematic("ship_land"))
        {
            if (m_cine.Active())
            {
                m_cineAfter = "ship_land";
            }
            else
            {
                PlayCinematic("ship_land");
            }
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
    if (m_facility.Seed() != seed || m_mission.stage == MissionState::Stage::Over)
    {
        ChangeFacility(seed);
    }
    m_shipOrbiting = seed;
    m_shipReady = true;
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
            const int region = m_campaign.region >= 0 ? m_campaign.region : m_campaign.travel.region;
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
    // In orbit: the sensors look the whole of it over, and find places to go down that nobody had charted.
    const uint8_t bits = static_cast<uint8_t>(CampaignState::kKnownScanned | (SensorTier() >= 2 ? CampaignState::kKnownDeep : 0));
    m_campaign.Learn(m_campaign.system, body->index, bits);
    int found = 0;
    for (int i = 0; i < static_cast<int>(body->regions.size()) && found < 1 + SensorTier(); ++i)
    {
        if (!RegionKnown(*body, i))
        {
            m_campaign.FindRegion(m_campaign.system, body->index, i);
            m_campaign.AddLog("region", CampaignState::RegionKey(m_campaign.system, body->index, i), body->regions[static_cast<size_t>(i)].designation,
                              "Found from orbit of " + body->name + ".");
            ++found;
        }
    }
    m_campaign.AddLog("planet", CampaignState::BodyKey(m_campaign.system, body->index), body->name, "Surveyed from orbit. " + SummaryOf(*body));
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
                if (m_map == MapChoice::Ship && HasCinematic("ship_arrive"))
                {
                    PlayCinematic("ship_arrive");
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
                    // There: shown arriving, aboard -- the cinematic's own marker says when it is over the planet.
                    if (m_map == MapChoice::Ship && HasCinematic("ship_arrive"))
                    {
                        PlayCinematic("ship_arrive");
                    }
                    else
                    {
                        ArriveAtBody();
                    }
                }
                else
                {
                    PRED_LOG_INFO(Gameplay, "The ship has come to rest between the planets");
                }
            }
            // The sensors, as the ship passes things.
            m_sensorIn -= dt;
            if (m_sensorIn <= 0.0f)
            {
                m_sensorIn = kSensorEvery;
                const float range = SensorRange(SensorTier());
                for (const Body& body : system->bodies)
                {
                    if (glm::length(system->Position(body.index, m_campaign.clock) - glm::vec3(m_campaign.travel.position)) <= range &&
                        m_campaign.Learn(m_campaign.system, body.index, CampaignState::kKnownScanned))
                    {
                        m_campaign.AddLog("planet", CampaignState::BodyKey(m_campaign.system, body.index), body.name,
                                          "Scanned in passing. " + SummaryOf(body));
                        CampaignChanged();
                    }
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
                m_host.SendTravel(travel);
            }
        }
    }
    // The intercom, on every machine alike, from what it sees change: a course set, changed or called off, and places
    // found by the ship's scan of the body it is over.
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
        }
        size_t found = 0;
        for (const auto& [key, isFound] : m_campaign.regionsFound)
        {
            found += isFound ? 1 : 0;
        }
        if (m_travelSeen && found > m_regionsSeen && !underway)
        {
            Say("region_found", 2.0f);
        }
        m_regionsSeen = found;
        m_travelSeenUnderway = underway;
        m_travelSeenTarget = target;
        m_travelSeen = true;
    }
    UpdateShipGround();
    m_ship.UpdateAirlock(m_scene, m_app->GetMeshes(), dt);
    m_ship.UpdateFittings(m_scene, m_app->GetMeshes(), dt);
    for (const Entity control : m_airlockControls)
    {
        if (Interactable* door = m_interactions.Find(control))
        {
            door->enabled = m_map == MapChoice::Ship && !m_cine.Active() && (ShipLanded() || m_campaign.doorOpen);
            door->verb = m_campaign.doorOpen ? "Close" : "Open";
        }
    }
    if (Interactable* helm = m_interactions.Find(m_helm))
    {
        helm->enabled = m_campaign.plan.set && m_map == MapChoice::Ship && !m_cine.Active();
        helm->verb = m_campaign.travel.underway ? "Change course for" : "Set out for";
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

void PredationGame::UpdateShipGround()
{
    const StarSystem* system = CurrentSystem();
    if (system == nullptr)
    {
        return;
    }
    // Standing at a hub: its legs down, the station round it, the world's ground in its own colours, the door open.
    {
        const bool landed = ShipLanded() && system->Find(m_campaign.body) != nullptr;
        if (landed)
        {
            m_groundBody = m_campaign.body;
            m_groundRegion = m_campaign.travel.region;
        }
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
        m_ship.SetStageField(m_scene, m_app->GetMeshes(), GroundCinematic() && at != nullptr, field);
        m_ship.SetGear(landed, GroundCinematic() && m_stageGear);
        m_ship.SetStair(landed, GroundCinematic() && m_stageStair);
        m_ship.SetAirlockOpen(m_scene, m_app->GetMeshes(), landed && m_campaign.doorOpen);
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

bool PredationGame::GroundCinematic() const
{
    if (!m_cine.Active())
    {
        return false;
    }
    const std::string& name = m_cine.Playing().name;
    return name == "ship_takeoff" || name == "ship_land";
}

void PredationGame::PlayLeaving(bool fromGround)
{
    // Up off a hub's pad: its cinematic. Out of orbit, nobody is taken out of the ship to watch it go: the engines light,
    // the dust starts past the windows, the intercom says so -- unless the drive is good enough that the trip is all but a
    // cut, which is shown as one.
    if (fromGround && HasCinematic("ship_takeoff"))
    {
        PlayCinematic("ship_takeoff");
    }
    else if (DriveTier() >= Travel::kInstantTier && HasCinematic("ship_depart"))
    {
        PlayCinematic("ship_depart");
    }
    else if (m_map == MapChoice::Ship)
    {
        PlayNamed("World/shuttle_launch", ShipMap::ToWorld({0.0f, 1.0f, 22.0f}), 0.8f, 0.55f, false);
    }
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
        return m_universe.Glance(SystemId::Unpack(plan.system)).name;
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

SiteMap::Look PredationGame::SiteLookHere()
{
    // The site the shuttle goes down to is on the body the ship is over: its own ground and rock, and snow only where it is
    // cold enough and there is air to carry it.
    SiteMap::Look look;
    const StarSystem* system = m_campaignOpen ? CurrentSystem() : nullptr;
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    if (body == nullptr)
    {
        return look;
    }
    if (const BiomeDef* biome = m_universeData.Biome(body->biome))
    {
        look.ground = biome->siteGround;
        look.rock = biome->siteRock;
    }
    look.snow = body->temperature < -8.0f && body->air > 0.015f;
    return look;
}

SitePlan::SiteKind PredationGame::SiteKindOf(const LandingRegion& region)
{
    return region.kind == "outpost"   ? SitePlan::SiteKind::Station
           : region.kind == "survey"  ? SitePlan::SiteKind::Survey
           : region.kind == "wreck"   ? SitePlan::SiteKind::Wreck
           : region.kind == "signal"  ? SitePlan::SiteKind::Signal
                                      : SitePlan::SiteKind::Facility;
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
    const glm::vec3 sun = SunOverBody(system, body);
    glm::vec3 east = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up);
    east = glm::length(east) > 1.0e-4f ? glm::normalize(east) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 north = glm::cross(up, east);
    const glm::vec3 towardsSun = glm::normalize(glm::vec3(glm::dot(sun, east), glm::dot(sun, up), -glm::dot(sun, north)));
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
    environment.sunDirection = -towardsSun;
    const glm::vec3 starLight = glm::mix(system.starColor, glm::vec3(1.0f), 0.4f);
    // Low in a sky with air in it, the light comes through more of it, and reddens.
    environment.sunColor = glm::mix(starLight, starLight * glm::vec3(1.0f, 0.6f, 0.34f), low * air);
    environment.sunIntensity = 2.4f * day * (1.0f - 0.35f * body.clouds);
    if (air < 0.015f)
    {
        // No air at all: a black sky with the stars in it whatever the hour, and a hard sun.
        environment.stars = 1.0f;
        environment.sunDisc = 0.006f;
        environment.ambientSky = glm::vec3(0.03f, 0.032f, 0.04f) * (0.3f + 0.7f * day);
        environment.ambientGround = ground * 0.08f * day + glm::vec3(0.008f);
        environment.fogColor = glm::vec3(0.0f);
        environment.fogStart = 3000.0f;
        environment.fogEnd = 9000.0f;
        return;
    }
    // Any air at all lights the sky by day -- a thin one less blue and less bright, its stars showing through as the sun goes
    // low -- and the stars come out at night, as much as the cloud lets them.
    const float thick = glm::smoothstep(0.015f, 0.2f, air);
    environment.stars = std::clamp((1.0f - day * (0.6f + 0.4f * thick)) * (1.0f - 0.8f * body.clouds), 0.0f, 1.0f);
    // Night not black: the sky's own glow and the outpost's lights are enough to see the shapes of things by.
    const glm::vec3 night{0.04f, 0.048f, 0.07f};
    const glm::vec3 daySky = body.airColor * (0.3f + 0.25f * air) * glm::mix(0.55f, 1.0f, thick);
    environment.ambientSky = glm::mix(night, daySky, day);
    const glm::vec3 haze = glm::mix(body.airColor, ground, 0.25f) * 0.42f;
    environment.fogColor = glm::mix(night * 0.8f, glm::mix(haze, haze * glm::vec3(1.25f, 0.8f, 0.6f), low), day);
    environment.ambientGround = ground * 0.12f * day + glm::vec3(0.018f, 0.02f, 0.026f);
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
    // Never so far that the edge of the ground round the hub (440 m out) shows.
    environment.fogStart = 30.0f * visibility;
    environment.fogEnd = std::clamp(420.0f * visibility, 140.0f, 400.0f);
}

} // namespace pred
