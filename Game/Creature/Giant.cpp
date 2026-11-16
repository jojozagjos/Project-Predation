#include "Game/Creature/Giant.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/MeshSimplify.h"
#include "Engine/Render/Sdf.h"
#include "Engine/Render/SurfaceNets.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/CreatureTraits.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

namespace pred
{

namespace
{

constexpr float kTau = glm::two_pi<float>();
// A squid's arm: so many links, each a bone.
constexpr int kArmLinks = 6;

glm::quat Yawed(float yaw)
{
    return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

float Flat(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(glm::vec2(a.x - b.x, a.z - b.z));
}

float Wrap(float angle)
{
    return std::remainder(angle, kTau);
}

glm::mat4 Unscaled(const glm::mat4& m)
{
    glm::mat4 out = m;
    for (int c = 0; c < 3; ++c)
    {
        const float length = glm::length(glm::vec3(m[c]));
        if (length > 1e-6f)
        {
            out[c] = m[c] / length;
        }
    }
    return out;
}

// Where a ray from `inside` out along `way` leaves a shape: on its skin, for an eye or a light to sit on rather than sink into.
glm::vec3 OnSkin(const std::function<float(const glm::vec3&)>& field, const glm::vec3& inside, const glm::vec3& way, float most)
{
    const glm::vec3 dir = glm::normalize(way);
    float t = 0.0f;
    for (int i = 0; i < 64 && t < most; ++i)
    {
        const float d = field(inside + dir * t);
        if (d > -0.01f)
        {
            break;
        }
        t += std::max(-d * 0.8f, most / 200.0f);
    }
    return inside + dir * t;
}

// Small spheres -- eyes, lights -- as one mesh, white for an emissive material.
MeshData Beads(const std::vector<glm::vec4>& beads)
{
    MeshData mesh;
    for (const glm::vec4& bead : beads)
    {
        const int rings = 6;
        const int around = 8;
        const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
        for (int j = 0; j <= rings; ++j)
        {
            const float v = static_cast<float>(j) / rings * glm::pi<float>();
            for (int i = 0; i <= around; ++i)
            {
                const float u = static_cast<float>(i) / around * kTau;
                const glm::vec3 n{std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u)};
                MeshVertex vertex;
                vertex.position = glm::vec3(bead) + n * bead.w;
                vertex.normal = n;
                vertex.color = 0xffffffffu;
                mesh.vertices.push_back(vertex);
            }
        }
        for (int j = 0; j < rings; ++j)
        {
            for (int i = 0; i < around; ++i)
            {
                const uint32_t a = first + static_cast<uint32_t>(j * (around + 1) + i);
                const uint32_t b = a + static_cast<uint32_t>(around + 1);
                // (Wound to face out: the other way round, only their far insides were drawn -- a crescent of each.)
                mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
            }
        }
    }
    return mesh;
}

// A box's distance, its edges rounded by r: exact, so a cut made with it never leaves the sampling a wrong idea of where the surface is
// (an ellipsoid long and thin enough, cut out of a head, left a hole through it).
float RoundedBox(const glm::vec3& p, const glm::vec3& half, float r)
{
    const glm::vec3 q = glm::abs(p) - half + glm::vec3(r);
    return glm::length(glm::max(q, glm::vec3(0.0f))) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f) - r;
}

// A frame at a point of a body going along `back` (from its front towards its tail): right, up, back -- turned about `back` by `roll`.
glm::mat4 Frame(const glm::vec3& at, const glm::vec3& back, float roll)
{
    const glm::vec3 z = glm::normalize(back);
    glm::vec3 x = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), z);
    x = glm::length(x) > 1e-4f ? glm::normalize(x) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 y = glm::cross(z, x);
    if (std::abs(roll) > 1e-4f)
    {
        const glm::quat turn = glm::angleAxis(roll, z);
        x = turn * x;
        y = turn * y;
    }
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(x, 0.0f);
    m[1] = glm::vec4(y, 0.0f);
    m[2] = glm::vec4(z, 0.0f);
    m[3] = glm::vec4(at, 1.0f);
    return m;
}

} // namespace

const char* Giant::ModeName(Mode mode)
{
    switch (mode)
    {
    case Mode::Roam: return "roam";
    case Mode::Notice: return "notice";
    case Mode::Approach: return "approach";
    case Mode::Play: return "play";
    case Mode::Strike: return "strike";
    case Mode::Linger: return "linger";
    case Mode::Hold: return "hold";
    case Mode::Dead: return "dead";
    case Mode::Count: break;
    }
    return "?";
}

const char* Giant::FormName(Form form)
{
    switch (form)
    {
    case Form::Serpent: return "serpent";
    case Form::Whale: return "whale";
    case Form::Fish: return "plated fish";
    case Form::Ray: return "ray";
    case Form::Squid: return "squid";
    case Form::Count: break;
    }
    return "?";
}

Giant::Giant(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, GiantKind kind, uint32_t seed, const glm::vec3& at, float yaw)
    : m_scene(scene), m_meshes(meshes), m_physics(physics), m_kind(kind), m_seed(seed), m_look(CreatureAnatomy::FromSeed(seed)),
      m_position(at), m_yaw(yaw)
{
    const auto started = std::chrono::steady_clock::now();
    if (kind == GiantKind::Walker)
    {
        BuildWalker(meshes);
    }
    else
    {
        BuildSwimmer(meshes);
    }
    m_lookAt = at + Yawed(yaw) * glm::vec3(0.0f, 0.0f, -50.0f);
    m_roamTo = at;
    m_holdPoint = at;
    // (Up at the surface rarely: once in some minutes, its own.)
    m_nextSurfaceAt = 300.0f + static_cast<float>(seed % 240u);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(AI, "Giant made in {:.0f} ms: {}", ms, Describe());
}

Giant::~Giant()
{
    for (Entity* entity : {&m_skinEntity, &m_glowEntity, &m_eyeEntity})
    {
        if (entity->IsValid())
        {
            m_scene.Destroy(*entity);
            *entity = Entity{};
        }
    }
    for (const BoneBox& box : m_boxes)
    {
        m_physics.DestroyBody(box.body);
    }
    for (const MeshHandle mesh : {m_skinMesh, m_glowMesh, m_eyeMesh})
    {
        if (mesh.IsValid())
        {
            m_meshes.Release(mesh);
        }
    }
}

std::string Giant::Describe() const
{
    char line[512];
    if (m_kind == GiantKind::Walker)
    {
        std::snprintf(line, sizeof(line), "a strider %.0f m tall: %s (%.0f of %.0f health)", m_bodyHeight, m_look.Describe().c_str(), m_health, m_maxHealth);
    }
    else
    {
        std::snprintf(line, sizeof(line), "a leviathan %.0f m long, a %s; %s, %s%s (%.0f of %.0f health)", m_size, FormName(m_form), ColourFamilyName(m_look.family),
                      SkinPatternName(m_look.pattern), m_glowMesh.IsValid() ? ", glowing" : "", m_health, m_maxHealth);
    }
    return line;
}

void Giant::MakeVisual(MeshLibrary& meshes, const MeshData& skin, const MeshData* glow, const MeshData* eyes, bool eyesGlow, bool dynamicEyes)
{
    const std::string prefix = "giant_" + std::to_string(m_seed) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this));
    m_skinMesh = meshes.CreateDynamic(skin, prefix + "_skin");
    Material flesh = Material::Diffuse(glm::vec3(1.0f), m_look.roughness);
    flesh.weathering = 0.0f;
    m_skinEntity = m_scene.CreateMeshEntity(prefix + "_skin", Transform{}, m_skinMesh, flesh);
    if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_skinEntity))
    {
        renderer->castsShadow = true;
        renderer->blocksSky = false;
        renderer->farVisible = true;
    }
    if (glow != nullptr && !glow->indices.empty())
    {
        m_glowMesh = meshes.CreateDynamic(*glow, prefix + "_glow");
        Material lights = Material::Diffuse(glm::vec3(0.02f), 0.3f);
        lights.emissive = (glm::length(m_look.glow) > 0.01f ? m_look.glow : m_look.eyeGlow) * 3.0f;
        lights.weathering = 0.0f;
        m_glowEntity = m_scene.CreateMeshEntity(prefix + "_glow", Transform{}, m_glowMesh, lights);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_glowEntity))
        {
            renderer->castsShadow = false;
        }
    }
    if (eyes != nullptr && !eyes->indices.empty())
    {
        m_eyeMesh = dynamicEyes ? meshes.CreateDynamic(*eyes, prefix + "_eyes") : meshes.Upload(*eyes, prefix + "_eyes");
        // Lit, the eyes of some -- the first of it seen in the dark or the deep; wet black glass, with a glint, the rest.
        Material look = Material::Diffuse(eyesGlow ? glm::vec3(0.02f) : glm::vec3(0.015f), eyesGlow ? 0.3f : 0.08f);
        look.emissive = eyesGlow ? m_look.eyeGlow * 2.5f : m_look.eyeGlow * 0.04f;
        look.weathering = 0.0f;
        m_eyeEntity = m_scene.CreateMeshEntity(prefix + "_eyes", Transform{}, m_eyeMesh, look);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_eyeEntity))
        {
            renderer->castsShadow = false;
        }
    }
}

// --- The strider: a creature's body, made huge ---------------------------------------------------------------------------------

