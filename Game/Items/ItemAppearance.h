#pragma once

#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Items/ItemDatabase.h"

namespace pred
{

// What an item looks like, derived from its data entry.
//
// Both the world (loose items lying about) and the inventory icon atlas build their geometry from
// here, so an item never looks like two different things depending on where you are seeing it. When
// authored meshes replace the primitive placeholders, this is the only place that changes.
MeshData ItemMesh(const ItemDefinition& definition);
Material ItemMaterial(const ItemDefinition& definition);

} // namespace pred
