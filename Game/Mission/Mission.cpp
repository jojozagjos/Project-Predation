#include "Game/Mission/Mission.h"

#include "Game/World/FacilityMap.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

// SplitMix64, as the site is planned with: the same sequence on every machine.
class Random
{
public:
    explicit Random(uint64_t seed) : m_state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    float Range(float low, float high) { return low + (high - low) * Unit(); }
    bool Chance(float p) { return Unit() < p; }
    // One of `count` things, each as likely.
    size_t Index(size_t count) { return std::min(static_cast<size_t>(Unit() * static_cast<float>(count)), count - 1); }

private:
    uint64_t m_state;
};

glm::vec3 Facing(float yaw)
{
    return {-std::sin(yaw), 0.0f, -std::cos(yaw)};
}

} // namespace

bool BreakerPanel(const FacilityLayout& building, glm::vec3& at, float& yaw)
{
    for (const FacilityLayout::Placed& thing : building.things)
    {
        if (thing.thing != FacilityLayout::Thing::Breaker)
        {
            continue;
        }
        // Flat on the wall: its back a couple of millimetres off the wall's face rather than the couple of centimetres
        // something standing on the floor is given.
        at = FacilityMap::PlacedAgainstWall(building, thing, MissionSpec::kBreakerSize.z) +
             glm::vec3(0.0f, MissionSpec::kBreakerHeight, 0.0f) - Facing(thing.yaw) * 0.018f;
        yaw = thing.yaw;
        return true;
    }
    return false;
}

MissionPlan MissionPlan::Generate(const SitePlan& site, uint32_t seed)
{
    MissionPlan plan;
    plan.seed = seed;
    Random random((static_cast<uint64_t>(seed) << 20) ^ 0xDA7A5EEDull);

    // The terminal stands on a bench, in a room that can be reached without the keycard and is not the one a creature
    // would build in -- in any of the site's buildings.
    struct Candidate
    {
        int building = 0;
        size_t thing = 0;
    };
    std::vector<Candidate> candidates;
    for (size_t b = 0; b < site.buildings.size(); ++b)
    {
        const FacilityLayout& building = site.buildings[b];
        const std::vector<bool> reach = building.Reachable(false);
        for (size_t t = 0; t < building.things.size(); ++t)
        {
            const FacilityLayout::Placed& thing = building.things[t];
            if (thing.thing != FacilityLayout::Thing::Bench)
            {
                continue;
            }
            const glm::ivec2 cell{static_cast<int>(std::floor(thing.at.x)), static_cast<int>(std::floor(thing.at.y))};
            const int room = building.RoomAt(thing.floor, cell.x, cell.y);
            if (room < 0 || room == building.nestRoom || !reach[building.Index(thing.floor, cell.x, cell.y)])
            {
                continue;
            }
            candidates.push_back({static_cast<int>(b), t});
        }
    }
    if (candidates.empty())
    {
        return plan;
    }
    // Any building as likely as any other -- the big one has the most benches, and picking among benches put the data in
    // it nearly every time -- and then any bench in it.
    std::vector<int> withBenches;
    for (const Candidate& candidate : candidates)
    {
        if (std::find(withBenches.begin(), withBenches.end(), candidate.building) == withBenches.end())
        {
            withBenches.push_back(candidate.building);
        }
    }
    const int pickedBuilding = withBenches[random.Index(withBenches.size())];
    std::vector<const Candidate*> inIt;
    for (const Candidate& candidate : candidates)
    {
        if (candidate.building == pickedBuilding)
        {
            inIt.push_back(&candidate);
        }
    }
    const Candidate& chosen = *inIt[random.Index(inIt.size())];

    const FacilityLayout& building = site.buildings[static_cast<size_t>(chosen.building)];
    const FacilityLayout::Placed& bench = building.things[chosen.thing];
    const glm::vec3 facing = Facing(bench.yaw);
    const glm::vec3 size = MissionSpec::kTerminalSize;
    plan.building = chosen.building;
    plan.floor = bench.floor;
    plan.room = building.RoomAt(bench.floor, static_cast<int>(std::floor(bench.at.x)), static_cast<int>(std::floor(bench.at.y)));
    // At the back of the bench, its screen towards the room; the drive comes out in front of it.
    const glm::vec3 benchFoot = FacilityMap::PlacedAgainstWall(building, bench, bench.size.y);
    plan.terminal = benchFoot + glm::vec3(0.0f, bench.height + size.y * 0.5f, 0.0f) - facing * (bench.size.y * 0.5f - size.z * 0.5f - 0.06f);
    plan.terminalYaw = bench.yaw;
    plan.driveAt = plan.terminal + facing * (size.z * 0.5f + 0.14f) - glm::vec3(0.0f, size.y * 0.5f - 0.06f, 0.0f);

    glm::vec3 panel;
    float panelYaw = 0.0f;
    plan.powerOut = random.Chance(0.4f) && BreakerPanel(building, panel, panelYaw);
    for (const FacilityLayout::Lamp& lamp : building.lamps)
    {
        if (std::find(plan.circuits.begin(), plan.circuits.end(), lamp.circuit) == plan.circuits.end())
        {
            plan.circuits.push_back(lamp.circuit);
        }
    }
    plan.mapGiven = !random.Chance(0.3f);
    plan.downloadSeconds = random.Range(35.0f, 60.0f);
    return plan;
}

namespace MissionRules
{

MissionState Start(const MissionPlan& plan)
{
    MissionState state;
    state.stage = plan.Valid() ? MissionState::Stage::Find : MissionState::Stage::None;
    state.powered = !plan.powerOut;
    return state;
}

bool CanDownload(const MissionState& state)
{
    return state.stage == MissionState::Stage::Find && state.powered;
}

bool CanRestorePower(const MissionState& state)
{
    return !state.powered && state.stage != MissionState::Stage::None && state.stage != MissionState::Stage::Over;
}

bool StartDownload(MissionState& state)
{
    if (!CanDownload(state))
    {
        return false;
    }
    state.stage = MissionState::Stage::Downloading;
    state.progress = 0.0f;
    return true;
}

bool RestorePower(MissionState& state)
{
    if (!CanRestorePower(state))
    {
        return false;
    }
    state.powered = true;
    return true;
}

bool ToggleLaunch(MissionState& state)
{
    if (state.stage == MissionState::Stage::None || state.stage == MissionState::Stage::Over)
    {
        return false;
    }
    state.launchIn = state.Launching() ? -1.0f : MissionSpec::kLaunchSeconds;
    return true;
}

Ticked Tick(MissionState& state, const MissionPlan& plan, float dt, bool attended)
{
    Ticked ticked;
    state.attended = attended && state.stage == MissionState::Stage::Downloading;
    if (state.stage == MissionState::Stage::Downloading && state.attended && state.powered)
    {
        state.progress += dt / std::max(plan.downloadSeconds, 1.0f);
        if (state.progress >= 1.0f)
        {
            state.progress = 1.0f;
            state.stage = MissionState::Stage::Carry;
            state.attended = false;
            ticked.downloaded = true;
        }
    }
    if (state.Launching() && state.stage != MissionState::Stage::Over)
    {
        state.launchIn = std::max(state.launchIn - dt, 0.0f);
        ticked.launched = state.launchIn <= 0.0f;
    }
    return ticked;
}

void Finish(MissionState& state, bool driveAboard, uint8_t aboard)
{
    state.stage = MissionState::Stage::Over;
    state.recovered = driveAboard;
    state.aboard = aboard;
    state.launchIn = -1.0f;
    state.attended = false;
}

} // namespace MissionRules

} // namespace pred
