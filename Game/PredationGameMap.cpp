// The system map and the ship's travel: looking at the system from the navigation console, pointing things out to the
// others, choosing where to go and where to go down, and the ship flying there. The map's picture is Game/Campaign/
// SystemMap and Engine/Render/PlanetRenderer; travel is Game/Campaign/Travel.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/Renderer.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace pred
{

namespace
{

// How often the host tells everybody how the ship stands and who is pointing at what, and how often the sensors look.
constexpr float kTravelSendEvery = 0.1f;
constexpr float kSensorEvery = 1.0f;
constexpr float kTau = 6.28318530718f;


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
    case CampaignAction::Pointer:
        if (player < kMaxPlayers)
        {
            m_pointing[player] = static_cast<int8_t>(std::clamp(a, -1, 127));
            m_mapOpenMask = static_cast<uint8_t>(b != 0 ? (m_mapOpenMask | (1u << player)) : (m_mapOpenMask & ~(1u << player)));
        }
        return true;

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
        m_campaign.landed = false;
        PlayLeaving(true);
    }
    m_campaign.landed = false;
    m_campaign.travel.region = region;
    CampaignChanged();
    if (region < 0)
    {
        m_shipReady = false;
        m_shipOrbiting = 0;
        return;
    }
    const uint16_t seed = SiteSeed(body->regions[static_cast<size_t>(region)].seed);
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
    // What this player is pointing at, for the others: a body of the system the ship is in, on its map.
    const bool pointable = m_mapOpen && m_mapLevel != MapLevel::Galaxy && m_mapSystem == m_campaign.system;
    const int pointer = pointable ? (m_mapHovered >= 0 ? m_mapHovered : m_mapSelected) : -1;
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
            m_campaign.body = travel.body;
            m_pointing = travel.pointing;
            m_mapOpenMask = travel.mapOpen;
        }
        else if (m_campaign.travel.underway && !m_campaign.travel.interstellar)
        {
            // Between the host's words, flown the same way here, so the map moves smoothly.
            Travel::Step(m_campaign, *system, dt, DriveTier());
        }
        if (pointer != m_pointerSent || m_mapOpen != m_mapOpenSent)
        {
            m_pointerSent = pointer;
            m_mapOpenSent = m_mapOpen;
            AskCampaign(CampaignAction::Pointer, pointer, m_mapOpen ? 1 : 0);
        }
    }
    else
    {
        DoCampaignAction(LocalPlayerId(), CampaignAction::Pointer, pointer, m_mapOpen ? 1 : 0);
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
                    if (glm::length(system->Position(body.index, m_campaign.clock) - m_campaign.travel.position) <= range &&
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
                travel.pointing = m_pointing;
                travel.mapOpen = m_mapOpenMask;
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
    // Standing at a hub: its legs down, the pad, the hub round it and the world's ground in its own colours.
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
        m_ship.SetField(m_scene, m_app->GetMeshes(), landed, ground, rock);
        m_ship.SetStageField(m_scene, m_app->GetMeshes(), GroundCinematic() && at != nullptr, ground, rock);
        m_ship.SetGear(m_scene, landed, GroundCinematic() && m_stageGear);
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
    // Up off a hub's pad, or out of orbit.
    const char* name = fromGround && HasCinematic("ship_takeoff") ? "ship_takeoff" : "ship_depart";
    if (HasCinematic(name))
    {
        PlayCinematic(name);
    }
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
    const StarSystem* system = CurrentSystem();
    const Body* body = system != nullptr ? system->Find(m_groundBody) : nullptr;
    const int region = m_groundRegion;
    if (body == nullptr || region < 0 || region >= static_cast<int>(body->regions.size()))
    {
        return;
    }
    // Where the star is from the place: its height and bearing over the ground there, as the world turns. In the
    // ship's frame, up is up, north is ahead of the bow and east to starboard.
    const glm::vec3 up = AreaOnGlobe(body->regions[static_cast<size_t>(region)]);
    const glm::vec3 sun = SunOverBody(*system, *body);
    glm::vec3 east = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up);
    east = glm::length(east) > 1.0e-4f ? glm::normalize(east) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 north = glm::cross(up, east);
    const glm::vec3 towardsSun = glm::normalize(glm::vec3(glm::dot(sun, east), glm::dot(sun, up), -glm::dot(sun, north)));
    const float height = towardsSun.y;
    const float day = glm::smoothstep(-0.12f, 0.2f, height);
    const float low = 1.0f - glm::smoothstep(0.04f, 0.4f, height);
    const float air = std::clamp(body->air, 0.0f, 1.0f);
    glm::vec3 ground{0.4f};
    if (const BiomeDef* biome = m_universeData.Biome(body->biome))
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
    const glm::vec3 starLight = glm::mix(system->starColor, glm::vec3(1.0f), 0.4f);
    // Low in a sky with air in it, the light comes through more of it, and reddens.
    environment.sunColor = glm::mix(starLight, starLight * glm::vec3(1.0f, 0.6f, 0.34f), low * air);
    environment.sunIntensity = 2.4f * day * (1.0f - 0.35f * body->clouds);
    if (air < 0.08f)
    {
        // No air to speak of: a black sky with the stars in it whatever the hour, and a hard sun.
        environment.stars = 1.0f;
        environment.sunDisc = 0.006f;
        environment.ambientSky = glm::vec3(0.03f, 0.032f, 0.04f) * (0.3f + 0.7f * day);
        environment.ambientGround = ground * 0.08f * day + glm::vec3(0.008f);
        environment.fogColor = glm::vec3(0.0f);
        environment.fogStart = 3000.0f;
        environment.fogEnd = 9000.0f;
        return;
    }
    environment.stars = 0.0f;
    // Night not black: the sky's own glow and the hub's lights are enough to see the shapes of things by.
    const glm::vec3 night{0.03f, 0.036f, 0.055f};
    const glm::vec3 daySky = body->airColor * (0.3f + 0.25f * air);
    environment.ambientSky = glm::mix(night, daySky, day);
    const glm::vec3 haze = glm::mix(body->airColor, ground, 0.25f) * 0.42f;
    environment.fogColor = glm::mix(night * 0.8f, glm::mix(haze, haze * glm::vec3(1.25f, 0.8f, 0.6f), low), day);
    environment.ambientGround = ground * 0.12f * day + glm::vec3(0.018f, 0.02f, 0.026f);
    // How far anybody can see: by the air and the weather.
    float visibility = 1.0f;
    if (const WeatherDef* weather = m_universeData.Weather(body->weather))
    {
        visibility *= weather->visibility;
    }
    if (const AtmosphereDef* atmosphere = m_universeData.Atmosphere(body->atmosphere))
    {
        visibility *= atmosphere->visibility;
    }
    // Never so far that the edge of the ground round the hub (440 m out) shows.
    environment.fogStart = 30.0f * visibility;
    environment.fogEnd = std::clamp(420.0f * visibility, 140.0f, 400.0f);
}