void Giant::BuildWalker(MeshLibrary& meshes)
{
    SeededRandom random(static_cast<uint64_t>(m_seed) * 2862933555777941757ull + 3037000493ull);
    // How big: most of them twenty-odd metres, now and then one three times that.
    const float h = 18.0f + 42.0f * std::pow(random.Unit(), 2.2f);
    m_skin = std::make_shared<const CreatureSkin>(CreatureSkin::Build(m_look));
    const CreatureSkin& skin = *m_skin;
    m_scale = h / std::max(m_look.TopHeight(), 0.3f);
    m_bodyHeight = h;
    m_size = h;
    m_speed = 2.4f + h * 0.045f;
    const float S = m_scale;
    m_bodyRadii = {m_look.width * S * 0.5f, std::max((h - m_look.hipHeight * S * 0.9f) / 1.2f, 1.0f), m_look.length * S * 0.5f};
    m_maxHealth = 1500.0f + 90.0f * h;
    m_health = m_maxHealth;
    // What it holds things in: a crawler's long hands; anything else its jaws.
    if (m_look.plan == BodyPlan::Crawler)
    {
        const CreatureAnatomy::RestPose rest = m_look.Rest();
        for (size_t i = 0; i < skin.limbs.size() && i < rest.legs.size(); ++i)
        {
            if (rest.legs[i].pair->arm)
            {
                m_hand = skin.limbs[i].end;
                break;
            }
        }
    }
    // Its eyes: the pinpricks in its sockets, made big enough to be seen at its size.
    MeshData eyes;
    if (!skin.glints.empty())
    {
        std::vector<glm::vec4> beads;
        for (const glm::vec3& at : skin.glints)
        {
            beads.push_back(glm::vec4(at, skin.glintRadius * 1.8f));
        }
        eyes = Beads(beads);
    }
    MakeVisual(meshes, skin.mesh, skin.glow.vertices.empty() ? nullptr : &skin.glow, &eyes, true, false);
    if (m_skinEntity.IsValid())
    {
        if (Transform* transform = m_scene.GetTransform(m_skinEntity))
        {
            transform->scale = glm::vec3(S);
        }
    }
    m_rig.Init(m_look, skin, m_position / S, -m_yaw);
    // What rounds find: a capsule round every bone with thickness to it, at its size.
    for (size_t b = 0; b < skin.bones.size(); ++b)
    {
        const SkinBone& bone = skin.bones[b];
        if (bone.kind == BoneKind::Jaw)
        {
            continue;
        }
        const float length = glm::distance(bone.head, bone.tail) * S;
        const float radius = std::clamp(bone.radius * 1.1f * S, 0.3f, std::max(length * 0.7f, 0.4f));
        BoneBox box;
        box.bone = static_cast<int>(b);
        box.halfLength = glm::distance(bone.head, bone.tail) * 0.5f;
        box.head = static_cast<int>(b) == skin.head;
        box.body = m_physics.CreateCapsule(std::max(length * 0.5f - radius * 0.5f, 0.05f), radius, Transform{}, BodyMotion::Kinematic, 1000.0f, PhysicsLayer::Hitbox);
        if (box.body.IsValid())
        {
            m_boxes.push_back(box);
        }
    }
}

void Giant::AnimateWalker(const GiantWorld& world, float time, float dt)
{
    (void)time;
    const CreatureSkin& skin = *m_skin;
    const float S = m_scale;
    // As slowly as a thing that size moves: a step that is a third of a second for something a couple of metres tall is a second and
    // more for something twenty times that (time goes with the root of size).
    const float slow = 1.0f / std::sqrt(S);
    const float rigDt = dt * slow;
    m_rigTime += rigDt;
    RigInput input;
    input.position = m_position / S;
    input.yaw = -m_yaw;
    input.velocity = m_mode == Mode::Dead ? glm::vec3(0.0f) : m_velocity / (S * slow);
    input.ground = [&world, S](const glm::vec3& from, float, float& height)
    {
        if (!world.ground)
        {
            return false;
        }
        height = world.ground(from.x * S, from.z * S) / S;
        return true;
    };
    input.look = m_mode != Mode::Dead;
    input.lookAt = m_lookAt / S;
    if (m_mode == Mode::Strike)
    {
        input.action = m_hand >= 0 ? RigAction::Grab : RigAction::Bite;
        input.actionPhase = m_strike;
        input.actionTarget = m_strikeAt / S;
        input.windup = m_strike < 0.6f ? glm::smoothstep(0.0f, 0.6f, m_strike) : 0.0f;
    }
    else if (m_mode == Mode::Hold)
    {
        input.action = m_hand >= 0 ? RigAction::Carry : RigAction::None;
        input.lookAt = m_holdPoint / S;
    }
    // Drawn up on its back legs, one that does, to look at what it has found.
    const bool stands = m_look.uprightLean > 0.0f && (m_mode == Mode::Notice || m_mode == Mode::Play);
    m_upright += ((stands ? 1.0f : 0.0f) - m_upright) * std::min(dt * 0.6f, 1.0f);
    input.upright = m_upright;
    input.time = m_rigTime;
    input.dt = rigDt;
    if (m_mode != Mode::Dead || m_topple < 0.05f)
    {
        m_rig.Update(input);
    }
    const std::vector<glm::mat4>& bones = m_rig.Bones();
    const glm::mat4& root = m_rig.Root();
    m_skinning.resize(bones.size());
    for (size_t b = 0; b < bones.size(); ++b)
    {
        m_skinning[b] = bones[b] * skin.bones[b].restInverse;
    }
    AABB bounds;
    SkinVertices(skin, m_skinning, m_skinned, bounds);
    m_meshes.UpdateDynamic(m_skinMesh, m_skinned, bounds);

    // Where it is drawn: its rig's root, as big as it is -- and killed, toppled over onto its side about the ground under it.
    glm::vec3 at = glm::vec3(root[3]) * S;
    glm::quat turn = glm::quat_cast(glm::mat3(Unscaled(root)));
    if (m_mode == Mode::Dead)
    {
        const float was = m_topple;
        m_topple = std::min(m_topple + dt / 3.2f, 1.0f);
        const float fall = m_topple * m_topple;
        const glm::vec3 ahead = turn * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::quat over = glm::angleAxis(fall * glm::half_pi<float>() * 0.92f * m_toppleSide, ahead);
        const glm::vec3 pivot{at.x, world.ground ? world.ground(at.x, at.z) : at.y, at.z};
        at = pivot + over * (at - pivot);
        turn = over * turn;
        if (was < 1.0f && m_topple >= 1.0f && m_footfalls.size() < 16)
        {
            m_footfalls.push_back(pivot);
        }
    }
    const glm::mat4 draw = glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(turn) * glm::scale(glm::mat4(1.0f), glm::vec3(S));
    const auto place = [&](Entity entity)
    {
        if (Transform* transform = entity.IsValid() ? m_scene.GetTransform(entity) : nullptr)
        {
            transform->position = at;
            transform->rotation = turn;
            transform->scale = glm::vec3(S);
        }
    };
    place(m_skinEntity);
    if (m_glowEntity.IsValid())
    {
        AABB glowBounds;
        SkinMesh(skin.glow, skin.glowWeights, m_skinning, m_glowSkinned, glowBounds);
        m_meshes.UpdateDynamic(m_glowMesh, m_glowSkinned, glowBounds);
        place(m_glowEntity);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_glowEntity))
        {
            renderer->material.emissive = m_mode == Mode::Dead ? glm::vec3(0.0f) : (glm::length(m_look.glow) > 0.01f ? m_look.glow : m_look.eyeGlow) * 3.0f;
        }
    }
    const glm::mat4 headWorld = draw * m_skinning[static_cast<size_t>(skin.head)];
    if (m_eyeEntity.IsValid())
    {
        if (Transform* transform = m_scene.GetTransform(m_eyeEntity))
        {
            transform->position = glm::vec3(headWorld[3]);
            transform->rotation = glm::quat_cast(glm::mat3(Unscaled(headWorld)));
            transform->scale = glm::vec3(S);
        }
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_eyeEntity))
        {
            renderer->material.emissive = m_mode == Mode::Dead ? glm::vec3(0.0f) : m_look.eyeGlow * 2.5f;
        }
    }
    const SkinBone& headBone = skin.bones[static_cast<size_t>(skin.head)];
    m_head = glm::vec3(headWorld * glm::vec4((headBone.head + headBone.tail) * 0.5f, 1.0f));
    // What it holds, where it holds it: in its hand, or its jaws -- eased, so whoever it is is carried, not shaken.
    glm::vec3 hold = m_head;
    if (m_hand >= 0)
    {
        hold = glm::vec3(draw * m_skinning[static_cast<size_t>(m_hand)] * glm::vec4(skin.bones[static_cast<size_t>(m_hand)].tail, 1.0f));
    }
    else if (skin.jaw >= 0)
    {
        hold = glm::vec3(draw * m_skinning[static_cast<size_t>(skin.jaw)] * glm::vec4(skin.bones[static_cast<size_t>(skin.jaw)].tail, 1.0f));
    }
    m_holdWanted = hold;
    m_holdPoint = m_holding >= 0 ? m_holdPoint + (hold - m_holdPoint) * std::min(dt * 5.0f, 1.0f) : hold;
    // Its feet coming down.
    for (const glm::vec3& foot : m_rig.TakeFootfalls())
    {
        if (m_mode != Mode::Dead && m_footfalls.size() < 16)
        {
            m_footfalls.push_back(foot * S);
        }
    }
    // What rounds find, where its bones are.
    for (const BoneBox& box : m_boxes)
    {
        const glm::mat4 bone = draw * bones[static_cast<size_t>(box.bone)];
        Transform transform;
        transform.position = glm::vec3(bone * glm::vec4(0.0f, box.halfLength, 0.0f, 1.0f));
        transform.rotation = glm::quat_cast(glm::mat3(Unscaled(bone)));
        m_physics.SetTransform(box.body, transform);
    }
}

