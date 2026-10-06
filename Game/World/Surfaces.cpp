#include "Game/World/Surfaces.h"

#include "Engine/Core/Paths.h"
#include "Engine/Render/TextureLibrary.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <filesystem>

namespace pred
{

namespace Surfaces
{

namespace
{

TextureLibrary* g_textures = nullptr;

// The map of a set, whichever of the usual image types it was saved as; white (nothing) when it is not there.
TextureHandle Map(const std::string& set, const char* kind)
{
    const std::filesystem::path folder = Paths::AssetsRoot() / "Textures" / set;
    for (const char* extension : {".png", ".jpg", ".jpeg"})
    {
        const std::filesystem::path file = folder / (set + "_" + kind + extension);
        if (std::filesystem::exists(file))
        {
            return g_textures->LoadSurfaceMap(file.string(), "surface/" + set + "/" + kind);
        }
    }
    return TextureHandle{};
}

} // namespace

void SetLibrary(TextureLibrary* textures)
{
    g_textures = textures;
}

Material Apply(Material material, const std::string& name, float metres, float keep, bool natural)
{
    if (g_textures == nullptr || metres <= 0.0f)
    {
        return material;
    }
    const TextureHandle colour = Map(name, "BaseColor");
    if (!colour.IsValid())
    {
        return material;
    }
    material.baseColorTexture = colour;
    // Tinted to the material's colour on average, the set giving only its detail: a photograph of sand or grass has a
    // colour of its own, and multiplied by a world's colour as well it would come out twice as deep.
    // (Its average as drawn: with only so much of its colour kept, so much nearer grey.)
    const glm::vec3 mean = g_textures->MeanColour(colour);
    const glm::vec3 drawn = glm::mix(glm::vec3(glm::dot(mean, glm::vec3(0.2126f, 0.7152f, 0.0722f))), mean, std::clamp(keep, 0.0f, 1.0f));
    material.baseColor /= glm::max(drawn, glm::vec3(0.02f));
    material.surfaceKeep = keep;
    material.surfaceNatural = natural;
    material.normalTexture = Map(name, "Normal");
    material.roughnessTexture = Map(name, "Roughness");
    material.surfaceScale = metres;
    return material;
}

Material ApplySteep(Material material, const std::string& name, float metres)
{
    if (g_textures == nullptr || metres <= 0.0f || material.surfaceScale <= 0.0f)
    {
        return material;
    }
    const TextureHandle colour = Map(name, "BaseColor");
    if (!colour.IsValid())
    {
        return material;
    }
    material.steepColorTexture = colour;
    material.steepNormalTexture = Map(name, "Normal");
    material.steepRoughnessTexture = Map(name, "Roughness");
    material.steepScale = metres;
    // The material's colour is already over the first set's average; over this one's instead.
    // (The first set's average as drawn, against this one's as drawn -- most of its own colour let go of, in the shader.)
    const glm::vec3 first = g_textures->MeanColour(material.baseColorTexture);
    const glm::vec3 firstDrawn = glm::mix(glm::vec3(glm::dot(first, glm::vec3(0.2126f, 0.7152f, 0.0722f))), first, material.surfaceKeep);
    const glm::vec3 own = g_textures->MeanColour(colour);
    const glm::vec3 ownDrawn = glm::mix(glm::vec3(glm::dot(own, glm::vec3(0.2126f, 0.7152f, 0.0722f))), own, 0.35f);
    material.steepTint = firstDrawn / glm::max(ownDrawn, glm::vec3(0.02f));
    return material;
}

} // namespace Surfaces

} // namespace pred
