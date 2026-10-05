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

// Each player's colour on the map, by their number.
constexpr ImU32 kPlayerColours[kMaxPlayers] = {IM_COL32(236, 156, 64, 255), IM_COL32(90, 200, 230, 255), IM_COL32(130, 220, 120, 255),
                                               IM_COL32(220, 120, 220, 255)};
constexpr ImU32 kAmber = IM_COL32(236, 156, 64, 255);
constexpr ImU32 kText = IM_COL32(220, 226, 230, 255);
constexpr ImU32 kDim = IM_COL32(130, 138, 146, 255);

// How far the ship's sensors see while under way, in astronomical units, by their tier.
float SensorRange(int tier)
{
    return 0.05f * std::pow(3.0f, static_cast<float>(std::clamp(tier, 0, 6)));
}

std::string About(float seconds)
{
    if (seconds < 50.0f)
    {
        return "under a minute";
    }
    const int minutes = static_cast<int>(std::round(seconds / 60.0f));
    return "about " + std::to_string(std::max(minutes, 1)) + " min";
}

// A region's seed as the site builder takes it.
uint16_t SiteSeed(uint32_t seed)
{
    const auto folded = static_cast<uint16_t>((seed ^ (seed >> 16)) & 0xFFFFu);
    return folded == 0 ? uint16_t{1} : folded;
}

uint32_t Abgr(int r, int g, int b, int a)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
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
    if (!RegionKnown(body, region) || !body.Landable())
    {
        return false;
    }
    // Somewhere with services is docked at, not gone down to in the shuttle: that comes with the shipyard.
    const RegionKindDef* kind = m_universeData.RegionKind(body.regions[static_cast<size_t>(region)].kind);
    return kind == nullptr || !kind->service;
}

// --- Opening and closing ----------------------------------------------------------------------------------------------

void PredationGame::OpenSystemMap()
{
    const StarSystem* system = CurrentSystem();
    if (system == nullptr)
    {
        return;
    }
    m_mapOpen = true;
    m_inventoryOpen = false;
    CloseLoadout();
    m_wantMouseCaptured = false;
    UpdateMouseCapture();
    if (m_mapSelected < 0 || m_mapSelected >= static_cast<int>(system->bodies.size()))
    {
        m_mapSelected = m_campaign.travel.underway ? m_campaign.travel.target : m_campaign.body;
    }
    if (!m_mapFramed)
    {
        // The whole system in view at first, from above and to one side.
        float outermost = 10.0f;
        for (const SystemMapView::Drawn& drawn : SystemMapView::Layout(*system, m_campaign.clock))
        {
            outermost = std::max(outermost, glm::length(drawn.at));
        }
        m_mapView.Focus(glm::vec3(0.0f), outermost * 2.2f);
        m_mapFramed = true;
    }
    PlayNamed("UI/confirm", m_renderEye, 0.5f, 1.0f, false);
}

void PredationGame::CloseSystemMap()
{
    if (!m_mapOpen)
    {
        return;
    }
    m_mapOpen = false;
    m_mapHovered = -1;
    m_wantMouseCaptured = !m_paused;
    UpdateMouseCapture();
    PlayNamed("UI/back", m_renderEye, 0.5f, 1.0f, false);
}

