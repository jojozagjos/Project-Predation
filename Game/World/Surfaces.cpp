#include "Game/World/Surfaces.h"

#include "Engine/Core/Paths.h"
#include "Engine/Render/TextureLibrary.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <filesystem>
#include <vector>

namespace pred
{

namespace Surfaces
{

namespace
{

TextureLibrary* g_textures = nullptr;

// The map of a set, whichever of the usual image types it was saved as; white (nothing) when it is not there.
// Which set each base colour texture laid on is, for what a step on it sounds like.
std::vector<std::string> g_setOf;

TextureHandle Map(const std::string& set, const char* kind)
{
    const std::filesystem::path folder = Paths::AssetsRoot() / "Textures" / set;
    for (const char* extension : {".png", ".jpg", ".jpeg"})
    {
        const std::filesystem::path file = folder / (set + "_" + kind + extension);
        if (std::filesystem::exists(file))
        {
            const TextureHandle handle = g_textures->LoadSurfaceMap(file.string(), "surface/" + set + "/" + kind);
            if (handle.IsValid() && std::string(kind) == "BaseColor")
            {
                g_setOf.resize(std::max<size_t>(g_setOf.size(), static_cast<size_t>(handle.index) + 1));
                g_setOf[handle.index] = set;
            }
            return handle;
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

namespace
{

// The footstep surfaces, as numbered for a body's tag (from one: nought is none).
constexpr const char* kSteps[] = {"concrete", "stone", "metal", "gravel", "wood", "snow", "sand", "grass", "dirt", "mud", "ice"};

} // namespace

uint32_t StepOfSet(const std::string& name, bool metal)
{
    struct Sounds
    {
        const char* set;
        const char* step;
    };
    static constexpr Sounds kSounds[] = {
        {"snow_02", "snow"},          {"regolith", "gravel"},       {"rock_ground", "gravel"},  {"sand", "sand"},
        {"basalt", "gravel"},         {"wet_rock", "mud"},          {"grass_dirt", "grass"},    {"jungle_floor", "grass"},
        {"toxic_crust", "dirt"},      {"storm_flats", "gravel"},    {"crystal_ground", "ice"},  {"rock_cliff", "stone"},
        {"soil_cliff", "dirt"},       {"ice_cliff", "ice"},         {"concrete_slab", "concrete"}, {"pavement", "concrete"},
        {"asphalt", "concrete"},      {"blast_wall", "concrete"},   {"facility_floor", "concrete"}, {"facility_wall", "concrete"},
        {"concrete_pillar", "concrete"}, {"plant_grime", "concrete"}, {"deck_plate", "metal"},   {"facility_stairs", "metal"},
        {"metal_grate", "metal"},     {"worktop", "wood"},          {"tabletop", "wood"},       {"fabric_cushion", "wood"},
    };
    std::string step = metal ? "metal" : "concrete";
    for (const Sounds& sounds : kSounds)
    {
        if (name == sounds.set)
        {
            step = sounds.step;
            break;
        }
    }
    // Any other set of plate, panel or paint: metal.
    if (!name.empty() && step == "concrete" && (name.find("metal") != std::string::npos || name.find("hull") != std::string::npos ||
                                                name.find("panel") != std::string::npos || name.find("plat") != std::string::npos ||
                                                name.find("cladding") != std::string::npos || name.find("crate") != std::string::npos ||
                                                name.find("shell") != std::string::npos || name.find("roof") != std::string::npos))
    {
        step = "metal";
    }
    for (size_t i = 0; i < std::size(kSteps); ++i)
    {
        if (step == kSteps[i])
        {
            return static_cast<uint32_t>(i) + 1u;
        }
    }
    return 1u;
}

uint32_t StepOf(const Material& material)
{
    const bool metal = material.metallic > 0.3f;
    if (material.surfaceScale > 0.0f && material.baseColorTexture.IsValid() && material.baseColorTexture.index < g_setOf.size())
    {
        return StepOfSet(g_setOf[material.baseColorTexture.index], metal);
    }
    return StepOfSet(std::string(), metal);
}

const char* StepName(uint32_t tag)
{
    return tag >= 1 && tag <= std::size(kSteps) ? kSteps[tag - 1] : nullptr;
}

} // namespace Surfaces

} // namespace pred
