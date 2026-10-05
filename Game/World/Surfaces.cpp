#include "Game/World/Surfaces.h"

#include "Engine/Core/Paths.h"
#include "Engine/Render/TextureLibrary.h"

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

Material Apply(Material material, const std::string& name, float metres)
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
    material.normalTexture = Map(name, "Normal");
    material.roughnessTexture = Map(name, "Roughness");
    material.surfaceScale = metres;
    return material;
}

} // namespace Surfaces

} // namespace pred
