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
// way and losing it again at this many radians a second, a second. Half way round takes about twenty seconds.
constexpr float kTurnRate = 0.18f;
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

// The way between two directions, along the great circle from one to the other (s from 0 to 1).
glm::vec3 Between(const glm::vec3& a, const glm::vec3& b, float s)
{
    const float cosine = std::clamp(glm::dot(a, b), -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    if (angle < 1.0e-4f)
    {
        return b;
    }
    glm::vec3 axis = glm::cross(a, b);
    axis = glm::length(axis) > 1.0e-6f ? glm::normalize(axis) : glm::normalize(glm::cross(a, std::abs(a.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)));
    return glm::angleAxis(angle * s, axis) * a;
}

// From one attitude to another as a ship turns: its nose straight along the way from one heading to the other, and its top
// rolling round as it goes -- not a single twist about some skewed axis, which swings the nose round in a corkscrew.
glm::quat Turning(const glm::quat& from, const glm::quat& to, float s)
{
    const glm::vec3 forward = Between(from * glm::vec3(0.0f, 0.0f, -1.0f), to * glm::vec3(0.0f, 0.0f, -1.0f), s);
    const glm::vec3 up = Between(from * glm::vec3(0.0f, 1.0f, 0.0f), to * glm::vec3(0.0f, 1.0f, 0.0f), s);
    return Facing(forward, up);
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
        m_spaceArrived = m_spaceCrossing;
    }

    // Between the stars: nothing near, the star it left a dimming sun astern and the one it is heading for a brightening
    // point dead ahead -- the nearer of the two the one that lights the ship. (Once out of the system it left: leaving it, it is
    // still among its worlds, below.)
    const Body* leaving = m_campaign.travel.interstellar && Travel::LeavingDone(m_campaign) < 1.0f ? system->Find(m_campaign.travel.from) : nullptr;
    m_spaceCrossing = m_campaign.travel.interstellar && leaving == nullptr;
    if (m_campaign.travel.interstellar && leaving == nullptr)
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
        environment.sunColor = glm::mix(nearerAhead ? ahead.starColor : behindColour, glm::vec3(1.0f), 0.15f);
        environment.sunIntensity = glm::mix(0.25f, 1.1f, near * near);
        // The other star, as a point.
        const glm::vec3 other = nearerAhead ? astern : bow;
        const glm::vec3 otherColour = nearerAhead ? behindColour : ahead.starColor;
        environment.skyBodies[0] = glm::vec4(glm::normalize(other) * 2.0f, 1.0e-6f);
        environment.skyBodies[1] = glm::vec4(glm::mix(otherColour, glm::vec3(1.0f), 0.4f) * 3.0f, 0.0f);
        // (The stars as they lay setting out: it holds its heading across.)
        environment.starTurn = m_spaceStarTurn;
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
    // Under way its top is to the system's north: the worlds, all going round in nearly the one plane, along the horizon in
    // the windows, and a new heading a turn to one side or the other rather than a loop.
    glm::vec3 wantUp{0.0f, 1.0f, 0.0f};
    bool held = false;
    const Body* at = travel.underway ? nullptr : system->Find(m_campaign.body);
    if (leaving != nullptr)
    {
        // Leaving across the stars (Travel::LeavingOffset): out from the orbit it was in, turning to face the way it is going
        // at a ship's pace and gathering speed, the world it left falling away behind.
        glm::vec3 out;
        glm::vec3 along;
        Travel::OrbitFrame(m_campaign, *system, leaving->index, travel.departed, out, along);
        reference = leaving->index;
        offset = out * Travel::OrbitRadius(*leaving) + Travel::LeavingOffset(m_campaign);
        glm::vec3 way = travel.toGalaxy - travel.fromGalaxy;
        way.y = 0.0f;
        wantForward = glm::length(way) > 1.0e-6f ? way : along;
    }
    else if (at != nullptr)
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
            // Not while a cinematic films it (a developer's): the stage holds still, and the sky would turn round it. Its cuts hide the turn.
            m_spaceSettleFrom = m_spaceAttitudeSet && clock - travel.orbitSince < kSettleSeconds && !CinematicFilmed() ? m_spaceAttitude
                                                                                                                      : Facing(wantForward, wantUp);
            m_spaceArriveReach = m_spaceApproachTo == at->index && clock - travel.orbitSince < kSettleSeconds ? m_spaceApproachReach : 0.0f;
        }
        // And down from how far out it came in to the orbit's height, over the same few seconds.
        if (m_spaceArriveReach > 0.0f && clock - travel.orbitSince < kSettleSeconds && clock >= travel.orbitSince && !CinematicFilmed())
        {
            const float s = static_cast<float>((clock - travel.orbitSince) / kSettleSeconds);
            offset = out * glm::mix(m_spaceArriveReach, Travel::OrbitRadius(*at), s * s * (3.0f - 2.0f * s));
        }
    }
    else if (const Body* from = travel.underway ? system->Find(travel.from) : nullptr; from != nullptr && Travel::AlignDone(m_campaign, *system) < 1.0f)
    {
        // Setting out: its nose lifting off the world below, over and up until it points out the way it is going, while it
        // swings round to that side of the world and climbs a little -- the world sliding down out of the windows and round
        // behind, never the nose swinging down through it (the short way round from the orbit's facing often is). Then, clear
        // of it, rolling level with the system for the way there.
        glm::vec3 out;
        glm::vec3 along;
        Travel::OrbitFrame(m_campaign, *system, from->index, travel.setOut, out, along);
        reference = from->index;
        offset = Travel::AlignOffset(m_campaign, *system, clock);
        const glm::quat start = Facing(along * std::cos(kOrbitPitch) - out * std::sin(kOrbitPitch), out * std::cos(kOrbitPitch) + along * std::sin(kOrbitPitch));
        const float s = Travel::AlignDone(m_campaign, *system);
        const float eased = s * s * (3.0f - 2.0f * s);
        // In orbit its right is the orbit's axis -- the system's north or south -- and the swing round the world is about the
        // same axis: so its nose comes up about its own right by as far as from the orbit's facing to straight out from the
        // world, and round with the swing by as far as that goes (AlignOffset, the same way and the same easing).
        const glm::vec3 right = start * glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 way = glm::length(travel.departWay) > 1.0e-6f ? glm::normalize(travel.departWay) : out;
        const float swing = std::acos(std::clamp(glm::dot(out, way), -1.0f, 1.0f));
        glm::vec3 swingAxis = glm::cross(out, way);
        swingAxis = glm::length(swingAxis) > 1.0e-5f ? glm::normalize(swingAxis) : glm::normalize(glm::cross(out, along));
        const float pitch = (kOrbitPitch + 1.5707963f) * eased + swing * eased * glm::dot(swingAxis, right);
        glm::quat turned = glm::angleAxis(pitch, right) * start;
        // Rolling level: its top round to the system's north, from about a third of the way.
        const glm::vec3 nose = turned * glm::vec3(0.0f, 0.0f, -1.0f);
        const float roll = (glm::dot(turned * glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)) >= 0.0f ? 1.5707963f : -1.5707963f) *
                           glm::smoothstep(0.3f, 1.0f, s);
        turned = glm::angleAxis(roll, nose) * turned;
        wantForward = nose;
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
            m_spaceApproachTo = to->index;
            m_spaceApproachReach = static_cast<float>(glm::length(travel.position - system->PositionD(to->index, clock)));
            // Nose on where it is going, all the way in: it comes in to the near side of it, and settles from there into the
            // orbit's facing. (Not its way through space, which near a world is mostly the world's own way round its star.)
            wantForward = glm::vec3(system->PositionD(to->index, clock) - travel.position);
        }
        else
        {
            wantForward = travel.velocity;
        }
    }
    glm::vec3 ship = glm::vec3(reference >= 0 ? system->PositionD(reference, clock) + glm::dvec3(offset) : travel.position);

    // Held exactly to what it wants in orbit and setting out (settling into orbit from how it came in); under way turned towards
    // it at a ship's pace -- gathering way and losing it again, so it comes to the new heading without overshooting.
    // (Come in from between the stars: facing its star first, as it was coming in, then turned from there.)
    if (m_spaceArrived && glm::length(ship) > 1.0e-6f)
    {
        m_spaceAttitude = Facing(-ship, glm::vec3(0.0f, 1.0f, 0.0f));
        m_spaceAttitudeSet = true;
        m_spaceTurnSpeed = 0.0f;
    }
    m_spaceArrived = false;
    if (glm::length(wantForward) > 1.0e-12f)
    {
        const glm::quat want = Facing(wantForward, wantUp);
        // Held to it -- unless that is a sudden swing of more than a few degrees (going round to be over somewhere else, the
        // orbit's way turning with it): then turned to at a ship's pace, as under way, never all at once.
        const float jump = m_spaceAttitudeSet ? 2.0f * std::acos(std::min(std::abs(glm::dot(m_spaceAttitude, want)), 1.0f)) : 0.0f;
        const bool settling = at != nullptr && clock - travel.orbitSince < kSettleSeconds && clock >= travel.orbitSince;
        if ((held && (jump < 0.05f || settling || CinematicFilmed())) || !m_spaceAttitudeSet)
        {
            m_spaceAttitude = want;
            m_spaceTurnSpeed = 0.0f;
            if (at != nullptr && clock - travel.orbitSince < kSettleSeconds && clock >= travel.orbitSince && !CinematicFilmed())
            {
                const float s = static_cast<float>((clock - travel.orbitSince) / kSettleSeconds);
                m_spaceAttitude = Turning(m_spaceSettleFrom, want, s * s * (3.0f - 2.0f * s));
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
    // Filmed going down into the world out of its orbit (ship_entry): falling towards it -- straight down at it from where it is, by
    // the same share of the way down each moment, the world growing until it is all there is below and its horizon flattening -- to
    // some seventy kilometres over it, where the air is burning round the ship and the cutscene goes to the field.
    if (at != nullptr && reference == at->index && m_cine.Active() && m_cine.Playing().name == "ship_entry")
    {
        const float radiusAu = RadiusAu(*at);
        const float s = glm::smoothstep(0.03f, 1.0f, m_cine.Time() / std::max(m_cine.Playing().duration, 0.1f));
        const float fromHigh = std::max(glm::length(offset) - radiusAu, radiusAu * 1.0e-4f);
        const float toHigh = radiusAu * (70000.0f / (std::max(at->radius, 0.02f) * 6.371e6f));
        const float high = std::exp(glm::mix(std::log(fromHigh), std::log(std::min(toHigh, fromHigh)), s));
        offset = glm::normalize(offset) * (radiusAu + high);
        ship = glm::vec3(system->PositionD(reference, clock) + glm::dvec3(offset));
    }
    // From the system's directions to the world's: the ship's own right, up and forward onto the rooms' starboard, overhead
    // and bow.
    m_spaceToWorld = glm::mat3(starboard, overhead, -bow) * glm::transpose(glm::mat3_cast(m_spaceAttitude));
    // Diving at the site out of the bay, riding the shuttle (ship_launch): from the ship's place in its orbit down to over the site,
    // the world turning under the windows as it goes -- so that at the moment the site's own sky takes over, the world, the stars
    // and the sun are exactly where they will be there, seen from where the shuttle will be (DiveFrame): the land the shuttle
    // comes down onto is the world it dived at, not another picture faded in over it. (Only from the seats: from the ship, or a
    // camera outside, the world is where it is.)
    if (const float dive = LaunchDive(); dive > 0.0f && at != nullptr && at->index == m_campaign.body && reference == at->index &&
                                         m_mySeat >= 0 && m_cameraMode == CameraMode::FirstPerson)
    {
        glm::mat3 bodyToSite;
        glm::vec3 centre;
        float radiusMetres = 0.0f;
        glm::quat leavingTurn;
        CinePose start;
        glm::vec3 pad;
        if (SiteGlobe(bodyToSite, centre, radiusMetres) && DiveFrame(leavingTurn, start, pad))
        {
            // The body's frame in the system's, as the world is turned now (StarSystem::FromOver).
            glm::mat3 fromBody;
            for (int i = 0; i < 3; ++i)
            {
                glm::vec3 axis{0.0f};
                axis[i] = 1.0f;
                fromBody[i] = system->FromOver(at->index, axis, clock);
            }
            const float radiusAu = RadiusAu(*at);
            const float auPerMetre = radiusAu / radiusMetres;
            // How the body is turned in the world as it is from the ship; and, coming in to the site on a heading (turned about the pad),
            // as it will be from the shuttle there -- the heading the one nearest the other, chosen as the dive starts.
            const glm::quat fromTurn = glm::quat_cast(m_spaceToWorld * fromBody);
            const glm::quat bodyTurn = glm::quat_cast(bodyToSite);
            const auto onHeading = [&](float heading)
            { return glm::normalize(leavingTurn * glm::inverse(glm::angleAxis(heading, glm::vec3(0.0f, 1.0f, 0.0f)) * start.rotation) * bodyTurn); };
            if (!m_diveApproachSet)
            {
                float nearest = 10.0f;
                for (int step = 0; step < 360; ++step)
                {
                    const float heading = glm::radians(static_cast<float>(step));
                    const float apart = 2.0f * std::acos(std::min(std::abs(glm::dot(fromTurn, onHeading(heading))), 1.0f));
                    if (apart < nearest)
                    {
                        nearest = apart;
                        m_diveApproach = heading;
                    }
                }
                m_diveApproachSet = true;
            }
            const glm::quat approach = glm::angleAxis(m_diveApproach, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 siteEye = pad + approach * (start.position - pad);
            // Where the eye is, in the body's frame: the ship in its orbit, to over the site as the shuttle comes into its sky.
            const glm::vec3 fromEye = glm::transpose(fromBody) * offset;
            const glm::vec3 toEye = glm::transpose(bodyToSite) * (siteEye - centre) * auPerMetre;
            const glm::quat toTurn = onHeading(m_diveApproach);
            // Turned first, while the world is still small and far below -- the shuttle coming round onto the heading it will come
            // down on, as a craft does in space -- and then only down: no swinging round sideways as it falls into the world.
            const float eased = dive * dive * (3.0f - 2.0f * dive);
            const float turning = glm::smoothstep(0.0f, 0.5f, dive);
            const glm::quat turn = glm::normalize(glm::slerp(fromTurn, toTurn, turning));
            // Round over the world to above the site, and down -- by the same share of the way down each moment (its height
            // eased between the two in proportion, not in metres), as a fall towards a world looks: slow far off, then rushing.
            const glm::vec3 fromWay = glm::normalize(fromEye);
            const glm::vec3 toWay = glm::normalize(toEye);
            const glm::vec3 way = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(fromWay, toWay), eased) * fromWay);
            const float fromHigh = std::max(glm::length(fromEye) - radiusAu, radiusAu * 1.0e-6f);
            const float toHigh = std::max(glm::length(toEye) - radiusAu, radiusAu * 1.0e-9f);
            const float high = std::exp(glm::mix(std::log(fromHigh), std::log(toHigh), dive));
            offset = fromBody * (way * (radiusAu + high));
            ship = glm::vec3(system->PositionD(reference, clock) + glm::dvec3(offset));
            m_spaceToWorld = glm::mat3_cast(turn) * glm::transpose(fromBody);
            // And the cabin lit from below by the world it is falling into, as it will be in its sky (SetSkyOver's ground light).
            Environment site = environment;
            SetSkyOver(*system, *at, m_campaign.travel.region, site);
            environment.ambientGround = glm::mix(environment.ambientGround, site.ambientGround, eased * eased);
        }
    }
    const auto toWorld = [&](const glm::vec3& v) { return m_spaceToWorld * v; };
    // And the stars with it: turned as the ship turns, so they wheel slowly past the windows going round an orbit (once in
    // its sixteen minutes) and swing across them setting out -- never painted on the glass.
    environment.starTurn = glm::transpose(m_spaceToWorld);
    m_spaceStarTurn = environment.starTurn;
    // Where a body is from the ship, measured from the reference body so the near one is exact.
    const glm::dvec3 origin = reference >= 0 ? system->PositionD(reference, clock) : travel.position;
    const auto fromShip = [&](int index) { return glm::vec3(system->PositionD(index, clock) - origin) - (reference >= 0 ? offset : glm::vec3(0.0f)); };


    // --- The star ----------------------------------------------------------------------------------------------------
    const float fromStar = std::max(glm::length(ship), 0.05f);
    const glm::vec3 towardsStar = glm::normalize(toWorld(-ship));
    environment.sunDirection = -towardsStar;
    environment.sunColor = glm::mix(system->starColor, glm::vec3(1.0f), 0.15f);
    // No brighter than a clear noon on the ground, however near the star: the eye (and a camera) sets itself by the sunlit
    // things in front of it, so a sunlit hull reads as lit, not as glowing.
    environment.sunIntensity = 1.7f * std::clamp(std::sqrt(system->luminosity) / fromStar, 0.35f, 1.3f);
    // As big as the star is from here: a sun's radius is 0.00465 astronomical units; a little larger, so it reads.
    environment.sunDisc = std::clamp(0.00465f * std::max(system->starRadius, 0.1f) / fromStar * 1.3f, 0.0012f, 0.06f);

    m_spaceSunLight = environment.sunColor * environment.sunIntensity;
    // And a companion star, where the system has one (StarSystem::companion): its disc where it is from the ship, as big and as bright
    // against the first as it is.
    environment.secondSun = glm::vec4(0.0f);
    environment.secondSunColor = glm::vec4(0.0f);
    if (system->HasCompanion())
    {
        const glm::vec3 apart = system->CompanionAt(clock) - ship;
        const float distance = std::max(glm::length(apart), 0.01f);
        const bool close = system->companionApart < 1.0f;
        const float first = std::max(system->luminosity - (close ? system->companionLuminosity : 0.0f), 0.001f);
        const float ratio = system->companionLuminosity / first * (fromStar * fromStar) / (distance * distance);
        environment.secondSun = glm::vec4(glm::normalize(toWorld(apart)), std::clamp(0.00465f * system->companionRadius / distance * 1.3f, 0.0009f, 0.06f));
        environment.secondSunColor = glm::vec4(glm::mix(system->companionColor, glm::vec3(1.0f), 0.15f), 0.55f * std::clamp(ratio, 0.012f, 1.0f));
    }

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
        // (The one below, as the shuttle dives at it out of the bay: growing until it is all there is below -- the eye taken down
        // to it, above.)
        const float angle = std::asin(std::min(RadiusAu(body) / distance, 0.99999994f));
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
    m_planets.SetCamera(glm::vec3(0.0f), 1.0f, static_cast<float>(renderer.Height()) / std::max(m_renderVerticalFov, 0.1f));
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
        // The site being dived at, the colour of its land from up there, coming into the windows (SiteFarColour).
        const bool diving = body->index == m_campaign.body && LaunchDive() > 0.0f && m_mySeat >= 0;
        if (diving && m_campaign.travel.region >= 0 && m_campaign.travel.region < static_cast<int>(body->regions.size()))
        {
            const bool air = body->air >= 0.015f;
            const float metres = std::max(body->radius, 0.02f) * 6.371e6f;
            m_planets.SetSite(AreaOnGlobe(body->regions[static_cast<size_t>(m_campaign.travel.region)]), SiteFarColour(), (air ? 60000.0f : 11500.0f) / metres,
                              (air ? 300000.0f : 40000.0f) / metres);
        }
        m_planets.Body(Renderer::kViewSkyBodies, model, look, towardsStar, light, time * 0.02f);
        m_planets.ClearSite();
        m_planets.Rings(Renderer::kViewSkyBodies, rings, look, towardsStar, light);
    }
    // Back as the map wants it.
    m_planets.SetOutput(false, BGFX_STATE_DEPTH_TEST_LESS);
}

