#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

struct SitePlan;
struct SiteFauna;
class SiteTerrain;

// Signs of the organism in the land beyond the creatures, where one lives or has come through (SiteFauna): formations that
// branch out over the ground like roots or veins from a half-buried knot, in the creature's own colours (SiteMap paints them).
// Round its lair where it lives -- the plainer the nearer, outside the building or the cave it has gone into, or at the lair itself
// out on the land -- and along the way of one only passing through; none where nothing lives. How plain they are is the site's
// strength (0 to 1: SiteMap::SignStrength). And one rhythm they all keep, the same on every world (Pulse). Planned from the
// site's seed, so every machine has the same; no text, nothing said about them.
namespace Signs
{

struct Formation
{
    glm::vec3 at{0.0f}; // its knot, on the ground
    float size = 10.0f; // metres its branches reach
    float yaw = 0.0f;
    uint32_t seed = 0;
};

// Where they are on a site with signs of `strength` (none at 0), by what lives there.
std::vector<Formation> Make(uint32_t seed, float strength, const SitePlan& plan, const SiteTerrain& terrain, const SiteFauna& fauna);

// One formation's rock, in the world, white for its material to colour.
MeshData Rock(const Formation& formation, float strength, const SiteTerrain& terrain);

// The rhythm, 0 to 1: a soft swell every `rhythm` seconds of the campaign's clock, the same everywhere.
float Pulse(double seconds, float rhythm);

} // namespace Signs

} // namespace pred
