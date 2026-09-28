#pragma once

#include "Game/World/FacilityLayout.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

namespace SiteSpec
{
// The corner of the site, east of the testing area: none of it reaches the test map (which ends at x = 45)
// or the lab south of it.
inline constexpr glm::vec3 kOrigin{70.0f, 0.0f, -110.0f};
// How much open ground there is inside the rock that closes it in, each way.
inline constexpr float kSize = 190.0f;
// The site every machine builds until the host says otherwise.
inline constexpr uint16_t kDefaultSeed = 1;
} // namespace SiteSpec

// A mission site, planned from a seed: a few buildings standing on open ground, closed in by rock, with a
// place to land, the routes between them and things along the way to get behind.
//
// Only a plan, like each of its buildings (a FacilityLayout apiece): SiteMap builds it, and every machine
// plans the same site from the same seed, so only the seed is ever sent.
//
// The shape of it, by design:
// - more than one building, so a mission can send people across open ground and a creature can come from
//   outside as well as through the ducts;
// - each with more than one way in, so there is always another route;
// - closed in by rock rather than by anything invisible: the edge of the site is a thing you can see;
// - lit badly outside, by floodlights over the doors and the landing and a few along the way, some of
//   them not working.
struct SitePlan
{
    enum class BlockKind : uint8_t
    {
        Ground,    // what everything stands on
        Cliff,     // the rock that closes the site in
        Rock,      // a boulder: cover
        Container, // a freight container: cover, and a landmark
        Pad,       // where the craft sets down
        PipeX,     // an overhead pipe, running along x
        PipeZ,     // and along z
        Support,   // what holds a pipe up
        Tank       // a fuel tank, standing on end
    };

    struct Block
    {
        BlockKind kind = BlockKind::Rock;
        glm::vec3 centre{0.0f};
        glm::vec3 size{1.0f};
        float yaw = 0.0f;
    };

    enum class LampKind : uint8_t
    {
        Pole,  // on a pole, over the ground
        Flood  // over a door, on the wall
    };

    struct Lamp
    {
        LampKind kind = LampKind::Pole;
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f, -1.0f, 0.0f};
        float range = 16.0f;
        FacilityLayout::LampMood mood = FacilityLayout::LampMood::Steady;
    };

    // What it is like outside: the light there is (a low moon, or none), and how far you can see.
    struct Sky
    {
        glm::vec3 sunDirection{-0.3f, -0.6f, -0.4f};
        glm::vec3 sunColor{0.55f, 0.62f, 0.8f};
        float sunIntensity = 0.12f;
        glm::vec3 ambientSky{0.02f, 0.025f, 0.035f};
        glm::vec3 ambientGround{0.01f, 0.01f, 0.012f};
        glm::vec3 fogColor{0.02f, 0.025f, 0.03f};
        float fogStart = 6.0f;
        float fogEnd = 60.0f;
    };

    uint32_t seed = 0;
    glm::vec3 origin = SiteSpec::kOrigin;
    float size = SiteSpec::kSize;
    std::vector<FacilityLayout> buildings;
    std::vector<Block> blocks;
    std::vector<Lamp> lamps;
    // Where everybody arrives, and which way they face: into the site.
    glm::vec3 landing{0.0f};
    float landingYaw = 0.0f;
    Sky sky;

    static SitePlan Generate(uint32_t seed);

    // The ground a building stands on, in the world, grown by `margin` metres all round.
    static void Footprint(const FacilityLayout& building, float margin, glm::vec2& min, glm::vec2& max);
    // Whether a point on the ground is under a building, or within `margin` of one.
    bool InBuilding(glm::vec2 xz, float margin) const;
};

} // namespace pred