bool PredationGame::SiteGlobe(glm::mat3& bodyToSite, glm::vec3& centre, float& radius)
{
    const StarSystem* system = m_campaignOpen ? CurrentSystem() : nullptr;
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    const int region = m_campaign.travel.region;
    if (body == nullptr || region < 0 || region >= static_cast<int>(body->regions.size()) || !m_facility.Built())
    {
        return false;
    }
    // As SetSkyOver has it: up over the place, east, and north ahead (the site's z is south).
    const glm::vec3 up = AreaOnGlobe(body->regions[static_cast<size_t>(region)]);
    glm::vec3 east = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up);
    east = glm::length(east) > 1.0e-4f ? glm::normalize(east) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 north = glm::cross(up, east);
    bodyToSite = glm::transpose(glm::mat3(east, up, -north));
    radius = std::max(body->radius, 0.02f) * 6.371e6f;
    // Its ground where the land made round the site ends, curving away from there: the land's edge on the world's ground, not stood up
    // over it or sunk under it.
    const SitePlan& site = m_facility.Plan();
    const float reach = m_facility.FarRimReach();
    const float top = m_facility.FarRimHeight() + reach * reach / (2.0f * radius);
    centre = glm::vec3(site.origin.x + site.size * 0.5f, top - radius, site.origin.z + site.size * 0.5f);
    return true;
}

