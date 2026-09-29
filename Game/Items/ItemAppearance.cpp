#include "Game/Items/ItemAppearance.h"

#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/Weapons/WeaponDatabase.h"

#include <map>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace pred
{

namespace
{

// A device: a case lying flat, its screen on its top face mapped to the whole of its texture, every other face mapped to
// the texture's corner, which is painted the case's colour; and a grip under the tracker, held like a pistol.
MeshData DeviceMesh(const ItemDefinition& definition)
{
    MeshData mesh;
    const glm::vec3 size = definition.size;
    const auto caseUv = [](MeshData piece)
    {
        for (MeshVertex& vertex : piece.vertices)
        {
            vertex.uv = {0.004f, 0.004f};
        }
        return piece;
    };
    mesh.Append(caseUv(Primitives::Box(size)), glm::mat4(1.0f));
    // The screen: a little proud of the top, inset from its edges.
    const float w = size.x * 0.43f;
    const float d = size.z * 0.42f;
    const float y = size.y * 0.5f + 0.002f;
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    mesh.vertices.push_back(MeshVertex{{-w, y, -d}, up, {0.02f, 0.02f}});
    mesh.vertices.push_back(MeshVertex{{w, y, -d}, up, {0.98f, 0.02f}});
    mesh.vertices.push_back(MeshVertex{{w, y, d}, up, {0.98f, 0.98f}});
    mesh.vertices.push_back(MeshVertex{{-w, y, d}, up, {0.02f, 0.98f}});
    mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    if (definition.device == "tracker")
    {
        const glm::vec3 grip{size.x * 0.28f, size.x * 0.75f, size.z * 0.34f};
        mesh.Append(caseUv(Primitives::Box(grip)),
                    glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -size.y * 0.5f - grip.y * 0.45f, size.z * 0.12f)) *
                        glm::mat4_cast(glm::angleAxis(glm::radians(-18.0f), glm::vec3(1.0f, 0.0f, 0.0f))));
    }
    return mesh;
}

} // namespace

MeshData ItemMesh(const ItemDefinition& definition, const WeaponDatabase* weapons)
{
    if (!definition.device.empty())
    {
        return DeviceMesh(definition);
    }
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

std::vector<ItemPart> ItemParts(const ItemDefinition& definition, const WeaponDatabase* weapons,
                                TextureLibrary* textures)
{
    std::vector<ItemPart> parts;
    if (weapons != nullptr)
    {
        if (const WeaponDefinition* weapon = weapons->Get(weapons->ForItem(definition.key)))
        {
            WeaponVisual visual = BuildWeaponVisual(*weapon, textures);
            parts.reserve(visual.parts.size());
            for (WeaponVisual::Part& part : visual.parts)
            {
                parts.push_back({std::move(part.mesh), part.material, part.rest});
            }
            if (!parts.empty())
            {
                return parts;
            }
        }
    }
    parts.push_back({ItemMesh(definition, nullptr), ItemMaterial(definition), glm::mat4(1.0f)});
    return parts;
}

namespace
{

std::map<std::string, TextureHandle>& DeviceFaces()
{
    static std::map<std::string, TextureHandle> faces;
    return faces;
}

} // namespace

void SetDeviceFace(const std::string& device, TextureHandle face)
{
    DeviceFaces()[device] = face;
}

Material ItemMaterial(const ItemDefinition& definition)
{
    Material material;
    material.baseColor = definition.color;
    material.roughness = definition.roughness;
    material.metallic = definition.metallic;
    material.emissive = definition.color * definition.emissive;
    // A device: its case and its screen both from its face, the screen glowing by its picture.
    if (!definition.device.empty())
    {
        const auto found = DeviceFaces().find(definition.device);
        if (found != DeviceFaces().end() && found->second.IsValid())
        {
            material.baseColor = glm::vec3(1.0f);
            material.baseColorTexture = found->second;
            material.emissive = glm::vec3(1.1f);
            material.emissiveTextured = true;
        }
    }
    return material;
}

} // namespace pred
