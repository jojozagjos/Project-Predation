#include "Game/World/Surfaces.h"

#include "Game/World/Foliage.h"

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

// Which set each base colour texture laid on is, for what a step on it sounds like.
std::vector<std::string> g_setOf;

// Where a set's map is, whichever of the usual image types it was saved as (empty: it has none).
std::string File(const std::string& set, const char* kind)
{
    const std::filesystem::path folder = Paths::AssetsRoot() / "Textures" / set;
    for (const char* extension : {".png", ".jpg", ".jpeg"})
    {
        const std::filesystem::path file = folder / (set + "_" + kind + extension);
        if (std::filesystem::exists(file))
        {
            return file.string();
        }
    }
    return std::string();
}

// The map of a set; nothing when it is not there.
TextureHandle Map(const std::string& set, const char* kind)
{
    const std::string file = File(set, kind);
    if (file.empty())
    {
        return TextureHandle{};
    }
    // The colour with its roughness in its alpha (TextureLibrary::LoadSurfaceColour).
    if (std::string(kind) == "BaseColor")
    {
        const TextureHandle handle = g_textures->LoadSurfaceColour(file, File(set, "Roughness"), "surface/" + set + "/" + kind);
        if (handle.IsValid())
        {
            g_setOf.resize(std::max<size_t>(g_setOf.size(), static_cast<size_t>(handle.index) + 1));
            g_setOf[handle.index] = set;
        }
        return handle;
    }
    return g_textures->LoadSurfaceMap(file, "surface/" + set + "/" + kind);
}

} // namespace

void SetLibrary(TextureLibrary* textures)
{
    g_textures = textures;
}

TextureHandle FoliageAtlas()
{
    if (g_textures == nullptr)
    {
        return TextureHandle{};
    }
    return g_textures->UploadCutout(Foliage::Atlas(), "foliage_atlas");
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
    material.surfaceScale = metres;
    return material;
}

Material ApplyWorn(Material material, const std::string& name, float metres, float wear)
{
    if (g_textures == nullptr || metres <= 0.0f || wear <= 0.0f)
    {
        return material;
    }
    const TextureHandle colour = Map(name, "BaseColor");
    if (!colour.IsValid())
    {
        return material;
    }
    // The shader takes the paint's light and dark from the photograph as its brightest channel there (the paint's own
    // colour let go of); over that channel's average here, so the paint comes out the material's colour on average,
    // however light or dark the photograph's paint is. (The bare metal is its own colour, whatever this.)
    const glm::vec3 mean = g_textures->MeanColour(colour);
    material.baseColor /= std::max(std::max(mean.r, std::max(mean.g, mean.b)), 0.05f);
    material.baseColorTexture = colour;
    material.normalTexture = Map(name, "Normal");
    material.surfaceScale = metres;
    material.surfaceKeep = 0.0f;
    material.surfaceWear = wear;
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

Material ApplyBlend(Material material, const std::string& name, float metres, const glm::vec3& colour, float share, float shore)
{
    if (g_textures == nullptr || metres <= 0.0f || material.surfaceScale <= 0.0f || (share <= 0.0f && shore <= 0.0f))
    {
        return material;
    }
    const TextureHandle texture = Map(name, "BaseColor");
    if (!texture.IsValid())
    {
        return material;
    }
    material.blendColorTexture = texture;
    material.blendNormalTexture = Map(name, "Normal");
    material.blendScale = metres;
    material.blendShare = share;
    material.blendShore = shore;
    // Its own colour, over this set's average as drawn (as much of its own colour kept as the first's): whatever the material's
    // colour and what is painted on the ground under it, which the shader takes back out of it.
    const glm::vec3 mean = g_textures->MeanColour(texture);
    const glm::vec3 drawn = glm::mix(glm::vec3(glm::dot(mean, glm::vec3(0.2126f, 0.7152f, 0.0722f))), mean, material.surfaceKeep);
    material.blendTint = colour / glm::max(drawn, glm::vec3(1.0e-3f));
    return material;
}

namespace
{

// The footstep surfaces, as numbered for a body's tag (from one: nought is none).
constexpr const char* kSteps[] = {"concrete", "stone", "metal", "gravel", "wood", "snow", "sand", "grass", "dirt", "ice"};

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
        {"basalt", "gravel"},         {"wet_rock", "gravel"},       {"grass_dirt", "grass"},    {"jungle_floor", "grass"},
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

namespace
{

// As fs_mesh's SurfaceHash and SurfacePatches: a value at each whole point, eased between.
float PatchHash(float x, float z)
{
    const auto fract = [](float v) { return v - std::floor(v); };
    float px = fract(x * 0.1031f);
    float pz = fract(z * 0.1030f);
    const float d = px * (pz + 33.33f) + pz * (px + 33.33f);
    px += d;
    pz += d;
    return fract((px + pz) * px);
}

float Patches(float x, float z)
{
    const float ix = std::floor(x);
    const float iz = std::floor(z);
    float fx = x - ix;
    float fz = z - iz;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);
    const float a = PatchHash(ix, iz);
    const float b = PatchHash(ix + 1.0f, iz);
    const float c = PatchHash(ix, iz + 1.0f);
    const float d = PatchHash(ix + 1.0f, iz + 1.0f);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
}

} // namespace

bool OutcropAt(float x, float z, float normalUp)
{
    // As the shader has it, without the ground picture's own light and dark at its edges: over half of the way into it.
    const float spread = Patches(x * 0.035f + 7.3f, z * 0.035f + 1.9f) * 0.65f + Patches(x * 0.11f, z * 0.11f) * 0.35f;
    const float rise = std::clamp((0.996f - normalUp) / (0.996f - 0.975f), 0.0f, 1.0f);
    return spread > 0.71f && rise > 0.5f;
}

namespace
{

float BlendLay(float x, float z, float share)
{
    // Patches some tens of metres across, ragged with smaller ones down to a footprint, and as much of them over the line as
    // is asked for.
    const float patch = Patches(x * 0.017f + 41.3f, z * 0.017f + 13.7f) * 0.5f + Patches(x * 0.053f + 5.1f, z * 0.053f + 23.9f) * 0.25f +
                        Patches(x * 0.19f + 2.3f, z * 0.19f + 8.8f) * 0.13f + Patches(x * 0.7f + 7.7f, z * 0.7f + 1.1f) * 0.08f +
                        Patches(x * 2.3f + 4.4f, z * 2.3f + 9.2f) * 0.04f;
    return patch - (0.5f + (0.5f - std::clamp(share, 0.0f, 1.0f)) * 0.45f);
}

float ShoreLay(float x, float z, float y, float waterLevel, float shore)
{
    // From the water up to about `shore` metres above it, its top edge wandering.
    return (waterLevel + shore * (0.6f + 0.8f * Patches(x * 0.13f + 3.3f, z * 0.13f + 6.1f)) - y) * 0.25f;
}

} // namespace

bool BlendAt(float x, float z, float y, float share, float waterLevel, float shore)
{
    const float lay = share > 0.0f ? BlendLay(x, z, share) : -1.0f;
    return std::max(lay, shore > 0.0f ? ShoreLay(x, z, y, waterLevel, shore) : -1.0f) > 0.0f;
}

const char* StepName(uint32_t tag)
{
    return tag >= 1 && tag <= std::size(kSteps) ? kSteps[tag - 1] : nullptr;
}

} // namespace Surfaces

} // namespace pred
