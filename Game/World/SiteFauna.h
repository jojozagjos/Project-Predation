#pragma once

#include "Game/Creature/CreatureAnatomy.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

struct SitePlan;

// What lives at a site (Phase 7: creatures from the world, not arriving): worked out from the site's plan and its world, from its
// seed, so every machine has the same without being told -- whether anything lives there at all (often nothing does: a place
// should feel empty), and if so what kind of thing, how many, where it has its lair, and what it is doing when the crew lands.
struct SiteFauna
{
    // Nothing; something that lives here, its lair somewhere on the site; or something only passing through, with no lair.
    enum class Presence : uint8_t
    {
        None,
        Lives,
        Passing
    };
    Presence presence = Presence::None;

    // The kind of thing it is -- its colours, its size, its body (CreatureSeed) -- and how many of it. Each one is the kind with
    // its own detail (SeedOf).
    ColourFamily family = ColourFamily::Slate;
    CreatureSize size = CreatureSize::Usual;
    BodyPlan plan = BodyPlan::Quadruped;
    int count = 0;
    uint32_t SeedOf(int one) const;

    // Where it lives: in the dark room of a building far from its way in (FacilityLayout::lairRoom), in a cave's chamber under the
    // ground, or out on the land somewhere sheltered; on its floor. None for one passing through.
    enum class Lair : uint8_t
    {
        None,
        Building,
        Cave,
        Outside
    };
    Lair lair = Lair::None;
    glm::vec3 lairAt{0.0f};
    int lairBuilding = -1;
    int lairCave = -1;

    // What it is doing when the crew lands: about the site; asleep in its lair (it was here first); or at a kill near it.
    enum class Doing : uint8_t
    {
        Prowling,
        Sleeping,
        Feeding
    };
    Doing doing = Doing::Prowling;

    uint32_t seed = 0;

    // What a test asks for (site_fauna), over what the site's seed draws: each -1 for its own.
    struct Wanted
    {
        int presence = -1; // Presence
        int size = -1;     // CreatureSize
        int lair = -1;     // Lair: Building, Cave or Outside
        int doing = -1;    // Doing
    };

    // From the site's plan, its world's biome (universe.json), how far out it is (the signs' strength: 0 home to 1 the far reaches:
    // the further out, the likelier something lives there).
    static SiteFauna Plan(const SitePlan& site, const std::string& biome, float farOut, uint32_t seed, const Wanted& wanted = {});

    bool Any() const { return presence != Presence::None && count > 0; }
    std::string Describe() const;
};

const char* SitePresenceName(SiteFauna::Presence presence);

} // namespace pred
