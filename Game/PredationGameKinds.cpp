// Two kinds of mission on survey ground (Phase 6, new kinds). A beacon: its three parts -- mast, dish, power cell -- in the crate in the
// shuttle, each carried up (both hands) to the site's highest ground and fitted in turn, then its dish aimed up close with its knobs to
// the bearing on its plate and switched on. Samples: the cold case from the shuttle's bench carried to each of three outcrops, a core
// drilled at each straight into it -- loud -- and back aboard before it warms: away from the shuttle it keeps them cold five minutes,
// and warmed through they spoil and must be drilled again. The host works both and says how they stand with the mission.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Game/PredationGameShared.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

glm::vec3 PredationGame::HighGround() const
{
    // Out among the site's open ground -- not on or by a building, nor by the pad, nor in water -- the highest of a few dozen places,
    // the same on every machine (from the site's seed, and its land).
    const SitePlan& plan = m_facility.Plan();
    const glm::vec2 middle = glm::vec2(plan.origin.x, plan.origin.z) + glm::vec2(plan.size * 0.5f);
    const glm::vec2 pad{plan.landing.x, plan.landing.z};
    glm::vec3 best = m_missionPlan.markers.empty() ? glm::vec3(middle.x, plan.origin.y, middle.y) : m_missionPlan.markers.front();
    float highest = -1.0e9f;
    uint32_t h = m_facility.Seed() * 2654435761u + 0x8EAC0u;
    for (int k = 0; k < 64; ++k)
    {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        const float angle = static_cast<float>(h & 0xFFFFu) / 65535.0f * glm::two_pi<float>();
        const float out = (0.1f + 0.35f * static_cast<float>((h >> 16) & 0xFFFFu) / 65535.0f) * plan.size;
        const glm::vec2 at = middle + glm::vec2(std::cos(angle), std::sin(angle)) * out;
        if (plan.InBuilding(at, 6.0f) || glm::distance(at, pad) < 30.0f || m_facility.Terrain().WaterDepth(at.x, at.y) > 0.0f)
        {
            continue;
        }
        const float y = m_facility.Terrain().Height(at.x, at.y);
        if (y > highest)
        {
            highest = y;
            best = glm::vec3(at.x, y, at.y);
        }
    }
    return best;
}

void PredationGame::PutBeaconParts()
{
    // In the crate on the cabin's bench: the mast, the dish and the cell, lying in it.
    if (m_missionPlan.kind != MissionPlan::Kind::Beacon)
    {
        return;
    }
    const std::vector<glm::vec3>& slots = m_missionProps.CrateSlots();
    for (size_t k = 0; k < m_beaconItems.size() && k < slots.size(); ++k)
    {
        if (m_beaconItems[k] == kInvalidItem)
        {
            continue;
        }
        const int index = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, m_beaconItems[k], 1,
                                              slots[k] + glm::vec3(0.0f, 0.1f, 0.0f), glm::vec3(0.0f), -1, -1);
        if (WorldObjects::Pickup* placed = index >= 0 ? m_world.GetPickup(index) : nullptr; placed != nullptr && placed->body.IsValid())
        {
            Transform lying;
            lying.position = slots[k] + glm::vec3(0.0f, 0.1f, 0.0f);
            lying.rotation = m_missionProps.CrateTurn();
            m_app->GetPhysics().SetTransform(placed->body, lying);
        }
        if (m_sessionMode == SessionMode::Host && index >= 0)
        {
            m_host.Broadcast(PickupSpawnedEvent(static_cast<uint8_t>(index), static_cast<uint16_t>(m_beaconItems[k]), 1, LoadToWire(-1), LoadToWire(-1),
                                                slots[k] + glm::vec3(0.0f, 0.1f, 0.0f), glm::vec3(0.0f)));
        }
    }
    const glm::vec3 at = m_missionPlan.markers.empty() ? glm::vec3(0.0f) : m_missionPlan.markers.front();
    PRED_LOG_INFO(Gameplay, "Mission: a beacon to set up at {:.1f} {:.1f} {:.1f}, aimed {} deg and {} up; its parts in the crate in the shuttle", at.x, at.y, at.z,
                  m_missionPlan.aimBearing * 360 / MissionSpec::kAimSteps, m_missionPlan.aimTilt * 10);
}