// --- The leviathan: a body of its own --------------------------------------------------------------------------------------------

void Giant::BuildSwimmer(MeshLibrary& meshes)
{
    SeededRandom random(static_cast<uint64_t>(m_seed) * 6364136223846793005ull + 1442695040888963407ull);
    // Its kind of body: a serpent most often, then the rest.
    {
        const float pick = random.Unit();
        m_form = pick < 0.28f ? Form::Serpent : pick < 0.48f ? Form::Whale : pick < 0.66f ? Form::Fish : pick < 0.83f ? Form::Ray : Form::Squid;
    }
    const float L = 28.0f + 52.0f * std::pow(random.Unit(), 1.8f);
    m_size = L;
    m_speed = 3.2f + L * 0.03f;
    m_maxHealth = 1500.0f + 90.0f * L;
    m_health = m_maxHealth;
    m_spineBones = 14;
    const int N = m_spineBones;
    // How thick it is, by its kind and its own.
    const float R = L * (m_form == Form::Serpent ? random.Range(0.04f, 0.06f) : m_form == Form::Whale ? random.Range(0.1f, 0.13f)
                         : m_form == Form::Fish   ? random.Range(0.08f, 0.11f) : m_form == Form::Ray ? random.Range(0.035f, 0.05f)
                                                  : random.Range(0.08f, 0.1f));
    const float wings = m_form == Form::Ray ? L * random.Range(0.32f, 0.48f) : 0.0f;
    const glm::vec2 squash = m_form == Form::Fish ? glm::vec2(0.72f, 1.15f) : m_form == Form::Whale ? glm::vec2(1.0f, 0.88f) : glm::vec2(1.0f);
    const bool crest = m_form == Form::Serpent && random.Unit() < 0.6f;
    const float bumps = m_form == Form::Fish ? 0.16f : 0.07f;
    const uint32_t grain = m_seed * 31u + 7u;
    const auto profile = [&](float t)
    {
        switch (m_form)
        {
        case Form::Serpent:
            return R * (0.7f + 0.3f * glm::smoothstep(0.0f, 0.12f, t)) * (1.0f - 0.85f * std::pow(t, 1.6f));
        case Form::Whale:
            return R * (0.55f + 0.45f * std::sin(glm::pi<float>() * std::min(t * 1.4f, 1.0f))) * (1.0f - 0.82f * glm::smoothstep(0.5f, 1.0f, t));
        case Form::Fish:
            return R * (0.6f + 0.4f * std::sin(glm::pi<float>() * std::min(t * 1.6f, 1.0f))) * (1.0f - 0.85f * glm::smoothstep(0.45f, 0.95f, t));
        case Form::Ray:
            return R * (1.0f - 0.85f * glm::smoothstep(0.45f, 1.0f, t));
        case Form::Squid:
            return t < 0.47f ? R * (0.6f + 0.4f * glm::smoothstep(0.0f, 0.2f, t)) : R * (0.75f - 0.4f * glm::smoothstep(0.47f, 0.6f, t));
        case Form::Count:
            break;
        }
        return R;
    };
    m_radius.resize(static_cast<size_t>(N));
    for (int k = 0; k < N; ++k)
    {
        m_radius[static_cast<size_t>(k)] = std::max(profile(static_cast<float>(k) / static_cast<float>(N - 1)), 0.4f);
    }
    // A squid's arms: from round the back of its head, trailing.
    struct Arm
    {
        std::vector<glm::vec3> points;
    };
    std::vector<Arm> arms;
    if (m_form == Form::Squid)
    {
        m_arms = 6 + 2 * static_cast<int>(random.Unit() * 2.0f);
        for (int j = 0; j < m_arms; ++j)
        {
            const float a = kTau * (static_cast<float>(j) + 0.5f) / static_cast<float>(m_arms);
            const glm::vec3 root{std::cos(a) * R * 0.5f, std::sin(a) * R * 0.5f, 0.58f * L};
            const glm::vec3 end{std::cos(a) * R * 1.5f, std::sin(a) * R * 1.5f, L};
            Arm arm;
            for (int i = 0; i <= kArmLinks; ++i)
            {
                arm.points.push_back(glm::mix(root, end, static_cast<float>(i) / kArmLinks));
            }
            arms.push_back(arm);
            m_armRoots.push_back(root);
        }
    }
    const float mouthR = profile(0.04f);
    const glm::vec3 mouth{0.0f, -0.12f * mouthR, 0.02f * L};
    const glm::vec3 mouthRadii{0.72f * mouthR, 0.13f * mouthR, 0.07f * L};
    const bool hasMouth = m_form != Form::Squid;
    // Its teeth: a row down each lip, into the slot of the mouth.
    std::vector<std::pair<glm::vec3, glm::vec3>> teeth;
    if (hasMouth)
    {
        const int count = 6 + static_cast<int>(random.Unit() * 6.0f);
        for (int i = 0; i < count; ++i)
        {
            const float s = (static_cast<float>(i) + 0.5f) / static_cast<float>(count) * 2.0f - 1.0f;
            const float x = s * mouthRadii.x * 0.85f;
            const float z = mouth.z - mouthRadii.z * 0.6f + std::abs(s) * mouthRadii.z * 0.9f;
            teeth.push_back({glm::vec3(x, mouth.y + mouthRadii.y * 1.4f, z), glm::vec3(x, mouth.y + mouthRadii.y * 0.1f, z)});
            teeth.push_back({glm::vec3(x, mouth.y - mouthRadii.y * 1.4f, z + mouthRadii.z * 0.05f), glm::vec3(x, mouth.y - mouthRadii.y * 0.1f, z + mouthRadii.z * 0.05f)});
        }
    }
    const float toothR = mouthR * 0.05f;
    constexpr int kSamples = 24;
    const auto field = [&](const glm::vec3& p)
    {
        const float t = std::clamp(p.z / L, 0.0f, 1.0f);
        // Its body: its spine's line, round in section -- or flattened or deepened, by its kind.
        const glm::vec3 q{p.x / squash.x, p.y / squash.y, p.z};
        float d = 1.0e9f;
        const float tailEnd = m_form == Form::Squid ? 0.6f : 1.0f;
        for (int i = 0; i < kSamples; ++i)
        {
            const float a = tailEnd * static_cast<float>(i) / kSamples;
            const float b = tailEnd * static_cast<float>(i + 1) / kSamples;
            if (std::abs(t - (a + b) * 0.5f) > 0.12f && i != 0 && i != kSamples - 1)
            {
                continue;
            }
            d = std::min(d, Sdf::RoundCone(q, glm::vec3(0.0f, 0.0f, a * L), glm::vec3(0.0f, 0.0f, b * L), profile(a), profile(b)));
        }
        d *= std::min(squash.x, squash.y);
        switch (m_form)
        {
        case Form::Serpent:
            if (crest)
            {
                for (int i = 1; i < 9; ++i)
                {
                    const float ct = static_cast<float>(i) / 10.0f;
                    d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, profile(ct) * 0.85f, ct * L), glm::vec3(0.06f * R, 0.55f * profile(ct), 0.07f * L)), R * 0.2f);
                }
            }
            break;
        case Form::Whale:
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, 0.0f, 0.97f * L), glm::vec3(0.22f * L, 0.018f * L, 0.05f * L)), R * 0.3f);
            for (const float side : {-1.0f, 1.0f})
            {
                d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(side * profile(0.25f) * 1.05f, -profile(0.25f) * 0.45f, 0.25f * L), glm::vec3(0.12f * L, 0.016f * L, 0.045f * L)),
                                     R * 0.25f);
            }
            break;
        case Form::Fish:
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, 0.0f, 0.97f * L), glm::vec3(0.016f * L, 0.17f * L, 0.06f * L)), R * 0.25f);
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, profile(0.35f) * squash.y * 0.95f, 0.35f * L), glm::vec3(0.012f * L, 0.1f * L, 0.08f * L)), R * 0.2f);
            break;
        case Form::Ray:
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, 0.0f, 0.28f * L), glm::vec3(wings, 0.045f * L, 0.27f * L)), R * 0.6f);
            break;
        case Form::Squid:
            // Its fins at its mantle's point, and its arms.
            for (const float side : {-1.0f, 1.0f})
            {
                d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(side * R * 0.9f, 0.0f, 0.1f * L), glm::vec3(0.22f * L, 0.012f * L, 0.08f * L)), R * 0.2f);
            }
            for (const Arm& arm : arms)
            {
                for (int i = 0; i < kArmLinks; ++i)
                {
                    const float ra = R * 0.22f * (1.0f - 0.8f * static_cast<float>(i) / kArmLinks);
                    const float rb = R * 0.22f * (1.0f - 0.8f * static_cast<float>(i + 1) / kArmLinks);
                    d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, arm.points[static_cast<size_t>(i)], arm.points[static_cast<size_t>(i + 1)], ra, rb), R * 0.12f);
                }
            }
            break;
        case Form::Count:
            break;
        }
        // Its mouth, cut in under the front of its head, its teeth in it.
        if (hasMouth && p.z < 0.2f * L)
        {
            d = Sdf::SmoothCut(d, RoundedBox(p - mouth, mouthRadii, mouthRadii.y * 0.8f), mouthR * 0.05f);
            for (const auto& [base, tip] : teeth)
            {
                d = std::min(d, Sdf::RoundCone(p, base, tip, toothR, toothR * 0.2f));
            }
        }
        // Lumped, plated or scarred a little, as grown things are.
        return d + (Sdf::Fbm(p / (R * 0.7f), grain, 2) - 0.5f) * R * bumps;
    };
    const float reachX = std::max({R * 1.8f, wings * 1.05f, m_form == Form::Whale ? 0.25f * L : 0.0f, m_form == Form::Squid ? R * 1.7f : 0.0f});
    const float reachY = std::max(R * 1.6f * squash.y, m_form == Form::Fish ? 0.2f * L : 0.0f);
    const glm::vec3 lo{-reachX, -reachY, -0.04f * L};
    const glm::vec3 hi{reachX, reachY, 1.04f * L};
    const float cell = std::max(L / 120.0f, 0.22f);
    const DistanceGrid grid = SampleDistance(lo, hi, cell, field);
    MeshData mesh = SurfaceNets(grid);
    {
        std::vector<uint32_t> kept;
        mesh = SimplifyMesh(mesh, 12000, cell * 0.05f, kept);
    }
    const auto normalAt = [&](const glm::vec3& p)
    {
        const float h = cell * 0.5f;
        const glm::vec3 n{field(p + glm::vec3(h, 0, 0)) - field(p - glm::vec3(h, 0, 0)), field(p + glm::vec3(0, h, 0)) - field(p - glm::vec3(0, h, 0)),
                          field(p + glm::vec3(0, 0, h)) - field(p - glm::vec3(0, 0, h))};
        const float length = glm::length(n);
        return length > 1e-6f ? n / length : glm::vec3(0.0f, 1.0f, 0.0f);
    };
    // Painted as a creature of its colours is: darker on its back than its belly, its own markings; the inside of its mouth dark and
    // its teeth pale.
    for (MeshVertex& vertex : mesh.vertices)
    {
        glm::vec3 p = vertex.position;
        p -= normalAt(p) * field(p);
        vertex.position = p;
        vertex.normal = normalAt(p);
        const float back = glm::smoothstep(-0.4f, 0.5f, vertex.normal.y);
        glm::vec3 colour = glm::mix(m_look.underside, m_look.skin, back);
        colour = glm::mix(colour, m_look.markings, SkinMarkingAt(m_look, p, vertex.normal, R * 0.6f));
        if (hasMouth && p.z < 0.2f * L)
        {
            // (Only the slot's own walls: what the cut left, at the cut's surface -- the whole front of its face had been painted it.)
            const float inMouth = 1.0f - glm::smoothstep(0.0f, mouthR * 0.03f, RoundedBox(p - mouth, mouthRadii, mouthRadii.y * 0.8f));
            colour = glm::mix(colour, glm::vec3(0.18f, 0.04f, 0.05f), inMouth);
            for (const auto& [base, tip] : teeth)
            {
                if (Sdf::RoundCone(p, base, tip, toothR, toothR * 0.2f) < toothR * 0.3f)
                {
                    colour = glm::vec3(0.82f, 0.79f, 0.68f);
                }
            }
        }
        vertex.color = SkinColour(colour, 0.9f);
        vertex.uv = {0.0f, 0.0f};
    }

    // Its bones: the spine, head first; a ray's two wings; a squid's arms, a chain each.
    m_wingBones = m_form == Form::Ray ? 2 : 0;
    const int bones = N + m_wingBones + m_arms * kArmLinks;
    m_restInverse.assign(static_cast<size_t>(bones), glm::mat4(1.0f));
    for (int k = 0; k < N; ++k)
    {
        m_restInverse[static_cast<size_t>(k)] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -L * static_cast<float>(k) / static_cast<float>(N - 1)));
    }
    for (int w = 0; w < m_wingBones; ++w)
    {
        m_restInverse[static_cast<size_t>(N + w)] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -0.28f * L));
    }
    for (int j = 0; j < m_arms; ++j)
    {
        for (int i = 0; i < kArmLinks; ++i)
        {
            m_restInverse[static_cast<size_t>(N + m_wingBones + j * kArmLinks + i)] = glm::translate(glm::mat4(1.0f), -arms[static_cast<size_t>(j)].points[static_cast<size_t>(i)]);
        }
    }
    m_armPoints.clear();
    for (const Arm& arm : arms)
    {
        m_armPoints.push_back(arm.points);
    }
    // Which bones each point of it goes with: along the spine, two at a time; out on a wing, with the wing; on an arm, its link.
    const auto weigh = [&](const glm::vec3& p)
    {
        SkinWeights weights;
        if (m_form == Form::Squid && p.z > 0.56f * L && glm::length(glm::vec2(p.x, p.y)) > R * 0.3f)
        {
            float best = 1.0e9f;
            int bestArm = 0;
            int bestLink = 0;
            for (int j = 0; j < m_arms; ++j)
            {
                for (int i = 0; i < kArmLinks; ++i)
                {
                    const glm::vec3& a = arms[static_cast<size_t>(j)].points[static_cast<size_t>(i)];
                    const glm::vec3& b = arms[static_cast<size_t>(j)].points[static_cast<size_t>(i + 1)];
                    const glm::vec3 ab = b - a;
                    const float s = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
                    const float distance = glm::distance(p, a + ab * s);
                    if (distance < best)
                    {
                        best = distance;
                        bestArm = j;
                        bestLink = i;
                    }
                }
            }
            weights.bone[0] = static_cast<uint8_t>(N + m_wingBones + bestArm * kArmLinks + bestLink);
            weights.weight[0] = 1.0f;
            return weights;
        }
        const float s = std::clamp(p.z / L, 0.0f, 1.0f) * static_cast<float>(N - 1);
        const int b0 = std::min(static_cast<int>(s), N - 2);
        const float f = std::clamp(s - static_cast<float>(b0), 0.0f, 1.0f);
        float wing = 0.0f;
        if (m_form == Form::Ray && p.z < 0.6f * L)
        {
            wing = glm::smoothstep(0.25f * wings, 0.9f * wings, std::abs(p.x));
        }
        weights.bone[0] = static_cast<uint8_t>(b0);
        weights.weight[0] = (1.0f - f) * (1.0f - wing);
        weights.bone[1] = static_cast<uint8_t>(b0 + 1);
        weights.weight[1] = f * (1.0f - wing);
        if (wing > 0.0f)
        {
            weights.bone[2] = static_cast<uint8_t>(N + (p.x > 0.0f ? 1 : 0));
            weights.weight[2] = wing;
        }
        return weights;
    };
    m_weights.clear();
    for (const MeshVertex& vertex : mesh.vertices)
    {
        m_weights.push_back(weigh(vertex.position));
    }
    m_rest = mesh;

    // Its eyes, on its head (a squid's at the back of its mantle, where its arms grow from), big on a squid; lit on some.
    std::vector<glm::vec4> eyes;
    const int pairs = std::clamp(m_look.eyes / 2, 1, 3);
    const float eyeR = (m_form == Form::Squid ? R * 0.22f : mouthR * random.Range(0.1f, 0.16f));
    for (int k = 0; k < pairs; ++k)
    {
        for (const float side : {-1.0f, 1.0f})
        {
            const float z = m_form == Form::Squid ? 0.53f * L + static_cast<float>(k) * R * 0.5f : (0.035f + 0.03f * static_cast<float>(k)) * L;
            const glm::vec3 inside{0.0f, profile(z / L) * (0.2f + 0.18f * static_cast<float>(k)), z};
            const glm::vec3 on = OnSkin(field, inside, glm::vec3(side, 0.25f, 0.0f), R * 3.0f);
            eyes.push_back(glm::vec4(on - glm::vec3(side * eyeR * 0.35f, 0.0f, 0.0f), eyeR));
        }
    }
    m_eyeRest = Beads(eyes);
    m_eyeWeights.clear();
    for (const MeshVertex& vertex : m_eyeRest.vertices)
    {
        m_eyeWeights.push_back(weigh(vertex.position));
    }
    const bool eyesGlow = m_look.photophores > 0 || random.Unit() < 0.4f;
    // Its lights, on some: rows down its flanks.
    m_glowRest = MeshData{};
    m_glowWeights.clear();
    if (m_look.photophores > 0)
    {
        std::vector<glm::vec4> lights;
        const int count = 10 + static_cast<int>(random.Unit() * 14.0f);
        for (int i = 0; i < count; ++i)
        {
            const float t = 0.1f + 0.75f * static_cast<float>(i) / static_cast<float>(count - 1);
            for (const float side : {-1.0f, 1.0f})
            {
                const glm::vec3 inside{0.0f, profile(t) * 0.1f, t * L};
                const glm::vec3 on = OnSkin(field, inside, glm::vec3(side, -0.15f, 0.0f), std::max(R, wings) * 3.0f);
                lights.push_back(glm::vec4(on, std::max(profile(t) * 0.06f, 0.15f)));
            }
        }
        m_glowRest = Beads(lights);
        for (const MeshVertex& vertex : m_glowRest.vertices)
        {
            m_glowWeights.push_back(weigh(vertex.position));
        }
    }
    MakeVisual(meshes, m_rest, m_glowRest.indices.empty() ? nullptr : &m_glowRest, &m_eyeRest, eyesGlow, true);
    m_skinning.assign(static_cast<size_t>(bones), glm::mat4(1.0f));
    // What rounds find: a capsule down each length of its spine, its head the first.
    for (int k = 0; k + 1 < N; ++k)
    {
        BoneBox box;
        box.bone = k;
        box.halfLength = L / static_cast<float>(N - 1) * 0.5f;
        box.head = m_form == Form::Squid ? k == N / 2 + 1 : k == 0;
        box.body = m_physics.CreateCapsule(box.halfLength, std::max(m_radius[static_cast<size_t>(k)] * (m_form == Form::Ray && k < N / 2 ? 1.6f : 1.0f), 0.5f),
                                           Transform{}, BodyMotion::Kinematic, 1000.0f, PhysicsLayer::Hitbox);
        if (box.body.IsValid())
        {
            m_boxes.push_back(box);
        }
    }
    // Laid out straight behind where it starts.
    const glm::vec3 back = Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 64; ++i)
    {
        m_trail.push_back(m_position + back * (L * 1.3f * static_cast<float>(i) / 63.0f));
    }
    for (std::vector<glm::vec3>& arm : m_armPoints)
    {
        for (glm::vec3& point : arm)
        {
            point = m_position + Yawed(m_yaw) * point;
        }
    }
}

