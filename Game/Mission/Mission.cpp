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

MissionPlan::Kind MissionPlan::KindFor(uint32_t siteSeed)
{
    // (Which way a facility's or a signal's goes, on a draw of its own from the site's seed.)
    const bool other = SitePlan::OtherWay(siteSeed);
    switch (SitePlan::KindOf(siteSeed))
    {
    case SitePlan::SiteKind::Station: return Kind::Restore;
    case SitePlan::SiteKind::Survey:
    {
        // (Readings most of the time; now and then a beacon to set up, or samples to bring back cold -- on a draw of its own.)
        const uint32_t way = ((static_cast<uint32_t>(siteSeed) * 2654435761u) >> 20) % 5u;
        return way == 3u ? Kind::Beacon : way == 4u ? Kind::Samples : Kind::Survey;
    }
    case SitePlan::SiteKind::Signal: return other ? Kind::Revive : Kind::Signal;
    case SitePlan::SiteKind::Facility: return other ? Kind::Activate : Kind::Download;
    case SitePlan::SiteKind::Wreck: break;
    }
    return Kind::Download;
}

namespace
{

// A facility's plant got running as a chain, not a switch (Phase 6), where its building has benches enough in rooms other than the
// console's: its generator's filler, its pump and its valves each on the wall over one of them, in a room of its own as far as there are
// rooms, and a fuel can on another (or with the valves, where there is none). The valves found anyhow, and their line-up another way.
void PlacePlantChain(MissionPlan& plan, const SitePlan& site, uint32_t seed)
{
    // (And a station's life support the same way -- you, 2026-11-23: not just click something and wait; FIX 724.)
    if ((plan.kind != MissionPlan::Kind::Activate && plan.kind != MissionPlan::Kind::Restore) || plan.cave >= 0 || plan.building < 0)
    {
        return;
    }
    const FacilityLayout& building = site.buildings[static_cast<size_t>(plan.building)];
    const SiteSystems systems = SiteSystems::Survey(site);
    std::vector<const SiteSystems::Terminal*> benches;
    for (const SiteSystems::Terminal* bench : systems.OpenTerminals())
    {
        if (bench->spot.building == plan.building && bench->spot.room != plan.room)
        {
            benches.push_back(bench);
        }
    }
    if (benches.size() < 3)
    {
        return;
    }
    Random draw((static_cast<uint64_t>(seed) << 20) ^ 0x91A47ull);
    // Shuffled, and then those in rooms not yet used first.
    for (size_t k = benches.size(); k > 1; --k)
    {
        std::swap(benches[k - 1], benches[draw.Index(k)]);
    }
    std::vector<const SiteSystems::Terminal*> picked;
    std::vector<int> rooms;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const SiteSystems::Terminal* bench : benches)
        {
            const bool newRoom = std::find(rooms.begin(), rooms.end(), bench->spot.room) == rooms.end();
            if (picked.size() < 4 && std::find(picked.begin(), picked.end(), bench) == picked.end() && (pass == 1 || newRoom))
            {
                picked.push_back(bench);
                rooms.push_back(bench->spot.room);
            }
        }
    }
    // Over a bench, on its wall: its middle so high over the bench's top.
    const auto onWall = [&](const SiteSystems::Terminal& bench, float depth, float over, glm::vec3& at, float& yaw)
    {
        const FacilityLayout::Fitted& fitted = building.fittings[bench.fitting];
        FacilityLayout::Placed placed;
        placed.floor = fitted.floor;
        placed.at = fitted.at;
        placed.yaw = fitted.yaw;
        const glm::vec3 size = FacilityLayout::FittingSize(fitted.kind);
        const glm::vec3 foot = FacilityMap::PlacedAgainstWall(building, placed, size.y);
        const glm::vec3 facing = Facing(fitted.yaw);
        at = foot - facing * (size.y * 0.5f) + facing * (depth * 0.5f + 0.004f) + glm::vec3(0.0f, size.z + over, 0.0f);
        yaw = fitted.yaw;
        return foot + glm::vec3(0.0f, size.z, 0.0f);
    };
    plan.chain = true;
    onWall(*picked[0], MissionSpec::kFillerSize.z, 0.55f, plan.filler, plan.fillerYaw);
    onWall(*picked[1], MissionSpec::kPumpSize.z, 0.5f, plan.pump, plan.pumpYaw);
    const glm::vec3 valvesBench = onWall(*picked[2], MissionSpec::kManifoldSize.z, 0.6f, plan.valves, plan.valvesYaw);
    float unused = 0.0f;
    glm::vec3 unusedAt{0.0f};
    plan.fuelCan = picked.size() > 3 ? onWall(*picked[3], 0.1f, 0.5f, unusedAt, unused) : valvesBench;
    plan.fuelCan += glm::vec3(0.0f, 0.25f, 0.0f);
    // (Kept in another of the site's buildings, where it has one with a bench -- its stores: you, 2026-11-24, missions hardly took
    // anybody to other buildings; FIX 738. On a draw of its own, so nothing else about the chain changes.)
    {
        std::vector<const SiteSystems::Terminal*> stores;
        for (const SiteSystems::Terminal* bench : systems.OpenTerminals())
        {
            if (bench->spot.building != plan.building)
            {
                stores.push_back(bench);
            }
        }
        if (!stores.empty())
        {
            Random store((static_cast<uint64_t>(seed) << 20) ^ 0x570Eull);
            const SiteSystems::Terminal& bench = *stores[store.Index(stores.size())];
            const FacilityLayout& other = site.buildings[static_cast<size_t>(bench.spot.building)];
            const FacilityLayout::Fitted& fitted = other.fittings[bench.fitting];
            FacilityLayout::Placed placed;
            placed.floor = fitted.floor;
            placed.at = fitted.at;
            placed.yaw = fitted.yaw;
            const glm::vec3 size = FacilityLayout::FittingSize(fitted.kind);
            plan.fuelCan = FacilityMap::PlacedAgainstWall(other, placed, size.y) + glm::vec3(0.0f, size.z + 0.25f, 0.0f);
            plan.fuelBuilding = bench.spot.building;
        }
    }
    // The line-up: two or three open; found otherwise.
    do
    {
        plan.lineUp = static_cast<uint8_t>(draw.Index(1u << MissionSpec::kPlantValves));
    } while (std::popcount(static_cast<unsigned>(plan.lineUp)) < 2 || std::popcount(static_cast<unsigned>(plan.lineUp)) > 3);
    do
    {
        plan.valvesFound = static_cast<uint8_t>(draw.Index(1u << MissionSpec::kPlantValves));
    } while (plan.valvesFound == plan.lineUp);
}

