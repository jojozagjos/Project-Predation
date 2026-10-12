#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

struct SitePlan;
class SiteTerrain;

// What lies about on a site's ground, by the world it is on: boulders and rock spires, ice shards on a frozen world,
// crystal on a crystalline one, basalt columns on a volcanic one, shrubs, trees and grass where things grow, dead trees and
// scrub where they have stopped. In clusters, as such things are, on the world's own ground only -- never on what was
// levelled for buildings, pads and the ways between them (SiteTerrain::Wildness) -- and only on slopes they could stand
// on. Planned from the seed, so every machine has the same.
namespace SiteScatter
{

enum class Kind : uint8_t
{
    Boulder,
    Spire,
    IceShard,
    Crystal,
    Basalt,
    Shrub,
    Tree,
    DeadTree,
    Grass,
    Count
};

// One thing on the ground: which of its kind's shapes (`variant`), where its foot is, how it is turned and tipped (radians),
// and how big (metres, as its shape is about a metre).
struct Item
{
    Kind kind = Kind::Boulder;
    int variant = 0;
    glm::vec3 at{0.0f};
    float yaw = 0.0f;
    float tip = 0.0f;
    glm::vec3 scale{1.0f};
};

// How many shapes each kind has to choose from.
inline constexpr int kVariants = 4;

// What lies on a site of this world (the biome's id: universe.json), and where.
std::vector<Item> Plan(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain);

// A kind's shape, about a metre across (a tree or a spire a metre tall), its foot at the origin, faceted; what grows painted
// `growth` (on its vertices: a tree's bark is painted its own), the rest white, for its material to colour.
MeshData Shape(Kind kind, int variant, uint32_t seed, const glm::vec3& growth);

// Its leaves, on cards cut out by the foliage atlas (Foliage; Material::cutout), tinted `growth`: empty for a kind that has none.
MeshData Leaves(Kind kind, int variant, uint32_t seed, const glm::vec3& growth);

// Whether one of a kind is solid at this size (a boulder you walk round; grass you walk through).
bool Solid(Kind kind, const glm::vec3& scale);

} // namespace SiteScatter

} // namespace pred
