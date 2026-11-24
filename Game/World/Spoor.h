#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>

namespace pred
{

struct SitePlan;
struct SiteFauna;
class SiteTerrain;

// The marks what lives at a site leaves (Phase 7, the signs reworked): its tracks pressed into the ground along the ways it goes --
// from its lair down to the water, out to the buildings -- in mud or snow alike; a drag mark from where it took something to where it
// ate it; blood where it fed, and drops of it back to its lair (no bodies); claw gouges in the wall beside the doors of a building it lives in; a shed skin by its
// lair. How fresh they are says how long ago (dark and crisp and many where it is about now, faded and few where it only came through
// a while back), and how big they are, how big it is: a giant strider's tracks pits metres across. Nothing where nothing lives. Worked
// out from the site's seed, the same on every machine.
namespace Spoor
{

// The marks as one mesh in the world, painted (each vertex its colour) for a plain white material; `groundTint` is the ground's own
// colour at a point (SiteMap::GroundTint), the marks drawn darker or paler than it.
MeshData Make(uint32_t seed, const SitePlan& plan, const SiteTerrain& terrain, const SiteFauna& fauna,
              const std::function<glm::vec3(float, float)>& groundTint);

} // namespace Spoor

} // namespace pred