void PredationGame::SetSpaceSky(Environment& environment)
{
    // The system as it is from where the ship is: the star where it really is, the planet it is over or heading for big
    // ahead or below, and every other body where it really is and as big as it really looks -- a disc when it is near
    // enough to have a size, a point of light when it is not. Directions in the system are turned into the ship's:
    // forward is the way it is heading (the bow), up is the system's up.
    const StarSystem* system = CurrentSystem();
    if (system == nullptr)
    {
        return;
    }
    for (glm::vec4& body : environment.skyBodies)
    {
        body = glm::vec4(0.0f);
    }
    const glm::vec3 bow = glm::normalize(ShipSpec::kTravelHeading);
    const glm::vec3 starboard = glm::normalize(glm::cross(bow, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 overhead = glm::cross(starboard, bow);

    // Between the stars: nothing near, the star it left a dimming sun astern and the one it is heading for a brightening
    // point dead ahead -- the nearer of the two the one that lights the ship.
    if (m_campaign.travel.interstellar)
    {
        const SystemGlance& ahead = m_universe.Glance(SystemId::Unpack(m_campaign.travel.toSystem));
        const glm::vec3 behindColour = system->starColor;
        const float done = Travel::CrossingDone(m_campaign);
        const bool nearerAhead = done >= 0.5f;
        const float near = nearerAhead ? (done - 0.5f) * 2.0f : 1.0f - done * 2.0f;
        const glm::vec3 astern = -bow;
        const glm::vec3 lit = nearerAhead ? bow : astern;
        environment.sunDirection = -glm::normalize(lit + overhead * 0.04f);
        environment.sunColor = glm::mix(nearerAhead ? ahead.starColor : behindColour, glm::vec3(1.0f), 0.3f);
        environment.sunIntensity = glm::mix(0.25f, 1.1f, near * near);
        environment.planetRadius = 0.0f;
        // The other star, as a point.
        const glm::vec3 other = nearerAhead ? astern : bow;
        const glm::vec3 otherColour = nearerAhead ? behindColour : ahead.starColor;
        environment.skyBodies[0] = glm::vec4(glm::normalize(other) * 2.0f, 1.0e-6f);
        environment.skyBodies[1] = glm::vec4(glm::mix(otherColour, glm::vec3(1.0f), 0.4f) * 3.0f, 0.0f);
        return;
    }

    const glm::vec3 ship = Travel::ShipPosition(m_campaign, *system);
    const int main = m_campaign.travel.underway ? m_campaign.travel.target : m_campaign.body;
    glm::vec3 heading{0.0f, 0.0f, -1.0f};
    if (m_campaign.travel.underway && main >= 0)
    {
        // The bow on where it is going (round the star, when that is the way), so the destination is dead ahead and
        // the sun stays where it is in the sky rather than swinging about as the ship speeds up and slows.
        const std::vector<glm::vec3> path = Travel::Preview(m_campaign, *system, DriveTier(), 24);
        const glm::vec3 there = system->Position(main, m_campaign.clock);
        heading = path.size() > 3 && glm::length(path[3] - ship) > 1.0e-6f ? path[3] - ship : there - ship;
    }
    else if (m_campaign.travel.underway)
    {
        heading = m_campaign.travel.velocity;
    }
    else if (main >= 0)
    {
        // In orbit, going round with the body: forward is the way it goes round its star.
        heading = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), system->Position(main, m_campaign.clock));
    }
    if (glm::length(heading) < 1.0e-9f)
    {
        heading = {0.0f, 0.0f, -1.0f};
    }
    const glm::vec3 forward = glm::normalize(heading);
    glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
    right = glm::length(right) > 1.0e-4f ? glm::normalize(right) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up = glm::cross(right, forward);
    // In orbit the ship goes round the body below, so everything else goes slowly round the ship: the sun rises over the
    // planet's edge and sets behind it, and the night side comes under the ship and goes again.
    const glm::vec3 below = glm::normalize(glm::vec3(0.12f, -0.42f, -0.9f));
    const bool orbiting = !m_campaign.travel.underway && system->Find(main) != nullptr;
    glm::mat4 orbit(1.0f);
    if (orbiting)
    {
        constexpr double kOrbitSeconds = 960.0;
        const float angle = static_cast<float>(std::fmod(m_campaign.clock / kOrbitSeconds, 1.0)) * kTau;
        orbit = glm::rotate(glm::mat4(1.0f), -angle, glm::normalize(glm::cross(below, bow)));
    }
    const auto toWorld = [&](const glm::vec3& v)
    {
        const glm::vec3 inShip = glm::dot(v, right) * starboard + glm::dot(v, up) * overhead + glm::dot(v, forward) * bow;
        return glm::vec3(orbit * glm::vec4(inShip, 0.0f));
    };

    // The star.
    const float fromStar = std::max(glm::length(ship), 0.05f);
    const glm::vec3 towardsStar = glm::normalize(toWorld(-ship));
    environment.sunDirection = -towardsStar;
    environment.sunColor = glm::mix(system->starColor, glm::vec3(1.0f), 0.3f);
    environment.sunIntensity = 1.7f * std::clamp(std::sqrt(system->luminosity) / fromStar, 0.35f, 2.2f);
    // As big as the star is from here: a sun's radius is 0.00465 astronomical units; a little larger, so it reads.
    environment.sunDisc = std::clamp(0.00465f * std::max(system->starRadius, 0.1f) / fromStar * 1.3f, 0.0012f, 0.06f);

    // The body it is over or heading for.
    environment.planetRadius = 0.0f;
    if (const Body* body = system->Find(main))
    {
        environment.planet = LookOf(*body);
        // How big it is from orbit, by how big it is: a small moon a ball in the window, a gas giant filling it.
        const float orbitSize = std::clamp(0.72f * std::pow(std::max(body->radius, 0.05f), 0.32f), 0.3f, 1.25f);
        if (m_campaign.travel.underway)
        {
            // Where it is, growing as the ship closes on it: a point a long way off, the size it is from orbit at the end.
            const glm::vec3 there = system->Position(main, m_campaign.clock) - ship;
            const float distance = std::max(glm::length(there), Travel::kArrival);
            const float size = std::sqrt(std::max(body->radius, 0.1f));
            environment.planetDirection = glm::length(there) > 1.0e-9f ? glm::normalize(toWorld(there)) : bow;
            environment.planetRadius = std::clamp(0.0016f * size / distance, 0.012f, orbitSize * 0.65f);
        }
        else
        {
            environment.planetDirection = below;
            environment.planetRadius = orbitSize;
            // Behind the planet, the sun is gone and the ship is in its shadow: only the lamps, and what light the
            // planet's day side throws back.
            const float apart = std::acos(std::clamp(glm::dot(towardsStar, below), -1.0f, 1.0f));
            const float shade = glm::smoothstep(orbitSize - 0.04f, orbitSize + 0.06f, apart);
            environment.sunIntensity *= glm::mix(0.06f, 1.0f, shade);
        }
    }

    // Everything else, as many as the sky draws: the nearest first, though what would be brightest counts for something --
    // a gas giant across the system is seen before a pebble of a moon round it.
    constexpr float kEarthRadiusAu = 4.26e-5f;
    std::vector<std::pair<float, int>> others;
    for (const Body& body : system->bodies)
    {
        if (body.index == main)
        {
            continue;
        }
        const float distance = glm::length(system->Position(body.index, m_campaign.clock) - ship);
        if (distance < 1.0e-7f)
        {
            continue;
        }
        others.emplace_back(distance / std::sqrt(std::max(body.radius, 0.05f)), body.index);
    }
    std::sort(others.begin(), others.end());
    for (int i = 0; i < Environment::kSkyBodies && i < static_cast<int>(others.size()); ++i)
    {
        const Body& body = system->bodies[static_cast<size_t>(others[static_cast<size_t>(i)].second)];
        const glm::vec3 there = system->Position(body.index, m_campaign.clock);
        const float distance = glm::length(there - ship);
        const glm::vec3 way = toWorld(there - ship);
        if (glm::length(way) < 1.0e-12f)
        {
            continue;
        }
        // As big as it really is from here; most are far too small to be anything but a point, which is what they are.
        const float radius = std::asin(std::min(std::max(body.radius, 0.02f) * kEarthRadiusAu / distance, 0.95f));
        // As bright as it is big, lit (by how far it is from its star) and near: brighter than the stars, the near and
        // the large much brighter.
        const float fromItsStar = std::max(glm::length(there), 0.05f);
        const float flux = body.radius * body.radius * system->luminosity / (fromItsStar * fromItsStar * distance * distance);
        const float bright = std::clamp(1.0f + 0.28f * std::log10(std::max(flux, 1.0e-12f)), 0.35f, 2.4f);
        environment.skyBodies[i * 2] = glm::vec4(glm::normalize(way) * bright, radius);
        environment.skyBodies[i * 2 + 1] = glm::vec4(glm::mix(body.groundA, body.cloudColor, body.clouds * 0.6f), body.air);
    }
}

} // namespace pred