glm::vec3 PredationGame::GlobeLight(const StarSystem& system, const Body& body) const
{
    // As SetSpaceSky lights the worlds out of the windows: the star's light where the world is, never brighter than a clear noon.
    const float fromStar = std::max(glm::length(system.Position(body.index, m_campaign.clock)), 0.05f);
    const glm::vec3 colour = glm::mix(system.starColor, glm::vec3(1.0f), 0.15f);
    const float intensity = 1.7f * std::clamp(std::sqrt(system.luminosity) / fromStar, 0.35f, 1.3f);
    return colour * intensity / (3.14159265f * 1.6f);
}

glm::vec3 PredationGame::SiteFarSeen()
{
    const StarSystem* system = m_campaignOpen ? CurrentSystem() : nullptr;
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    const int region = m_campaign.travel.region;
    if (body == nullptr || region < 0 || region >= static_cast<int>(body->regions.size()))
    {
        return glm::vec3(0.1f);
    }
    // The land's own sky worked out (SetSkyOver), and its ground in it far off: with air, the haze it fades into; without, its ground
    // in the light.
    Environment sky = m_scene.GetEnvironment();
    SetSkyOver(*system, *body, region, sky);
    if (body->air >= 0.015f)
    {
        return sky.fogColor;
    }
    const glm::vec3 world = glm::mix(body->groundA, body->groundB, 0.4f);
    const float sunUp = std::max(-sky.sunDirection.y, 0.0f);
    return world * (sky.sunColor * sky.sunIntensity * glm::mix(0.15f, 0.55f, sunUp) + sky.ambientSky);
}

