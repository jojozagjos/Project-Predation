#include "Game/Items/ItemAppearance.h"

#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/Weapons/WeaponDatabase.h"

#include <glm/gtc/matrix_transform.hpp>

namespace pred
{

MeshData ItemMesh(const ItemDefinition& definition, const WeaponDatabase* weapons)
{
    if (weapons != nullptr)
    {
        if (const WeaponDefinition* weapon = weapons->Get(weapons->ForItem(definition.key)))
        {
            // The real model, magazine included, so a dropped rifle and its inventory icon are the
            // same object the player was just holding.
            return BuildWeaponVisual(*weapon).Combined();
        }
    }

    switch (definition.shape)
    {
    case ItemShape::Cylinder:
        return Primitives::Cylinder(definition.size.x * 0.5f, definition.size.y, 16);
    case ItemShape::Sphere:
        return Primitives::Sphere(definition.size.x * 0.5f, 16, 12);
    case ItemShape::Box:
    default:
        return Primitives::Box(definition.size);
    }
}

Material ItemMaterial(const ItemDefinition& definition)
{
    Material material;
    material.baseColor = definition.color;
    material.roughness = definition.roughness;
    material.metallic = definition.metallic;
    material.emissive = definition.color * definition.emissive;
    return material;
}

} // namespace pred
