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
    case Mode::Hunt: return "hunt";
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
    case Form::Serpent: return "sea serpent";
    case Form::Eel: return "eel";
    case Form::Armoured: return "armoured serpent";
    case Form::Frilled: return "frilled serpent";
    case Form::Barbed: return "barbed serpent";
    case Form::Paddled: return "paddled sea reptile";
    case Form::Dragon: return "sea dragon";
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
    for (Entity* entity : {&m_skinEntity, &m_glowEntity, &m_eyeEntity, &m_strandEntity})
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
    for (const MeshHandle mesh : {m_skinMesh, m_glowMesh, m_eyeMesh, m_strandMesh})
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
        std::snprintf(line, sizeof(line), "a strider %.0f m tall on %d legs%s; %s, %s%s (%.0f of %.0f health)", m_bodyHeight, static_cast<int>(m_legs.size()),
                      m_strands.empty() ? "" : (", " + std::to_string(m_strands.size()) + " strands hanging").c_str(), ColourFamilyName(m_look.family),
                      SkinPatternName(m_look.pattern), m_glowMesh.IsValid() ? ", glowing" : "", m_health, m_maxHealth);
    }
    else
    {
        std::snprintf(line, sizeof(line), "a leviathan %.0f m long, %s %s; %s, %s%s (%.0f of %.0f health)", m_size, m_form == Form::Armoured || m_form == Form::Eel ? "an" : "a", FormName(m_form), ColourFamilyName(m_look.family),
                      SkinPatternName(m_look.pattern), m_glowMesh.IsValid() ? ", glowing" : "", m_health, m_maxHealth);
    }
    return line;
}

void Giant::MakeVisual(MeshLibrary& meshes, const MeshData& skin, const MeshData* glow, const MeshData* eyes, bool eyesGlow, bool dynamicEyes)
{
    m_eyesGlow = eyesGlow;
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

// --- The strider: a hull on long legs --------------------------------------------------------------------------------------------

namespace
{

// A part sculpted as a distance field and meshed (Sdf, SurfaceNets), painted as a creature of its family is -- darker over the
// back, paler under the belly, its markings over that (SkinMarkingAt) -- and made simpler to about `target` triangles.
MeshData Sculpt(const glm::vec3& lo, const glm::vec3& hi, float cell, const std::function<float(const glm::vec3&)>& field,
                const CreatureAnatomy& look, float markScale, size_t target)
{
    const DistanceGrid grid = SampleDistance(lo, hi, cell, field);
    MeshData mesh = SurfaceNets(grid);
    if (mesh.indices.size() / 3 > target)
    {
        std::vector<uint32_t> kept;
        mesh = SimplifyMesh(mesh, target, cell * 0.05f, kept);
    }
    const auto normalAt = [&](const glm::vec3& p)
    {
        const float h = cell * 0.5f;
        const glm::vec3 n{field(p + glm::vec3(h, 0, 0)) - field(p - glm::vec3(h, 0, 0)), field(p + glm::vec3(0, h, 0)) - field(p - glm::vec3(0, h, 0)),
                          field(p + glm::vec3(0, 0, h)) - field(p - glm::vec3(0, 0, h))};
        const float length = glm::length(n);
        return length > 1e-6f ? n / length : glm::vec3(0.0f, 1.0f, 0.0f);
    };
    for (MeshVertex& vertex : mesh.vertices)
    {
        glm::vec3 p = vertex.position;
        p -= normalAt(p) * field(p);
        vertex.position = p;
        vertex.normal = normalAt(p);
        const float back = glm::smoothstep(-0.4f, 0.5f, vertex.normal.y);
        const glm::vec3 shaded = glm::mix(look.underside, look.skin, back);
        const float mark = SkinMarkingAt(look, p, vertex.normal, markScale);
        vertex.color = SkinColour(glm::mix(shaded, look.markings, mark), 0.9f);
        vertex.uv = {0.0f, 0.0f};
    }
    return mesh;
}

glm::quat AlongY(const glm::vec3& along)
{
    const float length = glm::length(along);
    if (length < 1e-5f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    return glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), along / length);
}

// One part into the whole mesh, every point of it the given bone's.
void AddPart(MeshData& whole, std::vector<SkinWeights>& weights, const MeshData& part, int bone)
{
    const uint32_t first = static_cast<uint32_t>(whole.vertices.size());
    whole.vertices.insert(whole.vertices.end(), part.vertices.begin(), part.vertices.end());
    for (const uint32_t index : part.indices)
    {
        whole.indices.push_back(first + index);
    }
    SkinWeights one;
    one.bone[0] = static_cast<uint8_t>(bone);
    one.weight[0] = 1.0f;
    weights.insert(weights.end(), part.vertices.size(), one);
}

constexpr int kStrandRing = 7;

} // namespace

void Giant::BuildWalker(MeshLibrary& meshes)
{
    SeededRandom random(static_cast<uint64_t>(m_seed) * 2862933555777941757ull + 3037000493ull);
    // How big, and every proportion of it, its own: nothing in steps -- most of them twenty-odd metres, now and then one three times that.
    const float h = 18.0f + 52.0f * std::pow(random.Unit(), 2.2f);
    m_bodyHeight = h;
    m_size = h;
    m_speed = 2.4f + h * 0.045f;
    m_maxHealth = 1500.0f + 90.0f * h;
    m_health = m_maxHealth;
    const glm::vec3 radii{h * random.Range(0.08f, 0.17f), h * random.Range(0.05f, 0.11f), h * random.Range(0.13f, 0.3f)};
    m_bodyRadii = radii;
    const float hump = random.Range(0.0f, 1.0f);
    const float sag = random.Range(0.2f, 1.0f);
    const int segments = random.Unit() < 0.3f ? 2 + static_cast<int>(random.Unit() * 2.0f) : 0; // (a body in sections, now and then)
    const int spines = random.Unit() < 0.45f ? 3 + static_cast<int>(random.Unit() * 8.0f) : 0;
    const float spineLength = radii.y * random.Range(0.4f, 1.3f);
    const bool plated = random.Unit() < 0.4f;
    const uint32_t grain = m_seed * 31u + 7u;
    const float cell = std::max(h * 0.011f, 0.18f);
    // The head's size, known before the hull: the neck it hangs from runs out of the hull's front to it.
    const float hl = h * random.Range(0.07f, 0.12f);
    std::function<float(const glm::vec3&)> hullField;

    // Its legs first: where they join the hull (never at its front, where its head hangs: a leg there came out of its head).
    const float pick = random.Unit();
    const int legCount = pick < 0.28f ? 3 : pick < 0.55f ? 4 : pick < 0.72f ? 5 : pick < 0.88f ? 6 : pick < 0.95f ? 7 : 8;
    const float spread = h * random.Range(0.5f, 0.78f);
    const float thick = h * random.Range(0.016f, 0.034f);
    const float legTurn = random.Range(-0.3f, 0.3f);
    const bool spurred = random.Unit() < 0.5f;
    const bool knobbed = random.Unit() < 0.5f;
    for (int k = 0; k < legCount; ++k)
    {
        Leg leg;
        // Round it, but the front quarter left for its head.
        const float angle = glm::pi<float>() * 0.3f + legTurn + (kTau - glm::pi<float>() * 0.6f) * (static_cast<float>(k) + 0.5f) / static_cast<float>(legCount) +
                            random.Range(-0.08f, 0.08f);
        const glm::vec3 out{std::sin(angle), 0.0f, -std::cos(angle)};
        leg.hip = out * glm::vec3(radii.x * 0.78f, 0.0f, radii.z * 0.72f) + glm::vec3(0.0f, -radii.y * 0.1f, 0.0f);
        const float reach = std::sqrt(h * h + spread * spread);
        leg.upper = reach * random.Range(0.55f, 0.7f);
        leg.lower = reach * 1.25f - leg.upper;
        leg.foot = m_position + Yawed(m_yaw) * (out * spread);
        leg.foot.y = m_position.y;
        leg.from = leg.to = leg.foot;
        m_legs.push_back(leg);
    }

    MeshData whole;
    m_weights.clear();
    // The hull: long or round, a hump or none, a sagging belly, in sections or one, spines down its back or plates over it, and a ball
    // at every hip for its legs to turn in -- lumped as grown things are.
    {
        const auto field = [&](const glm::vec3& p)
        {
            float d = Sdf::Ellipsoid(p, radii);
            if (segments > 0)
            {
                for (int s = 0; s < segments; ++s)
                {
                    const float z = (static_cast<float>(s) - static_cast<float>(segments - 1) * 0.5f) * radii.z * 0.7f;
                    d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, 0.0f, z), radii * glm::vec3(1.08f, 1.05f, 0.45f)), radii.y * 0.15f);
                }
            }
            if (hump > 0.25f)
            {
                d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, radii.y * 0.55f, radii.z * 0.1f), radii * glm::vec3(0.6f, 0.55f * hump + 0.2f, 0.7f)), radii.y * 0.4f);
            }
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, -radii.y * 0.55f * sag, radii.z * 0.2f), radii * glm::vec3(0.55f, 0.5f * sag + 0.1f, 0.55f)),
                                 radii.y * 0.5f);
            for (int k = 0; k < spines; ++k)
            {
                const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(spines) * 1.6f - 0.8f;
                const glm::vec3 base{0.0f, radii.y * 0.75f * std::sqrt(std::max(1.0f - t * t, 0.1f)), t * radii.z};
                // (Never finer at the tip than the hull's grid can hold: finer, each spine came out as broken triangles across its back.)
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, base, base + glm::vec3(0.0f, spineLength, radii.z * 0.25f), std::max(radii.y * 0.14f, cell * 4.0f),
                                                       std::max(radii.y * 0.03f, cell * 2.2f)),
                                     radii.y * 0.1f);
            }
            // The neck: out of the front of the hull and down to where the head hangs, as thick as the head's back.
            d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(0.0f, -radii.y * 0.1f, -radii.z * 0.75f), glm::vec3(0.0f, -radii.y * 0.3f, -(radii.z + hl * 0.35f)),
                                                   std::max(hl * 0.55f, radii.y * 0.45f), hl * 0.38f),
                                 radii.y * 0.25f);
            for (const Leg& leg : m_legs)
            {
                d = Sdf::SmoothUnion(d, glm::length(p - leg.hip) - thick * 2.0f, thick);
            }
            if (plated)
            {
                const float across = p.z / (radii.z * 0.22f);
                const float ridge = std::abs(across - std::floor(across) - 0.5f);
                d -= std::max(p.y / radii.y, 0.0f) * (0.5f - ridge) * radii.y * 0.12f;
            }
            return d + (Sdf::Fbm(p / (radii.y * 0.6f), grain, 3) - 0.5f) * radii.y * 0.18f;
        };
        const glm::vec3 reach = radii * 1.5f + glm::vec3(spineLength + thick * 3.0f + cell * 3.0f + hl);
        AddPart(whole, m_weights, Sculpt(-reach, reach, cell * 1.4f, field, m_look, h * 0.05f, 10000), 0);
        hullField = field;
        // Its lights along its flanks, on the few whose family has them.
        m_glowRest = MeshData{};
        m_glowWeights.clear();
        if (glm::length(m_look.glow) > 0.01f && m_look.photophores > 0)
        {
            std::vector<glm::vec4> beads;
            const int rows = 1 + static_cast<int>(random.Unit() * 3.0f);
            for (int side = -1; side <= 1; side += 2)
            {
                for (int k = 0; k < rows * 5; ++k)
                {
                    const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(rows * 5) * 1.6f - 0.8f;
                    const glm::vec3 inside{0.0f, radii.y * (-0.1f + 0.25f * static_cast<float>(k % rows)), t * radii.z};
                    const glm::vec3 at = OnSkin(field, inside, glm::vec3(static_cast<float>(side), 0.0f, 0.0f), radii.x * 2.0f);
                    beads.push_back({at.x, at.y, at.z, h * 0.007f});
                }
            }
            m_glowRest = Beads(beads);
            SkinWeights one;
            one.weight[0] = 1.0f;
            m_glowWeights.assign(m_glowRest.vertices.size(), one);
        }
    }
    // (Hung out in front of the hull on its neck, not sunk into its front: the head was half inside it.)
    m_neck = glm::vec3(0.0f, -radii.y * 0.3f, -(radii.z + hl * 0.35f));

    // The head (you, 2026-11-16: not a bug's -- no mandibles, no beak, no row of eyes): a skull. A domed braincase behind, a long muzzle
    // out in front of it, heavy brows over two deep sockets with the eyes sunk in them looking forward -- something that looks at you
    // and thinks -- cheekbones under them, a mouth cut the length of the muzzle with teeth down it, slits for nostrils at its end, and
    // on some a ridge down the middle of the skull and horns swept back from behind the brows. Each its own: long or blunt in the
    // muzzle, high or low in the dome, heavy or light in the brow.
    //
    // And not all one skull (you, 2026-11-17: all the giants have the same head, pretty much): five kinds of head, from its seed (not
    // drawn from its own numbers: the rest of it is as it was) -- the long skull; a hammerhead, its eyes out at the ends of lobes either
    // side; a blunt one, short in the face, heavy in the brow and the jaw; a crested one, long and narrow in the muzzle with a crest swept
    // back over its neck; and a shovel, flat and wide, its eyes up on top.
    {
        enum class HeadKind { Long, Hammer, Blunt, Crested, Shovel };
        uint32_t headPick = m_seed * 0x9E3779B1u;
        headPick ^= headPick >> 16;
        const HeadKind kind = static_cast<HeadKind>((headPick * 0x85EBCA6Bu >> 24) % 5u);
        const float drawnSnout = random.Range(0.8f, 1.3f);
        const float snout = kind == HeadKind::Hammer  ? 0.55f + (drawnSnout - 0.8f) * 0.6f
                          : kind == HeadKind::Blunt   ? 0.2f + (drawnSnout - 0.8f) * 0.4f
                          : kind == HeadKind::Crested ? 1.3f + (drawnSnout - 0.8f) * 0.8f
                                                      : drawnSnout; // the muzzle's length, against the head's
        const float dome = random.Range(0.85f, 1.2f) * (kind == HeadKind::Blunt ? 1.15f : kind == HeadKind::Shovel ? 0.75f : 1.0f); // how high the braincase
        const float brow = random.Range(0.8f, 1.25f) * (kind == HeadKind::Blunt ? 1.5f : 1.0f);                                      // how heavy the brows
        const bool ridged = random.Unit() < 0.5f;
        const bool horned = random.Unit() < 0.5f && kind != HeadKind::Hammer && kind != HeadKind::Crested;
        const bool fourEyes = random.Unit() < 0.3f;     // a smaller pair over and behind the first, now and then
        // How wide it is against the usual, and how deep its muzzle.
        const float widen = kind == HeadKind::Blunt ? 1.2f : kind == HeadKind::Crested ? 0.82f : kind == HeadKind::Shovel ? 1.45f : 1.0f;
        const float deep = kind == HeadKind::Blunt ? 1.2f : kind == HeadKind::Crested ? 0.85f : kind == HeadKind::Shovel ? 0.55f : 1.0f;
        const float muzzleEnd = hl * (0.55f + 0.95f * snout);
        const glm::vec3 socket = kind == HeadKind::Hammer ? glm::vec3(hl * 0.66f, hl * 0.12f, -hl * 0.52f)
                               : kind == HeadKind::Shovel ? glm::vec3(hl * 0.2f, hl * 0.2f, -hl * 0.66f)
                                                          : glm::vec3(hl * 0.2f * widen, hl * 0.13f, -hl * 0.62f);
        const float mouthY = -hl * 0.13f;
        // Teeth down both sides of the mouth, over its lip: the top row hanging, a long one near the front.
        const auto teeth = [&](const glm::vec3& p)
        {
            const glm::vec3 mirror{std::abs(p.x), p.y, p.z};
            float d = 1.0e9f;
            for (int k = 0; k < 7; ++k)
            {
                const float z = -hl * 0.48f - (muzzleEnd - hl * 0.55f) * static_cast<float>(k) / 6.0f;
                const float sideX = hl * (0.25f - 0.1f * static_cast<float>(k) / 6.0f) * widen;
                const float longest = k == 5 ? 1.8f : 1.0f;
                d = std::min(d, Sdf::RoundCone(mirror, glm::vec3(sideX, mouthY + hl * 0.03f, z), glm::vec3(sideX * 0.97f, mouthY - hl * 0.05f * longest, z), hl * 0.022f,
                                               hl * 0.004f));
            }
            return d;
        };
        // Its lower jaw, apart (you, 2026-11-17: the giants' jaws too): under the mouth line, from the end of its muzzle back to the end of
        // the mouth -- not its teeth, which hang from the skull over the lip.
        const float hingeZ = -hl * 0.42f;
        const auto jawRegion = [&](const glm::vec3& p) { return std::max(std::max(p.y - mouthY, p.z - hingeZ), -(teeth(p) - hl * 0.01f)); };
        m_jawHinge = glm::vec3(0.0f, mouthY, hingeZ);
        const auto field = [&](const glm::vec3& p)
        {
            const glm::vec3 mirror{std::abs(p.x), p.y, p.z};
            // (The skull itself made in a space squeezed or stretched across, for how wide this one is.)
            const glm::vec3 w{p.x / widen, p.y, p.z};
            const glm::vec3 wMirror{std::abs(w.x), w.y, w.z};
            // The braincase, and the ball at the neck's end it turns on.
            float d = Sdf::Ellipsoid(w - glm::vec3(0.0f, hl * 0.1f, -hl * 0.3f), glm::vec3(hl * 0.4f, hl * 0.36f * dome, hl * 0.46f)) * 0.8f;
            d = Sdf::SmoothUnion(d, glm::length(w) - hl * 0.3f, hl * 0.18f);
            // The muzzle: long, a little deeper than it is wide, narrowing to its end.
            const float along = std::clamp((-p.z - hl * 0.5f) / std::max(muzzleEnd - hl * 0.5f, 0.01f), 0.0f, 1.0f);
            const glm::vec3 muzzleRadii{hl * (0.27f - 0.11f * along), hl * (0.24f - 0.08f * along) * deep, (muzzleEnd - hl * 0.3f) * 0.5f};
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(w - glm::vec3(0.0f, -hl * 0.03f, -(hl * 0.3f + muzzleEnd) * 0.5f), muzzleRadii) * 0.8f, hl * 0.16f);
            // The brows, overhanging the sockets; the cheekbones under them.
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(wMirror - glm::vec3(hl * 0.2f, hl * 0.25f, -hl * 0.6f), glm::vec3(hl * 0.17f, hl * 0.08f * brow, hl * 0.17f)) * 0.8f,
                                 hl * 0.06f);
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(wMirror - glm::vec3(hl * 0.29f, -hl * 0.04f, -hl * 0.45f), glm::vec3(hl * 0.1f, hl * 0.1f, hl * 0.24f)) * 0.8f,
                                 hl * 0.08f);
            d *= std::min(widen, 1.0f);
            // A hammerhead's lobes, out either side, its eyes at their ends.
            if (kind == HeadKind::Hammer)
            {
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(mirror, glm::vec3(hl * 0.15f, hl * 0.12f, -hl * 0.5f), glm::vec3(socket.x, socket.y, socket.z), hl * 0.13f, hl * 0.12f),
                                     hl * 0.1f);
                d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(mirror - glm::vec3(socket.x, socket.y + hl * 0.05f, socket.z), glm::vec3(hl * 0.14f, hl * 0.12f, hl * 0.15f)) * 0.8f,
                                     hl * 0.05f);
            }
            // A crested one's crest: a blade of bone swept back from its brow up over the back of its head.
            if (kind == HeadKind::Crested)
            {
                const glm::vec3 thin{p.x * 4.0f, p.y, p.z};
                d = Sdf::SmoothUnion(d, 0.25f * Sdf::RoundCone(thin, glm::vec3(0.0f, hl * 0.3f * dome, -hl * 0.55f), glm::vec3(0.0f, hl * 0.95f, hl * 0.95f), hl * 0.16f, hl * 0.05f),
                                     hl * 0.08f);
            }
            // A blunt one's heavy jaw: deeper under its mouth, out at the sides.
            if (kind == HeadKind::Blunt)
            {
                d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(mirror - glm::vec3(hl * 0.2f, mouthY - hl * 0.1f, -(hl * 0.25f + muzzleEnd) * 0.5f), glm::vec3(hl * 0.16f, hl * 0.13f, (muzzleEnd - hl * 0.2f) * 0.5f)) * 0.8f,
                                     hl * 0.1f);
            }
            // A ridge down the middle of the skull; horns swept back and out from behind the brows.
            if (ridged)
            {
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(0.0f, hl * 0.3f * dome, -hl * 0.55f), glm::vec3(0.0f, hl * 0.42f * dome, hl * 0.1f), hl * 0.06f,
                                                       hl * 0.05f),
                                     hl * 0.08f);
            }
            if (horned)
            {
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(mirror, glm::vec3(hl * 0.27f, hl * 0.3f, -hl * 0.35f), glm::vec3(hl * 0.42f, hl * 0.55f, hl * 0.2f), hl * 0.09f,
                                                       hl * 0.05f),
                                     hl * 0.06f);
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(mirror, glm::vec3(hl * 0.42f, hl * 0.55f, hl * 0.2f), glm::vec3(hl * 0.5f, hl * 0.5f, hl * 0.75f), hl * 0.05f,
                                                       hl * 0.025f),
                                     hl * 0.04f);
            }
            // The sockets, deep under the brows.
            d = Sdf::SmoothCut(d, glm::length(mirror - socket) - hl * 0.1f, hl * 0.03f);
            if (fourEyes)
            {
                d = Sdf::SmoothCut(d, glm::length(mirror - (socket + glm::vec3(hl * 0.04f, hl * 0.12f, hl * 0.12f))) - hl * 0.055f, hl * 0.02f);
            }
            // The mouth: cut the length of the muzzle along each side, from its end back under the cheek.
            const float slot = std::max(std::abs(p.y - mouthY) - hl * 0.018f, -p.z - muzzleEnd + hl * 0.02f);
            d = Sdf::SmoothCut(d, std::max(slot, p.z + hl * 0.42f), hl * 0.01f);
            // Nostrils: two slits on top near its end.
            d = Sdf::SmoothCut(d, Sdf::Ellipsoid(mirror - glm::vec3(hl * 0.07f * widen, hl * 0.08f * deep, -(muzzleEnd - hl * 0.12f)), glm::vec3(hl * 0.025f, hl * 0.04f, hl * 0.07f)) * 0.8f,
                               hl * 0.01f);
            d = std::min(d, teeth(p));
            return d + (Sdf::Fbm(p / (hl * 0.22f), grain + 5u, 2) - 0.5f) * hl * 0.04f;
        };
        const glm::vec3 reach{hl * (kind == HeadKind::Hammer ? 1.0f : 0.9f * std::max(widen, 1.0f)), hl * 1.2f, std::max(muzzleEnd + hl * 0.3f, hl * 1.3f)};
        // The skull on the head's bone; the jaw on its own (the bone after the legs'), a little into the skull where they meet, so open
        // it runs into it rather than hanging off a cut.
        AddPart(whole, m_weights, Sculpt(-reach, reach, std::max(hl * 0.03f, 0.05f), [&](const glm::vec3& p) { return std::max(field(p), -jawRegion(p)); }, m_look,
                                         hl * 0.3f, 9000),
                1);
        m_jawBone = 2 + 2 * static_cast<int>(m_legs.size());
        const float overlap = hl * 0.03f;
        AddPart(whole, m_weights,
                Sculpt(-reach, glm::vec3(reach.x, mouthY + overlap + hl * 0.06f, hingeZ + overlap + hl * 0.06f), std::max(hl * 0.03f, 0.05f),
                       [&](const glm::vec3& p) { return std::max(field(p), jawRegion(p) - overlap); }, m_look, hl * 0.3f, 4000),
                m_jawBone);
        // Its eyes, sunk in the sockets and looking out forward; a smaller pair over and behind on some.
        std::vector<glm::vec4> eyes;
        for (const float side : {-1.0f, 1.0f})
        {
            eyes.push_back({side * socket.x, socket.y, socket.z - hl * 0.035f, hl * 0.068f});
            if (fourEyes)
            {
                eyes.push_back({side * (socket.x + hl * 0.04f), socket.y + hl * 0.12f, socket.z + hl * 0.12f - hl * 0.02f, hl * 0.038f});
            }
        }
        m_eyeRest = Beads(eyes);
        SkinWeights one;
        one.bone[0] = 1;
        one.weight[0] = 1.0f;
        m_eyeWeights.assign(m_eyeRest.vertices.size(), one);
    }

    // The legs: each a long thigh up to a knee well over the hull and a shin down to a point -- knobbed at the knee or not, spurred or
    // not -- its knee a ball the shin turns on.
    for (size_t k = 0; k < m_legs.size(); ++k)
    {
        const Leg& leg = m_legs[k];
        const uint32_t legGrain = grain + 100u * static_cast<uint32_t>(k + 1);
        const float upper = leg.upper;
        const float lower = leg.lower;
        const auto thigh = [&](const glm::vec3& p)
        {
            float d = Sdf::RoundCone(p, glm::vec3(0.0f), glm::vec3(0.0f, upper, 0.0f), thick * 1.3f, thick);
            d = Sdf::SmoothUnion(d, glm::length(p - glm::vec3(0.0f, upper, 0.0f)) - thick * (knobbed ? 1.6f : 1.2f), thick * 0.5f);
            return d + (Sdf::Fbm(p / (thick * 1.5f), legGrain, 2) - 0.5f) * thick * 0.25f;
        };
        const auto shin = [&](const glm::vec3& p)
        {
            float d = Sdf::RoundCone(p, glm::vec3(0.0f), glm::vec3(0.0f, lower, 0.0f), thick, thick * 0.18f);
            if (spurred)
            {
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(0.0f, lower * 0.33f, 0.0f), glm::vec3(thick * 1.8f, lower * 0.3f, 0.0f), thick * 0.4f, thick * 0.08f),
                                     thick * 0.3f);
            }
            return d + (Sdf::Fbm(p / (thick * 1.2f), legGrain + 3u, 2) - 0.5f) * thick * 0.2f;
        };
        const float legCell = std::max(thick * 0.28f, 0.08f);
        AddPart(whole, m_weights, Sculpt(glm::vec3(-thick * 2.2f), glm::vec3(thick * 2.2f, upper + thick * 2.2f, thick * 2.2f), legCell, thigh, m_look, h * 0.04f, 2500),
                2 + 2 * static_cast<int>(k));
        AddPart(whole, m_weights, Sculpt(glm::vec3(-thick * 2.0f), glm::vec3(thick * 3.0f, lower + thick * 2.0f, thick * 2.0f), legCell, shin, m_look, h * 0.04f, 2500),
                3 + 2 * static_cast<int>(k));
    }
    m_rest = whole;
    m_skinning.assign(static_cast<size_t>(2 + 2 * m_legs.size() + 1), glm::mat4(1.0f)); // (and its jaw's)

    // The strands hanging from its belly -- on some of them only -- each its own: long or short, thick or thin, stiff or limp.
    if (random.Unit() < 0.45f)
    {
        const int count = 2 + static_cast<int>(random.Unit() * 6.0f);
        for (int k = 0; k < count; ++k)
        {
            Strand strand;
            const float a = random.Range(0.0f, kTau);
            // Grown out of its underside -- on the skin, a little into it, so it joins it -- and long enough to reach the ground and take
            // somebody off it (you, 2026-11-16).
            const glm::vec3 inside{std::sin(a) * radii.x * 0.4f, -radii.y * 0.2f, std::cos(a) * radii.z * 0.5f};
            strand.thick = h * random.Range(0.008f, 0.016f);
            strand.root = OnSkin(hullField, inside, glm::vec3(0.0f, -1.0f, 0.0f), radii.y * 3.0f) + glm::vec3(0.0f, strand.thick * 0.6f, 0.0f);
            const int links = 10 + static_cast<int>(random.Unit() * 8.0f);
            const float total = h * random.Range(0.85f, 1.05f);
            strand.length = total / static_cast<float>(links);
            strand.stiffness = random.Range(0.1f, 0.6f);
            strand.phase = random.Range(0.0f, kTau);
            const glm::vec3 start = m_position + glm::vec3(0.0f, h, 0.0f) + Yawed(m_yaw) * strand.root;
            for (int i = 0; i <= links; ++i)
            {
                strand.points.push_back(start - glm::vec3(0.0f, strand.length * static_cast<float>(i), 0.0f));
            }
            strand.last = strand.points;
            m_strands.push_back(std::move(strand));
        }
    }
    MakeVisual(meshes, m_rest, m_glowRest.indices.empty() ? nullptr : &m_glowRest, &m_eyeRest, true, true);
    // The strands: one mesh of tubes, rings round each link, made again every frame from where their links are.
    if (!m_strands.empty())
    {
        MeshData tubes;
        for (const Strand& strand : m_strands)
        {
            const uint32_t first = static_cast<uint32_t>(tubes.vertices.size());
            const uint32_t rings = static_cast<uint32_t>(strand.points.size());
            for (uint32_t i = 0; i < rings * kStrandRing; ++i)
            {
                MeshVertex vertex;
                const float t = static_cast<float>(i / kStrandRing) / static_cast<float>(rings);
                vertex.color = SkinColour(glm::mix(m_look.skin, m_look.underside, t), 0.9f);
                tubes.vertices.push_back(vertex);
            }
            for (uint32_t r = 0; r + 1 < rings; ++r)
            {
                for (uint32_t j = 0; j < kStrandRing; ++j)
                {
                    const uint32_t a = first + r * kStrandRing + j;
                    const uint32_t b = first + r * kStrandRing + (j + 1) % kStrandRing;
                    const uint32_t c = a + kStrandRing;
                    const uint32_t d = b + kStrandRing;
                    // (Wound to face out: the other way round, every face looked inwards, and with the near side of the tube not drawn
                    // what showed was the inside of its far side -- the strands looked inside out: you, 2026-11-17.)
                    tubes.indices.insert(tubes.indices.end(), {a, b, c, b, d, c});
                }
            }
        }
        m_strandShape = tubes;
        const std::string name = "giant_" + std::to_string(m_seed) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "_strands";
        m_strandMesh = meshes.CreateDynamic(tubes, name);
        Material flesh = Material::Diffuse(glm::vec3(1.0f), m_look.roughness);
        flesh.weathering = 0.0f;
        m_strandEntity = m_scene.CreateMeshEntity(name, Transform{}, m_strandMesh, flesh);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_strandEntity))
        {
            renderer->blocksSky = false;
            renderer->farVisible = true;
        }
    }

    // What rounds find: its hull, its head, and each shin.
    const auto box = [&](int bone, float halfLength, float radius, bool head)
    {
        BoneBox b;
        b.bone = bone;
        b.halfLength = halfLength;
        b.head = head;
        b.body = m_physics.CreateCapsule(std::max(halfLength, 0.05f), std::max(radius, 0.2f), Transform{}, BodyMotion::Kinematic, 1000.0f, PhysicsLayer::Hitbox);
        if (b.body.IsValid())
        {
            m_boxes.push_back(b);
        }
    };
    box(0, radii.z * 0.7f, radii.y, false);
    box(1, h * 0.05f, h * 0.045f, true);
    for (size_t k = 0; k < m_legs.size(); ++k)
    {
        box(3 + 2 * static_cast<int>(k), m_legs[k].lower * 0.5f, thick * 1.2f, false);
    }
    m_bodyY = m_position.y + h;
}