void Giant::AnimateSwimmer(const GiantWorld& world, float time, float dt)
{
    (void)world;
    (void)time;
    const int N = m_spineBones;
    const float L = m_size;
    // Where its front has been: the rest of it follows the same way, as a swimming thing's body does.
    if (m_trail.empty() || glm::distance(m_trail.front(), m_position) > L / 200.0f)
    {
        m_trail.push_front(m_position);
        while (m_trail.size() > 400)
        {
            m_trail.pop_back();
        }
    }
    const auto alongTrail = [&](float distance)
    {
        float walked = 0.0f;
        glm::vec3 last = m_position;
        for (const glm::vec3& point : m_trail)
        {
            const float step = glm::distance(last, point);
            if (walked + step >= distance && step > 1e-5f)
            {
                return glm::mix(last, point, (distance - walked) / step);
            }
            walked += step;
            last = point;
        }
        // Past the end of where it has been: on straight behind.
        const glm::vec3 back = m_trail.size() >= 2 ? glm::normalize(m_trail.back() - m_trail[m_trail.size() - 2] + glm::vec3(1e-5f)) : Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, 1.0f);
        return last + back * (distance - walked);
    };
    const bool dead = m_mode == Mode::Dead;
    const float speed = glm::length(m_velocity);
    // The beat: quicker the faster it goes; stilled, dead.
    if (!dead)
    {
        m_swimPhase += dt * (0.5f + speed / std::max(L * 0.08f, 1.0f)) * (m_form == Form::Whale ? 0.7f : m_form == Form::Ray ? 0.6f : 1.0f);
    }
    m_roll += ((dead ? glm::pi<float>() : 0.0f) - m_roll) * std::min(dt * 0.4f, 1.0f);
    std::vector<glm::vec3> spine(static_cast<size_t>(N));
    for (int k = 0; k < N; ++k)
    {
        spine[static_cast<size_t>(k)] = alongTrail(L * static_cast<float>(k) / static_cast<float>(N - 1));
    }
    std::vector<glm::mat4> frames(static_cast<size_t>(N));
    for (int k = 0; k < N; ++k)
    {
        const glm::vec3& here = spine[static_cast<size_t>(k)];
        const glm::vec3 back = k + 1 < N ? spine[static_cast<size_t>(k + 1)] - here : here - spine[static_cast<size_t>(k - 1)];
        frames[static_cast<size_t>(k)] = Frame(here, glm::length(back) > 1e-4f ? back : Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, 1.0f), m_roll);
    }
    // Its swimming, by its kind: waves down it from side to side, a beat up and down, nearly still with its wings beating, or pulsing.
    for (int k = 0; k < N; ++k)
    {
        const float t = static_cast<float>(k) / static_cast<float>(N - 1);
        const float wave = std::sin(m_swimPhase * kTau - t * 4.5f);
        glm::vec3 offset{0.0f};
        const glm::vec3 right = glm::vec3(frames[static_cast<size_t>(k)][0]);
        const glm::vec3 up = glm::vec3(frames[static_cast<size_t>(k)][1]);
        switch (m_form)
        {
        case Form::Serpent: offset = right * (0.07f * L * t * wave); break;
        case Form::Fish: offset = right * (0.06f * L * std::pow(glm::smoothstep(0.35f, 1.0f, t), 1.5f) * wave); break;
        case Form::Whale: offset = up * (0.045f * L * glm::smoothstep(0.45f, 1.0f, t) * wave); break;
        case Form::Ray: offset = up * (0.012f * L * wave) + right * (0.02f * L * glm::smoothstep(0.6f, 1.0f, t) * wave); break;
        case Form::Squid: offset = right * (0.01f * L * t * wave); break;
        case Form::Count: break;
        }
        if (dead)
        {
            offset *= 0.0f;
        }
        frames[static_cast<size_t>(k)][3] += glm::vec4(offset, 0.0f);
        m_skinning[static_cast<size_t>(k)] = frames[static_cast<size_t>(k)] * m_restInverse[static_cast<size_t>(k)];
    }
    // A ray's wings, beating.
    if (m_wingBones == 2)
    {
        const int mid = static_cast<int>(0.28f * static_cast<float>(N - 1) + 0.5f);
        const float flap = dead ? 0.0f : 0.45f * std::sin(m_swimPhase * kTau);
        const glm::mat4 at = frames[static_cast<size_t>(mid)];
        for (int w = 0; w < 2; ++w)
        {
            const float side = w == 0 ? -1.0f : 1.0f;
            const glm::mat4 bent = at * glm::mat4_cast(glm::angleAxis(flap * side, glm::vec3(0.0f, 0.0f, 1.0f)));
            m_skinning[static_cast<size_t>(N + w)] = bent * m_restInverse[static_cast<size_t>(N + w)];
        }
    }
    // A squid's arms, trailing from where they grow, each link following the last.
    if (m_arms > 0)
    {
        const int base = static_cast<int>(0.58f * static_cast<float>(N - 1) + 0.5f);
        const glm::mat4 at = frames[static_cast<size_t>(base)];
        const float link = (L * 0.42f) / static_cast<float>(kArmLinks);
        for (int j = 0; j < m_arms; ++j)
        {
            std::vector<glm::vec3>& points = m_armPoints[static_cast<size_t>(j)];
            const glm::vec3 root = m_armRoots[static_cast<size_t>(j)];
            points[0] = glm::vec3(at * glm::vec4(root.x, root.y, 0.0f, 1.0f));
            const glm::vec3 back = glm::vec3(at[2]);
            const float sway = std::sin(m_swimPhase * kTau * 0.7f + static_cast<float>(j)) * 0.15f;
            for (int i = 1; i <= kArmLinks; ++i)
            {
                glm::vec3& p = points[static_cast<size_t>(i)];
                // Drawn back behind it by the water, spread a little, curling.
                const glm::vec3 spread = glm::normalize(glm::vec3(at * glm::vec4(root.x, root.y, 0.0f, 0.0f)) + glm::vec3(1e-4f));
                p += (points[static_cast<size_t>(i - 1)] + (back + spread * (0.25f + sway)) * link - p) * std::min(dt * 3.0f, 1.0f);
                const glm::vec3 d = p - points[static_cast<size_t>(i - 1)];
                p = points[static_cast<size_t>(i - 1)] + (glm::length(d) > 1e-4f ? glm::normalize(d) : back) * link;
            }
            for (int i = 0; i < kArmLinks; ++i)
            {
                const glm::vec3 a = points[static_cast<size_t>(i)];
                const glm::vec3 b = points[static_cast<size_t>(i + 1)];
                const glm::mat4 frame = Frame(a, b - a, 0.0f);
                // The link at rest ran from its start along its own way; turned onto where it is now.
                const glm::vec3 restA = glm::vec3(-m_restInverse[static_cast<size_t>(N + m_wingBones + j * kArmLinks + i)][3]);
                const glm::vec3 restB = i + 1 < kArmLinks ? glm::vec3(-m_restInverse[static_cast<size_t>(N + m_wingBones + j * kArmLinks + i + 1)][3])
                                                          : restA + (restA - glm::vec3(-m_restInverse[static_cast<size_t>(N + m_wingBones + j * kArmLinks + i - 1)][3]));
                const glm::mat4 restFrame = Frame(restA, restB - restA, 0.0f);
                m_skinning[static_cast<size_t>(N + m_wingBones + j * kArmLinks + i)] = frame * glm::inverse(restFrame);
            }
        }
    }
    // (Drawn about where it is, not about the world's origin: placed at the origin, the whole of it was taken for part of whatever is
    // built there -- the testing area -- and hidden with it while the eye was at the site.)
    const glm::vec3 origin = m_position;
    const auto recentre = [&](std::vector<MeshVertex>& vertices, AABB& box)
    {
        for (MeshVertex& vertex : vertices)
        {
            vertex.position -= origin;
        }
        box.min -= origin;
        box.max -= origin;
    };
    const auto place = [&](Entity entity)
    {
        if (Transform* transform = entity.IsValid() ? m_scene.GetTransform(entity) : nullptr)
        {
            transform->position = origin;
            transform->rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
    };
    AABB bounds;
    SkinMesh(m_rest, m_weights, m_skinning, m_skinned, bounds);
    recentre(m_skinned, bounds);
    m_meshes.UpdateDynamic(m_skinMesh, m_skinned, bounds);
    place(m_skinEntity);
    if (m_glowEntity.IsValid())
    {
        AABB glowBounds;
        SkinMesh(m_glowRest, m_glowWeights, m_skinning, m_glowSkinned, glowBounds);
        recentre(m_glowSkinned, glowBounds);
        m_meshes.UpdateDynamic(m_glowMesh, m_glowSkinned, glowBounds);
        place(m_glowEntity);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_glowEntity))
        {
            renderer->material.emissive = dead ? glm::vec3(0.0f) : (glm::length(m_look.glow) > 0.01f ? m_look.glow : m_look.eyeGlow) * 3.0f;
        }
    }
    if (m_eyeEntity.IsValid())
    {
        AABB eyeBounds;
        SkinMesh(m_eyeRest, m_eyeWeights, m_skinning, m_eyeSkinned, eyeBounds);
        recentre(m_eyeSkinned, eyeBounds);
        m_meshes.UpdateDynamic(m_eyeMesh, m_eyeSkinned, eyeBounds);
        place(m_eyeEntity);
        if (dead)
        {
            if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_eyeEntity))
            {
                renderer->material.emissive = glm::vec3(0.0f);
            }
        }
    }
    // Its head: the front of it (a squid's, where its arms grow); its jaws just under and ahead of it.
    const int headBone = m_form == Form::Squid ? static_cast<int>(0.55f * static_cast<float>(N - 1) + 0.5f) : 0;
    const glm::mat4& headFrame = frames[static_cast<size_t>(headBone)];
    m_head = glm::vec3(headFrame[3]) + glm::vec3(headFrame[1]) * (m_radius[static_cast<size_t>(headBone)] * 0.2f);
    const glm::vec3 jaws = m_form == Form::Squid ? glm::vec3(headFrame[3]) + glm::vec3(headFrame[2]) * (L * 0.06f)
                                                 : glm::vec3(headFrame[3]) - glm::vec3(headFrame[1]) * (m_radius[0] * 0.15f) - glm::vec3(headFrame[2]) * (m_radius[0] * 0.2f);
    m_holdWanted = jaws;
    m_holdPoint = m_holding >= 0 ? m_holdPoint + (jaws - m_holdPoint) * std::min(dt * 5.0f, 1.0f) : jaws;
    // What rounds find, down its spine.
    for (const BoneBox& box : m_boxes)
    {
        const glm::vec3 a = glm::vec3(frames[static_cast<size_t>(box.bone)][3]);
        const glm::vec3 b = glm::vec3(frames[static_cast<size_t>(box.bone + 1)][3]);
        Transform transform;
        transform.position = (a + b) * 0.5f;
        const glm::vec3 along = b - a;
        transform.rotation = glm::length(along) > 1e-4f ? glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(along)) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        m_physics.SetTransform(box.body, transform);
    }
}

