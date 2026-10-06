// Space out of the windows, in a campaign: the system as it is from where the ship is.
//
// The ship has a place in its system and an attitude in it. In orbit it goes round the body it is at, a few of that body's
// radii out, its nose pitched down so the body is in the windscreen. Leaving, it starts from where it was in that orbit, and
// the orbit's height fades into the distance as it goes; arriving, the last of the way is drawn in so that it comes into orbit
// exactly where it reaches it. It turns to a new heading at a ship's pace rather than at once: setting out, the world it is
// leaving swings across the windows and falls behind.
//
// The star is the sky's (its disc, its light). The planets and moons near enough to have a size are drawn as bodies --
// their ground, seas, cloud, craters and rings, as the navigation map draws them -- in a view of their own between the sky and
// the world (Renderer::kViewSkyBodies), from a camera that only turns with the eye; the rest are points of light in the sky.

#include "Game/PredationGame.h"

#include "Engine/Render/DepthConvention.h"
#include "Engine/Render/Renderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

constexpr float kEarthRadiusAu = 4.26e-5f;
// How far its nose is pitched down from the way it goes round, so the body below is in the windscreen (radians).
constexpr float kOrbitPitch = 0.95f;
// Under way, how it turns to a new heading: slowly, as a ship of its size does -- at most this many radians a second, gathering
// way and losing it again at this many radians a second, a second. Half way round takes a quarter of a minute.
constexpr float kTurnRate = 0.25f;
constexpr float kTurnEase = 0.08f;
// Coming into orbit, how long it takes to settle from the way it came in to the orbit's facing.
constexpr double kSettleSeconds = 7.0;
// Smaller than this across (radians from middle to edge), a body is a point of light in the sky rather than a body.
constexpr float kBodyAngle = 0.0035f;
// How far off the nearest body is drawn, in the bodies' view; the rest further by the square root of how much further they
// are, so they go behind one another in the right order.
constexpr float kNearestDrawn = 1000.0f;

float RadiusAu(const Body& body)
{
    return std::max(body.radius, 0.02f) * kEarthRadiusAu;
}

// An attitude facing `forward`, its top towards `upHint`: the ship's own right, up and back as columns.
glm::quat Facing(const glm::vec3& forward, const glm::vec3& upHint)
{
    const glm::vec3 f = glm::length(forward) > 1.0e-9f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 r = glm::cross(f, upHint);
    if (glm::length(r) < 1.0e-4f)
    {
        r = glm::cross(f, glm::vec3(1.0f, 0.0f, 0.0f));
    }
    r = glm::normalize(r);
    const glm::vec3 u = glm::cross(r, f);
    return glm::quat_cast(glm::mat3(r, u, -f));
}

} // namespace

