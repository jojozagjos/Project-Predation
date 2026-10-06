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

constexpr float kTau = 6.28318530718f;
constexpr float kEarthRadiusAu = 4.26e-5f;
// How long the ship takes to go once round what it orbits, as the windows show it.
constexpr double kOrbitSeconds = 960.0;
// How far its nose is pitched down from the way it goes round, so the body below is in the windscreen (radians).
constexpr float kOrbitPitch = 0.95f;
// How it turns to a new heading: slowly, as a ship of its size does -- at most this many radians a second, gathering way
// and losing it again at this many radians a second, a second. Half way round takes the best part of a minute.
constexpr float kTurnRate = 0.07f;
constexpr float kTurnEase = 0.012f;
// Smaller than this across (radians from middle to edge), a body is a point of light in the sky rather than a body.
constexpr float kBodyAngle = 0.0035f;
// Leaving a body, how it seems to fall away: the distance shown grows by e every this many seconds from the orbit's height,
// until it has caught up with how far the ship has really gone. The trips are quick -- a planet's width in the first second
// -- and the world left behind would be gone before the ship had begun to turn from it.
constexpr float kFallAway = 10.0f;
// How far off the nearest body is drawn, in the bodies' view; the rest further by the square root of how much further they
// are, so they go behind one another in the right order.
constexpr float kNearestDrawn = 1000.0f;

float RadiusAu(const Body& body)
{
    return std::max(body.radius, 0.02f) * kEarthRadiusAu;
}