glm::vec3 Giant::FootFor(const GiantWorld& world, size_t index, const glm::vec3& ahead) const
{
    // Where it would put this foot: out from its hip, on the ground, a step ahead of where it is going -- and never on a roof.
    const Leg& leg = m_legs[index];
    const glm::vec3 out = glm::normalize(glm::vec3(leg.hip.x, 0.0f, leg.hip.z) + glm::vec3(1e-4f));
    const float spread = m_bodyHeight * 0.62f;
    // The best of a few places about there (you, 2026-11-16: its feet came down badly): never on or in anything built, and on the
    // flattest ground near where it would go -- turned a little round the hip or in or out a little, as a foot finds its footing.
    if (world.ground)
    {
        float best = -1.0e9f;
        glm::vec3 found{0.0f};
        for (const float turn : {0.0f, 0.18f, -0.18f, 0.36f, -0.36f})
        {
            for (const float far : {1.0f, 0.86f, 1.14f})
            {
                const glm::vec3 way = glm::angleAxis(turn, glm::vec3(0.0f, 1.0f, 0.0f)) * out;
                glm::vec3 at = m_position + Yawed(m_yaw) * (way * (spread * far)) + ahead;
                // On a roof (you, 2026-11-16: walking over buildings, up onto them): well in from its edges, the foot on top of it.
                // Anywhere else built -- by a wall, on the margin round a building, on a tank -- no.
                float footY = world.ground(at.x, at.z);
                bool onRoof = false;
                if (world.roof)
                {
                    const float top = world.roof(at.x, at.z);
                    constexpr float kIn = 2.5f;
                    onRoof = top > footY + 1.0f && world.roof(at.x + kIn, at.z) >= top - 0.5f && world.roof(at.x - kIn, at.z) >= top - 0.5f &&
                             world.roof(at.x, at.z + kIn) >= top - 0.5f && world.roof(at.x, at.z - kIn) >= top - 0.5f;
                    if (onRoof)
                    {
                        footY = top;
                    }
                }
                if (!onRoof && world.builtUp && world.builtUp(at.x, at.z) > 0.0f)
                {
                    continue;
                }
                // (Nor its leg down through a building's wall: from the foot back up towards its knee, the leg higher than what is built.)
                if (world.builtUp)
                {
                    bool through = false;
                    for (const float u : {0.08f, 0.18f, 0.3f})
                    {
                        const glm::vec3 up = glm::mix(at, m_position, u);
                        // (Measured from the ground there: the leg's height over it, from a foot on the ground or on a roof.)
                        const float legOver = footY - world.ground(up.x, up.z) + m_bodyHeight * 0.85f * (u / 0.35f);
                        through = through || world.builtUp(up.x, up.z) > legOver - 1.5f;
                    }
                    if (through)
                    {
                        continue;
                    }
                }
                at.y = footY;
                const float slope = onRoof ? 0.0f
                                           : std::abs(world.ground(at.x + 2.0f, at.z) - world.ground(at.x - 2.0f, at.z)) +
                                                 std::abs(world.ground(at.x, at.z + 2.0f) - world.ground(at.x, at.z - 2.0f));
                const float score = -std::abs(turn) * 4.0f - std::abs(far - 1.0f) * 6.0f - slope * 0.6f - (onRoof ? 0.5f : 0.0f);
                if (score > best && (!world.roofed || !world.roofed(at + glm::vec3(0.0f, 1.0f, 0.0f))))
                {
                    best = score;
                    found = at;
                }
            }
        }
        if (best > -1.0e8f)
        {
            return found;
        }
    }
    glm::vec3 foot = m_position + Yawed(m_yaw) * (out * spread) + ahead;
    for (int tries = 0; tries < 6; ++tries)
    {
        foot.y = world.ground ? world.ground(foot.x, foot.z) : m_position.y;
        if (!world.roofed || !world.roofed(foot + glm::vec3(0.0f, 1.0f, 0.0f)))
        {
            break;
        }
        foot += Yawed(m_yaw) * (out * 4.0f);
    }
    return foot;
}