void PredationGame::SetSpaceSky(Environment& environment)
{
    const StarSystem* system = CurrentSystem();
    m_spaceBodies.clear();
    if (system == nullptr)
    {
        return;
    }
    for (glm::vec4& body : environment.skyBodies)
    {
        body = glm::vec4(0.0f);
    }
    environment.planetRadius = 0.0f;
    const float dt = std::clamp(m_lastFrameSeconds, 0.0f, 0.1f);
    const glm::vec3 bow = glm::normalize(ShipSpec::kTravelHeading);
    const glm::vec3 starboard = glm::normalize(glm::cross(bow, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 overhead = glm::cross(starboard, bow);
    if (system->id.Packed() != m_spaceSystem)
    {
        m_spaceSystem = system->id.Packed();
        m_spaceAttitudeSet = false;
        m_spaceOrbitBody = -1;
    }

    // Between the stars: nothing near, the star it left a dimming sun astern and the one it is heading for a brightening
    // point dead ahead -- the nearer of the two the one that lights the ship.
    if (m_campaign.travel.interstellar)
    {
        m_spaceAttitudeSet = false;
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
        // The other star, as a point.
        const glm::vec3 other = nearerAhead ? astern : bow;
        const glm::vec3 otherColour = nearerAhead ? behindColour : ahead.starColor;
        environment.skyBodies[0] = glm::vec4(glm::normalize(other) * 2.0f, 1.0e-6f);
        environment.skyBodies[1] = glm::vec4(glm::mix(otherColour, glm::vec3(1.0f), 0.4f) * 3.0f, 0.0f);
        return;
    }

    // --- Where the ship is, and which way it faces -------------------------------------------------------------------------
    // Measured from the body near it when there is one -- the one it is in orbit of, leaving or coming to -- so a small one is
    // not lost in a float's steps at an astronomical unit from the star.
    const double clock = m_campaign.clock;
    const CampaignState::Travel& travel = m_campaign.travel;
    int reference = -1;
    glm::vec3 offset{0.0f};
    glm::vec3 wantForward{0.0f};
    glm::vec3 wantUp = m_spaceAttitudeSet ? m_spaceAttitude * glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    bool held = false;
    const Body* at = travel.underway ? nullptr : system->Find(m_campaign.body);
    if (at != nullptr)
    {
        // In orbit: going round, its nose pitched down to the ground below and held there -- the world stays where it is in the
        // windows while its ground goes by underneath.
        glm::vec3 out;
        glm::vec3 along;
        Travel::OrbitFrame(m_campaign, *system, at->index, clock, out, along);
        reference = at->index;
        offset = out * Travel::OrbitRadius(*at);
        wantForward = along * std::cos(kOrbitPitch) - out * std::sin(kOrbitPitch);
        wantUp = out * std::cos(kOrbitPitch) + along * std::sin(kOrbitPitch);
        held = true;
        // Just come into orbit: settling from the way it came in, over a few seconds.
        if (m_spaceOrbitBody != at->index || m_spaceOrbitSince != travel.orbitSince)
        {
            m_spaceOrbitBody = at->index;
            m_spaceOrbitSince = travel.orbitSince;
            m_spaceSettleFrom = m_spaceAttitudeSet && clock - travel.orbitSince < kSettleSeconds ? m_spaceAttitude : Facing(wantForward, wantUp);
        }
    }
    else if (const Body* from = travel.underway ? system->Find(travel.from) : nullptr; from != nullptr && Travel::AlignDone(m_campaign, *system) < 1.0f)
    {
        // Setting out: turning from the orbit's facing to the way it is going, while it swings round to that side of the world
        // and climbs a little -- the world sliding down out of the windows and round behind.
        glm::vec3 out;
        glm::vec3 along;
        Travel::OrbitFrame(m_campaign, *system, from->index, travel.setOut, out, along);
        reference = from->index;
        offset = Travel::AlignOffset(m_campaign, *system, clock);
        const glm::quat start = Facing(along * std::cos(kOrbitPitch) - out * std::sin(kOrbitPitch), out * std::cos(kOrbitPitch) + along * std::sin(kOrbitPitch));
        const glm::quat end = Facing(travel.departWay, start * glm::vec3(0.0f, 1.0f, 0.0f));
        const float s = Travel::AlignDone(m_campaign, *system);
        const glm::quat turned = glm::slerp(start, end, s * s * (3.0f - 2.0f * s));
        wantForward = turned * glm::vec3(0.0f, 0.0f, -1.0f);
        wantUp = turned * glm::vec3(0.0f, 1.0f, 0.0f);
        held = true;
    }
    else if (travel.underway)
    {
        // Under way: nose on where it is going, or, coming to a stop, the way it is going. Measured from whichever of what it left and where it is going is nearer.
        float nearest = 1.0e9f;
        for (const int body : {travel.from, travel.target})
        {
            if (system->Find(body) == nullptr)
            {
                continue;
            }
            const float distance = static_cast<float>(glm::length(system->PositionD(body, clock) - travel.position));
            if (distance < nearest)
            {
                nearest = distance;
                reference = body;
            }
        }
        if (reference >= 0)
        {
            offset = glm::vec3(travel.position - system->PositionD(reference, clock));
        }
        if (const Body* to = system->Find(travel.target))
        {
            // Nose on where it is going, all the way in: it comes in to the near side of it, and settles from there into the
            // orbit's facing. (Not its way through space, which near a world is mostly the world's own way round its star.)
            wantForward = glm::vec3(system->PositionD(to->index, clock) - travel.position);
        }
        else
        {
            wantForward = travel.velocity;
        }
    }
    const glm::vec3 ship = glm::vec3(reference >= 0 ? system->PositionD(reference, clock) + glm::dvec3(offset) : travel.position);

    // Held exactly to what it wants in orbit and setting out (settling into orbit from how it came in); under way turned towards
    // it at a ship's pace -- gathering way and losing it again, so it comes to the new heading without overshooting.
    if (glm::length(wantForward) > 1.0e-12f)
    {
        const glm::quat want = Facing(wantForward, wantUp);
        if (held || !m_spaceAttitudeSet)
        {
            m_spaceAttitude = want;
            m_spaceTurnSpeed = 0.0f;
            if (at != nullptr && clock - travel.orbitSince < kSettleSeconds && clock >= travel.orbitSince)
            {
                const float s = static_cast<float>((clock - travel.orbitSince) / kSettleSeconds);
                m_spaceAttitude = glm::slerp(m_spaceSettleFrom, want, s * s * (3.0f - 2.0f * s));
            }
            m_spaceAttitudeSet = true;
        }
        else
        {
            const float cosine = std::min(std::abs(glm::dot(m_spaceAttitude, want)), 1.0f);
            const float apart = 2.0f * std::acos(cosine);
            const float wanted = std::min(kTurnRate, std::sqrt(2.0f * kTurnEase * apart));
            m_spaceTurnSpeed = m_spaceTurnSpeed < wanted ? std::min(wanted, m_spaceTurnSpeed + kTurnEase * dt) : wanted;
            if (apart > 1.0e-5f)
            {
                m_spaceAttitude = glm::normalize(glm::slerp(m_spaceAttitude, want, std::min(1.0f, m_spaceTurnSpeed * dt / apart)));
            }
        }
    }
    // From the system's directions to the world's: the ship's own right, up and forward onto the rooms' starboard, overhead
    // and bow.
    m_spaceToWorld = glm::mat3(starboard, overhead, -bow) * glm::transpose(glm::mat3_cast(m_spaceAttitude));
    const auto toWorld = [&](const glm::vec3& v) { return m_spaceToWorld * v; };
    // Where a body is from the ship, measured from the reference body so the near one is exact.
    const glm::dvec3 origin = reference >= 0 ? system->PositionD(reference, clock) : travel.position;
    const auto fromShip = [&](int index) { return glm::vec3(system->PositionD(index, clock) - origin) - (reference >= 0 ? offset : glm::vec3(0.0f)); };


    // --- The star ----------------------------------------------------------------------------------------------------
    const float fromStar = std::max(glm::length(ship), 0.05f);
    const glm::vec3 towardsStar = glm::normalize(toWorld(-ship));
    environment.sunDirection = -towardsStar;
    environment.sunColor = glm::mix(system->starColor, glm::vec3(1.0f), 0.3f);
    environment.sunIntensity = 1.7f * std::clamp(std::sqrt(system->luminosity) / fromStar, 0.35f, 2.2f);
    // As big as the star is from here: a sun's radius is 0.00465 astronomical units; a little larger, so it reads.
    environment.sunDisc = std::clamp(0.00465f * std::max(system->starRadius, 0.1f) / fromStar * 1.3f, 0.0012f, 0.06f);

    m_spaceSunLight = environment.sunColor * environment.sunIntensity;

    // --- The planets and moons ----------------------------------------------------------------------------------------
    // Those near enough to have a size, as bodies (DrawSpaceBodies), the nearest first; the rest as points, as many as the
    // sky draws, the brightest first.
    std::vector<std::pair<float, int>> points;
    float shade = 1.0f;
    for (const Body& body : system->bodies)
    {
        const glm::vec3 there = system->Position(body.index, clock);
        const glm::vec3 v = fromShip(body.index);
        const float distance = glm::length(v);
        if (distance < 1.0e-12f)
        {
            continue;
        }
        const float angle = std::asin(std::min(RadiusAu(body) / distance, 0.995f));
        const glm::vec3 way = glm::normalize(toWorld(v));
        if (angle > kBodyAngle)
        {
            m_spaceBodies.push_back({body.index, way, angle, distance});
            // Between the ship and the star, it hides it: the ship is in its shadow.
            const float apart = std::acos(std::clamp(glm::dot(towardsStar, way), -1.0f, 1.0f));
            shade = std::min(shade, glm::smoothstep(angle - environment.sunDisc, angle + environment.sunDisc, apart));
            continue;
        }
        const float fromItsStar = std::max(glm::length(there), 0.05f);
        const float flux = body.radius * body.radius * system->luminosity / (fromItsStar * fromItsStar * distance * distance);
        points.emplace_back(-flux, body.index);
    }
    environment.sunIntensity *= glm::mix(0.06f, 1.0f, shade);
    std::sort(m_spaceBodies.begin(), m_spaceBodies.end(), [](const SpaceBody& a, const SpaceBody& b) { return a.distance < b.distance; });
    if (!m_spaceBodies.empty())
    {
        const float nearest = m_spaceBodies.front().distance;
        for (SpaceBody& body : m_spaceBodies)
        {
            body.depth = std::min(kNearestDrawn * std::sqrt(body.distance / nearest), 1.5e6f);
        }
    }
    std::sort(points.begin(), points.end());
    for (int i = 0; i < Environment::kSkyBodies && i < static_cast<int>(points.size()); ++i)
    {
        const Body& body = system->bodies[static_cast<size_t>(points[static_cast<size_t>(i)].second)];
        const float distance = glm::length(fromShip(body.index));
        const glm::vec3 way = toWorld(fromShip(body.index));
        // As bright as it is big, lit (by how far it is from its star) and near: brighter than the stars, the near and the
        // large much brighter.
        const float bright = std::clamp(1.0f + 0.28f * std::log10(std::max(-points[static_cast<size_t>(i)].first, 1.0e-12f)), 0.35f, 2.4f);
        environment.skyBodies[i * 2] = glm::vec4(glm::normalize(way) * bright, std::asin(std::min(RadiusAu(body) / distance, 0.95f)));
        environment.skyBodies[i * 2 + 1] = glm::vec4(glm::mix(body.groundA, body.cloudColor, body.clouds * 0.6f), body.air);
    }
}

void PredationGame::DrawSpaceBodies()
{
    const StarSystem* system = CurrentSystem();
    if (m_spaceBodies.empty() || system == nullptr || !m_planets.IsValid() || !m_skyInShip || m_screen == Screen::Title)
    {
        return;
    }
    // Only turned as the eye is: the bodies are where they are in the sky, however the eye moves about the ship.
    const Renderer& renderer = m_app->GetRenderer();
    glm::mat4 view = renderer.ViewMatrix();
    view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    // Their depth beyond all of the world's, which is already drawn and hides them where it is: so near a near plane that a body
    // a thousand units off is nearer the far end than any of the world's surfaces within kilometres of the eye.
    const glm::mat4 projection = Depth::Perspective(m_renderVerticalFov, m_renderAspect, 1.0e-3f, 4.0e6f);
    bgfx::setViewTransform(Renderer::kViewSkyBodies, glm::value_ptr(view), glm::value_ptr(projection));
    m_planets.SetOutput(true, Depth::Test());
    m_planets.SetCamera(glm::vec3(0.0f), 1.0f);
    const float time = static_cast<float>(std::fmod(m_campaign.clock, 100000.0));
    const glm::mat4 turn(m_spaceToWorld);
    // Lit as everything else in the picture is: a surface gives back its colour over pi of the light falling on it (the
    // scene's shading), where the planets' own shading gives back its colour times 1.6 of it -- a world out of the windows
    // was three times as bright as the ship's own hull beside it.
    const glm::vec3 light = m_spaceSunLight / (3.14159265f * 1.6f);
    for (const SpaceBody& drawn : m_spaceBodies)
    {
        const Body* body = system->Find(drawn.index);
        if (body == nullptr)
        {
            continue;
        }
        // As big as it really looks, wherever it is drawn: its radius over how far off it is drawn is its true angle.
        const float radius = drawn.depth * std::sin(drawn.angle);
        float spin = 0.0f;
        system->SunOver(body->index, m_campaign.clock, &spin);
        glm::mat4 frame = glm::translate(glm::mat4(1.0f), drawn.direction * drawn.depth) * turn;
        frame = glm::rotate(frame, body->tilt, glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::mat4 rings = glm::scale(frame, glm::vec3(radius));
        const glm::mat4 model = glm::scale(glm::rotate(frame, spin, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(radius));
        const glm::vec3 there = system->Position(body->index, m_campaign.clock);
        const glm::vec3 towardsStar = glm::length(there) > 1.0e-9f ? glm::normalize(m_spaceToWorld * -there) : glm::vec3(0.0f, 1.0f, 0.0f);
        const PlanetLook look = LookOf(*body);
        m_planets.Body(Renderer::kViewSkyBodies, model, look, towardsStar, light, 0.0f, time * 0.02f);
        m_planets.Rings(Renderer::kViewSkyBodies, rings, look, towardsStar, light);
    }
    // Back as the map wants it.
    m_planets.SetOutput(false, BGFX_STATE_DEPTH_TEST_LESS);
}

} // namespace pred