// --- The mind -----------------------------------------------------------------------------------------------------------------

bool Giant::Sees(const GiantWorld& world, const GiantSensed& who) const
{
    if (!who.alive || who.roofed)
    {
        return false;
    }
    const glm::vec3 chest = who.feet + glm::vec3(0.0f, who.height * 0.7f, 0.0f);
    const float away = glm::distance(m_head, chest);
    if (m_kind == GiantKind::Swimmer)
    {
        if (!who.inWater || away > 80.0f)
        {
            return false;
        }
    }
    else if (away > 300.0f)
    {
        return false;
    }
    const glm::vec3 ahead = Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 to = chest - m_head;
    if (glm::dot(glm::normalize(glm::vec3(to.x, 0.0f, to.z) + glm::vec3(1e-4f)), ahead) < -0.35f)
    {
        return false;
    }
    return !world.clear || world.clear(m_head, chest);
}

void Giant::Steer(const GiantWorld& world, const glm::vec3& goal, float speed, float dt)
{
    glm::vec3 wanted = goal;
    const glm::vec2 fromMiddle = glm::vec2(m_position.x, m_position.z) - world.middle;
    if (glm::length(fromMiddle) > world.reach)
    {
        wanted = glm::vec3(world.middle.x, m_position.y, world.middle.y);
    }
    float bearing = std::atan2(-(wanted.x - m_position.x), -(wanted.z - m_position.z));
    // Round what is in its way rather than through it: a building its body would not clear (the strider), anything built (the
    // leviathan) -- felt for ahead, a little to each side, the clearer way taken.
    if (world.builtUp)
    {
        const float lookAhead = m_kind == GiantKind::Walker ? m_bodyRadii.z + 12.0f : m_size * 0.4f + 8.0f;
        const float clears = m_kind == GiantKind::Walker ? m_bodyHeight - m_bodyRadii.y * 1.2f : 0.0f;
        const auto blocked = [&](float angle)
        {
            for (const float along : {0.5f, 1.0f})
            {
                const glm::vec3 at = m_position + Yawed(angle) * glm::vec3(0.0f, 0.0f, -lookAhead * along);
                const float built = world.builtUp(at.x, at.z);
                if (built > 0.0f && (m_kind == GiantKind::Swimmer || built > clears))
                {
                    return true;
                }
            }
            return false;
        };
        if (blocked(bearing))
        {
            for (const float off : {0.6f, -0.6f, 1.2f, -1.2f, 1.9f, -1.9f})
            {
                if (!blocked(bearing + off))
                {
                    bearing += off;
                    break;
                }
            }
        }
    }
    const float turnRate = m_kind == GiantKind::Walker ? 0.45f : 0.6f;
    const float turn = std::clamp(Wrap(bearing - m_yaw), -turnRate * dt, turnRate * dt);
    m_yaw = Wrap(m_yaw + turn);
    const float facing = std::cos(Wrap(bearing - m_yaw));
    const float pace = speed * glm::mix(0.35f, 1.0f, std::max(facing, 0.0f));
    const glm::vec3 ahead = Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 velocity = ahead * pace;
    if (m_kind == GiantKind::Swimmer)
    {
        // Up and down slowly, its depth eased: never a head bobbing up and straight back down.
        velocity.y = std::clamp((wanted.y - m_position.y) * 0.35f, -1.6f, 1.6f);
    }
    m_velocity += (velocity - m_velocity) * std::min(dt * 1.2f, 1.0f);
    m_position += m_velocity * dt;
    if (m_kind == GiantKind::Walker)
    {
        if (world.ground)
        {
            m_position.y = world.ground(m_position.x, m_position.z);
        }
    }
    else if (world.water && world.ground)
    {
        // Between the sea's floor and its surface -- up to it, its back just out of the water, when it surfaces.
        float surface = 0.0f;
        const float floor = world.ground(m_position.x, m_position.z);
        const float r = m_radius.empty() ? 2.0f : m_radius.front();
        if (world.water(glm::vec3(m_position.x, floor + 0.5f, m_position.z), surface) > 0.0f)
        {
            const float top = m_surfaceUntil > 0.0f ? surface - r * 0.35f : surface - r - 1.5f;
            m_position.y = std::clamp(m_position.y, floor + r + 1.0f, std::max(top, floor + r + 1.0f));
        }
        else
        {
            m_position.y = std::max(m_position.y, floor + r + 1.0f);
        }
    }
}

