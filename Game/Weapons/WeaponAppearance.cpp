#include "Game/Weapons/WeaponAppearance.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <unordered_map>

namespace pred
{
namespace
{

// A part of a procedurally built weapon, described the same way the editor describes one, so
// exporting it produces a file that opens and edits like anything else.
ModelPart MakePart(const char* name, const glm::vec3& size, const glm::vec3& centre,
                   const glm::vec3& colour, float pitchDegrees = 0.0f)
{
    ModelPart part;
    part.name = name;
    part.shape = PartShape::Box;
    part.size = size;
    part.position = centre;
    part.rotation = {pitchDegrees, 0.0f, 0.0f};
    part.color = colour;
    part.roughness = 0.42f;
    part.metallic = 0.6f;
    return part;
}

// Builds the model a weapon has when nobody has authored one for it. Expressed as a ModelAsset
// rather than as raw geometry so the same code can both draw it and write it out for editing.
ModelAsset ProceduralModel(const WeaponDefinition& definition)
{
    ModelAsset model;
    model.name = definition.key;

    const float length = std::max(definition.size.z, 0.12f);
    const float height = std::max(definition.size.y, 0.06f);
    const float width = std::max(definition.size.x, 0.03f);
    const bool longArm = length > 0.36f; // anything this size gets a stock and a handguard
    const glm::vec3 colour = definition.color;

    model.parts.push_back(MakePart("receiver", {width, height * 0.44f, length * 0.46f},
                                   {0.0f, height * 0.34f, length * 0.10f}, colour));
    model.parts.push_back(MakePart("grip", {width * 0.78f, height * 0.82f, length * 0.13f},
                                   {0.0f, -height * 0.30f, -length * 0.02f}, colour * 0.85f, -14.0f));

    if (longArm)
    {
        model.parts.push_back(MakePart("handguard", {width * 0.86f, height * 0.34f, length * 0.30f},
                                       {0.0f, height * 0.30f, length * 0.42f}, colour * 0.92f));
        model.parts.push_back(MakePart("barrel", {width * 0.34f, height * 0.22f, length * 0.24f},
                                       {0.0f, height * 0.32f, length * 0.68f}, colour * 0.8f));
        model.parts.push_back(MakePart("stock", {width * 0.80f, height * 0.50f, length * 0.30f},
                                       {0.0f, height * 0.30f, -length * 0.28f}, colour * 0.9f));
    }
    else
    {
        model.parts.push_back(MakePart("slide", {width * 0.92f, height * 0.30f, length * 0.44f},
                                       {0.0f, height * 0.42f, length * 0.28f}, colour));
    }

    // The sight block's top face is the sight line, which is what aiming puts on the view axis, so
    // where it sits decides how the weapon is held when the sights come up.
    const float sightBase = height * (longArm ? 0.56f : 0.60f);
    const float sightBlock = height * 0.20f;
    model.parts.push_back(MakePart("rear_sight", {width * 0.32f, sightBlock, length * 0.09f},
                                   {0.0f, sightBase + sightBlock * 0.5f, length * (longArm ? 0.16f : 0.30f)},
                                   colour * 0.7f));
    model.parts.push_back(MakePart("front_sight", {width * 0.16f, sightBlock * 0.9f, length * 0.04f},
                                   {0.0f, sightBase + sightBlock * 0.45f, length * (longArm ? 0.62f : 0.44f)},
                                   colour * 0.7f));

    // The magazine is built about its own top so a reload can slide it straight down.
    const float magazineDepth = height * (longArm ? 1.05f : 0.86f);
    const glm::vec3 seated{0.0f, -height * 0.10f, length * (longArm ? 0.14f : 0.0f)};
    ModelPart magazine = MakePart("magazine", {width * 0.70f, magazineDepth, length * 0.13f},
                                  seated + glm::vec3(0.0f, -magazineDepth * 0.5f, 0.0f), colour * 0.75f, -6.0f);
    model.parts.push_back(std::move(magazine));

    ModelSocket grip;
    grip.name = "grip";
    model.sockets.push_back(grip);

    ModelSocket support;
    support.name = "support";
    support.position = {0.0f, height * 0.16f, longArm ? length * 0.42f : length * 0.16f};
    model.sockets.push_back(support);

    ModelSocket muzzle;
    muzzle.name = "muzzle";
    muzzle.position = {0.0f, height * 0.32f, length * (longArm ? 0.80f : 0.50f)};
    model.sockets.push_back(muzzle);

    ModelSocket magazineWell;
    magazineWell.name = "magazine";
    magazineWell.position = seated;
    model.sockets.push_back(magazineWell);

    ModelSocket sight;
    sight.name = "sight";
    sight.position = {0.0f, sightBase + sightBlock, length * (longArm ? 0.16f : 0.30f)};
    model.sockets.push_back(sight);

    return model;
}

WeaponVisual FromModel(const ModelAsset& model, const WeaponDefinition& definition,
                       std::shared_ptr<const ModelAsset> shared)
{
    WeaponVisual visual;
    visual.asset = std::move(shared);

    for (const ModelPart& part : model.parts)
    {
        WeaponVisual::Part built;
        built.name = part.name;
        built.mesh = model.BuildPartMesh(part);
        built.material.baseColor = part.color;
        built.material.roughness = part.roughness;
        built.material.metallic = part.metallic;
        built.material.emissive = part.color * part.emissive;
        built.rest = part.LocalMatrix();
        if (part.name == "magazine")
        {
            visual.magazinePart = static_cast<int>(visual.parts.size());
        }
        visual.parts.push_back(std::move(built));
    }

    // Sockets are the contract between a model and the hands that hold it. Anything missing falls
    // back to something derived from the weapon's footprint, so a half-finished model still works.
    const float length = std::max(definition.size.z, 0.12f);
    const float height = std::max(definition.size.y, 0.06f);

    if (const ModelSocket* socket = model.FindSocket("support"))
    {
        visual.supportGrip = socket->position;
    }
    else
    {
        visual.supportGrip = {0.0f, height * 0.16f, length * 0.42f};
    }
    if (const ModelSocket* socket = model.FindSocket("muzzle"))
    {
        visual.muzzle = socket->position;
    }
    else
    {
        visual.muzzle = {0.0f, height * 0.32f, length * 0.8f};
    }
    if (const ModelSocket* socket = model.FindSocket("magazine"))
    {
        visual.magazineSeated = socket->position;
    }
    if (const ModelSocket* socket = model.FindSocket("sight"))
    {
        visual.sightHeight = socket->position.y;
    }
    else
    {
        visual.sightHeight = height * 0.76f;
    }
    return visual;
}

// Loaded models are shared: several weapons can name the same one, and reloading a weapon should
// not re-read the file every time it is drawn.
std::unordered_map<std::string, std::shared_ptr<const ModelAsset>>& ModelCache()
{
    static std::unordered_map<std::string, std::shared_ptr<const ModelAsset>> cache;
    return cache;
}

} // namespace

MeshData WeaponVisual::Combined() const
{
    MeshData combined;
    for (const Part& part : parts)
    {
        combined.Append(part.mesh, part.rest);
    }
    return combined;
}

WeaponVisual BuildWeaponVisual(const WeaponDefinition& definition)
{
    if (!definition.model.empty())
    {
        auto& cache = ModelCache();
        auto found = cache.find(definition.model);
        if (found == cache.end())
        {
            auto loaded = std::make_shared<ModelAsset>();
            if (loaded->LoadFromFile(ModelDirectory() / (definition.model + ".json")))
            {
                found = cache.emplace(definition.model, std::move(loaded)).first;
            }
            else
            {
                PRED_LOG_WARN(Gameplay, "Weapon '{}' names model '{}', which did not load; using the "
                                        "built-in shape instead",
                              definition.key, definition.model);
            }
        }
        if (found != cache.end() && found->second != nullptr)
        {
            return FromModel(*found->second, definition, found->second);
        }
    }

    const ModelAsset model = ProceduralModel(definition);
    return FromModel(model, definition, nullptr);
}

bool ExportWeaponModel(const WeaponDefinition& definition, const std::string& modelName)
{
    ModelAsset model = ProceduralModel(definition);
    model.name = modelName;
    return model.SaveToFile(ModelDirectory() / (modelName + ".json"));
}

} // namespace pred
