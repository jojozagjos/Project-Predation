#pragma once

#include "Engine/Render/Mesh.h"

#include <filesystem>

namespace pred
{

// Reads a Wavefront OBJ into engine geometry.
//
// OBJ rather than glTF to begin with, because it needs no third-party dependency and every model
// site, Sketchfab included, offers it. Positions, normals and faces are read; materials, textures
// and anything animated are not, which suits the placeholder look the game has now. Faces with more
// than three corners are fanned into triangles, and missing normals are generated from the winding.
//
// Returns false and leaves `out` untouched if the file cannot be read or contains no triangles.
bool LoadObjMesh(const std::filesystem::path& file, MeshData& out);

// Scales and centres geometry so it fits inside a box of `targetSize` metres, and reports what
// scale was applied. Downloads arrive in wildly different units, and a model that lands a hundred
// times too large is indistinguishable from one that failed to load.
float NormalizeMeshScale(MeshData& mesh, float targetSize);

} // namespace pred