void Giant::Enter(Mode mode, float time, const char* goal)
{
    if (m_mode != mode)
    {
        m_mode = mode;
        m_modeSince = time;
    }
    m_goal = goal;
}

void Giant::Think(const GiantWorld& world, const std::vector<GiantSensed>& people, float time, float dt)
{
    m_shownOnly = false;
    // Dead: still. A walker where it fell; a leviathan floats up, belly first, to the surface and lies there.
    if (m_mode == Mode::Dead)
    {
        m_velocity *= std::max(1.0f - dt * 0.8f, 0.0f);
        if (m_kind == GiantKind::Swimmer && world.water && world.ground)
        {
            float surface = 0.0f;
            const float floor = world.ground(m_position.x, m_position.z);
            if (world.water(glm::vec3(m_position.x, floor + 0.5f, m_position.z), surface) > 0.0f)
            {
                const float r = m_radius.empty() ? 2.0f : m_radius.front();
                m_position.y = std::min(m_position.y + dt * 0.5f, surface - r * 0.3f);
            }
        }
        m_position += glm::vec3(m_velocity.x, 0.0f, m_velocity.z) * dt;
        m_shownAt = m_position;
        m_shownYaw = m_yaw;
        return;
    }
    for (const GiantSensed& who : people)
    {
        auto found = std::find_if(m_aware.begin(), m_aware.end(), [&](const auto& a) { return a.first == who.id; });
        if (found == m_aware.end())
        {
            m_aware.push_back({who.id, 0.0f});
            found = m_aware.end() - 1;
        }
        if (Sees(world, who))
        {
            const float away = glm::distance(m_head, who.feet);
            const float range = m_kind == GiantKind::Swimmer ? 80.0f : 300.0f;
            const float moving = std::min(glm::length(who.velocity) / 3.0f, 1.0f);
            const float rate = (1.0f - away / range) * (0.35f + 0.65f * moving) * (who.height < 1.3f ? 0.55f : 1.0f) * (0.4f + 0.6f * who.light);
            found->second = std::min(found->second + rate * dt * 0.55f, 1.5f);
            if (found->second >= 1.0f && (m_target < 0 || m_target == who.id))
            {
                m_target = who.id;
                m_targetAt = who.feet;
                m_lastSeen = time;
            }
        }
        else
        {
            found->second = std::max(found->second - dt * 0.05f, 0.0f);
        }
    }
    const GiantSensed* target = nullptr;
    const GiantSensed* held = nullptr;
    for (const GiantSensed& who : people)
    {
        if (who.id == m_target && who.alive)
        {
            target = &who;
        }
        if (who.id == m_holding)
        {
            held = &who;
        }
    }
    if (target == nullptr && m_target >= 0 && m_holding < 0)
    {
        m_target = -1;
    }
    m_annoyed = std::max(m_annoyed - dt * 0.01f, 0.0f);
    m_hunger = std::min(m_hunger + dt * 0.004f, 1.0f);
    // Now and then -- rarely, once in some minutes -- with nothing on its mind, the leviathan comes up to the surface for a while.
    if (m_kind == GiantKind::Swimmer)
    {
        if (m_surfaceUntil > 0.0f && time > m_surfaceUntil)
        {
            m_surfaceUntil = -1.0f;
            m_nextSurfaceAt = time + 360.0f + static_cast<float>(m_seed % 300u);
        }
        if (m_mode == Mode::Roam && m_surfaceUntil < 0.0f && time > m_nextSurfaceAt)
        {
            m_surfaceUntil = time + 30.0f;
        }
    }
    const float reachOver = m_kind == GiantKind::Walker ? m_bodyHeight * 0.55f : 7.0f;
    const bool seenNow = target != nullptr && time - m_lastSeen < 0.5f;
    if (m_mode != Mode::Hold && target != nullptr && target->roofed)
    {
        Enter(Mode::Linger, time, "waiting about where they went in");
    }
    else if (m_mode != Mode::Hold && target != nullptr && m_kind == GiantKind::Swimmer && !target->inWater)
    {
        Enter(Mode::Linger, time, "circling where they left the water");
    }

    switch (m_mode)
    {
    case Mode::Roam:
    {
        m_goal = m_surfaceUntil > 0.0f ? "up at the surface" : "going about";
        if (!m_haveRoam || Flat(m_position, m_roamTo) < 15.0f)
        {
            SeededRandom random(static_cast<uint64_t>(m_seed) + static_cast<uint64_t>(time * 10.0f));
            const float a = random.Range(0.0f, kTau);
            const float r = world.reach * random.Range(0.3f, 0.9f);
            m_roamTo = glm::vec3(world.middle.x + std::cos(a) * r, m_position.y, world.middle.y + std::sin(a) * r);
            if (m_kind == GiantKind::Swimmer && world.ground && world.water)
            {
                float surface = 0.0f;
                const float floor = world.ground(m_roamTo.x, m_roamTo.z);
                const float depth = world.water(glm::vec3(m_roamTo.x, floor + 0.5f, m_roamTo.z), surface);
                m_roamTo.y = depth > 0.0f ? floor + depth * random.Range(0.3f, 0.7f) : m_position.y;
            }
            m_haveRoam = true;
        }
        glm::vec3 goal = m_roamTo;
        if (m_surfaceUntil > 0.0f)
        {
            goal.y += 1000.0f; // (held to the surface by Steer)
        }
        Steer(world, goal, m_speed, dt);
        m_lookAt = m_position + Yawed(m_yaw) * glm::vec3(0.0f, -5.0f, -60.0f);
        for (const auto& [id, aware] : m_aware)
        {
            if (aware > 0.4f)
            {
                for (const GiantSensed& who : people)
                {
                    if (who.id == id)
                    {
                        m_targetAt = who.feet;
                    }
                }
                Enter(Mode::Notice, time, "has seen something; looking");
            }
        }
        if (target != nullptr)
        {
            Enter(Mode::Approach, time, "coming to look at what it has found");
            m_call = true;
        }
        break;
    }
    case Mode::Notice:
    {
        m_velocity *= std::max(1.0f - dt * 1.5f, 0.0f);
        m_position += m_velocity * dt;
        m_lookAt = m_targetAt + glm::vec3(0.0f, 1.0f, 0.0f);
        if (target != nullptr)
        {
            Enter(Mode::Approach, time, "coming to look at what it has found");
            m_call = true;
        }
        else if (time - m_modeSince > 8.0f)
        {
            Enter(Mode::Roam, time, "going about");
        }
        break;
    }
    case Mode::Approach:
    {
        if (target == nullptr)
        {
            Enter(Mode::Linger, time, "looking for where it went");
            break;
        }
        m_targetAt = seenNow ? target->feet : m_targetAt;
        m_lookAt = m_targetAt + glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 goal = m_kind == GiantKind::Swimmer ? m_targetAt + glm::vec3(0.0f, 2.0f, 0.0f) : m_targetAt;
        Steer(world, goal, m_speed * (m_kind == GiantKind::Walker ? 1.7f : 1.4f), dt);
        if (Flat(m_kind == GiantKind::Walker ? m_position : m_head, m_targetAt) < reachOver * 1.6f)
        {
            Enter(Mode::Play, time, "over it, looking it over");
            // (Not long: a few seconds of looking, sooner the rougher it is playing -- then it reaches for them.)
            m_playUntil = time + (m_kind == GiantKind::Walker ? 3.0f : 5.0f) * (1.0f - 0.6f * m_annoyed);
        }
        break;
    }
    case Mode::Play:
    {
        if (target == nullptr)
        {
            Enter(Mode::Linger, time, "looking for where it went");
            break;
        }
        m_targetAt = target->feet;
        m_lookAt = m_targetAt + glm::vec3(0.0f, 0.8f, 0.0f);
        if (m_kind == GiantKind::Walker)
        {
            if (Flat(m_position, m_targetAt) > reachOver * 0.8f)
            {
                Steer(world, m_targetAt, m_speed * 0.8f, dt);
            }
            else
            {
                m_velocity *= std::max(1.0f - dt * 2.0f, 0.0f);
                m_position += m_velocity * dt;
            }
        }
        else
        {
            const float round = time * 0.35f;
            const float radius = glm::mix(26.0f, 10.0f, std::clamp((time - m_modeSince) / 10.0f, 0.0f, 1.0f));
            Steer(world, m_targetAt + glm::vec3(std::cos(round) * radius, 1.0f, std::sin(round) * radius), m_speed * 1.2f, dt);
        }
        if (time >= m_playUntil && time >= m_nextStrikeAt)
        {
            Enter(Mode::Strike, time, m_kind == GiantKind::Walker ? "reaching down for it" : "goes for it");
            m_strike = 0.0f;
            m_struck = false;
            // Where it reaches -- where they are going to be, near enough -- fixed now: seen coming, it can be got away from.
            m_strikeAt = target->feet + target->velocity * 0.6f;
        }
        break;
    }
    case Mode::Strike:
    {
        const float windup = m_kind == GiantKind::Walker ? 1.9f : 1.2f;
        // (The leviathan's rush the longer: from where it circled, a good few lengths of it away, it has to get there.)
        const float duration = windup + (m_kind == GiantKind::Walker ? 0.9f : 2.2f);
        m_strike = std::clamp((time - m_modeSince) / duration, 0.0f, 1.0f);
        m_lookAt = m_strikeAt + glm::vec3(0.0f, 0.5f, 0.0f);
        if (m_kind == GiantKind::Swimmer)
        {
            const bool rushing = time - m_modeSince > windup;
            if (target != nullptr && rushing)
            {
                m_strikeAt = target->feet + glm::vec3(0.0f, 0.9f, 0.0f);
            }
            Steer(world, rushing ? m_strikeAt + glm::normalize(m_strikeAt - m_position + glm::vec3(1e-4f)) * 6.0f : m_strikeAt, m_speed * (rushing ? 3.2f : 0.8f), dt);
        }
        else
        {
            m_velocity *= std::max(1.0f - dt * 3.0f, 0.0f);
            m_position += m_velocity * dt;
        }
        const float landsAt = windup / duration;
        if (!m_struck && m_strike >= landsAt)
        {
            m_struck = true;
            const glm::vec3 reachedTo = m_kind == GiantKind::Walker ? m_strikeAt : m_holdWanted;
            const float radius = m_kind == GiantKind::Walker ? 3.2f : 5.0f;
            const GiantSensed* caught = nullptr;
            float nearest = radius;
            for (const GiantSensed& who : people)
            {
                if (!who.alive || who.roofed)
                {
                    continue;
                }
                const float off = m_kind == GiantKind::Walker ? Flat(who.feet, reachedTo) : glm::distance(who.feet + glm::vec3(0.0f, 0.9f, 0.0f), reachedTo);
                // (Nothing solid between: never through a wall, a roof, the shuttle's hull.)
                const glm::vec3 chest = who.feet + glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::vec3 from = m_kind == GiantKind::Walker ? chest + glm::vec3(0.0f, 6.0f, 0.0f) : m_head;
                if (off < nearest && (!world.clear || world.clear(from, chest)))
                {
                    nearest = off;
                    caught = &who;
                }
            }
            // (The leviathan's jaws close on whoever they reach at any moment of its rush, not only at its first.)
            if (caught == nullptr && m_kind == GiantKind::Swimmer && m_strike < 1.0f)
            {
                m_struck = false;
            }
            if (caught != nullptr)
            {
                Blow blow;
                blow.player = caught->id;
                blow.at = reachedTo;
                // Taken up -- the strider's into its hand or its jaws, to be looked at; the leviathan's, now and then, in its jaws to be
                // dragged down; otherwise a bite in passing.
                const bool takes = m_kind == GiantKind::Walker || (m_seed + static_cast<uint32_t>(time * 3.0f)) % 5u < 3u;
                if (takes)
                {
                    blow.grab = true;
                    blow.damage = m_kind == GiantKind::Swimmer ? 20.0f : 0.0f;
                    m_holding = caught->id;
                    m_holdSince = time;
                    m_hurtHolding = 0.0f;
                    m_holdPoint = caught->feet + glm::vec3(0.0f, 1.0f, 0.0f);
                    m_nextBiteAt = time + 1.2f;
                    m_eats = false;
                    Enter(Mode::Hold, time, m_kind == GiantKind::Walker ? "has it; lifting it up to look at" : "has it in its jaws; taking it down");
                }
                else
                {
                    blow.damage = std::clamp(55.0f + 70.0f * m_annoyed + 25.0f * (1.0f - nearest / radius), 30.0f, 140.0f);
                    const glm::vec3 away = caught->feet - reachedTo;
                    blow.push = (glm::length(away) > 0.1f ? glm::normalize(away) : Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f)) * 9.0f + glm::vec3(0.0f, 3.0f, 0.0f);
                }
                m_blows.push_back(blow);
            }
        }
        if (m_strike >= 1.0f && m_mode == Mode::Strike)
        {
            m_strike = 0.0f;
            m_nextStrikeAt = time + glm::mix(5.0f, 2.0f, m_annoyed);
            Enter(target != nullptr ? Mode::Play : Mode::Linger, time, target != nullptr ? "missed; over it again" : "looking for where it went");
            m_playUntil = time + glm::mix(4.0f, 1.0f, m_annoyed);
        }
        break;
    }
    case Mode::Hold:
    {
        // Whoever it holds is dead: done with them -- eaten.
        if (held == nullptr || !held->alive)
        {
            Blow blow;
            blow.player = m_holding;
            blow.release = true;
            m_blows.push_back(blow);
            m_holding = -1;
            m_hunger = 0.0f;
            m_nextStrikeAt = time + 10.0f;
            Enter(target != nullptr ? Mode::Play : Mode::Roam, time, "finished with what it held");
            break;
        }
        m_strike = 0.0f;
        const float heldFor = time - m_holdSince;
        if (m_kind == GiantKind::Walker)
        {
            // Lifted to its eyes and turned over, looked at; then -- for a reason -- eaten, hungry or angry, or thrown away, played with
            // enough. Never dropped for nothing.
            m_velocity *= std::max(1.0f - dt * 2.0f, 0.0f);
            m_position += m_velocity * dt;
            m_lookAt = m_holdPoint;
            if (heldFor < 2.5f)
            {
                m_goal = "has it; lifting it up to look at";
                break;
            }
            if (heldFor < 6.0f)
            {
                m_goal = "turning it over in front of its eyes";
                break;
            }
            if (!m_eats && heldFor < 6.0f + dt * 1.5f)
            {
                m_eats = m_hunger > 0.5f || m_annoyed > 0.45f;
            }
            if (m_eats)
            {
                m_goal = m_annoyed > 0.45f ? "angry: eating what it holds" : "hungry: eating what it holds";
                if (time >= m_nextBiteAt)
                {
                    Blow bite;
                    bite.player = m_holding;
                    bite.damage = 40.0f + 30.0f * m_annoyed;
                    bite.at = m_holdPoint;
                    m_blows.push_back(bite);
                    m_nextBiteAt = time + 1.4f;
                }
                break;
            }
            // Done playing: thrown away, hard.
            Blow blow;
            blow.player = m_holding;
            blow.at = m_holdPoint;
            blow.damage = 20.0f;
            blow.push = Yawed(m_yaw + 1.2f) * glm::vec3(0.0f, 0.0f, -1.0f) * 16.0f + glm::vec3(0.0f, 7.0f, 0.0f);
            blow.release = true;
            m_blows.push_back(blow);
            m_holding = -1;
            m_nextStrikeAt = time + 8.0f;
            Enter(Mode::Play, time, "tired of it; threw it away");
            m_playUntil = time + 4.0f;
            break;
        }
        // The leviathan: down with them, chewing, deeper and deeper -- and let go far under, for them to swim back up from if they can.
        m_lookAt = m_holdPoint;
        Steer(world, m_position + Yawed(m_yaw) * glm::vec3(0.0f, -12.0f, -14.0f), m_speed * 0.9f, dt);
        m_goal = "dragging it down in its jaws";
        if (time >= m_nextBiteAt)
        {
            Blow bite;
            bite.player = m_holding;
            bite.damage = 14.0f + 16.0f * m_annoyed;
            bite.at = m_holdPoint;
            m_blows.push_back(bite);
            m_nextBiteAt = time + 1.3f;
        }
        if (heldFor > 9.0f)
        {
            Blow blow;
            blow.player = m_holding;
            blow.at = m_holdPoint;
            blow.push = glm::vec3(0.0f, -2.0f, 0.0f);
            blow.release = true;
            m_blows.push_back(blow);
            m_holding = -1;
            m_nextStrikeAt = time + 12.0f;
            Enter(Mode::Linger, time, "let go of it far under");
        }
        break;
    }
    case Mode::Linger:
    {
        m_lookAt = m_targetAt + glm::vec3(0.0f, 2.0f, 0.0f);
        if (m_kind == GiantKind::Swimmer)
        {
            const float round = time * 0.2f;
            Steer(world, m_targetAt + glm::vec3(std::cos(round) * 30.0f, -4.0f, std::sin(round) * 30.0f), m_speed, dt);
        }
        else if (Flat(m_position, m_targetAt) > m_bodyHeight * 0.6f)
        {
            Steer(world, m_targetAt, m_speed, dt);
        }
        else
        {
            m_velocity *= std::max(1.0f - dt, 0.0f);
            m_position += m_velocity * dt;
        }
        const bool back = target != nullptr && !target->roofed && (m_kind == GiantKind::Walker || target->inWater);
        if (back)
        {
            Enter(Mode::Approach, time, "there it is again");
        }
        else if (time - m_modeSince > 30.0f)
        {
            m_target = -1;
            for (auto& entry : m_aware)
            {
                entry.second = 0.0f;
            }
            Enter(Mode::Roam, time, "lost interest");
            m_haveRoam = false;
        }
        break;
    }
    case Mode::Dead:
    case Mode::Count:
        break;
    }
    m_shownAt = m_position;
    m_shownYaw = m_yaw;
}

