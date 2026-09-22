#include "Game/Weapons/WeaponAppearance.h"

#include "Engine/Core/Paths.h"

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

// What is drawn when a weapon's model is missing or will not load.
//
// This used to be a rather good little gun built out of boxes, and that was the problem. A stand-in
// that looks like a weapon is a stand-in nobody notices: the model silently did not load, the game
// carried on, and the only way to find out was to wonder for a while why an edit to the file was
// not showing up. A missing asset has to be louder than the thing it replaces, so this is a
// magenta marker that is not shaped like anything and glows in the dark.
//
// The sockets are still here and still in sensible places. That is deliberate -- the hands should
// hold the marker properly rather than flail at it, because an arm bug on top of a missing model
// is two problems to read at once.
ModelAsset MissingModel(const WeaponDefinition& definition)
{
    ModelAsset model;
    model.name = "missing";

    const float length = std::max(definition.size.z, 0.12f);
    const float height = std::max(definition.size.y, 0.06f);
    const float width = std::max(definition.size.x, 0.03f);
    constexpr glm::vec3 kMagenta{1.0f, 0.0f, 0.8f};

    // An upright bar and a block under it: an exclamation mark, near enough, and nothing anybody
    // would mistake for a firearm at any angle.
    ModelPart bar = MakePart("missing_bar", {width * 0.5f, height * 1.6f, length * 0.10f},
                             {0.0f, height * 0.9f, 0.0f}, kMagenta);
    bar.emissive = 0.6f;
    bar.metallic = 0.0f;
    bar.roughness = 1.0f;
    model.parts.push_back(std::move(bar));

    ModelPart dot = MakePart("missing_dot", {width * 0.5f, height * 0.35f, length * 0.10f},
                             {0.0f, -height * 0.25f, 0.0f}, kMagenta);
    dot.emissive = 0.6f;
    dot.metallic = 0.0f;
    dot.roughness = 1.0f;
    model.parts.push_back(std::move(dot));

    ModelSocket grip;
    grip.name = "grip";
    model.sockets.push_back(grip);

    ModelSocket support;
    support.name = "support";
    support.position = {0.0f, height * 0.16f, length * 0.30f};
    model.sockets.push_back(support);

    ModelSocket muzzle;
    muzzle.name = "muzzle";
    muzzle.position = {0.0f, height * 0.32f, length * 0.5f};
    model.sockets.push_back(muzzle);

    ModelSocket sight;
    sight.name = "sight";
    sight.position = {0.0f, height * 0.6f, 0.0f};
    model.sockets.push_back(sight);

    return model;
}

ModelAsset StarterModelImpl(const WeaponDefinition& definition)
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
                       std::shared_ptr<const ModelAsset> shared, TextureLibrary* textures)
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
        // The image the part names, read once and shared by name. A part with none keeps the
        // library's white pixel, so an untextured model draws in its material colours as before.
        if (textures != nullptr && !part.texture.empty())
        {
            if (const auto resolved = Paths::Resolve(part.texture))
            {
                built.material.baseColorTexture = textures->LoadFromFile(resolved->string(), part.texture);
            }
        }
        built.rest = part.LocalMatrix();
        if (part.name == "magazine")
        {
            visual.magazinePart = static_cast<int>(visual.parts.size());
        }
        visual.parts.push_back(std::move(built));
    }

    ApplyWeaponSockets(visual, model, definition);
    return visual;
}

} // namespace

void ApplyWeaponSockets(WeaponVisual& visual, const ModelAsset& model,
                        const WeaponDefinition& definition)
{
    // Sockets are the contract between a model and the hands that hold it. Anything missing falls
    // back to something derived from the weapon's footprint, so a half-finished model still works.
    //
    // Separate from building the visual because moving a socket changes none of the geometry, and
    // the editor moves sockets a great deal: rebuilding every mesh for it copied a quarter of a
    // megabyte per part per frame and took the frame rate with it.
    const float length = std::max(definition.size.z, 0.12f);
    const float height = std::max(definition.size.y, 0.06f);

    if (const ModelSocket* socket = model.FindSocket("grip"))
    {
        visual.triggerGrip = socket->position;
        visual.gripRotation = socket->Rotation();
    }
    // Where the gun itself is carried, which is a separate question from where the hand is on it.
    // Falling back to the grip is what every model written before this socket existed expects.
    if (const ModelSocket* socket = model.FindSocket("carry"))
    {
        visual.carryPoint = socket->position;
        if (socket->rotation != glm::vec3(0.0f))
        {
            visual.gripRotation = socket->Rotation();
        }
    }
    else
    {
        visual.carryPoint = visual.triggerGrip;
    }
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
        visual.sightPoint = socket->position;
        // Falls back to the grip's turn rather than to none. A model corrected by turning its grip
        // is corrected for the whole weapon; leaving the sight square would have the gun snap back
        // to its uncorrected attitude the moment the sights came up.
        visual.sightRotation =
            socket->rotation == glm::vec3(0.0f) ? visual.gripRotation : socket->Rotation();
    }
    else
    {
        visual.sightPoint = {0.0f, height * 0.76f, visual.triggerGrip.z + length * 0.20f};
        visual.sightRotation = visual.gripRotation;
    }

    // Measured rather than named, because no exporter marks the back of a stock and nobody would
    // remember to place a socket there. Taken from the drawn geometry so it means the same thing for
    // an imported model as for one built from numbers.
    {
        float back = 0.0f;
        bool any = false;
        for (const WeaponVisual::Part& part : visual.parts)
        {
            for (const MeshVertex& vertex : part.mesh.vertices)
            {
                const glm::vec3 inWeapon = glm::vec3(part.rest * glm::vec4(vertex.position, 1.0f));
                if (!any || inWeapon.z < back)
                {
                    back = inWeapon.z;
                    any = true;
                }
            }
        }
        visual.rearPoint = {0.0f, visual.triggerGrip.y, any ? back : visual.triggerGrip.z};
    }
}

