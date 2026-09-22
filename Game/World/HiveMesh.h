#pragma once

#include "Engine/Render/Mesh.h"

#include <cstdint>

namespace pred
{

// The hive a creature builds: a mound of something grown rather than built, heaped round a gaping
// hollow, with egg sacs half sunk in its sides and cords rising from it into the dark above. Sculpted
// the way a creature is -- blended shapes turned into one surface -- and painted per vertex.
//
// `seed` makes one nest differ from the next; `radius` is how far the mound reaches; `ceiling` is how
// high the cords climb.
MeshData BuildHiveMesh(uint32_t seed, float radius, float ceiling);

} // namespace pred
