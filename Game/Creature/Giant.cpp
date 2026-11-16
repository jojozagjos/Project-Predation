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
        std::snprintf(line, sizeof(line), "a leviathan %.0f m long, a %s; %s, %s%s (%.0f of %.0f health)", m_size, FormName(m_form), ColourFamilyName(m_look.family),
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
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, base, base + glm::vec3(0.0f, spineLength, radii.z * 0.25f), radii.y * 0.12f, radii.y * 0.02f), radii.y * 0.1f);
            }
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
        const glm::vec3 reach = radii * 1.5f + glm::vec3(spineLength + thick * 3.0f + cell * 3.0f);
        AddPart(whole, m_weights, Sculpt(-reach, reach, cell * 1.4f, field, m_look, h * 0.05f, 10000), 0);
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
    m_neck = glm::vec3(0.0f, -radii.y * 0.3f, -radii.z * 0.95f);

    // The head: hung low at the front, its own -- long or blunt, mandibles or none, crested or not, horned now and then -- its eyes a
    // cluster on it.
    {
        const float hl = h * random.Range(0.06f, 0.12f);
        const float crest = random.Range(0.0f, 1.0f);
        const float snout = random.Range(0.5f, 1.4f);
        const int mandibles = random.Unit() < 0.25f ? 0 : (random.Unit() < 0.6f ? 2 : 4);
        const int horns = random.Unit() < 0.3f ? 2 : 0;
        const auto field = [&](const glm::vec3& p)
        {
            float d = Sdf::Ellipsoid(p - glm::vec3(0.0f, 0.0f, -hl * 0.6f * snout), glm::vec3(hl * 0.42f, hl * 0.38f, hl * 0.75f * snout));
            // (Its neck's end, a ball the head turns on.)
            d = Sdf::SmoothUnion(d, glm::length(p) - hl * 0.35f, hl * 0.2f);
            for (int k = 0; k < mandibles; ++k)
            {
                const float side = (k % 2 == 0) ? -1.0f : 1.0f;
                const float lower = static_cast<float>(k / 2);
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(side * hl * 0.25f, -hl * (0.2f + 0.1f * lower), -hl * snout),
                                                       glm::vec3(side * hl * 0.1f, -hl * (0.45f + 0.15f * lower), -hl * (1.65f * snout - 0.2f * lower)), hl * 0.13f, hl * 0.03f),
                                     hl * 0.08f);
            }
            for (int k = 0; k < horns; ++k)
            {
                const float side = (k % 2 == 0) ? -1.0f : 1.0f;
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(side * hl * 0.25f, hl * 0.2f, -hl * 0.4f), glm::vec3(side * hl * 0.6f, hl * 0.9f, hl * 0.3f), hl * 0.1f, hl * 0.02f),
                                     hl * 0.08f);
            }
            if (crest > 0.4f)
            {
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(0.0f, hl * 0.25f, -hl * 0.5f), glm::vec3(0.0f, hl * (0.4f + crest * 0.6f), hl * 0.35f), hl * 0.14f, hl * 0.04f),
                                     hl * 0.1f);
            }
            return d + (Sdf::Fbm(p / (hl * 0.25f), grain + 5u, 2) - 0.5f) * hl * 0.06f;
        };
        const glm::vec3 reach{hl * 1.0f, hl * 1.6f, hl * 2.2f * std::max(snout, 1.0f)};
        AddPart(whole, m_weights, Sculpt(-reach, reach, std::max(hl * 0.04f, 0.06f), field, m_look, hl * 0.3f, 5000), 1);
        std::vector<glm::vec4> eyes;
        const int count = 2 + 2 * static_cast<int>(random.Unit() * 4.0f);
        for (int k = 0; k < count; ++k)
        {
            const float side = (k % 2 == 0) ? -1.0f : 1.0f;
            const float row = static_cast<float>(k / 2);
            const glm::vec3 inside{side * hl * 0.12f, hl * 0.05f, -hl * snout * (0.85f - 0.12f * row)};
            const glm::vec3 at = OnSkin(field, inside, glm::vec3(side * 0.8f, 0.4f + 0.1f * row, -0.5f), hl * 2.0f);
            eyes.push_back({at.x, at.y, at.z, hl * 0.045f});
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
    m_skinning.assign(static_cast<size_t>(2 + 2 * m_legs.size()), glm::mat4(1.0f));

    // The strands hanging from its belly -- on some of them only -- each its own: long or short, thick or thin, stiff or limp.
    if (random.Unit() < 0.45f)
    {
        const int count = 2 + static_cast<int>(random.Unit() * 6.0f);
        for (int k = 0; k < count; ++k)
        {
            Strand strand;
            const float a = random.Range(0.0f, kTau);
            strand.root = glm::vec3(std::sin(a) * radii.x * 0.5f, -radii.y * 0.85f, std::cos(a) * radii.z * 0.6f);
            const int links = 6 + static_cast<int>(random.Unit() * 10.0f);
            const float total = h * random.Range(0.25f, 0.6f);
            strand.length = total / static_cast<float>(links);
            strand.stiffness = random.Range(0.1f, 0.6f);
            strand.thick = h * random.Range(0.005f, 0.012f);
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
                    tubes.indices.insert(tubes.indices.end(), {a, c, b, b, c, d});
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
    if (m_mode == Mode::Strike)
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
            leg.step += dt / stepTime;
            const float s = std::min(leg.step, 1.0f);
            const float eased = s * s * (3.0f - 2.0f * s);
            leg.foot = glm::mix(leg.from, leg.to, eased) + glm::vec3(0.0f, std::sin(s * glm::pi<float>()) * m_bodyHeight * 0.1f, 0.0f);
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
        if (m_frozen)
        {
            continue;
        }
        const glm::vec3 wanted = FootFor(world, k, ahead);
        const float behind = Flat(leg.foot, wanted);
        const bool neighbourUp = m_legs[(k + 1) % count].step >= 0.0f || m_legs[(k + count - 1) % count].step >= 0.0f;
        const bool due = behind > (speed > 0.3f ? stride * 0.7f : stride * 0.35f);
        const bool overdue = behind > stride * 1.5f;
        if ((due && stepping < atOnce && !neighbourUp) || overdue)
        {
            leg.from = leg.foot;
            leg.to = wanted;
            leg.step = 0.0f;
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
    // what it plays with or holds -- and its height eased, not jumping when a foot comes down on something higher.
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
    const bool low = m_mode == Mode::Play || m_mode == Mode::Strike || m_mode == Mode::Hold;
    const float crouch = low ? 0.16f : 0.0f;
    const float wantY = feetY + m_bodyHeight * (1.0f - crouch);
    m_bodyY += (wantY - m_bodyY) * std::min(dt * 1.5f, 1.0f);
    glm::vec3 body = glm::vec3(m_position.x, m_bodyY + m_dip, m_position.z);
    const float pitch = (low ? 0.16f : 0.0f) + m_lean.x;
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
        fall = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(glm::angleAxis(down * glm::half_pi<float>() * 0.9f * m_toppleSide, facing)) *
               glm::translate(glm::mat4(1.0f), -pivot);
        if (was < 1.0f && m_topple >= 1.0f && m_footfalls.size() < 16)
        {
            m_footfalls.push_back(pivot);
        }
    }
    const auto part = [&](const glm::vec3& at, const glm::quat& rotation) { return fall * glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(rotation); };
    m_skinning[0] = part(body, bodyTurn);

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
    const glm::quat wanted = glm::quatLookAtRH(glm::normalize(look), glm::vec3(0.0f, 1.0f, 0.0f));
    if (!dead)
    {
        m_headTurn = glm::slerp(m_headTurn, wanted, std::min(dt * 1.4f, 1.0f));
    }
    m_skinning[1] = part(neck, m_headTurn);
    m_head = glm::vec3(fall * glm::vec4(neck + m_headTurn * glm::vec3(0.0f, 0.0f, -m_bodyHeight * 0.06f), 1.0f));

    // Holding somebody: up from where it took them to just under its head, turned over there -- eased, held steady.
    m_holdWanted = m_head + m_headTurn * glm::vec3(0.0f, -m_bodyHeight * 0.05f, -m_bodyHeight * 0.05f) - glm::vec3(0.0f, 2.2f, 0.0f);
    // (To be thrown away: swung down in front of it, a few metres off the ground.)
    if (m_lowering)
    {
        m_holdWanted = m_position + Yawed(m_yaw) * glm::vec3(0.0f, 5.0f, -m_bodyHeight * 0.3f);
    }
    if (m_mode == Mode::Hold)
    {
        m_holdPoint += (m_holdWanted - m_holdPoint) * std::min(dt * (m_lowering ? 1.6f : 0.9f), 1.0f);
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

    // Its strands, hanging and swaying (each link held to the next, damped); playing, the nearest reaching down; holding, wrapped
    // round what it holds. Drawn as tubes through their links.
    if (!m_strands.empty())
    {
        size_t holder = 0;
        float nearest = 1.0e9f;
        for (size_t s = 0; s < m_strands.size(); ++s)
        {
            const float d = glm::distance(m_strands[s].points.front(), m_holdPoint);
            if (d < nearest)
            {
                nearest = d;
                holder = s;
            }
        }
        size_t vertex = 0;
        for (size_t s = 0; s < m_strands.size(); ++s)
        {
            Strand& strand = m_strands[s];
            strand.points[0] = glm::vec3(fall * glm::vec4(body + bodyTurn * strand.root, 1.0f));
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
                strand.points[i] += moved + glm::vec3(0.0f, -9.8f, 0.0f) * dt * dt + (rest - now) * (strand.stiffness * 0.01f);
                strand.last[i] = now;
            }
            glm::vec3& tip = strand.points.back();
            if (m_mode == Mode::Hold && s == holder)
            {
                tip += (m_holdPoint + glm::vec3(0.0f, 1.2f, 0.0f) - tip) * std::min(dt * 4.0f, 1.0f);
            }
            else if (m_mode == Mode::Play)
            {
                tip += (m_lookAt - tip) * std::min(dt * 0.25f, 1.0f);
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
            for (size_t i = 0; i < strand.points.size(); ++i)
            {
                const glm::vec3 a = strand.points[i > 0 ? i - 1 : 0];
                const glm::vec3 b = strand.points[std::min(i + 1, strand.points.size() - 1)];
                const glm::vec3 along = glm::normalize(b - a + glm::vec3(0.0f, -1e-4f, 0.0f));
                glm::vec3 side = glm::cross(along, glm::vec3(0.0f, 0.0f, 1.0f));
                side = glm::length(side) > 1e-3f ? glm::normalize(side) : glm::vec3(1.0f, 0.0f, 0.0f);
                const glm::vec3 other = glm::cross(along, side);
                const float r = strand.thick * (1.0f - 0.7f * static_cast<float>(i) / static_cast<float>(strand.points.size()));
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
    (void)time;

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
    // Its kind of body: a serpent most often, then the rest.
    {
        const float pick = random.Unit();
        m_form = pick < 0.28f ? Form::Serpent : pick < 0.48f ? Form::Whale : pick < 0.66f ? Form::Fish : pick < 0.83f ? Form::Ray : Form::Squid;
    }
    // (Really big, some of them: most forty-odd metres, now and then one a hundred and more.)
    const float L = 30.0f + 110.0f * std::pow(random.Unit(), 2.0f);
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
    const glm::vec3 mouthRadii{0.62f * mouthR, 0.09f * mouthR, 0.05f * L};
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
    const float toothR = mouthR * 0.035f;
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
        // (Only along what of it there is: not out past a squid's mantle among its arms, nor down a ray's thin tail -- placed there,
        // they hung in the water by themselves.)
        const float lastT = m_form == Form::Squid ? 0.45f : m_form == Form::Ray ? 0.45f : 0.85f;
        for (int i = 0; i < count; ++i)
        {
            const float t = 0.1f + (lastT - 0.1f) * static_cast<float>(i) / static_cast<float>(count - 1);
            for (const float side : {-1.0f, 1.0f})
            {
                const glm::vec3 inside{0.0f, profile(t) * 0.1f, t * L};
                const glm::vec3 on = OnSkin(field, inside, glm::vec3(side, -0.15f, 0.0f), std::max(R, wings) * 3.0f);
                if (std::abs(field(on)) < std::max(profile(t) * 0.15f, 0.3f))
                {
                    lights.push_back(glm::vec4(on, std::max(profile(t) * 0.06f, 0.15f)));
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
        // (Its beat as quick as it is going: a stroke for so far forward -- driven too fast, its tail thrashed while it hardly moved.)
        m_swimPhase += dt * (0.12f + speed / std::max(L * 0.35f, 1.0f)) * (m_form == Form::Whale ? 0.7f : m_form == Form::Ray ? 0.6f : 1.0f);
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
    const float effort = std::clamp(speed / std::max(m_speed, 0.5f), 0.2f, 1.0f);
    for (int k = 0; k < N; ++k)
    {
        const float t = static_cast<float>(k) / static_cast<float>(N - 1);
        const float wave = std::sin(m_swimPhase * kTau - t * 4.5f) * effort;
        glm::vec3 offset{0.0f};
        const glm::vec3 right = glm::vec3(frames[static_cast<size_t>(k)][0]);
        const glm::vec3 up = glm::vec3(frames[static_cast<size_t>(k)][1]);
        switch (m_form)
        {
        case Form::Serpent: offset = right * (0.04f * L * t * wave); break;
        case Form::Fish: offset = right * (0.03f * L * std::pow(glm::smoothstep(0.35f, 1.0f, t), 1.5f) * wave); break;
        case Form::Whale: offset = up * (0.025f * L * glm::smoothstep(0.45f, 1.0f, t) * wave); break;
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
    }
    if (m_eyeEntity.IsValid())
    {
        AABB eyeBounds;
        SkinMesh(m_eyeRest, m_eyeWeights, m_skinning, m_eyeSkinned, eyeBounds);
        recentre(m_eyeSkinned, eyeBounds);
        m_meshes.UpdateDynamic(m_eyeMesh, m_eyeSkinned, eyeBounds);
        place(m_eyeEntity);
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
    // (Quicker round in a rush, as anything is going for something.)
    const float turnRate = m_kind == GiantKind::Walker ? 0.45f : m_mode == Mode::Strike ? 1.4f : 0.6f;
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
    // Being looked at, from a way off, a strider stands dead still -- its eyes dimmed, nothing of it moving -- for as long as it is
    // looked at; it comes on again the moment nobody is (you, 2026-11-15: something frightening). Not once it is angry.
    m_frozen = false;
    if (m_kind == GiantKind::Walker && target != nullptr && m_annoyed < 0.3f && (m_mode == Mode::Approach || m_mode == Mode::Notice || m_mode == Mode::Roam))
    {
        const glm::vec3 eye = target->feet + glm::vec3(0.0f, target->height * 0.9f, 0.0f);
        const glm::vec3 to = m_head - eye;
        const float away = glm::length(to);
        m_frozen = away > 40.0f && glm::dot(to / std::max(away, 1e-3f), glm::normalize(target->look + glm::vec3(1e-5f))) > std::cos(glm::radians(14.0f));
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
            // Dark (its lights out: Animate), round behind them and below, where they are not looking -- and from there it comes.
            const glm::vec3 flatLook = glm::normalize(glm::vec3(target->look.x, 0.0f, target->look.z) + glm::vec3(1e-4f, 0.0f, 0.0f));
            const glm::vec3 behind = m_targetAt - flatLook * 18.0f + glm::vec3(0.0f, -7.0f, 0.0f);
            // (Not straight through them to get there: round, a little to one side.)
            const glm::vec3 side = glm::cross(flatLook, glm::vec3(0.0f, 1.0f, 0.0f)) * ((m_seed & 1u) != 0u ? 14.0f : -14.0f);
            const bool there = Flat(m_head, behind) < 12.0f;
            Steer(world, glm::distance(m_head, m_targetAt) < 14.0f && !there ? behind + side : behind, m_speed * 1.1f, dt);
            m_goal = there ? "behind them in the dark; coming" : "its lights out; going round behind them";
            if (there)
            {
                m_playUntil = std::min(m_playUntil, time);
            }
            else
            {
                m_playUntil = std::max(m_playUntil, time + 0.5f);
            }
            if (time - m_modeSince > 25.0f)
            {
                m_playUntil = time; // (round long enough: it comes whatever)
            }
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
                    m_lowering = false;
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
            m_lowering = false;
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
            // Done playing: brought down near the ground (Animate), then thrown away, hard -- hurt, flung along the ground, but not dropped
            // from its eyes to die of the fall.
            m_lowering = true;
            if (m_holdPoint.y > m_position.y + 6.0f && heldFor < 12.0f)
            {
                m_goal = "tired of it; bringing it down to throw away";
                break;
            }
            Blow blow;
            blow.player = m_holding;
            blow.at = m_holdPoint;
            blow.damage = 20.0f;
            blow.push = Yawed(m_yaw + 1.2f) * glm::vec3(0.0f, 0.0f, -1.0f) * 13.0f + glm::vec3(0.0f, 3.0f, 0.0f);
            blow.release = true;
            m_blows.push_back(blow);
            m_holding = -1;
            m_lowering = false;
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
            // (Enough that it is the worst thing that can happen down there, not so much that nobody lives to be let go far under:
            // at 14 a bite, everybody was dead a breath before it let go.)
            bite.damage = 10.0f + 14.0f * m_annoyed;
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
    m_lowering = false;
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
    // Its lights, on every machine from what it is doing: a leviathan stalking somebody puts its lights out -- dark, it comes round
    // behind them -- and they flare as it rushes; a strider being watched stands still, its eyes dimmed.
    {
        const bool stalking = m_kind == GiantKind::Swimmer && (m_mode == Mode::Play || m_mode == Mode::Approach);
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
