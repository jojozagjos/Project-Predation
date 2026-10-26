#include "Game/World/SitePlan.h"

#include "Game/World/FacilityMap.h"

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

SitePlan SitePlan::Generate(uint32_t seed)
{
    SitePlan plan;
    plan.seed = seed;
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
        options.massing = MassingOf(plan.type, seed, static_cast<int>(plan.buildings.size()));
        // Basements under some (you, 2026-10-20): the main building more often, now and then two deep. On a draw of their own.
        {
            const uint32_t deep = Mix(seed, 0xBA5E0u + static_cast<uint32_t>(b)) % 100u;
            options.basements = b == 0 ? (deep < 15u ? 2 : deep < 60u ? 1 : 0) : (deep < 30u ? 1 : 0);
        }
        plan.buildings.push_back(FacilityLayout::Generate(Mix(seed, static_cast<uint32_t>(b) + 1u), options));
        planned.push_back({options, Mix(seed, static_cast<uint32_t>(b) + 1u)});
        finish(plan.buildings.size() - 1);
    }

    // Now and then (you, 2026-10-26: sometimes, not often) the main building joined to a neighbour under the ground: both
    // planned again with a floor under the ground and a doorway down there facing the other, and a passage between the two
    // through the site's grid -- out from one, along, and in to the other -- under no building. The nearest neighbour it can
    // be done with, or none. On a draw of its own.
    if (kind != SiteKind::Wreck && plan.buildings.size() >= 2 && Mix(seed, 0x7E44E1u) % 100u < 22u)
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
            // (Under its canopy, where it has one.)
            lamp.position = ExitPoint(building, exit, 0.35f) + glm::vec3(0.0f, building.outside.canopies ? 2.7f : 3.1f, 0.0f);
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
            plan.blocks.push_back({Kind::PipeX, glm::vec3((x0 + corner.x) * 0.5f, y, start.y), {std::abs(corner.x - x0), kPipeThick, kPipeThick}, 0.0f});
            plan.blocks.push_back({Kind::PipeZ, glm::vec3(corner.x, y, (corner.y + z1) * 0.5f), {kPipeThick, kPipeThick, std::abs(z1 - corner.y)}, 0.0f});
            plan.blocks.push_back({Kind::Elbow, glm::vec3(corner.x, y, corner.y), glm::vec3(kPipeThick + 0.12f), 0.0f});
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
                    plan.blocks.push_back({Kind::Support, glm::vec3(at.x, O.y + high * 0.5f, at.y), {0.3f, high, 1.0f}, yaw});
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
            beacon.position = glm::vec3(at.x, plan.origin.y + 1.0f, at.y);
            beacon.range = 9.0f;
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
        // In the first one's bowl, the field generator: a housing the size of a freight container, its long side across
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
            Block housing{Kind::Container, glm::vec3(valley.end.x, plan.origin.y + size.y * 0.5f, valley.end.y), size, std::atan2(-back.x, -back.y)};
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
    return plan;
}

} // namespace pred