void PredationGame::DestroySystemMapTarget()
{
    if (bgfx::isValid(m_mapBuffer))
    {
        bgfx::destroy(m_mapBuffer);
    }
    m_mapBuffer = BGFX_INVALID_HANDLE;
    m_mapTexture = BGFX_INVALID_HANDLE;
    m_mapWidth = 0;
    m_mapHeight = 0;
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
        if (!Travel::SetCourse(m_campaign, *system, a))
        {
            return false;
        }
        m_campaign.travel.region = b;
        m_shipReady = false;
        m_shipOrbiting = 0;
        PRED_LOG_INFO(Gameplay, "Player {} set a course for {}", player, system->bodies[static_cast<size_t>(a)].name);
        if (leaving && HasCinematic("ship_depart"))
        {
            PlayCinematic("ship_depart");
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
        region = -1;
        for (int i = 0; i < static_cast<int>(body->regions.size()) && region < 0; ++i)
        {
            region = RegionLandable(*body, i) ? i : -1;
        }
    }
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
    const int pointer = m_mapOpen ? (m_mapHovered >= 0 ? m_mapHovered : m_mapSelected) : -1;
    if (m_sessionMode == SessionMode::Client)
    {
        if (m_client.TravelsReceived() != m_appliedTravels)
        {
            m_appliedTravels = m_client.TravelsReceived();
            const TravelMessage& travel = m_client.LatestTravel();
            m_campaign.clock = travel.clock;
            m_campaign.travel.underway = travel.underway;
            m_campaign.travel.target = travel.target;
            m_campaign.travel.position = travel.position;
            m_campaign.travel.velocity = travel.velocity;
            m_campaign.travel.region = travel.region;
            m_campaign.body = travel.body;
            m_pointing = travel.pointing;
            m_mapOpenMask = travel.mapOpen;
        }
        else if (m_campaign.travel.underway)
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
        if (m_campaign.travel.underway && !CinematicHoldsWorld())
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
                travel.underway = m_campaign.travel.underway;
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

// --- The picture ----------------------------------------------------------------------------------------------------

void PredationGame::RenderSystemMap()
{
    const StarSystem* system = CurrentSystem();
    if (!m_mapOpen || system == nullptr || !m_planets.IsValid())
    {
        return;
    }
    const Renderer& renderer = m_app->GetRenderer();
    const auto width = static_cast<uint16_t>(std::max(renderer.Width(), 16));
    const auto height = static_cast<uint16_t>(std::max(renderer.Height(), 16));
    if (width != m_mapWidth || height != m_mapHeight)
    {
        DestroySystemMapTarget();
    }
    if (!bgfx::isValid(m_mapBuffer))
    {
        const uint64_t flags = BGFX_TEXTURE_RT_MSAA_X4 | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureHandle attachments[2] = {
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, flags),
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_MSAA_X4 | BGFX_TEXTURE_RT_WRITE_ONLY)};
        if (!bgfx::isValid(attachments[0]) || !bgfx::isValid(attachments[1]))
        {
            PRED_LOG_ERROR(Render, "System map: could not create its render target");
            return;
        }
        m_mapBuffer = bgfx::createFrameBuffer(2, attachments, true);
        m_mapTexture = attachments[0];
        m_mapWidth = width;
        m_mapHeight = height;
    }

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const bool homogeneous = renderer.HomogeneousDepth();
    const glm::mat4 view = m_mapView.View();
    const glm::mat4 projection = m_mapView.Projection(aspect, homogeneous);
    const glm::vec3 eye = m_mapView.Eye();
    const bgfx::ViewId sky = Renderer::kViewMapFirst;
    const bgfx::ViewId bodies = static_cast<bgfx::ViewId>(sky + 1);
    const bgfx::ViewId glow = static_cast<bgfx::ViewId>(sky + 2);
    const bgfx::ViewId lines = static_cast<bgfx::ViewId>(sky + 3);
    for (bgfx::ViewId id = sky; id <= lines; ++id)
    {
        bgfx::setViewFrameBuffer(id, m_mapBuffer);
        bgfx::setViewRect(id, 0, 0, width, height);
        bgfx::setViewTransform(id, glm::value_ptr(view), glm::value_ptr(projection));
        bgfx::setViewClear(id, id == sky ? (BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH) : BGFX_CLEAR_NONE, 0x000000ff, 1.0f, 0);
        bgfx::touch(id);
    }

    // The stars behind it all, and the sky's sun where the star is, which the star itself then covers.
    Environment space;
    space.stars = 1.0f;
    space.planetRadius = 0.0f;
    space.sunDirection = glm::length(eye) > 1.0e-3f ? glm::normalize(eye) : glm::vec3(0.0f, -1.0f, 0.0f);
    space.sunColor = system->starColor;
    space.fogColor = glm::vec3(0.0f);
    space.skySun = 0.0f;
    m_app->GetSkyRenderer().Draw(sky, space, view, projection);

    m_planets.SetCamera(eye, 1.0f);
    const float time = static_cast<float>(std::fmod(m_campaign.clock, 100000.0));
    m_planets.Star(bodies, glow, glm::vec3(0.0f), SystemMapView::kStarRadius, system->starColor, m_mapView.Right(), m_mapView.Up(), time);

    const std::vector<SystemMapView::Drawn> drawn = SystemMapView::Layout(*system, m_campaign.clock);
    for (const Body& body : system->bodies)
    {
        const SystemMapView::Drawn& at = drawn[body.index];
        // Turned on its axis as the day goes round, the axis tipped by its tilt.
        glm::mat4 model = glm::translate(glm::mat4(1.0f), at.at);
        model = glm::rotate(model, body.tilt, glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::mat4 ringModel = glm::scale(model, glm::vec3(at.radius));
        model = glm::rotate(model, static_cast<float>(std::fmod(m_campaign.clock / std::max(body.day, 1.0f), 1.0)) * kTau, glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(at.radius));
        float highlight = body.index == m_mapSelected ? 1.0f : body.index == m_mapHovered ? 0.55f : 0.0f;
        for (int player = 0; player < kMaxPlayers; ++player)
        {
            highlight = std::max(highlight, m_pointing[static_cast<size_t>(player)] == body.index && player != LocalPlayerId() ? 0.45f : 0.0f);
        }
        const glm::vec3 towardsStar = glm::length(at.at) > 1.0e-4f ? -glm::normalize(at.at) : glm::vec3(0.0f, 1.0f, 0.0f);
        const PlanetLook look = LookOf(body);
        m_planets.Body(bodies, model, look, towardsStar, system->starColor, highlight, time * 0.02f);
        m_planets.Rings(glow, ringModel, look, towardsStar, system->starColor);

        // Its orbit: faint, brighter for the one picked out.
        const bool picked = body.index == m_mapSelected;
        const std::vector<glm::vec3> loop = SystemMapView::Orbit(*system, body.index, drawn, m_campaign.clock, body.kind == BodyKind::Planet ? 160 : 48);
        const uint32_t colour = picked ? Abgr(236, 156, 64, 170) : body.kind == BodyKind::Planet ? Abgr(120, 140, 170, 70) : Abgr(120, 140, 170, 35);
        for (size_t i = 1; i < loop.size(); ++i)
        {
            m_planets.Line(loop[i - 1], loop[i], colour);
        }
    }

    // The ship, and where it is heading.
    const glm::vec3 shipAu = Travel::ShipPosition(m_campaign, *system);
    const glm::vec3 ship = SystemMapView::ShipAt(drawn, m_campaign.travel.underway ? -1 : m_campaign.body, shipAu);
    const float mark = std::max(m_mapView.Distance() * 0.008f, 0.08f);
    const uint32_t shipColour = Abgr(255, 236, 200, 255);
    m_planets.Line(ship - glm::vec3(mark, 0.0f, 0.0f), ship + glm::vec3(mark, 0.0f, 0.0f), shipColour);
    m_planets.Line(ship - glm::vec3(0.0f, mark, 0.0f), ship + glm::vec3(0.0f, mark, 0.0f), shipColour);
    m_planets.Line(ship - glm::vec3(0.0f, 0.0f, mark), ship + glm::vec3(0.0f, 0.0f, mark), shipColour);
    if (m_campaign.travel.underway && m_campaign.travel.target >= 0)
    {
        const glm::vec3 there = drawn[static_cast<size_t>(m_campaign.travel.target)].at;
        // Dashed, so it reads as a course and not an orbit.
        constexpr int kDashes = 40;
        for (int i = 0; i < kDashes; i += 2)
        {
            const float t0 = static_cast<float>(i) / kDashes;
            const float t1 = static_cast<float>(i + 1) / kDashes;
            m_planets.Line(glm::mix(ship, there, t0), glm::mix(ship, there, t1), Abgr(236, 156, 64, 220));
        }
    }
    m_planets.FlushLines(lines);
}

void PredationGame::DrawSystemMap()
{
    const StarSystem* system = CurrentSystem();
    if (!m_mapOpen || system == nullptr)
    {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;
    const float aspect = size.x / std::max(size.y, 1.0f);
    const bool homogeneous = m_app->GetRenderer().HomogeneousDepth();
    m_mapView.Update(ImGui::GetIO().DeltaTime);
    const std::vector<SystemMapView::Drawn> drawn = SystemMapView::Layout(*system, m_campaign.clock);

    // The picture, and the whole of it a surface to turn, zoom and point with.
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(size);
    constexpr ImGuiWindowFlags kBack = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin("##systemmap", nullptr, kBack);
    ImGui::PopStyleVar();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (bgfx::isValid(m_mapTexture))
    {
        draw->AddImage(static_cast<ImTextureID>(ImGuiLayer::TextureId(m_mapTexture)), origin, {origin.x + size.x, origin.y + size.y});
    }
    else
    {
        draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(0, 0, 0, 255));
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##mapsurface", size);
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse{(io.MousePos.x - origin.x) / size.x, (io.MousePos.y - origin.y) / size.y};
    m_mapHovered = ImGui::IsItemHovered() ? m_mapView.Pick(mouse, drawn, aspect, homogeneous) : -1;
    if (ImGui::IsItemActivated())
    {
        m_mapDragged = false;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
    {
        m_mapView.Turn(-io.MouseDelta.x * 0.006f, io.MouseDelta.y * 0.006f);
        m_mapDragged = true;
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
    {
        m_mapView.Zoom(std::pow(0.87f, io.MouseWheel));
    }
    if (ImGui::IsItemDeactivated() && !m_mapDragged)
    {
        m_mapSelected = m_mapHovered;
        if (m_mapSelected >= 0)
        {
            PlayNamed("UI/click", m_renderEye, 0.5f, 1.0f, false);
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m_mapHovered >= 0)
    {
        m_mapView.Focus(drawn[static_cast<size_t>(m_mapHovered)].at, drawn[static_cast<size_t>(m_mapHovered)].radius * 14.0f);
    }

    // Names by the bodies: every planet's; a moon's when close enough to tell them apart, or when it is picked out.
    ImFont* font = ImGui::GetFont();
    const float small = ImGui::GetFontSize() * 0.85f;
    const auto toScreen = [&](const glm::vec3& at, ImVec2& out)
    {
        glm::vec2 point;
        if (!m_mapView.OnScreen(at, aspect, homogeneous, point) || point.x < -0.1f || point.x > 1.1f || point.y < -0.1f || point.y > 1.1f)
        {
            return false;
        }
        out = {origin.x + point.x * size.x, origin.y + point.y * size.y};
        return true;
    };
    for (const Body& body : system->bodies)
    {
        const SystemMapView::Drawn& at = drawn[body.index];
        const bool picked = body.index == m_mapSelected || body.index == m_mapHovered;
        if (body.kind == BodyKind::Moon && !picked && m_mapView.Distance() > 30.0f)
        {
            continue;
        }
        // A planet's name over it; a moon's to its side, out of its planet's way.
        ImVec2 point;
        const bool moon = body.kind == BodyKind::Moon;
        if (!toScreen(moon ? at.at : at.at + glm::vec3(0.0f, at.radius * 1.3f, 0.0f), point))
        {
            continue;
        }
        const uint8_t known = m_campaign.Known(m_campaign.system, body.index);
        const ImU32 colour = picked ? kAmber : known != 0 ? kText : kDim;
        const ImVec2 extent = font->CalcTextSizeA(small, FLT_MAX, 0.0f, body.name.c_str());
        const ImVec2 corner = moon ? ImVec2{point.x + 10.0f, point.y - extent.y * 0.5f} : ImVec2{point.x - extent.x * 0.5f, point.y - extent.y};
        draw->AddText(font, small, {corner.x + 1.0f, corner.y + 1.0f}, IM_COL32(0, 0, 0, 200), body.name.c_str());
        draw->AddText(font, small, corner, colour, body.name.c_str());
    }
    // The ship.
    {
        const glm::vec3 ship = SystemMapView::ShipAt(drawn, m_campaign.travel.underway ? -1 : m_campaign.body, Travel::ShipPosition(m_campaign, *system));
        ImVec2 point;
        if (toScreen(ship, point))
        {
            draw->AddCircle(point, 7.0f, IM_COL32(255, 236, 200, 230), 0, 1.5f);
            draw->AddText(font, small, {point.x + 10.0f, point.y - small * 0.5f}, IM_COL32(255, 236, 200, 230), "SHIP");
        }
    }
    // What everybody else is pointing at: a ring round it in their colour, and their name.
    {
        const std::vector<RemotePlayerView>& remotes = RemotePlayers();
        for (int player = 0; player < kMaxPlayers; ++player)
        {
            const int body = m_pointing[static_cast<size_t>(player)];
            if (player == LocalPlayerId() || body < 0 || body >= static_cast<int>(drawn.size()))
            {
                continue;
            }
            std::string name = "Player " + std::to_string(player + 1);
            for (const RemotePlayerView& remote : remotes)
            {
                name = remote.id == player ? remote.name : name;
            }
            ImVec2 point;
            if (toScreen(drawn[static_cast<size_t>(body)].at, point))
            {
                draw->AddCircle(point, 18.0f + 4.0f * static_cast<float>(player), kPlayerColours[player], 0, 2.0f);
                draw->AddText(font, small, {point.x + 22.0f, point.y + 6.0f + small * static_cast<float>(player)}, kPlayerColours[player], name.c_str());
            }
        }
    }
    ImGui::End();

    constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    const StarDef* star = nullptr;
    for (const StarDef& def : m_universeData.stars)
    {
        star = def.id == system->star ? &def : star;
    }

    // The bodies, down the left.
    ImGui::SetNextWindowPos({origin.x + 20.0f, origin.y + 20.0f});
    ImGui::SetNextWindowSize({280.0f, size.y - 140.0f});
    ImGui::SetNextWindowBgAlpha(0.72f);
    if (ImGui::Begin("##mapbodies", nullptr, kPanel))
    {
        ImGui::TextColored({0.92f, 0.62f, 0.25f, 1.0f}, "NAVIGATION");
        ImGui::TextUnformatted(system->name.c_str());
        ImGui::TextDisabled("%s", star != nullptr ? star->name.c_str() : system->star.c_str());
        ImGui::Separator();
        for (const Body& body : system->bodies)
        {
            ImGui::PushID(body.index);
            std::string label = (body.kind == BodyKind::Moon ? "    " : "") + body.name;
            if (!m_campaign.travel.underway && body.index == m_campaign.body)
            {
                label += "   (here)";
            }
            else if (m_campaign.travel.underway && body.index == m_campaign.travel.target)
            {
                label += "   (heading)";
            }
            for (int player = 0; player < kMaxPlayers; ++player)
            {
                if (player != LocalPlayerId() && m_pointing[static_cast<size_t>(player)] == body.index)
                {
                    label += "  *";
                }
            }
            if (ImGui::Selectable(label.c_str(), body.index == m_mapSelected))
            {
                m_mapSelected = body.index;
                m_mapView.Focus(drawn[body.index].at, drawn[body.index].radius * 14.0f);
            }
            if (ImGui::IsItemHovered())
            {
                m_mapHovered = body.index;
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (ImGui::SmallButton("Whole system"))
        {
            float outermost = 10.0f;
            for (const SystemMapView::Drawn& at : drawn)
            {
                outermost = std::max(outermost, glm::length(at.at));
            }
            m_mapView.Focus(glm::vec3(0.0f), outermost * 2.2f);
        }
    }
    ImGui::End();

    // What is known of the one picked out, down the right.
    const Body* picked = system->Find(m_mapSelected);
    if (picked != nullptr)
    {
        ImGui::SetNextWindowPos({origin.x + size.x - 360.0f, origin.y + 20.0f});
        ImGui::SetNextWindowSize({340.0f, 0.0f});
        ImGui::SetNextWindowBgAlpha(0.78f);
        if (ImGui::Begin("##mapinfo", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize))
        {
            const uint8_t known = m_campaign.Known(m_campaign.system, picked->index);
            const bool records = (known & CampaignState::kKnownRecords) != 0;
            const bool scanned = (known & (CampaignState::kKnownScanned | CampaignState::kKnownVisited)) != 0;
            const bool deep = (known & (CampaignState::kKnownDeep | CampaignState::kKnownVisited)) != 0;
            ImGui::TextUnformatted(picked->name.c_str());
            if (picked->kind == BodyKind::Moon)
            {
                ImGui::TextDisabled("Moon of %s", system->bodies[static_cast<size_t>(picked->parent)].name.c_str());
            }
            else
            {
                ImGui::TextDisabled("Planet, %.2f AU from the star", picked->orbit);
            }
            ImGui::Separator();
            const auto row = [](const char* what, const std::string& value, bool knownValue)
            {
                ImGui::TextDisabled("%s", what);
                ImGui::SameLine(110.0f);
                if (knownValue)
                {
                    ImGui::TextUnformatted(value.c_str());
                }
                else
                {
                    ImGui::TextDisabled("Unknown");
                }
            };
            const BiomeDef* biome = m_universeData.Biome(picked->biome);
            row("Type", biome != nullptr ? biome->name : picked->biome, picked->gas || scanned || records);
            if (!picked->gas)
            {
                const AtmosphereDef* air = m_universeData.Atmosphere(picked->atmosphere);
                row("Atmosphere", air != nullptr ? air->name : picked->atmosphere, scanned);
                row("Temperature", std::to_string(static_cast<int>(std::round(picked->temperature))) + " C", scanned);
                const WeatherDef* weather = m_universeData.Weather(picked->weather);
                row("Weather", weather != nullptr ? weather->name : picked->weather, scanned);
                const TerrainDef* terrain = m_universeData.Terrain(picked->terrain);
                row("Terrain", terrain != nullptr ? terrain->name : picked->terrain, deep);
                const CivilizationDef* civ = m_universeData.Civilization(picked->civilization);
                std::string settled = civ != nullptr ? civ->name : std::string("None");
                if (!records && scanned && civ != nullptr && !civ->inhabited)
                {
                    settled = civ->records ? "Structures detected" : "None detected";
                }
                row("Settlement", settled, records || scanned);
            }
            std::string traits;
            for (const std::string& id : picked->specials)
            {
                const SpecialDef* special = m_universeData.Special(id);
                const bool visible = id == "rings" || id == "aurora";
                if (deep || (visible && scanned))
                {
                    traits += (traits.empty() ? "" : ", ") + (special != nullptr ? special->name : id);
                }
            }
            if (!traits.empty())
            {
                row("Notable", traits, true);
            }

            // How far, and how long to get there.
            const glm::vec3 ship = Travel::ShipPosition(m_campaign, *system);
            const float distance = glm::length(system->Position(picked->index, m_campaign.clock) - ship);
            const bool here = !m_campaign.travel.underway && picked->index == m_campaign.body;
            if (!here)
            {
                char away[64];
                std::snprintf(away, sizeof(away), "%.2f AU, ", distance);
                row("Distance", away + About(Travel::Seconds(distance, DriveTier())), true);
            }

            // Places to go down.
            if (picked->Landable())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("LANDING REGIONS");
                int shown = 0;
                const bool heading = (m_campaign.travel.underway && m_campaign.travel.target == picked->index) || here;
                for (int i = 0; i < static_cast<int>(picked->regions.size()); ++i)
                {
                    if (!RegionKnown(*picked, i))
                    {
                        continue;
                    }
                    ++shown;
                    const LandingRegion& region = picked->regions[static_cast<size_t>(i)];
                    const bool landable = RegionLandable(*picked, i);
                    const bool chosen = heading && m_campaign.travel.region == i;
                    ImGui::PushID(i);
                    ImGui::BeginDisabled(!landable);
                    if (ImGui::Selectable(region.designation.c_str(), chosen) && heading && landable)
                    {
                        AskCampaign(CampaignAction::SetRegion, i);
                    }
                    ImGui::EndDisabled();
                    if (!landable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip("A service location: docking comes with the shipyard.");
                    }
                    ImGui::PopID();
                }
                if (shown == 0)
                {
                    ImGui::TextDisabled(scanned ? "None found yet." : "None charted. Go closer to look.");
                }
            }

            ImGui::Spacing();
            const bool aboard = m_map == MapChoice::Ship;
            if (here)
            {
                ImGui::TextColored({0.92f, 0.62f, 0.25f, 1.0f}, m_shipReady ? "In orbit. The shuttle is ready in the hangar." : "In orbit.");
            }
            else if (m_campaign.travel.underway && m_campaign.travel.target == picked->index)
            {
                ImGui::TextColored({0.92f, 0.62f, 0.25f, 1.0f}, "On course.");
            }
            else
            {
                ImGui::BeginDisabled(!aboard || m_cine.Active());
                if (ImGui::Button("Set course", {-1.0f, 32.0f}))
                {
                    // Down to whichever region is picked out, or the first that can be gone down to.
                    int region = -1;
                    for (int i = 0; i < static_cast<int>(picked->regions.size()) && region < 0; ++i)
                    {
                        region = RegionLandable(*picked, i) ? i : -1;
                    }
                    AskCampaign(CampaignAction::SetCourse, picked->index, region);
                    PlayNamed("UI/confirm", m_renderEye, 0.6f, 1.0f, false);
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }

    // How the ship stands, along the bottom, and the way out.
    ImGui::SetNextWindowPos({origin.x + size.x * 0.5f, origin.y + size.y - 20.0f}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.78f);
    if (ImGui::Begin("##mapstatus", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize))
    {
        const Body* target = system->Find(m_campaign.travel.target);
        const Body* at = system->Find(m_campaign.body);
        if (m_campaign.travel.underway && target != nullptr)
        {
            const float left = glm::length(system->Position(target->index, m_campaign.clock) - m_campaign.travel.position);
            ImGui::Text("Under way to %s, %s.", target->name.c_str(), About(Travel::Seconds(left, DriveTier())).c_str());
        }
        else if (m_campaign.travel.underway)
        {
            ImGui::TextUnformatted("Coming to a stop.");
        }
        else if (at != nullptr)
        {
            const int region = m_campaign.travel.region;
            if (region >= 0 && region < static_cast<int>(at->regions.size()))
            {
                ImGui::Text("In orbit of %s.  Going down to %s.", at->name.c_str(), at->regions[static_cast<size_t>(region)].designation.c_str());
            }
            else
            {
                ImGui::Text("In orbit of %s.", at->name.c_str());
            }
        }
        else
        {
            ImGui::TextUnformatted("Holding position between the planets.");
        }
        if (m_campaign.travel.underway && m_campaign.travel.target >= 0)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Call off the course"))
            {
                AskCampaign(CampaignAction::CancelCourse);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Close"))
        {
            CloseSystemMap();
        }
        ImGui::TextDisabled("Drag to turn, wheel to zoom, click to pick out, double-click to go to it. Esc closes.");
    }
    ImGui::End();
}

// --- Space out of the windows ------------------------------------------------------------------------------------

void PredationGame::SetSpaceSky(Environment& environment)
{
    // The system as it is from where the ship is: the star where it really is, the planet it is over or heading for big
    // ahead or below, and every other body near enough to see a lit disc in its own direction. Directions in the system
    // are turned into the ship's: forward is the way it is heading (the bow), up is the system's up.
    const StarSystem* system = CurrentSystem();
    if (system == nullptr)
    {
        return;
    }
    const glm::vec3 ship = Travel::ShipPosition(m_campaign, *system);
    const int main = m_campaign.travel.underway ? m_campaign.travel.target : m_campaign.body;
    glm::vec3 heading{0.0f, 0.0f, -1.0f};
    if (m_campaign.travel.underway && main >= 0)
    {
        heading = system->Position(main, m_campaign.clock) - ship;
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
    const glm::vec3 bow = glm::normalize(ShipSpec::kTravelHeading);
    const glm::vec3 starboard = glm::normalize(glm::cross(bow, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 overhead = glm::cross(starboard, bow);
    const auto toWorld = [&](const glm::vec3& v) { return glm::dot(v, right) * starboard + glm::dot(v, up) * overhead + glm::dot(v, forward) * bow; };

    // The star.
    const float fromStar = std::max(glm::length(ship), 0.05f);
    const glm::vec3 towardsStar = glm::normalize(toWorld(-ship));
    environment.sunDirection = -towardsStar;
    environment.sunColor = glm::mix(system->starColor, glm::vec3(1.0f), 0.3f);
    environment.sunIntensity = 1.7f * std::clamp(std::sqrt(system->luminosity) / fromStar, 0.35f, 2.2f);

    // The body it is over or heading for.
    environment.planetRadius = 0.0f;
    if (const Body* body = system->Find(main))
    {
        environment.planet = LookOf(*body);
        if (m_campaign.travel.underway)
        {
            // Dead ahead, growing as the ship closes on it: a point a long way off, the size it is from orbit at the end.
            const float distance = std::max(glm::length(system->Position(main, m_campaign.clock) - ship), Travel::kArrival);
            const float size = std::sqrt(std::max(body->radius, 0.1f));
            environment.planetDirection = bow;
            environment.planetRadius = std::clamp(0.0016f * size / distance, 0.012f, 0.6f);
        }
        else
        {
            environment.planetDirection = glm::normalize(glm::vec3(0.12f, -0.42f, -0.9f));
            environment.planetRadius = body->gas ? 1.05f : 0.92f;
        }
    }

    // Everything else, the nearest first, as many as the sky draws.
    std::vector<std::pair<float, int>> others;
    for (const Body& body : system->bodies)
    {
        if (body.index == main)
        {
            continue;
        }
        others.emplace_back(glm::length(system->Position(body.index, m_campaign.clock) - ship), body.index);
    }
    std::sort(others.begin(), others.end());
    for (int i = 0; i < Environment::kSkyBodies; ++i)
    {
        environment.skyBodies[i * 2] = glm::vec4(0.0f);
        environment.skyBodies[i * 2 + 1] = glm::vec4(0.0f);
        if (i >= static_cast<int>(others.size()))
        {
            continue;
        }
        const Body& body = system->bodies[static_cast<size_t>(others[static_cast<size_t>(i)].second)];
        const float distance = std::max(others[static_cast<size_t>(i)].first, 1.0e-4f);
        const glm::vec3 way = toWorld(system->Position(body.index, m_campaign.clock) - ship);
        if (glm::length(way) < 1.0e-9f)
        {
            continue;
        }
        // Larger than life, so a planet across the system is a speck rather than nothing; a moon of the body below is a
        // proper disc.
        const float size = std::sqrt(std::max(body.radius, 0.05f));
        const float radius = std::clamp(0.0012f * size / distance, 0.003f, 0.14f);
        environment.skyBodies[i * 2] = glm::vec4(glm::normalize(way), radius);
        environment.skyBodies[i * 2 + 1] = glm::vec4(glm::mix(body.groundA, body.cloudColor, body.clouds * 0.6f), body.air);
    }
}

} // namespace pred
