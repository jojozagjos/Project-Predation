#pragma once

#include "Engine/Render/Mesh.h"
#include "Game/World/SiteTerrain.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

struct SitePlan;

// The great things of the land round a site, to see and find your way by: rock arches, spires, flat-topped buttes, balanced
// rocks, giant trees, crystal, towers of ice -- by the world and the shape of its land, each its own shape. Planned from the
// site's seed, so every machine has the same ones in the same places.
namespace Landmark
{

enum class Kind : uint8_t
{
    None,
    Arch,
    Spire,
    Butte,
    BalancedRock,
    GreatTree,
    Crystals,
    IceTower
};

struct Placed
{
    Kind kind = Kind::None;
    glm::vec3 at{0.0f};  // its foot, on the ground
    float yaw = 0.0f;    // radians
    float size = 0.0f;   // metres: how high (a butte, how wide)
    float radius = 0.0f; // metres round its foot that nothing else grows or lies in
    uint32_t seed = 0;   // its own shape's
    glm::vec3 tint{1.0f}; // its own shade of the world's rock
};

// Which, and where, over the land round the site out to where it is drawn: by the world, in groups where its land makes
// them, some near and some on the horizon and sometimes none close -- never on the site's own ground.
std::vector<Placed> Scatter(uint32_t seed, const std::string& biome, SiteTerrain::Shape shape, const SitePlan& plan, const SiteTerrain& terrain);

// Its shape where it stands, in the world (no transform), painted its own shade for its material to colour: the rock kinds and the
// crystal and ice; empty for a giant tree, which is made of a tree's own shapes (SiteScatter), scaled up.
MeshData Shape(const Placed& placed);

const char* Name(Kind kind);

} // namespace Landmark

} // namespace pred
