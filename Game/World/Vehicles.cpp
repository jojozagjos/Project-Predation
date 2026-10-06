#include "Game/World/Vehicles.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Game/World/Surfaces.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace pred
{
namespace
{

// Colours: a worn pale hull and dark metal for the shuttle; for the crawler a faded working orange, grey inside, and
// black rubber tracks.
constexpr glm::vec3 kShuttleHull{0.44f, 0.45f, 0.43f};
constexpr glm::vec3 kShuttleTrim{0.2f, 0.21f, 0.22f};
constexpr glm::vec3 kCrawlerBody{0.52f, 0.22f, 0.12f};
constexpr glm::vec3 kCrawlerInside{0.3f, 0.3f, 0.31f};
constexpr glm::vec3 kCrawlerTrack{0.1f, 0.1f, 0.11f};
constexpr glm::vec3 kGlass{0.08f, 0.1f, 0.12f};

struct Builder
{
    ModelAsset& model;

    ModelPart& Box(const std::string& name, float x0, float x1, float y0, float y1, float z0, float z1, const glm::vec3& colour,
                   float roughness = 0.75f, float metallic = 0.0f, float emissive = 0.0f)
    {
        ModelPart part;
        part.name = name;
        part.shape = PartShape::Box;
        part.position = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f};
        part.size = {x1 - x0, y1 - y0, z1 - z0};
        part.color = colour;
        part.roughness = roughness;
        part.metallic = metallic;
        part.emissive = emissive;
        model.parts.push_back(part);
        return model.parts.back();
    }

    void Socket(const std::string& name, const glm::vec3& position, const glm::vec3& rotation = glm::vec3(0.0f))
    {
        ModelSocket socket;
        socket.name = name;
        socket.position = position;
        socket.rotation = rotation;
        model.sockets.push_back(socket);
    }

    // A ramp hinged along its top edge at `hinge`, running back (+z) and down to the ground, and the three clips that
    // close, open and hold it closed: the hinge turns about x, and the ramp is carried round with it.
    void Ramp(const glm::vec3& hinge, float length, float width, float thickness, const glm::vec3& colour)
    {
        ModelPart& pivot = Box("fx_ramp_hinge", hinge.x - 0.005f, hinge.x + 0.005f, hinge.y - 0.005f, hinge.y + 0.005f, hinge.z - 0.005f,
                               hinge.z + 0.005f, colour);
        (void)pivot;
        // Steep enough that the underside of its far edge rests on the ground, rather than its top surface -- which would put
        // the rest of its thickness into the ground.
        const float slope = std::asin(std::clamp((hinge.y - thickness) / length, 0.0f, 1.0f));
        const float c = std::cos(slope);
        const float s = std::sin(slope);
        // Its top surface runs from the hinge down to the ground: the middle of it is back from the top edge by half its
        // length, and under it by half its thickness.
        const glm::vec3 fromHinge{0.0f, thickness * 0.5f * c + length * 0.5f * s, thickness * 0.5f * s - length * 0.5f * c};
        ModelPart& ramp = Box("ramp", -width * 0.5f, width * 0.5f, -thickness * 0.5f, thickness * 0.5f, -length * 0.5f, length * 0.5f, colour);
        ramp.position = hinge - fromHinge;
        ramp.rotation = {glm::degrees(slope), 0.0f, 0.0f};
        ramp.parent = "fx_ramp_hinge";

        const float closed = -(90.0f + glm::degrees(slope));
        const auto clip = [&](const std::string& name, float duration, float from, float to)
        {
            AnimationClip animation;
            animation.name = name;
            animation.duration = duration;
            AnimationTrack track;
            track.part = "fx_ramp_hinge";
            AnimationKey first;
            first.time = 0.0f;
            first.rotation = {from, 0.0f, 0.0f};
            AnimationKey last;
            last.time = duration;
            last.rotation = {to, 0.0f, 0.0f};
            track.keys = {first, last};
            animation.tracks.push_back(track);
            model.clips.push_back(animation);
        };
        clip("ramp_closed", 0.1f, closed, closed);
        clip("ramp_opening", 2.5f, closed, 0.0f);
        clip("ramp_closing", 2.5f, 0.0f, closed);
    }
};

} // namespace

