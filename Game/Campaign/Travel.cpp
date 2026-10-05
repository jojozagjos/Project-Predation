#include "Game/Campaign/Travel.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace Travel
{

namespace
{

// How a body is moving, from where it is a moment either side.
glm::vec3 BodyVelocity(const StarSystem& system, int body, double clock)
{
    constexpr double kStep = 0.5;
    return (system.Position(body, clock + kStep) - system.Position(body, clock - kStep)) / static_cast<float>(kStep * 2.0);
}

// Where to steer for, to go from one place to another without passing through the star: the place itself, or, while
// the straight way passes too close, a point beside the star on the side the line passes.
glm::vec3 AimPoint(const glm::vec3& from, const glm::vec3& to, float clearance)
{
    const glm::vec3 way = to - from;
    const float length2 = glm::dot(way, way);
    if (length2 < 1.0e-12f)
    {
        return to;
    }
    const float t = std::clamp(glm::dot(-from, way) / length2, 0.0f, 1.0f);
    const glm::vec3 closest = from + way * t;
    const float distance = glm::length(closest);
    if (distance >= clearance || t <= 0.0f || t >= 1.0f)
    {
        return to;
    }
    // Out from the star through where the line comes closest; dead through the middle, off to one side of it.
    glm::vec3 out = distance > 1.0e-6f ? closest / distance : glm::cross(way, glm::vec3(0.0f, 1.0f, 0.0f));
    out = glm::length(out) > 1.0e-9f ? glm::normalize(out) : glm::vec3(1.0f, 0.0f, 0.0f);
    return out * (clearance * 1.3f);
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
    return 2.0f * std::sqrt(std::max(distance, 0.0f) / Acceleration(tier));
}

glm::vec3 ShipPosition(const CampaignState& campaign, const StarSystem& system)
{
    if (campaign.travel.underway || campaign.body < 0)
    {
        return campaign.travel.position;
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
        // Leaving a body: from where it is, moving as it moves.
        if (campaign.body >= 0)
        {
            campaign.travel.position = system.Position(campaign.body, campaign.clock);
            campaign.travel.velocity = BodyVelocity(system, campaign.body, campaign.clock);
        }
        campaign.travel.underway = true;
        campaign.body = -1;
        campaign.region = -1;
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
    const float most = accel * dt;
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
        travel.position += travel.velocity * dt;
        return false;
    }

    // Steer by how the ship is moving against the body: closing on it as fast as it can and still stop in time -- by way
    // of a point beside the star, if the straight way is through it.
    const glm::vec3 there = system.Position(travel.target, campaign.clock);
    const glm::vec3 moving = BodyVelocity(system, travel.target, campaign.clock);
    const float distance = glm::length(there - travel.position);
    const glm::vec3 aim = AimPoint(travel.position, there, StarClearance(system, travel.position, there));
    const glm::vec3 to = aim - travel.position;
    const float toAim = std::max(glm::length(to), 1.0e-9f);
    const float remaining = toAim + glm::length(there - aim);
    if (distance <= kArrival)
    {
        travel.position = there;
        travel.velocity = moving;
        travel.underway = false;
        campaign.body = travel.target;
        campaign.region = -1;
        travel.target = -1;
        return true;
    }
    const glm::vec3 relative = travel.velocity - moving;
    const float closing = std::sqrt(2.0f * accel * std::max(remaining - kArrival * 0.5f, 0.0f)) * 0.95f;
    const glm::vec3 wanted = to / toAim * closing;
    glm::vec3 change = wanted - relative;
    const float size = glm::length(change);
    if (size > most)
    {
        change *= most / size;
    }
    travel.velocity += change;
    travel.position += travel.velocity * dt;
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
    const float distance = glm::length(system.Position(campaign.travel.target, campaign.clock) - campaign.travel.position);
    const float dt = std::max(Seconds(distance * 1.5f, tier) / 300.0f, 0.05f);
    constexpr int kSteps = 600;
    const int every = std::max(kSteps / points, 1);
    path.push_back(ahead.travel.position);
    for (int i = 1; i <= kSteps; ++i)
    {
        ahead.clock += dt;
        const bool arrived = Step(ahead, system, dt, tier);
        if (arrived || i % every == 0)
        {
            path.push_back(ahead.travel.position);
        }
        if (arrived || !ahead.travel.underway)
        {
            break;
        }
    }
    return path;
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
    if (universe.System(toSystem) == nullptr || (!campaign.travel.interstellar && toSystem == campaign.system) ||
        (campaign.travel.interstellar && toSystem == campaign.travel.toSystem))
    {
        return false;
    }
    const glm::vec3 from = GalaxyPosition(campaign, universe);
    const glm::vec3 to = universe.SystemPosition(SystemId::Unpack(toSystem));
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
    travel.position = back * (outermost * 1.25f);
    travel.velocity = glm::vec3(0.0f);
    travel.interstellar = false;
    travel.underway = false;
    travel.target = -1;
    travel.region = -1;
    return true;
}

} // namespace Travel

} // namespace pred