glm::vec3 PredationGame::SiteFarColour()
{
    const StarSystem* system = m_campaignOpen ? CurrentSystem() : nullptr;
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    const int region = m_campaign.travel.region;
    if (body == nullptr || region < 0 || region >= static_cast<int>(body->regions.size()))
    {
        return glm::vec3(0.3f);
    }
    // What the globe's ground must be for the land to be seen as it is there, lit as PlanetRenderer lights it -- straight up into the
    // sun's light, and a little besides (fs_planet).
    const glm::vec3 up = AreaOnGlobe(body->regions[static_cast<size_t>(region)]);
    const float lit = glm::smoothstep(-0.1f, 0.3f, glm::dot(up, SunOverBody(*system, *body)));
    const glm::vec3 light = GlobeLight(*system, *body) * 1.6f;
    return glm::clamp(SiteFarSeen() / (light * lit + glm::vec3(0.05f)), glm::vec3(0.0f), glm::vec3(60.0f));
}

glm::quat PredationGame::DiveApproach(float time) const
{
    // On the heading it dived on until it is over the pad, hovering (surface_insertion's last few seconds before it sets down), then
    // turned onto the pad's own.
    const float on = 1.0f - glm::smoothstep(35.0f, 40.0f, time);
    return glm::angleAxis(m_diveApproach * on, glm::vec3(0.0f, 1.0f, 0.0f));
}