namespace
{

// Loaded models are shared: several weapons can name the same one, and reloading a weapon should
// not re-read the file every time it is drawn.
std::unordered_map<std::string, std::shared_ptr<const ModelAsset>>& ModelCache()
{
    static std::unordered_map<std::string, std::shared_ptr<const ModelAsset>> cache;
    return cache;
}

} // namespace

void ForgetWeaponModels()
{
    ModelCache().clear();
}

std::shared_ptr<const ModelAsset> LoadWeaponModel(const std::string& name)
{
    if (name.empty())
    {
        return nullptr;
    }
    auto& cache = ModelCache();
    if (const auto found = cache.find(name); found != cache.end())
    {
        return found->second;
    }
    auto loaded = std::make_shared<ModelAsset>();
    const std::filesystem::path file = ModelPath(name);
    if (file.empty() || !loaded->LoadFromFile(file))
    {
        return nullptr;
    }
    // Checked once, when it is read, rather than every time it is drawn.
    for (const std::string& socket : MissingWeaponSockets(*loaded))
    {
        PRED_LOG_ERROR(Gameplay, "Model '{}' has no '{}' socket; the weapon will be held wrong", name, socket);
    }
    return cache.emplace(name, std::move(loaded)).first->second;
}

bool ApplyClipTimings(WeaponDefinition& definition, const ModelAsset& model)
{
    bool changed = false;
    if (const AnimationClip* reload = model.FindClip("reload"); reload != nullptr && reload->duration > 0.05f)
    {
        changed = changed || definition.reloadSeconds != reload->duration;
        definition.reloadSeconds = reload->duration;
    }
    if (const AnimationClip* empty = model.FindClip("reload_empty"); empty != nullptr && empty->duration > 0.05f)
    {
        changed = changed || definition.reloadEmptySeconds != empty->duration;
        definition.reloadEmptySeconds = empty->duration;
    }
    return changed;
}

MeshData WeaponVisual::Combined() const
{
    MeshData combined;
    for (const Part& part : parts)
    {
        combined.Append(part.mesh, part.rest);
    }
    return combined;
}

std::shared_ptr<const ModelAsset> AnimationCopy(const ModelAsset& model)
{
    // Everything a clip needs -- where each part rests, the sockets, the clips themselves -- and none
    // of the imported geometry, which is megabytes and is already in the meshes.
    auto copy = std::make_shared<ModelAsset>();
    copy->name = model.name;
    copy->sockets = model.sockets;
    copy->clips = model.clips;
    copy->parts.reserve(model.parts.size());
    for (const ModelPart& part : model.parts)
    {
        ModelPart light = part;
        light.mesh = MeshData{};
        copy->parts.push_back(std::move(light));
    }
    return copy;
}

WeaponVisual BuildWeaponVisualFrom(const ModelAsset& model, const WeaponDefinition& definition,
                                   TextureLibrary* textures)
{
    // With its clips. A model held straight from memory -- the one open in the editor -- used to be
    // built with none, so the preview could not play the very animations being made in front of it.
    return FromModel(model, definition, AnimationCopy(model), textures);
}

std::vector<std::string> MissingWeaponSockets(const ModelAsset& model)
{
    // The sockets a weapon cannot be held or fired without. `carry` and `magazine` are not here
    // because each has a real answer when it is absent -- carry is the grip, and a weapon with no
    // magazine part has no magazine well -- rather than a guess standing in for an oversight.
    static constexpr const char* kRequired[] = {"grip", "support", "muzzle", "sight"};
    std::vector<std::string> missing;
    for (const char* name : kRequired)
    {
        if (model.FindSocket(name) == nullptr)
        {
            missing.emplace_back(name);
        }
    }
    return missing;
}

WeaponVisual BuildWeaponVisual(const WeaponDefinition& definition, TextureLibrary* textures)
{
    // No quiet substitutions in here. A weapon whose model is missing draws the marker and says so
    // as an error, because the alternative -- a plausible shape in your hands and a line in the log
    // nobody reads -- is how an afternoon goes on an asset that was never loading in the first
    // place.
    if (definition.model.empty())
    {
        PRED_LOG_ERROR(Gameplay, "Weapon '{}' names no model. Add \"model\" to it in weapons.json.",
                       definition.key);
        const ModelAsset marker = MissingModel(definition);
        return FromModel(marker, definition, nullptr, textures);
    }

    const std::shared_ptr<const ModelAsset> model = LoadWeaponModel(definition.model);
    if (model == nullptr)
    {
        PRED_LOG_ERROR(Gameplay, "Weapon '{}' names model '{}', which did not load from {}", definition.key,
                       definition.model, ModelPath(definition.model).string());
        const ModelAsset marker = MissingModel(definition);
        return FromModel(marker, definition, nullptr, textures);
    }
    return FromModel(*model, definition, model, textures);
}

ModelAsset StarterWeaponModel(const WeaponDefinition& definition)
{
    return StarterModelImpl(definition);
}

bool ExportWeaponModel(const WeaponDefinition& definition, const std::string& modelName)
{
    ModelAsset model = StarterModelImpl(definition);
    model.name = modelName;
    return model.SaveToFile(ModelPathFor(modelName, "Weapons"));
}

} // namespace pred
