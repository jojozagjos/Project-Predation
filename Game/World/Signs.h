#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

struct SitePlan;
class SiteTerrain;

// Signs of the organism in the land beyond the creatures, at the rare site that has any: formations of rock that branch out
// over the ground like roots or veins from a half-buried knot, and round the stronger of them glossy growths on the ground.
// How plain they are is the site's strength (0 to 1, from how far it is from home: SiteMap::Look::signs) -- faint, small and
// the colour of the rock near home, larger and darker with a wet sheen further out. And one rhythm they all keep, the same on
// every world (Pulse). Planned from the site's seed, so every machine has the same; no text, nothing said about them.
namespace Signs
{

struct Formation
{
    glm::vec3 at{0.0f}; // its knot, on the ground
    float size = 10.0f; // metres its branches reach
    float yaw = 0.0f;
    uint32_t seed = 0;
};

// Where they are on a site with signs of `strength` (none at 0): one or a few, on its own ground and round it.
std::vector<Formation> Make(uint32_t seed, float strength, const SitePlan& plan, const SiteTerrain& terrain);

// One formation's rock, in the world, white for its material to colour; and the growths on the ground round it (empty for a
// faint one).
MeshData Rock(const Formation& formation, float strength, const SiteTerrain& terrain);
MeshData Growths(const Formation& formation, float strength, const SiteTerrain& terrain);

// The rhythm, 0 to 1: a soft swell every `rhythm` seconds of the campaign's clock, the same everywhere.
float Pulse(double seconds, float rhythm);

} // namespace Signs

} // namespace pred