// The data in a server rack rather than on a terminal's bench (Phase 6: data got out by hand), more often than not where the
// terminal's building has a server room (on a draw of its own): the drive bay on the face of one of its racks, at chest height. Pulled
// out; or, damaged (a third of the time where there is another rack to take a board from), fitted with a controller board out of
// another rack's service hatch first; or, an archive too big for a drive (a third of the time), copied onto the copy unit from the
// shuttle and carried back in that. (Not secured, nor its supply faulted: those are a terminal's.)
void PlaceInRack(MissionPlan& plan, const SitePlan& site, uint32_t seed)
{
    if (plan.kind != MissionPlan::Kind::Download || plan.cave >= 0 || plan.building < 0)
    {
        return;
    }
    Random draw((static_cast<uint64_t>(seed) << 20) ^ 0x5E4BE4ull);
    if (!draw.Chance(0.6f))
    {
        return;
    }
    const FacilityLayout& building = site.buildings[static_cast<size_t>(plan.building)];
    const SiteSystems systems = SiteSystems::Survey(site);
    std::vector<const SiteSystems::Terminal*> racks;
    for (const SiteSystems::Terminal& rack : systems.racks)
    {
        if (rack.open && rack.spot.building == plan.building)
        {
            racks.push_back(&rack);
        }
    }
    if (racks.empty())
    {
        return;
    }
    const auto bayOf = [&](size_t fitting, glm::vec3& at, float& yaw)
    {
        const FacilityLayout::Fitted& fitted = building.fittings[fitting];
        FacilityLayout::Placed placed;
        placed.floor = fitted.floor;
        placed.at = fitted.at;
        placed.yaw = fitted.yaw;
        const glm::vec3 rackSize = FacilityLayout::FittingSize(fitted.kind);
        const glm::vec3 foot = FacilityMap::PlacedAgainstWall(building, placed, rackSize.y);
        yaw = fitted.yaw;
        at = foot + Facing(yaw) * (rackSize.y * 0.5f + MissionSpec::kBaySize.z * 0.5f) + glm::vec3(0.0f, MissionSpec::kBayHeight, 0.0f);
    };
    const SiteSystems::Terminal& chosen = *racks[draw.Index(racks.size())];
    bayOf(chosen.fitting, plan.terminal, plan.terminalYaw);
    plan.floor = chosen.spot.floor;
    plan.room = chosen.spot.room;
    plan.driveAt = plan.terminal + Facing(plan.terminalYaw) * 0.3f;
    plan.code = 0;
    plan.clue = glm::vec3(0.0f);
    plan.blownFuse = -1;
    const float kind = draw.Range(0.0f, 1.0f);
    plan.data = kind < 0.34f ? MissionPlan::Data::Archive : kind < 0.67f ? MissionPlan::Data::Damaged : MissionPlan::Data::Pull;
    if (plan.data == MissionPlan::Data::Archive)
    {
        plan.downloadSeconds = MissionSpec::kArchiveSeconds;
    }
    if (plan.data == MissionPlan::Data::Damaged)
    {
        // Its board from another rack of the building -- any, in this room or another.
        std::vector<const SiteSystems::Terminal*> others;
        for (const SiteSystems::Terminal& rack : systems.racks)
        {
            if (rack.spot.building == plan.building && rack.fitting != chosen.fitting && rack.spot.room >= 0)
            {
                others.push_back(&rack);
            }
        }
        if (others.empty())
        {
            plan.data = MissionPlan::Data::Pull;
        }
        else
        {
            bayOf(others[draw.Index(others.size())]->fitting, plan.donor, plan.donorYaw);
        }
    }
}

// The optional goal (MissionPlan::extra): about three missions in five (on a draw of their own, so nothing else about the mission
// changes for it), a records drive on a bench in another of the site's buildings than the mission's own -- any bench a room that
// runs things has, reached without the keycard. None where the site has no other building with one.
void PlaceExtra(MissionPlan& plan, const SitePlan& site, uint32_t seed)
{
    Random extra((static_cast<uint64_t>(seed) << 20) ^ 0xE7A4ull);
    if (!extra.Chance(0.6f))
    {
        return;
    }
    // (The survey kept while its terminals are looked through: they are its own.)
    const SiteSystems systems = SiteSystems::Survey(site);
    std::vector<const SiteSystems::Terminal*> benches;
    for (const SiteSystems::Terminal* terminal : systems.OpenTerminals())
    {
        if (terminal->spot.building != plan.building)
        {
            benches.push_back(terminal);
        }
    }
    if (benches.empty())
    {
        return;
    }
    const SiteSystems::Terminal& chosen = *benches[extra.Index(benches.size())];
    const FacilityLayout& building = site.buildings[static_cast<size_t>(chosen.spot.building)];
    const FacilityLayout::Fitted& fitted = building.fittings[chosen.fitting];
    FacilityLayout::Placed bench;
    bench.floor = fitted.floor;
    bench.at = fitted.at;
    bench.yaw = fitted.yaw;
    const glm::vec3 benchSize = FacilityLayout::FittingSize(fitted.kind);
    // Lying on the bench top, a little off its middle towards the room.
    plan.extraBuilding = chosen.spot.building;
    // (Now and then a part for the ship instead of a records drive.)
    if (extra.Chance(MissionSpec::kPartChance))
    {
        plan.extraPart = static_cast<int>(extra.Index(MissionSpec::kPartKinds));
    }
    plan.extra = FacilityMap::PlacedAgainstWall(building, bench, benchSize.y) + glm::vec3(0.0f, benchSize.z + 0.06f, 0.0f) +
                 Facing(bench.yaw) * 0.1f;
}

} // namespace

std::vector<std::string> MissionPlan::KitWanted() const
{
    std::vector<std::string> kit;
    if (!Valid() || kind == Kind::Explore)
    {
        return kit;
    }
    kit.emplace_back("objective_tracker");
    if (mapGiven)
    {
        kit.emplace_back("site_map");
    }
    if (Faulted() || data == Data::Damaged)
    {
        kit.emplace_back("multitool");
    }
    if (Faulted() || boardFault >= 0)
    {
        kit.emplace_back("circuit_tester");
    }
    return kit;
}

MissionPlan MissionPlan::Exploring(uint32_t seed)
{
    MissionPlan plan;
    plan.seed = seed;
    plan.kind = Kind::Explore;
    return plan;
}

float MissionPlan::Complexity() const
{
    float cost = 0.0f;
    cost += code != 0 ? 1.0f : 0.0f;
    cost += powerOut ? 1.0f + (boardFault >= 0 ? 0.5f : 0.0f) : 0.0f;
    cost += feedCut ? 1.5f : 0.0f;
    cost += Faulted() ? 1.5f : 0.0f;
    cost += data == Data::Damaged ? 1.5f : data == Data::Archive ? 1.0f : 0.0f;
    cost += chain ? 2.0f : 0.0f;
    return cost;
}

namespace
{

// The complication budget (FIX 741): complications taken away, one at a time, until what is left costs no more than `budget` --
// which, from the plan's own seed, so the same on every machine. How much each would save decides nothing: any of them may go, so
// two missions of a budget are not the same two complications.
void Trim(MissionPlan& plan, uint32_t seed, float budget)
{
    if (budget < 0.0f)
    {
        return;
    }
    enum class Cut { Code, Power, Breaker, Feed, Fuse, Rack, Chain };
    Random draw((static_cast<uint64_t>(seed) << 20) ^ 0xB0D6E7ull);
    // (And now and then less than it allows: a quiet one -- not every mission has everything go wrong.)
    budget *= draw.Range(0.55f, 1.0f);
    while (plan.Complexity() > budget + 1e-3f)
    {
        std::vector<Cut> cuts;
        if (plan.code != 0)
        {
            cuts.push_back(Cut::Code);
        }
        // (A station's power is its mission: never taken away -- only what is on top of it.)
        if (plan.powerOut && plan.kind != MissionPlan::Kind::Restore)
        {
            cuts.push_back(Cut::Power);
        }
        if (plan.powerOut && plan.boardFault >= 0)
        {
            cuts.push_back(Cut::Breaker);
        }
        if (plan.feedCut)
        {
            cuts.push_back(Cut::Feed);
        }
        if (plan.Faulted())
        {
            cuts.push_back(Cut::Fuse);
        }
        if (plan.data == MissionPlan::Data::Damaged || plan.data == MissionPlan::Data::Archive)
        {
            cuts.push_back(Cut::Rack);
        }
        if (plan.chain)
        {
            cuts.push_back(Cut::Chain);
        }
        if (cuts.empty())
        {
            return;
        }
        switch (cuts[draw.Index(cuts.size())])
        {
        case Cut::Code:
            plan.code = 0;
            plan.clue = glm::vec3(0.0f);
            plan.clueRoom = -1;
            plan.clueBuilding = -1;
            break;
        case Cut::Power:
            plan.powerOut = false;
            plan.feedCut = false;
            plan.boardFault = -1;
            plan.boardTripped = 0;
            break;
        case Cut::Breaker: plan.boardFault = -1; break;
        case Cut::Feed: plan.feedCut = false; break;
        case Cut::Fuse: plan.blownFuse = -1; break;
        case Cut::Rack: plan.data = MissionPlan::Data::Pull; break;
        case Cut::Chain: plan.chain = false; break;
        }
    }
}

} // namespace

