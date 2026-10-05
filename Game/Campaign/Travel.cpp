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

} // namespace

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
    if (body >= 0 && system.Find(body) == nullptr)
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

    // Steer by how the ship is moving against the body: closing on it as fast as it can and still stop in time.
    const glm::vec3 there = system.Position(travel.target, campaign.clock);
    const glm::vec3 moving = BodyVelocity(system, travel.target, campaign.clock);
    const glm::vec3 to = there - travel.position;
    const float distance = glm::length(to);
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
    const float closing = std::sqrt(2.0f * accel * std::max(distance - kArrival * 0.5f, 0.0f)) * 0.95f;
    const glm::vec3 wanted = to / distance * closing;
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

} // namespace Travel

} // namespace pred