namespace Vehicles
{

ModelAsset ShuttleModel()
{
    // Along it: the nose at -z, the cabin's front wall at z = -5.5, its open back at 2.5 and the ramp down to the pad
    // behind that. Across it: 4 m. Up: the cabin floor 0.7 m off the pad, two and a half metres of headroom.
    ModelAsset model;
    model.name = "shuttle";
    Builder b{model};
    const float back = -5.5f;
    const float door = 2.5f;
    const float half = 2.0f;
    const float wall = 0.2f;
    const float deck = 0.7f;
    const float ceiling = 3.2f;
    int leg = 0;
    for (const float z : {back + 0.7f, door - 0.7f})
    {
        for (const float x : {-1.3f, 1.3f})
        {
            b.Box("leg_" + std::to_string(leg++), x - 0.15f, x + 0.15f, 0.0f, 0.25f, z - 0.15f, z + 0.15f, kShuttleTrim, 0.55f, 0.6f);
        }
    }
    b.Box("belly", -1.6f, 1.6f, 0.25f, 0.55f, back, door, kShuttleTrim, 0.55f, 0.6f);
    b.Box("deck", -half, half, 0.55f, deck, back, door, kShuttleHull);
    b.Box("wall_right", half - wall, half, deck, ceiling, back, door, kShuttleHull);
    b.Box("wall_left", -half, -half + wall, deck, ceiling, back, door, kShuttleHull);
    b.Box("front_wall", -half + wall, half - wall, deck, ceiling, back, back + wall, kShuttleHull);
    b.Box("roof", -half, half, ceiling, ceiling + 0.25f, back, door, kShuttleHull);
    b.Box("header", -half + wall, half - wall, ceiling - 0.3f, ceiling, door - wall, door, kShuttleHull);
    b.Box("bench_right", half - wall - 0.45f, half - wall, deck, deck + 0.45f, -3.6f, 0.8f, kShuttleHull);
    b.Box("bench_left", -half + wall, -half + wall + 0.45f, deck, deck + 0.45f, -3.6f, 0.8f, kShuttleHull);
    b.Box("nose", -1.5f, 1.5f, 1.1f, 3.0f, back - 1.2f, back, kShuttleTrim, 0.55f, 0.6f);
    b.Box("engine_right", half, half + 0.8f, 1.1f, 2.3f, -5.0f, -1.2f, kShuttleTrim, 0.55f, 0.6f);
    b.Box("engine_left", -half - 0.8f, -half, 1.1f, 2.3f, -5.0f, -1.2f, kShuttleTrim, 0.55f, 0.6f);
    b.Box("fx_engine_glow_right", half + 0.1f, half + 0.7f, 1.2f, 2.2f, -1.2f, -1.15f, {1.0f, 0.55f, 0.25f}, 0.5f, 0.0f, 0.6f);
    b.Box("fx_engine_glow_left", -half - 0.7f, -half - 0.1f, 1.2f, 2.2f, -1.2f, -1.15f, {1.0f, 0.55f, 0.25f}, 0.5f, 0.0f, 0.6f);
    b.Box("fin", -0.08f, 0.08f, ceiling + 0.25f, ceiling + 1.2f, -5.2f, -3.2f, kShuttleTrim, 0.55f, 0.6f);
    b.Box("fx_cabin_light", -0.4f, 0.4f, ceiling - 0.05f, ceiling, -1.9f, -1.1f, {1.0f, 0.95f, 0.85f}, 0.5f, 0.0f, 1.5f);
    // The launch console, against the front wall, its screen on top towards the cabin.
    b.Box("console", -0.5f, 0.5f, deck, deck + 0.95f, back + wall, back + wall + 0.45f, kShuttleTrim, 0.55f, 0.6f);
    b.Box("fx_console_screen", -0.4f, 0.4f, deck + 0.95f, deck + 0.97f, back + wall + 0.06f, back + wall + 0.4f, {0.3f, 0.6f, 1.0f}, 0.4f, 0.0f, 0.6f);
    // As wide as the hull, not just the opening: closed, it covers the whole back, walls' edges and all.
    b.Ramp({0.0f, deck, door}, 2.596f, 2.0f * half - 0.04f, 0.04f, kShuttleHull);
    b.Socket("arrival", {0.0f, deck, -1.0f}, {0.0f, 180.0f, 0.0f});
    b.Socket("console", {0.0f, deck, back + wall + 0.225f}, {0.0f, 180.0f, 0.0f});
    b.Socket("lamp", {0.0f, ceiling - 0.05f, -1.5f});
    // A landing light under its belly, straight down.
    b.Socket("light_landing", {0.0f, 0.2f, -2.0f}, {-90.0f, 0.0f, 0.0f});
    b.Socket("cabin_min", {-half + wall, deck - 0.5f, back + wall});
    b.Socket("cabin_max", {half - wall, ceiling, door});
    return model;
}

ModelAsset BayDoorsModel()
{
    // Nine metres across and fourteen long, their tops flush with the hangar floor at y = 0: two leaves, each hinged
    // along its outer edge, that swing down and out of the way for the shuttle to drop through.
    ModelAsset model;
    model.name = "hangar_doors";
    Builder b{model};
    const float half = 4.5f;
    const float length = 7.0f;
    const float thickness = 0.3f;
    const glm::vec3 steel{0.24f, 0.25f, 0.26f};
    const glm::vec3 yellow{0.72f, 0.55f, 0.1f};
    const glm::vec3 black{0.06f, 0.06f, 0.06f};
    for (const float side : {-1.0f, 1.0f})
    {
        const std::string name = side < 0.0f ? "left" : "right";
        const std::string hinge = "fx_hinge_" + name;
        b.Box(hinge, side * half - 0.005f, side * half + 0.005f, -thickness * 0.5f - 0.005f, -thickness * 0.5f + 0.005f, -0.005f, 0.005f, steel);
        const float outer = side * (half - 0.01f);
        const float inner = side * 0.01f;
        ModelPart& leaf = b.Box("door_" + name, std::min(outer, inner), std::max(outer, inner), -thickness, 0.0f, -length, length, steel, 0.6f, 0.5f);
        leaf.parent = hinge;
        // Stripes across its inner edge, where the two meet.
        for (int stripe = 0; stripe < 10; ++stripe)
        {
            const float z0 = -length + static_cast<float>(stripe) * 1.4f;
            const float edge = side * 0.01f;
            const float band = side * 0.6f;
            ModelPart& mark = b.Box("fx_stripe_" + name + "_" + std::to_string(stripe), std::min(edge, band), std::max(edge, band), 0.0f, 0.004f,
                                    z0, z0 + 1.4f, stripe % 2 == 0 ? yellow : black, 0.7f);
            mark.parent = hinge;
        }
    }
    const auto clip = [&](const std::string& name, float duration, float from, float to)
    {
        AnimationClip animation;
        animation.name = name;
        animation.duration = duration;
        for (const float side : {-1.0f, 1.0f})
        {
            AnimationTrack track;
            track.part = side < 0.0f ? "fx_hinge_left" : "fx_hinge_right";
            AnimationKey first;
            first.time = 0.0f;
            first.rotation = {0.0f, 0.0f, side * from};
            AnimationKey last;
            last.time = duration;
            last.rotation = {0.0f, 0.0f, side * to};
            track.keys = {first, last};
            animation.tracks.push_back(track);
        }
        model.clips.push_back(animation);
    };
    clip("doors_closed", 0.1f, 0.0f, 0.0f);
    clip("doors_opening", 3.5f, 0.0f, 100.0f);
    clip("doors_open", 0.1f, 100.0f, 100.0f);
    clip("doors_closing", 3.5f, 100.0f, 0.0f);
    return model;
}

std::shared_ptr<ModelAsset> Load(const std::string& name)
{
    auto model = std::make_shared<ModelAsset>();
    const std::filesystem::path file = ModelPath(name);
    if (!file.empty() && model->LoadFromFile(file))
    {
        return model;
    }
    if (name == "shuttle")
    {
        *model = ShuttleModel();
    }
    else if (name == "hangar_doors")
    {
        *model = BayDoorsModel();
    }
    else
    {
        return nullptr;
    }
    // Written the first time, so from now on it is a model like any other -- where the game keeps its models, and
    // nowhere else: a test, run from somewhere with no models folder, makes it and leaves the disk alone.
    if (!std::filesystem::exists(Paths::AssetsRoot() / "Models"))
    {
        return model;
    }
    const std::filesystem::path written = ModelPathFor(name, "Vehicles");
    if (model->SaveToFile(written))
    {
        RescanModels();
        PRED_LOG_INFO(Gameplay, "Wrote the {} to {}, to be edited in the model editor", name, written.string());
    }
    return model;
}

} // namespace Vehicles

