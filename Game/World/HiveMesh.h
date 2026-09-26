#pragma once

#include "Engine/Render/Mesh.h"

#include <cstdint>

namespace pred
{

// A nest, as creatures grow them: not a heap on the floor but a living thing rooted in a wall. A heart
// the size of a person's chest hangs from the wall, its vessels spread out across it, and growth
// spreads from it over the floor, the walls and the ceiling round about as the nest gets older.
// Sculpted the way a creature is -- blended shapes turned into one surface -- and painted per vertex,
// with how wet each part is in the alpha of its colour.

// The heart, and the flesh that roots it into the wall, in the wall's own frame: +Z comes out of the
// wall, +Y is up, and the wall itself is z = 0. Two meshes because only one of them beats.
struct NestHeartMeshes
{
    MeshData heart;
    MeshData roots;
};
NestHeartMeshes BuildNestHeart(uint32_t seed);

// Where the middle of the heart is, in that frame: what is shot at, lit and heard.
inline constexpr float kNestHeartOut = 0.34f;
inline constexpr float kNestHeartUp = 0.05f;

// One patch of growth, in the frame of the surface it lies on: +Y out of the surface, the surface
// itself at y = 0, about a metre from its middle to its edge before it is scaled. Nothing of it goes
// more than a couple of centimetres behind the surface, so a patch on a thin wall is not seen poking
// out of the far side. `variant` makes the patches of one nest differ; every third carries egg sacs.
MeshData BuildNestGrowth(uint32_t seed, int variant);

// A root of the nest creeping across a surface from one patch of it to the next: a unit long along +z,
// a unit thick at its root tapering towards its tip, flattened against the surface under it (+y is out of
// the surface), wandering a little side to side. Stretched to fit between two patches.
MeshData BuildNestTendril(uint32_t seed, int variant);

} // namespace pred