void Giant::AnimateWalker(const GiantWorld& world, float time, float dt)
{
    const glm::quat turn = Yawed(m_yaw);
    const bool dead = m_mode == Mode::Dead;
    const float speed = dead || m_frozen ? 0.0f : glm::length(glm::vec2(m_velocity.x, m_velocity.z));
    const float stride = m_bodyHeight * 0.32f;
    // Quicker steps the faster it goes, so its feet keep up with it -- never so quick a thing that size looks light.
    const float stepTime = std::clamp(stride / std::max(speed, 0.5f) * 0.55f, 0.55f, 1.3f) * std::sqrt(m_bodyHeight / 25.0f);
    const glm::vec3 ahead = glm::vec3(m_velocity.x, 0.0f, m_velocity.z) * (stepTime * 1.1f);
    // The leg it reaches down with, striking: the nearest to where it reaches.
    int reaching = -1;
    if (m_mode == Mode::Strike && m_strands.empty())
    {
        float nearest = 1.0e9f;
        for (size_t k = 0; k < m_legs.size(); ++k)
        {
            const float d = Flat(m_legs[k].foot, m_strikeAt);
            if (d < nearest)
            {
                nearest = d;
                reaching = static_cast<int>(k);
            }
        }
    }
    // Its feet: about half at a time, never two side by side, each lifted when it has fallen behind where it would go, swung over
    // and put down -- heard, felt, its weight coming down on it. Dead or standing still, none.
    int stepping = 0;
    for (const Leg& leg : m_legs)
    {
        stepping += leg.step >= 0.0f ? 1 : 0;
    }
    const int atOnce = std::max(1, static_cast<int>(m_legs.size()) / 2);
    const size_t count = m_legs.size();
    for (size_t k = 0; k < count && !dead; ++k)
    {
        Leg& leg = m_legs[k];
        if (static_cast<int>(k) == reaching)
        {
            continue;
        }
        if (leg.step >= 0.0f)
        {
            leg.step += dt / std::max(leg.duration, 0.1f);
            const float s = std::min(leg.step, 1.0f);
            const float eased = s * s * (3.0f - 2.0f * s);
            leg.foot = glm::mix(leg.from, leg.to, eased) + glm::vec3(0.0f, std::sin(s * glm::pi<float>()) * std::max(leg.arc, m_bodyHeight * 0.1f), 0.0f);
            if (leg.step >= 1.0f)
            {
                leg.step = -1.0f;
                leg.foot = leg.to;
                if (m_footfalls.size() < 16)
                {
                    m_footfalls.push_back(leg.foot);
                }
                m_dipSpeed -= m_bodyHeight * 0.05f;
            }
            continue;
        }
        const glm::vec3 wanted = FootFor(world, k, ahead);
        const float behind = Flat(leg.foot, wanted);
        // Holding still, it holds still on its feet set under it: a foot left trailing as it stopped is brought in first (you,
        // 2026-11-17: the giants sometimes have a leg lagging behind -- frozen mid-stride, it was held out behind it as long as it was
        // looked at).
        if (m_frozen && behind < stride * 0.45f)
        {
            continue;
        }
        const bool neighbourUp = m_legs[(k + 1) % count].step >= 0.0f || m_legs[(k + count - 1) % count].step >= 0.0f;
        const bool due = behind > (speed > 0.3f ? stride * 0.7f : stride * 0.35f);
        // (Not let fall more than a little over a stride behind before it is taken, waiting its turn or not: at a stride and a half a
        // leg could trail half the giant's height behind it.)
        const bool overdue = behind > stride * 1.1f;
        if ((due && stepping < atOnce && !neighbourUp) || overdue)
        {
            leg.from = leg.foot;
            leg.to = wanted;
            leg.step = 0.0f;
            // (Lifted clear over anything built that the foot swings across, not through it.)
            leg.arc = 0.0f;
            if (world.builtUp)
            {
                for (const float u : {0.25f, 0.5f, 0.75f})
                {
                    const glm::vec3 p = glm::mix(leg.from, leg.to, u);
                    const float built = world.builtUp(p.x, p.z);
                    if (built > 0.0f)
                    {
                        const float under = glm::mix(leg.from.y, leg.to.y, u);
                        leg.arc = std::max(leg.arc, built - under + 3.0f);
                    }
                }
            }
            // (As long as the leg has far to go: up over a building or onto its roof, a step the usual time whipped the leg up and down
            // -- you, 2026-11-16: its limbs moved really fast on top of a building.)
            leg.duration = stepTime * (1.0f + (std::max(leg.arc - m_bodyHeight * 0.1f, 0.0f) + std::abs(leg.to.y - leg.from.y)) / std::max(m_bodyHeight * 0.2f, 1.0f));
            ++stepping;
        }
    }
    if (reaching >= 0)
    {
        Leg& leg = m_legs[static_cast<size_t>(reaching)];
        const float windup = 1.9f / 2.8f;
        const glm::vec3 ground = glm::vec3(m_strikeAt.x, world.ground ? world.ground(m_strikeAt.x, m_strikeAt.z) : m_strikeAt.y, m_strikeAt.z);
        // (Raised, held -- seen coming -- and brought down by them.)
        if (m_strike < windup)
        {
            leg.foot = glm::mix(leg.foot, ground + glm::vec3(0.0f, m_bodyHeight * 0.4f, 0.0f) + (ground - m_position) * 0.1f, std::min(dt * 2.0f, 1.0f));
        }
        else
        {
            leg.foot = glm::mix(leg.foot, ground + glm::normalize(glm::vec3(ground.x - m_position.x, 0.0f, ground.z - m_position.z) + glm::vec3(1e-4f)) * 3.5f,
                                std::min(dt * 10.0f, 1.0f));
        }
        leg.from = leg.to = leg.foot;
    }

    // Its weight: sunk a little under each foot put down and springing back; leaning off the feet it has up; its front lowered to
    // what it hunts or holds -- and its height eased, not jumping when a foot comes down on something higher.
    m_dipSpeed += (-m_dip * 30.0f - m_dipSpeed * 7.0f) * dt;
    m_dip += m_dipSpeed * dt;
    glm::vec2 leanWanted{0.0f};
    float feetY = 0.0f;
    for (const Leg& leg : m_legs)
    {
        feetY += leg.step < 0.0f ? leg.foot.y : leg.to.y;
        if (leg.step >= 0.0f)
        {
            const glm::vec3 out = glm::normalize(glm::vec3(leg.hip.x, 0.0f, leg.hip.z) + glm::vec3(1e-4f));
            leanWanted += glm::vec2(out.z, -out.x) * 0.04f;
        }
    }
    m_lean += (leanWanted - m_lean) * std::min(dt * 2.0f, 1.0f);
    feetY /= static_cast<float>(std::max<size_t>(m_legs.size(), 1));
    m_bob += dt * speed / std::max(stride, 0.5f) * glm::pi<float>();
    const bool low = m_mode == Mode::Hunt || m_mode == Mode::Strike || m_mode == Mode::Hold;
    // Bent right down to look at somebody close, or in at a window where they went (you, 2026-11-16): its body lowered on its legs, its
    // front down, its head brought down to them.
    m_peek += (m_peekWanted - m_peek) * std::min(dt * 0.6f, 1.0f);
    const float crouch = std::max(low ? 0.16f : 0.0f, 0.5f * m_peek);
    float wantY = feetY + m_bodyHeight * (1.0f - crouch);
    // Over a building, its belly kept clear of the roof under it -- up over it as it walks across (you, 2026-11-16), bending down or not.
    if (world.roof)
    {
        float under = -1.0e9f;
        for (const glm::vec3& at : {glm::vec3(0.0f), glm::vec3(m_bodyRadii.x, 0.0f, 0.0f), glm::vec3(-m_bodyRadii.x, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, m_bodyRadii.z),
                                    glm::vec3(0.0f, 0.0f, -m_bodyRadii.z)})
        {
            const glm::vec3 p = m_position + turn * at;
            under = std::max(under, world.roof(p.x, p.z));
        }
        if (under > -1.0e8f)
        {
            wantY = std::max(wantY, under + m_bodyRadii.y * 1.3f + 2.0f);
        }
    }
    if (dead)
    {
        // Dead, it is a body (you, 2026-11-16: ragdoll): its legs give under it, its feet sliding out, and it comes down under its own
        // weight onto the ground, settling there.
        // (Lying with its head clear of the ground: lowered to the bottom of its hull, the head hung under it went into the ground.)
        // (Over a building, down onto its roof, not through it.)
        const float roofUnder = world.roof ? world.roof(m_position.x, m_position.z) : -1.0e9f;
        const float floor = std::max(world.ground ? world.ground(m_position.x, m_position.z) : m_position.y, roofUnder) + m_bodyRadii.y * 1.15f;
        if (!m_landed)
        {
            m_fallSpeed += 9.8f * 0.45f * dt;
            m_bodyY = std::max(m_bodyY - m_fallSpeed * dt, floor);
            for (Leg& leg : m_legs)
            {
                const glm::vec3 out = glm::normalize(glm::vec3(leg.foot.x - m_position.x, 0.0f, leg.foot.z - m_position.z) + glm::vec3(1e-4f));
                if (Flat(leg.foot, m_position) < m_bodyHeight * 0.95f)
                {
                    // (As fast as it is big: a tall one's knees were left standing high over it as it lay.)
                    leg.foot += out * std::min(m_fallSpeed, 6.0f) * 0.6f * std::max(1.0f, m_bodyHeight / 22.0f) * dt;
                    leg.foot.y = world.ground ? world.ground(leg.foot.x, leg.foot.z) : leg.foot.y;
                }
            }
            // Down: the ground taking its weight once -- heard and felt -- and then it lies still. (It bounced on and on, each bounce a
            // footfall shaking the picture, and its feet slid without end.)
            if (m_bodyY <= floor + 1.0e-3f)
            {
                m_landed = true;
                m_fallSpeed = 0.0f;
                if (m_footfalls.size() < 16)
                {
                    m_footfalls.push_back(glm::vec3(m_position.x, floor, m_position.z));
                }
            }
        }
    }
    else
    {
        m_fallSpeed = 0.0f;
        m_bodyY += (wantY - m_bodyY) * std::min(dt * 1.5f, 1.0f);
    }
    glm::vec3 body = glm::vec3(m_position.x, m_bodyY + m_dip, m_position.z);
    const float pitch = std::max(low ? 0.16f : 0.0f, 0.35f * m_peek) + m_lean.x;
    glm::quat bodyTurn = turn * glm::angleAxis(-pitch, glm::vec3(1.0f, 0.0f, 0.0f)) * glm::angleAxis(m_lean.y, glm::vec3(0.0f, 0.0f, 1.0f));

    // Dead: its legs give, and it comes down and over onto its side.
    glm::mat4 fall(1.0f);
    if (dead)
    {
        const float was = m_topple;
        m_topple = std::min(m_topple + dt / 3.0f, 1.0f);
        const float down = m_topple * m_topple;
        const glm::vec3 pivot{m_position.x, world.ground ? world.ground(m_position.x, m_position.z) : m_position.y, m_position.z};
        const glm::vec3 facing = turn * glm::vec3(0.0f, 0.0f, -1.0f);
        // (Down on its belly, slumped a little to one side: not thrown over onto its back, its legs in the air.)
        fall = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(glm::angleAxis(down * 0.3f * m_toppleSide, facing)) * glm::translate(glm::mat4(1.0f), -pivot);
        if (was < 1.0f && m_topple >= 1.0f && m_footfalls.size() < 16)
        {
            m_footfalls.push_back(pivot);
        }
    }
    const auto part = [&](const glm::vec3& at, const glm::quat& rotation) { return fall * glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(rotation); };
    m_skinning[0] = part(body, bodyTurn);
    // Breathing: its body swelling and easing, slow, deeper as it hunts (still, dead).
    if (!dead)
    {
        const float deep = m_mode == Mode::Hunt || m_mode == Mode::Strike || m_mode == Mode::Hold ? 0.03f : m_frozen ? 0.0f : 0.015f;
        const float swell = 1.0f + deep * std::sin(time * (m_mode == Mode::Hunt ? 0.9f : 0.45f) + static_cast<float>(m_seed % 17u));
        m_skinning[0] = m_skinning[0] * glm::scale(glm::mat4(1.0f), glm::vec3(swell, 1.0f + (swell - 1.0f) * 0.7f, 1.0f));
    }

    // Its legs, from their hips to their feet, bent at the knee up and out.
    for (size_t k = 0; k < m_legs.size(); ++k)
    {
        const Leg& leg = m_legs[k];
        const glm::vec3 hip = body + bodyTurn * leg.hip;
        glm::vec3 toFoot = leg.foot - hip;
        float d = glm::length(toFoot);
        const float most = (leg.upper + leg.lower) * 0.98f;
        if (d > most)
        {
            toFoot *= most / d;
            d = most;
        }
        const glm::vec3 along = d > 1e-4f ? toFoot / d : glm::vec3(0.0f, -1.0f, 0.0f);
        const float a = leg.upper;
        const float b = leg.lower;
        const float x = (a * a - b * b + d * d) / (2.0f * std::max(d, 1e-3f));
        const float rise = std::sqrt(std::max(a * a - x * x, 0.0f));
        const glm::vec3 out = glm::normalize(glm::vec3(hip.x - body.x, 0.0f, hip.z - body.z) + glm::vec3(1e-4f));
        glm::vec3 bend = glm::vec3(0.0f, 1.0f, 0.0f) + out * 0.5f;
        bend = glm::normalize(bend - along * glm::dot(bend, along));
        const glm::vec3 knee = hip + along * x + bend * rise;
        m_skinning[2 + 2 * k] = part(hip, AlongY(knee - hip));
        m_skinning[3 + 2 * k] = part(knee, AlongY(hip + toFoot - knee));
    }

    // Its head, turned (and lowered) towards what it looks at, slowly.
    const glm::vec3 neck = body + bodyTurn * m_neck;
    glm::vec3 look = m_lookAt - neck;
    if (glm::length(look) < 0.5f)
    {
        look = bodyTurn * glm::vec3(0.0f, 0.0f, -1.0f);
    }
    // (Never turned round further than its neck lets it -- about it, up and down -- or it turned back into its own body.)
    {
        const glm::vec3 local = glm::inverse(bodyTurn) * glm::normalize(look);
        float yaw = std::atan2(local.x, -local.z);
        float lift = std::asin(std::clamp(local.y, -1.0f, 1.0f));
        yaw = std::clamp(yaw, -1.2f, 1.2f);
        lift = std::clamp(lift, -1.2f, 0.5f);
        look = bodyTurn * glm::vec3(std::sin(yaw) * std::cos(lift), std::sin(lift), -std::cos(yaw) * std::cos(lift));
    }
    if (dead)
    {
        look = bodyTurn * glm::normalize(glm::vec3(0.0f, -0.12f, -1.0f));
    }
    const glm::quat wanted = glm::quatLookAtRH(glm::normalize(look), glm::vec3(0.0f, 1.0f, 0.0f));
    if (!dead || m_topple < 1.0f)
    {
        m_headTurn = glm::slerp(m_headTurn, wanted, std::min(dt * 1.4f, 1.0f));
    }
    m_skinning[1] = part(neck, m_headTurn);
    // Its jaw: hanging a little open as it breathes, working as it hunts, wide as it strikes, chewing what it eats, slack dead.
    if (m_jawBone > 0 && m_jawBone < static_cast<int>(m_skinning.size()))
    {
        float open = 0.04f + 0.03f * std::sin(time * 0.5f + static_cast<float>(m_seed % 13u));
        if (dead)
        {
            open = 0.35f;
        }
        else if (m_mode == Mode::Strike)
        {
            open = m_strike < 0.6f ? 0.65f : 0.15f;
        }
        else if (m_mode == Mode::Hold)
        {
            open = m_eats ? 0.45f + 0.4f * std::sin(time * 5.0f) : 0.25f;
        }
        else if (m_mode == Mode::Hunt || m_mode == Mode::Approach)
        {
            open = 0.18f + 0.1f * std::sin(time * 1.2f + static_cast<float>(m_seed % 7u));
        }
        else if (m_mode == Mode::Notice || m_frozen)
        {
            open = 0.02f; // (shut: holding still)
        }
        m_gape += (open - m_gape) * std::min(dt * 3.0f, 1.0f);
        m_skinning[static_cast<size_t>(m_jawBone)] = m_skinning[1] * glm::translate(glm::mat4(1.0f), m_jawHinge) *
                                                      glm::rotate(glm::mat4(1.0f), -std::max(m_gape, 0.0f) * 0.7f, glm::vec3(1.0f, 0.0f, 0.0f)) *
                                                      glm::translate(glm::mat4(1.0f), -m_jawHinge);
    }
    m_head = glm::vec3(fall * glm::vec4(neck + m_headTurn * glm::vec3(0.0f, 0.0f, -m_bodyHeight * 0.06f), 1.0f));

    // Holding somebody: up from where it took them to just under its head, turned over there -- eased, held steady.
    m_holdWanted = m_head + m_headTurn * glm::vec3(0.0f, -m_bodyHeight * 0.05f, -m_bodyHeight * 0.05f) - glm::vec3(0.0f, 2.2f, 0.0f);
    // Throwing it (Think): drawn back and up beside it first, then swung round and forward, down to a height it can be thrown from
    // -- low enough that the throw, not the fall, is what hurts most.
    if (m_lowering)
    {
        const float ground = world.ground ? world.ground(m_position.x, m_position.z) : m_position.y;
        const float drawn = m_throwSince >= 0.0f ? time - m_throwSince : 0.0f;
        const glm::vec3 local = drawn < kThrowWindup ? glm::vec3(m_bodyHeight * 0.3f, 0.0f, m_bodyHeight * 0.1f)
                                                     : glm::vec3(-m_bodyHeight * 0.1f, 0.0f, -m_bodyHeight * 0.4f);
        const glm::vec3 at = m_position + Yawed(m_yaw) * local;
        const float high = drawn < kThrowWindup ? std::min(m_bodyHeight * 0.7f, 26.0f) : std::min(m_bodyHeight * 0.25f, 7.0f);
        m_holdWanted = glm::vec3(at.x, ground + high, at.z);
    }
    // (Taken from just inside a building: drawn out through the doorway, the way the strand came in, before it is lifted.)
    if (m_mode == Mode::Hold && time < m_pullOutUntil)
    {
        // (Out the way they came in: from where it took them, through the doorway and on -- not towards wherever it stands, which
        // could be across the building.)
        const glm::vec3 out = glm::normalize(glm::vec3(m_reachDoor.x - m_reachInside.x, 0.0f, m_reachDoor.z - m_reachInside.z) + glm::vec3(1e-4f, 0.0f, 0.0f));
        m_holdWanted = m_reachDoor + out * 2.5f + glm::vec3(0.0f, 1.0f, 0.0f);
    }
    if (m_mode == Mode::Hold)
    {
        const bool flinging = m_lowering && m_throwSince >= 0.0f && time - m_throwSince >= kThrowWindup;
        m_holdPoint += (m_holdWanted - m_holdPoint) * std::min(dt * (flinging ? 7.0f : time < m_pullOutUntil ? 3.0f : m_lowering ? 2.2f : 0.9f), 1.0f);
    }
    else if (m_holding < 0)
    {
        m_holdPoint = m_strikeAt;
    }

    // The one mesh, posed; drawn about where it is.
    const glm::vec3 origin = m_position;
    const auto draw = [&](MeshHandle mesh, Entity entity, const MeshData& rest, const std::vector<SkinWeights>& weights, std::vector<MeshVertex>& out)
    {
        if (!mesh.IsValid() || rest.vertices.empty())
        {
            return;
        }
        AABB bounds;
        SkinMesh(rest, weights, m_skinning, out, bounds);
        for (MeshVertex& vertex : out)
        {
            vertex.position -= origin;
        }
        bounds.min -= origin;
        bounds.max -= origin;
        m_meshes.UpdateDynamic(mesh, out, bounds);
        if (Transform* transform = entity.IsValid() ? m_scene.GetTransform(entity) : nullptr)
        {
            transform->position = origin;
            transform->rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            transform->scale = glm::vec3(1.0f);
        }
    };
    draw(m_skinMesh, m_skinEntity, m_rest, m_weights, m_skinned);
    draw(m_glowMesh, m_glowEntity, m_glowRest, m_glowWeights, m_glowSkinned);
    draw(m_eyeMesh, m_eyeEntity, m_eyeRest, m_eyeWeights, m_eyeSkinned);

    // Its strands, hanging and swaying (each link held to the next, damped); hunting, the nearest reaching down; holding, wrapped
    // round what it holds. Drawn as tubes through their links.
    if (!m_strands.empty())
    {
        const bool working = m_mode == Mode::Hunt || m_mode == Mode::Strike || m_mode == Mode::Hold;
        if (!working)
        {
            m_holder = -1;
            m_shapeFor = -1;
        }
        else if (m_holder < 0 || m_holder >= static_cast<int>(m_strands.size()))
        {
            const glm::vec3 target = m_mode == Mode::Strike ? m_strikeAt : m_mode == Mode::Hold ? m_holdPoint : m_lookAt;
            float nearest = 1.0e9f;
            for (size_t s = 0; s < m_strands.size(); ++s)
            {
                const float d = glm::distance(m_strands[s].points.front(), target);
                if (d < nearest)
                {
                    nearest = d;
                    m_holder = static_cast<int>(s);
                }
            }
        }
        const size_t holder = m_holder < 0 ? m_strands.size() : static_cast<size_t>(m_holder);
        size_t vertex = 0;
        for (size_t s = 0; s < m_strands.size(); ++s)
        {
            Strand& strand = m_strands[s];
            strand.points[0] = glm::vec3(fall * glm::vec4(body + bodyTurn * strand.root, 1.0f));
            // Grown out of the skin as it is drawn, not where the body would put it held still: the belly bends, breathes and swings
            // with the walk, and the root, held to the body alone, stood off it (you, 2026-11-16: they don't look like they connect).
            // Its root a little into the skin, its first link straight out of it.
            glm::vec3 outward{0.0f, -1.0f, 0.0f};
            if (!m_skinned.empty())
            {
                if (strand.skin < 0 || strand.skin >= static_cast<int>(m_skinned.size()))
                {
                    float nearest = 1.0e9f;
                    for (size_t v = 0; v < m_skinned.size(); ++v)
                    {
                        const glm::vec3 off = m_skinned[v].position + origin - strand.points[0];
                        const float d = glm::dot(off, off);
                        if (d < nearest && m_skinned[v].normal.y < 0.2f)
                        {
                            nearest = d;
                            strand.skin = static_cast<int>(v);
                        }
                    }
                }
                if (strand.skin >= 0)
                {
                    const MeshVertex& at = m_skinned[static_cast<size_t>(strand.skin)];
                    const float n = glm::length(at.normal);
                    outward = n > 1e-3f ? glm::normalize(glm::mix(at.normal / n, glm::vec3(0.0f, -1.0f, 0.0f), 0.4f)) : outward;
                    strand.points[0] = at.position + origin - outward * (strand.thick * 0.9f);
                }
            }
            const float keep = 0.97f - 0.08f * strand.stiffness;
            for (size_t i = 1; i < strand.points.size(); ++i)
            {
                const glm::vec3 now = strand.points[i];
                glm::vec3 moved = (now - strand.last[i]) * keep;
                const float most = strand.length * 0.3f;
                if (glm::length(moved) > most)
                {
                    moved *= most / glm::length(moved);
                }
                const glm::vec3 rest = strand.points[0] - glm::vec3(0.0f, strand.length * static_cast<float>(i), 0.0f);
                // Alive: never quite still -- a slow curl and sway of its own along it, more towards its end (dead, none).
                const float u = static_cast<float>(i) / static_cast<float>(strand.points.size());
                const glm::vec3 sway = dead ? glm::vec3(0.0f)
                                            : bodyTurn * glm::vec3(std::sin(time * 0.9f + strand.phase + u * 3.0f), 0.3f * std::sin(time * 1.3f + strand.phase * 2.0f + u * 5.0f),
                                                                   std::cos(time * 0.7f + strand.phase + u * 2.0f)) *
                                                  (strand.length * 0.6f * u * dt);
                strand.points[i] += moved + glm::vec3(0.0f, -9.8f, 0.0f) * dt * dt + (rest - now) * (strand.stiffness * 0.01f) + sway;
                strand.last[i] = now;
            }
            glm::vec3& tip = strand.points.back();
            if (m_mode == Mode::Hold && s == holder)
            {
                // (Round their middle, not over their face: wrapped at the eye, it swung across the view all the while it held them.)
                tip += (m_holdPoint + glm::vec3(0.0f, 0.8f, 0.0f) - tip) * std::min(dt * 4.0f, 1.0f);
            }
            else if (m_mode == Mode::Strike && s == holder)
            {
                // Reaching down for them, then whipping round them: the strike is its strand's.
                tip += (m_strikeAt + glm::vec3(0.0f, 1.0f, 0.0f) - tip) * std::min(dt * (m_strike < 0.6f ? 2.0f : 8.0f), 1.0f);
            }
            else if (m_mode == Mode::Hunt)
            {
                tip += (m_lookAt - tip) * std::min(dt * 0.25f, 1.0f);
            }
            // (Its first link out of the skin, the way the skin faces.)
            if (strand.points.size() > 1)
            {
                const glm::vec3 out = strand.points[0] + outward * strand.length;
                strand.points[1] = glm::mix(strand.points[1], out, 0.7f);
            }
            for (int pass = 0; pass < 4; ++pass)
            {
                for (size_t i = 1; i < strand.points.size(); ++i)
                {
                    const glm::vec3 apart = strand.points[i] - strand.points[i - 1];
                    const float length = glm::length(apart);
                    if (length > 1e-4f)
                    {
                        strand.points[i] = strand.points[i - 1] + apart * (strand.length / length);
                    }
                }
            }
            // The one that does the work, seen doing it (you, 2026-11-16: it only ever wiggled): hunting, reaching out towards them and
            // feeling about, raised and swaying; striking, whipped down in an arch onto them; holding, run down to them and coiled
            // round their middle. Its links drawn to that shape, over the hanging they would otherwise do.
            if (s == holder && (m_mode == Mode::Hunt || m_mode == Mode::Strike || (m_mode == Mode::Hold && m_holding >= 0)))
            {
                const glm::vec3 root = strand.points[0];
                const size_t links = strand.points.size();
                glm::vec3 end{0.0f};
                float weight = 0.0f;
                float arch = 0.25f;
                if (m_mode == Mode::Hunt)
                {
                    // Out towards them, most of the way, and moving about there, slowly.
                    const glm::vec3 toward = m_lookAt - root;
                    end = root + toward * 0.6f + glm::vec3(std::sin(time * 0.7f + strand.phase), 0.5f * std::sin(time * 1.1f), std::cos(time * 0.6f)) * (m_bodyHeight * 0.06f);
                    weight = 0.65f;
                    arch = 0.15f;
                }
                else if (m_mode == Mode::Strike)
                {
                    // Drawn back and up as it winds up, then down onto them.
                    const float windup = 1.9f / 2.8f;
                    const glm::vec3 at = m_strikeAt + glm::vec3(0.0f, 1.0f, 0.0f);
                    end = m_strike < windup ? glm::mix(root + (at - root) * 0.4f + glm::vec3(0.0f, m_bodyHeight * 0.15f, 0.0f), at, std::pow(m_strike / windup, 3.0f)) : at;
                    weight = glm::smoothstep(0.0f, 0.25f, m_strike);
                    arch = m_strike < windup ? 0.35f : 0.1f;
                }
                else
                {
                    end = m_holdPoint + glm::vec3(0.0f, 0.8f, 0.0f);
                    weight = 1.0f;
                    arch = 0.2f;
                }
                const glm::vec3 span = end - root;
                glm::vec3 bend = root + span * 0.5f + glm::vec3(0.0f, glm::length(span) * arch, 0.0f) +
                                 glm::normalize(glm::cross(span, glm::vec3(0.0f, 1.0f, 0.0f)) + glm::vec3(1e-4f)) * (glm::length(span) * 0.12f);
                // Reaching in through a doorway, and drawing what it took out through it: the curve through the doorway's middle.
                if (m_reachingIn && (m_mode == Mode::Strike || (m_mode == Mode::Hold && time < m_pullOutUntil + 0.6f)))
                {
                    bend = 2.0f * (m_reachDoor + glm::vec3(0.0f, 1.3f, 0.0f)) - 0.5f * (root + end);
                }
                // Eased to (above): faster in a strike, its whip, and slower otherwise.
                if (m_shapeFor != static_cast<int>(s))
                {
                    m_shapeFor = static_cast<int>(s);
                    m_shapeEnd = strand.points.back();
                    m_shapeBend = strand.points[strand.points.size() / 2];
                }
                const float follow = std::min(dt * (m_mode == Mode::Strike ? 10.0f : 4.0f), 1.0f);
                m_shapeEnd += (end - m_shapeEnd) * follow;
                m_shapeBend += (bend - m_shapeBend) * follow;
                end = m_shapeEnd;
                bend = m_shapeBend;
                // Holding: its last few links a coil round them, turning slowly as it holds.
                const size_t coil = m_mode == Mode::Hold ? std::min<size_t>(5, links - 2) : 0;
                for (size_t i = 1; i < links; ++i)
                {
                    glm::vec3 shaped;
                    if (coil > 0 && i + coil >= links)
                    {
                        const float k = static_cast<float>(i + coil - links) / static_cast<float>(coil);
                        const float a = k * kTau * 1.25f + time * 0.6f;
                        shaped = m_holdPoint + glm::vec3(std::cos(a) * 0.5f, 0.55f + k * 0.6f, std::sin(a) * 0.5f);
                    }
                    else
                    {
                        const float t = static_cast<float>(i) / static_cast<float>(links - 1 - coil);
                        shaped = (1.0f - t) * (1.0f - t) * root + 2.0f * (1.0f - t) * t * bend + t * t * end;
                    }
                    strand.points[i] = glm::mix(strand.points[i], shaped, std::min(weight * std::min(dt * 9.0f, 1.0f) * 1.6f + (weight >= 1.0f ? 0.3f : 0.0f), 1.0f));
                    strand.last[i] = glm::mix(strand.last[i], strand.points[i], weight);
                }
            }
            // Holding somebody: none of them across their face -- the rest kept a couple of metres off their eyes, the one that holds them
            // all but its end (you, 2026-11-16: held by a giant, its strands swung back and forth through the picture).
            if (m_mode == Mode::Hold && m_holding >= 0)
            {
                const glm::vec3 eyes = m_holdPoint + glm::vec3(0.0f, 1.55f, 0.0f);
                const size_t last = strand.points.size() - 1;
                for (size_t i = 1; i < strand.points.size(); ++i)
                {
                    if (s == holder && i + 6 > last)
                    {
                        continue;
                    }
                    const float clear = s == holder ? 1.1f : 2.4f;
                    const glm::vec3 off = strand.points[i] - eyes;
                    const float d = glm::length(off);
                    if (d < clear)
                    {
                        strand.points[i] = eyes + (d > 1e-3f ? off / d : glm::vec3(0.0f, -1.0f, 0.0f)) * clear;
                        strand.last[i] = strand.points[i];
                    }
                }
            }
            // Lying on the ground where they reach it, never into it (you, 2026-11-16: they went into the ground).
            if (world.ground)
            {
                for (size_t i = 1; i < strand.points.size(); ++i)
                {
                    glm::vec3& p = strand.points[i];
                    const float down = static_cast<float>(i) / static_cast<float>(strand.points.size());
                    const float floor = world.ground(p.x, p.z) + strand.thick * (1.0f - 0.75f * down) + 0.08f;
                    if (p.y < floor)
                    {
                        p.y = floor;
                        strand.last[i].y = std::max(strand.last[i].y, floor); // (and no bounce off it)
                    }
                    // Nor into a building: laid over its roof (you, 2026-11-16: they clipped into buildings).
                    if (world.roof)
                    {
                        const float roof = world.roof(p.x, p.z) + strand.thick + 0.08f;
                        if (roof > -1.0e8f && p.y < roof)
                        {
                            p.y = roof;
                            strand.last[i].y = std::max(strand.last[i].y, roof);
                        }
                    }
                }
            }
            // Not bunched up together: held a few of their widths apart from the others; and smooth along themselves, no kinks.
            for (size_t other = 0; other < m_strands.size(); ++other)
            {
                if (other == s)
                {
                    continue;
                }
                const Strand& them = m_strands[other];
                const float apartAtLeast = (strand.thick + them.thick) * 2.5f;
                for (size_t i = 2; i < strand.points.size() && i < them.points.size(); ++i)
                {
                    const glm::vec3 d = strand.points[i] - them.points[i];
                    const float length = glm::length(d);
                    if (length < apartAtLeast && length > 1e-4f)
                    {
                        // (Moved, not thrown: its last place moved with it, or the push was a speed it kept, and they twitched.)
                        const glm::vec3 push = d / length * ((apartAtLeast - length) * 0.5f);
                        strand.points[i] += push;
                        strand.last[i] += push;
                    }
                }
            }
            for (size_t i = 1; i + 1 < strand.points.size(); ++i)
            {
                const glm::vec3 smoothed = glm::mix(strand.points[i], (strand.points[i - 1] + strand.points[i + 1]) * 0.5f, 0.2f);
                strand.last[i] += smoothed - strand.points[i];
                strand.points[i] = smoothed;
            }
            // And no link moved further in a frame than a strand can move, over and above its root's own going with the body: a jump
            // anywhere above (a shape changed, a push off a roof, a constraint pulled taut) was a link flung across in one frame.
            if (strand.drawn.size() == strand.points.size() && dt > 0.0f)
            {
                const glm::vec3 rootMoved = strand.points[0] - strand.drawn[0];
                const float most = (m_mode == Mode::Strike ? 26.0f : 18.0f) * dt;
                // (And eased there, over and above its root's going: the steps above pulled a link one way and another from frame to
                // frame, and it shook back and forth -- the strands' spasm.)
                const float ease = 1.0f - std::exp(-dt * (m_mode == Mode::Strike ? 16.0f : 12.0f));
                for (size_t i = 1; i < strand.points.size(); ++i)
                {
                    glm::vec3 own = (strand.points[i] - strand.drawn[i] - rootMoved) * ease;
                    const float length = glm::length(own);
                    if (length > most)
                    {
                        own *= most / length;
                    }
                    const glm::vec3 kept = strand.drawn[i] + rootMoved + own;
                    strand.last[i] += kept - strand.points[i];
                    strand.points[i] = kept;
                }
            }
            strand.drawn = strand.points;
            // Each ring turned on from the last (carried along it), not set afresh from a fixed direction: where it ran along that
            // direction the rings flipped round, and the tube twisted and jumped as it swung through it.
            glm::vec3 side{0.0f};
            for (size_t i = 0; i < strand.points.size(); ++i)
            {
                const glm::vec3 a = strand.points[i > 0 ? i - 1 : 0];
                const glm::vec3 b = strand.points[std::min(i + 1, strand.points.size() - 1)];
                const glm::vec3 along = glm::normalize(b - a + glm::vec3(0.0f, -1e-4f, 0.0f));
                if (i == 0)
                {
                    side = glm::cross(along, bodyTurn * glm::vec3(0.0f, 0.0f, 1.0f));
                    if (glm::length(side) < 1e-3f)
                    {
                        side = glm::cross(along, bodyTurn * glm::vec3(1.0f, 0.0f, 0.0f));
                    }
                }
                side -= along * glm::dot(side, along);
                side = glm::length(side) > 1e-4f ? glm::normalize(side) : glm::normalize(glm::cross(along, glm::vec3(0.0f, 0.0f, 1.0f)) + glm::vec3(1e-3f, 0.0f, 0.0f));
                const glm::vec3 other = glm::cross(along, side);
                const float down = static_cast<float>(i) / static_cast<float>(strand.points.size());
                const float r = strand.thick * (1.0f - 0.75f * down) * (1.0f + 1.2f * std::pow(std::max(1.0f - down * 6.0f, 0.0f), 2.0f));
                for (int j = 0; j < kStrandRing; ++j)
                {
                    const float u = kTau * static_cast<float>(j) / kStrandRing;
                    const glm::vec3 n = side * std::cos(u) + other * std::sin(u);
                    MeshVertex& v = m_strandShape.vertices[vertex++];
                    v.position = strand.points[i] + n * r - origin;
                    v.normal = n;
                }
            }
        }
        AABB bounds;
        for (const MeshVertex& v : m_strandShape.vertices)
        {
            bounds.min = glm::min(bounds.min, v.position);
            bounds.max = glm::max(bounds.max, v.position);
        }
        m_meshes.UpdateDynamic(m_strandMesh, m_strandShape.vertices, bounds);
        if (Transform* transform = m_scene.GetTransform(m_strandEntity))
        {
            transform->position = origin;
        }
    }

    // What rounds find.
    for (const BoneBox& box : m_boxes)
    {
        const glm::mat4& bone = m_skinning[static_cast<size_t>(box.bone)];
        Transform transform;
        if (box.bone == 0)
        {
            transform.position = glm::vec3(bone[3]);
            transform.rotation = glm::quat_cast(glm::mat3(Unscaled(bone))) * glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        }
        else if (box.bone == 1)
        {
            transform.position = m_head;
            transform.rotation = glm::quat_cast(glm::mat3(Unscaled(bone))) * glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        }
        else
        {
            transform.position = glm::vec3(bone * glm::vec4(0.0f, box.halfLength, 0.0f, 1.0f));
            transform.rotation = glm::quat_cast(glm::mat3(Unscaled(bone)));
        }
        m_physics.SetTransform(box.body, transform);
    }
}