MissionPlan MissionPlan::Generate(const SitePlan& site, uint32_t seed, float budget)
{
    MissionPlan plan;
    plan.seed = seed;
    Random random((static_cast<uint64_t>(seed) << 20) ^ 0xDA7A5EEDull);

    // A signal site's mission is a signal (Phase 4): a transmitter set up somewhere out on the site, among the buildings or round them,
    // to be found and shut down -- or, gone dead, brought back on the air -- worked by hand at it, and back to the shuttle.
    if (SitePlan::KindOf(site.seed) == SitePlan::SiteKind::Signal)
    {
        MissionPlan signal;
        signal.seed = seed;
        signal.kind = KindFor(site.seed);
        Random where((static_cast<uint64_t>(seed) << 20) ^ 0x516A1ull);
        const glm::vec2 middle = glm::vec2(site.origin.x, site.origin.z) + glm::vec2(site.size * 0.5f);
        const glm::vec2 pad{site.landing.x, site.landing.z};
        // About four in ten (on a draw of their own), set up indoors (you, 2026-11-15: missions that use the place): on the floor of a
        // room of one of the site's buildings, in front of a bench -- tracked down from outside, then hunted for through the rooms.
        if (Random((static_cast<uint64_t>(seed) << 20) ^ 0x1D00Bull).Chance(0.4f))
        {
            const SiteSystems systems = SiteSystems::Survey(site);
            const std::vector<const SiteSystems::Terminal*> benches = systems.OpenTerminals();
            if (!benches.empty())
            {
                const SiteSystems::Terminal& chosen = *benches[where.Index(benches.size())];
                const FacilityLayout& building = site.buildings[static_cast<size_t>(chosen.spot.building)];
                const FacilityLayout::Fitted& fitted = building.fittings[chosen.fitting];
                FacilityLayout::Placed bench;
                bench.floor = fitted.floor;
                bench.at = fitted.at;
                bench.yaw = fitted.yaw;
                const glm::vec3 benchSize = FacilityLayout::FittingSize(fitted.kind);
                const glm::vec3 foot = FacilityMap::PlacedAgainstWall(building, bench, benchSize.y);
                signal.markers.push_back(foot + Facing(bench.yaw) * (benchSize.y * 0.5f + 0.55f));
                signal.heightSet = 1u; // (its height its floor's, not the ground's out there)
                return signal;
            }
        }
        for (int attempt = 0; attempt < 200 && signal.markers.empty(); ++attempt)
        {
            const float angle = where.Range(0.0f, glm::two_pi<float>());
            const glm::vec2 at = middle + glm::vec2(std::cos(angle), std::sin(angle)) * where.Range(0.18f, 0.44f) * site.size;
            if (!site.InBuilding(at, 5.0f) && glm::distance(at, pad) > 40.0f)
            {
                signal.markers.emplace_back(at.x, site.origin.y, at.y);
            }
        }
        if (!signal.markers.empty())
        {
            return signal;
        }
    }
    // A survey site's mission is a survey (Phase 4): readings at markers set out across the site -- four to six of them (you,
    // 2026-11-15: longer missions), out on the open ground among the buildings and round them and out to its edges, well apart, none
    // in a building nor by the pad -- and back to the shuttle with them.
    if (SitePlan::KindOf(site.seed) == SitePlan::SiteKind::Survey)
    {
        MissionPlan survey;
        survey.seed = seed;
        survey.kind = KindFor(site.seed);
        Random markers((static_cast<uint64_t>(seed) << 20) ^ 0x5E7A11ull);
        // A beacon: one place, the site's highest ground (the game finds it, from the land as it is made: its middle stands in until
        // then), and its aim. Samples: three outcrops, well apart.
        if (survey.kind == Kind::Beacon)
        {
            survey.markers.emplace_back(site.origin.x + site.size * 0.5f, site.origin.y, site.origin.z + site.size * 0.5f);
            survey.aimBearing = static_cast<uint8_t>(markers.Index(MissionSpec::kAimSteps));
            survey.aimTilt = static_cast<uint8_t>(1 + markers.Index(MissionSpec::kTiltSteps - 1));
            survey.mapGiven = true;
            PlaceExtra(survey, site, seed);
            return survey;
        }
        const int wanted = survey.kind == Kind::Samples ? MissionSpec::kSampleSites : 4 + static_cast<int>(markers.Index(3));
        const glm::vec2 middle = glm::vec2(site.origin.x, site.origin.z) + glm::vec2(site.size * 0.5f);
        const glm::vec2 pad{site.landing.x, site.landing.z};
        for (int attempt = 0; attempt < 300 && static_cast<int>(survey.markers.size()) < wanted; ++attempt)
        {
            const float angle = markers.Range(0.0f, glm::two_pi<float>());
            const float out = markers.Range(0.12f, 0.46f) * site.size;
            const glm::vec2 at = middle + glm::vec2(std::cos(angle), std::sin(angle)) * out;
            bool fits = !site.InBuilding(at, 5.0f) && glm::distance(at, pad) > 22.0f;
            for (const glm::vec3& other : survey.markers)
            {
                fits = fits && glm::distance(at, glm::vec2(other.x, other.z)) > site.size * 0.15f;
            }
            if (fits)
            {
                survey.markers.emplace_back(at.x, site.origin.y, at.y);
            }
        }
        if (survey.markers.size() >= 3)
        {
            survey.mapGiven = true;
            PlaceExtra(survey, site, seed);
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
    // (Something to be got running -- a station's life support, a facility's plant -- only in a building that could still run: not one
    // burnt out, nor one half fallen in. Data can be got off a terminal anywhere it survived.)
    const bool running = SitePlan::KindOf(site.seed) == SitePlan::SiteKind::Station ||
                         (SitePlan::KindOf(site.seed) == SitePlan::SiteKind::Facility && KindFor(site.seed) == Kind::Activate);
    const auto couldRun = [&](int b)
    {
        const FacilityLayout& building = site.buildings[static_cast<size_t>(b)];
        return building.condition != FacilityLayout::Condition::Burnt && !building.ruin.Has();
    };
    bool anyCouldRun = false;
    for (const SiteSystems::Terminal* terminal : systems.OpenTerminals())
    {
        anyCouldRun = anyCouldRun || couldRun(terminal->spot.building);
    }
    for (const SiteSystems::Terminal* terminal : systems.OpenTerminals())
    {
        if (!running || !anyCouldRun || couldRun(terminal->spot.building))
        {
            candidates.push_back({terminal->spot.building, terminal->fitting});
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
    // Secured, about two data terminals in five (on a draw of their own): its access code on a note on a bench elsewhere -- in another
    // of the site's buildings where there is one to leave it in (you, 2026-11-24: missions hardly took anybody to other buildings;
    // FIX 738), otherwise another room of its own -- to be searched for. (Not where there is no other bench to leave it on.)
    {
        Random secured((static_cast<uint64_t>(seed) << 20) ^ 0xC0DEull);
        if (secured.Range(0.0f, 1.0f) < 0.4f)
        {
            const auto roomOf = [&](const Candidate& c)
            {
                const FacilityLayout& in = site.buildings[static_cast<size_t>(c.building)];
                const FacilityLayout::Fitted& at = in.fittings[c.thing];
                return in.RoomAt(at.floor, static_cast<int>(std::floor(at.at.x)), static_cast<int>(std::floor(at.at.y)));
            };
            // (Where somebody would write it down and leave it, given the choice: an office, the control room, a lab -- you, 2026-11-23: it
            // was too easy to miss.)
            const auto isDesk = [&](const Candidate& c)
            {
                const int room = roomOf(c);
                const FacilityLayout& in = site.buildings[static_cast<size_t>(c.building)];
                const FacilityLayout::Purpose purpose = room >= 0 ? in.rooms[static_cast<size_t>(room)].purpose : FacilityLayout::Purpose::None;
                return purpose == FacilityLayout::Purpose::Office || purpose == FacilityLayout::Purpose::Control || purpose == FacilityLayout::Purpose::Lab;
            };
            std::vector<const Candidate*> otherBuildings;
            std::vector<const Candidate*> elsewhere;
            for (const Candidate& other : candidates)
            {
                if (other.building != chosen.building)
                {
                    otherBuildings.push_back(&other);
                }
                else if (&other != &chosen && roomOf(other) != plan.room)
                {
                    elsewhere.push_back(&other);
                }
            }
            if (!otherBuildings.empty())
            {
                elsewhere = otherBuildings;
            }
            std::vector<const Candidate*> desks;
            for (const Candidate* other : elsewhere)
            {
                if (isDesk(*other))
                {
                    desks.push_back(other);
                }
            }
            if (!desks.empty())
            {
                elsewhere = desks;
            }
            if (!elsewhere.empty())
            {
                const Candidate& noteAt = *elsewhere[secured.Index(elsewhere.size())];
                const FacilityLayout& noteBuilding = site.buildings[static_cast<size_t>(noteAt.building)];
                const FacilityLayout::Fitted& noteOn = noteBuilding.fittings[noteAt.thing];
                plan.clueBuilding = noteAt.building;
                plan.clueFloor = noteOn.floor;
                plan.clueRoom = roomOf(noteAt);
                FacilityLayout::Placed other;
                other.floor = noteOn.floor;
                other.at = noteOn.at;
                other.yaw = noteOn.yaw;
                const glm::vec3 otherSize = FacilityLayout::FittingSize(noteOn.kind);
                plan.clue = FacilityMap::PlacedAgainstWall(noteBuilding, other, otherSize.y) + glm::vec3(0.0f, otherSize.z + 0.002f, 0.0f);
                plan.clueYaw = other.yaw;
                plan.code = static_cast<uint16_t>(1000 + secured.Index(9000));
            }
        }
    }
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
    // More often than not, where the site has a cave (you, 2026-11-15: missions in the caves), down there instead: a survey station
    // on the floor of its deepest chamber the water has not reached (on a draw of its own, so nothing else about the mission changes
    // for it), on the mine's power.
    // (At a mine, nearly always: the caves are what a mining site is about -- you, 2026-11-23.)
    if (!site.caves.empty() && Random((static_cast<uint64_t>(seed) << 20) ^ 0xCA7E5EEDull).Chance(site.type == SitePlan::FacilityType::Mining ? 0.9f : 0.6f))
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
            // (A survey station down a mine's cave has no access code: nobody would lock one down there, nor leave its code on a bench
            // up in the buildings -- you, 2026-11-23.)
            plan.code = 0;
            plan.clue = glm::vec3(0.0f);
            plan.clueRoom = -1;
            plan.clueBuilding = -1;
        }
    }
    // A station's on the ground: its power out and its life support down -- the breaker reset, then the system restarted at the
    // console (the terminal's place), and back to the shuttle. Nothing to carry: what was wanted is done there.
    if (SitePlan::KindOf(site.seed) == SitePlan::SiteKind::Station && plan.cave < 0)
    {
        plan.kind = Kind::Restore;
        plan.powerOut = BreakerPanel(building, panel, panelYaw);
        plan.downloadSeconds = Random((static_cast<uint64_t>(seed) << 20) ^ 0x2E57ull).Range(20.0f, 32.0f);
    }
    // A facility's, half the time, is to get its systems running: its power back where it is out (as for the data), then its plant
    // started at a control console in the terminal's place, a system at a time -- and back to the shuttle, nothing to carry.
    // (Not down a cave: the station there is a survey station on a stand, with data on it.)
    if (SitePlan::KindOf(site.seed) == SitePlan::SiteKind::Facility && KindFor(site.seed) == Kind::Activate && plan.cave < 0)
    {
        plan.kind = Kind::Activate;
        plan.downloadSeconds = Random((static_cast<uint64_t>(seed) << 20) ^ 0xAC71ull).Range(22.0f, 34.0f);
    }
    // (Only a data terminal is secured: a console's switches are its own.)
    if (plan.AtConsole())
    {
        plan.code = 0;
        plan.clue = glm::vec3(0.0f);
        plan.clueYaw = 0.0f;
    }
    // Its feed cut too, about four times in ten where the power is out (on a draw of its own): its feeder cabinet outside, against its
    // wall beside one of its doors (not a vehicle bay's, nor one a covered way joins), its front away from the building, where a
    // building's supply comes in. (Not on the sea's floor: its supply comes down to it under the sea, and a cabinet stood out on the
    // sand made no sense; nor stood out in the open a few metres off -- you, 2026-11-24; FIX 737.)
    if (plan.powerOut && plan.cave < 0 && !site.buildings[static_cast<size_t>(plan.building)].HasMoonPool())
    {
        Random feed((static_cast<uint64_t>(seed) << 20) ^ 0xFEEDull);
        const FacilityLayout& fed = site.buildings[static_cast<size_t>(plan.building)];
        std::vector<const FacilityLayout::Exit*> doors;
        for (const FacilityLayout::Exit& exit : fed.exits)
        {
            if (!exit.bay && !exit.tubed)
            {
                doors.push_back(&exit);
            }
        }
        if (feed.Chance(0.4f) && !doors.empty())
        {
            const FacilityLayout::Exit& door = *doors[feed.Index(doors.size())];
            const glm::vec2 outward = door.side == 0 ? glm::vec2(-1.0f, 0.0f) : door.side == 1 ? glm::vec2(1.0f, 0.0f) : door.side == 2 ? glm::vec2(0.0f, -1.0f)
                                                                                                                          : glm::vec2(0.0f, 1.0f);
            const glm::vec2 cell = glm::vec2(door.cell) + glm::vec2(0.5f);
            const glm::vec3 inside = FacilityMap::ToWorld(fed, fed.ground, cell);
            const glm::vec3 out = glm::normalize(FacilityMap::ToWorld(fed, fed.ground, cell + outward) - inside);
            const glm::vec3 along{-out.z, 0.0f, out.x};
            plan.feedCut = true;
            const float side = feed.Chance(0.5f) ? 1.0f : -1.0f;
            // Against the wall, a little to one side of the door -- and of the airlock built out round it, where it has one.
            const glm::vec3 wall = (inside + FacilityMap::ToWorld(fed, fed.ground, cell + outward)) * 0.5f;
            const bool locked = fed.outside.airlocks && !(door.roomy && !door.bay);
            // (Clear of the wall's own thickness and its cladding, out from the cells' line.)
            plan.cabinet = wall + out * (MissionSpec::kCabinetSize.z * 0.5f + 0.8f) +
                           along * ((locked ? FacilitySpec::kAirlockWidth * 0.5f + FacilitySpec::kAirlockWall + 1.1f : 1.9f) * side);
            plan.cabinetYaw = std::atan2(-out.x, -out.z);
        }
    }
    // Its supply faulted, about a terminal or a console in three (on a draw of its own): a fuse blown in the supply box on the wall
    // over its bench, beside it. (Not down a cave: a survey station's stand has no wall.)
    if (plan.cave < 0)
    {
        Random fault((static_cast<uint64_t>(seed) << 20) ^ 0xF05Eull);
        if (fault.Chance(0.35f))
        {
            const glm::vec3 side{std::cos(bench.yaw), 0.0f, -std::sin(bench.yaw)};
            const glm::vec3 wall = benchFoot - facing * (benchSize.y * 0.5f);
            plan.blownFuse = static_cast<int>(fault.Index(MissionSpec::kFuses));
            plan.fuseBox = wall + facing * (MissionSpec::kFuseBoxSize.z * 0.5f + 0.004f) + side * (benchSize.x * 0.5f - 0.3f) +
                           glm::vec3(0.0f, benchSize.z + 0.6f, 0.0f);
            plan.fuseBoxYaw = bench.yaw;
        }
    }
    // The board, where the power is out (on a draw of its own): two to four of its breakers found tripped, and about half the time
    // one of those on a circuit with a fault -- which takes the main switch out again until it is found and left off.
    if (plan.powerOut)
    {
        Random board((static_cast<uint64_t>(seed) << 20) ^ 0xB0A2Dull);
        const int tripped = 2 + static_cast<int>(board.Index(3));
        while (std::popcount(static_cast<unsigned>(plan.boardTripped)) < tripped)
        {
            plan.boardTripped = static_cast<uint8_t>(plan.boardTripped | (1u << board.Index(static_cast<size_t>(MissionSpec::kBoardBreakers))));
        }
        if (board.Chance(0.5f))
        {
            int pick = static_cast<int>(board.Index(static_cast<size_t>(tripped)));
            for (int b = 0; b < MissionSpec::kBoardBreakers; ++b)
            {
                if (((plan.boardTripped >> b) & 1u) != 0u && pick-- == 0)
                {
                    plan.boardFault = b;
                }
            }
        }
    }
    PlaceInRack(plan, site, seed);
    PlacePlantChain(plan, site, seed);
    PlaceExtra(plan, site, seed);
    Trim(plan, seed, budget);
    return plan;
}

namespace MissionRules
{

MissionState Start(const MissionPlan& plan)
{
    MissionState state;
    // (Only looking round: from the start, it is getting back to the shuttle with what was found -- and the shuttle may go whenever.)
    state.stage = plan.kind == MissionPlan::Kind::Explore ? MissionState::Stage::Carry
                  : plan.Valid()                          ? MissionState::Stage::Find
                                                          : MissionState::Stage::None;
    state.powered = !plan.powerOut;
    // The board as it is found: with the power out, its main switch gone and some of its breakers with it.
    state.boardMain = state.powered;
    state.boardOn = state.powered ? MissionSpec::kBoardAll : static_cast<uint8_t>(MissionSpec::kBoardAll & ~plan.boardTripped);
    state.unlocked = !plan.Secured();
    state.supplied = !plan.Faulted();
    state.fed = !plan.feedCut;
    // A plant's valves as they are found.
    state.valves = plan.valvesFound;
    // A rack's drive: good, unless it is the damaged one.
    state.rack = plan.data == MissionPlan::Data::Damaged ? 0 : MissionState::kRackBoard;
    // A survey's markers, all in their crate to begin with.
    if (plan.kind == MissionPlan::Kind::Survey)
    {
        state.markers = static_cast<uint8_t>(std::min<size_t>(plan.markers.size(), MissionSpec::kMaxMarkers));
    }
    // A dead transmitter, to be brought back: found off, its power and its sender both.
    if (plan.kind == MissionPlan::Kind::Revive)
    {
        state.device = static_cast<uint8_t>(MissionState::kSenderOff | MissionState::kPowerOff);
    }
    return state;
}

Worked WorkKeypad(MissionState& state, const MissionPlan& plan, int key)
{
    if (!plan.Secured() || state.unlocked || !state.powered || !state.supplied || state.stage != MissionState::Stage::Find || key < 0 || key >= kKeys)
    {
        return Worked::Nothing;
    }
    if (key == kKeyClear)
    {
        state.typed = 0;
        state.typedCount = 0;
        return Worked::Switched;
    }
    if (key == kKeyEnter)
    {
        const bool right = state.typedCount == 4 && state.typed == plan.code;
        state.typed = 0;
        state.typedCount = 0;
        if (right)
        {
            state.unlocked = true;
            return Worked::Done;
        }
        return Worked::Tripped;
    }
    if (state.typedCount >= 4)
    {
        return Worked::Nothing;
    }
    state.typed = static_cast<uint16_t>(state.typed * 10 + key);
    ++state.typedCount;
    return Worked::Switched;
}

Worked WorkBox(MissionState& state, const MissionPlan& plan, int control, bool tool)
{
    if (!plan.Faulted() || state.supplied || state.stage != MissionState::Stage::Find || control < 0 || control >= kBoxControls)
    {
        return Worked::Nothing;
    }
    // Whether that has given the terminal its supply back.
    const auto settled = [&]
    {
        state.supplied = state.boxIsolator && state.boxFuses == MissionSpec::kFusesAll && state.boxSpared;
        return state.supplied;
    };
    if (control < kBoxIsolator)
    {
        const uint8_t bit = static_cast<uint8_t>(1u << (control - kBoxScrew));
        if ((state.boxScrews & bit) != 0 || !tool)
        {
            return Worked::Nothing;
        }
        state.boxScrews = static_cast<uint8_t>(state.boxScrews | bit);
        return BoxOpen(state) ? Worked::Opened : Worked::Switched;
    }
    if (control == kBoxIsolator)
    {
        state.boxIsolator = !state.boxIsolator;
        return settled() ? Worked::Done : Worked::Switched;
    }
    if (!BoxOpen(state))
    {
        return Worked::Nothing;
    }
    const int fuse = control - kBoxFuse;
    const uint8_t bit = static_cast<uint8_t>(1u << fuse);
    // A holder opened, or shut again -- on the spare, where the blown fuse was. Live, either draws an arc.
    if ((state.boxFuses & bit) != 0)
    {
        state.boxFuses = static_cast<uint8_t>(state.boxFuses & ~bit);
        // (No arc from the blown one: nothing flows through it.)
        return state.boxIsolator && (fuse != plan.blownFuse || state.boxSpared) ? Worked::Tripped : Worked::Switched;
    }
    state.boxFuses = static_cast<uint8_t>(state.boxFuses | bit);
    if (fuse == plan.blownFuse)
    {
        state.boxSpared = true;
    }
    if (settled())
    {
        return Worked::Done;
    }
    return state.boxIsolator ? Worked::Tripped : Worked::Switched;
}

int LegLength(const MissionState::Marker& marker, int leg)
{
    return (marker.legs >> (leg * 2)) & 3;
}

int GroundUnder(const MissionState::Marker& marker, int leg)
{
    return (marker.ground >> (leg * 2)) & 3;
}

bool MarkerLevel(const MissionState::Marker& marker)
{
    const int first = LegLength(marker, 0) - GroundUnder(marker, 0);
    for (int leg = 1; leg < MissionSpec::kMarkerLegs; ++leg)
    {
        if (LegLength(marker, leg) - GroundUnder(marker, leg) != first)
        {
            return false;
        }
    }
    return true;
}

bool MarkerWorking(const MissionState::Marker& marker)
{
    constexpr uint8_t legs = MissionState::kLegOut * 7u;
    return (marker.setup & (MissionState::kPlanted | MissionState::kOn | legs | MissionState::kFallen | MissionState::kPacked)) ==
               (MissionState::kPlanted | MissionState::kOn | legs) &&
           MarkerLevel(marker);
}

int MarkersWorking(const MissionState& state)
{
    int working = 0;
    for (int m = 0; m < state.markers; ++m)
    {
        working += MarkerWorking(state.survey[static_cast<size_t>(m)]) ? 1 : 0;
    }
    return working;
}

bool MarkerPackable(const MissionState& state, int marker)
{
    if (marker < 0 || marker >= state.markers || ((state.readings >> marker) & 1u) == 0u)
    {
        return false;
    }
    const uint8_t setup = state.survey[static_cast<size_t>(marker)].setup;
    return (setup & (MissionState::kPlanted | MissionState::kOn | MissionState::kLegOut * 7u | MissionState::kFallen)) == MissionState::kPlanted;
}

glm::vec2 LegWay(int leg)
{
    // The first leg towards whoever faces it (its -z), the other two a third of a turn either side of it, back and out -- each in sight
    // beside the stake from in front (you, 2026-11-23: the back leg was hidden behind it, hard to adjust).
    const float angle = glm::pi<float>() + static_cast<float>(leg) * glm::two_pi<float>() / static_cast<float>(MissionSpec::kMarkerLegs);
    return {std::sin(angle), std::cos(angle)};
}

glm::vec2 MarkerLean(const MissionState::Marker& marker)
{
    // A leg bearing more than the others (longer, or on higher ground) pushes the head away from it.
    float mean = 0.0f;
    for (int leg = 0; leg < MissionSpec::kMarkerLegs; ++leg)
    {
        mean += static_cast<float>(LegLength(marker, leg) - GroundUnder(marker, leg));
    }
    mean /= static_cast<float>(MissionSpec::kMarkerLegs);
    glm::vec2 lean{0.0f};
    for (int leg = 0; leg < MissionSpec::kMarkerLegs; ++leg)
    {
        lean -= LegWay(leg) * (static_cast<float>(LegLength(marker, leg) - GroundUnder(marker, leg)) - mean);
    }
    return lean;
}

Worked PlantMarker(MissionState& state, const MissionPlan& plan, int marker, uint8_t ground)
{
    if (plan.kind != MissionPlan::Kind::Survey || state.stage == MissionState::Stage::None || state.stage == MissionState::Stage::Over || marker < 0 ||
        marker >= state.markers)
    {
        return Worked::Nothing;
    }
    MissionState::Marker& one = state.survey[static_cast<size_t>(marker)];
    if ((one.setup & (MissionState::kPlanted | MissionState::kPacked)) != 0u)
    {
        return Worked::Nothing;
    }
    // Pushed into the ground, its legs folded along it, off -- and its reading where it had got to, if it had been up before.
    one.setup = MissionState::kPlanted;
    one.legs = 0;
    one.ground = ground;
    return Worked::Opened;
}

Worked WorkMarker(MissionState& state, const MissionPlan& plan, int marker, int control, uint8_t ground)
{
    if (plan.kind != MissionPlan::Kind::Survey || state.stage == MissionState::Stage::None || state.stage == MissionState::Stage::Over || marker < 0 ||
        marker >= state.markers || control < 0 || control >= kMarkerControls)
    {
        return Worked::Nothing;
    }
    MissionState::Marker& one = state.survey[static_cast<size_t>(marker)];
    if ((one.setup & MissionState::kPlanted) == 0u)
    {
        return Worked::Nothing;
    }
    const bool fallen = (one.setup & MissionState::kFallen) != 0u;
    if (control == kMarkerStake)
    {
        // Knocked over: stood back up, pushed in again where it stood -- not quite as it was, so it wants levelling again.
        if (fallen)
        {
            one.setup = static_cast<uint8_t>(one.setup & ~MissionState::kFallen);
            one.ground = ground;
            return Worked::Opened;
        }
        if (MarkerPackable(state, marker))
        {
            one.setup = MissionState::kPacked;
            one.legs = 0;
            return Worked::Done;
        }
        return Worked::Nothing;
    }
    if (fallen)
    {
        return Worked::Nothing;
    }
    if (control < kMarkerClamp)
    {
        one.setup = static_cast<uint8_t>(one.setup ^ (MissionState::kLegOut << (control - kMarkerLeg)));
        return Worked::Opened;
    }
    if (control < kMarkerSwitch)
    {
        const int leg = control - kMarkerClamp;
        if ((one.setup & (MissionState::kLegOut << leg)) == 0u)
        {
            return Worked::Nothing;
        }
        const int longer = (LegLength(one, leg) + 1) % MissionSpec::kLegSteps;
        one.legs = static_cast<uint8_t>((one.legs & ~(3u << (leg * 2))) | (static_cast<unsigned>(longer) << (leg * 2)));
        return Worked::Switched;
    }
    one.setup = static_cast<uint8_t>(one.setup ^ MissionState::kOn);
    return Worked::Switched;
}

Worked WorkRack(MissionState& state, const MissionPlan& plan, int control, bool carrying)
{
    if (!plan.AtRack() || state.stage == MissionState::Stage::None || state.stage == MissionState::Stage::Over)
    {
        return Worked::Nothing;
    }
    const auto has = [&](uint8_t bit) { return (state.rack & bit) != 0u; };
    const auto set = [&](uint8_t bit, bool on) { state.rack = static_cast<uint8_t>(on ? state.rack | bit : state.rack & ~bit); };
    const bool finding = state.stage == MissionState::Stage::Find;
    switch (control)
    {
    case kRackCover:
        // (Not shut on the copy unit's cable.)
        if (has(MissionState::kRackUnit) || has(MissionState::kRackSled))
        {
            return Worked::Nothing;
        }
        set(MissionState::kRackCover, !has(MissionState::kRackCover));
        return Worked::Opened;
    case kRackSled:
        if (!finding || !has(MissionState::kRackCover) || plan.data == MissionPlan::Data::Archive)
        {
            return Worked::Nothing;
        }
        // Damaged: drawn out to its board, or pushed back; good: pulled, the drive out in the hand.
        if (!has(MissionState::kRackBoard))
        {
            set(MissionState::kRackSled, !has(MissionState::kRackSled));
            return Worked::Opened;
        }
        set(MissionState::kRackSled, false);
        state.stage = MissionState::Stage::Carry;
        state.progress = 1.0f;
        return Worked::Done;
    case kRackSlot:
        if (!finding || plan.data != MissionPlan::Data::Damaged || !has(MissionState::kRackSled) || has(MissionState::kRackBoard) || !carrying)
        {
            return Worked::Nothing;
        }
        set(MissionState::kRackBoard, true);
        return Worked::Switched;
    case kRackPort:
        if (!finding || plan.data != MissionPlan::Data::Archive || !has(MissionState::kRackCover) || has(MissionState::kRackUnit) || !carrying)
        {
            return Worked::Nothing;
        }
        set(MissionState::kRackUnit, true);
        state.stage = MissionState::Stage::Downloading;
        state.progress = 0.0f;
        return Worked::Switched;
    case kRackUnit:
        if (!has(MissionState::kRackUnit) || state.stage != MissionState::Stage::Carry)
        {
            return Worked::Nothing;
        }
        set(MissionState::kRackUnit, false);
        return Worked::Done;
    case kDonorHatch:
        if (plan.data != MissionPlan::Data::Damaged)
        {
            return Worked::Nothing;
        }
        set(MissionState::kDonorHatch, !has(MissionState::kDonorHatch));
        return Worked::Opened;
    case kDonorBoard:
        if (plan.data != MissionPlan::Data::Damaged || !has(MissionState::kDonorHatch) || has(MissionState::kDonorTaken) || !carrying)
        {
            return Worked::Nothing;
        }
        set(MissionState::kDonorTaken, true);
        return Worked::Done;
    default: break;
    }
    return Worked::Nothing;
}

int NextBeaconPart(const MissionState& state)
{
    for (int part = 0; part < MissionSpec::kBeaconParts; ++part)
    {
        if (((state.beacon >> part) & 1u) == 0u)
        {
            return part;
        }
    }
    return MissionSpec::kBeaconParts;
}

bool BeaconAimed(const MissionState& state, const MissionPlan& plan)
{
    return state.bearing == plan.aimBearing && state.tilt == plan.aimTilt;
}

Worked WorkBeacon(MissionState& state, const MissionPlan& plan, int control, bool carrying)
{
    if (plan.kind != MissionPlan::Kind::Beacon || state.stage != MissionState::Stage::Find)
    {
        return Worked::Nothing;
    }
    const int next = NextBeaconPart(state);
    Worked worked = Worked::Nothing;
    switch (control)
    {
    case kBeaconFit:
        if (next >= MissionSpec::kBeaconParts || !carrying)
        {
            return Worked::Nothing;
        }
        state.beacon = static_cast<uint8_t>(state.beacon | (1u << next));
        worked = Worked::Opened;
        break;
    case kBeaconBearing:
        if (next <= 1) // (its dish not on yet)
        {
            return Worked::Nothing;
        }
        state.bearing = static_cast<uint8_t>((state.bearing + 1) % MissionSpec::kAimSteps);
        worked = Worked::Switched;
        break;
    case kBeaconTilt:
        if (next <= 1)
        {
            return Worked::Nothing;
        }
        state.tilt = static_cast<uint8_t>((state.tilt + 1) % MissionSpec::kTiltSteps);
        worked = Worked::Switched;
        break;
    case kBeaconSwitch:
        if (next < MissionSpec::kBeaconParts) // (no cell in it)
        {
            return Worked::Nothing;
        }
        state.beacon = static_cast<uint8_t>(state.beacon ^ MissionState::kBeaconOn);
        worked = Worked::Switched;
        break;
    default: return Worked::Nothing;
    }
    // On and aimed: it is working, and that is the work done.
    if ((state.beacon & MissionState::kBeaconOn) != 0u && NextBeaconPart(state) >= MissionSpec::kBeaconParts && BeaconAimed(state, plan))
    {
        state.stage = MissionState::Stage::Carry;
        state.progress = 1.0f;
        return Worked::Done;
    }
    return worked;
}

Worked DrillSample(MissionState& state, const MissionPlan& plan, int site, bool carrying)
{
    if (plan.kind != MissionPlan::Kind::Samples || state.stage != MissionState::Stage::Find || site < 0 || site >= static_cast<int>(plan.markers.size()) ||
        site >= MissionSpec::kSampleSites || ((state.cored >> site) & 1u) != 0u || !carrying)
    {
        return Worked::Nothing;
    }
    const int turns = ((state.drilled >> (site * 2)) & 3) + 1;
    if (turns < MissionSpec::kDrillTurns)
    {
        state.drilled = static_cast<uint8_t>((state.drilled & ~(3u << (site * 2))) | (static_cast<unsigned>(turns) << (site * 2)));
        return Worked::Switched;
    }
    state.cored = static_cast<uint8_t>(state.cored | (1u << site));
    state.drilled = static_cast<uint8_t>(state.drilled & ~(3u << (site * 2)));
    if (std::popcount(static_cast<unsigned>(state.cored)) >= static_cast<int>(plan.markers.size()))
    {
        state.stage = MissionState::Stage::Carry;
        state.progress = 1.0f;
    }
    return Worked::Done;
}

void SpoilSamples(MissionState& state)
{
    state.cored = 0;
    state.drilled = 0;
    if (state.stage == MissionState::Stage::Carry)
    {
        state.stage = MissionState::Stage::Find;
        state.progress = 0.0f;
    }
}

bool PlantReady(const MissionState& state, const MissionPlan& plan)
{
    return !plan.chain || ((state.plant & MissionState::kFueled) != 0u && state.primed >= MissionSpec::kPrimeStrokes && state.valves == plan.lineUp);
}

Worked WorkPlant(MissionState& state, const MissionPlan& plan, int control, bool carrying)
{
    const bool live = state.stage == MissionState::Stage::Find || state.stage == MissionState::Stage::Downloading;
    if (!plan.chain || !live || control < 0 || control >= kPlantControls)
    {
        return Worked::Nothing;
    }
    Worked worked = Worked::Nothing;
    if (control == kFillerCap)
    {
        state.plant = static_cast<uint8_t>(state.plant ^ MissionState::kFillerOpen);
        worked = Worked::Opened;
    }
    else if (control == kFillerPour)
    {
        if ((state.plant & MissionState::kFillerOpen) == 0u || (state.plant & MissionState::kFueled) != 0u)
        {
            return Worked::Nothing;
        }
        // A station's scrubber: its spent cartridge out first, by hand; then the fresh one in.
        if (plan.kind == MissionPlan::Kind::Restore && (state.plant & MissionState::kSpentOut) == 0u)
        {
            if (carrying)
            {
                return Worked::Nothing; // (the slot still full)
            }
            state.plant = static_cast<uint8_t>(state.plant | MissionState::kSpentOut);
            return Worked::Opened;
        }
        if (!carrying)
        {
            return Worked::Nothing;
        }
        state.plant = static_cast<uint8_t>(state.plant | MissionState::kFueled);
        worked = Worked::Done;
    }
    else if (control == kPumpLever)
    {
        if (state.primed >= MissionSpec::kPrimeStrokes)
        {
            return Worked::Nothing;
        }
        ++state.primed;
        worked = state.primed >= MissionSpec::kPrimeStrokes ? Worked::Done : Worked::Switched;
    }
    else
    {
        state.valves = static_cast<uint8_t>(state.valves ^ (1u << (control - kValve)));
        worked = Worked::Switched;
        // Turned while the plant comes up: the lot trips, to be started again.
        if (state.stage == MissionState::Stage::Downloading)
        {
            state.device = MissionState::kConsoleTripped;
            state.progress = 0.0f;
            state.stage = MissionState::Stage::Find;
            state.attended = false;
            worked = Worked::Tripped;
        }
    }
    state.plant = static_cast<uint8_t>(state.valves == plan.lineUp ? state.plant | MissionState::kLinedUp : state.plant & ~MissionState::kLinedUp);
    return worked;
}

bool KnockMarker(MissionState& state, int marker)
{
    if (marker < 0 || marker >= state.markers)
    {
        return false;
    }
    MissionState::Marker& one = state.survey[static_cast<size_t>(marker)];
    if ((one.setup & MissionState::kPlanted) == 0u || (one.setup & MissionState::kFallen) != 0u)
    {
        return false;
    }
    // (Its legs folded under it as it went over: kicked out again once it is stood up.)
    one.setup = static_cast<uint8_t>((one.setup | MissionState::kFallen) & ~(MissionState::kLegOut * 7u));
    return true;
}

bool AllReadings(const MissionState& state, const MissionPlan& plan)
{
    return !plan.markers.empty() && std::popcount(static_cast<unsigned>(state.readings)) >= static_cast<int>(plan.markers.size());
}

bool CanDownload(const MissionState& state)
{
    return state.stage == MissionState::Stage::Find && state.powered && state.supplied;
}

bool CanRestorePower(const MissionState& state)
{
    return !state.powered && state.stage != MissionState::Stage::None && state.stage != MissionState::Stage::Over;
}

bool StartDownload(MissionState& state)
{
    // (A secured terminal not until its code has been entered.)
    if (!state.unlocked)
    {
        return false;
    }
    if (!CanDownload(state))
    {
        return false;
    }
    state.stage = MissionState::Stage::Downloading;
    state.progress = 0.0f;
    return true;
}

Worked WorkBoard(MissionState& state, const MissionPlan& plan, int control)
{
    if (!CanRestorePower(state))
    {
        return Worked::Nothing;
    }
    if (control == kBoardDoor)
    {
        if (state.boardOpen)
        {
            return Worked::Nothing;
        }
        state.boardOpen = true;
        return Worked::Opened;
    }
    const int breaker = control - kBoardBreaker;
    if (!state.boardOpen || control < kBoardMain || breaker >= MissionSpec::kBoardBreakers)
    {
        return Worked::Nothing;
    }
    const uint8_t fault = plan.boardFault >= 0 ? static_cast<uint8_t>(1u << plan.boardFault) : uint8_t{0};
    Worked worked = Worked::Switched;
    if (control == kBoardMain)
    {
        if (state.boardMain)
        {
            state.boardMain = false;
        }
        else if ((state.boardOn & fault) != 0)
        {
            worked = Worked::Tripped; // (onto a fault: it will not stay on)
        }
        else
        {
            state.boardMain = true;
        }
    }
    else
    {
        const uint8_t bit = static_cast<uint8_t>(1u << breaker);
        state.boardOn = static_cast<uint8_t>(state.boardOn ^ bit);
        if (state.boardMain && (state.boardOn & bit & fault) != 0)
        {
            state.boardMain = false; // (the fault switched in under power: the main switch goes)
            worked = Worked::Tripped;
        }
    }
    if (state.boardMain && static_cast<uint8_t>(state.boardOn | fault) == MissionSpec::kBoardAll && state.fed)
    {
        state.powered = true;
        worked = Worked::Done;
    }
    return worked;
}

Worked WorkCabinet(MissionState& state, const MissionPlan& plan, int control)
{
    if (!plan.feedCut || state.fed || state.stage == MissionState::Stage::None || state.stage == MissionState::Stage::Over || control < 0 ||
        control >= kCabinetControls)
    {
        return Worked::Nothing;
    }
    if (control == kCabinetDoor)
    {
        if (state.cabinetOpen)
        {
            return Worked::Nothing;
        }
        state.cabinetOpen = true;
        return Worked::Opened;
    }
    if (!state.cabinetOpen)
    {
        return Worked::Nothing;
    }
    if (control == kCabinetMain)
    {
        // (Into the cut cable with the standby on the bus: it trips.)
        if (!state.feedMain && state.changeover == 2)
        {
            return Worked::Tripped;
        }
        state.feedMain = !state.feedMain;
    }
    else if (control == kCabinetStandby)
    {
        state.feedStandby = !state.feedStandby;
    }
    else
    {
        const uint8_t next = state.changeover != 1 ? uint8_t{1} : state.changeoverBack ? uint8_t{0} : uint8_t{2};
        if (next == 1)
        {
            state.changeoverBack = state.changeover == 2;
        }
        if (next == 2 && state.feedMain)
        {
            state.changeover = 1; // (thrown back to off)
            state.changeoverBack = false;
            return Worked::Tripped;
        }
        state.changeover = next;
    }
    state.fed = state.changeover == 2 && state.feedStandby && !state.feedMain;
    if (!state.fed)
    {
        return Worked::Switched;
    }
    // Fed: and with the board already set, the power is back.
    const uint8_t fault = plan.boardFault >= 0 ? static_cast<uint8_t>(1u << plan.boardFault) : uint8_t{0};
    if (state.boardMain && static_cast<uint8_t>(state.boardOn | fault) == MissionSpec::kBoardAll)
    {
        state.powered = true;
    }
    return Worked::Done;
}

bool BreakerLive(const MissionState& state, int breaker)
{
    return state.fed && state.boardMain && ((state.boardOn >> breaker) & 1u) != 0u;
}

int BreakerOf(const MissionPlan& plan, size_t circuit)
{
    // The lamps come through the sound breakers only -- the faulted one feeds something else, so a building is not left dark by
    // the one breaker it has to leave off -- from a different one a seed, so they are not always the board's first few.
    int sound[MissionSpec::kBoardBreakers] = {};
    int count = 0;
    for (int b = 0; b < MissionSpec::kBoardBreakers; ++b)
    {
        if (b != plan.boardFault)
        {
            sound[count++] = b;
        }
    }
    return sound[(circuit + plan.seed) % static_cast<size_t>(count)];
}

float ConsoleCap(const MissionState& state)
{
    return static_cast<float>(std::popcount(static_cast<unsigned>(state.device & ((1u << kConsoleSystems) - 1u)))) / static_cast<float>(kConsoleSystems);
}

Worked WorkConsole(MissionState& state, const MissionPlan& plan, int system)
{
    const bool live = state.stage == MissionState::Stage::Find || state.stage == MissionState::Stage::Downloading;
    if (!plan.AtConsole() || !state.powered || !state.supplied || !live || system < 0 || system >= kConsoleSystems || !PlantReady(state, plan))
    {
        return Worked::Nothing;
    }
    const uint8_t systems = static_cast<uint8_t>(state.device & ((1u << kConsoleSystems) - 1u));
    const uint8_t bit = static_cast<uint8_t>(1u << system);
    const uint8_t before = static_cast<uint8_t>(bit - 1u); // (those before it, all on)
    const float held = static_cast<float>(system) / static_cast<float>(kConsoleSystems);
    if ((systems & bit) != 0)
    {
        // Off: it and those after it stop, and the pressure falls back to what the ones before it hold.
        state.device = static_cast<uint8_t>(systems & before);
        state.progress = std::min(state.progress, held);
        if (state.device == 0)
        {
            state.stage = MissionState::Stage::Find;
            state.attended = false;
        }
        return Worked::Switched;
    }
    if (systems != before || state.progress < held - 1e-4f)
    {
        // Out of its turn, or before the one before it is up to pressure: the lot trips.
        state.device = MissionState::kConsoleTripped;
        state.progress = 0.0f;
        state.stage = MissionState::Stage::Find;
        state.attended = false;
        return Worked::Tripped;
    }
    state.device = static_cast<uint8_t>(systems | bit);
    state.stage = MissionState::Stage::Downloading;
    return Worked::Switched;
}

Worked WorkTransmitter(MissionState& state, const MissionPlan& plan, int control)
{
    if (!plan.AtTransmitter() || state.stage != MissionState::Stage::Find || plan.markers.empty())
    {
        return Worked::Nothing;
    }
    // Brought back on the air: its lid open, its power on, and then -- powered -- its sender, and it is sending again. (The sender
    // with no power under it does nothing.)
    if (plan.kind == MissionPlan::Kind::Revive)
    {
        if (control == kCaseLid)
        {
            if ((state.device & MissionState::kLidOpen) != 0)
            {
                return Worked::Nothing;
            }
            state.device = static_cast<uint8_t>(state.device | MissionState::kLidOpen);
            return Worked::Opened;
        }
        if ((state.device & MissionState::kLidOpen) == 0)
        {
            return Worked::Nothing;
        }
        if (control == kCasePower)
        {
            state.device = static_cast<uint8_t>(state.device ^ MissionState::kPowerOff);
            // (Power pulled from under it: its sender drops out with it.)
            if ((state.device & MissionState::kPowerOff) != 0)
            {
                state.device = static_cast<uint8_t>(state.device | MissionState::kSenderOff);
            }
            return Worked::Switched;
        }
        if (control != kCaseSender || (state.device & MissionState::kPowerOff) != 0)
        {
            return Worked::Nothing;
        }
        state.device = static_cast<uint8_t>(state.device & ~MissionState::kSenderOff);
        state.readings = 1;
        state.stage = MissionState::Stage::Carry;
        return Worked::Done;
    }
    if (control == kCaseLid)
    {
        if ((state.device & MissionState::kLidOpen) != 0)
        {
            return Worked::Nothing;
        }
        state.device = static_cast<uint8_t>(state.device | MissionState::kLidOpen);
        return Worked::Opened;
    }
    if ((state.device & MissionState::kLidOpen) == 0)
    {
        return Worked::Nothing;
    }
    if (control == kCaseSender)
    {
        state.device = static_cast<uint8_t>(state.device ^ MissionState::kSenderOff);
        return Worked::Switched;
    }
    if (control != kCasePower)
    {
        return Worked::Nothing;
    }
    if ((state.device & MissionState::kSenderOff) == 0)
    {
        return Worked::Tripped; // (the power pulled from under it while it sends)
    }
    state.device = static_cast<uint8_t>(state.device | MissionState::kPowerOff);
    state.readings = 1;
    state.stage = MissionState::Stage::Carry;
    return Worked::Done;
}

bool ToggleLaunch(MissionState& state, float seconds)
{
    if (state.stage == MissionState::Stage::None || state.stage == MissionState::Stage::Over)
    {
        return false;
    }
    state.launchIn = state.Launching() ? -1.0f : std::max(seconds, 1.0f);
    return true;
}

Ticked Tick(MissionState& state, const MissionPlan& plan, float dt, bool attended)
{
    Ticked ticked;
    state.attended = attended && state.stage == MissionState::Stage::Downloading;
    // A survey's readings: each working marker's taken while enough are working at once (`attended`: they are); then back to the
    // shuttle once all are. (`progress`: how far they have all got.)
    if (plan.kind == MissionPlan::Kind::Survey)
    {
        if (state.stage == MissionState::Stage::Find && state.markers > 0)
        {
            const bool enough = MarkersWorking(state) >= MissionSpec::kMarkersNeeded;
            float sum = 0.0f;
            for (int m = 0; m < state.markers; ++m)
            {
                MissionState::Marker& one = state.survey[static_cast<size_t>(m)];
                const bool taken = ((state.readings >> m) & 1u) != 0u;
                if (enough && !taken && MarkerWorking(one))
                {
                    one.read = std::min(one.read + dt / MissionSpec::kMarkerReadSeconds, 1.0f);
                    if (one.read >= 1.0f)
                    {
                        state.readings = static_cast<uint8_t>(state.readings | (1u << m));
                        ticked.read = true;
                    }
                }
                sum += one.read;
            }
            state.progress = sum / static_cast<float>(state.markers);
            state.attended = enough;
            if (AllReadings(state, plan))
            {
                state.stage = MissionState::Stage::Carry;
                state.attended = false;
            }
        }
    }
    else if (!plan.AtTransmitter() && state.stage == MissionState::Stage::Downloading && state.attended && state.powered && state.supplied)
    {
        // (A station's restart goes only as far as what is switched on at its console allows, and waits there.)
        const float cap = plan.AtConsole() ? ConsoleCap(state) : 1.0f;
        state.progress = std::max(state.progress, std::min(state.progress + dt / std::max(plan.downloadSeconds, 1.0f), cap));
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

void Finish(MissionState& state, bool driveAboard, uint8_t aboard, bool extraAboard)
{
    state.stage = MissionState::Stage::Over;
    state.recovered = driveAboard;
    state.extra = extraAboard;
    state.aboard = aboard;
    state.launchIn = -1.0f;
    state.attended = false;
}

} // namespace MissionRules

} // namespace pred