bool PredationGame::PerformBeaconInteraction(int control, uint8_t player)
{
    const int next = MissionRules::NextBeaconPart(m_mission);
    const ItemId part = next < MissionSpec::kBeaconParts ? m_beaconItems[static_cast<size_t>(next)] : kInvalidItem;
    const bool carrying = control == MissionRules::kBeaconFit && part != kInvalidItem && Carries(player, part);
    const MissionRules::Worked worked = MissionRules::WorkBeacon(m_mission, m_missionPlan, control, carrying);
    if (worked == MissionRules::Worked::Nothing || m_missionPlan.markers.empty())
    {
        return false;
    }
    const glm::vec3 at = m_missionPlan.markers.front() + glm::vec3(0.0f, 1.4f, 0.0f);
    if (control == MissionRules::kBeaconFit)
    {
        SpendItem(player, part);
        ShareSound("World/crate_close", at, 0.8f);
        MakeNoise(NoiseKind::Item, at, 18.0f, player);
        PRED_LOG_INFO(Gameplay, "Player {} fitted the beacon's {}", player, next == 0 ? "mast" : next == 1 ? "dish" : "power cell");
    }
    else
    {
        ShareSound("World/switch", at, control == MissionRules::kBeaconSwitch ? 0.7f : 0.4f);
    }
    if (worked == MissionRules::Worked::Done)
    {
        ShareSound("World/terminal_done", at, 0.9f);
        PRED_LOG_INFO(Gameplay, "The beacon is on and aimed");
    }
    BroadcastMission();
    return true;
}

bool PredationGame::PerformSampleInteraction(int site, uint8_t player)
{
    const bool carrying = Carries(player, m_coldItem);
    const MissionRules::Worked worked = MissionRules::DrillSample(m_mission, m_missionPlan, site, carrying);
    if (worked == MissionRules::Worked::Nothing)
    {
        return false;
    }
    const glm::vec3 at = m_missionPlan.markers[static_cast<size_t>(site)] + glm::vec3(0.0f, 0.8f, 0.0f);
    // The drill biting into the rock: heard a long way.
    ShareSound("World/door_bash", at, 0.6f);
    MakeNoise(NoiseKind::Door, at, 28.0f, player);
    if (worked == MissionRules::Worked::Done)
    {
        ShareSound("World/pickup", at, 0.7f);
        PRED_LOG_INFO(Gameplay, "Player {} drilled a core at outcrop {} ({} of {})", player, site, std::popcount(static_cast<unsigned>(m_mission.cored)),
                      m_missionPlan.markers.size());
    }
    BroadcastMission();
    return true;
}

void PredationGame::UpdateSamples(float dt)
{
    // The cold case: chilled aboard the shuttle (four times as fast), warming anywhere else -- and warmed through, its cores spoil.
    if (m_missionPlan.kind != MissionPlan::Kind::Samples || !IsAuthority() ||
        (m_mission.stage != MissionState::Stage::Find && m_mission.stage != MissionState::Stage::Carry))
    {
        return;
    }
    uint8_t aboard = 0;
    if (m_player.State().alive && m_facility.Shuttle().Aboard(m_player.State().position))
    {
        aboard = static_cast<uint8_t>(aboard | (1u << LocalPlayerId()));
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.alive && m_facility.Shuttle().Aboard(remote.position))
        {
            aboard = static_cast<uint8_t>(aboard | (1u << remote.id));
        }
    }
    const bool chilled = CountAboard(m_coldItem, aboard) > 0;
    if (chilled)
    {
        m_mission.cold = std::min(m_mission.cold + dt * 4.0f, MissionSpec::kColdSeconds);
        return;
    }
    if (m_mission.cored == 0)
    {
        return;
    }
    m_mission.cold = std::max(m_mission.cold - dt, 0.0f);
    if (m_mission.cold <= 0.0f)
    {
        MissionRules::SpoilSamples(m_mission);
        ShareSound("World/terminal_dead", m_facility.Shuttle().Home().position, 0.6f);
        PRED_LOG_INFO(Gameplay, "The cold case warmed through: the cores are spoiled");
        BroadcastMission();
    }
}

} // namespace pred