// --- The leviathan: a body of its own --------------------------------------------------------------------------------------------

void Giant::BuildSwimmer(MeshLibrary& meshes)
{
    SeededRandom random(static_cast<uint64_t>(m_seed) * 6364136223846793005ull + 1442695040888963407ull);
    // A serpent always (you, 2026-11-16: serpents and their kinds, nothing like a squid or a ray), of one of its kinds.
    m_form = static_cast<Form>(std::min(static_cast<int>(random.Unit() * static_cast<float>(Form::Count)), static_cast<int>(Form::Count) - 1));
    // (Really big, some of them: most forty-odd metres, now and then one a hundred and more.)
    const float L = 30.0f + 110.0f * std::pow(random.Unit(), 2.0f);
    m_size = L;
    m_speed = 3.2f + L * 0.03f;
    m_maxHealth = 1500.0f + 90.0f * L;
    m_health = m_maxHealth;
    m_spineBones = 18;
    const int N = m_spineBones;
    // How thick it is, by its kind and its own; flattened side to side, an eel.
    const float R = L * (m_form == Form::Armoured ? random.Range(0.055f, 0.07f) : m_form == Form::Paddled ? random.Range(0.06f, 0.075f)
                         : m_form == Form::Eel ? random.Range(0.035f, 0.045f) : random.Range(0.042f, 0.056f));
    const glm::vec2 squash = m_form == Form::Eel ? glm::vec2(0.72f, 1.15f) : glm::vec2(1.0f);
    // Segmented, as the first ones were (you: too smooth is not frightening): rings of it, each swelling between grooves -- an armoured
    // one's plates, each lapping over the next.
    const float rings = std::floor(random.Range(18.0f, 30.0f) * (m_form == Form::Armoured ? 0.7f : 1.0f));
    const float groove = m_form == Form::Armoured ? 0.16f : random.Range(0.08f, 0.13f);
    const bool crest = m_form == Form::Serpent && random.Unit() < 0.6f;
    const float bumps = 0.05f;
    const uint32_t grain = m_seed * 31u + 7u;
    // The head: its length against the body, how wide its skull is, how long its snout.
    // (Long and low, a serpent's, not a ball: as long as it was wide, the head read as a blob.)
    // (Long, low and wedge-shaped: you, 2026-11-16 -- the head a round blob, nothing threatening about it.)
    const float head = L * random.Range(0.12f, 0.15f);
    const float skullW = R * random.Range(0.85f, 1.0f);
    const float snout = random.Range(0.8f, 1.2f);
    const auto body = [&](float t)
    {
        // A neck narrower than the head behind it, swelling to the body and tapering to the tail.
        const float neck = 0.72f + 0.28f * glm::smoothstep(head / L, head / L + 0.08f, t);
        return R * neck * (1.0f - 0.86f * std::pow(std::clamp(t, 0.0f, 1.0f), 1.5f));
    };
    const auto ringed = [&](float t)
    {
        const float r = std::fmod(std::max(t - head / L, 0.0f) * rings, 1.0f);
        const float swell = m_form == Form::Armoured ? std::pow(1.0f - r, 0.6f) : std::pow(std::sin(glm::pi<float>() * r), 0.5f);
        return body(t) * (1.0f - groove + groove * swell);
    };
    m_radius.resize(static_cast<size_t>(N));
    for (int k = 0; k < N; ++k)
    {
        m_radius[static_cast<size_t>(k)] = std::max(body(static_cast<float>(k) / static_cast<float>(N - 1)), 0.4f);
    }
    // The face, a serpent's: a long skull over a lower jaw, a mouth line between them the length of the head, teeth down both lips, the
    // eyes high on the skull under a heavy brow.
    const float mouthY = -0.2f * R;
    const float mouthFront = -0.35f * head * snout;
    const float mouthBack = 0.75f * head;
    const glm::vec3 mouthHalf{skullW * 1.2f, 0.045f * R, (mouthBack - mouthFront) * 0.5f};
    const auto along = [&](float z) { return std::clamp((z - mouthFront) / std::max(mouthBack - mouthFront, 1.0e-3f), 0.0f, 1.0f); };
    const auto skullHalfWidth = [&](float z)
    {
        // The skull's width at a point along it: a viper's wedge, widest at the hinge of its jaws, narrowing to the snout.
        return skullW * (0.42f + 0.58f * std::pow(along(z), 0.6f));
    };
    // How high its skull stands over the mouth line, and how deep its lower jaw hangs under it, along it.
    // (Heavy, a skull and a jaw with weight in them -- you, 2026-11-16, its pictures: the head of something that bites through hulls.)
    const auto skullHigh = [&](float z) { return R * (0.3f + 0.42f * std::pow(along(z), 0.75f)); };
    const auto jawDeep = [&](float z) { return R * (0.2f + 0.3f * along(z)); };
    const float tipZ = mouthFront - 0.06f * head;
    const float eyeZ = glm::mix(mouthFront, mouthBack, 0.52f);
    std::vector<std::pair<glm::vec3, glm::vec3>> teeth;
    {
        const int count = 10 + static_cast<int>(random.Unit() * 5.0f);
        const float toothLong = 0.2f * R;
        for (int i = 0; i < count; ++i)
        {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
            const float z = glm::mix(mouthFront + 0.06f * head, mouthBack - 0.1f * head, u);
            const float x = skullHalfWidth(z) * 0.82f;
            const float longer = 1.0f - 0.35f * u; // (shorter towards the back)
            for (const float side : {-1.0f, 1.0f})
            {
                teeth.push_back({glm::vec3(side * x, mouthY + mouthHalf.y + toothLong * 0.4f, z), glm::vec3(side * x, mouthY + mouthHalf.y - toothLong * longer, z)});
                teeth.push_back({glm::vec3(side * x * 0.92f, mouthY - mouthHalf.y - toothLong * 0.4f, z + 0.02f * head),
                                 glm::vec3(side * x * 0.92f, mouthY - mouthHalf.y + toothLong * 0.8f, z + 0.02f * head)});
            }
        }
    }
    const float toothR = 0.04f * R;
    // Its fangs: a pair at the front of the upper jaw hanging down past the lower lip, a pair from the lower jaw standing up in front
    // of the upper -- long, thick at the root.
    struct Fang
    {
        glm::vec3 base;
        glm::vec3 tip;
        float r;
    };
    std::vector<Fang> fangs;
    {
        const float z = mouthFront + 0.1f * head;
        const float x = skullHalfWidth(z) * 0.7f;
        const float upper = R * random.Range(0.45f, 0.65f);
        const float lower = R * random.Range(0.35f, 0.5f);
        for (const float side : {-1.0f, 1.0f})
        {
            fangs.push_back({glm::vec3(side * x, mouthY + 0.1f * R, z), glm::vec3(side * x * 0.9f, mouthY - upper, z + 0.05f * head), 0.07f * R});
            fangs.push_back({glm::vec3(side * x * 0.85f, mouthY - 0.1f * R, z + 0.12f * head),
                             glm::vec3(side * x * 0.95f, mouthY + lower, z + 0.08f * head), 0.06f * R});
        }
    }
    // The mouth line, parted right through the front of its snout: stopped short of the tip, the snout's end was one skin from lip to
    // lip, stretched across the front of the open mouth (you, 2026-11-17: the front of the mouth, the lip is connected).
    const float slitFront = mouthFront - 0.06f * head - 0.2f * head;
    const glm::vec3 slitMiddle{0.0f, mouthY, (slitFront + mouthBack) * 0.5f};
    const glm::vec3 slitHalf{mouthHalf.x, mouthHalf.y, (mouthBack - slitFront) * 0.5f};
    const auto inMouth = [&](const glm::vec3& p) { return RoundedBox(p - slitMiddle, slitHalf, slitHalf.y * 0.8f); };
    // Inside its mouth (you, 2026-11-17: the inside of its jaw looked wrong -- two flat cut faces with a red slab between): the roof of
    // it arched up into the skull between the rows of upper teeth, a trough down the lower jaw between its rows, both leaving a gum
    // along the lip for the teeth to stand in; and at the back, its throat, going on down into it.
    const auto palate = [&](const glm::vec3& p)
    {
        const float z = std::clamp(p.z, mouthFront, mouthBack);
        const float w = skullHalfWidth(z) * 0.74f;
        const float high = skullHigh(z) * 0.32f;
        const glm::vec2 across{p.x / std::max(w, 1e-3f), (p.y - mouthY) / std::max(high, 1e-3f)};
        const float slice = (glm::length(across) - 1.0f) * std::min(w, high);
        return std::max(slice, std::max(mouthFront + 0.1f * head - p.z, p.z - mouthBack - 0.05f * head));
    };
    const auto trough = [&](const glm::vec3& p)
    {
        const float z = std::clamp(p.z, mouthFront, mouthBack);
        const float w = skullHalfWidth(z) * 0.68f;
        const float deep = jawDeep(z) * 0.38f;
        const glm::vec2 across{p.x / std::max(w, 1e-3f), (p.y - mouthY) / std::max(deep, 1e-3f)};
        const float slice = (glm::length(across) - 1.0f) * std::min(w, deep);
        return std::max(slice, std::max(mouthFront + 0.12f * head - p.z, p.z - mouthBack - 0.05f * head));
    };
    // A sea dragon's whiskers: from the sides of its snout, long, hanging down and back (nothing on any other kind).
    const auto whiskers = [&](const glm::vec3& p)
    {
        float d = 1.0e9f;
        if (m_form != Form::Dragon || p.z > head * 2.4f)
        {
            return d;
        }
        for (const float side : {-1.0f, 1.0f})
        {
            const glm::vec3 root{side * skullHalfWidth(mouthFront + 0.15f * head) * 0.9f, mouthY + 0.12f * R, mouthFront + 0.15f * head};
            const glm::vec3 bend = root + glm::vec3(side * 0.7f * R, -0.9f * R, 0.5f * head);
            d = std::min(d, Sdf::RoundCone(p, root, bend, 0.07f * R, 0.04f * R));
            d = std::min(d, Sdf::RoundCone(p, bend, bend + glm::vec3(side * 0.3f * R, -0.5f * R, 1.1f * head), 0.04f * R, 0.015f * R));
        }
        return d;
    };
    const auto throat = [&](const glm::vec3& p)
    {
        return Sdf::RoundCone(p, glm::vec3(0.0f, mouthY, mouthBack - 0.15f * head), glm::vec3(0.0f, mouthY + 0.05f * R, mouthBack + 0.45f * head),
                              0.28f * R, 0.16f * R);
    };
    constexpr int kSamples = 30;
    const auto field = [&](const glm::vec3& p)
    {
        const float t = std::clamp(p.z / L, 0.0f, 1.0f);
        const glm::vec3 q{p.x / squash.x, p.y / squash.y, p.z};
        float d = 1.0e9f;
        for (int i = 0; i < kSamples; ++i)
        {
            const float a = static_cast<float>(i) / kSamples;
            const float b = static_cast<float>(i + 1) / kSamples;
            if (std::abs(t - (a + b) * 0.5f) > 0.08f && i != 0 && i != kSamples - 1)
            {
                continue;
            }
            d = std::min(d, Sdf::RoundCone(q, glm::vec3(0.0f, 0.0f, a * L), glm::vec3(0.0f, 0.0f, b * L), body(a), body(b)));
        }
        d *= std::min(squash.x, squash.y);
        // The rings: the surface pulled in at each groove (by the ring profile against the smooth one).
        d += body(t) - ringed(t);
        // The head: a skull, a snout, a lower jaw, a brow over each eye.
        if (p.z < head * 1.6f)
        {
            // Swept along it, slice by slice: an ellipse for the skull standing over the mouth line, flat-topped, and one for the jaw
            // hanging under it -- each as wide and as high as it is there.
            const float zc = std::clamp(p.z, tipZ, mouthBack + 0.35f * head);
            // Its snout rounded off over its last fifth -- narrower, lower and shallower towards its end, as a muzzle is -- not the
            // whole head's section cut off flat at its end (you, 2026-11-17: the front of its face was flat).
            const float cap = 0.22f * head;
            const float into = std::clamp((p.z - tipZ) / cap, 0.0f, 1.0f);
            const float round = std::sqrt(std::max(1.0f - (1.0f - into) * (1.0f - into), 0.0f));
            const float w = skullHalfWidth(zc) * glm::mix(0.3f, 1.0f, round);
            const float hu = skullHigh(zc) * glm::mix(0.4f, 1.0f, round);
            const float hj = jawDeep(zc) * glm::mix(0.4f, 1.0f, round);
            const glm::vec2 upper{p.x / w, (p.y - (mouthY + hu * 0.45f)) / (hu * 0.62f)};
            float h = (glm::length(upper) - 1.0f) * std::min(w, hu * 0.62f);
            const glm::vec2 lower{p.x / (w * 0.9f), (p.y - (mouthY - hj * 0.45f)) / (hj * 0.7f)};
            h = Sdf::SmoothUnion(h, (glm::length(lower) - 1.0f) * std::min(w * 0.9f, hj * 0.7f), R * 0.05f);
            h = std::max(h, tipZ - 0.02f * head - p.z);
            h = std::max(h, p.z - (mouthBack + 0.35f * head));
            // Its snout's end rounded, not cut off flat.
            h = Sdf::SmoothUnion(h, Sdf::Ellipsoid(p - glm::vec3(0.0f, mouthY, tipZ + 0.04f * head),
                                                   glm::vec3(skullHalfWidth(tipZ) * 0.8f, (skullHigh(tipZ) + jawDeep(tipZ)) * 0.45f, 0.06f * head)) * 0.8f,
                                 R * 0.03f);
            for (const float side : {-1.0f, 1.0f})
            {
                // Brows: ridges slanting down to the snout over each eye -- a frown.
                const float ew = skullHalfWidth(eyeZ);
                const float top = mouthY + skullHigh(eyeZ) * 1.0f;
                h = Sdf::SmoothUnion(h, Sdf::RoundCone(p, glm::vec3(side * ew * 0.55f, top + 0.08f * R, eyeZ + 0.28f * head),
                                                       glm::vec3(side * ew * 0.92f, top - 0.02f * R, eyeZ - 0.18f * head), 0.1f * R, 0.06f * R),
                                     R * 0.05f);
                // Its cheeks: the swelling at the back of the jaws a viper has, that makes its head a spade.
                h = Sdf::SmoothUnion(h, Sdf::Ellipsoid(p - glm::vec3(side * skullW * 0.82f, mouthY + 0.02f * R, mouthBack - 0.12f * head),
                                                       glm::vec3(0.3f * R, 0.3f * R, 0.32f * head)) * 0.8f,
                                     R * 0.12f);
                // Its eyes' sockets, under the brow: two, the second a little behind and below the first.
                h = Sdf::SmoothCut(h, glm::length(p - glm::vec3(side * ew * 0.95f, top - 0.16f * R, eyeZ)) - 0.13f * R, 0.03f * R);
                const float ew2 = skullHalfWidth(eyeZ + 0.16f * head);
                h = Sdf::SmoothCut(h, glm::length(p - glm::vec3(side * ew2 * 0.98f, mouthY + skullHigh(eyeZ + 0.16f * head) - 0.3f * R, eyeZ + 0.16f * head)) - 0.1f * R,
                                   0.03f * R);
                // Knobs of bone along the brow.
                for (int k = 0; k < 3; ++k)
                {
                    const float z = eyeZ - 0.12f * head + 0.14f * head * static_cast<float>(k);
                    h = Sdf::SmoothUnion(h, glm::length(p - glm::vec3(side * skullHalfWidth(z) * 0.78f, mouthY + skullHigh(z) * 1.02f, z)) - 0.07f * R, R * 0.04f);
                }
                // Bony spikes down the side of the lower jaw, pointing down and back.
                for (int k = 0; k < 4; ++k)
                {
                    const float z = glm::mix(mouthFront + 0.25f * head, mouthBack - 0.05f * head, static_cast<float>(k) / 3.0f);
                    const glm::vec3 root{side * skullHalfWidth(z) * 0.88f, mouthY - jawDeep(z) * 0.7f, z};
                    h = Sdf::SmoothUnion(h, Sdf::RoundCone(p, root, root + glm::vec3(side * 0.18f * R, -0.28f * R, 0.12f * head), 0.07f * R, 0.015f * R), R * 0.03f);
                }
                // A horn swept back from behind each brow, and a shorter spine under it.
                const glm::vec3 hornRoot{side * skullHalfWidth(mouthBack) * 0.6f, mouthY + skullHigh(mouthBack) * 0.9f, mouthBack - 0.05f * head};
                h = Sdf::SmoothUnion(h, Sdf::RoundCone(p, hornRoot, hornRoot + glm::vec3(side * 0.25f * R, 0.35f * R, 0.55f * head), 0.11f * R, 0.025f * R), R * 0.05f);
                const glm::vec3 spineRoot{side * skullHalfWidth(mouthBack) * 0.9f, mouthY + skullHigh(mouthBack) * 0.45f, mouthBack + 0.05f * head};
                h = Sdf::SmoothUnion(h, Sdf::RoundCone(p, spineRoot, spineRoot + glm::vec3(side * 0.35f * R, 0.1f * R, 0.4f * head), 0.08f * R, 0.02f * R), R * 0.05f);
                // A nostril pit near the snout's end.
                h = Sdf::SmoothCut(h, Sdf::Ellipsoid(p - glm::vec3(side * skullHalfWidth(tipZ) * 0.55f, mouthY + skullHigh(tipZ) * 0.9f, tipZ + 0.12f * head),
                                                     glm::vec3(0.018f * R, 0.03f * R, 0.06f * head)) * 0.8f,
                                   0.01f * R);
            }
            // A low ridge down the middle of its skull, plated.
            h = Sdf::SmoothUnion(h, Sdf::RoundCone(p, glm::vec3(0.0f, mouthY + skullHigh(eyeZ - 0.2f * head) * 1.02f, eyeZ - 0.2f * head),
                                                   glm::vec3(0.0f, mouthY + skullHigh(mouthBack) * 1.05f, mouthBack + 0.2f * head), 0.05f * R, 0.07f * R),
                                 R * 0.06f);
            // (Its ellipsoids' distances only bounds, too long far from them: taken at face value, the coarse pass over the grid
            // skipped cells the surface ran through, and the snout's front came out a hole. Shortened, nothing is skipped that is
            // near it.)
            d = Sdf::SmoothUnion(d, h * 0.55f, R * 0.14f);
            // The mouth line, the length of its head, and its teeth.
            d = Sdf::SmoothCut(d, inMouth(p), 0.03f * R);
        }
        switch (m_form)
        {
        case Form::Serpent:
            if (crest)
            {
                for (int i = 2; i < 12; ++i)
                {
                    const float ct = static_cast<float>(i) / 13.0f;
                    d = Sdf::SmoothUnion(d, 0.55f * Sdf::Ellipsoid(p - glm::vec3(0.0f, body(ct) * 0.85f, ct * L), glm::vec3(0.06f * R, 0.5f * body(ct), 0.035f * L)), R * 0.15f);
                }
            }
            break;
        case Form::Eel:
            // A fin the length of its back and under its tail, a ribbon.
            if (t > 0.12f)
            {
                const float rise = body(t) * squash.y;
                d = Sdf::SmoothUnion(d, 0.55f * Sdf::Ellipsoid(glm::vec3(p.x, p.y - rise * 0.9f, (t - 0.56f) * L), glm::vec3(0.03f * R, 0.45f * R, 0.46f * L)), R * 0.1f);
                if (t > 0.45f)
                {
                    d = Sdf::SmoothUnion(d, 0.55f * Sdf::Ellipsoid(glm::vec3(p.x, p.y + rise * 0.9f, (t - 0.75f) * L), glm::vec3(0.03f * R, 0.35f * R, 0.28f * L)), R * 0.1f);
                }
            }
            break;
        case Form::Armoured:
        case Form::Barbed:
        {
            // A plate's ridge -- or a barb -- standing up from every ring along its back.
            const float r = std::fmod(std::max(t - head / L, 0.0f) * rings, 1.0f);
            const float z = (t - r / rings) * L + 0.25f * L / rings;
            if (t > head / L && t < 0.93f)
            {
                const float at = std::clamp(z / L, 0.0f, 1.0f);
                const glm::vec3 base{0.0f, ringed(at) * 0.8f, z};
                const glm::vec3 tip = base + glm::vec3(0.0f, (m_form == Form::Barbed ? 0.7f : 0.3f) * body(at), (m_form == Form::Barbed ? 0.35f : 0.15f) * L / rings);
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, base, tip, 0.16f * body(at), 0.02f * R), R * 0.05f);
            }
            break;
        }
        case Form::Frilled:
            // A frill round its neck, a fan of spines webbed between, laid back.
            if (p.z > head * 0.6f && p.z < head * 2.0f)
            {
                for (int i = 0; i < 9; ++i)
                {
                    const float a = glm::radians(-150.0f + 300.0f * static_cast<float>(i) / 8.0f);
                    const glm::vec3 root{std::sin(a) * R * 0.6f, std::cos(a) * R * 0.6f, head * 1.0f};
                    const glm::vec3 tip = root + glm::vec3(std::sin(a) * R * 1.3f, std::cos(a) * R * 1.3f, head * 0.6f);
                    d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, root, tip, 0.14f * R, 0.06f * R), R * 0.2f);
                }
            }
            break;
        case Form::Paddled:
        {
            // Four flippers, flat and swept back, out of its sides at its shoulders and its hips; a fluke standing up at its tail's end.
            for (const float at : {0.2f, 0.52f})
            {
                const float r0 = body(at);
                const float size = at < 0.3f ? 1.0f : 0.8f;
                for (const float side : {-1.0f, 1.0f})
                {
                    const glm::vec3 root{side * r0 * 0.7f, -r0 * 0.35f, at * L};
                    const glm::vec3 tip = root + glm::vec3(side * R * 1.9f * size, -R * 0.5f * size, 0.09f * L * size);
                    // (Flattened: measured squashed up and down about the flipper's own height.)
                    const glm::vec3 flat{p.x, root.y + (p.y - root.y) * 2.6f, p.z};
                    d = Sdf::SmoothUnion(d, 0.38f * Sdf::RoundCone(flat, root, tip, 0.42f * R * size, 0.14f * R * size), R * 0.2f);
                }
            }
            if (t > 0.86f)
            {
                d = Sdf::SmoothUnion(d, 0.55f * Sdf::Ellipsoid(p - glm::vec3(0.0f, 0.0f, 0.97f * L), glm::vec3(0.05f * R, 1.3f * R, 0.06f * L)), R * 0.1f);
            }
            break;
        }
        case Form::Dragon:
        {
            // A mane of spines down its neck, laid back; fins out of its shoulders; and (on the head: below) long whiskers.
            if (p.z > head * 0.7f && p.z < head * 3.6f)
            {
                for (int i = 0; i < 8; ++i)
                {
                    const float z = head * (0.9f + 0.33f * static_cast<float>(i));
                    const float r0 = body(std::clamp(z / L, 0.0f, 1.0f));
                    const glm::vec3 root{0.0f, r0 * 0.8f, z};
                    d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, root, root + glm::vec3(0.0f, r0 * 0.9f, head * 0.45f), 0.1f * R, 0.02f * R), R * 0.08f);
                }
            }
            for (const float side : {-1.0f, 1.0f})
            {
                const float r0 = body(0.17f);
                const glm::vec3 root{side * r0 * 0.75f, -r0 * 0.2f, 0.17f * L};
                const glm::vec3 flat{p.x, root.y + (p.y - root.y) * 3.0f, p.z};
                d = Sdf::SmoothUnion(d, 0.33f * Sdf::RoundCone(flat, root, root + glm::vec3(side * R * 1.3f, -R * 0.3f, 0.07f * L), 0.35f * R, 0.06f * R), R * 0.15f);
            }
            break;
        }
        case Form::Count:
            break;
        }
        d = std::min(d, whiskers(p));
        return d + (Sdf::Fbm(p / (R * 0.7f), grain, 2) - 0.5f) * R * bumps;
    };
    const float reachX = R * (m_form == Form::Frilled ? 2.6f : m_form == Form::Paddled ? 2.9f : m_form == Form::Dragon ? 2.3f : 1.8f);
    const float reachY = R * (m_form == Form::Frilled ? 2.6f : m_form == Form::Barbed ? 2.4f : m_form == Form::Paddled ? 1.9f : m_form == Form::Dragon ? 2.4f : 1.8f) *
                         squash.y;
    // Its lower jaw, apart from the rest (it opens: you, 2026-11-16 -- its mouth never did): all of it under the mouth line, from the
    // front of the mouth back to its hinge.
    m_jawHinge = glm::vec3(0.0f, mouthY, mouthBack);
    // How much of a point of its head goes with the jaw as it opens (you, 2026-11-17: the jaw did not go smoothly into the body, a red
    // band at its back -- built apart, cut off the head by a flat wall at its hinge, the wall showed as it opened; and a lip across the
    // middle of the open mouth -- the jaw built a little up into the head all along it, to be hidden there shut). Now the head and its
    // jaw are one skin, parted only along the mouth: all of what is under the mouth line in front goes with the jaw; behind the corners
    // of the mouth, less and less the further back and the nearer the mouth line -- the skin of its throat stretched as it opens, as a
    // snake's is. (Not its whiskers, though they hang down past the mouth line: they stay with the skull -- cut in two by it once, they
    // came apart in pieces by its face.)
    const auto jawWeight = [&](const glm::vec3& p)
    {
        if (p.y > mouthY || whiskers(p) < 0.06f * R)
        {
            return 0.0f;
        }
        const float back = 1.0f - glm::smoothstep(mouthBack - 0.1f * head, mouthBack + 0.5f * head, p.z);
        const float behindCorner = glm::smoothstep(mouthBack - 0.3f * head, mouthBack + 0.02f * head, p.z);
        const float low = glm::mix(1.0f, 1.0f - glm::smoothstep(mouthY - 0.45f * R, mouthY, p.y), behindCorner);
        return std::clamp(back * low, 0.0f, 1.0f);
    };
    std::vector<float> jawWeights;
    // Built in three: the body behind the head at the grid it always was; the head and the jaw on one many times finer -- its face had
    // been made at the body's grid, a few dozen cells across the whole head (you: its face and edges far too coarse). Each overlaps
    // the next a little, inside the other where they meet.
    MeshData mesh;
    m_weights.clear();
    const float cell = std::max(L / 150.0f, 0.2f);
    const float fine = std::max(head / 60.0f, 0.05f);
    const auto part = [&](const std::function<float(const glm::vec3&)>& shape, const glm::vec3& lo, const glm::vec3& hi, float step, size_t budget, bool jaw)
    {
        const DistanceGrid grid = SampleDistance(lo, hi, step, shape);
        MeshData piece = SurfaceNets(grid);
        if (piece.indices.size() / 3 > budget)
        {
            std::vector<uint32_t> kept;
            piece = SimplifyMesh(piece, budget, step * 0.05f, kept);
        }
        const auto normalAt = [&](const glm::vec3& p)
        {
            const float h = step * 0.5f;
            const glm::vec3 n{shape(p + glm::vec3(h, 0, 0)) - shape(p - glm::vec3(h, 0, 0)), shape(p + glm::vec3(0, h, 0)) - shape(p - glm::vec3(0, h, 0)),
                              shape(p + glm::vec3(0, 0, h)) - shape(p - glm::vec3(0, 0, h))};
            const float length = glm::length(n);
            return length > 1e-6f ? n / length : glm::vec3(0.0f, 1.0f, 0.0f);
        };
        // Painted as a creature of its colours is: darker on its back than its belly, its markings, the grooves between its rings
        // darker; the inside of its mouth near black (and the cut where its jaw meets its skull, its throat), its teeth pale.
        const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
        for (MeshVertex vertex : piece.vertices)
        {
            glm::vec3 p = vertex.position;
            p -= normalAt(p) * shape(p);
            vertex.position = p;
            vertex.normal = normalAt(p);
            const float t = std::clamp(p.z / L, 0.0f, 1.0f);
            const float back = glm::smoothstep(-0.4f, 0.5f, vertex.normal.y);
            glm::vec3 colour = glm::mix(m_look.underside, m_look.skin, back);
            colour = glm::mix(colour, m_look.markings, SkinMarkingAt(m_look, p, vertex.normal, R * 0.6f));
            const float inGroove = 1.0f - glm::smoothstep(0.0f, groove * 0.6f, (ringed(t) / std::max(body(t), 1.0e-3f)) - (1.0f - groove));
            colour *= 1.0f - 0.35f * inGroove * (t > head / L ? 1.0f : 0.0f);
            if (p.z < head * 1.2f)
            {
                // The lips' edges, along the mouth line, a dark wet red (only there: not down the back of the jaw).
                colour = glm::mix(colour, glm::vec3(0.2f, 0.04f, 0.06f), 1.0f - glm::smoothstep(0.0f, fine * 2.0f, std::max(inMouth(p), 0.0f)));
                // Its gums and the inside of its mouth a wet dark pink, darker further in, its throat going to black.
                const float inside = 1.0f - glm::smoothstep(0.0f, fine * 2.5f, std::min(std::abs(palate(p)), std::abs(trough(p))));
                const float deep = glm::smoothstep(mouthFront, mouthBack + 0.2f * head, p.z);
                colour = glm::mix(colour, glm::mix(glm::vec3(0.55f, 0.16f, 0.18f), glm::vec3(0.18f, 0.03f, 0.05f), deep), inside);
                colour = glm::mix(colour, glm::vec3(0.04f, 0.01f, 0.015f), 1.0f - glm::smoothstep(0.0f, fine * 3.0f, std::abs(throat(p))));

            }
            vertex.color = SkinColour(colour, 0.9f);
            vertex.uv = {0.0f, 0.0f};
            mesh.vertices.push_back(vertex);
            m_weights.push_back(SkinWeights{});
            jawWeights.push_back(jaw ? jawWeight(p) : 0.0f);
        }
        for (const uint32_t index : piece.indices)
        {
            mesh.indices.push_back(first + index);
        }
    };
    const float front = -1.1f * head * snout - 0.15f * head;
    const float neckEnd = head * (m_form == Form::Frilled || m_form == Form::Dragon ? 2.2f : 1.5f);
    part([&](const glm::vec3& p) { return std::max(field(p), neckEnd - head * 0.3f - p.z); }, {-reachX, -reachY, neckEnd - head * 0.4f}, {reachX, reachY, 1.04f * L}, cell, 13000,
         false);
    const glm::vec3 headLo{-std::min(reachX, std::max(skullW * 1.6f, R * 1.4f) + (m_form == Form::Dragon ? R * 1.4f : 0.0f)), -reachY, front};
    const glm::vec3 headHi{-headLo.x, reachY, neckEnd};
    // (The head drawn a little in towards its end, so where it stops it is inside the body's coarser skin: its end showed as a collar.)
    const auto inset = [&](float z) { return cell * 0.35f * glm::smoothstep(neckEnd - head * 0.45f, neckEnd, z); };
    part([&](const glm::vec3& p)
         {
             // (The roof of its mouth cut only up into the skull, the trough only down into the jaw: one skin, they would each have cut
             // into the other half too.)
             const float solid = std::max(field(p) + inset(p.z), p.z - neckEnd + fine);
             const float hollow = std::min(std::max(palate(p), mouthY - p.y), std::max(trough(p), p.y - mouthY));
             return Sdf::SmoothCut(Sdf::SmoothCut(solid, hollow, 0.04f * R), throat(p), 0.05f * R);
         },
         headLo, headHi, fine, 22000, true);
    // Its teeth and fangs (you, 2026-11-16: in the mouth they were white splotches -- carved out of the head's grid, one or two of its
    // cells across, they melted into blobs): each a cone of its own, smooth and sharp at any size, sunk a little into the gum -- the
    // upper jaw's on the head, the lower's on the jaw, opening with it -- ivory, darker at the root.
    {
        const auto cone = [&](const glm::vec3& base, const glm::vec3& tip, float radius)
        {
            constexpr int kSides = 10;
            const glm::vec3 axis = glm::normalize(tip - base + glm::vec3(1e-5f));
            const glm::vec3 side = glm::normalize(glm::cross(axis, std::abs(axis.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)));
            const glm::vec3 other = glm::cross(axis, side);
            const glm::vec3 root = base - axis * radius * 0.6f;
            const bool lower = base.y < mouthY;
            const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
            const float length = glm::distance(root, tip);
            for (int k = 0; k <= kSides; ++k)
            {
                const float a = kTau * static_cast<float>(k % kSides) / kSides;
                const glm::vec3 out = side * std::cos(a) + other * std::sin(a);
                MeshVertex ring;
                ring.position = root + out * radius;
                ring.normal = glm::normalize(out * length + axis * radius);
                ring.color = SkinColour(glm::vec3(0.62f, 0.56f, 0.46f), 0.9f);
                mesh.vertices.push_back(ring);
                MeshVertex point;
                point.position = tip;
                point.normal = ring.normal;
                point.color = SkinColour(glm::vec3(0.9f, 0.87f, 0.78f), 0.9f);
                mesh.vertices.push_back(point);
            }
            for (int k = 0; k < kSides; ++k)
            {
                const uint32_t a0 = first + static_cast<uint32_t>(k * 2);
                mesh.indices.insert(mesh.indices.end(), {a0, a0 + 2, a0 + 1, a0 + 1, a0 + 2, a0 + 3});
            }
            m_weights.resize(mesh.vertices.size(), SkinWeights{});
            // (A lower tooth with its jaw as much as the gum it stands in.)
            jawWeights.resize(mesh.vertices.size(), lower ? std::max(jawWeight(base), 0.0f) : 0.0f);
        };
        for (const auto& [base, tip] : teeth)
        {
            cone(base, tip, toothR);
        }
        for (const Fang& fang : fangs)
        {
            cone(fang.base, fang.tip, fang.r);
        }
    }

    // Its bones: the spine, head first.
    m_restInverse.assign(static_cast<size_t>(N), glm::mat4(1.0f));
    for (int k = 0; k < N; ++k)
    {
        m_restInverse[static_cast<size_t>(k)] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -L * static_cast<float>(k) / static_cast<float>(N - 1)));
    }
    const auto weigh = [&](const glm::vec3& p)
    {
        SkinWeights weights;
        // The head all on the first bone -- its jaw is on that bone's frame too -- and the body behind it coming off it smoothly over the
        // next length of a bone. (The head was put on whichever bone its length reached, so a long head's back was on the second: as
        // the body bent, the back of its skull and its cheeks went one way and its jaw the other -- the jaw stuck out to the side, and
        // the fins and spikes on the back of its head stood off it: you, 2026-11-17.)
        const float s = std::clamp(p.z / L, 0.0f, 1.0f) * static_cast<float>(N - 1);
        const int b0 = std::min(static_cast<int>(s), N - 2);
        const float f = std::clamp(s - static_cast<float>(b0), 0.0f, 1.0f);
        const float link = L / static_cast<float>(N - 1);
        const float u = glm::smoothstep(head, head + link, p.z);
        weights.bone[0] = 0;
        weights.weight[0] = 1.0f - u;
        weights.bone[1] = static_cast<uint8_t>(b0);
        weights.weight[1] = u * (1.0f - f);
        weights.bone[2] = static_cast<uint8_t>(b0 + 1);
        weights.weight[2] = u * f;
        return weights;
    };
    for (size_t i = 0; i < mesh.vertices.size(); ++i)
    {
        // Each on the spine; and what goes with the jaw, on it for that much (its own bone, the one after the spine's).
        m_weights[i] = weigh(mesh.vertices[i].position);
        const float jaw = i < jawWeights.size() ? jawWeights[i] : 0.0f;
        if (jaw > 0.0f)
        {
            for (int k = 0; k < 3; ++k)
            {
                m_weights[i].weight[static_cast<size_t>(k)] *= 1.0f - jaw;
            }
            m_weights[i].bone[3] = static_cast<uint8_t>(N);
            m_weights[i].weight[3] = jaw;
        }
    }
    m_rest = mesh;

    // Its eyes, high on its skull under the brow: lit on some.
    std::vector<glm::vec4> eyes;
    const int pairs = 2;
    const float eyeR = R * random.Range(0.065f, 0.08f);
    for (int k = 0; k < pairs; ++k)
    {
        for (const float side : {-1.0f, 1.0f})
        {
            // (In the socket under its brow -- a second, smaller pair behind on some.)
            const float z = eyeZ + 0.16f * head * static_cast<float>(k);
            const float ew = skullHalfWidth(z);
            const float r = k == 0 ? eyeR : eyeR * 0.8f;
            const float y = k == 0 ? mouthY + skullHigh(z) - 0.16f * R : mouthY + skullHigh(z) - 0.3f * R;
            eyes.push_back(glm::vec4(side * (ew * (k == 0 ? 0.95f : 0.98f) - r * 0.35f), y, z - r * 0.2f, r));
        }
    }
    m_eyeRest = Beads(eyes);
    m_eyeWeights.clear();
    for (const MeshVertex& vertex : m_eyeRest.vertices)
    {
        m_eyeWeights.push_back(weigh(vertex.position));
    }
    const bool eyesGlow = m_look.photophores > 0 || random.Unit() < 0.4f;
    // Its lights, on some: rows down its flanks, on the skin.
    m_glowRest = MeshData{};
    m_glowWeights.clear();
    if (m_look.photophores > 0)
    {
        std::vector<glm::vec4> lights;
        const int count = 10 + static_cast<int>(random.Unit() * 14.0f);
        for (int i = 0; i < count; ++i)
        {
            const float t = 0.12f + 0.73f * static_cast<float>(i) / static_cast<float>(count - 1);
            for (const float side : {-1.0f, 1.0f})
            {
                const glm::vec3 inside{0.0f, body(t) * 0.1f, t * L};
                const glm::vec3 on = OnSkin(field, inside, glm::vec3(side, -0.15f, 0.0f), R * 3.0f);
                if (std::abs(field(on)) < std::max(body(t) * 0.15f, 0.3f))
                {
                    lights.push_back(glm::vec4(on, std::max(body(t) * 0.06f, 0.15f)));
                }
            }
        }
        m_glowRest = Beads(lights);
        for (const MeshVertex& vertex : m_glowRest.vertices)
        {
            m_glowWeights.push_back(weigh(vertex.position));
        }
    }
    MakeVisual(meshes, m_rest, m_glowRest.indices.empty() ? nullptr : &m_glowRest, &m_eyeRest, eyesGlow, true);
    m_skinning.assign(static_cast<size_t>(N + 1), glm::mat4(1.0f)); // (its spine's, and its jaw's)
    // What rounds find: a capsule down each length of its spine, its head the first.
    for (int k = 0; k + 1 < N; ++k)
    {
        BoneBox box;
        box.bone = k;
        box.halfLength = L / static_cast<float>(N - 1) * 0.5f;
        box.head = k == 0;
        box.body = m_physics.CreateCapsule(box.halfLength, std::max(m_radius[static_cast<size_t>(k)], 0.5f), Transform{}, BodyMotion::Kinematic, 1000.0f,
                                           PhysicsLayer::Hitbox);
        if (box.body.IsValid())
        {
            m_boxes.push_back(box);
        }
    }
    // Laid out straight behind where it starts.
    const glm::vec3 behind = Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 64; ++i)
    {
        m_trail.push_back(m_position + behind * (L * 1.3f * static_cast<float>(i) / 63.0f));
    }
}