void Giant::Drop(float time, const char* why)
{
    if (m_holding < 0)
    {
        return;
    }
    m_holding = -1;
    if (m_mode == Mode::Dead)
    {
        return;
    }
    m_mode = Mode::Play;
    m_modeSince = time;
    m_playUntil = time + 3.0f;
    m_nextStrikeAt = time + 5.0f;
    m_goal = why;
}

void Giant::TakeDamage(float amount, int byPlayer, float time, BodyHandle hit)
{
    if (m_mode == Mode::Dead)
    {
        return;
    }
    // In the head, most: the place to aim.
    for (const BoneBox& box : m_boxes)
    {
        if (box.head && hit.IsValid() && box.body == hit)
        {
            amount *= 2.5f;
        }
    }
    m_health = std::max(m_health - amount, 0.0f);
    if (m_health <= 0.0f)
    {
        // Killed: it lets go of anybody it had, and falls -- over onto one side, or belly up in the water.
        if (m_holding >= 0)
        {
            Blow blow;
            blow.player = m_holding;
            blow.release = true;
            m_blows.push_back(blow);
            m_holding = -1;
        }
        m_mode = Mode::Dead;
        m_modeSince = time;
        m_deadSince = time;
        m_goal = "dead";
        m_toppleSide = (m_seed & 1u) != 0u ? 1.0f : -1.0f;
        m_call = true;
        return;
    }
    // Felt, not feared: it turns on whoever did it, and plays rougher -- and hurt enough while it holds somebody, it lets them go.
    m_annoyed = std::min(m_annoyed + amount / 250.0f, 1.0f);
    if (m_holding >= 0)
    {
        m_hurtHolding += amount;
        if (m_hurtHolding > std::max(260.0f, m_maxHealth * 0.06f))
        {
            Blow blow;
            blow.player = m_holding;
            blow.release = true;
            m_blows.push_back(blow);
            Drop(time, "shot until it let go");
        }
    }
    auto found = std::find_if(m_aware.begin(), m_aware.end(), [&](const auto& a) { return a.first == byPlayer; });
    if (found == m_aware.end())
    {
        m_aware.push_back({byPlayer, 1.0f});
    }
    else
    {
        found->second = std::max(found->second, 1.0f);
    }
    if (m_target < 0 && byPlayer >= 0)
    {
        m_target = byPlayer;
        m_lastSeen = time;
    }
    if (m_mode == Mode::Roam || m_mode == Mode::Notice)
    {
        m_mode = Mode::Approach;
        m_modeSince = time;
        m_call = true;
    }
}

