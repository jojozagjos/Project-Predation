#pragma once

#include "Engine/Render/Mesh.h"

#include <cstdint>
#include <vector>

namespace pred
{

// Fewer triangles for the same shape: edges collapsed where the surface is flattest and the colours and
// normals change least, until there are about `targetTriangles` left or collapsing more would move the
// surface by more than `maxError` of the mesh's size. Nothing new is made -- every vertex that is left
// was one of the originals -- so `kept` gives, for each vertex of the result, which original it was,
// for carrying anything else stored per vertex (a creature's bone weights) across.
MeshData SimplifyMesh(const MeshData& mesh, size_t targetTriangles, float maxError, std::vector<uint32_t>& kept);

} // namespace pred
