#include "Game/Mission/Mission.h"
#include "Game/Mission/SiteSystems.h"

#include "Game/World/FacilityMap.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
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

    // A survey site's mission is a survey (Phase 4): readings at markers set out across the site -- three to five of them, out on the
    // open ground among the buildings and round them, well apart, none in a building nor by the pad -- and back to the shuttle with them.
    if (SitePlan::KindOf(site.seed) == SitePlan::SiteKind::Survey)
    {
        MissionPlan survey;
        survey.seed = seed;
        survey.kind = Kind::Survey;
        Random markers((static_cast<uint64_t>(seed) << 20) ^ 0x5E7A11ull);
        const int wanted = 3 + static_cast<int>(markers.Index(3));
        const glm::vec2 middle = glm::vec2(site.origin.x, site.origin.z) + glm::vec2(site.size * 0.5f);
        const glm::vec2 pad{site.landing.x, site.landing.z};
        for (int attempt = 0; attempt < 200 && static_cast<int>(survey.markers.size()) < wanted; ++attempt)
        {
            const float angle = markers.Range(0.0f, glm::two_pi<float>());
            const float out = markers.Range(0.15f, 0.42f) * site.size;
            const glm::vec2 at = middle + glm::vec2(std::cos(angle), std::sin(angle)) * out;
            bool fits = !site.InBuilding(at, 5.0f) && glm::distance(at, pad) > 22.0f;
            for (const glm::vec3& other : survey.markers)
            {
                fits = fits && glm::distance(at, glm::vec2(other.x, other.z)) > site.size * 0.18f;
            }
            if (fits)
            {
                survey.markers.emplace_back(at.x, site.origin.y, at.y);
            }
        }
        if (survey.markers.size() >= 3)
        {
            survey.mapGiven = true;
            return survey;
        }
    }
    // The terminal stands on a bench, in a room that can be reached without the keycard and is not the one a creature
    // would build in -- in any of the site's buildings (SiteSystems: its open terminals).
    struct Candidate
    {
        int building = 0;
        size_t thing = 0;
    };
    std::vector<Candidate> candidates;
    const SiteSystems systems = SiteSystems::Survey(site);
    for (const SiteSystems::Terminal* terminal : systems.OpenTerminals())
    {
        candidates.push_back({terminal->spot.building, terminal->fitting});
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
    // The plain bench a room that runs things has, nothing on it (FacilityLayout::Fitting::Terminal).
    const FacilityLayout::Fitted& fitted = building.fittings[chosen.thing];
    FacilityLayout::Placed bench;
    bench.floor = fitted.floor;
    bench.at = fitted.at;
    bench.yaw = fitted.yaw;
    const glm::vec3 benchSize = FacilityLayout::FittingSize(fitted.kind);
    const glm::vec3 facing = Facing(bench.yaw);
    const glm::vec3 size = MissionSpec::kTerminalSize;
    plan.building = chosen.building;
    plan.floor = bench.floor;
    plan.room = building.RoomAt(bench.floor, static_cast<int>(std::floor(bench.at.x)), static_cast<int>(std::floor(bench.at.y)));
    // At the back of the bench, its screen towards the room; the drive comes out in front of it.
    const glm::vec3 benchFoot = FacilityMap::PlacedAgainstWall(building, bench, benchSize.y);
    plan.terminal = benchFoot + glm::vec3(0.0f, benchSize.z + size.y * 0.5f, 0.0f) - facing * (benchSize.y * 0.5f - size.z * 0.5f - 0.06f);
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
    // Now and then, where the site has a cave, down there instead: a survey station on the floor of its deepest chamber the water
    // has not reached (on a draw of its own, so nothing else about the mission changes for it), on the mine's power.
    if (!site.caves.empty() && Random((static_cast<uint64_t>(seed) << 20) ^ 0xCA7E5EEDull).Chance(0.35f))
    {
        const SitePlan::Cave& cave = site.caves.front();
        const glm::vec4* deepest = nullptr;
        for (const glm::vec4& chamber : cave.chambers)
        {
            const float floor = chamber.y - chamber.w * 0.45f;
            if (floor > cave.water + 0.3f && (deepest == nullptr || floor < deepest->y - deepest->w * 0.45f))
            {
                deepest = &chamber;
            }
        }
        if (deepest != nullptr)
        {
            // (On the floor found there: the cave's floor runs from one part's to the next's, and is not quite the chamber's own.)
            const float floor = cave.FloorUnder(glm::vec3(deepest->x, deepest->y - deepest->w * 0.45f, deepest->z));
            plan.cave = 0;
            plan.building = cave.building;
            plan.room = -1;
            plan.floor = 0;
            // In the middle of the chamber, on its stand, facing back the way into it.
            plan.terminalYaw = std::atan2(cave.mouth.x - deepest->x, cave.mouth.z - deepest->z) + glm::pi<float>();
            const glm::vec3 faces = Facing(plan.terminalYaw);
            plan.terminal = glm::vec3(deepest->x, floor + MissionSpec::kStandHeight + size.y * 0.5f, deepest->z);
            plan.driveAt = plan.terminal + faces * (size.z * 0.5f + 0.14f) - glm::vec3(0.0f, size.y * 0.5f - 0.06f, 0.0f);
            plan.circuits.clear();
            for (const FacilityLayout::Lamp& lamp : site.buildings[static_cast<size_t>(cave.building)].lamps)
            {
                if (std::find(plan.circuits.begin(), plan.circuits.end(), lamp.circuit) == plan.circuits.end())
                {
                    plan.circuits.push_back(lamp.circuit);
                }
            }
            plan.powerOut = plan.powerOut && BreakerPanel(site.buildings[static_cast<size_t>(cave.building)], panel, panelYaw);
        }
    }
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

bool StartReading(MissionState& state, const MissionPlan& plan, int marker)
{
    if (plan.kind != MissionPlan::Kind::Survey || state.stage != MissionState::Stage::Find || marker < 0 || marker >= static_cast<int>(plan.markers.size()) ||
        ((state.readings >> marker) & 1u) != 0u)
    {
        return false;
    }
    state.stage = MissionState::Stage::Downloading;
    state.reading = marker;
    state.progress = 0.0f;
    return true;
}

bool AllReadings(const MissionState& state, const MissionPlan& plan)
{
    return !plan.markers.empty() && std::popcount(static_cast<unsigned>(state.readings)) >= static_cast<int>(plan.markers.size());
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
    // A survey's reading: taken while somebody stands at its marker; then the next, or back to the shuttle once all are.
    if (plan.kind == MissionPlan::Kind::Survey)
    {
        if (state.stage == MissionState::Stage::Downloading && state.attended && state.reading >= 0)
        {
            state.progress += dt / MissionSpec::kReadingSeconds;
            if (state.progress >= 1.0f)
            {
                state.readings = static_cast<uint8_t>(state.readings | (1u << state.reading));
                state.reading = -1;
                state.progress = 0.0f;
                state.attended = false;
                state.stage = AllReadings(state, plan) ? MissionState::Stage::Carry : MissionState::Stage::Find;
                ticked.read = true;
            }
        }
    }
    else if (state.stage == MissionState::Stage::Downloading && state.attended && state.powered)
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