// --- One standing in the world ---------------------------------------------------------------------------

namespace
{

glm::mat4 Matrix(const CinePose& pose)
{
    return glm::translate(glm::mat4(1.0f), pose.position) * glm::mat4_cast(pose.rotation);
}

} // namespace

bool VehicleProp::Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics, std::shared_ptr<ModelAsset> model, const CinePose& home,
                        uint32_t overlapGroup, const std::string& prefix)
{
    Clear(scene, physics);
    if (model == nullptr)
    {
        return false;
    }
    m_model = std::move(model);
    m_home = home;
    for (const ModelPart& part : m_model->parts)
    {
        Material material = Material::Diffuse(part.color, part.roughness);
        material.metallic = part.metallic;
        material.emissive = part.color * part.emissive;
        if (!part.surface.empty())
        {
            material = Surfaces::Apply(material, part.surface, part.surfaceScale, part.surfaceKeep);
        }
        const MeshHandle mesh = meshes.Upload(m_model->BuildPartMesh(part), "vehicle_" + m_model->name + "_" + part.name);
        m_parts.push_back(scene.CreateMeshEntity(prefix + part.name, Transform{}, mesh, material));
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_parts.back()))
        {
            renderer->castsShadow = part.emissive <= 0.0f;
        }
        // Solid where it rests, all but its glass, its lamps and its glow.
        if (physics == nullptr || part.name.rfind("fx_", 0) == 0 || !part.visible)
        {
            continue;
        }
        const glm::mat4 world = Matrix(home) * m_model->PartMatrixAt(part, nullptr, 0.0f);
        Transform transform;
        transform.position = glm::vec3(world[3]);
        transform.rotation = glm::normalize(glm::quat_cast(glm::mat3(world)));
        const glm::vec3 size = part.shape == PartShape::Box ? part.size
                               : part.shape == PartShape::Sphere ? glm::vec3(part.size.x)
                                                                 : glm::vec3(part.size.x, part.size.y, part.size.x);
        const BodyHandle body = physics->CreateBox(glm::max(size * 0.5f, glm::vec3(0.005f)), transform, BodyMotion::Static);
        if (overlapGroup != 0)
        {
            physics->SetOverlapGroup(body, overlapGroup);
        }
        m_bodies.push_back(body);
    }
    GoHome(scene);
    return true;
}