void Giant::AnimateSwimmer(const GiantWorld& world, float time, float dt)
{
    const int N = m_spineBones;
    const float L = m_size;
    // Where its front has been: the rest of it follows the same way, as a swimming thing's body does. (From the first, a level way
    // laid out behind it, its own length and more: laid out only as it swam, the rest of it was drawn on straight behind until then, and
    // where the two met, a kink ran down it as it went -- it twitched.)
    if (m_trail.empty())
    {
        const glm::vec3 behind = Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, 1.0f);
        for (int i = 0; i <= 300; ++i)
        {
            m_trail.push_back(m_position + behind * (L / 200.0f * static_cast<float>(i)));
        }
    }
    if (glm::distance(m_trail.front(), m_position) > L / 200.0f)
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
        // Past the end of where it has been: on straight behind, level -- not along however its first few metres went: just made, rising
        // to the depth it swims at, the whole of the rest of it stood straight up out of the sea from behind its head.
        glm::vec3 back = m_trail.size() >= 2 ? m_trail.back() - m_trail[m_trail.size() - 2] : glm::vec3(0.0f);
        back.y = 0.0f;
        back = glm::length(back) > 1e-3f ? glm::normalize(back) : Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, 1.0f);
        return last + back * (distance - walked);
    };
    const bool dead = m_mode == Mode::Dead;
    const float speed = glm::length(m_velocity);
    if (!dead)
    {
        // (Its beat as quick as it is going: a stroke for so far forward -- driven too fast, its tail thrashed while it hardly moved.)
        m_swimPhase += dt * (0.12f + speed / std::max(L * 0.35f, 1.0f));
    }
    m_roll += ((dead ? glm::pi<float>() : 0.0f) - m_roll) * std::min(dt * 0.4f, 1.0f);
    std::vector<glm::vec3> spine(static_cast<size_t>(N));
    const float link = L / static_cast<float>(N - 1);
    if (dead)
    {
        // Dead, it is a body, not a swimmer (you, 2026-11-16: ragdoll): its spine a chain let go, each joint borne up by the water (an
        // armoured one sinking under its plates), slowed by it, lying on the sea's floor or along the surface as it comes to them, each
        // link kept its length.
        if (m_deadPoints.size() != static_cast<size_t>(N))
        {
            m_deadPoints.clear();
            for (int k = 0; k < N; ++k)
            {
                m_deadPoints.push_back(alongTrail(link * static_cast<float>(k)));
            }
            m_deadPrevious = m_deadPoints;
        }
        const float sink = m_form == Form::Armoured ? -1.2f : 0.9f; // m/s^2: up, mostly; down, plated
        for (size_t k = 0; k < m_deadPoints.size(); ++k)
        {
            glm::vec3& p = m_deadPoints[k];
            const glm::vec3 moved = (p - m_deadPrevious[k]) * 0.97f;
            m_deadPrevious[k] = p;
            float surface = 0.0f;
            const bool wet = world.water && world.water(p, surface) > 0.0f;
            p += moved + glm::vec3(0.0f, wet ? sink : -9.8f, 0.0f) * dt * dt;
            if (world.ground)
            {
                p.y = std::max(p.y, world.ground(p.x, p.z) + m_radius[k] * 0.6f);
            }
            if (wet && sink > 0.0f)
            {
                p.y = std::min(p.y, surface - m_radius[k] * 0.3f);
            }
        }
        for (int pass = 0; pass < 4; ++pass)
        {
            for (size_t k = 0; k + 1 < m_deadPoints.size(); ++k)
            {
                const glm::vec3 d = m_deadPoints[k + 1] - m_deadPoints[k];
                const float length = glm::length(d);
                if (length > 1e-4f)
                {
                    const glm::vec3 fix = d * ((length - link) / length) * 0.5f;
                    m_deadPoints[k] += fix;
                    m_deadPoints[k + 1] -= fix;
                }
            }
        }
        spine = m_deadPoints;
        m_position = m_deadPoints.front();
    }
    else
    {
        m_deadPoints.clear();
        for (int k = 0; k < N; ++k)
        {
            spine[static_cast<size_t>(k)] = alongTrail(link * static_cast<float>(k));
            // Kept in the water, all of it, unless it means to come up -- and off the sea's floor: wherever its front has been (made
            // where it had to come down from, say), its body is never left standing up out of the sea, nor through the floor.
            glm::vec3& p = spine[static_cast<size_t>(k)];
            const float r = k < static_cast<int>(m_radius.size()) ? m_radius[static_cast<size_t>(k)] : 1.0f;
            if (world.ground)
            {
                const float floor = world.ground(p.x, p.z);
                float surface = 0.0f;
                if (m_surfaceUntil <= 0.0f && world.water && world.water(glm::vec3(p.x, floor + 0.5f, p.z), surface) > 0.0f)
                {
                    p.y = std::min(p.y, surface - r * 1.6f); // (with its spines and fins under too)
                }
                p.y = std::max(p.y, floor + r * 0.6f);
            }
        }
    }
    // And its body a smooth curve along the way its front went, not that way's corners: wherever its front turned or rose sharply -- a
    // rush begun, a push off something built -- the corner ran down it joint by joint as it swam, each one jerked round it in turn.
    // (Each joint drawn towards between its neighbours, a few times over; its head where it is.)
    if (!dead)
    {
        for (int pass = 0; pass < 3; ++pass)
        {
            std::vector<glm::vec3> smoothed = spine;
            for (int k = 1; k + 1 < N; ++k)
            {
                smoothed[static_cast<size_t>(k)] =
                    glm::mix(spine[static_cast<size_t>(k)], (spine[static_cast<size_t>(k - 1)] + spine[static_cast<size_t>(k + 1)]) * 0.5f, 0.5f);
            }
            spine = std::move(smoothed);
        }
        // And eased from where each joint was a moment ago -- the end of its tail most, with nothing past it to round it -- each link
        // then kept its own length from the one before.
        if (m_spineEased.size() != spine.size())
        {
            m_spineEased = spine;
        }
        for (int k = 1; k < N; ++k)
        {
            const float u = static_cast<float>(k) / static_cast<float>(N - 1);
            glm::vec3& p = m_spineEased[static_cast<size_t>(k)];
            p += (spine[static_cast<size_t>(k)] - p) * std::min(dt * glm::mix(14.0f, 6.0f, u), 1.0f);
            const glm::vec3& prior = m_spineEased[static_cast<size_t>(k - 1)];
            const glm::vec3 along = p - prior;
            const float have = glm::length(along);
            const float want = glm::distance(spine[static_cast<size_t>(k)], spine[static_cast<size_t>(k - 1)]);
            if (have > 1e-4f)
            {
                p = prior + along * (want / have);
            }
        }
        m_spineEased[0] = spine[0];
        spine = m_spineEased;
    }
    else
    {
        m_spineEased.clear();
    }
    std::vector<glm::mat4> frames(static_cast<size_t>(N));
    // Each joint's way along it from both its neighbours, and eased from the frame before: the path its front took is a run of straight
    // pieces, and taken from one neighbour alone the way turned at once wherever a joint passed a corner of it -- and the swimming wave,
    // pushed along that way's side, jumped with it: the body twitched (you, 2026-11-17).
    if (m_spineBack.size() != static_cast<size_t>(N))
    {
        m_spineBack.assign(static_cast<size_t>(N), glm::vec3(0.0f));
    }
    for (int k = 0; k < N; ++k)
    {
        const glm::vec3& here = spine[static_cast<size_t>(k)];
        const glm::vec3 ahead = spine[static_cast<size_t>(std::max(k - 1, 0))];
        const glm::vec3 behind = spine[static_cast<size_t>(std::min(k + 1, N - 1))];
        glm::vec3 back = behind - ahead;
        back = glm::length(back) > 1e-4f ? glm::normalize(back) : Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3& eased = m_spineBack[static_cast<size_t>(k)];
        eased = glm::length(eased) < 0.5f ? back : glm::normalize(eased + (back - eased) * std::min(dt * 12.0f, 1.0f) + glm::vec3(1e-6f));
        frames[static_cast<size_t>(k)] = Frame(here, eased, m_roll);
    }
    // Its swimming: waves down it from side to side, growing towards the tail -- an eel's wider, an armoured one's stiffer.
    const float effort = std::clamp(speed / std::max(m_speed, 0.5f), 0.2f, 1.0f);
    const float amplitude = m_form == Form::Eel ? 0.05f : m_form == Form::Armoured ? 0.028f : 0.04f;
    for (int k = 0; k < N; ++k)
    {
        const float t = static_cast<float>(k) / static_cast<float>(N - 1);
        const float wave = std::sin(m_swimPhase * kTau - t * 6.0f) * effort;
        const glm::vec3 right = glm::vec3(frames[static_cast<size_t>(k)][0]);
        if (!dead)
        {
            frames[static_cast<size_t>(k)][3] += glm::vec4(right * (amplitude * L * std::pow(t, 0.8f) * wave), 0.0f);
        }
        m_skinning[static_cast<size_t>(k)] = frames[static_cast<size_t>(k)] * m_restInverse[static_cast<size_t>(k)];
    }
    // Its jaw (you, 2026-11-16: its mouth never opened): nearly shut going about, breathing water through it; parted as it hunts,
    // working; gathering for a strike a little open, then wide as it comes, snapping shut as it bites; dead, slack.
    {
        float wanted = 0.12f + 0.05f * std::sin(time * 0.6f + static_cast<float>(m_seed % 11u));
        if (dead)
        {
            wanted = 0.45f;
        }
        else if (m_mode == Mode::Strike)
        {
            const bool bit = m_shownOnly ? m_strike > 0.75f : m_struck;
            wanted = bit ? 0.0f : m_strike < 0.2f ? 0.3f : 1.0f;
        }
        else if (m_mode == Mode::Hunt)
        {
            wanted = 0.38f + 0.14f * std::sin(time * 1.4f + static_cast<float>(m_seed % 7u));
        }
        else if (m_mode == Mode::Notice || m_mode == Mode::Approach)
        {
            wanted = 0.25f;
        }
        if (m_gapeHeld >= 0.0f)
        {
            wanted = m_gapeHeld;
        }
        m_gape += (wanted - m_gape) * std::min(dt * (wanted < m_gape ? 10.0f : 3.0f), 1.0f);
        const glm::mat4 jaw = glm::translate(glm::mat4(1.0f), m_jawHinge) * glm::rotate(glm::mat4(1.0f), -m_gape * 0.62f, glm::vec3(1.0f, 0.0f, 0.0f)) *
                              glm::translate(glm::mat4(1.0f), -m_jawHinge);
        if (m_skinning.size() > static_cast<size_t>(N))
        {
            m_skinning[static_cast<size_t>(N)] = m_skinning[0] * jaw;
        }
    }
    m_spineAt.resize(static_cast<size_t>(N));
    for (int k = 0; k < N; ++k)
    {
        m_spineAt[static_cast<size_t>(k)] = glm::vec3(frames[static_cast<size_t>(k)][3]);
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
    }
    if (m_eyeEntity.IsValid())
    {
        AABB eyeBounds;
        SkinMesh(m_eyeRest, m_eyeWeights, m_skinning, m_eyeSkinned, eyeBounds);
        recentre(m_eyeSkinned, eyeBounds);
        m_meshes.UpdateDynamic(m_eyeMesh, m_eyeSkinned, eyeBounds);
        place(m_eyeEntity);
    }
    // Its head and its jaws, just under and ahead of it.
    const glm::mat4& headFrame = frames[0];
    m_head = glm::vec3(headFrame[3]) + glm::vec3(headFrame[1]) * (m_radius[0] * 0.2f);
    m_holdWanted = glm::vec3(headFrame[3]) - glm::vec3(headFrame[1]) * (m_radius[0] * 0.15f) - glm::vec3(headFrame[2]) * (m_radius[0] * 0.3f);
    m_holdPoint = m_holdWanted;
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

