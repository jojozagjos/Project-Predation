#include "Game/World/SiteFauna.h"

#include "Game/Creature/CreatureTraits.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/SitePlan.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace pred
{

namespace
{

int Pick(SeededRandom& random, int count)
{
    return std::min(static_cast<int>(random.Unit() * static_cast<float>(count)), count - 1);
}

// The colours that suit a kind of world: the ash and embers of a volcanic one, the sand and ochre of a desert, the blue-black of
// the deep on an ocean world. A world not here takes any.
std::vector<ColourFamily> FamiliesFor(const std::string& biome)
{
    using F = ColourFamily;
    if (biome == "frozen")
    {
        return {F::Ice, F::Slate, F::BoneRust, F::Abyss};
    }
    if (biome == "rocky")
    {
        return {F::BoneRust, F::Slate, F::Amber, F::Ochre, F::Blood};
    }
    if (biome == "barren")
    {
        return {F::BoneRust, F::Obsidian, F::Flesh, F::Amber};
    }
    if (biome == "desert")
    {
        return {F::Sand, F::Ochre, F::Amber, F::Blood};
    }
    if (biome == "volcanic")
    {
        return {F::Ember, F::Obsidian, F::Blood};
    }
    if (biome == "oceanic")
    {
        return {F::Abyss, F::Iridescent, F::Violet, F::Slate};
    }
    if (biome == "temperate")
    {
        return {F::Moss, F::Jungle, F::Ochre, F::Blood};
    }
    if (biome == "jungle")
    {
        return {F::Jungle, F::Moss, F::Toxic, F::Iridescent};
    }
    if (biome == "toxic")
    {
        return {F::Toxic, F::Violet, F::Flesh};
    }
    if (biome == "stormworld")
    {
        return {F::Slate, F::Violet, F::Abyss, F::Obsidian};
    }
    if (biome == "crystalline")
    {
        return {F::Iridescent, F::Violet, F::Ice, F::Amber};
    }
    std::vector<F> all;
    for (int f = 0; f < static_cast<int>(F::Count); ++f)
    {
        all.push_back(static_cast<F>(f));
    }
    return all;
}

} // namespace

const char* SitePresenceName(SiteFauna::Presence presence)
{
    switch (presence)
    {
    case SiteFauna::Presence::None:
        return "nothing";
    case SiteFauna::Presence::Lives:
        return "lives here";
    case SiteFauna::Presence::Passing:
        return "passing through";
    }
    return "?";
}

uint32_t SiteFauna::SeedOf(int one) const
{
    // The same kind -- colours, size, body -- and its own detail: no two of a group alike.
    uint32_t h = seed * 2654435761u + static_cast<uint32_t>(one) * 40503u + 0x6A09E667u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return CreatureSeed::Chosen(h, family, size, plan);
}

SiteFauna SiteFauna::Plan(const SitePlan& site, const std::string& biome, float farOut, uint32_t seed, const Wanted& wanted)
{
    SiteFauna fauna;
    fauna.seed = seed;
    SeededRandom random((static_cast<uint64_t>(seed) << 17) ^ 0xFA0A5u);
    // Often nothing: a place is as likely as not to be empty near home, more often occupied the further out it is. (Where it is, it
    // matters: it lives there, or came through.)
    const float occupied = std::clamp(0.4f + 0.35f * farOut, 0.0f, 0.85f);
    const bool here = random.Unit() < occupied;
    fauna.presence = random.Unit() < 0.75f ? Presence::Lives : Presence::Passing;
    // A giant: on the sea's floor most of the time, a leviathan; on land, now and then, a strider -- and then nothing else.
    {
        const float giantDraw = random.Unit();
        const float chance = site.submerged ? 0.75f : std::clamp(0.06f + 0.12f * farOut, 0.0f, 0.2f);
        fauna.giant = wanted.giant >= 0 ? wanted.giant > 0 : giantDraw < chance;
        if (fauna.giant)
        {
            fauna.giantSwims = site.submerged;
            const std::vector<ColourFamily> families = FamiliesFor(biome);
            const ColourFamily family = families[static_cast<size_t>(random.Unit() * static_cast<float>(families.size())) % families.size()];
            fauna.giantSeed = CreatureSeed::Chosen(static_cast<uint32_t>(random.Next()), family, CreatureSize::Big, BodyPlan::Count);
            const float angle = random.Range(0.0f, 6.2831853f);
            const float out = site.size * 0.5f + random.Range(60.0f, 140.0f);
            fauna.giantAt = glm::vec2(site.origin.x, site.origin.z) + glm::vec2(site.size * 0.5f) + glm::vec2(std::cos(angle), std::sin(angle)) * out;
            fauna.presence = Presence::None;
            return fauna;
        }
    }
    if (wanted.presence >= 0)
    {
        fauna.presence = static_cast<Presence>(std::clamp(wanted.presence, 0, 2));
    }
    else if (!here)
    {
        fauna.presence = Presence::None;
    }
    if (fauna.presence == Presence::None)
    {
        return fauna;
    }

    // Its kind: colours for its world, a kind of body, a size -- small ones in a group (into the vents), a big one alone and never in
    // a building (it would not get through its doors).
    const std::vector<ColourFamily> families = FamiliesFor(biome);
    fauna.family = families[static_cast<size_t>(Pick(random, static_cast<int>(families.size())))];
    fauna.plan = static_cast<BodyPlan>(Pick(random, static_cast<int>(BodyPlan::Count)));
    const float size = random.Unit();
    fauna.size = size < 0.12f ? CreatureSize::Tiny : size < 0.3f ? CreatureSize::Small : size < 0.82f ? CreatureSize::Usual : CreatureSize::Big;
    if (wanted.size > 0)
    {
        fauna.size = static_cast<CreatureSize>(std::clamp(wanted.size, 1, 4));
    }
    fauna.count = fauna.size == CreatureSize::Tiny    ? 3 + Pick(random, 2)
                  : fauna.size == CreatureSize::Small ? 2 + Pick(random, 2)
                  : fauna.size == CreatureSize::Usual ? (random.Unit() < 0.15f ? 2 : 1)
                                                      : 1;

    if (fauna.presence == Presence::Passing)
    {
        fauna.doing = Doing::Prowling;
        return fauna;
    }

    // Its lair: a cave's chamber where the site has one, as often as not; else the dark room far from a building's way in; else out
    // on the land, somewhere off the beaten track. A big one in a cave or outside only.
    const bool big = fauna.size == CreatureSize::Big;
    std::vector<int> rooms;
    for (size_t b = 0; b < site.buildings.size(); ++b)
    {
        if (site.buildings[b].lairRoom >= 0)
        {
            rooms.push_back(static_cast<int>(b));
        }
    }
    const bool caves = !site.caves.empty() && !site.caves.front().chambers.empty();
    float where = random.Unit();
    // (Or where a test asks: a building's lair, a cave's, or one outside.)
    if (wanted.lair == static_cast<int>(Lair::Building) && !rooms.empty())
    {
        where = 1.0f;
    }
    if (wanted.lair == static_cast<int>(Lair::Cave) && caves)
    {
        where = 0.0f;
    }
    if (caves && wanted.lair != static_cast<int>(Lair::Outside) && (where < 0.55f || big || rooms.empty()))
    {
        fauna.lair = Lair::Cave;
        fauna.lairCave = Pick(random, static_cast<int>(site.caves.size()));
        const SitePlan::Cave& cave = site.caves[static_cast<size_t>(fauna.lairCave)];
        // Its deepest chamber: the furthest from the way in.
        float furthest = -1.0f;
        for (const glm::vec4& chamber : cave.chambers)
        {
            const float away = glm::distance(glm::vec3(chamber), cave.mouth);
            if (away > furthest)
            {
                furthest = away;
                fauna.lairAt = glm::vec3(chamber);
            }
        }
        fauna.lairAt.y = cave.FloorUnder(fauna.lairAt);
        // Under its water, a flooded cave's: nothing that breathes lives down there (yet).
        if (fauna.lairAt.y < cave.water)
        {
            fauna.lair = Lair::None;
        }
    }
    if (fauna.lair == Lair::None && !big && !rooms.empty() && wanted.lair != static_cast<int>(Lair::Outside))
    {
        fauna.lair = Lair::Building;
        fauna.lairBuilding = rooms[static_cast<size_t>(Pick(random, static_cast<int>(rooms.size())))];
        const FacilityLayout& building = site.buildings[static_cast<size_t>(fauna.lairBuilding)];
        const FacilityLayout::Room& room = building.rooms[static_cast<size_t>(building.lairRoom)];
        fauna.lairAt = FacilityMap::ToWorld(building, room.floor, glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f);
    }
    if (fauna.lair == Lair::None)
    {
        // Out on the land: well away from where the shuttle sets down, towards an edge of the site.
        fauna.lair = Lair::Outside;
        const glm::vec3 middle = site.origin + glm::vec3(site.size * 0.5f, 0.0f, site.size * 0.5f);
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const float angle = random.Range(0.0f, 6.2831853f);
            const float reach = random.Range(0.3f, 0.45f) * site.size;
            const glm::vec3 at = middle + glm::vec3(std::cos(angle) * reach, 0.0f, std::sin(angle) * reach);
            if (glm::distance(glm::vec3(at.x, 0.0f, at.z), glm::vec3(site.landing.x, 0.0f, site.landing.z)) > 60.0f && !site.Indoors(at, 6.0f))
            {
                fauna.lairAt = at;
                break;
            }
        }
    }
    // It was here first: as often as not asleep in its lair when the crew lands, now and then at a kill near it, otherwise about.
    const float doing = random.Unit();
    fauna.doing = doing < 0.45f ? Doing::Sleeping : doing < 0.62f ? Doing::Feeding : Doing::Prowling;
    if (wanted.doing >= 0)
    {
        fauna.doing = static_cast<Doing>(std::clamp(wanted.doing, 0, 2));
    }
    return fauna;
}

std::string SiteFauna::Describe() const
{
    if (giant)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "a giant -- %s (seed %u) -- starting at %.0f %.0f", giantSwims ? "a leviathan in the sea" : "a strider on the land",
                      giantSeed, giantAt.x, giantAt.y);
        return line;
    }
    if (!Any())
    {
        return "nothing lives here";
    }
    static const char* const kLairs[] = {"no lair", "a lair in a building", "a lair in a cave", "a lair out on the land"};
    static const char* const kDoing[] = {"about", "asleep", "feeding"};
    static const char* const kSizes[] = {"any", "tiny", "small", "usual", "big"};
    char line[256];
    std::snprintf(line, sizeof(line), "%d %s %s %s (%s), %s; %s at %.0f %.0f %.0f", count, kSizes[static_cast<size_t>(size)],
                  ColourFamilyName(family), BodyPlanName(plan), SitePresenceName(presence), kDoing[static_cast<size_t>(doing)],
                  kLairs[static_cast<size_t>(lair)], lairAt.x, lairAt.y, lairAt.z);
    return line;
}

} // namespace pred
