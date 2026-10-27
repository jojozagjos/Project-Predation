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
// How far past the site's open ground Plan's own trees go (every leaf of them); FarTrees has those further out.
inline constexpr float kTreeReach = 200.0f;

// What lies on a site of this world (the biome's id: universe.json), and where.
// `clearings`: (x, z, radius) of ground kept clear, round the landmarks.
std::vector<Item> Plan(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain,
                       const std::vector<glm::vec3>& clearings = {});

// The rocks of the land round the site past kTreeReach, as FarTrees has its trees: the larger rocks, spires, ice, crystal and
// basalt of the world (fewer than near, and bigger, standing for the rest), each a few faces painted its colour, out to where
// one is a pixel -- so the ground does not go bare past the site. Drawn only.
MeshData FarScatter(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain, const glm::vec3& rock,
                    const std::vector<glm::vec3>& clearings = {}, bool airless = false);

// A kind's shape, about a metre across (a tree or a spire a metre tall), its foot at the origin, faceted: a tree's bark painted
// on its vertices, the rest white, for its material to colour (what grows on it is Leaves).
MeshData Shape(Kind kind, int variant, uint32_t seed);

// Its leaves, on cards cut out by the foliage atlas (Foliage; Material::cutout), tinted `growth`: empty for a kind that has none.
MeshData Leaves(Kind kind, int variant, uint32_t seed, const glm::vec3& growth);

// How wooded a place is on a world, 0 to 1: its woods some hundreds of metres across and the clearings between (none where
// no trees grow). Plan's trees and FarTrees grow by it, the grass thins under it (SiteMap).
float Woodland(uint32_t seed, const std::string& biome, float x, float z);

// The trees of the land round the site past kTreeReach (Plan has those nearer), out to where one is a pixel: each a crown of
// a few faces, painted `growth`, and a trunk -- drawn only, nothing to walk into.
MeshData FarTrees(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain, const glm::vec3& growth,
                  const std::vector<glm::vec3>& clearings = {});

// Whether one of a kind is solid at this size (a boulder you walk round; grass you walk through).
bool Solid(Kind kind, const glm::vec3& scale);

} // namespace SiteScatter

} // namespace pred
