#pragma once

#include "Engine/Render/Mesh.h"
#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"

#include <vector>

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

// One of a building's windows (FacilityLayout::Outside): the wall it is in -- which way it faces and the line of cells' edges it
// stands on (as FacilityLayout::OuterWall's) -- the bay along it and the floor; where its frame is, along the wall (world)
// and up it; and whether there is somewhere behind it to look out of, a room, a corridor or a stair, so it is an opening
// through the wall (FacilityMap cuts it) rather than dark glass on the wall of nothing.
struct Window
{
    int side = 0;
    int line = 0;
    int bay = 0;
    int floor = 0;
    float a = 0.0f;
    float b = 0.0f;
    float low = 0.0f;
    float high = 0.0f;
    bool open = false; // an opening through the wall, a room behind it to look out of; or only glass on the outside of the wall
    bool tall = false; // a bar across it, two thirds up
};
std::vector<Window> Windows(const FacilityLayout& layout);
// How wide a window's frame is all round: the opening through the wall is inside it.
inline constexpr float kWindowFrame = 0.08f;

// The round forms' meshes (FacilityMap::Dressing::Form): half a sphere on its flat side, `radius` across it and `height` up,
// its rim's middle at the origin; and a shallow bowl `radius` across and `depth` deep, opening up, its rim's middle at the
// origin, seen from both sides.
MeshData Dome(float radius, float height, int segments = 32);
MeshData Dish(float radius, float depth, int segments = 24);
// A pipe's bend: a quarter of a ring, `radius` round its middle, the pipe `thick` across, with the corner it turns at at the origin:
// in along +x from -x, round, and out along +z -- or along -z, `turn` below nought, the same mirrored.
MeshData PipeBend(float radius, float thick, float turn = 1.0f);
// A window's glass: a flat pane `wide` across and `high` up, its middle at the origin, drawn only from in front -- its -z, out
// of the building -- so from outside it is the glass, dark and giving back the sky, and from in the room it is not there at
// all and the view out is clear (nothing draws glass that can be seen through).
MeshData Pane(float wide, float high);

} // namespace FacilityOutside

} // namespace pred