bool PredationGame::DiveFrame(glm::quat& leaving, CinePose& start, glm::vec3& pad)
{
    if (!m_cine.Active() || m_cine.Playing().name != "ship_launch")
    {
        m_diveKnown = false;
        return false;
    }
    if (!m_diveKnown)
    {
        const auto found = m_cinematics.find("surface_insertion");
        if (found == m_cinematics.end() || !m_facility.Built())
        {
            return false;
        }
        // The shuttle as surface_insertion starts it in the site's sky (before it is turned onto the heading it comes in on), and as
        // ship_launch has it at the moment the site is gone to (go_to_site): the one turned onto the other is the site's frame in the
        // ship's, for the windows.
        m_diveStart = CinematicSampler(found->second, CinematicBindingsNow()).Actor("shuttle", 0.0f);
        m_diveApproach = 0.0f;
        m_diveApproachSet = false;
        float switchAt = m_cine.Playing().duration;
        for (const Marker& marker : m_cine.Playing().markers)
        {
            if (marker.name == "go_to_site")
            {
                switchAt = marker.time;
            }
        }
        m_diveLeaving = m_cine.Sampler().Actor("shuttle", switchAt).rotation;
        m_divePad = m_facility.Plan().ShuttleBase();
        m_diveKnown = true;
    }
    leaving = m_diveLeaving;
    start = m_diveStart;
    pad = m_divePad;
    return true;
}