bool Giant::Owns(BodyHandle body) const
{
    return std::any_of(m_boxes.begin(), m_boxes.end(), [&](const BoneBox& box) { return box.body == body; });
}

std::vector<Giant::Blow> Giant::TakeBlows()
{
    return std::exchange(m_blows, {});
}

std::vector<glm::vec3> Giant::TakeFootfalls()
{
    return std::exchange(m_footfalls, {});
}

bool Giant::TakeCall()
{
    return std::exchange(m_call, false);
}

void Giant::SetShown(const glm::vec3& at, float yaw, Mode mode, const glm::vec3& lookAt, float strike, const glm::vec3& strikeAt)
{
    m_shownOnly = true;
    m_shownAt = at;
    m_shownYaw = yaw;
    if (mode == Mode::Dead && m_mode != Mode::Dead)
    {
        m_toppleSide = (m_seed & 1u) != 0u ? 1.0f : -1.0f;
    }
    m_mode = mode;
    m_lookAt = lookAt;
    m_strike = strike;
    m_strikeAt = strikeAt;
}

void Giant::Animate(const GiantWorld& world, float time, float dt)
{
    if (m_shownOnly)
    {
        const glm::vec3 before = m_position;
        m_position += (m_shownAt - m_position) * std::min(dt * 6.0f, 1.0f);
        m_yaw = Wrap(m_yaw + Wrap(m_shownYaw - m_yaw) * std::min(dt * 6.0f, 1.0f));
        m_velocity = dt > 0.0f ? (m_position - before) / dt : glm::vec3(0.0f);
    }
    if (m_kind == GiantKind::Walker)
    {
        AnimateWalker(world, time, dt);
    }
    else
    {
        AnimateSwimmer(world, time, dt);
    }
}

} // namespace pred
