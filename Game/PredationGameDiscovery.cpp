// What the crew has found (Q1 of the gameplay review, FIX 742): the mission's things -- its terminal, its breaker board, a fuse box,
// a feeder cabinet, a plant's or a station's chain and its fuel, a code note, a donor rack, the optional find -- are known to the
// objective tracker only once somebody has seen them: looked at from near enough, with nothing between. The host watches everybody
// and says what has been seen (MissionState::seen), so a thing found by one is known to all. Before that the tracker knows only what
// the briefing's map gave: which building it is in, by the name on its sign -- found the rest of the way by looking.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Game/PredationGameShared.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

// How near, and how squarely looked at, a thing has to be to be seen; and how near it is seen whichever way somebody faces.
constexpr float kSeenFrom = 16.0f;
constexpr float kSeenAhead = 0.3f;
constexpr float kTouching = 2.5f;

} // namespace

bool PredationGame::ThingAt(int bit, glm::vec3& at) const
{
    const MissionPlan& plan = m_missionPlan;
    const bool facility = plan.building >= 0 && !plan.AtMarkers();
    switch (1u << bit)
    {
    case MissionState::kSeenTerminal: at = plan.terminal; return facility;
    case MissionState::kSeenBoard:
    {
        float yaw = 0.0f;
        return plan.powerOut && MissionBreakerAt(at, yaw);
    }
    case MissionState::kSeenBox: at = plan.fuseBox; return plan.Faulted();
    case MissionState::kSeenCabinet: at = plan.cabinet; return plan.feedCut;
    case MissionState::kSeenFiller: at = plan.filler; return plan.chain;
    case MissionState::kSeenPump: at = plan.pump; return plan.chain;
    case MissionState::kSeenValves: at = plan.valves; return plan.chain;
    case MissionState::kSeenFuel: at = plan.fuelCan; return plan.chain;
    case MissionState::kSeenNote: at = plan.clue; return plan.code != 0;
    case MissionState::kSeenDonor: at = plan.donor; return plan.data == MissionPlan::Data::Damaged;
    case MissionState::kSeenExtra: at = plan.extra; return plan.HasExtra();
    default: return false;
    }
}

int PredationGame::BuildingOf(const glm::vec3& at) const
{
    const SitePlan& site = m_facility.Plan();
    for (size_t b = 0; b < site.buildings.size(); ++b)
    {
        glm::vec2 lo;
        glm::vec2 hi;
        SitePlan::Footprint(site.buildings[b], 0.0f, lo, hi);
        if (at.x > lo.x && at.x < hi.x && at.z > lo.y && at.z < hi.y)
        {
            return static_cast<int>(b);
        }
    }
    return -1;
}

void PredationGame::UpdateSeen(float dt)
{
    if (!IsAuthority() || !AtSite() || !m_missionPlan.Valid() || m_mission.stage == MissionState::Stage::None || m_mission.stage == MissionState::Stage::Over)
    {
        return;
    }
    m_discoveryIn -= dt;
    if (m_discoveryIn > 0.0f)
    {
        return;
    }
    m_discoveryIn = 0.25f;
    // Everybody's eyes, and the way each is looking.
    struct Eye
    {
        glm::vec3 at;
        glm::vec3 look;
    };
    std::vector<Eye> eyes;
    const auto lookOf = [](float yaw, float pitch) { return glm::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)); };
    if (m_player.State().alive)
    {
        eyes.push_back({m_player.View().eyePosition, lookOf(m_lookYaw, m_lookPitch)});
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : m_host.Remotes())
        {
            if (remote.alive && remote.id != LocalPlayerId())
            {
                eyes.push_back({remote.position + glm::vec3(0.0f, 1.6f, 0.0f), lookOf(remote.yaw, remote.pitch)});
            }
        }
    }
    const uint16_t before = m_mission.seen;
    // (The terminal found dead without a map is seen too.)
    if (m_mission.found)
    {
        m_mission.seen = static_cast<uint16_t>(m_mission.seen | MissionState::kSeenTerminal);
    }
    const PhysicsWorld& physics = m_app->GetPhysics();
    for (int bit = 0; bit < MissionState::kSeenCount; ++bit)
    {
        glm::vec3 thing;
        if ((m_mission.seen & (1u << bit)) != 0u || !ThingAt(bit, thing))
        {
            continue;
        }
        for (const Eye& eye : eyes)
        {
            const glm::vec3 to = thing - eye.at;
            const float away = glm::length(to);
            if (away > kSeenFrom)
            {
                continue;
            }
            const glm::vec3 way = to / std::max(away, 1e-3f);
            if (away > kTouching && glm::dot(way, eye.look) < kSeenAhead)
            {
                continue;
            }
            // Nothing between: the first thing the line meets is the thing itself, or near enough it (a wall it is on).
            const RayHit hit = physics.RayCastStatic(eye.at, way, away);
            if (hit && hit.distance < away - 0.8f)
            {
                continue;
            }
            m_mission.seen = static_cast<uint16_t>(m_mission.seen | (1u << bit));
            break;
        }
    }
    if (m_mission.seen != before)
    {
        PRED_LOG_INFO(Gameplay, "The crew has seen: {:#05x}", m_mission.seen);
        BroadcastMission();
    }
}

bool PredationGame::ObjectiveTargetFrom(const glm::vec3& here, glm::vec3& at, std::string* label) const
{
    if (!ObjectiveTargetRaw(here, at))
    {
        return false;
    }
    if (!m_facility.Built() || !m_facility.Contains(here) || !m_missionPlan.Valid())
    {
        return true;
    }
    // One of the mission's things not yet seen: only the building it is in, by its sign, where the briefing gave a map -- and nothing
    // more once inside it: it is to be searched for.
    for (int bit = 0; bit < MissionState::kSeenCount; ++bit)
    {
        glm::vec3 thing;
        if (!ThingAt(bit, thing) || glm::distance(thing, at) > 0.3f)
        {
            continue;
        }
        if ((m_mission.seen & (1u << bit)) != 0u)
        {
            return true;
        }
        const int b = BuildingOf(thing);
        if (!m_missionPlan.mapGiven || b < 0)
        {
            if (label != nullptr)
            {
                *label = "NO FIX";
            }
            return false;
        }
        const FacilityLayout& building = m_facility.Plan().buildings[static_cast<size_t>(b)];
        if (label != nullptr)
        {
            *label = building.outside.label;
        }
        if (BuildingOf(here) == b)
        {
            if (label != nullptr)
            {
                *label = "IN " + building.outside.label;
            }
            return false;
        }
        // Its nearest way in.
        float best = 1.0e9f;
        for (const FacilityLayout::Exit& exit : building.exits)
        {
            const glm::vec3 door = FacilityMap::ExitOutside(building, exit, 1.5f);
            const float d = glm::distance(door, here);
            if (d < best)
            {
                best = d;
                at = door;
            }
        }
        return best < 1.0e9f;
    }
    return true;
}

} // namespace pred