void PredationGame::DrawSiteGlobe()
{
    const StarSystem* system = m_campaignOpen ? CurrentSystem() : nullptr;
    const Body* body = system != nullptr ? system->Find(m_campaign.body) : nullptr;
    glm::mat3 bodyToSite;
    glm::vec3 centre;
    float radius = 0.0f;
    if (m_map != MapChoice::Facility || body == nullptr || body->gas || !m_planets.IsValid() || m_screen == Screen::Title ||
        !SiteGlobe(bodyToSite, centre, radius))
    {
        return;
    }
    // As the worlds out of the windows are drawn (DrawSpaceBodies), as big as it is from the eye's height: a thousand units off, so
    // its depth is beyond all of the land's, which hides it where it is -- round the land, out to its horizon, it shows.
    const glm::vec3 apart = centre - m_renderEye;
    const float distance = std::max(glm::length(apart), radius * 1.0000001f);
    const glm::vec3 direction = apart / glm::length(apart);
    const float angle = std::asin(std::min(radius / distance, 0.99999994f));
    constexpr float kDrawnAt = 1000.0f;
    const Renderer& renderer = m_app->GetRenderer();
    glm::mat4 view = renderer.ViewMatrix();
    view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::mat4 projection = Depth::Perspective(m_renderVerticalFov, m_renderAspect, 1.0e-3f, 4.0e6f);
    bgfx::setViewTransform(Renderer::kViewSkyBodies, glm::value_ptr(view), glm::value_ptr(projection));
    m_planets.SetOutput(true, Depth::Test());
    m_planets.SetCamera(glm::vec3(0.0f), 1.0f, static_cast<float>(renderer.Height()) / std::max(m_renderVerticalFov, 0.1f));
    const glm::mat4 model = glm::scale(glm::translate(glm::mat4(1.0f), direction * kDrawnAt) * glm::mat4(bodyToSite), glm::vec3(kDrawnAt * std::sin(angle)));
    const Environment& environment = m_scene.GetEnvironment();
    const glm::vec3 towardsStar = glm::normalize(bodyToSite * SunOverBody(*system, *body));
    const glm::vec3 light = GlobeLight(*system, *body);
    // Round the site, the colour its land is far off just now (the fog it fades into): the land and the globe one picture.
    const bool air = body->air >= 0.015f;
    const glm::vec3 up = AreaOnGlobe(body->regions[static_cast<size_t>(m_campaign.travel.region)]);
    const float lit = glm::smoothstep(-0.1f, 0.3f, glm::dot(up, SunOverBody(*system, *body)));
    const glm::vec3 albedo = glm::clamp(environment.fogColor / (light * 1.6f * lit + glm::vec3(0.05f)), glm::vec3(0.0f), glm::vec3(60.0f));
    m_planets.SetSite(up, albedo, (air ? 60000.0f : 11500.0f) / radius, (air ? 300000.0f : 40000.0f) / radius);
    const float time = static_cast<float>(std::fmod(m_campaign.clock, 100000.0));
    // Behind all the land, wherever it is: the globe is drawn only where there is none (its shell would otherwise lie over the land
    // seen from above it, until the shuttle came down through it).
    m_planets.SetBehind(true, Depth::Reversed() ? 1.0e-9f : 1.0f - 1.0e-7f);
    m_planets.Body(Renderer::kViewSkyBodies, model, LookOf(*body), towardsStar, light, time * 0.02f);
    m_planets.SetBehind(false, 0.0f);
    m_planets.ClearSite();
    m_planets.SetOutput(false, BGFX_STATE_DEPTH_TEST_LESS);
}

} // namespace pred
