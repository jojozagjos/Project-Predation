#pragma once

#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Items/ItemDatabase.h"

namespace pred
{

class TextureLibrary;
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

// The same item as the parts it is made of, each with its own material and its own place.
//
// For anything drawn as the thing itself rather than as a stand-in shape. A weapon is several parts
// -- metal, polymer, a texture when the model has one -- and ItemMesh above merges them into one
// lump that can only wear one colour, which is right for a collision shape and wrong for anything
// somebody looks at. Anything that is not a weapon is one part, exactly as ItemMesh and
// ItemMaterial describe it.
struct ItemPart
{
    MeshData mesh;
    Material material;
    glm::mat4 transform{1.0f};
};
std::vector<ItemPart> ItemParts(const ItemDefinition& definition, const WeaponDatabase* weapons,
                                TextureLibrary* textures);

} // namespace pred
