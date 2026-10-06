#include "Game/World/SitePlan.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
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
    return building.origin + glm::vec3(cells.x * kCell, 0.0f, cells.y * kCell);
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
    return false;
}

bool SitePlan::Indoors(const glm::vec3& at) const
{
    for (const FacilityLayout& building : buildings)
    {
        glm::vec2 min;
        glm::vec2 max;
        Footprint(building, 0.0f, min, max);
        const float roof = building.origin.y + static_cast<float>(building.floors) * FacilityLayout::kStorey - FacilityLayout::kSlab;
        if (at.x > min.x && at.x < max.x && at.z > min.y && at.z < max.y && at.y > building.origin.y - 0.5f && at.y < roof)
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

SitePlan SitePlan::Generate(uint32_t seed)
{
    SitePlan plan;
    plan.seed = seed;
    plan.kind = KindOf(seed);
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
            if (DistanceTo(landingLocal, corner, corner + extent) < 30.0f)
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
        plan.buildings.push_back(FacilityLayout::Generate(Mix(seed, static_cast<uint32_t>(b) + 1u), options));
        // Each building on circuits of its own (a hundred apart), so one losing its power leaves the rest lit.
        const int circuitBase = 100 * static_cast<int>(plan.buildings.size());
        for (FacilityLayout::Lamp& lamp : plan.buildings.back().lamps)
        {
            lamp.circuit += circuitBase;
        }
    }

    {
        const glm::vec2 into{std::sin(plan.landingYaw), -std::cos(plan.landingYaw)};
        plan.rampFoot.position = world(landingLocal.x + into.x * 11.0f, landingLocal.y + into.y * 11.0f);
        plan.rampFoot.yaw = plan.landingYaw;
    }

    // The ground, a little below the buildings' floors so the two never fight over which is drawn.
    plan.blocks.push_back({Kind::Ground, world(S * 0.5f, S * 0.5f) + glm::vec3(0.0f, -0.27f, 0.0f), {S + 90.0f, 0.5f, S + 90.0f}, 0.0f});

    // Rock all the way round: a ragged wall, and a taller one behind it that fills the wall's gaps.
    for (int side = 0; side < 4; ++side)
    {
        for (int row = 0; row < 2; ++row)
        {
            for (float t = -14.0f; t < S + 14.0f; t += random.Range(8.0f, 12.0f))
            {
                const float width = random.Range(11.0f, 17.0f);
                const float deep = random.Range(12.0f, 18.0f);
                const float height = row == 0 ? random.Range(9.0f, 18.0f) : random.Range(18.0f, 28.0f);
                const float out = (row == 0 ? 2.0f : -8.0f) + random.Range(-3.0f, 3.0f); // how far its face is inside the edge
                glm::vec3 centre;
                glm::vec3 size;
                if (side < 2)
                {
                    const float u = side == 0 ? out - deep * 0.5f : S - out + deep * 0.5f;
                    centre = world(u, t);
                    size = {deep, height, width};
                }
                else
                {
                    const float v = side == 2 ? out - deep * 0.5f : S - out + deep * 0.5f;
                    centre = world(t, v);
                    size = {width, height, deep};
                }
                centre.y = height * 0.5f - 1.0f;
                plan.blocks.push_back({Kind::Cliff, centre, size, random.Range(-0.25f, 0.25f)});
            }
        }
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
            lamp.position = ExitPoint(building, exit, 0.35f) + glm::vec3(0.0f, 3.1f, 0.0f);
            lamp.direction = glm::normalize(glm::vec3(out.x * 0.7f, -1.0f, out.y * 0.7f));
            lamp.range = 15.0f;
            lamp.mood = PickMood(random, 0.8f);
            plan.lamps.push_back(lamp);
        }
        if (building.exits.empty())
        {
            continue;
        }
        const glm::vec2 from = flat(plan.landing);
        const glm::vec2 to = flat(ExitPoint(building, building.exits.front(), 4.0f));
        const float length = glm::length(to - from);
        const glm::vec2 dir = (to - from) / std::max(length, 1.0e-3f);
        const glm::vec2 across{-dir.y, dir.x};
        int n = 0;
        // Stopping well short of the door, where a vehicle waits.
        for (float d = 16.0f; d < length - 16.0f; d += random.Range(20.0f, 26.0f), ++n)
        {
            const glm::vec2 at = from + dir * d + across * ((n % 2 == 0) ? 3.5f : -3.5f);
            if (plan.InBuilding(at, 3.0f))
            {
                continue;
            }
            SitePlan::Lamp lamp;
            lamp.kind = LampKind::Pole;
            lamp.position = glm::vec3(at.x, O.y + 5.0f, at.y);
            lamp.range = 16.0f;
            lamp.mood = PickMood(random, 0.6f);
            plan.lamps.push_back(lamp);
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
    // A pipe overhead from each building to the next, on supports: something to follow in the dark.
    for (size_t b = 0; b + 1 < plan.buildings.size(); ++b)
    {
        glm::vec2 aMin, aMax, bMin, bMax;
        Footprint(plan.buildings[b], 0.0f, aMin, aMax);
        Footprint(plan.buildings[b + 1], 0.0f, bMin, bMax);
        // Out of the side of one nearest the other, along x, then along z into the side of the other.
        const glm::vec2 aCentre = (aMin + aMax) * 0.5f;
        const glm::vec2 bCentre = (bMin + bMax) * 0.5f;
        const float startX = bCentre.x > aCentre.x ? aMax.x : aMin.x;
        const float startZ = glm::clamp(bCentre.y, aMin.y + 2.0f, aMax.y - 2.0f);
        const float turnX = glm::clamp(aCentre.x + (bCentre.x - aCentre.x), bMin.x + 2.0f, bMax.x - 2.0f);
        const float endZ = bCentre.y > startZ ? bMin.y : bMax.y;
        const glm::vec2 corner{turnX, startZ};
        // Only the one way round: out of the first clear of it, and the corner outside the second. Buildings
        // side by side, or one straight behind the other, get none.
        bool blocked = (turnX > aMin.x - 3.0f && turnX < aMax.x + 3.0f) ||
                       (corner.x > bMin.x - 3.0f && corner.x < bMax.x + 3.0f && corner.y > bMin.y - 3.0f && corner.y < bMax.y + 3.0f);
        // Not across a third building, and not over the pad.
        for (size_t k = 0; k < plan.buildings.size(); ++k)
        {
            if (k == b || k == b + 1)
            {
                continue;
            }
            glm::vec2 kMin, kMax;
            Footprint(plan.buildings[k], 2.0f, kMin, kMax);
            const bool crossesX = std::max(startX, turnX) > kMin.x && std::min(startX, turnX) < kMax.x && startZ > kMin.y && startZ < kMax.y;
            const bool crossesZ = std::max(startZ, endZ) > kMin.y && std::min(startZ, endZ) < kMax.y && turnX > kMin.x && turnX < kMax.x;
            blocked = blocked || crossesX || crossesZ;
        }
        const glm::vec2 pad = flat(plan.landing);
        blocked = blocked || DistanceTo(pad, glm::min(glm::vec2(startX, startZ), corner), glm::max(glm::vec2(startX, startZ), corner)) < kPadHalf + 3.0f ||
                  DistanceTo(pad, glm::min(corner, glm::vec2(turnX, endZ)), glm::max(corner, glm::vec2(turnX, endZ))) < kPadHalf + 3.0f;
        if (blocked || std::abs(turnX - startX) < 4.0f || std::abs(endZ - startZ) < 4.0f)
        {
            continue;
        }
        constexpr float kPipeHeight = 2.7f; // clear of a head, and of most of what walks
        constexpr float kPipeThick = 0.7f;
        plan.blocks.push_back({Kind::PipeX, glm::vec3((startX + turnX) * 0.5f, O.y + kPipeHeight, startZ),
                               {std::abs(turnX - startX) + kPipeThick * 0.5f, kPipeThick, kPipeThick}, 0.0f});
        plan.blocks.push_back({Kind::PipeZ, glm::vec3(turnX, O.y + kPipeHeight, (startZ + endZ) * 0.5f),
                               {kPipeThick, kPipeThick, std::abs(endZ - startZ) + kPipeThick * 0.5f}, 0.0f});
        const auto supports = [&](glm::vec2 from, glm::vec2 to)
        {
            const float length = glm::length(to - from);
            for (float d = 3.0f; d < length - 2.0f; d += 7.0f)
            {
                const glm::vec2 at = from + (to - from) * (d / length);
                plan.blocks.push_back({Kind::Support, glm::vec3(at.x, O.y + (kPipeHeight - kPipeThick * 0.5f) * 0.5f, at.y),
                                       {0.3f, kPipeHeight - kPipeThick * 0.5f, 0.3f}, 0.0f});
            }
        };
        supports({startX, startZ}, corner);
        supports(corner, {turnX, endZ});
    }
    return plan;
}

} // namespace pred