// How far from a body's middle the ship goes round it.
float OrbitRadius(const Body& body)
{
    return RadiusAu(body) * (body.gas ? 1.35f : 1.6f);
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
        m_spaceShipSet = false;
        m_spaceOrbitBody = -1;
        m_spaceFromBody = -1;
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

    // --- Where the ship is, and which way it wants to face ---------------------------------------------------------------
    const double clock = m_campaign.clock;
    const glm::vec3 actual = Travel::ShipPosition(m_campaign, *system);
    glm::vec3 ship = actual;
    glm::vec3 wantForward = m_campaign.travel.velocity;
    glm::vec3 wantUp{0.0f, 1.0f, 0.0f};
    const Body* at = m_campaign.travel.underway ? nullptr : system->Find(m_campaign.body);
    if (at != nullptr)
    {
        const glm::vec3 centre = system->Position(at->index, clock);
        if (m_spaceOrbitBody != at->index)
        {
            // Into orbit where the ship came in; with nothing to go by, on the day side a little round from noon.
            glm::vec3 out = m_spaceShipSet && glm::length(m_spaceShip - centre) > 1.0e-12f ? glm::normalize(m_spaceShip - centre) : glm::vec3(0.0f);
            if (glm::length(out) < 0.5f)
            {
                const glm::vec3 sunward = glm::length(centre) > 1.0e-9f ? -glm::normalize(centre) : glm::vec3(1.0f, 0.0f, 0.0f);
                out = glm::normalize(sunward + glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), sunward) + glm::vec3(1.0e-6f)) * 0.8f);
            }
            glm::vec3 along = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), out);
            along = glm::length(along) > 1.0e-4f ? glm::normalize(along) : glm::vec3(0.0f, 0.0f, 1.0f);
            m_spaceOrbitOut = out;
            m_spaceOrbitAlong = glm::normalize(along - out * glm::dot(along, out));
            m_spaceOrbitBody = at->index;
            m_spaceOrbitSince = clock;
        }
        const float angle = static_cast<float>(std::fmod((clock - m_spaceOrbitSince) / kOrbitSeconds, 1.0)) * kTau;
        const glm::vec3 out = m_spaceOrbitOut * std::cos(angle) + m_spaceOrbitAlong * std::sin(angle);
        const glm::vec3 along = -m_spaceOrbitOut * std::sin(angle) + m_spaceOrbitAlong * std::cos(angle);
        ship = centre + out * OrbitRadius(*at);
        wantForward = along * std::cos(kOrbitPitch) - out * std::sin(kOrbitPitch);
        wantUp = out * std::cos(kOrbitPitch) + along * std::sin(kOrbitPitch);
        // What it leaves from, when it goes: here, where it is in the orbit now.
        m_spaceFromBody = at->index;
        m_spaceFromOut = out;
        m_spaceLeftAt = clock;
    }
    else
    {
        m_spaceOrbitBody = -1;
        // Out of the orbit of what it left: from where it was in it, falling away from it at the pace it seems to (kFallAway)
        // until that has caught up with where the ship really is.
        if (const Body* from = system->Find(m_spaceFromBody))
        {
            const glm::vec3 centre = system->Position(from->index, clock);
            const float reach = OrbitRadius(*from);
            const float gone = glm::length(actual - centre);
            const float since = static_cast<float>(std::max(clock - m_spaceLeftAt, 0.0));
            const float seen = std::min(gone, reach * (std::exp(std::min(since / kFallAway, 30.0f)) - 1.0f));
            const float away = glm::smoothstep(0.0f, reach * 40.0f, seen);
            glm::vec3 way = gone > 1.0e-12f ? (actual - centre) / gone : m_spaceFromOut;
            way = glm::mix(m_spaceFromOut, way, away);
            way = glm::length(way) > 1.0e-6f ? glm::normalize(way) : m_spaceFromOut;
            ship = centre + way * (seen + reach * (1.0f - away));
            if (seen >= gone && away >= 1.0f)
            {
                m_spaceFromBody = -1;
            }
        }
        // Into the orbit of what it is coming to: drawn in over the last of the way, so it reaches the orbit's height
        // exactly as it arrives.
        if (const Body* to = system->Find(m_campaign.travel.target))
        {
            const glm::vec3 centre = system->Position(to->index, clock);
            const float reach = OrbitRadius(*to);
            const float distance = glm::length(ship - centre);
            const float arrival = Travel::ArrivalDistance(*to);
            if (distance > 1.0e-12f)
            {
                const float drawn = distance > arrival ? distance - (arrival - reach) * std::exp(-(distance - arrival) / (arrival * 4.0f))
                                                       : reach * distance / arrival;
                ship = centre + (ship - centre) / distance * drawn;
            }
            wantForward = centre - ship;
        }
    }
    m_spaceShip = ship;
    m_spaceShipSet = true;

    // Turned towards the heading wanted at a ship's pace -- at once only the first time there is one. Its rate of turn eases
    // up and eases off again so that it comes to the new heading without overshooting it.
    if (glm::length(wantForward) > 1.0e-12f)
    {
        const glm::quat want = Facing(wantForward, wantUp);
        if (!m_spaceAttitudeSet)
        {
            m_spaceAttitude = want;
            m_spaceAttitudeSet = true;
            m_spaceTurnSpeed = 0.0f;
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

    // --- The star ----------------------------------------------------------------------------------------------------
    const float fromStar = std::max(glm::length(ship), 0.05f);
    const glm::vec3 towardsStar = glm::normalize(toWorld(-ship));
    environment.sunDirection = -towardsStar;
    environment.sunColor = glm::mix(system->starColor, glm::vec3(1.0f), 0.3f);
    environment.sunIntensity = 1.7f * std::clamp(std::sqrt(system->luminosity) / fromStar, 0.35f, 2.2f);
    // As big as the star is from here: a sun's radius is 0.00465 astronomical units; a little larger, so it reads.
    environment.sunDisc = std::clamp(0.00465f * std::max(system->starRadius, 0.1f) / fromStar * 1.3f, 0.0012f, 0.06f);

    // --- The planets and moons ----------------------------------------------------------------------------------------
    // Those near enough to have a size, as bodies (DrawSpaceBodies), the nearest first; the rest as points, as many as the
    // sky draws, the brightest first.
    std::vector<std::pair<float, int>> points;
    float shade = 1.0f;
    for (const Body& body : system->bodies)
    {
        const glm::vec3 there = system->Position(body.index, clock);
        const glm::vec3 v = there - ship;
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
        const glm::vec3 there = system->Position(body.index, clock);
        const float distance = glm::length(there - ship);
        const glm::vec3 way = toWorld(there - ship);
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
    const glm::mat4 projection = Depth::Perspective(m_renderVerticalFov, m_renderAspect, 1.0f, 4.0e6f);
    bgfx::setViewTransform(Renderer::kViewSkyBodies, glm::value_ptr(view), glm::value_ptr(projection));
    m_planets.SetOutput(true, Depth::Test());
    m_planets.SetCamera(glm::vec3(0.0f), 1.0f);
    const float time = static_cast<float>(std::fmod(m_campaign.clock, 100000.0));
    const glm::mat4 turn(m_spaceToWorld);
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
        m_planets.Body(Renderer::kViewSkyBodies, model, look, towardsStar, system->starColor, 0.0f, time * 0.02f);
        m_planets.Rings(Renderer::kViewSkyBodies, rings, look, towardsStar, system->starColor);
    }
    // Back as the map wants it.
    m_planets.SetOutput(false, BGFX_STATE_DEPTH_TEST_LESS);
}

} // namespace pred
