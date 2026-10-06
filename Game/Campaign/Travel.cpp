#include "Game/Campaign/Travel.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace Travel
{

namespace
{

constexpr float kTau = 6.28318530718f;
constexpr float kEarthRadiusAu = 4.26e-5f;
// Setting out: the least time to turn and swing round, and how much more for half way round the world; how much higher than
// its orbit it is when it starts to burn.
constexpr float kAlignLeast = 8.0f;
constexpr float kAlignMore = 14.0f;
constexpr float kClimb = 0.25f;
// Within this many orbits' heights of the world it leaves it goes straight out the way it set out, gathering way: nothing it
// steers by can turn it back into the world.
constexpr float kDepartReach = 25.0f;
// Into orbit within this much of the orbit's height (a share of it, and at least this much, which is what a float can tell at
// an astronomical unit from the star).
constexpr float kArriveShare = 0.05f;
constexpr float kArriveLeast = 2.0e-7f;

// How a body is moving, from where it is a moment either side -- in double, or a float's steps far out are most of it.
glm::vec3 BodyVelocity(const StarSystem& system, int body, double clock)
{
    constexpr double kStep = 0.5;
    return glm::vec3((system.PositionD(body, clock + kStep) - system.PositionD(body, clock - kStep)) / (kStep * 2.0));
}

// How hard a body is being turned in its orbit: a moon going round its planet fast can pull away from a ship pushing gently.
float BodyPull(const StarSystem& system, int body, double clock)
{
    constexpr double kStep = 0.5;
    const glm::dvec3 before = system.PositionD(body, clock - kStep);
    const glm::dvec3 now = system.PositionD(body, clock);
    const glm::dvec3 after = system.PositionD(body, clock + kStep);
    return static_cast<float>(glm::length((after - now * 2.0 + before) / (kStep * kStep)));
}

glm::vec3 Unit(const glm::vec3& v, const glm::vec3& otherwise)
{
    const float length = glm::length(v);
    return length > 1.0e-12f ? v / length : otherwise;
}

// Something to keep clear of: the star, or a world, and how far from its middle.
struct Obstacle
{
    glm::vec3 centre{0.0f};
    float clearance = 0.0f;
};

// How far a course keeps from a world it is passing: a little inside the height a ship orbits at, so one leaving its
// orbit is already clear of it.
float WorldClearance(const Body& body)
{
    return OrbitRadius(body) * 0.9f;
}

// Where to steer for, to go from one place to another without passing through anything: the place itself, or, while the
// straight way passes too close to something, a point beside the first thing in the way, on the side the line passes. What
// the ship is already inside the clearance of (the world it is just leaving) is not in the way.
glm::vec3 AimPoint(const glm::vec3& from, const glm::vec3& to, const std::vector<Obstacle>& obstacles)
{
    const glm::vec3 way = to - from;
    const float length2 = glm::dot(way, way);
    if (length2 < 1.0e-24f)
    {
        return to;
    }
    float first = 2.0f;
    glm::vec3 aim = to;
    for (const Obstacle& obstacle : obstacles)
    {
        if (glm::length(from - obstacle.centre) <= obstacle.clearance || glm::length(to - obstacle.centre) <= obstacle.clearance)
        {
            continue;
        }
        const float t = std::clamp(glm::dot(obstacle.centre - from, way) / length2, 0.0f, 1.0f);
        const glm::vec3 closest = from + way * t;
        const float distance = glm::length(closest - obstacle.centre);
        if (distance >= obstacle.clearance || t <= 0.0f || t >= 1.0f || t >= first)
        {
            continue;
        }
        // Out from its middle through where the line comes closest; dead through the middle, off to one side of it.
        glm::vec3 out = distance > obstacle.clearance * 1.0e-4f ? (closest - obstacle.centre) / distance : glm::cross(way, glm::vec3(0.0f, 1.0f, 0.0f));
        out = glm::length(out) > 1.0e-12f ? glm::normalize(out) : glm::vec3(1.0f, 0.0f, 0.0f);
        first = t;
        aim = obstacle.centre + out * (obstacle.clearance * 1.3f);
    }
    return aim;
}

// Everything to keep clear of on the way from one place to another, where each is as the step begins: the star, and every world
// -- where it is going too, which a way to the near side of it never passes through, but a way round the star might.
std::vector<Obstacle> Obstacles(const StarSystem& system, double clock, const glm::dvec3& origin, const glm::vec3& from, const glm::vec3& to)
{
    std::vector<Obstacle> obstacles;
    obstacles.reserve(system.bodies.size() + 1);
    obstacles.push_back({glm::vec3(-origin), StarClearance(system, glm::vec3(origin) + from, glm::vec3(origin) + to)});
    for (const Body& body : system.bodies)
    {
        obstacles.push_back({glm::vec3(system.PositionD(body.index, clock) - origin), WorldClearance(body)});
    }
    return obstacles;
}

// Never inside a world: put back out on its surface (a little over it), moving no further into it than the world itself is.
void KeepOut(CampaignState::Travel& travel, const StarSystem& system, double clock)
{
    for (const Body& body : system.bodies)
    {
        const float least = std::max(body.radius, 0.02f) * kEarthRadiusAu * 1.1f;
        const glm::dvec3 centre = system.PositionD(body.index, clock);
        const glm::vec3 off = glm::vec3(travel.position - centre);
        const float distance = glm::length(off);
        if (distance >= least)
        {
            continue;
        }
        const glm::vec3 out = distance > least * 1.0e-4f ? off / distance : glm::vec3(0.0f, 1.0f, 0.0f);
        travel.position = centre + glm::dvec3(out * least);
        const glm::vec3 moving = BodyVelocity(system, body.index, clock);
        const float inward = glm::dot(travel.velocity - moving, out);
        if (inward < 0.0f)
        {
            travel.velocity -= out * inward;
        }
    }
}

} // namespace

float StarClearance(const StarSystem& system, const glm::vec3& from, const glm::vec3& to)
{
    // Further from a brighter star; never so far that a planet close in cannot be reached.
    const float wanted = std::clamp(0.14f * std::sqrt(std::max(system.luminosity, 0.01f)), 0.05f, 0.7f);
    return std::min(wanted, 0.75f * std::min(glm::length(from), glm::length(to)));
}

float Acceleration(int tier)
{
    // A first drive makes an astronomical unit in about a minute and a half; each tier pushes two and a half times as
    // hard, so the fifth crosses a whole system in under a minute.
    return 4.0e-4f * std::pow(2.5f, static_cast<float>(std::clamp(tier, 0, 12)));
}

float Seconds(float distance, int tier)
{
    // Out and back at full push, and the gentle going near each end: about four of the gentle pace's time constants out and
    // seven settling in, and the turn before it all.
    const float gentle = std::sqrt(Gentleness(tier));
    return 2.0f * std::sqrt(std::max(distance, 0.0f) / Acceleration(tier)) + 11.0f / gentle +
           (kAlignLeast + kAlignMore * 0.5f) / (1.0f + static_cast<float>(std::max(tier, 0)));
}

float Gentleness(int tier)
{
    // A first drive leaves a world over about twenty seconds and settles into orbit over half a minute; a better one sooner.
    return 0.04f * std::pow(2.5f, static_cast<float>(std::clamp(tier, 0, 12)));
}

float OrbitRadius(const Body& body)
{
    return std::max(body.radius, 0.02f) * kEarthRadiusAu * (body.gas ? 1.35f : 1.6f);
}

void Lift(CampaignState& campaign)
{
    if (!campaign.landed)
    {
        return;
    }
    campaign.landed = false;
    campaign.travel.orbitOut = glm::vec3(0.0f);
    campaign.travel.orbitSince = campaign.clock;
}

void OrbitFrame(const CampaignState& campaign, const StarSystem& system, int body, double clock, glm::vec3& out, glm::vec3& along)
{
    const CampaignState::Travel& travel = campaign.travel;
    glm::vec3 first = travel.orbitOut;
    if (glm::length(first) < 0.5f)
    {
        // Nowhere set: over the day side, a little round from noon, as it was when it came into orbit.
        const glm::vec3 centre = system.Position(body, travel.orbitSince);
        const glm::vec3 sunward = Unit(-centre, glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 side = Unit(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), sunward), glm::vec3(0.0f, 0.0f, 1.0f));
        first = sunward + side * 0.8f;
    }
    first = Unit(first, glm::vec3(1.0f, 0.0f, 0.0f));
    glm::vec3 way = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), first);
    way = glm::length(way) > 1.0e-4f ? way : glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), first);
    way = Unit(way - first * glm::dot(way, first), glm::vec3(0.0f, 0.0f, 1.0f));
    const double turns = (clock - travel.orbitSince) / kOrbitSeconds;
    const float angle = static_cast<float>((turns - std::floor(turns)) * static_cast<double>(kTau));
    out = first * std::cos(angle) + way * std::sin(angle);
    along = -first * std::sin(angle) + way * std::cos(angle);
}

