#pragma once

#include "Engine/Render/Mesh.h"
#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"

namespace pred
{

// A building's outside, dressed as its plan says (FacilityLayout::Outside): windows along its floors, bands at every floor and
// ribs between its bays, a stripe of its accent colour round its top, canopies and frames at its ways out, and what stands
// on its roof -- plant, vents, stacks, panels, a mast, dishes, a dome. Each piece a little proud of the cladding or standing
// on the roof, never into the building; drawn only (FacilityMap::Dressing). Only a plan, like the building's: nothing that
// needs the engine, every machine dressing it the same from the same plan.
namespace FacilityOutside
{

void Dress(const FacilityLayout& layout, FacilityMap::Blueprint& out);

// The round forms' meshes (FacilityMap::Dressing::Form): half a sphere on its flat side, `radius` across it and `height` up,
// its rim's middle at the origin; and a shallow bowl `radius` across and `depth` deep, opening up, its rim's middle at the
// origin, seen from both sides.
MeshData Dome(float radius, float height, int segments = 32);
MeshData Dish(float radius, float depth, int segments = 24);

} // namespace FacilityOutside

} // namespace pred
