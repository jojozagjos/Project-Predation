#pragma once

#include "Engine/Assets/ModelAsset.h"
#include "Game/World/FacilityLayout.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

namespace SiteSpec
{
// The corner of the site's open ground: kilometres from the testing area, the lab and the ship (round x 0, z 0 and
// x -1500), so the land round a site (SiteTerrain::FarMesh, out to its horizon) reaches none of them, and none of them is
// seen from it.
inline constexpr glm::vec3 kOrigin{100.0f, 0.0f, 6000.0f};
// How much open ground there is, each way.
inline constexpr float kSize = 300.0f;
// How far past the open ground anybody may go, at the narrowest round it (SiteTerrain::PastRoam), and where the valleys
// out into the land (SitePlan::valleys) start from.
inline constexpr float kRoam = 60.0f;
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
    // What kind of place it is, which decides how much is built there and what lies about it -- a research facility (the
    // most: several buildings, pipes between them), a remote station (two, a radio mast, tanks), a survey site (a field
    // shelter in open, broken ground), a wreck (what is left standing, and a field of debris round it), a signal source (a
    // mast, and what is beside it). Carried in the seed's top three bits, so the one number sent plans the same place
    // everywhere.
    enum class SiteKind : uint8_t
    {
        Facility = 0,
        Station,
        Survey,
        Wreck,
        Signal
    };
    static uint16_t SeedFor(uint16_t seed, SiteKind kind);
    static SiteKind KindOf(uint32_t seed);

    // What the place is for, one per site by what kind of place it is (a research facility may be any of the first five; a
    // station a habitat, a relay or an observatory; a survey a camp or an observatory; a signal a relay): it sets how its
    // buildings look from outside (FacilityLayout::Outside) -- and, to come, what is in them. None for a wreck.
    enum class FacilityType : uint8_t
    {
        None,
        ResearchLab,
        Mining,
        Refinery,
        CargoDepot,
        Habitat,
        CommsRelay,
        Observatory,
        SurveyCamp
    };
    static FacilityType TypeOf(uint32_t seed, SiteKind kind);
    static const char* TypeName(FacilityType type);

    enum class BlockKind : uint8_t
    {
        Rock,      // a boulder: cover
        Container, // a freight container: cover, and a landmark
        Pad,       // where the craft sets down
        PipeX,     // an overhead pipe, running along x
        PipeZ,     // and along z
        Support,   // what holds a pipe up
        Tank,      // a fuel tank, standing on end
        Mast,      // a radio mast: a tall lattice, its foot on the ground
        Debris     // a piece of something broken: a plate of hull, half buried, tipped
    };

    struct Block
    {
        BlockKind kind = BlockKind::Rock;
        glm::vec3 centre{0.0f};
        glm::vec3 size{1.0f};
        float yaw = 0.0f;
        float tip = 0.0f; // tipped over about its own x (debris), radians
        // Out in the land (a valley's end), not on the site's levelled ground: stood where the ground is there, which nobody
        // levels for it.
        bool outlying = false;
    };

    enum class LampKind : uint8_t
    {
        Pole,  // on a pole, over the ground
        Flood, // over a door, on the wall
        Beacon // a short post at the pad's edge, a red light pulsing slowly
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
    SiteKind kind = SiteKind::Facility;
    FacilityType type = FacilityType::None;
    glm::vec3 origin = SiteSpec::kOrigin;
    float size = SiteSpec::kSize;
    std::vector<FacilityLayout> buildings;
    std::vector<Block> blocks;
    std::vector<Lamp> lamps;
    // The middle of the landing pad, half a metre over it, and which way the shuttle standing there faces: its ramp
    // towards the middle of the site.
    glm::vec3 landing{0.0f};
    float landingYaw = 0.0f;
    // Where the shuttle stands: the middle of the pad, on its surface.
    glm::vec3 ShuttleBase() const { return landing - glm::vec3(0.0f, 0.5f, 0.0f); }

    // A place on the ground and which way something standing there faces, turned as a look is: (sin, 0, -cos).
    struct Spot
    {
        glm::vec3 position{0.0f};
        float yaw = 0.0f;
    };
    // Off the foot of the shuttle's ramp, facing into the site: where somebody put on the site with nothing to bring them
    // stands. Kept clear.
    Spot rampFoot;
    Sky sky;
    // The valleys out into the land from the site's open ground (SiteTerrain shapes the ground to them): each a line from the
    // side of the open ground out (a few bends), how wide it is either side of that, and the bowl at its end (its middle,
    // how far round). Two or three, spread round the site.
    struct Valley
    {
        std::vector<glm::vec2> line;
        float halfWidth = 20.0f;
        glm::vec2 end{0.0f};
        float bowl = 26.0f;
    };
    std::vector<Valley> valleys;
    // And what stands out there (blocks, outlying): a field generator in one valley's bowl -- a housing the site's power can
    // come from, its breaker on the side facing back down the valley -- and a relay mast in another. -1: none.
    int generator = -1;
    int relay = -1;
    // A windsock by the pad, where it stands (on the ground there; nought: none): swung and filled by the wind (SiteMap).
    glm::vec3 windsock{0.0f};
    // Ground kept low, whatever the land round it does (world x0, z0, x1, z1): the way a ship comes in at an outpost.
    std::vector<glm::vec4> keepLow;
    // Ground levelled at the plan's height (world x0, z0, x1, z1), the land blending into it round about: what an outpost has
    // poured.
    std::vector<glm::vec4> levelled;
    // What stands out on the land round an outpost, each where the ground is there (SiteMap): its parts about its foot (up from
    // the ground), solid and only to be seen, stood on the lowest of the ground within `reach` of its foot (world x, z).
    struct Outlying
    {
        glm::vec2 at{0.0f};
        float reach = 0.5f;
        std::vector<ModelPart> solid;
        std::vector<ModelPart> dressing;
    };
    std::vector<Outlying> outlying;
    // Roads out across the land (an outpost's road in): along these points (world x, z), so wide -- the ground graded along
    // them, worn to gravel, nothing growing on them (SiteTerrain).
    struct Road
    {
        std::vector<glm::vec2> line;
        float width = 8.0f;
    };
    std::vector<Road> roads;
    // No cliffs or ravines round where anybody may go, only the soft limit, the land running on (an outpost).
    bool openEdge = false;

    static SitePlan Generate(uint32_t seed);

    // The ground a building stands on, in the world, grown by `margin` metres all round.
    static void Footprint(const FacilityLayout& building, float margin, glm::vec2& min, glm::vec2& max);
    // Whether a point on the ground is under a building or on what is poured (levelled), or within `margin` of one.
    bool InBuilding(glm::vec2 xz, float margin) const;
    // Whether a point is inside one: within its outer wall and under its roof -- or, with a `margin`, that close to being.
    bool Indoors(const glm::vec3& at, float margin = 0.0f) const;
};

} // namespace pred
