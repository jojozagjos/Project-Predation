#include "Game/World/SitePlan.h"

#include "Game/World/FacilityMap.h"

#include "Engine/Render/Sdf.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <queue>

namespace pred
{
namespace
{

// SplitMix64, as the buildings are planned with: the same sequence on every machine.
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
    int Int(int low, int high) { return low + static_cast<int>(Next() % static_cast<uint64_t>(high - low + 1)); }
    bool Chance(float p) { return Unit() < p; }

private:
    uint64_t m_state;
};

uint32_t Mix(uint32_t seed, uint32_t salt)
{
    uint32_t h = seed * 0x9E3779B1u ^ (salt + 0x7F4A7C15u);
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

using Kind = SitePlan::BlockKind;
using Mood = FacilityLayout::LampMood;
constexpr float kCell = FacilityLayout::kCell;

// How the outside can be, one picked per site: always night, and never much to see by.
const SitePlan::Sky kSkies[] = {
    // A low moon through thin cloud: shapes at a distance, no colour.
    {{-0.35f, -0.45f, -0.55f}, {0.55f, 0.62f, 0.8f}, 0.16f, {0.025f, 0.03f, 0.042f}, {0.01f, 0.01f, 0.013f},
     {0.025f, 0.03f, 0.038f}, 8.0f, 85.0f},
    // Overcast and moonless: the floodlights are all there is.
    {{0.2f, -0.8f, 0.3f}, {0.4f, 0.45f, 0.55f}, 0.05f, {0.012f, 0.014f, 0.018f}, {0.006f, 0.006f, 0.007f},
     {0.012f, 0.014f, 0.016f}, 5.0f, 55.0f},
    // Haze, blowing: close fog, lit a little from somewhere above it.
    {{-0.1f, -0.7f, 0.5f}, {0.5f, 0.55f, 0.6f}, 0.1f, {0.03f, 0.033f, 0.038f}, {0.014f, 0.014f, 0.016f},
     {0.045f, 0.048f, 0.052f}, 3.0f, 38.0f},
};

// How a building of a kind of place looks from outside -- the first, the main one, with what that kind of place is known by
// (a dome, stacks, panels, dishes); the rest of the same family, plainer. Each a shade lighter or darker than the next, on a
// draw of its own.
FacilityLayout::Outside OutsideOf(SitePlan::FacilityType type, uint32_t seed, int building)
{
    using Type = SitePlan::FacilityType;
    using Windows = FacilityLayout::Outside::Windows;
    Random random(Mix(seed, 0x0D75u + static_cast<uint32_t>(building)));
    const bool main = building == 0;
    FacilityLayout::Outside look;
    const auto colours = [&](glm::vec3 cladding, glm::vec3 trim, glm::vec3 accent, const char* surface)
    {
        look.cladding = cladding * random.Range(0.88f, 1.1f);
        look.trim = trim;
        look.accent = accent;
        look.surface = surface;
    };
    switch (type)
    {
    case Type::None: break;
    case Type::ResearchLab:
        // Clean and pale: glass along every floor, bands, a teal stripe round the top, canopies; plant on the roof.
        colours({0.62f, 0.64f, 0.66f}, {0.2f, 0.22f, 0.24f}, {0.08f, 0.42f, 0.48f}, "building_cladding");
        look.windows = Windows::Strip;
        look.bands = look.stripe = look.canopies = look.roofing = true;
        look.units = main ? random.Int(2, 4) : random.Int(1, 2);
        look.vents = random.Int(1, 3);
        look.antenna = main && random.Unit() < 0.5f;
        break;
    case Type::Mining:
        // Weathered grey corrugated sheds, ribbed, few windows, a hazard-yellow stripe; vents, and a stack on the main one.
        colours({0.46f, 0.44f, 0.4f}, {0.2f, 0.19f, 0.18f}, {0.78f, 0.6f, 0.08f}, "shed_roof");
        look.windows = Windows::Punched;
        look.windowEvery = 2;
        look.ribEvery = 2;
        look.stripe = true;
        look.vents = random.Int(2, 4);
        look.stacks = main ? 1 : 0;
        look.units = 1;
        break;
    case Type::Refinery:
        // Grey-green, ribbed, the main one windowless; stacks banded red, and vents.
        colours({0.32f, 0.36f, 0.33f}, {0.22f, 0.22f, 0.21f}, {0.62f, 0.14f, 0.1f}, "building_cladding");
        look.windows = main ? Windows::None : Windows::Punched;
        look.windowEvery = 2;
        look.ribEvery = 3;
        look.stripe = true;
        look.stacks = main ? random.Int(2, 3) : random.Int(0, 1);
        look.vents = random.Int(3, 5);
        look.units = random.Int(1, 2);
        break;
    case Type::CargoDepot:
        // Blue corrugated sheds, white trim, an orange stripe; hardly a window, canopies over the doors.
        colours({0.17f, 0.28f, 0.44f}, {0.72f, 0.72f, 0.7f}, {0.82f, 0.42f, 0.1f}, "shed_roof");
        look.windows = main ? Windows::None : Windows::Punched;
        look.windowEvery = 3;
        look.ribEvery = 2;
        look.stripe = look.canopies = look.roofing = true;
        look.units = 1;
        look.vents = random.Int(1, 2);
        break;
    case Type::Habitat:
        // Warm white panels, tall windows, bands, canopies; panels on the roof.
        colours({0.7f, 0.66f, 0.58f}, {0.3f, 0.27f, 0.24f}, {0.24f, 0.48f, 0.32f}, "habitat_shell");
        look.windows = Windows::Tall;
        look.bands = look.canopies = look.roofing = true;
        look.solar = main || random.Unit() < 0.5f;
        look.units = random.Int(1, 2);
        look.vents = random.Int(1, 2);
        break;
    case Type::CommsRelay:
        // Dark, a window to every bay, bands and a red stripe; a mast on every roof, and dishes.
        colours({0.2f, 0.21f, 0.23f}, {0.52f, 0.54f, 0.56f}, {0.72f, 0.18f, 0.14f}, "building_cladding");
        look.windows = Windows::Punched;
        look.bands = look.stripe = true;
        look.antenna = true;
        look.dishes = main ? random.Int(2, 3) : 1;
        look.units = random.Int(1, 2);
        break;
    case Type::Observatory:
        // White panels, glass along its floors, a navy stripe; the dome on the main one, a dish on the rest.
        colours({0.74f, 0.75f, 0.76f}, {0.24f, 0.26f, 0.3f}, {0.14f, 0.22f, 0.46f}, "habitat_shell");
        look.windows = Windows::Strip;
        look.bands = look.stripe = look.canopies = look.roofing = true;
        look.dome = main;
        look.dishes = main ? 0 : 1;
        look.antenna = !main;
        look.units = 1;
        break;
    case Type::SurveyCamp:
        // Olive sheds, ribbed, a window to every bay, an orange stripe; panels and a mast.
        colours({0.38f, 0.38f, 0.27f}, {0.2f, 0.2f, 0.16f}, {0.78f, 0.44f, 0.12f}, "shed_roof");
        look.windows = Windows::Punched;
        look.ribEvery = 2;
        look.stripe = true;
        look.solar = look.antenna = true;
        look.vents = 1;
        break;
    }
    return look;
}

// A building's shape, by what the place is for: the main one with the shapes that kind of place is built in -- a lab's wings
// and courtyards, a refinery's or a relay's tower over a lower hall, a habitat's cross or H -- the rest mostly simpler. On a
// draw of its own.
FacilityLayout::Massing MassingOf(SitePlan::FacilityType type, uint32_t seed, int building)
{
    using Type = SitePlan::FacilityType;
    using M = FacilityLayout::Massing;
    Random random(Mix(seed, 0x5A9Eu + static_cast<uint32_t>(building)));
    const bool main = building == 0;
    const auto pick = [&](std::initializer_list<M> from) { return *(from.begin() + random.Int(0, static_cast<int>(from.size()) - 1)); };
    switch (type)
    {
    case Type::None: return M::Block;
    case Type::ResearchLab: return main ? pick({M::L, M::U, M::H, M::T, M::Court, M::Stepped}) : pick({M::L, M::T, M::Block, M::Stepped});
    case Type::Mining: return main ? pick({M::Tower, M::L, M::Stepped}) : pick({M::Block, M::L, M::Tower});
    case Type::Refinery: return main ? pick({M::Tower, M::Stepped, M::T}) : pick({M::Block, M::L, M::Tower});
    case Type::CargoDepot: return main ? pick({M::Stepped, M::L, M::T}) : pick({M::Block, M::L});
    case Type::Habitat: return main ? pick({M::Cross, M::H, M::U, M::Court}) : pick({M::L, M::T, M::Cross});
    case Type::CommsRelay: return main ? pick({M::Tower, M::T, M::L}) : pick({M::Block, M::L, M::Tower});
    case Type::Observatory: return main ? pick({M::Cross, M::T, M::Stepped}) : pick({M::L, M::Block});
    case Type::SurveyCamp: return pick({M::L, M::T, M::Block});
    }
    return M::Block;
}

// What a kind of place's buildings are for -- which rooms they have (FacilityLayout::Program).
FacilityLayout::Program ProgramOf(SitePlan::FacilityType type)
{
    using Type = SitePlan::FacilityType;
    using G = FacilityLayout::Program;
    switch (type)
    {
    case Type::ResearchLab: return G::Research;
    case Type::Mining:
    case Type::Refinery: return G::Industrial;
    case Type::CargoDepot: return G::Depot;
    case Type::Habitat: return G::Living;
    case Type::CommsRelay: return G::Comms;
    case Type::Observatory: return G::Observatory;
    case Type::SurveyCamp: return G::Field;
    case Type::None: return G::Generic;
    }
    return G::Generic;
}

// Towards the outside, from a way out of a building.
glm::vec2 Outward(int side)
{
    return side == 0 ? glm::vec2(-1.0f, 0.0f) : side == 1 ? glm::vec2(1.0f, 0.0f) : side == 2 ? glm::vec2(0.0f, -1.0f) : glm::vec2(0.0f, 1.0f);
}

// A point just outside a way out, `distance` metres beyond the wall, on the ground.
glm::vec3 ExitPoint(const FacilityLayout& building, const FacilityLayout::Exit& exit, float distance)
{
    const glm::vec2 out = Outward(exit.side);
    const glm::vec2 cells = glm::vec2(exit.cell) + glm::vec2(0.5f) + out * (0.5f + distance / kCell);
    return building.origin + glm::vec3(cells.x * kCell, building.GroundLevel() - building.origin.y, cells.y * kCell);
}

Mood PickMood(Random& random, float working)
{
    const float roll = random.Unit();
    if (roll < working)
    {
        return Mood::Steady;
    }
    const float rest = (roll - working) / std::max(1.0f - working, 1.0e-3f);
    return rest < 0.45f ? Mood::Flicker : rest < 0.75f ? Mood::Failing : Mood::Dead;
}

// How far a point is from a rectangle on the ground; nought inside it.
float DistanceTo(glm::vec2 p, glm::vec2 min, glm::vec2 max)
{
    const glm::vec2 nearest = glm::clamp(p, min, max);
    return glm::length(p - nearest);
}


// What has become of a building (FacilityLayout::Condition), on a draw of its own so nothing else on the site comes out any
// different for it: and with it, its lamps -- left, most of them dead and the rest failing; burnt, nearly all dead -- and
// what is in it -- left, much of it taken away; burnt, some of it gone. Never the bench a mission's terminal stands on.
FacilityLayout::Condition ConditionOf(Random& random)
{
    using Condition = FacilityLayout::Condition;
    const float roll = random.Unit();
    return roll < 0.35f ? Condition::Kept : roll < 0.65f ? Condition::Abandoned : roll < 0.87f ? Condition::Hurried : Condition::Burnt;
}

// What will become of the building `index` on a site (Neglect's draw, ahead of it): for what is planned by it before the building is.
FacilityLayout::Condition ConditionOf(uint32_t seed, int index)
{
    Random random(Mix(seed, 0xC07Du + static_cast<uint32_t>(index)));
    return ConditionOf(random);
}

void Neglect(FacilityLayout& building, uint32_t seed, int index)
{
    using Condition = FacilityLayout::Condition;
    using Mood = FacilityLayout::LampMood;
    Random random(Mix(seed, 0xC07Du + static_cast<uint32_t>(index)));
    building.condition = ConditionOf(random);
    const float dead = building.condition == Condition::Abandoned ? 0.55f : building.condition == Condition::Burnt ? 0.75f
                       : building.condition == Condition::Hurried ? 0.15f : 0.0f;
    for (FacilityLayout::Lamp& lamp : building.lamps)
    {
        if (lamp.mood == Mood::Emergency || dead <= 0.0f)
        {
            continue;
        }
        const float lampRoll = random.Unit();
        lamp.mood = lampRoll < dead ? Mood::Dead : lampRoll < dead + 0.15f ? (random.Chance(0.5f) ? Mood::Flicker : Mood::Failing) : lamp.mood;
    }
    const float taken = building.condition == Condition::Abandoned ? 0.4f : building.condition == Condition::Burnt ? 0.25f : 0.0f;
    if (taken > 0.0f)
    {
        std::vector<FacilityLayout::Fitted> kept;
        for (const FacilityLayout::Fitted& fitted : building.fittings)
        {
            if (fitted.kind == FacilityLayout::Fitting::Terminal || !random.Chance(taken))
            {
                kept.push_back(fitted);
            }
        }
        building.fittings = std::move(kept);
    }
}

// A cave from a mine's way into the rock (FacilityLayout::caves) -- a big one (you, 2026-11-03: caves can be huge): the adit straight
// out from the doorway, then a long way on through the rock, each passage on from the last, turning, going down far more than up,
// its floor flat across and never a step where two meet; long branches off it; chambers opening out along it, and a cavern or two
// where it has gone deep enough for one. Its roof always well under the ground over it (and SiteMap keeps it so wherever the land
// dips); clear of every building and inside the site. On a draw of its own.
// Whether something this wide can stand at `at` in a cave: clear of every building by a few metres, inside the site.
bool CaveClear(const SitePlan& plan, const glm::vec3& at, float radius)
{
    const glm::vec2 xz{at.x, at.z};
    if (xz.x < plan.origin.x + radius + 8.0f || xz.y < plan.origin.z + radius + 8.0f || xz.x > plan.origin.x + plan.size - radius - 8.0f ||
        xz.y > plan.origin.z + plan.size - radius - 8.0f)
    {
        return false;
    }
    for (const FacilityLayout& other : plan.buildings)
    {
        glm::vec2 min;
        glm::vec2 max;
        SitePlan::Footprint(other, radius + 3.0f, min, max);
        if (xz.x > min.x && xz.x < max.x && xz.y > min.y && xz.y < max.y && at.y > other.origin.y - radius - 2.0f)
        {
            return false;
        }
    }
    return true;
}

// At the least, rock between a cave's roof and the ground.
constexpr float kCaveCover = 4.0f;
// A passage's radius, held so its roof stays under the ground (`surface`) with rock to spare.
float CaveRoom(float surface, float level, float want)
{
    return std::clamp(want, 1.4f, std::max(1.4f, (surface - kCaveCover - level) / 1.55f));
}

// The adit out of a building's doorway into the rock: from the doorway's outside face, straight out.
SitePlan::Cave::Adit AditOf(const FacilityLayout& building, int index, const FacilityLayout::Exit& exit)
{
    SitePlan::Cave::Adit adit;
    adit.building = index;
    const glm::vec2 out = Outward(exit.side);
    adit.out = glm::vec3(out.x, 0.0f, out.y);
    const glm::vec3 line = FacilityMap::ToWorld(building, 0, glm::vec2(exit.cell) + glm::vec2(0.5f) + out * 0.5f);
    adit.mouth = line + adit.out * (FacilitySpec::kShellThickness + FacilitySpec::kCladding - FacilitySpec::kHalfWall);
    return adit;
}

// The first passage on from an adit's end, straight on out of it, its floor the adit's, so the one walks on into the other -- its
// rounded end just reaching back to the adit's end, where the two run together.
SitePlan::Cave::Passage PassageFrom(const SitePlan::Cave::Adit& adit, float radius)
{
    const glm::vec3 a = adit.mouth + adit.out * (adit.length + radius * 0.9f) + glm::vec3(0.0f, radius * 0.55f, 0.0f);
    return {a, a + adit.out * 1.2f, radius};
}

// A cave's lowest floor, and the box it all lies in (its adits too, from a little behind their mouths).
void CaveBounds(SitePlan::Cave& cave)
{
    cave.bottom = cave.mouth.y;
    cave.min = cave.mouth;
    cave.max = cave.mouth;
    for (const SitePlan::Cave::Passage& passage : cave.passages)
    {
        cave.bottom = std::min({cave.bottom, passage.a.y - passage.radius * 0.55f, passage.b.y - passage.radius * 0.55f});
        cave.min = glm::min(cave.min, glm::min(passage.a, passage.b) - glm::vec3(passage.radius + 1.5f));
        cave.max = glm::max(cave.max, glm::max(passage.a, passage.b) + glm::vec3(passage.radius + 1.5f));
    }
    for (const glm::vec4& each : cave.chambers)
    {
        cave.bottom = std::min(cave.bottom, each.y - each.w * 0.45f);
        cave.min = glm::min(cave.min, glm::vec3(each) - glm::vec3(each.w + 1.5f));
        cave.max = glm::max(cave.max, glm::vec3(each) + glm::vec3(each.w + 1.5f));
    }
    const auto adit = [&](const SitePlan::Cave::Adit& each)
    {
        const glm::vec3 across{-each.out.z, 0.0f, each.out.x};
        for (const glm::vec3 corner : {each.mouth - each.out * 1.8f + across * 2.6f + glm::vec3(0.0f, -0.8f, 0.0f),
                                       each.mouth + each.out * (each.length + 1.5f) - across * 2.6f + glm::vec3(0.0f, SitePlan::Cave::kAditHigh + 1.0f, 0.0f)})
        {
            cave.min = glm::min(cave.min, corner);
            cave.max = glm::max(cave.max, corner);
        }
        cave.bottom = std::min(cave.bottom, each.mouth.y);
    };
    adit(cave.First());
    for (const SitePlan::Cave::Adit& each : cave.joins)
    {
        adit(each);
    }
}

SitePlan::Cave PlanCave(const SitePlan& plan, int index, const FacilityLayout::Exit& exit, uint32_t seed)
{
    const FacilityLayout& building = plan.buildings[static_cast<size_t>(index)];
    SitePlan::Cave cave;
    cave.building = index;
    cave.seed = seed;
    {
        const SitePlan::Cave::Adit adit = AditOf(building, index, exit);
        cave.out = adit.out;
        cave.mouth = adit.mouth;
    }
    Random random(seed);
    const float surface = building.GroundLevel(); // the ground over it all, near enough (SiteMap keeps the roof under the real ground)
    constexpr float kCover = kCaveCover;
    const float floor = cave.mouth.y;
    const auto clear = [&](const glm::vec3& at, float radius) { return CaveClear(plan, at, radius); };
    const auto room = [&](float level, float want) { return CaveRoom(surface, level, want); };
    // A chamber's reach, likewise (its roof its reach over its floor).
    const auto wide = [&](float level, float want) { return std::min(want, surface - kCover - level); };
    const auto chamber = [&](const glm::vec3& at, float level, float want)
    {
        const float reach = wide(level, want);
        if (reach >= 4.0f && clear(glm::vec3(at.x, level, at.z), reach))
        {
            cave.chambers.push_back({at.x, level + reach * 0.45f, at.z, reach});
            return true;
        }
        return false;
    };
    const auto addPassage = [&](glm::vec3 from, float at, float heading, int count, float spread, float fall)
    {
        for (int k = 0; k < count; ++k)
        {
            bool placed = false;
            for (int attempt = 0; attempt < 10 && !placed; ++attempt)
            {
                // (Turning harder each time it cannot go on: round whatever is in the way, back on itself at the last.)
                const float turn = heading + random.Range(-spread, spread) * (1.0f + static_cast<float>(attempt) * 0.45f);
                const float length = random.Range(8.0f, 16.0f);
                const float next = std::clamp(at + random.Range(-fall, 0.8f), floor - 22.0f, floor);
                const float radius = room(std::max(at, next), random.Range(2.0f, 3.4f));
                const glm::vec3 a{from.x, at + radius * 0.55f, from.z};
                const glm::vec3 b{from.x + std::sin(turn) * length, next + radius * 0.55f, from.z + std::cos(turn) * length};
                if (!clear(b, radius) || !clear((a + b) * 0.5f, radius))
                {
                    continue;
                }
                cave.passages.push_back({a, b, radius});
                heading = turn;
                from = glm::vec3(b.x, next, b.z);
                at = next;
                placed = true;
                // Opening out where it has come to, now and then.
                if (random.Chance(0.25f))
                {
                    chamber(from, at, random.Range(5.0f, 9.0f));
                }
            }
            if (!placed)
            {
                return;
            }
        }
    };
    // The way on, from the adit's end.
    cave.passages.push_back(PassageFrom(cave.First(), 2.2f));
    const glm::vec3 start = cave.passages.back().b - glm::vec3(0.0f, 2.2f * 0.55f, 0.0f);
    const float heading = std::atan2(cave.out.x, cave.out.z);
    const size_t trunk = cave.passages.size();
    addPassage(glm::vec3(start.x, floor, start.z), floor, heading, random.Int(14, 22), 0.6f, 2.2f);
    const size_t made = cave.passages.size();
    // Long branches off it.
    for (int branch = random.Int(3, 6); branch > 0 && made > trunk + 2; --branch)
    {
        const SitePlan::Cave::Passage& from = cave.passages[static_cast<size_t>(random.Int(static_cast<int>(trunk) + 1, static_cast<int>(made) - 1))];
        const float off = std::atan2(from.b.x - from.a.x, from.b.z - from.a.z) + (random.Chance(0.5f) ? 1.0f : -1.0f) * random.Range(0.9f, 1.6f);
        const float level = from.b.y - from.radius * 0.55f;
        addPassage(glm::vec3(from.b.x, level, from.b.z), level, off, random.Int(3, 7), 0.6f, 1.6f);
    }
    // A cavern or two, where the way on has gone deep enough: the deepest places first.
    std::vector<size_t> deepest;
    for (size_t p = trunk; p < cave.passages.size(); ++p)
    {
        deepest.push_back(p);
    }
    std::sort(deepest.begin(), deepest.end(), [&](size_t a, size_t b) { return cave.passages[a].b.y < cave.passages[b].b.y; });
    int caverns = random.Int(1, 2);
    for (size_t p : deepest)
    {
        if (caverns == 0)
        {
            break;
        }
        const SitePlan::Cave::Passage& at = cave.passages[p];
        const float level = at.b.y - at.radius * 0.55f;
        if (chamber(at.b, level, random.Range(10.0f, 16.0f)))
        {
            --caverns;
        }
    }
    // And a chamber at the far end of the way on, to come to.
    if (made > trunk)
    {
        const SitePlan::Cave::Passage& last = cave.passages[made - 1];
        chamber(last.b, last.b.y - last.radius * 0.55f, random.Range(6.0f, 9.0f));
    }
    // Its lowest floor; and now and then the water standing in it, filling its low ground to a metre or two and more -- where the
    // roof comes down under it, a sump to swim through -- never up to the adit. On a draw of its own.
    CaveBounds(cave);
    Random flooding(Mix(seed, 0xF100Du));
    if (flooding.Chance(0.4f) && cave.bottom < floor - 1.6f)
    {
        cave.water = std::min(cave.bottom + flooding.Range(1.5f, 4.5f), floor - 0.6f);
    }
    return cave;
}

// Another building joined to a cave (you, 2026-11-03: several buildings can connect to one cave): its adit out of its lowest floor
// (`exit`), then a winding way on through the rock towards the nearest part of the cave, going down or up to it as it goes, until it
// is near enough to run straight in. False, and the cave as it was, where no such way can be found.
bool JoinCave(SitePlan::Cave& cave, const SitePlan& plan, int index, const FacilityLayout::Exit& exit, uint32_t seed)
{
    const FacilityLayout& building = plan.buildings[static_cast<size_t>(index)];
    const SitePlan::Cave::Adit adit = AditOf(building, index, exit);
    Random random(seed);
    const float surface = building.GroundLevel();
    std::vector<SitePlan::Cave::Passage> made{PassageFrom(adit, 2.0f)};
    glm::vec3 from = made.back().b - glm::vec3(0.0f, 2.0f * 0.55f, 0.0f); // on its floor
    // Into the nearest part of the cave out past its first passage: a passage's far end, on its floor.
    glm::vec3 target{0.0f};
    float best = 1.0e9f;
    for (size_t p = 1; p < cave.passages.size(); ++p)
    {
        const SitePlan::Cave::Passage& passage = cave.passages[p];
        const glm::vec3 floorAt = passage.b - glm::vec3(0.0f, passage.radius * 0.55f, 0.0f);
        const float away = glm::length(glm::vec2(floorAt.x - from.x, floorAt.z - from.z)) + std::abs(floorAt.y - from.y) * 2.0f;
        if (away < best && (cave.water < cave.bottom || floorAt.y > cave.water + 0.5f))
        {
            best = away;
            target = floorAt;
        }
    }
    if (best > 1.0e8f)
    {
        return false;
    }
    for (int k = 0; k < 14; ++k)
    {
        const glm::vec2 toward{target.x - from.x, target.z - from.z};
        const float left = glm::length(toward);
        // Near enough, and not too steep: straight in.
        if (left < 15.0f && std::abs(target.y - from.y) < left * 0.3f + 0.3f)
        {
            const float radius = CaveRoom(surface, std::max(from.y, target.y), 2.0f);
            made.push_back({from + glm::vec3(0.0f, radius * 0.55f, 0.0f), target + glm::vec3(0.0f, radius * 0.55f, 0.0f), radius});
            cave.joins.push_back(adit);
            cave.passages.insert(cave.passages.end(), made.begin(), made.end());
            CaveBounds(cave);
            // (Its water never up to this adit either.)
            cave.water = std::min(cave.water, adit.mouth.y - 0.6f);
            return true;
        }
        bool placed = false;
        for (int attempt = 0; attempt < 8 && !placed; ++attempt)
        {
            const float heading = std::atan2(toward.x, toward.y) + random.Range(-0.5f, 0.5f) * (1.0f + static_cast<float>(attempt) * 0.5f);
            const float length = std::min(random.Range(8.0f, 14.0f), left);
            const float next = from.y + std::clamp(target.y - from.y, -2.2f, 1.2f) * std::min(1.0f, length / std::max(left, 1.0f) * 2.0f);
            const float radius = CaveRoom(surface, std::max(from.y, next), random.Range(1.8f, 2.6f));
            const glm::vec3 a = from + glm::vec3(0.0f, radius * 0.55f, 0.0f);
            const glm::vec3 b{from.x + std::sin(heading) * length, next + radius * 0.55f, from.z + std::cos(heading) * length};
            if (!CaveClear(plan, b, radius) || !CaveClear(plan, (a + b) * 0.5f, radius))
            {
                continue;
            }
            made.push_back({a, b, radius});
            from = glm::vec3(b.x, next, b.z);
            placed = true;
        }
        if (!placed)
        {
            return false;
        }
    }
    return false;
}
} // namespace

void SitePlan::Footprint(const FacilityLayout& building, float margin, glm::vec2& min, glm::vec2& max)
{
    min = glm::vec2(building.origin.x, building.origin.z) - glm::vec2(margin);
    max = glm::vec2(building.origin.x + static_cast<float>(building.width) * kCell,
                    building.origin.z + static_cast<float>(building.depth) * kCell) +
          glm::vec2(margin);
}

bool SitePlan::InBuilding(glm::vec2 xz, float margin) const
{
    for (const FacilityLayout& building : buildings)
    {
        glm::vec2 min;
        glm::vec2 max;
        Footprint(building, margin, min, max);
        if (xz.x >= min.x && xz.x <= max.x && xz.y >= min.y && xz.y <= max.y)
        {
            return true;
        }
    }
    for (const glm::vec4& rect : levelled)
    {
        if (xz.x >= rect.x - margin && xz.x <= rect.z + margin && xz.y >= rect.y - margin && xz.y <= rect.w + margin)
        {
            return true;
        }
    }
    return false;
}


float SitePlan::Cave::Distance(const glm::vec3& at) const
{
    // Each adit a straight drive, flat-floored and square, as dug; each passage a tube along its way; each chamber a dome lower
    // than it is wide; run together where they meet. Floored flat across a little over half a passage's height under its middle
    // (a chamber's likewise), the floor running smoothly from one part's to the next's -- each part's floor counted by how far in
    // it a place is. (Each cut by its own floor and then run together, the floors dipped where two met -- by a third of a metre
    // where two passages joined, under a chamber's middle -- and what stood there was left in the air.) The rock's face broken up,
    // its walls and roof, not its floor; an adit's only a little, and the cave's coming in past its end. What is far off left out:
    // a cave is many parts, and the surface is asked for a great many times.
    constexpr float kBlend = 1.2f;
    float shapes = 1.0e9f;
    float weights = 0.0f;
    float levels = 0.0f;
    float dug = 0.0f; // how much of an adit's own a place is (its rock barely broken)
    const auto join = [&](float part, float level)
    {
        shapes = shapes > 1.0e8f ? part : Sdf::SmoothUnion(shapes, part, kBlend);
        const float weight = std::exp(-std::clamp(part, -8.0f, 20.0f) * 3.0f);
        weights += weight;
        levels += weight * level;
    };
    const auto drive = [&](const Adit& drift)
    {
        const glm::vec3 local = at - drift.mouth;
        const float along = glm::dot(local, drift.out);
        const float across = local.x * drift.out.z - local.z * drift.out.x;
        // From a little behind its mouth (in the building's wall: cut away where the cave's surface is made) to a little past its end.
        constexpr float kFrom = -1.5f;
        const float to = drift.length + 0.8f;
        const glm::vec3 q = glm::abs(glm::vec3(along - (kFrom + to) * 0.5f, local.y - kAditHigh * 0.5f, across)) - glm::vec3((to - kFrom) * 0.5f, kAditHigh * 0.5f, kAditHalf);
        const float box = glm::length(glm::max(q, glm::vec3(0.0f))) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
        join(box, drift.mouth.y);
        if (box < 2.0f)
        {
            dug = std::max(dug, 1.0f - glm::smoothstep(drift.length - 0.5f, drift.length + 2.5f, along));
        }
    };
    drive(First());
    for (const Adit& other : joins)
    {
        drive(other);
    }
    for (const Passage& passage : passages)
    {
        const glm::vec3 run = passage.b - passage.a;
        const float half = glm::length(run) * 0.5f;
        if (glm::length(at - (passage.a + passage.b) * 0.5f) - half - passage.radius > std::min(shapes, 1.0e8f) + kBlend * 2.0f)
        {
            continue;
        }
        const float t = std::clamp(glm::dot(at - passage.a, run) / std::max(glm::dot(run, run), 1.0e-6f), 0.0f, 1.0f);
        const glm::vec3 middle = passage.a + run * t;
        join(glm::length(at - middle) - passage.radius, middle.y - passage.radius * 0.55f);
    }
    for (const glm::vec4& chamber : chambers)
    {
        const glm::vec3 middle{chamber};
        if (glm::length(at - middle) - chamber.w > std::min(shapes, 1.0e8f) + kBlend * 2.0f)
        {
            continue;
        }
        join(Sdf::Ellipsoid(at - middle, glm::vec3(chamber.w, chamber.w * 0.55f, chamber.w)), middle.y - chamber.w * 0.45f);
    }
    const float floorAt = weights > 1.0e-30f ? levels / weights : at.y - 100.0f;
    const float rough = (Sdf::Fbm(at * 0.3f, seed, 3) - 0.5f) * 1.1f * (1.0f - 0.85f * dug);
    return std::max(shapes, floorAt - at.y) + rough * glm::smoothstep(0.15f, 0.9f, at.y - floorAt);
}

float SitePlan::Cave::FloorUnder(const glm::vec3& at) const
{
    // Down from a little over it, through the open space, to where the rock begins; then closer, halving.
    float open = at.y + 0.6f;
    if (Distance({at.x, open, at.z}) >= 0.0f)
    {
        return at.y;
    }
    float rock = open;
    for (int step = 0; step < 120; ++step)
    {
        rock -= 0.05f;
        if (Distance({at.x, rock, at.z}) >= 0.0f)
        {
            break;
        }
        open = rock;
        if (step == 119)
        {
            return at.y;
        }
    }
    for (int k = 0; k < 10; ++k)
    {
        const float middle = (open + rock) * 0.5f;
        (Distance({at.x, middle, at.z}) < 0.0f ? open : rock) = middle;
    }
    return (open + rock) * 0.5f;
}
bool SitePlan::Indoors(const glm::vec3& at, float margin) const
{
    for (const FacilityLayout& building : buildings)
    {
        glm::vec2 min;
        glm::vec2 max;
        Footprint(building, margin, min, max);
        if (at.x <= min.x || at.x >= max.x || at.z <= min.y || at.z >= max.y || at.y <= building.origin.y - 0.5f - margin)
        {
            continue;
        }
        // Under the roof of whatever part of it is there (or within `margin` of one): not in a courtyard, nor beside a wing,
        // nor over a lower roof.
        const glm::vec2 local = (glm::vec2(at.x, at.z) - glm::vec2(building.origin.x, building.origin.z)) / kCell;
        const float reach = margin / kCell;
        int highest = 0;
        for (int z = static_cast<int>(std::floor(local.y - reach)); z <= static_cast<int>(std::floor(local.y + reach)); ++z)
        {
            for (int x = static_cast<int>(std::floor(local.x - reach)); x <= static_cast<int>(std::floor(local.x + reach)); ++x)
            {
                highest = std::max(highest, building.Height(x, z));
            }
        }
        const float roof = building.origin.y + static_cast<float>(highest) * FacilityLayout::kStorey - FacilityLayout::kSlab;
        if (highest > 0 && at.y < roof + margin)
        {
            return true;
        }
    }
    // Or in a passage under the ground between two of them (Tunnel): its cells of the site's grid, from its floor up to the ground.
    for (const Tunnel& tunnel : tunnels)
    {
        if (at.y < tunnel.floor - 0.5f - margin || at.y > tunnel.floor + FacilityLayout::kStorey + margin)
        {
            continue;
        }
        const glm::vec2 local = (glm::vec2(at.x, at.z) - glm::vec2(origin.x, origin.z)) / kCell;
        const float reach = margin / kCell;
        for (const glm::ivec2 cell : tunnel.cells)
        {
            if (local.x >= static_cast<float>(cell.x) - reach && local.x <= static_cast<float>(cell.x + 1) + reach &&
                local.y >= static_cast<float>(cell.y) - reach && local.y <= static_cast<float>(cell.y + 1) + reach)
            {
                return true;
            }
        }
    }
    // Or down a cave (Cave), or in an adit out to it.
    for (const Cave& cave : caves)
    {
        const auto inAdit = [&](const Cave::Adit& adit)
        {
            const glm::vec3 local = at - adit.mouth;
            const float along = glm::dot(local, adit.out);
            const float across = std::abs(local.x * adit.out.z - local.z * adit.out.x);
            return along > -0.2f - margin && along < adit.length + 0.5f + margin && across < 1.4f + margin && local.y > -0.5f - margin && local.y < 3.0f + margin;
        };
        if (inAdit(cave.First()) || std::any_of(cave.joins.begin(), cave.joins.end(), inAdit))
        {
            return true;
        }
        if (glm::all(glm::greaterThan(at, cave.min - glm::vec3(margin))) && glm::all(glm::lessThan(at, cave.max + glm::vec3(margin))) &&
            cave.Distance(at) < margin + 0.3f)
        {
            return true;
        }
    }
    return false;
}

uint16_t SitePlan::SeedFor(uint16_t seed, SiteKind kind)
{
    const auto packed = static_cast<uint16_t>((seed & 0x1FFFu) | (static_cast<uint16_t>(kind) << 13));
    return packed == 0 ? uint16_t{1} : packed;
}

SitePlan::SiteKind SitePlan::KindOf(uint32_t seed)
{
    const uint32_t kind = (seed >> 13) & 0x7u;
    return kind <= static_cast<uint32_t>(SiteKind::Signal) ? static_cast<SiteKind>(kind) : SiteKind::Facility;
}

SitePlan::FacilityType SitePlan::TypeOf(uint32_t seed, SiteKind kind)
{
    // On a draw of its own, so nothing else about the place changes with it.
    const uint32_t roll = Mix(seed, 0x7E9Eu);
    using Type = FacilityType;
    switch (kind)
    {
    case SiteKind::Facility:
    {
        static constexpr Type kAny[] = {Type::ResearchLab, Type::Mining, Type::Refinery, Type::CargoDepot, Type::Habitat};
        return kAny[roll % std::size(kAny)];
    }
    case SiteKind::Station:
    {
        static constexpr Type kAny[] = {Type::Habitat, Type::CommsRelay, Type::Observatory};
        return kAny[roll % std::size(kAny)];
    }
    case SiteKind::Survey: return roll % 3u == 0u ? Type::Observatory : Type::SurveyCamp;
    case SiteKind::Signal: return Type::CommsRelay;
    case SiteKind::Wreck: return Type::None;
    }
    return Type::None;
}

const char* SitePlan::TypeName(FacilityType type)
{
    switch (type)
    {
    case FacilityType::None: return "none";
    case FacilityType::ResearchLab: return "research lab";
    case FacilityType::Mining: return "mining operation";
    case FacilityType::Refinery: return "refinery";
    case FacilityType::CargoDepot: return "cargo depot";
    case FacilityType::Habitat: return "habitat";
    case FacilityType::CommsRelay: return "comms relay";
    case FacilityType::Observatory: return "observatory";
    case FacilityType::SurveyCamp: return "survey camp";
    }
    return "none";
}

SitePlan SitePlan::Generate(uint32_t seed, bool submerged)
{
    SitePlan plan;
    plan.seed = seed;
    plan.submerged = submerged;
    plan.kind = KindOf(seed);
    plan.type = TypeOf(seed, plan.kind);
    const SiteKind kind = plan.kind;
    Random random((static_cast<uint64_t>(seed) << 16) ^ 0x517E5EEDull);
    const float S = plan.size;
    const glm::vec3 O = plan.origin;
    const auto world = [&](float u, float v) { return glm::vec3(O.x + u, O.y, O.z + v); };
    const auto flat = [](const glm::vec3& p) { return glm::vec2(p.x, p.z); };

    plan.sky = kSkies[random.Int(0, static_cast<int>(std::size(kSkies)) - 1)];

    // Where the craft sets down: towards the middle, a little off it, facing in -- so every building is a walk from it,
    // none of them the whole width of the site away.
    const float landAngle = random.Range(0.0f, 6.2831853f);
    const float landOut = random.Range(22.0f, 40.0f);
    const glm::vec2 landingLocal = glm::vec2(S * 0.5f) + glm::vec2(std::cos(landAngle), std::sin(landAngle)) * landOut;
    constexpr float kPadHalf = 7.0f;
    constexpr float kPadTop = 0.1f;
    plan.landing = world(landingLocal.x, landingLocal.y) + glm::vec3(0.0f, kPadTop + 0.5f, 0.0f);
    const glm::vec2 toCentre = glm::vec2(S * 0.5f) - landingLocal;
    plan.landingYaw = std::atan2(toCentre.x, -toCentre.y);

    // The buildings: at a facility three to five, the first much the biggest -- the one a mission's business is usually in
    // -- none too close to another or to the landing; fewer and smaller elsewhere, as the kind of place has them.
    const float roll = random.Unit();
    const int count = kind == SiteKind::Facility ? (roll < 0.4f ? 3 : roll < 0.8f ? 4 : 5)
                      : kind == SiteKind::Station || kind == SiteKind::Signal ? 2
                                                                      : 1;
    // The first building's size (cells across, at least and at most) and floors, by the kind of place.
    const glm::ivec2 mainCells = kind == SiteKind::Facility ? glm::ivec2(24, 30) : kind == SiteKind::Station ? glm::ivec2(18, 22) : glm::ivec2(14, 18);
    const glm::ivec2 mainFloors = kind == SiteKind::Facility ? glm::ivec2(3, 4) : kind == SiteKind::Station ? glm::ivec2(2, 3) : glm::ivec2(1, 2);
    std::vector<std::pair<glm::vec2, glm::vec2>> taken; // footprints, local
    // How each was asked for and its seed, so one can be planned again (joined under the ground, below); and what each gets
    // once planned: its outside by the kind of place, and circuits of its own (a hundred apart), so one losing its power
    // leaves the rest lit.
    std::vector<std::pair<FacilityLayout::Options, uint32_t>> planned;
    const auto finish = [&](size_t index)
    {
        FacilityLayout& building = plan.buildings[index];
        building.outside = OutsideOf(plan.type, seed, static_cast<int>(index));
        Neglect(building, seed, static_cast<int>(index));
        building.outside.label = std::string("BLOCK ") + static_cast<char>('A' + static_cast<int>(index % 26));
        // A mine's bunker (FacilityLayout::Massing::Buried, two floors or more under the ground): its lowest floor its workings.
        const bool buried = std::count(building.heights.begin(), building.heights.end(), building.ground) > 0;
        building.workings = index == 0 && plan.type == FacilityType::Mining && building.ground >= 2 && buried ? 0 : -1;
        for (FacilityLayout::Lamp& lamp : building.lamps)
        {
            lamp.circuit += 100 * static_cast<int>(index + 1);
        }
    };
    for (int b = 0; b < count; ++b)
    {
        glm::ivec2 cells{0};
        glm::vec2 corner{0.0f};
        bool placed = false;
        for (int attempt = 0; attempt < 400 && !placed; ++attempt)
        {
            const int shrink = attempt / 100; // smaller if nothing fits
            cells = b == 0 ? glm::ivec2(random.Int(mainCells.x, mainCells.y) - shrink * 2, random.Int(mainCells.x, mainCells.y) - shrink * 2)
                           : glm::ivec2(random.Int(12, 20) - shrink * 2, random.Int(12, 20) - shrink * 2);
            const glm::vec2 extent = glm::vec2(cells) * kCell;
            corner = {std::round(random.Range(32.0f, S - 32.0f - extent.x) / kCell) * kCell,
                      std::round(random.Range(32.0f, S - 32.0f - extent.y) / kCell) * kCell};
            if (DistanceTo(landingLocal, corner, corner + extent) < 46.0f)
            {
                continue;
            }
            placed = true;
            for (const auto& [min, max] : taken)
            {
                const glm::vec2 gapMin = min - glm::vec2(20.0f);
                const glm::vec2 gapMax = max + glm::vec2(20.0f);
                if (corner.x < gapMax.x && corner.x + extent.x > gapMin.x && corner.y < gapMax.y && corner.y + extent.y > gapMin.y)
                {
                    placed = false;
                    break;
                }
            }
        }
        if (!placed)
        {
            continue;
        }
        const glm::vec2 extent = glm::vec2(cells) * kCell;
        taken.push_back({corner, corner + extent});

        // Its first way out faces the landing, so there is a straight run to it; the rest face elsewhere.
        const glm::vec2 toLanding = landingLocal - (corner + extent * 0.5f);
        FacilityLayout::Options options;
        options.width = cells.x;
        options.depth = cells.y;
        options.minFloors = b == 0 ? mainFloors.x : 1;
        options.maxFloors = b == 0 ? mainFloors.y : (kind == SiteKind::Facility ? 3 : 2);
        options.exits = b == 0 ? 3 : random.Int(1, 2);
        options.exitSide = std::abs(toLanding.x) > std::abs(toLanding.y) ? (toLanding.x < 0.0f ? 0 : 1) : (toLanding.y < 0.0f ? 2 : 3);
        options.origin = world(corner.x, corner.y);
        options.massing = MassingOf(plan.type, seed, static_cast<int>(plan.buildings.size()));
        options.program = ProgramOf(plan.type);
        // Basements under some (you, 2026-10-20): the main building more often, now and then two deep. On a draw of their own.
        {
            const uint32_t deep = Mix(seed, 0xBA5E0u + static_cast<uint32_t>(b)) % 100u;
            options.basements = b == 0 ? (deep < 15u ? 2 : deep < 60u ? 1 : 0) : (deep < 30u ? 1 : 0);
        }
        // And now and then the main building of a lab, a relay or a mine a bunker (FacilityLayout::Massing::Buried): an entrance
        // block a storey high over its floors under the ground, two or three of them under the whole of it. On a draw of its own.
        if (b == 0)
        {
            const uint32_t bunker = Mix(seed, 0xB0B0u) % 100u;
            const uint32_t chance = plan.type == FacilityType::Mining ? 35u : plan.type == FacilityType::CommsRelay ? 25u : plan.type == FacilityType::ResearchLab ? 20u : 0u;
            if (bunker < chance)
            {
                options.massing = FacilityLayout::Massing::Buried;
                options.minFloors = 1;
                options.maxFloors = 1;
                options.basements = 2 + static_cast<int>(bunker % 2u);
                // (A mine's broken into a cave, most often: a way out of its workings into the rock.)
                options.cave = plan.type == FacilityType::Mining && Mix(seed, 0xCA7Eu) % 100u < 70u;
            }
        }
        // Half fallen in, now and then, where it was left long ago or burnt (FacilityLayout::ruin): never the main building, where the
        // mission's business mostly is, nor a bunker. On a draw of its own.
        {
            const FacilityLayout::Condition condition = ConditionOf(seed, static_cast<int>(plan.buildings.size()));
            options.ruined = !plan.buildings.empty() && options.massing != FacilityLayout::Massing::Buried &&
                             (condition == FacilityLayout::Condition::Abandoned || condition == FacilityLayout::Condition::Burnt) &&
                             Mix(seed, 0x2F11u + static_cast<uint32_t>(b)) % 100u < 50u;
        }
        // On the sea's floor: sealed, come into by a moon pool; nothing under the ground, never half fallen in.
        if (submerged)
        {
            options.exits = 0;
            options.moonPool = true;
            options.basements = 0;
            options.massing = options.massing == FacilityLayout::Massing::Buried ? FacilityLayout::Massing::Block : options.massing;
            options.minFloors = std::max(options.minFloors, 1);
            options.maxFloors = std::max(options.maxFloors, options.minFloors);
            options.cave = false;
            options.ruined = false;
        }
        plan.buildings.push_back(FacilityLayout::Generate(Mix(seed, static_cast<uint32_t>(b) + 1u), options));
        planned.push_back({options, Mix(seed, static_cast<uint32_t>(b) + 1u)});
        finish(plan.buildings.size() - 1);
    }

    // Now and then (you, 2026-10-26: sometimes, not often) the main building joined to a neighbour under the ground: both
    // planned again with a floor under the ground and a doorway down there facing the other, and a passage between the two
    // through the site's grid -- out from one, along, and in to the other -- under no building. The nearest neighbour it can
    // be done with, or none. On a draw of its own.
    if (!submerged && kind != SiteKind::Wreck && plan.buildings.size() >= 2 && Mix(seed, 0x7E44E1u) % 100u < 22u)
    {
        const auto middle = [](const FacilityLayout& building)
        { return glm::vec2(building.origin.x, building.origin.z) + glm::vec2(static_cast<float>(building.width), static_cast<float>(building.depth)) * (kCell * 0.5f); };
        const auto gridOf = [&](const FacilityLayout& building, glm::ivec2 cell)
        { return glm::ivec2(glm::round((glm::vec2(building.origin.x, building.origin.z) - glm::vec2(O.x, O.z)) / kCell)) + cell; };
        const auto outOf = [](int side) { return side == 0 ? glm::ivec2(-1, 0) : side == 1 ? glm::ivec2(1, 0) : side == 2 ? glm::ivec2(0, -1) : glm::ivec2(0, 1); };
        std::vector<size_t> others;
        for (size_t i = 1; i < plan.buildings.size(); ++i)
        {
            others.push_back(i);
        }
        std::sort(others.begin(), others.end(), [&](size_t p, size_t q)
                  { return glm::distance(middle(plan.buildings[p]), middle(plan.buildings[0])) < glm::distance(middle(plan.buildings[q]), middle(plan.buildings[0])); });
        for (const size_t other : others)
        {
            const glm::vec2 apart = middle(plan.buildings[other]) - middle(plan.buildings[0]);
            const int side = std::abs(apart.x) >= std::abs(apart.y) ? (apart.x > 0.0f ? 1 : 0) : (apart.y > 0.0f ? 3 : 2);
            FacilityLayout::Options here = planned[0].first;
            FacilityLayout::Options there = planned[other].first;
            here.basements = std::max(here.basements, 1);
            there.basements = std::max(there.basements, 1);
            here.tunnelSide = side;
            there.tunnelSide = side ^ 1;
            FacilityLayout first = FacilityLayout::Generate(planned[0].second, here);
            FacilityLayout second = FacilityLayout::Generate(planned[other].second, there);
            if (first.tunnels.empty() || second.tunnels.empty())
            {
                continue;
            }
            // Out of the one, along half way, across, and on in to the other.
            const glm::ivec2 from = gridOf(first, first.tunnels.front().cell) + outOf(side);
            const glm::ivec2 to = gridOf(second, second.tunnels.front().cell) + outOf(side ^ 1);
            const bool alongX = side < 2;
            const int turn = alongX ? (from.x + to.x) / 2 : (from.y + to.y) / 2;
            std::vector<glm::ivec2> cells{from};
            const auto step = [&](glm::ivec2 target)
            {
                while (cells.back() != target && cells.size() < 200)
                {
                    const glm::ivec2 d = target - cells.back();
                    cells.push_back(cells.back() + (d.x != 0 ? glm::ivec2(d.x > 0 ? 1 : -1, 0) : glm::ivec2(0, d.y > 0 ? 1 : -1)));
                }
            };
            step(alongX ? glm::ivec2(turn, from.y) : glm::ivec2(from.x, turn));
            step(alongX ? glm::ivec2(turn, to.y) : glm::ivec2(to.x, turn));
            step(to);
            // Under nothing built, on the site, not too long -- and leaving each the way its doorway faces.
            bool fits = cells.size() < 60 && (alongX ? (to.x - from.x) * (side == 1 ? 1 : -1) > 0 : (to.y - from.y) * (side == 3 ? 1 : -1) > 0);
            const int across = static_cast<int>(S / kCell);
            for (const glm::ivec2 cell : cells)
            {
                fits = fits && cell.x > 2 && cell.y > 2 && cell.x < across - 3 && cell.y < across - 3;
                for (size_t i = 0; i < plan.buildings.size() && fits; ++i)
                {
                    const FacilityLayout& building = i == 0 ? first : i == other ? second : plan.buildings[i];
                    const glm::ivec2 local = cell - gridOf(building, glm::ivec2(0));
                    fits = building.Height(local.x, local.y) == 0;
                }
            }
            // And beside no building's wall but at its own two doorways: alongside one, its wall and the building's would be
            // in one place.
            const glm::ivec2 doorHere = gridOf(first, first.tunnels.front().cell);
            const glm::ivec2 doorThere = gridOf(second, second.tunnels.front().cell);
            for (const glm::ivec2 cell : cells)
            {
                // (Nor at a corner of one, but by its doorways, where its wall runs straight.)
                const bool end = cell == from || cell == to;
                for (const glm::ivec2 n : {cell + glm::ivec2(1, 0), cell - glm::ivec2(1, 0), cell + glm::ivec2(0, 1), cell - glm::ivec2(0, 1), cell + glm::ivec2(1, 1),
                                           cell + glm::ivec2(1, -1), cell + glm::ivec2(-1, 1), cell - glm::ivec2(1, 1)})
                {
                    const glm::ivec2 door = cell == from ? doorHere : doorThere;
                    if (end && (n == door || (n - cell).x * (door - cell).x + (n - cell).y * (door - cell).y > 0))
                    {
                        continue;
                    }
                    for (size_t i = 0; i < plan.buildings.size() && fits; ++i)
                    {
                        const FacilityLayout& building = i == 0 ? first : i == other ? second : plan.buildings[i];
                        const glm::ivec2 local = n - gridOf(building, glm::ivec2(0));
                        fits = building.Height(local.x, local.y) == 0;
                    }
                }
            }
            if (!fits)
            {
                continue;
            }
            plan.buildings[0] = std::move(first);
            plan.buildings[other] = std::move(second);
            planned[0].first = here; // (as they are now, should either be planned again)
            planned[other].first = there;
            finish(0);
            finish(other);
            Tunnel tunnel;
            tunnel.from = 0;
            tunnel.to = static_cast<int>(other);
            tunnel.floor = plan.buildings[0].GroundLevel() - FacilityLayout::kStorey;
            tunnel.cells = std::move(cells);
            // The ground over it level, so it lies under a cover of earth all the way.
            for (const glm::ivec2 cell : tunnel.cells)
            {
                const glm::vec2 lo = glm::vec2(O.x, O.z) + glm::vec2(cell) * kCell;
                plan.levelled.push_back({lo.x, lo.y, lo.x + kCell, lo.y + kCell});
            }
            plan.tunnels.push_back(std::move(tunnel));
            break;
        }
    }

    // Caves broken into from a mine's workings (FacilityLayout::caves).
    for (size_t b = 0; b < plan.buildings.size(); ++b)
    {
        for (const FacilityLayout::Exit& mouth : plan.buildings[b].caves)
        {
            plan.caves.push_back(PlanCave(plan, static_cast<int>(b), mouth, Mix(seed, 0xCA7E5u + static_cast<uint32_t>(b))));
        }
    }
    // And now and then another building or two joined to a cave: the nearest others first, each planned again two floors deep
    // with a doorway out of its lowest floor on the side facing the cave, and a way of its own through the rock into it -- where
    // one can be found. On draws of their own.
    for (SitePlan::Cave& cave : plan.caves)
    {
        const glm::vec2 into{cave.mouth.x + cave.out.x * 20.0f, cave.mouth.z + cave.out.z * 20.0f};
        std::vector<size_t> others;
        for (size_t b = 0; b < plan.buildings.size(); ++b)
        {
            if (static_cast<int>(b) != cave.building && plan.buildings[b].caves.empty() && !plan.buildings[b].ruin.Has() && b < planned.size())
            {
                others.push_back(b);
            }
        }
        const auto middle = [](const FacilityLayout& building)
        { return glm::vec2(building.origin.x, building.origin.z) + glm::vec2(static_cast<float>(building.width), static_cast<float>(building.depth)) * (kCell * 0.5f); };
        std::sort(others.begin(), others.end(), [&](size_t p, size_t q)
                  { return glm::distance(middle(plan.buildings[p]), into) < glm::distance(middle(plan.buildings[q]), into); });
        int joined = 0;
        for (const size_t other : others)
        {
            if (joined == 2 || Mix(seed, 0x701E5u + static_cast<uint32_t>(other)) % 100u >= 60u)
            {
                continue;
            }
            // Its side facing the cave's middle.
            const glm::vec3 centre = (cave.min + cave.max) * 0.5f;
            const glm::vec2 apart = glm::vec2(centre.x, centre.z) - middle(plan.buildings[other]);
            FacilityLayout::Options options = planned[other].first;
            options.basements = std::max(options.basements, 2);
            options.cave = true;
            options.caveSide = std::abs(apart.x) >= std::abs(apart.y) ? (apart.x > 0.0f ? 1 : 0) : (apart.y > 0.0f ? 3 : 2);
            FacilityLayout relaid = FacilityLayout::Generate(planned[other].second, options);
            if (relaid.caves.empty())
            {
                continue;
            }
            // (Deeper now: clear still of every cave's passages, any of which might have gone under it.)
            bool under = false;
            for (const SitePlan::Cave& each : plan.caves)
            {
                for (const SitePlan::Cave::Passage& passage : each.passages)
                {
                    glm::vec2 min;
                    glm::vec2 max;
                    SitePlan::Footprint(relaid, passage.radius + 3.0f, min, max);
                    for (const glm::vec3& at : {passage.a, passage.b, (passage.a + passage.b) * 0.5f})
                    {
                        under = under || (at.x > min.x && at.x < max.x && at.z > min.y && at.z < max.y && at.y > relaid.origin.y - passage.radius - 2.0f);
                    }
                }
            }
            if (under)
            {
                continue;
            }
            FacilityLayout was = std::move(plan.buildings[other]);
            plan.buildings[other] = std::move(relaid);
            const SitePlan::Cave kept = cave;
            if (!JoinCave(cave, plan, static_cast<int>(other), plan.buildings[other].caves.front(), Mix(seed, 0x701E6u + static_cast<uint32_t>(other))))
            {
                plan.buildings[other] = std::move(was);
                cave = kept;
                continue;
            }
            planned[other].first = options;
            finish(other);
            ++joined;
        }
    }

    {
        const glm::vec2 into{std::sin(plan.landingYaw), -std::cos(plan.landingYaw)};
        plan.rampFoot.position = world(landingLocal.x + into.x * 11.0f, landingLocal.y + into.y * 11.0f);
        plan.rampFoot.yaw = plan.landingYaw;
    }

    // The pad, and floodlights at its corners.
    plan.blocks.push_back({Kind::Pad, world(landingLocal.x, landingLocal.y) + glm::vec3(0.0f, (kPadTop - 0.02f) * 0.5f, 0.0f),
                           {kPadHalf * 2.0f, kPadTop + 0.02f, kPadHalf * 2.0f}, 0.0f});
    for (const glm::vec2 corner : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
    {
        const glm::vec2 at = landingLocal + corner * (kPadHalf + 1.5f);
        SitePlan::Lamp lamp;
        lamp.kind = LampKind::Pole;
        lamp.position = world(at.x, at.y) + glm::vec3(0.0f, 6.0f, 0.0f);
        lamp.direction = glm::normalize(glm::vec3(-corner.x * 0.35f, -1.0f, -corner.y * 0.35f));
        lamp.range = 18.0f;
        lamp.mood = PickMood(random, 0.85f);
        plan.lamps.push_back(lamp);
    }

    // A floodlight over every way in, and poles along the way from the landing to each building's first.
    for (const FacilityLayout& building : plan.buildings)
    {
        for (const FacilityLayout::Exit& exit : building.exits)
        {
            SitePlan::Lamp lamp;
            lamp.kind = LampKind::Flood;
            const glm::vec2 out = Outward(exit.side);
            // (Under its canopy, where it has one; over a vehicle bay's high door, higher.)
            // Out from the wall on a bracket (SiteMap), far enough that its head clears the wall however it is tilted.
            lamp.position = ExitPoint(building, exit, 0.75f) + glm::vec3(0.0f, exit.bay ? 3.4f : building.outside.canopies ? 2.7f : 3.1f, 0.0f);
            lamp.direction = glm::normalize(glm::vec3(out.x * 0.7f, -1.0f, out.y * 0.7f));
            lamp.range = 15.0f;
            lamp.mood = PickMood(random, 0.8f);
            plan.lamps.push_back(lamp);
        }
    }

    // The way in, marked one way for the whole site (on a draw of its own): from the landing to each building's main door.
    {
        Random way(Mix(seed, 0x3A71u));
        const float pick = way.Unit();
        plan.wayIn = pick < 0.3f ? WayIn::LitEntrance : pick < 0.55f ? WayIn::Signposts : pick < 0.8f ? WayIn::Path : WayIn::Beacon;
        for (size_t b = 0; b < plan.buildings.size(); ++b)
        {
            FacilityLayout& building = plan.buildings[b];
            building.outside.labelLit = plan.wayIn == WayIn::LitEntrance;
            if (building.exits.empty())
            {
                continue;
            }
            const glm::vec2 from = flat(plan.landing);
            const glm::vec2 to = flat(ExitPoint(building, building.exits.front(), 3.0f));
            const float length = glm::length(to - from);
            const glm::vec2 dir = (to - from) / std::max(length, 1.0e-3f);
            const glm::vec2 across{-dir.y, dir.x};
            if (plan.wayIn == WayIn::Signposts)
            {
                // One, beside the way just off the pad's edge, saying where it goes -- a post for each building there, not a row
                // of them all the way to its door.
                const glm::vec2 at = from + dir * (kPadHalf + 4.0f) + across * 2.8f;
                if (length > kPadHalf + 14.0f && !plan.InBuilding(at, 2.0f))
                {
                    plan.signposts.push_back({glm::vec3(at.x, O.y, at.y), std::atan2(dir.x, dir.y), building.outside.label});
                }
            }
            else if (plan.wayIn == WayIn::Path)
            {
                // Worn in the ground from the pad's edge to the door, a point every metre and a half -- broken where it would cross
                // what is built (a bunker's roof, flush with the ground: FacilityLayout::Massing::Buried), walked over, not worn.
                std::vector<glm::vec2> path;
                const auto lay = [&]
                {
                    if (path.size() >= 2)
                    {
                        plan.paths.push_back(path);
                    }
                    path.clear();
                };
                for (float d = kPadHalf + 1.0f; d < length; d += 1.5f)
                {
                    const glm::vec2 at = from + dir * d;
                    const bool over = std::any_of(plan.buildings.begin(), plan.buildings.end(), [&](const FacilityLayout& other)
                    {
                        const glm::vec2 cell = (at - glm::vec2(other.origin.x, other.origin.z)) / kCell;
                        return other.Height(static_cast<int>(std::floor(cell.x)), static_cast<int>(std::floor(cell.y))) > other.ground;
                    });
                    if (over)
                    {
                        lay();
                        continue;
                    }
                    path.push_back(at);
                }
                path.push_back(to);
                lay();
            }
            else if (plan.wayIn == WayIn::Beacon && b == 0)
            {
                // A mast beside the main building's door, its red light seen from anywhere on the site.
                const FacilityLayout::Exit& exit = building.exits.front();
                const glm::vec2 out = Outward(exit.side);
                const glm::vec2 at = flat(ExitPoint(building, exit, 3.5f)) + glm::vec2(-out.y, out.x) * 3.0f;
                SitePlan::Lamp lamp;
                lamp.kind = LampKind::Mast;
                lamp.position = glm::vec3(at.x, O.y + 8.0f, at.y);
                lamp.range = 30.0f;
                plan.lamps.push_back(lamp);
            }
        }
    }

    // Things to get behind, and things to find your way by.
    std::vector<std::pair<glm::vec2, float>> cover; // centre and radius, in the world
    // Nothing at the foot of the shuttle's ramp.
    cover.push_back({flat(plan.rampFoot.position), 6.0f});
    // And nothing in front of any way in: a container or a rock outside a door is a door nobody can use.
    for (const FacilityLayout& building : plan.buildings)
    {
        for (const FacilityLayout::Exit& exit : building.exits)
        {
            cover.push_back({flat(ExitPoint(building, exit, 4.0f)), 5.0f});
        }
    }
    // Nor on a lamp's post.
    for (const SitePlan::Lamp& lamp : plan.lamps)
    {
        if (lamp.kind == LampKind::Pole)
        {
            cover.push_back({flat(lamp.position), 0.5f});
        }
    }
    const auto clear = [&](glm::vec2 at, float radius)
    {
        const glm::vec2 local = at - flat(O);
        if (local.x < 12.0f + radius || local.y < 12.0f + radius || local.x > S - 12.0f - radius || local.y > S - 12.0f - radius)
        {
            return false;
        }
        if (glm::distance(at, flat(plan.landing)) < kPadHalf + 6.0f + radius || plan.InBuilding(at, radius + 3.0f))
        {
            return false;
        }
        for (const auto& [centre, r] : cover)
        {
            if (glm::distance(centre, at) < r + radius + 2.0f)
            {
                return false;
            }
        }
        return true;
    };
    const auto somewhere = [&](float radius, glm::vec2& at)
    {
        for (int attempt = 0; attempt < 60; ++attempt)
        {
            at = flat(O) + glm::vec2(random.Range(0.0f, S), random.Range(0.0f, S));
            if (clear(at, radius))
            {
                cover.push_back({at, radius});
                return true;
            }
        }
        return false;
    };
    const int containers = kind == SiteKind::Facility || kind == SiteKind::Station ? random.Int(6, 10) : kind == SiteKind::Wreck ? random.Int(3, 6) : random.Int(1, 3);
    for (int i = 0; i < containers; ++i)
    {
        glm::vec2 at;
        if (!somewhere(3.4f, at))
        {
            continue;
        }
        const float yaw = (random.Chance(0.5f) ? 0.0f : glm::half_pi<float>()) + random.Range(-0.08f, 0.08f);
        const glm::vec3 size{6.06f, 2.59f, 2.44f};
        plan.blocks.push_back({Kind::Container, glm::vec3(at.x, O.y + size.y * 0.5f - 0.02f, at.y), size, yaw});
        if (random.Chance(0.3f))
        {
            plan.blocks.push_back({Kind::Container, glm::vec3(at.x, O.y + size.y * 1.5f - 0.02f, at.y), size, yaw + random.Range(-0.1f, 0.1f)});
        }
    }
    const int rocks = kind == SiteKind::Survey ? random.Int(34, 48) : random.Int(18, 28);
    for (int i = 0; i < rocks; ++i)
    {
        const glm::vec3 size{random.Range(1.0f, 3.5f), random.Range(0.8f, 2.2f), random.Range(1.0f, 3.5f)};
        glm::vec2 at;
        if (!somewhere(std::max(size.x, size.z) * 0.6f, at))
        {
            continue;
        }
        plan.blocks.push_back({Kind::Rock, glm::vec3(at.x, O.y + size.y * 0.5f - 0.25f, at.y), size, random.Range(0.0f, glm::pi<float>())});
    }
    // A radio mast at a station or a signal source: tall, beside the first building, a landmark from anywhere on the site.
    if ((kind == SiteKind::Station || kind == SiteKind::Signal) && !plan.buildings.empty())
    {
        glm::vec2 min;
        glm::vec2 max;
        Footprint(plan.buildings.front(), 0.0f, min, max);
        const float high = kind == SiteKind::Signal ? random.Range(30.0f, 40.0f) : random.Range(20.0f, 28.0f);
        for (int attempt = 0; attempt < 30; ++attempt)
        {
            const float angle = random.Range(0.0f, 6.2831853f);
            const glm::vec2 at = (min + max) * 0.5f + glm::vec2(std::cos(angle), std::sin(angle)) * (glm::length(max - min) * 0.5f + random.Range(8.0f, 16.0f));
            if (!clear(at, 3.0f))
            {
                continue;
            }
            cover.push_back({at, 3.0f});
            plan.blocks.push_back({Kind::Mast, glm::vec3(at.x, O.y + high * 0.5f, at.y), {1.4f, high, 1.4f}, random.Range(0.0f, 1.5f)});
            break;
        }
    }
    // At a wreck, what came down: great plates of it tipped into the ground, and smaller pieces strewn along the way it
    // came in, all round what is left standing.
    if (kind == SiteKind::Wreck && !plan.buildings.empty())
    {
        glm::vec2 min;
        glm::vec2 max;
        Footprint(plan.buildings.front(), 0.0f, min, max);
        const glm::vec2 middle = (min + max) * 0.5f;
        const float trail = random.Range(0.0f, 6.2831853f);
        const glm::vec2 along{std::cos(trail), std::sin(trail)};
        const int pieces = random.Int(16, 24);
        for (int i = 0; i < pieces; ++i)
        {
            const bool large = i < 4;
            const glm::vec3 size = large ? glm::vec3(random.Range(9.0f, 15.0f), random.Range(0.6f, 1.2f), random.Range(4.0f, 7.0f))
                                         : glm::vec3(random.Range(1.5f, 5.0f), random.Range(0.25f, 0.8f), random.Range(1.0f, 3.5f));
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                const float out = glm::length(max - min) * 0.5f + random.Range(6.0f, large ? 30.0f : 70.0f);
                const glm::vec2 at = middle + along * out * random.Range(0.3f, 1.0f) + glm::vec2(-along.y, along.x) * random.Range(-18.0f, 18.0f);
                const float radius = std::max(size.x, size.z) * 0.55f;
                if (!clear(at, radius))
                {
                    continue;
                }
                cover.push_back({at, radius});
                // The great plates stand up out of the ground where they dug in, an edge high; the small ones lie nearly flat.
                // Either way about a third of each rises clear of the ground.
                const float lean = large ? random.Range(0.5f, 0.95f) : random.Range(0.0f, 0.45f);
                const float tip = random.Chance(0.5f) ? lean : -lean;
                plan.blocks.push_back({Kind::Debris, glm::vec3(at.x, O.y + size.y * 0.2f + std::sin(lean) * size.z * 0.18f, at.y), size,
                                       random.Range(0.0f, glm::pi<float>()), tip});
                break;
            }
        }
    }
    // Fuel tanks, standing beside a building.
    for (const FacilityLayout& building : plan.buildings)
    {
        if (!random.Chance(kind == SiteKind::Station ? 0.9f : kind == SiteKind::Facility ? 0.6f : 0.3f))
        {
            continue;
        }
        glm::vec2 min;
        glm::vec2 max;
        Footprint(building, 0.0f, min, max);
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            const int side = random.Int(0, 3);
            const float t = random.Unit();
            const glm::vec2 at = side == 0   ? glm::vec2(min.x - 5.5f, glm::mix(min.y, max.y, t))
                                 : side == 1 ? glm::vec2(max.x + 5.5f, glm::mix(min.y, max.y, t))
                                 : side == 2 ? glm::vec2(glm::mix(min.x, max.x, t), min.y - 5.5f)
                                             : glm::vec2(glm::mix(min.x, max.x, t), max.y + 5.5f);
            if (!clear(at, 2.2f))
            {
                continue;
            }
            cover.push_back({at, 2.2f});
            plan.blocks.push_back({Kind::Tank, glm::vec3(at.x, O.y + 2.5f, at.y), {3.6f, 5.0f, 3.6f}, 0.0f});
            break;
        }
    }
    // A pipe overhead from each building to the next, on supports: something to follow in the dark. Out of a wall of the one
    // facing the other along x, round a bend, and along z into a wall of the other -- each end at a real stretch of wall on
    // the ground floor (whatever the building's shape), between two bays of windows (where nothing else is: no rib, no door
    // near), high enough to clear a head and a door's canopy and under the band at the next floor; collared where it meets
    // each wall, an elbow at the bend, and clear of every building on the way.
    {
        constexpr float kPipeHeight = 2.8f;
        constexpr float kPipeThick = 0.7f;
        constexpr float kSkin = FacilitySpec::kShellThickness + FacilitySpec::kCladding - FacilitySpec::kHalfWall;
        // Where a pipe may meet a building facing `side`: on a wall standing on the ground, at a line between two bays, a
        // bay and more from either end of the wall, no rib there, no way out within a few metres. The point on the wall's
        // cladding, in the world.
        const auto entries = [&](const FacilityLayout& building, int side)
        {
            std::vector<glm::vec2> at;
            const glm::vec2 corner{building.origin.x, building.origin.z};
            for (const FacilityLayout::OuterWall& wall : building.OuterWalls())
            {
                if (wall.side != side || wall.low != 0)
                {
                    continue;
                }
                const float out = (side == 0 || side == 2) ? -kSkin : kSkin;
                for (int k = wall.from + 2; k <= wall.to - 2; ++k)
                {
                    const int ribs = building.outside.windows == FacilityLayout::Outside::Windows::Strip ? 0 : building.outside.ribEvery;
                    if (ribs > 0 && k % ribs == 0)
                    {
                        continue;
                    }
                    const glm::vec2 point = side < 2 ? corner + glm::vec2(static_cast<float>(wall.line) * kCell + out, static_cast<float>(k) * kCell)
                                                     : corner + glm::vec2(static_cast<float>(k) * kCell, static_cast<float>(wall.line) * kCell + out);
                    bool byDoor = false;
                    for (const FacilityLayout::Exit& exit : building.exits)
                    {
                        const glm::vec3 door = ExitPoint(building, exit, 0.0f);
                        byDoor = byDoor || glm::distance(glm::vec2(door.x, door.z), point) < 3.5f;
                    }
                    if (!byDoor)
                    {
                        at.push_back(point);
                    }
                }
            }
            return at;
        };
        // Whether a point is in a building, or within `margin` of one -- by its shape, not its rectangle.
        const auto inBuilding = [&](glm::vec2 p, float margin)
        {
            for (const FacilityLayout& building : plan.buildings)
            {
                const glm::vec2 local = (p - glm::vec2(building.origin.x, building.origin.z)) / kCell;
                const float reach = margin / kCell;
                for (const glm::vec2 c : {local + glm::vec2(-reach, -reach), local + glm::vec2(reach, -reach), local + glm::vec2(-reach, reach),
                                          local + glm::vec2(reach, reach), local})
                {
                    if (building.Height(static_cast<int>(std::floor(c.x)), static_cast<int>(std::floor(c.y))) > 0)
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        // Whether a point is by anything standing on the ground already: a lamp's pole, a tank, a mast, a container, a rock.
        const auto byThings = [&](glm::vec2 p)
        {
            for (const SitePlan::Lamp& lamp : plan.lamps)
            {
                if (lamp.kind == LampKind::Pole && glm::distance(p, flat(lamp.position)) < 1.2f)
                {
                    return true;
                }
            }
            for (const SitePlan::Block& block : plan.blocks)
            {
                const float reach = std::max(block.size.x, block.size.z) * 0.5f + 0.8f;
                if (!block.outlying && block.kind != Kind::Pad && glm::distance(p, flat(block.centre)) < reach)
                {
                    return true;
                }
            }
            return false;
        };
        // A stretch clear of every building -- but for a metre or so at either end, where it goes into one -- and of whatever
        // stands about.
        const auto clearRun = [&](glm::vec2 a, glm::vec2 b, bool intoA, bool intoB)
        {
            const float length = glm::length(b - a);
            for (float d = intoA ? 1.2f : 0.0f; d <= length - (intoB ? 1.2f : 0.0f); d += 0.5f)
            {
                const glm::vec2 p = a + (b - a) * (d / std::max(length, 1.0e-3f));
                if (inBuilding(p, 0.9f) || byThings(p))
                {
                    return false;
                }
            }
            return true;
        };
        const glm::vec2 pad = flat(plan.landing);
        for (size_t b = 0; b + 1 < plan.buildings.size(); ++b)
        {
            const FacilityLayout& from = plan.buildings[b];
            const FacilityLayout& to = plan.buildings[b + 1];
            glm::vec2 aMin, aMax, bMin, bMax;
            Footprint(from, 0.0f, aMin, aMax);
            Footprint(to, 0.0f, bMin, bMax);
            const glm::vec2 aCentre = (aMin + aMax) * 0.5f;
            const glm::vec2 bCentre = (bMin + bMax) * 0.5f;
            const std::vector<glm::vec2> starts = entries(from, bCentre.x > aCentre.x ? 1 : 0);
            // The best: the shortest that fits.
            float best = 1.0e9f;
            glm::vec2 start{0.0f};
            glm::vec2 end{0.0f};
            for (const glm::vec2 s : starts)
            {
                for (const glm::vec2 e : entries(to, s.y < bCentre.y ? 2 : 3))
                {
                    const glm::vec2 corner{e.x, s.y};
                    const float length = std::abs(corner.x - s.x) + std::abs(e.y - corner.y);
                    // Out of the one the way its wall faces, into the other the way its faces; long enough to bend.
                    const bool ways = (corner.x - s.x) * (bCentre.x > aCentre.x ? 1.0f : -1.0f) > 4.0f && (e.y - corner.y) * (s.y < bCentre.y ? 1.0f : -1.0f) > 4.0f;
                    if (!ways || length >= best || inBuilding(corner, 1.5f) || !clearRun(s, corner, true, false) || !clearRun(corner, e, false, true) ||
                        DistanceTo(pad, glm::min(s, corner), glm::max(s, corner)) < kPadHalf + 3.0f ||
                        DistanceTo(pad, glm::min(corner, e), glm::max(corner, e)) < kPadHalf + 3.0f)
                    {
                        continue;
                    }
                    best = length;
                    start = s;
                    end = e;
                }
            }
            if (best >= 1.0e9f)
            {
                continue;
            }
            const glm::vec2 corner{end.x, start.y};
            // Into each wall a little past its cladding, so it goes in rather than stopping against it.
            const float dirX = corner.x > start.x ? 1.0f : -1.0f;
            const float dirZ = end.y > corner.y ? 1.0f : -1.0f;
            const float x0 = start.x - dirX * 0.25f;
            const float z1 = end.y + dirZ * 0.25f;
            const float y = O.y + kPipeHeight;
            // Round the corner in a bend (an elbow kBend round its middle), the two runs stopping where it starts and ends, a
            // flange at each join.
            constexpr float kBend = 1.0f;
            const float bendX = corner.x - dirX * kBend;
            const float bendZ = corner.y + dirZ * kBend;
            plan.blocks.push_back({Kind::PipeX, glm::vec3((x0 + bendX) * 0.5f, y, start.y), {std::abs(bendX - x0), kPipeThick, kPipeThick}, 0.0f});
            plan.blocks.push_back({Kind::PipeZ, glm::vec3(corner.x, y, (bendZ + z1) * 0.5f), {kPipeThick, kPipeThick, std::abs(z1 - bendZ)}, 0.0f});
            {
                // Its yaw the way it comes in (+x unturned, -x turned half round); `tip` which way round it goes from there.
                SitePlan::Block bend{Kind::Elbow, glm::vec3(corner.x, y, corner.y), {kBend, kPipeThick, kBend}, dirX > 0.0f ? 0.0f : glm::pi<float>()};
                bend.tip = dirX * dirZ;
                plan.blocks.push_back(bend);
            }
            plan.blocks.push_back({Kind::Collar, glm::vec3(bendX, y, start.y), {0.1f, kPipeThick + 0.1f, kPipeThick + 0.1f}, 0.0f});
            plan.blocks.push_back({Kind::Collar, glm::vec3(corner.x, y, bendZ), {0.1f, kPipeThick + 0.1f, kPipeThick + 0.1f}, glm::half_pi<float>()});
            // A collar against each wall: along x at the first, along z (turned) at the second.
            plan.blocks.push_back({Kind::Collar, glm::vec3(start.x + dirX * 0.06f, y, start.y), {0.12f, kPipeThick + 0.2f, kPipeThick + 0.2f}, 0.0f});
            plan.blocks.push_back({Kind::Collar, glm::vec3(end.x, y, end.y - dirZ * 0.06f), {0.12f, kPipeThick + 0.2f, kPipeThick + 0.2f}, glm::half_pi<float>()});
            // Supports under it, every seven metres or so, their saddles across it.
            const auto supports = [&](glm::vec2 a, glm::vec2 c, float yaw)
            {
                const float length = glm::length(c - a);
                const int count = std::max(1, static_cast<int>(length / 7.0f));
                for (int k = 1; k <= count; ++k)
                {
                    const glm::vec2 at = a + (c - a) * (static_cast<float>(k) / static_cast<float>(count + 1));
                    const float high = kPipeHeight - kPipeThick * 0.5f - 0.1f;
                    plan.blocks.push_back({Kind::Support, glm::vec3(at.x, O.y + high * 0.5f, at.y), {0.3f, high, kPipeThick + 0.1f}, yaw});
                    // Clamped to it there.
                    plan.blocks.push_back({Kind::Collar, glm::vec3(at.x, O.y + kPipeHeight, at.y), {0.1f, kPipeThick + 0.08f, kPipeThick + 0.08f}, yaw});
                }
            };
            supports(start, corner, 0.0f);
            supports(corner, end, glm::half_pi<float>());
            // Nothing put down after it under it: a container's as tall as it is low.
            for (const auto& [a, c] : {std::pair{start, corner}, std::pair{corner, end}})
            {
                const float length = glm::length(c - a);
                for (float d = 0.0f; d <= length; d += 2.0f)
                {
                    cover.push_back({a + (c - a) * (d / std::max(length, 1.0e-3f)), 1.6f});
                }
            }
        }
    }
    // Round the landing, a place that is used (on a draw of its own, so nothing else about the site changes): a red beacon
    // at the middle of each edge of the pad, pulsing; cargo staged to one side of it -- a container or two, a fuel tank --
    // where nothing else stands; and a windsock off one corner.
    {
        Random around((static_cast<uint64_t>(seed) << 11) ^ 0x1A4D1ull);
        const glm::vec2 pad{plan.landing.x, plan.landing.z};
        for (const glm::vec2 edge : {glm::vec2(1.0f, 0.0f), glm::vec2(-1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f, -1.0f)})
        {
            SitePlan::Lamp beacon;
            beacon.kind = LampKind::Beacon;
            const glm::vec2 at = pad + edge * (kPadHalf + 0.8f);
            // (Its red lamp sitting on its post's top, 0.9 m up: the lamp 0.12 tall, its middle that and half more.)
            beacon.position = glm::vec3(at.x, plan.origin.y + 0.96f, at.y);
            // A marker light, lighting the ground round its post and no further: reaching nine metres, the one off the end of
            // the ramp lit the inside of the shuttle red through its walls.
            beacon.range = 3.0f;
            plan.lamps.push_back(beacon);
        }
        const glm::vec2 into{std::sin(plan.landingYaw), -std::cos(plan.landingYaw)};
        const glm::vec2 side = glm::vec2(-into.y, into.x) * (around.Chance(0.5f) ? 1.0f : -1.0f);
        const auto roomFor = [&](const glm::vec2& at, float radius)
        {
            if (plan.InBuilding(at, radius + 2.0f))
            {
                return false;
            }
            for (const Block& block : plan.blocks)
            {
                if (glm::distance(at, glm::vec2(block.centre.x, block.centre.z)) < radius + std::max(block.size.x, block.size.z) * 0.5f + 1.5f)
                {
                    return false;
                }
            }
            // Nor on a lamp's post (the pad's at its corners).
            for (const SitePlan::Lamp& lamp : plan.lamps)
            {
                if (lamp.kind == LampKind::Pole && glm::distance(at, glm::vec2(lamp.position.x, lamp.position.z)) < radius + 1.0f)
                {
                    return false;
                }
            }
            return true;
        };
        const int staged = around.Int(1, 2);
        for (int i = 0; i <= staged; ++i)
        {
            const glm::vec2 at = pad + side * (kPadHalf + 5.0f) + into * (static_cast<float>(i) * 7.0f - 7.0f);
            const bool tank = i == staged;
            const glm::vec3 size = tank ? glm::vec3(2.6f, 3.2f, 2.6f) : glm::vec3(2.44f, 2.6f, 6.06f);
            if (!roomFor(at, tank ? 1.6f : 3.2f))
            {
                continue;
            }
            const float yaw = std::atan2(into.x, into.y) + around.Range(-0.08f, 0.08f);
            plan.blocks.push_back({tank ? Kind::Tank : Kind::Container, glm::vec3(at.x, plan.origin.y + size.y * 0.5f, at.y), size, yaw});
        }
        const glm::vec2 corner = pad + (side - into) * (kPadHalf + 4.0f) * 0.7071f;
        if (roomFor(corner, 0.8f))
        {
            plan.windsock = glm::vec3(corner.x, plan.origin.y, corner.y);
        }
    }
    // What stands round the buildings by what the place is (SiteStructures), on a draw of its own so nothing else about the site
    // changes: placed after everything else, clear of it all -- the buildings, their ways in, the paths, the pad and whatever stands
    // already.
    if (!plan.buildings.empty())
    {
        Random built((static_cast<uint64_t>(seed) << 11) ^ 0x57C7ull);
        const auto free = [&](glm::vec2 at, float radius)
        {
            if (!clear(at, radius))
            {
                return false;
            }
            for (const Block& block : plan.blocks)
            {
                if (glm::distance(at, glm::vec2(block.centre.x, block.centre.z)) < radius + std::max(block.size.x, block.size.z) * 0.5f + 1.5f)
                {
                    return false;
                }
            }
            for (const std::vector<glm::vec2>& path : plan.paths)
            {
                for (const glm::vec2& point : path)
                {
                    if (glm::distance(at, point) < radius + 2.5f)
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        // Somewhere `near` to `far` metres off a side of a building with `radius` of room there, and which way the building is
        // from it: false if nowhere.
        const auto beside = [&](const FacilityLayout& building, float near, float far, float radius, glm::vec2& at, glm::vec2& towards)
        {
            glm::vec2 min;
            glm::vec2 max;
            Footprint(building, 0.0f, min, max);
            for (int attempt = 0; attempt < 50; ++attempt)
            {
                const int side = built.Int(0, 3);
                const float t = built.Range(0.15f, 0.85f);
                const float out = built.Range(near, far) + radius;
                at = side == 0   ? glm::vec2(min.x - out, glm::mix(min.y, max.y, t))
                     : side == 1 ? glm::vec2(max.x + out, glm::mix(min.y, max.y, t))
                     : side == 2 ? glm::vec2(glm::mix(min.x, max.x, t), min.y - out)
                                 : glm::vec2(glm::mix(min.x, max.x, t), max.y + out);
                towards = side == 0 ? glm::vec2(1.0f, 0.0f) : side == 1 ? glm::vec2(-1.0f, 0.0f) : side == 2 ? glm::vec2(0.0f, 1.0f) : glm::vec2(0.0f, -1.0f);
                if (free(at, radius))
                {
                    return true;
                }
            }
            return false;
        };
        const auto put = [&](Kind kind, glm::vec2 at, glm::vec3 size, float yaw, float radius)
        {
            plan.blocks.push_back({kind, glm::vec3(at.x, O.y + size.y * 0.5f, at.y), size, yaw});
            cover.push_back({at, radius});
        };
        // (Turned so its own +z is towards `towards`.)
        const auto facing = [](glm::vec2 towards) { return std::atan2(towards.x, towards.y); };
        const FacilityLayout& main = plan.buildings.front();
        glm::vec2 at;
        glm::vec2 towards;
        switch (plan.type)
        {
        case FacilityType::Mining:
        {
            // The headframe over its shaft off the main building, its winder house behind it; spoil heaped further out.
            const float high = built.Range(20.0f, 26.0f);
            const float back = high * 0.75f + 4.0f;
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                if (beside(main, 6.0f, 14.0f, 6.0f, at, towards) && free(at - towards * back, 5.0f))
                {
                    put(Kind::Headframe, at, {8.0f, high, 8.0f}, facing(towards), 6.0f);
                    cover.push_back({at - towards * back, 5.0f});
                    break;
                }
            }
            for (int i = built.Int(1, 3); i > 0; --i)
            {
                if (beside(main, 22.0f, 45.0f, 9.0f, at, towards))
                {
                    put(Kind::SpoilHeap, at, {built.Range(12.0f, 18.0f), built.Range(4.0f, 7.0f), 16.0f}, built.Range(0.0f, 6.0f), 9.0f);
                }
            }
            break;
        }
        case FacilityType::Refinery:
        {
            // A tank farm in its bund off the main building, a pipe rack from it to the building, and the flare stack out
            // beyond.
            if (beside(main, 10.0f, 20.0f, 12.0f, at, towards))
            {
                const float yaw = facing(towards);
                const glm::vec2 x{std::cos(yaw), -std::sin(yaw)};
                const glm::vec2 z{std::sin(yaw), std::cos(yaw)};
                put(Kind::Bund, at, {20.0f, 1.0f, 20.0f}, yaw + glm::pi<float>(), 12.0f);
                for (const glm::vec2 corner : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
                {
                    const glm::vec2 tank = at + x * (corner.x * 4.6f) + z * (corner.y * 4.6f);
                    plan.blocks.push_back({Kind::Tank, glm::vec3(tank.x, O.y + 3.5f, tank.y), {5.6f, 7.0f, 5.6f}, yaw});
                }
                // The rack: from the bund's edge to the building's wall, along `towards`.
                glm::vec2 min;
                glm::vec2 max;
                Footprint(main, 0.0f, min, max);
                const glm::vec2 wall = glm::clamp(at, min, max);
                const glm::vec2 from = at + towards * 11.0f;
                const glm::vec2 to = wall - towards * 1.5f;
                const float length = glm::dot(to - from, towards);
                bool room = length > 6.0f;
                for (float d = 3.0f; room && d < length - 5.0f; d += 4.0f)
                {
                    // (Only its posts stand on the ground, every few metres: clear of the buildings and the paths -- under it, the way is clear.)
                    const glm::vec2 post = from + towards * d;
                    room = !plan.InBuilding(post, 1.0f);
                    for (const std::vector<glm::vec2>& path : plan.paths)
                    {
                        for (const glm::vec2& point : path)
                        {
                            room = room && glm::distance(post, point) > 3.0f;
                        }
                    }
                }
                if (room)
                {
                    const glm::vec2 middle = from + towards * (length * 0.5f);
                    plan.blocks.push_back({Kind::PipeRack, glm::vec3(middle.x, O.y + 3.0f, middle.y), {length, 6.0f, 3.0f}, std::atan2(-towards.y, towards.x)});
                }
            }
            if (beside(main, 30.0f, 55.0f, 6.0f, at, towards))
            {
                put(Kind::FlareStack, at, {1.2f, built.Range(24.0f, 32.0f), 1.2f}, 0.0f, 6.0f);
            }
            break;
        }
        case FacilityType::CargoDepot:
        {
            // A gantry crane over a yard of containers.
            if (beside(main, 10.0f, 24.0f, 12.0f, at, towards))
            {
                const float yaw = built.Chance(0.5f) ? facing(towards) : facing(towards) + glm::half_pi<float>();
                put(Kind::Gantry, at, {18.0f, 11.0f, 9.0f}, yaw, 12.0f);
                const glm::vec2 x{std::cos(yaw), -std::sin(yaw)};
                const glm::vec2 z{std::sin(yaw), std::cos(yaw)};
                const glm::vec3 box{6.06f, 2.59f, 2.44f};
                for (const float along : {-4.5f, 2.5f})
                {
                    for (const float across : {-1.6f, 1.6f})
                    {
                        const glm::vec2 c = at + x * along + z * across;
                        const int stack = built.Int(1, 3);
                        for (int level = 0; level < stack; ++level)
                        {
                            plan.blocks.push_back({Kind::Container, glm::vec3(c.x, O.y + box.y * (0.5f + static_cast<float>(level)) - 0.02f, c.y), box,
                                                   yaw + built.Range(-0.03f, 0.03f)});
                        }
                    }
                }
            }
            break;
        }
        case FacilityType::CommsRelay:
        case FacilityType::Observatory:
        {
            // A dish, or two, turned up to the sky.
            const bool big = plan.type == FacilityType::Observatory;
            for (int i = big ? 1 : built.Int(1, 2); i > 0; --i)
            {
                const float wide = big ? built.Range(11.0f, 14.0f) : built.Range(6.0f, 9.0f);
                if (beside(main, 12.0f, 30.0f, wide * 0.7f, at, towards))
                {
                    put(Kind::Dish, at, {wide, wide, wide}, built.Range(0.0f, 6.28f), wide * 0.7f);
                }
            }
            break;
        }
        default: break;
        }
        // Solar arrays in their fence -- most of all where people live and work far from anything; and a weather station where the
        // place watches the world.
        const float solar = plan.type == FacilityType::Habitat || plan.type == FacilityType::SurveyCamp || plan.type == FacilityType::ResearchLab ? 0.8f
                            : plan.type == FacilityType::CommsRelay || plan.type == FacilityType::Observatory                                 ? 0.6f
                                                                                                                                              : 0.25f;
        if (built.Chance(solar) && beside(main, 10.0f, 30.0f, 11.0f, at, towards))
        {
            const float yaw = facing(-towards); // (its gate, in its -z side, towards the building)
            put(Kind::SolarArray, at, {16.0f, 2.5f, 8.0f}, yaw, 11.0f);
            plan.blocks.push_back({Kind::Fence, glm::vec3(at.x, O.y + 1.1f, at.y), {20.0f, 2.2f, 12.0f}, yaw});
        }
        if ((plan.type == FacilityType::SurveyCamp || plan.type == FacilityType::ResearchLab || plan.type == FacilityType::Observatory) &&
            beside(main, 6.0f, 18.0f, 1.5f, at, towards))
        {
            put(Kind::WeatherStation, at, {1.0f, 6.0f, 1.0f}, built.Range(0.0f, 6.28f), 1.5f);
        }
    }
    // The valleys out into the land from the open ground (SiteTerrain shapes the ground to them), on a draw of their own so
    // nothing else about the site changes: two or three, spread round it, each bending a little and ending in a bowl.
    {
        Random valleys((static_cast<uint64_t>(seed) << 9) ^ 0x7A11E75ull);
        const glm::vec2 middle{plan.origin.x + plan.size * 0.5f, plan.origin.z + plan.size * 0.5f};
        const int valleyCount = valleys.Unit() < 0.5f ? 3 : 2;
        const float first = valleys.Range(0.0f, 6.2831853f);
        for (int i = 0; i < valleyCount; ++i)
        {
            const float angle = first + static_cast<float>(i) * 6.2831853f / static_cast<float>(valleyCount) + valleys.Range(-0.35f, 0.35f);
            const glm::vec2 way{std::cos(angle), std::sin(angle)};
            const glm::vec2 across{-way.y, way.x};
            const float length = valleys.Range(80.0f, 130.0f);
            const float bend = valleys.Range(-22.0f, 22.0f);
            Valley valley;
            valley.halfWidth = valleys.Range(14.0f, 22.0f);
            valley.bowl = valleys.Range(22.0f, 30.0f);
            // From the side of the open ground out, past the band round it and on.
            const float side = plan.size * 0.5f / std::max(std::abs(way.x), std::abs(way.y));
            const glm::vec2 from = middle + way * side;
            valley.line.push_back(from);
            for (int k = 1; k <= 3; ++k)
            {
                const float t = static_cast<float>(k) / 3.0f;
                valley.line.push_back(from + way * (SiteSpec::kRoam + length * t) + across * (bend * std::sin(t * 3.14159265f * 0.9f + 0.4f)));
            }
            valley.end = valley.line.back() + way * (valley.bowl * 0.4f);
            plan.valleys.push_back(valley);
        }
        // In the first one's bowl, the field generator: a housing about the size of a freight container, its long side across
        // the valley, its breaker on the side facing back down it; in the second's, a relay mast.
        const auto backDown = [&](const Valley& valley)
        {
            const glm::vec2 back = valley.line[valley.line.size() - 2] - valley.end;
            return glm::length(back) > 1.0e-3f ? back / glm::length(back) : glm::vec2(0.0f, 1.0f);
        };
        {
            const Valley& valley = plan.valleys[0];
            const glm::vec2 back = backDown(valley);
            const glm::vec3 size{6.06f, 2.6f, 2.44f};
            Block housing{Kind::Generator, glm::vec3(valley.end.x, plan.origin.y + size.y * 0.5f, valley.end.y), size, std::atan2(-back.x, -back.y)};
            housing.outlying = true;
            plan.generator = static_cast<int>(plan.blocks.size());
            plan.blocks.push_back(housing);
        }
        {
            const Valley& valley = plan.valleys[1];
            const float high = valleys.Range(16.0f, 24.0f);
            Block mast{Kind::Mast, glm::vec3(valley.end.x, plan.origin.y + high * 0.5f, valley.end.y), {1.4f, high, 1.4f}, valleys.Range(0.0f, 1.5f)};
            mast.outlying = true;
            plan.relay = static_cast<int>(plan.blocks.size());
            plan.blocks.push_back(mast);
        }
    }
    if (submerged)
    {
        Submerge(plan);
    }
    return plan;
}

void SitePlan::Submerge(SitePlan& plan)
{
    // Nothing that would not be there under the sea: what stood over the land and what lit it, the ways marked over it. (The relay
    // mast stands on, up out of the water; the field generator in its housing; tanks, containers, rocks, pipes and what came down.)
    using Kind = BlockKind;
    std::vector<Block> kept;
    int generator = -1;
    int relay = -1;
    for (size_t i = 0; i < plan.blocks.size(); ++i)
    {
        const Block& block = plan.blocks[i];
        const bool gone = block.kind == Kind::Headframe || block.kind == Kind::FlareStack || block.kind == Kind::Gantry || block.kind == Kind::SolarArray ||
                          block.kind == Kind::Dish || block.kind == Kind::Fence || block.kind == Kind::WeatherStation ||
                          (block.kind == Kind::Mast && static_cast<int>(i) != plan.relay);
        if (gone)
        {
            continue;
        }
        generator = static_cast<int>(i) == plan.generator ? static_cast<int>(kept.size()) : generator;
        relay = static_cast<int>(i) == plan.relay ? static_cast<int>(kept.size()) : relay;
        kept.push_back(block);
    }
    plan.blocks = std::move(kept);
    plan.generator = generator;
    plan.relay = relay;
    plan.signposts.clear();
    plan.paths.clear();
    plan.roads.clear();
    plan.wayIn = WayIn::LitEntrance;
    // Lit by lamps on the buildings' walls instead, a storey up, looking out and down over the floor round them: one on each side.
    plan.lamps.clear();
    for (const FacilityLayout& building : plan.buildings)
    {
        for (int side = 0; side < 4; ++side)
        {
            const glm::ivec2 inward = side == 0 ? glm::ivec2(1, 0) : side == 1 ? glm::ivec2(-1, 0) : side == 2 ? glm::ivec2(0, 1) : glm::ivec2(0, -1);
            const int span = side < 2 ? building.depth : building.width;
            // The cell nearest the middle of this side that has wall facing out this way.
            for (int k = 0; k < span; ++k)
            {
                const int along = span / 2 + ((k % 2 == 0) ? k / 2 : -(k + 1) / 2);
                if (along < 0 || along >= span)
                {
                    continue;
                }
                glm::ivec2 cell = side < 2 ? glm::ivec2(side == 0 ? 0 : building.width - 1, along) : glm::ivec2(along, side == 2 ? 0 : building.depth - 1);
                while (cell.x >= 0 && cell.y >= 0 && cell.x < building.width && cell.y < building.depth && building.Height(cell.x, cell.y) == 0)
                {
                    cell += inward;
                }
                if (cell.x < 0 || cell.y < 0 || cell.x >= building.width || cell.y >= building.depth)
                {
                    continue;
                }
                const glm::vec2 out = -glm::vec2(inward);
                const glm::vec2 face = glm::vec2(building.origin.x, building.origin.z) + (glm::vec2(cell) + glm::vec2(0.5f) + out * 0.5f) * kCell;
                Lamp lamp;
                lamp.kind = LampKind::Flood;
                lamp.position = glm::vec3(face.x + out.x * 0.45f, building.origin.y + 2.9f, face.y + out.y * 0.45f);
                lamp.direction = glm::normalize(glm::vec3(out.x, -0.7f, out.y));
                lamp.range = 16.0f;
                plan.lamps.push_back(lamp);
                break;
            }
        }
    }
}

} // namespace pred