void VehicleProp::Clear(Scene& scene, PhysicsWorld* physics)
{
    for (const Entity part : m_parts)
    {
        scene.Destroy(part);
    }
    m_parts.clear();
    if (physics != nullptr)
    {
        for (const BodyHandle body : m_bodies)
        {
            physics->DestroyBody(body);
        }
    }
    m_bodies.clear();
    m_model = nullptr;
}

void VehicleProp::Show(Scene& scene, const CinePose& pose, const std::string& clip, float clipTime)
{
    if (m_model == nullptr)
    {
        return;
    }
    m_shown = pose;
    const AnimationClip* animation = clip.empty() ? nullptr : m_model->FindClip(clip);
    const glm::mat4 root = Matrix(pose);
    for (size_t i = 0; i < m_parts.size() && i < m_model->parts.size(); ++i)
    {
        float visible = 1.0f;
        const glm::mat4 world = root * m_model->PartMatrixAt(m_model->parts[i], animation, clipTime, &visible);
        if (Transform* transform = scene.GetTransform(m_parts[i]))
        {
            transform->position = glm::vec3(world[3]);
            transform->rotation = glm::normalize(glm::quat_cast(glm::mat3(world)));
        }
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_parts[i]))
        {
            bool put = false;
            for (const std::string& prefix : m_hiddenPrefixes)
            {
                put = put || m_model->parts[i].name.rfind(prefix, 0) == 0;
            }
            renderer->visible = visible > 0.5f && !m_hidden && !put;
        }
    }
}

