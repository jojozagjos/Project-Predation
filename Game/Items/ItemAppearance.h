#pragma once

#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Items/ItemDatabase.h"

namespace pred
{

class WeaponDatabase;

// What an item looks like, derived from its data entry.
//
// The world (loose items lying about), the inventory icon atlas and the dropped-item physics all
// build their geometry from here, so an item never looks like two different things depending on
// where you are seeing it. When authored meshes replace the primitive placeholders, this is the
// only place that changes.
//
// `weapons` is optional, and supplying it is what makes a rifle on the floor and a rifle on an
// inventory icon look like the rifle in your hands rather than like a block.
MeshData ItemMesh(const ItemDefinition& definition, const WeaponDatabase* weapons = nullptr);
Material ItemMaterial(const ItemDefinition& definition);

} // namespace pred
