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
    uint32_t h = siteSeed * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    const bool other = (h & 0x100u) != 0u;
    switch (SitePlan::KindOf(siteSeed))
    {
    case SitePlan::SiteKind::Station: return Kind::Restore;
    case SitePlan::SiteKind::Survey: return Kind::Survey;
    case SitePlan::SiteKind::Signal: return other ? Kind::Revive : Kind::Signal;
    case SitePlan::SiteKind::Facility: return other ? Kind::Activate : Kind::Download;
    case SitePlan::SiteKind::Wreck: break;
    }
    return Kind::Download;
}

MissionPlan MissionPlan::Generate(const SitePlan& site, uint32_t seed)
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
    // Secured, about two data terminals in five (on a draw of their own): its access code on a note on another bench of the same
    // building, in another room -- to be searched for. (Not where there is no other bench to leave it on.)
    {
        Random secured((static_cast<uint64_t>(seed) << 20) ^ 0xC0DEull);
        if (secured.Range(0.0f, 1.0f) < 0.4f)
        {
            std::vector<const Candidate*> elsewhere;
            for (const Candidate* other : inIt)
            {
                const FacilityLayout::Fitted& at = building.fittings[other->thing];
                const int room = building.RoomAt(at.floor, static_cast<int>(std::floor(at.at.x)), static_cast<int>(std::floor(at.at.y)));
                if (other != &chosen && room != plan.room)
                {
                    elsewhere.push_back(other);
                }
            }
            if (!elsewhere.empty())
            {
                const FacilityLayout::Fitted& noteOn = building.fittings[elsewhere[secured.Index(elsewhere.size())]->thing];
                FacilityLayout::Placed other;
                other.floor = noteOn.floor;
                other.at = noteOn.at;
                other.yaw = noteOn.yaw;
                const glm::vec3 otherSize = FacilityLayout::FittingSize(noteOn.kind);
                plan.clue = FacilityMap::PlacedAgainstWall(building, other, otherSize.y) + glm::vec3(0.0f, otherSize.z + 0.002f, 0.0f);
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
    return plan;
}

namespace MissionRules
{

MissionState Start(const MissionPlan& plan)
{
    MissionState state;
    state.stage = plan.Valid() ? MissionState::Stage::Find : MissionState::Stage::None;
    state.powered = !plan.powerOut;
    // The board as it is found: with the power out, its main switch gone and some of its breakers with it.
    state.boardMain = state.powered;
    state.boardOn = state.powered ? MissionSpec::kBoardAll : static_cast<uint8_t>(MissionSpec::kBoardAll & ~plan.boardTripped);
    state.unlocked = !plan.Secured();
    state.supplied = !plan.Faulted();
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
    if (state.boardMain && static_cast<uint8_t>(state.boardOn | fault) == MissionSpec::kBoardAll)
    {
        state.powered = true;
        worked = Worked::Done;
    }
    return worked;
}

bool BreakerLive(const MissionState& state, int breaker)
{
    return state.boardMain && ((state.boardOn >> breaker) & 1u) != 0u;
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
    if (!plan.AtConsole() || !state.powered || !state.supplied || !live || system < 0 || system >= kConsoleSystems)
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
    if (plan.AtMarkers())
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
    else if (state.stage == MissionState::Stage::Downloading && state.attended && state.powered && state.supplied)
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