float AlignSeconds(const CampaignState& campaign, const StarSystem& system)
{
    const CampaignState::Travel& travel = campaign.travel;
    if (travel.from < 0 || system.Find(travel.from) == nullptr)
    {
        return 0.0f;
    }
    glm::vec3 out;
    glm::vec3 along;
    OrbitFrame(campaign, system, travel.from, travel.setOut, out, along);
    const float angle = std::acos(std::clamp(glm::dot(out, Unit(travel.departWay, out)), -1.0f, 1.0f));
    // A better drive turns it sooner, as it does everything.
    return (kAlignLeast + kAlignMore * angle / 3.14159265f) / (1.0f + static_cast<float>(std::max(campaign.Upgrade("travel"), 0)));
}

float AlignDone(const CampaignState& campaign, const StarSystem& system)
{
    const CampaignState::Travel& travel = campaign.travel;
    const float seconds = AlignSeconds(campaign, system);
    if (!travel.underway || seconds <= 0.0f)
    {
        return 1.0f;
    }
    return std::clamp(static_cast<float>(campaign.clock - travel.setOut) / seconds, 0.0f, 1.0f);
}

glm::vec3 AlignOffset(const CampaignState& campaign, const StarSystem& system, double clock)
{
    const CampaignState::Travel& travel = campaign.travel;
    const Body* from = system.Find(travel.from);
    if (from == nullptr)
    {
        return glm::vec3(0.0f);
    }
    // From where it was in its orbit round to the side facing the way it is going, eased in and out, and a little higher.
    glm::vec3 out;
    glm::vec3 along;
    OrbitFrame(campaign, system, travel.from, travel.setOut, out, along);
    const glm::vec3 way = Unit(travel.departWay, out);
    const float seconds = std::max(AlignSeconds(campaign, system), 1.0e-3f);
    const float s = std::clamp(static_cast<float>(clock - travel.setOut) / seconds, 0.0f, 1.0f);
    const float eased = s * s * (3.0f - 2.0f * s);
    const float angle = std::acos(std::clamp(glm::dot(out, way), -1.0f, 1.0f));
    glm::vec3 axis = glm::cross(out, way);
    // Dead behind: round the orbit's own way.
    axis = glm::length(axis) > 1.0e-5f ? glm::normalize(axis) : Unit(glm::cross(out, along), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 direction = glm::angleAxis(angle * eased, axis) * out;
    return direction * (OrbitRadius(*from) * (1.0f + kClimb * eased));
}

float RelativeSpeed(const CampaignState& campaign, const StarSystem& system)
{
    const CampaignState::Travel& travel = campaign.travel;
    if (!travel.underway || travel.interstellar)
    {
        return 0.0f;
    }
    if (AlignDone(campaign, system) < 1.0f)
    {
        return 0.0f;
    }
    // Against the nearer of what it left and where it is going.
    float best = 1.0e9f;
    float speed = glm::length(travel.velocity);
    for (const int body : {travel.from, travel.target})
    {
        if (system.Find(body) == nullptr)
        {
            continue;
        }
        const float distance = static_cast<float>(glm::length(system.PositionD(body, campaign.clock) - travel.position));
        if (distance < best)
        {
            best = distance;
            speed = glm::length(travel.velocity - BodyVelocity(system, body, campaign.clock));
        }
    }
    return speed;
}

bool Burning(const CampaignState& campaign, const StarSystem& system)
{
    return campaign.travel.underway && !campaign.travel.interstellar && AlignDone(campaign, system) >= 1.0f;
}

glm::vec3 ShipPosition(const CampaignState& campaign, const StarSystem& system)
{
    if (campaign.travel.underway || campaign.body < 0)
    {
        return glm::vec3(campaign.travel.position);
    }
    return system.Position(campaign.body, campaign.clock);
}

bool SetCourse(CampaignState& campaign, const StarSystem& system, int body)
{
    if ((body >= 0 && system.Find(body) == nullptr) || campaign.travel.interstellar)
    {
        return false;
    }
    if (!campaign.travel.underway)
    {
        if (body < 0 || body == campaign.body)
        {
            return false;
        }
        Lift(campaign);
        // Leaving a body: from where it is in its orbit, moving as the body moves -- first turning and swinging round to the
        // side facing where it is going (AlignSeconds).
        if (campaign.body >= 0)
        {
            const glm::vec3 centre = system.Position(campaign.body, campaign.clock);
            glm::vec3 out;
            glm::vec3 along;
            OrbitFrame(campaign, system, campaign.body, campaign.clock, out, along);
            campaign.travel.from = campaign.body;
            campaign.travel.setOut = campaign.clock;
            campaign.travel.departWay = Unit(system.Position(body, campaign.clock) - centre, out);
            campaign.travel.position = system.PositionD(campaign.body, campaign.clock) + glm::dvec3(out * OrbitRadius(*system.Find(campaign.body)));
            campaign.travel.velocity = BodyVelocity(system, campaign.body, campaign.clock);
        }
        campaign.travel.underway = true;
        campaign.body = -1;
        campaign.region = -1;
        campaign.landed = false;
        campaign.doorOpen = false;
    }
    campaign.travel.target = body;
    return true;
}

bool Step(CampaignState& campaign, const StarSystem& system, float dt, int tier)
{
    CampaignState::Travel& travel = campaign.travel;
    if (!travel.underway || dt <= 0.0f)
    {
        return false;
    }
    const float accel = Acceleration(tier);
    const float gentle = Gentleness(tier);
    // Never so gently it cannot keep up with where it is going: at least twice what turns that in its orbit.
    const float keepUp = system.Find(travel.target) != nullptr ? 2.0f * BodyPull(system, travel.target, campaign.clock) : 0.0f;
    float limit = accel;
    // Setting out: turning and swinging round to the side it leaves from, carried along by the world it is leaving.
    if (const Body* from = system.Find(travel.from))
    {
        const glm::dvec3 centre = system.PositionD(from->index, campaign.clock);
        if (AlignDone(campaign, system) < 1.0f)
        {
            travel.position = centre + glm::dvec3(AlignOffset(campaign, system, campaign.clock));
            travel.velocity = BodyVelocity(system, from->index, campaign.clock);
            return false;
        }
        const glm::dvec3 began = system.PositionD(from->index, campaign.clock - static_cast<double>(dt));
        // Then out, gently near it and harder the further it gets -- straight out the way it set out until it is well clear
        // (or half way to where it is going, a moon close by): only on the way out, the time that takes, so a ship turned back
        // towards it by a new course is not sent out again.
        const float distance = static_cast<float>(glm::length(travel.position - began));
        const float reach = OrbitRadius(*from);
        limit = std::min(limit, std::max(gentle * std::max(distance, reach), keepUp));
        float clear = reach * kDepartReach;
        if (system.Find(travel.target) != nullptr)
        {
            clear = std::min(clear, 0.5f * glm::length(system.Position(travel.target, travel.setOut) - system.Position(from->index, travel.setOut)));
        }
        const double outFor = AlignSeconds(campaign, system) + 4.0 / std::sqrt(static_cast<double>(gentle));
        if (distance < clear && travel.target >= 0 && campaign.clock < travel.setOut + outFor)
        {
            // Out the way it set out -- round anything in that way (a moon's planet, between it and another of its moons).
            const glm::vec3 way = Unit(travel.departWay, glm::vec3(0.0f, 0.0f, -1.0f));
            const glm::vec3 ahead = way * (clear - distance + reach);
            const glm::vec3 aim = AimPoint(glm::vec3(0.0f), ahead, Obstacles(system, campaign.clock, travel.position, glm::vec3(0.0f), ahead));
            travel.velocity += Unit(aim, way) * limit * dt;
            travel.position += glm::dvec3(travel.velocity) * static_cast<double>(dt);
            KeepOut(travel, system, campaign.clock);
            return false;
        }
        if (gentle * distance >= accel)
        {
            travel.from = -1;
        }
    }
    const float most = limit * dt;
    if (travel.target < 0)
    {
        // Nowhere to go: slow to a stop where it is.
        const float speed = glm::length(travel.velocity);
        if (speed <= most)
        {
            travel.velocity = glm::vec3(0.0f);
            travel.underway = false;
            return true;
        }
        travel.velocity -= travel.velocity / speed * most;
        travel.position += glm::dvec3(travel.velocity) * static_cast<double>(dt);
        return false;
    }

    // Steer by how the ship is moving against the body: for the near side of it at its orbit's height, closing as fast as it
    // can and still stop in time, and settling into the orbit over the last of the way -- by way of a point beside the star,
    // if the straight way is through it.
    const Body* target = system.Find(travel.target);
    // Where it is as the step begins -- the ship is still where it was then, the clock already moved on -- and how it moves
    // over the step, at its middle: a moon going round fast is a long way on by the end of a step, and steered for from there the
    // ship never quite closes the last of the way.
    const double begun = campaign.clock - static_cast<double>(dt);
    const glm::dvec3 there = system.PositionD(travel.target, begun);
    const glm::vec3 moving = BodyVelocity(system, travel.target, begun + static_cast<double>(dt) * 0.5);
    const glm::vec3 fromThere = glm::vec3(travel.position - there);
    const float distance = std::max(glm::length(fromThere), 1.0e-12f);
    const float height = OrbitRadius(*target);
    if (distance <= height + std::max(height * kArriveShare, kArriveLeast))
    {
        // Into orbit where it came in.
        travel.orbitOut = fromThere / distance;
        travel.orbitSince = campaign.clock;
        travel.position = system.PositionD(travel.target, campaign.clock);
        travel.velocity = moving;
        travel.underway = false;
        travel.from = -1;
        campaign.body = travel.target;
        campaign.region = -1;
        travel.target = -1;
        return true;
    }
    // Measured from the ship, so the last of the way is exact; round the star or a world only when the straight way is
    // through it.
    const glm::vec3 toShell = -fromThere * (1.0f - height / distance);
    const glm::vec3 aim = AimPoint(glm::vec3(0.0f), toShell, Obstacles(system, begun, travel.position, glm::vec3(0.0f), toShell));
    const glm::vec3 shell = toShell;
    const glm::vec3 to = aim;
    const float toAim = std::max(glm::length(to), 1.0e-12f);
    const float remaining = toAim + (aim == shell ? 0.0f : glm::length(shell - aim));
    const float push = std::min(most, std::max(gentle * std::max(distance, height), keepUp) * dt);
    const glm::vec3 relative = travel.velocity - moving;
    // As fast as it can close and still stop in time with the push it will have on the way: full push far out, and near the world
    // (within accel / gentle) only the gentle push, less the nearer it is -- so it slows on a curve that never asks for more
    // braking than there will be, rather than coming in too fast to stop and sliding past or into the world.
    const float gentleFrom = accel / gentle;
    const float closing = 0.9f * (remaining <= gentleFrom ? std::sqrt(gentle) * remaining
                                                          : std::sqrt(gentle * gentleFrom * gentleFrom + 2.0f * accel * (remaining - gentleFrom)));
    const glm::vec3 wanted = to / toAim * closing;
    glm::vec3 change = wanted - relative;
    const float size = glm::length(change);
    if (size > push)
    {
        change *= push / size;
    }
    travel.velocity += change;
    // Moved as it moves against where it is going, carried along with it: a world going round its star covers many times its
    // own size a second, and a ship that only matched its pace by steering would slide into it in the last of the way.
    travel.position = system.PositionD(travel.target, campaign.clock) + glm::dvec3(fromThere) + glm::dvec3(travel.velocity - moving) * static_cast<double>(dt);
    KeepOut(travel, system, campaign.clock);
    return false;
}

std::vector<glm::vec3> Preview(const CampaignState& campaign, const StarSystem& system, int tier, int points)
{
    std::vector<glm::vec3> path;
    if (!campaign.travel.underway || campaign.travel.interstellar || campaign.travel.target < 0 || points < 2)
    {
        return path;
    }
    // Flown ahead on a copy, in steps sized to the trip, a point kept every so often.
    CampaignState ahead;
    ahead.clock = campaign.clock;
    ahead.body = campaign.body;
    ahead.travel = campaign.travel;
    const float distance = glm::length(system.Position(campaign.travel.target, campaign.clock) - glm::vec3(campaign.travel.position));
    const float dt = std::max(Seconds(distance * 1.5f, tier) / 300.0f, 0.05f);
    ahead.upgrades = campaign.upgrades;
    constexpr int kSteps = 600;
    const int every = std::max(kSteps / points, 1);
    path.push_back(glm::vec3(ahead.travel.position));
    for (int i = 1; i <= kSteps; ++i)
    {
        ahead.clock += dt;
        const bool arrived = Step(ahead, system, dt, tier);
        if (arrived || i % every == 0)
        {
            path.push_back(glm::vec3(ahead.travel.position));
        }
        if (arrived || !ahead.travel.underway)
        {
            break;
        }
    }
    return path;
}

float CrossingRange(int tier)
{
    return tier < kCrossingTier ? 0.0f : 14.0f + 10.0f * static_cast<float>(tier - kCrossingTier);
}

float ChartRange(int sensorTier)
{
    return 22.0f + 14.0f * static_cast<float>(std::clamp(sensorTier, 0, 8));
}

float InterstellarSeconds(float lightYears, int tier)
{
    return 75.0f + 30.0f * std::max(lightYears, 0.0f) / (1.0f + 0.8f * static_cast<float>(std::clamp(tier, 0, 12)));
}

glm::vec3 GalaxyPosition(const CampaignState& campaign, Universe& universe)
{
    if (!campaign.travel.interstellar)
    {
        return universe.SystemPosition(SystemId::Unpack(campaign.system));
    }
    const float t = CrossingDone(campaign);
    // Easing in and out of it, as the drive takes up and gives up.
    const float eased = t * t * (3.0f - 2.0f * t);
    return glm::mix(campaign.travel.fromGalaxy, campaign.travel.toGalaxy, eased);
}

float CrossingDone(const CampaignState& campaign)
{
    if (!campaign.travel.interstellar || campaign.travel.duration <= 0.0f)
    {
        return campaign.travel.interstellar ? 1.0f : 0.0f;
    }
    return static_cast<float>(std::clamp((campaign.clock - campaign.travel.departed) / static_cast<double>(campaign.travel.duration), 0.0, 1.0));
}

bool SetSystemCourse(CampaignState& campaign, Universe& universe, uint64_t toSystem, int tier)
{
    if (tier < kCrossingTier || universe.System(toSystem) == nullptr || (!campaign.travel.interstellar && toSystem == campaign.system) ||
        (campaign.travel.interstellar && toSystem == campaign.travel.toSystem))
    {
        return false;
    }
    const glm::vec3 from = GalaxyPosition(campaign, universe);
    const glm::vec3 to = universe.SystemPosition(SystemId::Unpack(toSystem));
    if (glm::length(to - from) > CrossingRange(tier))
    {
        return false;
    }
    campaign.travel.interstellar = true;
    campaign.travel.underway = true;
    campaign.travel.toSystem = toSystem;
    campaign.travel.fromGalaxy = from;
    campaign.travel.toGalaxy = to;
    campaign.travel.departed = campaign.clock;
    campaign.travel.duration = InterstellarSeconds(glm::length(to - from), tier);
    campaign.travel.target = -1;
    campaign.travel.region = -1;
    campaign.travel.velocity = glm::vec3(0.0f);
    campaign.body = -1;
    campaign.region = -1;
    campaign.landed = false;
    campaign.doorOpen = false;
    return true;
}

bool StepInterstellar(CampaignState& campaign, Universe& universe)
{
    CampaignState::Travel& travel = campaign.travel;
    if (!travel.interstellar || CrossingDone(campaign) < 1.0f)
    {
        return false;
    }
    const StarSystem* arrived = universe.System(travel.toSystem);
    if (arrived == nullptr)
    {
        travel.interstellar = false;
        travel.underway = false;
        return true;
    }
    // At the edge of it, on the side it came in from, well outside the furthest planet, at rest.
    float outermost = 1.0f;
    for (const Body& body : arrived->bodies)
    {
        if (body.kind == BodyKind::Planet)
        {
            outermost = std::max(outermost, body.orbit);
        }
    }
    glm::vec3 back = travel.fromGalaxy - travel.toGalaxy;
    back.y = 0.0f;
    back = glm::length(back) > 1.0e-4f ? glm::normalize(back) : glm::vec3(1.0f, 0.0f, 0.0f);
    campaign.system = travel.toSystem;
    campaign.body = -1;
    campaign.region = -1;
    travel.position = glm::dvec3(back * (outermost * 1.25f));
    travel.velocity = glm::vec3(0.0f);
    travel.interstellar = false;
    travel.underway = false;
    travel.target = -1;
    travel.region = -1;
    return true;
}

} // namespace Travel

} // namespace pred