void VehicleProp::SetPartsHidden(Scene& scene, const std::string& prefix, bool hidden)
{
    const auto found = std::find(m_hiddenPrefixes.begin(), m_hiddenPrefixes.end(), prefix);
    if (hidden == (found != m_hiddenPrefixes.end()))
    {
        return;
    }
    if (hidden)
    {
        m_hiddenPrefixes.push_back(prefix);
    }
    else
    {
        m_hiddenPrefixes.erase(found);
    }
    if (m_model != nullptr)
    {
        Show(scene, m_shown);
    }
}

void VehicleProp::SetHidden(Scene& scene, bool hidden)
{
    if (hidden == m_hidden)
    {
        return;
    }
    m_hidden = hidden;
    Show(scene, m_shown);
}

Entity VehicleProp::Part(const std::string& name) const
{
    if (m_model == nullptr)
    {
        return Entity{};
    }
    for (size_t i = 0; i < m_model->parts.size() && i < m_parts.size(); ++i)
    {
        if (m_model->parts[i].name == name)
        {
            return m_parts[i];
        }
    }
    return Entity{};
}

bool VehicleProp::Socket(const std::string& name, CinePose& out) const
{
    if (m_model == nullptr)
    {
        return false;
    }
    const ModelSocket* socket = m_model->FindSocket(name);
    if (socket == nullptr)
    {
        return false;
    }
    out.position = m_home.Apply(socket->position);
    out.rotation = m_home.rotation * socket->Rotation();
    return true;
}

bool VehicleProp::SocketShown(const std::string& name, CinePose& out) const
{
    const ModelSocket* socket = m_model != nullptr ? m_model->FindSocket(name) : nullptr;
    if (socket == nullptr)
    {
        return false;
    }
    out.position = m_shown.Apply(socket->position);
    out.rotation = m_shown.rotation * socket->Rotation();
    return true;
}

bool VehicleProp::Aboard(const glm::vec3& point) const
{
    if (m_model == nullptr)
    {
        return false;
    }
    const ModelSocket* low = m_model->FindSocket("cabin_min");
    const ModelSocket* high = m_model->FindSocket("cabin_max");
    if (low == nullptr || high == nullptr)
    {
        return false;
    }
    const glm::vec3 local = glm::inverse(m_home.rotation) * (point - m_home.position);
    const glm::vec3 lo = glm::min(low->position, high->position);
    const glm::vec3 hi = glm::max(low->position, high->position);
    return local.x > lo.x && local.x < hi.x && local.y > lo.y && local.y < hi.y && local.z > lo.z && local.z < hi.z;
}

} // namespace pred