void Giant::Steer(const GiantWorld& world, const glm::vec3& goal, float speed, float dt, const glm::vec3* faceTo)
{
    glm::vec3 wanted = goal;
    const glm::vec2 fromMiddle = glm::vec2(m_position.x, m_position.z) - world.middle;
    if (glm::length(fromMiddle) > world.reach)
    {
        wanted = glm::vec3(world.middle.x, m_position.y, world.middle.y);
    }
    // Getting no nearer -- buildings in the way, felt for and pushed off, round and round against them (you, 2026-11-16: it stood a
    // hundred metres off and came no closer): it finds a way round, somewhere clear off to one side that is nearer where it is going,
    // and goes there first.
    m_steerClock += dt;
    if (world.builtUp && m_kind == GiantKind::Walker)
    {
        const float distance = Flat(m_position, wanted);
        if (m_steerClock >= m_detourUntil)
        {
            if (distance < m_bestDistance - 4.0f || m_steerClock - m_progressSince > 30.0f)
            {
                m_bestDistance = distance;
                m_progressSince = m_steerClock;
            }
            else if (m_steerClock - m_progressSince > 6.0f && distance > 20.0f)
            {
                const float out = m_bodyHeight * 0.62f + m_bodyRadii.z + 12.0f;
                const float tall = std::max(m_bodyHeight - m_bodyRadii.y * 1.4f - 4.0f, 0.0f); // (what it steps over)
                float best = 1.0e9f;
                for (int k = 0; k < 16; ++k)
                {
                    const float a = kTau * static_cast<float>(k) / 16.0f;
                    const glm::vec3 at = m_position + glm::vec3(std::sin(a), 0.0f, std::cos(a)) * (out * 1.4f);
                    const glm::vec3 half = (m_position + at) * 0.5f;
                    if (world.builtUp(at.x, at.z) > tall || world.builtUp(half.x, half.z) > tall ||
                        glm::length(glm::vec2(at.x, at.z) - world.middle) > world.reach)
                    {
                        continue;
                    }
                    const float score = Flat(at, wanted);
                    if (score < best)
                    {
                        best = score;
                        m_detour = at;
                    }
                }
                if (best < 1.0e8f)
                {
                    m_detourUntil = m_steerClock + 10.0f;
                    m_goal = "finding a way round the buildings";
                }
                m_progressSince = m_steerClock;
                m_bestDistance = distance;
            }
        }
        if (m_steerClock < m_detourUntil)
        {
            if (Flat(m_position, m_detour) < 6.0f)
            {
                m_detourUntil = m_steerClock;
            }
            else
            {
                wanted = m_detour;
            }
        }
    }
    float bearing = std::atan2(-(wanted.x - m_position.x), -(wanted.z - m_position.z));
    // Round what is in its way rather than through it: a building its body would not clear (the strider), anything built (the
    // leviathan) -- felt for ahead, a little to each side, the clearer way taken.
    if (world.builtUp)
    {
        // (A strider's feet stand out round it as far as it is tall and more, felt for that far. What its body clears it steps over --
        // its feet put down clear of it (FootFor), its legs kept off its walls, each step lifted over it; in among buildings it could
        // not walk round, it got no nearer anybody: you, 2026-11-16. What is taller than its belly is in its way.)
        const float lookAhead = m_kind == GiantKind::Walker ? m_bodyHeight * 0.62f + m_bodyRadii.z + 6.0f : m_size * 0.4f + 8.0f;
        const float clears = m_kind == GiantKind::Walker ? std::max(m_bodyHeight - m_bodyRadii.y * 1.4f - 4.0f, 0.0f) : 0.0f;
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
    // A leviathan: what is solid ahead of its head -- rock, the floor rising, anything built -- felt for along where it is going, a body
    // length and a half on; and the clearest way round it taken, up or down or to either side (you, 2026-11-16: it swam into things,
    // and through them).
    if (m_kind == GiantKind::Swimmer && world.clear && !m_spineAt.empty())
    {
        const glm::vec3 going = glm::normalize(wanted - m_head + glm::vec3(1e-4f));
        const float feel = std::max(m_radius.empty() ? 4.0f : m_radius.front() * 4.0f, 12.0f);
        if (!world.clear(m_head, m_head + going * feel))
        {
            const glm::vec3 side = glm::normalize(glm::cross(going, glm::vec3(0.0f, 1.0f, 0.0f)) + glm::vec3(1e-4f));
            const glm::vec3 up = glm::cross(side, going);
            for (const glm::vec3& way : {up, side, -side, -up, glm::normalize(up + side), glm::normalize(up - side)})
            {
                const glm::vec3 tryWay = glm::normalize(going + way * 1.2f);
                if (world.clear(m_head, m_head + tryWay * feel))
                {
                    wanted = m_head + tryWay * glm::distance(m_head, wanted);
                    bearing = std::atan2(-(wanted.x - m_position.x), -(wanted.z - m_position.z));
                    break;
                }
            }
        }
    }
    // (Quicker round in a rush, as anything is going for something.)
    const float turnRate = m_kind == GiantKind::Walker ? 0.45f : m_mode == Mode::Strike ? 1.0f : 0.5f;
    // A leviathan does not turn on the spot: something close behind it, it swings out wide and comes round in a loop, its body going on
    // the way its head went -- turned round where it lay, its head spun and its body knotted up behind it (you, 2026-11-22: its head
    // spinning round in place, flipping round, twisting).
    if (m_kind == GiantKind::Swimmer)
    {
        const float off = Wrap(bearing - m_yaw);
        const float radius = speed / std::max(turnRate, 0.1f);
        if (std::abs(off) > 1.2f && Flat(m_head, wanted) < radius * 1.8f)
        {
            bearing = Wrap(m_yaw + (off > 0.0f ? 0.35f : -0.35f));
        }
    }
    // Sidelong (a strider round somebody, facing them): turned to them, going the way it is going whichever way it faces -- its legs
    // stepping sideways under it.
    const bool sidelong = faceTo != nullptr && m_kind == GiantKind::Walker && Flat(m_position, *faceTo) > 1.0f;
    const float faceBearing = sidelong ? std::atan2(-(faceTo->x - m_position.x), -(faceTo->z - m_position.z)) : bearing;
    const float turn = std::clamp(Wrap(faceBearing - m_yaw), -turnRate * dt, turnRate * dt);
    m_yaw = Wrap(m_yaw + turn);
    const float facing = std::cos(Wrap(bearing - m_yaw));
    // (A leviathan keeps going as it turns, as anything swimming does: slowed to a third, it turned nearly where it was.)
    const float pace = sidelong ? speed * 0.75f : speed * glm::mix(m_kind == GiantKind::Swimmer ? 0.8f : 0.35f, 1.0f, std::max(facing, 0.0f));
    const glm::vec3 ahead = Yawed(sidelong ? bearing : m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 velocity = ahead * pace;
    if (m_kind == GiantKind::Swimmer)
    {
        // Up and down slowly, its depth eased: never a head bobbing up and straight back down.
        velocity.y = std::clamp((wanted.y - m_position.y) * 0.35f, -1.6f, 1.6f);
    }
    // A leviathan's head pushed off anything built that it would swim into: a ring round it at its own width and more, felt for, and
    // pushed away from what is there (its body follows its head's way) -- you, 2026-11-16: it went through a building.
    if (m_kind == GiantKind::Swimmer && world.builtUp)
    {
        glm::vec3 push{0.0f};
        const float width = std::max(m_radius.empty() ? 3.0f : m_radius.front() * 2.5f, 6.0f);
        for (int k = 0; k < 8; ++k)
        {
            const float a = kTau * static_cast<float>(k) / 8.0f;
            const glm::vec3 way{std::sin(a), 0.0f, std::cos(a)};
            const glm::vec3 at = m_head + way * width;
            const float top = world.builtUp(at.x, at.z) + (world.ground ? world.ground(at.x, at.z) : 0.0f);
            if (world.builtUp(at.x, at.z) > 0.0f && m_head.y < top + width * 0.5f)
            {
                push -= way;
            }
        }
        if (glm::length(push) > 0.1f)
        {
            velocity += glm::normalize(push) * m_speed * 1.5f;
            velocity.y += m_speed * 0.4f; // (and up, over it where it can)
        }
    }
    // And pushed off a building its feet would come down round.
    if (m_kind == GiantKind::Walker && world.builtUp)
    {
        glm::vec3 push{0.0f};
        const float clearOf = m_bodyHeight * 0.62f;
        const float over = std::max(m_bodyHeight - m_bodyRadii.y * 1.4f - 4.0f, 0.0f); // (what it steps over)
        for (int k = 0; k < 8; ++k)
        {
            const float a = kTau * static_cast<float>(k) / 8.0f;
            const glm::vec3 way{std::sin(a), 0.0f, std::cos(a)};
            const glm::vec3 at = m_position + way * clearOf;
            if (world.builtUp(at.x, at.z) > over)
            {
                push -= way;
            }
        }
        if (glm::length(push) > 0.1f)
        {
            velocity += glm::normalize(push) * std::max(pace, m_speed * 0.5f);
        }
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
            // (Up to just under the surface surfacing -- and rushing at somebody, or holding them: a big one kept its whole bulk under
            // the water could never reach anybody swimming near the top.)
            const bool up = m_surfaceUntil > 0.0f || m_mode == Mode::Strike || m_mode == Mode::Hold;
            const float top = up ? surface - r * 0.35f : surface - r - 1.5f;
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
    if (mode != Mode::Strike && mode != Mode::Hold)
    {
        m_reachingIn = false;
    }
    if (m_mode != mode)
    {
        // (Logged, as a creature's mind is: what it turned to, and why.)
        PRED_LOG_DEBUG(AI, "Giant: {} -> {} ({})", ModeName(m_mode), ModeName(mode), goal);
        // (Waiting about a place: from when it began, through any stamping or feeling at it in between.)
        if (mode == Mode::Linger && m_mode != Mode::Strike)
        {
            m_waitSince = time;
        }
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
            // (How lit they are matters to a strider's eyes; a leviathan's senses are the water's, and a torch is nothing to it -- you,
            // 2026-11-17.)
            const float lit = m_kind == GiantKind::Swimmer ? 1.0f : 0.4f + 0.6f * who.light;
            const float rate = (1.0f - away / range) * (0.35f + 0.65f * moving) * (who.height < 1.3f ? 0.55f : 1.0f) * lit;
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
    // Being looked at, from a way off, a strider stands dead still -- its eyes dimmed, nothing of it moving -- for as long as it is
    // looked at; it comes on again the moment nobody is (you, 2026-11-15: something frightening). Not once it is angry.
    m_frozen = false;
    // Close to somebody it knows of -- or where they went in, out of sight -- a strider bends right down to look at them, or in at the
    // windows (you, 2026-11-16).
    m_peekWanted = 0.0f;
    if (m_kind == GiantKind::Walker && m_mode != Mode::Dead && m_mode != Mode::Hold && m_mode != Mode::Strike)
    {
        const glm::vec3 near = target != nullptr ? target->feet : m_targetAt;
        const bool known = target != nullptr || m_mode == Mode::Linger;
        if (known && Flat(m_position, near) < m_bodyHeight * 1.6f && m_mode != Mode::Roam)
        {
            m_peekWanted = 1.0f;
        }
    }
    // (Never once it has been shot -- you, 2026-11-16: shot, it stood there looked at, and did nothing.)
    if (m_kind == GiantKind::Walker && target != nullptr && m_annoyed < 0.3f && time - m_lastHurt > 90.0f &&
        (m_mode == Mode::Approach || m_mode == Mode::Notice || m_mode == Mode::Roam))
    {
        const glm::vec3 eye = target->feet + glm::vec3(0.0f, target->height * 0.9f, 0.0f);
        const glm::vec3 to = m_head - eye;
        const float away = glm::length(to);
        const bool watched = away > 40.0f && glm::dot(to / std::max(away, 1e-3f), glm::normalize(target->look + glm::vec3(1e-5f))) > std::cos(glm::radians(14.0f));
        if (watched && time > m_freezeDecidedUntil)
        {
            // (Its own way, from its seed: about half of them freeze at all, and those two times in three.)
            uint32_t h = m_seed * 2654435761u + static_cast<uint32_t>(++m_freezeLooks) * 40503u;
            h ^= h >> 15;
            h *= 0x2C1B3C6Du;
            h ^= h >> 12;
            const bool freezer = ((m_seed * 2246822519u) >> 28) < 9u;
            m_freezeNow = freezer && (h % 100u) < 65u;
            m_freezeDecidedUntil = time + 40.0f;
        }
        m_frozen = watched && m_freezeNow;
        if (m_frozen)
        {
            Enter(Mode::Notice, time, "standing dead still while it is looked at");
            m_modeSince = time; // (held here as long as it is watched)
            m_velocity *= std::max(1.0f - dt * 4.0f, 0.0f);
            m_lookAt = eye;
            m_shownAt = m_position;
            m_shownYaw = m_yaw;
            return;
        }
    }
    const bool seenNow = target != nullptr && time - m_lastSeen < 0.5f;
    // (Not out of a strike at them: feeling at the way in, or reaching in, it was called off the moment it began.)
    if (m_mode != Mode::Hold && m_mode != Mode::Strike && target != nullptr && target->roofed)
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
        // Back to where it lost somebody, to see whether they have come out: there, it stands and looks a while.
        if (m_comeBackAt >= 0.0f && time >= m_comeBackAt)
        {
            m_roamTo = glm::vec3(m_comeBackTo.x, m_position.y, m_comeBackTo.z);
            m_haveRoam = true;
            m_goal = "coming back to see whether it has come out";
            if (Flat(m_position, m_comeBackTo) < m_bodyHeight * 0.8f)
            {
                m_comeBackAt = -2.0f; // (once)
                m_targetAt = m_comeBackTo;
                Enter(Mode::Notice, time, "back where it went in; looking for it");
            }
        }
        else if (!m_haveRoam || Flat(m_position, m_roamTo) < 15.0f)
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
        // (A leviathan does not stop dead in the water at what it has seen -- you, 2026-11-17: it stopped in place: it glides on,
        // slowing, turning its head and then itself towards it. A strider stops where it stands.)
        if (m_kind == GiantKind::Swimmer)
        {
            const glm::vec3 to = m_targetAt - m_head;
            const glm::vec3 on = m_position + glm::normalize(glm::vec3(to.x, 0.0f, to.z) + glm::vec3(1e-4f, 0.0f, 0.0f)) * std::max(m_size * 0.6f, 20.0f);
            Steer(world, glm::vec3(on.x, m_position.y, on.z), m_speed * 0.55f, dt);
        }
        else
        {
            m_velocity *= std::max(1.0f - dt * 1.5f, 0.0f);
            m_position += m_velocity * dt;
        }
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
        // Near, a strider does not walk straight in: it comes round them as it closes, sidelong, its eyes on them -- one way and then,
        // now and then, the other (you, 2026-11-16: wrapping round you).
        const float apart = Flat(m_position, m_targetAt);
        bool circling = false;
        if (m_kind == GiantKind::Walker && apart < reachOver * 2.6f && apart > reachOver * 1.2f)
        {
            if (time >= m_circleUntil)
            {
                m_circleSide = (static_cast<uint32_t>(time * 7.0f) + m_seed) % 3u == 0u ? m_circleSide : -m_circleSide;
                m_circleUntil = time + 5.0f + static_cast<float>(m_seed % 5u);
            }
            const glm::vec3 from = glm::normalize(glm::vec3(m_position.x - m_targetAt.x, 0.0f, m_position.z - m_targetAt.z) + glm::vec3(1e-4f, 0.0f, 0.0f));
            const glm::vec3 round = glm::angleAxis(0.75f * m_circleSide, glm::vec3(0.0f, 1.0f, 0.0f)) * from;
            const glm::vec3 slant = m_targetAt + round * (apart * 0.75f);
            // (Only round open ground: among buildings it went round and round against them, getting nowhere.)
            const glm::vec3 halfway = (m_position + slant) * 0.5f;
            circling = !world.builtUp || (world.builtUp(slant.x, slant.z) <= 0.0f && world.builtUp(halfway.x, halfway.z) <= 0.0f);
            if (circling)
            {
                Steer(world, slant, m_speed * 1.5f, dt, &m_targetAt);
                m_goal = "coming round it, closing in";
            }
        }
        if (!circling)
        {
            Steer(world, goal, m_speed * (m_kind == GiantKind::Walker ? 1.7f : 1.4f), dt);
        }
        // (A leviathan not up to them first: from a way off, seen, it turns away to go round out of sight -- Hunt.)
        if (Flat(m_kind == GiantKind::Walker ? m_position : m_head, m_targetAt) < (m_kind == GiantKind::Walker ? reachOver * 1.6f : 45.0f))
        {
            Enter(Mode::Hunt, time, "over it, sizing it up");
            // (Not long: a few seconds of looking, sooner the rougher it is -- then it reaches for them.)
            m_huntUntil = time + (m_kind == GiantKind::Walker ? 3.0f : 5.0f) * (1.0f - 0.6f * m_annoyed);
        }
        break;
    }
    case Mode::Hunt:
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
                Steer(world, m_targetAt, m_speed * 0.8f, dt, &m_targetAt);
            }
            else
            {
                // Over them it is not still: it shifts round them, slowly, sidelong, keeping its eyes on them -- cutting off the way
                // they would run.
                const glm::vec3 from = glm::normalize(glm::vec3(m_position.x - m_targetAt.x, 0.0f, m_position.z - m_targetAt.z) + glm::vec3(1e-4f, 0.0f, 0.0f));
                const glm::vec3 round = glm::angleAxis(0.5f * m_circleSide, glm::vec3(0.0f, 1.0f, 0.0f)) * from;
                Steer(world, m_targetAt + round * (reachOver * 0.6f), m_speed * 0.35f, dt, &m_targetAt);
            }
        }
        else
        {
            // Dark (its lights out: Animate), it does not circle where it can be watched (you, 2026-11-16): it swims off, out of
            // sight into the murk; out there, unseen, it goes round to behind where they are looking; and from behind it comes in,
            // fast, and goes for them from close. Missed, it goes off again.
            const float far = std::max(48.0f, m_size * 0.9f);
            const glm::vec3 flatLook = glm::normalize(glm::vec3(target->look.x, 0.0f, target->look.z) + glm::vec3(1e-4f, 0.0f, 0.0f));
            const glm::vec3 fromThem = glm::vec3(m_head.x - m_targetAt.x, 0.0f, m_head.z - m_targetAt.z);
            const float apart = glm::length(fromThem);
            const glm::vec3 outward = apart > 0.5f ? fromThem / apart : -flatLook;
            const float depth = -6.0f;
            // Already close, or already where they are not looking -- to one side, behind -- there is nothing to go round for: it
            // comes straight in (you, 2026-11-17: close to it, its going round out of sight made no sense).
            if (m_flankPhase < 2)
            {
                const bool watched = glm::dot(flatLook, outward) > std::cos(glm::radians(55.0f));
                if (apart < far * 0.9f || !watched)
                {
                    m_flankPhase = 2;
                    m_flankSince = time;
                }
            }
            if (m_flankPhase == 0)
            {
                // Away, straight out from them, to well past what can be seen down here.
                Steer(world, m_targetAt + outward * (far + 12.0f) + glm::vec3(0.0f, depth, 0.0f), m_speed * 1.3f, dt);
                m_goal = "swimming off into the murk";
                if (apart > far || time - m_modeSince > 16.0f)
                {
                    m_flankPhase = 1;
                    m_flankSince = time;
                }
            }
            else if (m_flankPhase == 1)
            {
                // Round, out there, to behind them: a little further round each moment, the shorter way.
                const glm::vec3 behind = -flatLook;
                const float turn = std::atan2(glm::dot(glm::cross(outward, behind), glm::vec3(0.0f, 1.0f, 0.0f)), glm::dot(outward, behind));
                const float step = std::clamp(turn, -0.6f, 0.6f);
                const glm::vec3 round = glm::angleAxis(step, glm::vec3(0.0f, 1.0f, 0.0f)) * outward;
                Steer(world, m_targetAt + round * far + glm::vec3(0.0f, depth, 0.0f), m_speed * 1.4f, dt);
                m_goal = "going round them, out of sight";
                if (std::abs(turn) < glm::radians(35.0f) || time - m_flankSince > 14.0f)
                {
                    m_flankPhase = 2;
                    m_flankSince = time;
                }
            }
            else if (time < m_runOnUntil)
            {
                // Just past them, missed: on the way it was going a moment, before it comes round.
                const glm::vec3 heading = Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f);
                Steer(world, m_position + heading * 60.0f + glm::vec3(0.0f, m_targetAt.y - 4.0f - m_position.y, 0.0f), m_speed * 1.6f, dt);
                m_goal = "past them, coming round";
                m_huntUntil = std::max(m_huntUntil, m_runOnUntil);
            }
            else
            {
                // In, from behind and a little below, fast and dark -- and once close, its rush.
                Steer(world, m_targetAt + glm::vec3(0.0f, -2.0f, 0.0f), m_speed * 2.2f, dt);
                m_goal = "coming in at them from behind, out of the dark";
                if (glm::distance(m_head, m_targetAt) < 26.0f || time - m_flankSince > 12.0f)
                {
                    m_huntUntil = std::min(m_huntUntil, time);
                    m_nextStrikeAt = std::min(m_nextStrikeAt, time);
                }
                else
                {
                    m_huntUntil = std::max(m_huntUntil, time + 0.5f); // (not its rush from out there: only once it is close)
                }
            }
            if (m_flankPhase < 2)
            {
                m_huntUntil = std::max(m_huntUntil, time + 0.5f);
            }
        }
        // (A strider reaches only for somebody it can reach -- it stabbed at them from too far, at nothing.)
        const bool inReach = m_kind == GiantKind::Swimmer || Flat(m_position, m_targetAt) < reachOver;
        if (time >= m_huntUntil && time >= m_nextStrikeAt && inReach)
        {
            Enter(Mode::Strike, time, m_kind == GiantKind::Swimmer ? "goes for it" : m_strands.empty() ? "raising a foot to stamp on it" : "reaching down for it");
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
            if (rushing && !m_rushStarted)
            {
                m_rushStarted = true;
                m_rushHeard = true;
            }
            // Never through a building, nor rock: something solid between it and them, it gives the rush up and goes off and round
            // again (you, 2026-11-16: it came at me through a building).
            if (rushing && world.clear && !m_struck && !world.clear(m_head, m_strikeAt))
            {
                m_strike = 1.0f;
            }
            // (At where they will be, its head to them -- not at a point six metres past them, through them: you, 2026-11-16.)
            if (target != nullptr && rushing)
            {
                const float closing = std::max(glm::length(m_velocity), 1.0f);
                m_strikeAt += target->velocity * std::min(glm::distance(m_head, m_strikeAt) / closing, 1.5f);
            }
            Steer(world, m_strikeAt, m_speed * (rushing ? 3.2f : 0.8f), dt);
            // Gone by them -- dodged -- it does not go on and away: it turns now and comes again.
            if (rushing && target != nullptr)
            {
                const float near = glm::distance(m_head, target->feet + glm::vec3(0.0f, 0.9f, 0.0f));
                if (near < m_rushNearest)
                {
                    m_rushNearest = near;
                    m_passedAt = time;
                }
                else if (m_rushNearest < 14.0f && near > m_rushNearest + 4.0f && !m_struck)
                {
                    m_strike = 1.0f;
                }
            }
        }
        else
        {
            m_velocity *= std::max(1.0f - dt * 3.0f, 0.0f);
            m_position += m_velocity * dt;
        }
        // (A leviathan's jaws can close from halfway into its wind-up: it is already coming.)
        const float landsAt = (m_kind == GiantKind::Walker ? windup : windup * 0.5f) / duration;
        if (!m_struck && m_strike >= landsAt)
        {
            m_struck = true;
            const glm::vec3 reachedTo = m_kind == GiantKind::Walker ? m_strikeAt : m_holdWanted;
            // (A leviathan's reach as wide as its jaws: a big one's mouth takes in far more than five metres of water.)
            const float radius = m_kind == GiantKind::Walker ? 3.2f : std::max(5.0f, (m_radius.empty() ? 2.0f : m_radius.front()) * 1.2f + 2.0f);
            const GiantSensed* caught = nullptr;
            float nearest = radius;
            for (const GiantSensed& who : people)
            {
                // (Under a roof only as it reaches in through a doorway: just inside it, and in line with it.)
                const bool inReach = m_reachingIn && Flat(who.feet, m_reachDoor) < 8.0f;
                if (!who.alive || (who.roofed && !inReach))
                {
                    continue;
                }
                const float off = m_kind == GiantKind::Walker ? Flat(who.feet, reachedTo) : glm::distance(who.feet + glm::vec3(0.0f, 0.9f, 0.0f), reachedTo);
                // (Nothing solid between: never through a wall, a roof, the shuttle's hull.)
                const glm::vec3 chest = who.feet + glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::vec3 from = who.roofed ? m_reachDoor + glm::vec3(0.0f, 1.2f, 0.0f)
                                       : m_kind == GiantKind::Walker ? chest + glm::vec3(0.0f, 6.0f, 0.0f) : m_head;
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
                // (The strider takes them up; a leviathan never takes anybody -- you, 2026-11-16 -- it bites in passing.)
                // (Taken only by its strands -- one with none brings its foot down on them instead: you, 2026-11-16.)
                const bool takes = m_kind == GiantKind::Walker && !m_strands.empty();
                if (takes)
                {
                    blow.grab = true;
                    blow.damage = m_kind == GiantKind::Swimmer ? 20.0f : 0.0f;
                    m_holding = caught->id;
                    m_holdSince = time;
                    m_hurtHolding = 0.0f;
                    m_holdPoint = caught->feet + glm::vec3(0.0f, 1.0f, 0.0f);
                    m_pullOutUntil = caught->roofed ? time + 1.6f : -1.0f;
                    m_reachInside = caught->feet;
                    m_nextBiteAt = time + 1.2f;
                    m_eats = false;
                    m_lowering = false;
                    Enter(Mode::Hold, time, m_kind == GiantKind::Walker ? "has it; lifting it up to look at" : "has it in its jaws; taking it down");
                }
                else
                {
                    blow.damage = std::clamp(55.0f + 70.0f * m_annoyed + 25.0f * (1.0f - nearest / radius), 30.0f, 140.0f);
                    // (A strider's foot stamped down on them: the ground shaking with it.)
                    if (m_kind == GiantKind::Walker && m_footfalls.size() < 16)
                    {
                        m_footfalls.push_back(reachedTo);
                    }
                    const glm::vec3 away = caught->feet - reachedTo;
                    blow.push = (glm::length(away) > 0.1f ? glm::normalize(away) : Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f)) * 9.0f + glm::vec3(0.0f, 3.0f, 0.0f);
                }
                m_blows.push_back(blow);
            }
        }
        if (m_strike >= 1.0f && m_mode == Mode::Strike)
        {
            m_rushStarted = false;
            m_strike = 0.0f;
            // (A leviathan, missed: carried on past them by its rush, it goes round again from out there -- not all the way off into the
            // murk first: you, 2026-11-17, not rarer.)
            m_flankPhase = 1;
            m_flankSince = time;
            m_rushNearest = 1.0e9f;
            // (On past them first, the way the rush took it, then round: turned straight back the moment it missed, it went to and fro
            // where it was -- you, 2026-11-22.)
            m_runOnUntil = m_kind == GiantKind::Swimmer ? time + 2.2f : -1.0f;
            // (A leviathan missed comes round again soon: a second or two, not another long circling.)
            m_nextStrikeAt = time + (m_kind == GiantKind::Swimmer ? glm::mix(2.5f, 1.0f, m_annoyed) : glm::mix(5.0f, 2.0f, m_annoyed));
            if (target != nullptr && target->roofed)
            {
                m_nextStrikeAt = time + 9.0f; // (at the way in they went by, now and then: not on and on)
            }
            Enter(target != nullptr ? Mode::Hunt : Mode::Linger, time, target != nullptr ? "missed; after it again" : "looking for where it went");
            m_huntUntil = time + (m_kind == GiantKind::Swimmer ? 1.5f : glm::mix(4.0f, 1.0f, m_annoyed));
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
            m_lowering = false;
            m_hunger = 0.0f;
            m_nextStrikeAt = time + 10.0f;
            Enter(target != nullptr ? Mode::Hunt : Mode::Roam, time, "finished with what it held");
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
            if (heldFor < 5.0f)
            {
                m_goal = "turning it over in front of its eyes";
                break;
            }
            // Then it does something with them (you, 2026-11-16: not lifting them up only to put them down again): eats them --
            // hungry, angry, or having thrown them once already -- or throws them (you, 2026-11-17: throw them, rather than put down).
            if (!m_eats && !m_lowering && heldFor < 5.0f + dt * 1.5f)
            {
                m_eats = m_hunger > 0.5f || m_annoyed > 0.45f || m_slams > 0;
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
            // Thrown: drawn back and up beside it, then swung round and flung away -- out across the ground, twenty-odd metres, hurt by
            // the throw and by the landing (the fall's own hurt, as from any height) -- and then it comes for them again.
            if (!m_lowering)
            {
                m_lowering = true;
                m_throwSince = time;
            }
            const float swung = time - m_throwSince;
            if (swung < kThrowWindup)
            {
                m_goal = "drawing it back to throw it";
                break;
            }
            // (Swung through first: just drawn back, the hold point is where it was drawn back to, and it let go from up there.)
            if (swung < kThrowWindup + 0.35f || (glm::distance(m_holdPoint, m_holdWanted) > 2.5f && swung < kThrowWindup + 1.6f))
            {
                m_goal = "throwing it";
                break;
            }
            Blow blow;
            blow.player = m_holding;
            blow.at = m_holdPoint;
            blow.damage = 10.0f;
            blow.push = Yawed(m_yaw - 0.35f) * glm::vec3(0.0f, 0.0f, -1.0f) * 13.0f + glm::vec3(0.0f, 3.0f, 0.0f);
            blow.release = true;
            m_blows.push_back(blow);
            ++m_slams;
            m_holding = -1;
            m_lowering = false;
            m_throwSince = -1.0f;
            m_nextStrikeAt = time + 3.0f;
            Enter(Mode::Hunt, time, "threw it away; coming for it again");
            m_huntUntil = time + 1.5f;
            break;
        }
        // (A leviathan holds nobody: let go at once, should it ever be holding somebody.)
        Drop(time, "it does not hold anybody");
        break;
    }
    case Mode::Linger:
    {
        m_lookAt = m_targetAt + glm::vec3(0.0f, 2.0f, 0.0f);
        if (m_kind == GiantKind::Swimmer)
        {
            // Gone up out of the water: it does not go round and round under where they went, as if it could still have them (you,
            // 2026-11-22: it swam in circles under the platform, trying to get me): it draws off, slowly, down into the murk a way
            // off, and waits there, turning over -- and gives it up, as before, after a while.
            const glm::vec3 away = glm::normalize(glm::vec3(m_position.x - m_targetAt.x, 0.0f, m_position.z - m_targetAt.z) + glm::vec3(1e-4f, 0.0f, 0.0f));
            const float drift = time * 0.06f + static_cast<float>(m_seed % 7u);
            const glm::vec3 off = glm::angleAxis(0.6f * std::sin(drift), glm::vec3(0.0f, 1.0f, 0.0f)) * away * std::max(60.0f, m_size * 1.5f);
            Steer(world, m_targetAt + off + glm::vec3(0.0f, -12.0f, 0.0f), m_speed * 0.5f, dt);
            m_goal = "drawn off, waiting in the murk";
        }
        else if (m_strands.empty() && Flat(m_position, m_targetAt) < m_bodyHeight * 1.2f)
        {
            // With nothing to reach in with, it does not stamp at the building (you, 2026-11-17: that made no sense, and its leg went
            // through the building): it goes slowly round outside where they went in, bent down, looking in -- now from one side, now
            // another -- a step or two at a time, stopping to look.
            if (time >= m_nextStrikeAt || Flat(m_lingerSpot, m_targetAt) > m_bodyHeight * 1.5f)
            {
                const glm::vec3 from = glm::normalize(glm::vec3(m_position.x - m_targetAt.x, 0.0f, m_position.z - m_targetAt.z) + glm::vec3(1e-4f, 0.0f, 0.0f));
                const float swing = (((m_seed + static_cast<uint32_t>(time)) & 1u) != 0u ? 1.0f : -1.0f) * 0.7f;
                m_lingerSpot = m_targetAt + glm::angleAxis(swing, glm::vec3(0.0f, 1.0f, 0.0f)) * from * (m_bodyHeight * 0.75f);
                m_nextStrikeAt = time + 7.0f;
            }
            if (Flat(m_position, m_lingerSpot) > m_bodyHeight * 0.15f)
            {
                Steer(world, m_lingerSpot, m_speed * 0.35f, dt);
            }
            else
            {
                m_velocity *= std::max(1.0f - dt, 0.0f);
                m_position += m_velocity * dt;
            }
        }
        else if (Flat(m_position, m_targetAt) > m_bodyHeight * 0.6f)
        {
            Steer(world, m_targetAt, m_speed, dt);
        }
        else
        {
            m_velocity *= std::max(1.0f - dt, 0.0f);
            m_position += m_velocity * dt;
            // Waiting on them where they went in, it does not only wait (you, 2026-11-16: something more frightening): every so often
            // it feels at the way in with a strand, tapping at it. And somebody still just inside, in line with the way in -- a few metres in, nothing between -- it reaches
            // in after, through the doorway, and can take them out through it. Further in, or round a corner, a building is safe.
            if (!m_strands.empty() && time >= m_nextStrikeAt && time - m_modeSince > 4.0f && target != nullptr && target->roofed)
            {
                const glm::vec3 toward = glm::normalize(glm::vec3(m_position.x - m_targetAt.x, 0.0f, m_position.z - m_targetAt.z) + glm::vec3(1e-4f, 0.0f, 0.0f));
                const glm::vec3 door = m_targetAt + glm::vec3(0.0f, 1.2f, 0.0f);
                const glm::vec3 chest = target->feet + glm::vec3(0.0f, 1.0f, 0.0f);
                m_reachingIn = !m_strands.empty() && Flat(target->feet, m_targetAt) < 7.0f && std::abs(target->feet.y - m_targetAt.y) < 2.5f &&
                               (!world.clear || world.clear(door, chest));
                m_reachDoor = m_targetAt;
                m_strikeAt = m_reachingIn ? target->feet : m_targetAt + toward * 3.5f;
                m_strike = 0.0f;
                m_struck = false;
                Enter(Mode::Strike, time, m_reachingIn ? "reaching in after it with a strand" : "feeling at where it went in with a strand");
                m_nextStrikeAt = time + 9.0f;
            }
        }
        const bool back = target != nullptr && !target->roofed && (m_kind == GiantKind::Walker || target->inWater);
        if (back)
        {
            Enter(Mode::Approach, time, "there it is again");
        }
        // (Twenty seconds, not forty-five: it waited by the doors too long and never moved on -- you, 2026-11-17.)
        else if (time - m_waitSince > 20.0f)
        {
            // (Not for good: it comes back in a minute or two to see -- once.)
            if (m_kind == GiantKind::Walker && m_comeBackAt < 0.0f)
            {
                m_comeBackTo = m_targetAt;
                m_comeBackAt = time + 60.0f + static_cast<float>((m_seed * 2654435761u) >> 26);
            }
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

void Giant::Force(Mode mode, int target, const glm::vec3& at, float time)
{
    if (m_mode == Mode::Dead || m_mode == Mode::Hold)
    {
        return;
    }
    m_target = target;
    m_targetAt = at;
    m_lastSeen = time;
    m_frozen = false;
    m_huntUntil = time;
    m_nextStrikeAt = time;
    m_flankPhase = 0;
    if (mode == Mode::Strike)
    {
        m_strike = 0.0f;
        m_struck = false;
        m_strikeAt = at;
    }
    if (mode == Mode::Hunt)
    {
        m_huntUntil = time + 3.0f;
    }
    Enter(mode, time, "made to (giant_do)");
    m_modeSince = time;
}

glm::vec3 Giant::HeadForward() const
{
    if (m_kind == GiantKind::Swimmer && m_spineAt.size() >= 2)
    {
        return glm::normalize(m_spineAt[0] - m_spineAt[1] + glm::vec3(1e-5f));
    }
    return {-std::sin(m_yaw), 0.0f, -std::cos(m_yaw)};
}

glm::vec3 Giant::OutOfBody(const glm::vec3& point, float pad) const
{
    glm::vec3 out{0.0f};
    if (m_kind != GiantKind::Swimmer || m_mode == Mode::Dead)
    {
        return out;
    }
    for (size_t k = 0; k + 1 < m_spineAt.size() && k + 1 < m_radius.size(); ++k)
    {
        const glm::vec3 a = m_spineAt[k];
        const glm::vec3 b = m_spineAt[k + 1];
        const glm::vec3 ab = b - a;
        const float u = std::clamp(glm::dot(point - a, ab) / std::max(glm::dot(ab, ab), 1e-4f), 0.0f, 1.0f);
        const glm::vec3 nearest = a + ab * u;
        const float r = glm::mix(m_radius[k], m_radius[k + 1], u) * 0.9f + pad;
        const glm::vec3 away = point - nearest;
        const float d = glm::length(away);
        if (d < r)
        {
            const glm::vec3 push = (d > 1e-3f ? away / d : glm::vec3(0.0f, 1.0f, 0.0f)) * (r - d);
            if (glm::length(push) > glm::length(out))
            {
                out = push;
            }
        }
    }
    return out;
}

void Giant::Drop(float time, const char* why)
{
    if (m_holding < 0)
    {
        return;
    }
    m_holding = -1;
    m_lowering = false;
    if (m_mode == Mode::Dead)
    {
        return;
    }
    m_mode = Mode::Hunt;
    m_modeSince = time;
    m_huntUntil = time + 3.0f;
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
    // Felt, not feared: it turns on whoever did it, and hunts rougher -- and hurt enough while it holds somebody, it lets them go.
    m_annoyed = std::min(std::max(m_annoyed, 0.35f) + amount / 250.0f, 1.0f);
    m_lastHurt = time;
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
    // Shot, it comes for whoever shot it, at once, wherever it was looking -- out of a freeze, out of looking about where they went.
    if (m_mode == Mode::Roam || m_mode == Mode::Notice || m_mode == Mode::Linger)
    {
        m_frozen = false;
        m_mode = Mode::Approach;
        m_modeSince = time;
        m_goal = "shot: coming for whoever did it";
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

Giant::ShownExtras Giant::Extras(float time) const
{
    ShownExtras extras;
    extras.peek = m_peekWanted;
    extras.frozen = m_frozen;
    extras.reachingIn = m_reachingIn;
    extras.eats = m_eats;
    extras.lowering = m_lowering;
    extras.holding = m_holding;
    extras.reachDoor = m_reachDoor;
    extras.reachInside = m_reachInside;
    extras.thrown = m_throwSince >= 0.0f ? time - m_throwSince : -1.0f;
    extras.pullOut = std::max(m_pullOutUntil - time, 0.0f);
    return extras;
}

void Giant::SetShownExtras(const ShownExtras& extras, float time)
{
    m_peekWanted = extras.peek;
    m_frozen = extras.frozen;
    m_reachingIn = extras.reachingIn;
    m_eats = extras.eats;
    m_lowering = extras.lowering;
    m_holding = extras.holding;
    m_reachDoor = extras.reachDoor;
    m_reachInside = extras.reachInside;
    // (The same moment in it, by this machine's clock: not the host's, which runs apart from it.)
    const float throwSince = extras.thrown >= 0.0f ? time - extras.thrown : -1.0f;
    if (throwSince < 0.0f || m_throwSince < 0.0f || std::abs(throwSince - m_throwSince) > 0.3f)
    {
        m_throwSince = throwSince;
    }
    m_pullOutUntil = extras.pullOut > 0.0f ? time + extras.pullOut : -1.0f;
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
    // Its lights, on every machine from what it is doing: a leviathan stalking somebody puts its lights out -- dark, it comes round
    // behind them -- and they flare as it rushes; a strider being watched stands still, its eyes dimmed.
    {
        // (Only while it is going round them in the dark -- flanking: you, 2026-11-16 -- not merely coming to look.)
        const bool stalking = m_kind == GiantKind::Swimmer && m_mode == Mode::Hunt;
        const bool still = m_kind == GiantKind::Walker && m_mode == Mode::Notice;
        const float darkWanted = stalking ? 1.0f : still ? 0.75f : 0.0f;
        m_dark += (darkWanted - m_dark) * std::min(dt * (darkWanted > m_dark ? 1.5f : 6.0f), 1.0f);
        const float flare = m_kind == GiantKind::Swimmer && m_mode == Mode::Strike ? 3.0f : 1.0f;
        const float lit = (1.0f - m_dark) * flare * (m_mode == Mode::Dead ? 0.0f : 1.0f);
        if (MeshRenderer* renderer = m_glowEntity.IsValid() ? m_scene.GetMeshRenderer(m_glowEntity) : nullptr)
        {
            renderer->material.emissive = (glm::length(m_look.glow) > 0.01f ? m_look.glow : m_look.eyeGlow) * 3.0f * lit;
        }
        if (MeshRenderer* renderer = m_eyeEntity.IsValid() ? m_scene.GetMeshRenderer(m_eyeEntity) : nullptr)
        {
            if (glm::length(renderer->material.emissive) > 0.0f || lit > 0.0f)
            {
                renderer->material.emissive = m_look.eyeGlow * (m_eyesGlow ? 2.5f : 0.04f) * lit;
            }
        }
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
