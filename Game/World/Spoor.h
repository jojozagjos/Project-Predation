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

// The marks, in two meshes in the world. `stains` is laid on what it is over (Material::decal): each vertex's colour multiplied into
// the ground or the wall under it, white at every edge -- prints pressed in, blood soaked in, a drag smeared, claws scored -- so the
// ground's own grain shows through and nothing has a hard edge (FIX 714: painted on as flat colours they looked stuck on). `solid` is
// what lies there as a thing (a shed skin), painted for a plain white material. `groundTint` is the ground's own colour at a point
// (SiteMap::GroundTint): what it is (grass, snow) says how a print shows in it.
struct Marks
{
    MeshData stains;
    MeshData solid;
};
// `covered` says whether anything stands on the ground at a point (a covered way, a block, a mast's foot, a building): no mark is
// laid there, nor across it (FIX 747).
Marks Make(uint32_t seed, const SitePlan& plan, const SiteTerrain& terrain, const SiteFauna& fauna,
           const std::function<glm::vec3(float, float)>& groundTint, const std::function<bool(float, float)>& covered = {});

} // namespace Spoor

} // namespace pred
