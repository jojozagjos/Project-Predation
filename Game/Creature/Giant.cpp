#include "Game/Creature/Giant.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/MeshSimplify.h"
#include "Engine/Render/Sdf.h"
#include "Engine/Render/SurfaceNets.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/CreatureSkin.h"
#include "Game/Creature/CreatureTraits.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
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

glm::quat Yawed(float yaw)
{
    return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

// From +Y (the way every limb is made, root at the origin) to `along`.
glm::quat AlongY(const glm::vec3& along)
{
    const float length = glm::length(along);
    if (length < 1e-5f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    return glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), along / length);
}

float Flat(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(glm::vec2(a.x - b.x, a.z - b.z));
}

float Wrap(float angle)
{
    return std::remainder(angle, kTau);
}

// A part sculpted as a distance field and meshed (Sdf, SurfaceNets), painted as a creature of its family is -- darker over the
// back, paler under the belly, its markings over that (SkinMarkingAt) -- and made simpler to about `target` triangles.
MeshData Sculpt(const glm::vec3& lo, const glm::vec3& hi, float cell, const std::function<float(const glm::vec3&)>& field,
                const CreatureAnatomy& look, float markScale, size_t target)
{
    const DistanceGrid grid = SampleDistance(lo, hi, cell, field);
    MeshData mesh = SurfaceNets(grid);
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
    if (mesh.indices.size() / 3 > target)
    {
        std::vector<uint32_t> kept;
        mesh = SimplifyMesh(mesh, target, cell * 0.05f, kept);
    }
    return mesh;
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
                mesh.vertices.push_back(vertex);
            }
        }
        for (int j = 0; j < rings; ++j)
        {
            for (int i = 0; i < around; ++i)
            {
                const uint32_t a = first + static_cast<uint32_t>(j * (around + 1) + i);
                const uint32_t b = a + static_cast<uint32_t>(around + 1);
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
    }
    return mesh;
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
    case Mode::Count: break;
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
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(AI, "Giant made in {:.0f} ms: {}", ms, Describe());
}

Giant::~Giant()
{
    const auto drop = [&](Part& part)
    {
        if (part.entity.IsValid())
        {
            m_scene.Destroy(part.entity);
            part.entity = Entity{};
        }
    };
    drop(m_body);
    drop(m_bodyGlow);
    drop(m_headPart);
    drop(m_eyes);
    drop(m_jaw);
    for (Leg& leg : m_legs)
    {
        drop(leg.thigh);
        drop(leg.shin);
    }
    for (Strand& strand : m_strands)
    {
        for (Part& part : strand.parts)
        {
            drop(part);
        }
    }
    for (Part& part : m_segments)
    {
        drop(part);
    }
    for (Part& part : m_segmentGlows)
    {
        drop(part);
    }
    for (const BodyHandle body : m_hitboxes)
    {
        m_physics.DestroyBody(body);
    }
    for (const MeshHandle mesh : m_ownMeshes)
    {
        m_meshes.Release(mesh);
    }
}

std::string Giant::Describe() const
{
    char line[256];
    if (m_kind == GiantKind::Walker)
    {
        std::snprintf(line, sizeof(line), "a strider %.0f m tall on %d legs, %d strands hanging; %s, %s", m_bodyHeight, static_cast<int>(m_legs.size()),
                      static_cast<int>(m_strands.size()), ColourFamilyName(m_look.family), SkinPatternName(m_look.pattern));
    }
    else
    {
        std::snprintf(line, sizeof(line), "a leviathan %.0f m long; %s, %s", m_size, ColourFamilyName(m_look.family), SkinPatternName(m_look.pattern));
    }
    return line;
}

Giant::Part Giant::MakePart(MeshLibrary& meshes, const MeshData& mesh, const std::string& name, bool glow)
{
    Part part;
    if (mesh.indices.empty())
    {
        return part;
    }
    part.mesh = meshes.Upload(mesh, "giant_" + std::to_string(m_seed) + "_" + name + "#" + std::to_string(m_ownMeshes.size()));
    m_ownMeshes.push_back(part.mesh);
    Material material = Material::Diffuse(glow ? glm::vec3(0.02f) : glm::vec3(1.0f), glow ? 0.3f : m_look.roughness);
    material.weathering = 0.0f;
    if (glow)
    {
        const glm::vec3 light = glm::length(m_look.glow) > 0.01f ? m_look.glow : m_look.eyeGlow;
        material.emissive = light * 1.6f;
    }
    part.entity = m_scene.CreateMeshEntity("giant_" + name, Transform{}, part.mesh, material);
    if (MeshRenderer* renderer = m_scene.GetMeshRenderer(part.entity))
    {
        renderer->castsShadow = !glow;
        renderer->blocksSky = false;
    }
    return part;
}

void Giant::ShowPart(const Part& part, const glm::vec3& at, const glm::quat& turn)
{
    if (!part.entity.IsValid())
    {
        return;
    }
    if (Transform* transform = m_scene.GetTransform(part.entity))
    {
        transform->position = at;
        transform->rotation = turn;
    }
}

void Giant::MakeHitbox(float halfLength, float radius)
{
    const BodyHandle body = m_physics.CreateCapsule(std::max(halfLength, 0.05f), std::max(radius, 0.1f), Transform{}, BodyMotion::Kinematic, 1000.0f,
                                                    PhysicsLayer::Hitbox);
    if (body.IsValid())
    {
        m_hitboxes.push_back(body);
    }
}

// --- The strider ---------------------------------------------------------------------------------------------------------------

void Giant::BuildWalker(MeshLibrary& meshes)
{
    SeededRandom random(static_cast<uint64_t>(m_seed) * 2862933555777941757ull + 3037000493ull);
    // How big, and every proportion of it, its own: nothing in steps.
    const float h = random.Range(17.0f, 34.0f);
    m_bodyHeight = h;
    m_size = h;
    m_speed = 2.4f + h * 0.045f;
    const glm::vec3 radii{h * random.Range(0.09f, 0.16f), h * random.Range(0.06f, 0.1f), h * random.Range(0.15f, 0.27f)};
    const float hump = random.Range(0.3f, 1.0f);
    const uint32_t grain = m_seed * 31u + 7u;
    const float cell = std::max(h * 0.012f, 0.18f);

    // The body: a long hull hung between the legs, a hump of it over the back, its underside sagging, lumped as grown things are.
    {
        const auto field = [&](const glm::vec3& p)
        {
            float d = Sdf::Ellipsoid(p, radii);
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, radii.y * 0.55f, radii.z * 0.1f), radii * glm::vec3(0.6f, 0.55f * hump + 0.2f, 0.7f)), radii.y * 0.4f);
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, -radii.y * 0.55f, radii.z * 0.2f), radii * glm::vec3(0.55f, 0.5f, 0.55f)), radii.y * 0.5f);
            return d + (Sdf::Fbm(p / (radii.y * 0.6f), grain, 3) - 0.5f) * radii.y * 0.18f;
        };
        const glm::vec3 reach = radii * 1.5f + glm::vec3(cell * 3.0f);
        m_body = MakePart(meshes, Sculpt(-reach, reach, cell * 1.4f, field, m_look, h * 0.05f, 9000), "body", false);
        // Its lights, in rows along its flanks, when its family has them.
        if (glm::length(m_look.glow) > 0.01f)
        {
            std::vector<glm::vec4> beads;
            const int rows = 2 + static_cast<int>(random.Unit() * 3.0f);
            for (int side = -1; side <= 1; side += 2)
            {
                for (int k = 0; k < rows * 4; ++k)
                {
                    const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(rows * 4) * 2.0f - 1.0f;
                    const float z = t * radii.z * 0.85f;
                    const float y = -radii.y * 0.1f + radii.y * 0.25f * static_cast<float>(k % rows);
                    const float x = radii.x * std::sqrt(std::max(1.0f - t * t * 0.8f, 0.05f)) * static_cast<float>(side) * 1.02f;
                    beads.push_back({x, y, z, h * 0.006f});
                }
            }
            m_bodyGlow = MakePart(meshes, Beads(beads), "lights", true);
        }
    }

    // The head: hung low at the front, long, its jaw a pair of mandibles, crested; its eyes in a cluster.
    {
        const float hl = h * random.Range(0.07f, 0.12f);
        const float crest = random.Range(0.0f, 1.0f);
        const auto field = [&](const glm::vec3& p)
        {
            float d = Sdf::Ellipsoid(p - glm::vec3(0.0f, 0.0f, -hl * 0.6f), glm::vec3(hl * 0.42f, hl * 0.38f, hl * 0.75f));
            for (const float side : {-1.0f, 1.0f})
            {
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(side * hl * 0.25f, -hl * 0.2f, -hl * 1.0f), glm::vec3(side * hl * 0.1f, -hl * 0.45f, -hl * 1.65f),
                                                       hl * 0.13f, hl * 0.03f), hl * 0.08f);
            }
            if (crest > 0.4f)
            {
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(0.0f, hl * 0.25f, -hl * 0.5f), glm::vec3(0.0f, hl * (0.4f + crest * 0.5f), hl * 0.35f), hl * 0.14f, hl * 0.04f),
                                     hl * 0.1f);
            }
            return d + (Sdf::Fbm(p / (hl * 0.25f), grain + 5u, 2) - 0.5f) * hl * 0.06f;
        };
        const glm::vec3 reach{hl * 0.8f, hl * 1.4f, hl * 2.0f};
        m_headPart = MakePart(meshes, Sculpt(-reach, reach, std::max(hl * 0.04f, 0.06f), field, m_look, hl * 0.3f, 5000), "head", false);
        std::vector<glm::vec4> eyes;
        const int count = 2 + 2 * static_cast<int>(random.Unit() * 3.5f);
        for (int k = 0; k < count; ++k)
        {
            const float side = (k % 2 == 0) ? -1.0f : 1.0f;
            const float row = static_cast<float>(k / 2);
            eyes.push_back({side * hl * (0.3f - 0.03f * row), hl * (0.12f + 0.07f * row), -hl * (1.05f - 0.12f * row), hl * 0.045f});
        }
        m_eyes = MakePart(meshes, Beads(eyes), "eyes", true);
        MakeHitbox(hl * 0.8f, hl * 0.45f);
        m_hitboxOn.push_back({-2, 0.0f}); // the head
    }
    MakeHitbox(radii.z * 0.7f, radii.y);
    m_hitboxOn.push_back({-1, 0.0f}); // the body

    // The legs: three to six, spread wide round it, each a long thigh rising to a knee well over the body and a shin down to a point.
    {
        const float pick = random.Unit();
        const int count = pick < 0.35f ? 3 : pick < 0.65f ? 4 : pick < 0.85f ? 5 : 6;
        const float spread = h * random.Range(0.5f, 0.75f);
        const float thick = h * random.Range(0.022f, 0.035f);
        const float turn = random.Range(0.0f, kTau);
        for (int k = 0; k < count; ++k)
        {
            Leg leg;
            const float angle = turn + kTau * static_cast<float>(k) / static_cast<float>(count) + random.Range(-0.15f, 0.15f);
            const glm::vec3 out{std::sin(angle), 0.0f, std::cos(angle)};
            leg.hip = out * glm::vec3(radii.x * 0.8f, 0.0f, radii.z * 0.8f) + glm::vec3(0.0f, radii.y * 0.25f, 0.0f);
            const float reach = std::sqrt(h * h + spread * spread);
            leg.upper = reach * random.Range(0.55f, 0.7f);
            leg.lower = reach * 1.25f - leg.upper;
            const uint32_t legGrain = grain + 100u * static_cast<uint32_t>(k + 1);
            const float upper = leg.upper;
            const float lower = leg.lower;
            const auto thigh = [&](const glm::vec3& p)
            {
                float d = Sdf::RoundCone(p, glm::vec3(0.0f), glm::vec3(0.0f, upper, 0.0f), thick * 1.3f, thick);
                d = Sdf::SmoothUnion(d, glm::length(p - glm::vec3(0.0f, upper, 0.0f)) - thick * 1.25f, thick * 0.5f);
                return d + (Sdf::Fbm(p / (thick * 1.5f), legGrain, 2) - 0.5f) * thick * 0.25f;
            };
            const auto shin = [&](const glm::vec3& p)
            {
                float d = Sdf::RoundCone(p, glm::vec3(0.0f), glm::vec3(0.0f, lower, 0.0f), thick, thick * 0.18f);
                // A spur a third of the way down.
                d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(0.0f, lower * 0.33f, 0.0f), glm::vec3(thick * 1.8f, lower * 0.3f, 0.0f), thick * 0.4f, thick * 0.08f),
                                     thick * 0.3f);
                return d + (Sdf::Fbm(p / (thick * 1.2f), legGrain + 3u, 2) - 0.5f) * thick * 0.2f;
            };
            const float legCell = std::max(thick * 0.28f, 0.08f);
            leg.thigh = MakePart(meshes, Sculpt(glm::vec3(-thick * 2.0f, -thick * 2.0f, -thick * 2.0f), glm::vec3(thick * 2.0f, upper + thick * 2.0f, thick * 2.0f), legCell,
                                                thigh, m_look, h * 0.04f, 2500),
                                 "thigh", false);
            leg.shin = MakePart(meshes, Sculpt(glm::vec3(-thick * 2.0f, -thick * 2.0f, -thick * 2.0f), glm::vec3(thick * 3.0f, lower + thick * 2.0f, thick * 2.0f), legCell,
                                               shin, m_look, h * 0.04f, 2500),
                                "shin", false);
            leg.foot = m_position + Yawed(m_yaw) * (out * spread);
            leg.foot.y = m_position.y;
            leg.from = leg.to = leg.foot;
            m_legs.push_back(leg);
            MakeHitbox(lower * 0.5f, thick * 1.2f);
            m_hitboxOn.push_back({k, 1.0f}); // its shin
        }
    }

    // The strands hanging from its belly, swinging as it walks, and reaching for what it plays with.
    {
        const int count = 3 + static_cast<int>(random.Unit() * 6.0f);
        const float link = h * random.Range(0.045f, 0.07f);
        const float thick = h * 0.009f;
        const auto field = [&](const glm::vec3& p) { return Sdf::RoundCone(p, glm::vec3(0.0f), glm::vec3(0.0f, link, 0.0f), thick, thick * 0.75f); };
        const MeshData linkMesh = Sculpt(glm::vec3(-thick * 2.0f), glm::vec3(thick * 2.0f, link + thick * 2.0f, thick * 2.0f), std::max(thick * 0.4f, 0.05f), field,
                                         m_look, h * 0.03f, 400);
        const MeshHandle shared = meshes.Upload(linkMesh, "giant_" + std::to_string(m_seed) + "_strand");
        m_ownMeshes.push_back(shared);
        Material material = Material::Diffuse(glm::vec3(1.0f), m_look.roughness);
        material.weathering = 0.0f;
        for (int k = 0; k < count; ++k)
        {
            Strand strand;
            const float a = random.Range(0.0f, kTau);
            strand.root = glm::vec3(std::sin(a) * radii.x * 0.5f, -radii.y * 0.85f, std::cos(a) * radii.z * 0.6f);
            strand.length = link;
            const int links = 5 + static_cast<int>(random.Unit() * 6.0f);
            const glm::vec3 start = m_position + glm::vec3(0.0f, h, 0.0f) + Yawed(m_yaw) * strand.root;
            for (int i = 0; i <= links; ++i)
            {
                strand.points.push_back(start - glm::vec3(0.0f, link * static_cast<float>(i), 0.0f));
            }
            strand.last = strand.points;
            for (int i = 0; i < links; ++i)
            {
                Part part;
                part.mesh = shared;
                part.entity = m_scene.CreateMeshEntity("giant_strand", Transform{}, shared, material);
                if (MeshRenderer* renderer = m_scene.GetMeshRenderer(part.entity))
                {
                    renderer->blocksSky = false;
                }
                strand.parts.push_back(part);
            }
            m_strands.push_back(std::move(strand));
        }
    }
}

// --- The leviathan -------------------------------------------------------------------------------------------------------------

void Giant::BuildSwimmer(MeshLibrary& meshes)
{
    SeededRandom random(static_cast<uint64_t>(m_seed) * 6364136223846793005ull + 1442695040888963407ull);
    const float length = random.Range(30.0f, 62.0f);
    m_size = length;
    m_speed = 3.5f + length * 0.03f;
    constexpr int kSegments = 16;
    m_segmentLength = length / static_cast<float>(kSegments + 1);
    const float girth = length * random.Range(0.045f, 0.065f);
    const float fins = random.Range(0.0f, 1.0f);
    const uint32_t grain = m_seed * 37u + 11u;
    const float cell = std::max(girth * 0.07f, 0.12f);

    // The head: long and heavy, an upper skull with the lower jaw its own part, hinged, and eyes along its sides.
    {
        const float hl = m_segmentLength * 1.8f;
        const float hw = girth * 1.05f;
        const auto skull = [&](const glm::vec3& p)
        {
            float d = Sdf::Ellipsoid(p - glm::vec3(0.0f, hw * 0.15f, -hl * 0.45f), glm::vec3(hw, hw * 0.7f, hl * 0.75f));
            // Plates down its brow.
            d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, hw * 0.6f, -hl * 0.2f), glm::vec3(hw * 0.5f, hw * 0.3f, hl * 0.6f)), hw * 0.3f);
            return d + (Sdf::Fbm(p / (hw * 0.35f), grain, 3) - 0.5f) * hw * 0.12f;
        };
        const glm::vec3 reach{hw * 1.6f, hw * 1.4f, hl * 1.4f};
        m_headPart = MakePart(meshes, Sculpt(-reach, reach, cell, skull, m_look, girth * 0.6f, 7000), "head", false);
        const auto jaw = [&](const glm::vec3& p)
        {
            float d = Sdf::Ellipsoid(p - glm::vec3(0.0f, -hw * 0.25f, -hl * 0.45f), glm::vec3(hw * 0.85f, hw * 0.35f, hl * 0.7f));
            // Teeth: a row of spikes along its rim.
            for (int k = 0; k < 8; ++k)
            {
                const float t = static_cast<float>(k) / 7.0f;
                for (const float side : {-1.0f, 1.0f})
                {
                    const glm::vec3 base{side * hw * 0.7f * (1.0f - t * 0.5f), -hw * 0.05f, -hl * (0.1f + 0.9f * t)};
                    d = std::min(d, Sdf::RoundCone(p, base, base + glm::vec3(0.0f, hw * 0.35f, 0.0f), hw * 0.07f, hw * 0.01f));
                }
            }
            return d;
        };
        m_jaw = MakePart(meshes, Sculpt(-reach, reach, cell, jaw, m_look, girth * 0.6f, 4000), "jaw", false);
        std::vector<glm::vec4> eyes;
        const int pairs = 1 + static_cast<int>(random.Unit() * 3.0f);
        for (int k = 0; k < pairs; ++k)
        {
            for (const float side : {-1.0f, 1.0f})
            {
                eyes.push_back({side * hw * 0.92f, hw * 0.3f, -hl * (0.55f + 0.18f * static_cast<float>(k)), hw * 0.09f});
            }
        }
        m_eyes = MakePart(meshes, Beads(eyes), "eyes", true);
        MakeHitbox(hl * 0.6f, hw);
        m_hitboxOn.push_back({-2, 0.0f});
    }

    // Its length: segments thickest a quarter of the way back, tapering to a tail with a fluke; fins at the shoulders and halfway.
    for (int i = 0; i < kSegments; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kSegments - 1);
        const float shape = t < 0.25f ? 0.85f + 0.6f * t : 1.0f - 0.82f * std::pow((t - 0.25f) / 0.75f, 1.3f);
        const float r = girth * std::max(shape, 0.12f);
        m_segmentRadius.push_back(r);
        const float half = m_segmentLength * 0.75f;
        const bool fin = (i == 1 || (i == 7 && fins > 0.4f));
        const bool tail = i == kSegments - 1;
        const uint32_t segGrain = grain + 50u * static_cast<uint32_t>(i + 1);
        const auto field = [&](const glm::vec3& p)
        {
            float d = Sdf::Ellipsoid(p, glm::vec3(r, r * 0.85f, half + r * 0.3f));
            // A ridge of plates down its back.
            d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, glm::vec3(0.0f, r * 0.7f, half * 0.4f), glm::vec3(0.0f, r * 1.25f, -half * 0.2f), r * 0.18f, r * 0.03f), r * 0.15f);
            if (fin)
            {
                for (const float side : {-1.0f, 1.0f})
                {
                    d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(side * r * 1.6f, -r * 0.3f, half * 0.3f), glm::vec3(r * 1.3f, r * 0.12f, r * 0.7f)), r * 0.2f);
                }
            }
            if (tail)
            {
                d = Sdf::SmoothUnion(d, Sdf::Ellipsoid(p - glm::vec3(0.0f, 0.0f, half * 1.2f), glm::vec3(girth * 1.6f, girth * 0.1f, girth * 0.7f)), r * 0.4f);
            }
            return d + (Sdf::Fbm(p / (r * 0.5f + 0.2f), segGrain, 2) - 0.5f) * r * 0.1f;
        };
        const float wide = (fin ? r * 3.2f : r * 1.4f) + (tail ? girth * 1.7f : 0.0f);
        const glm::vec3 reach{wide + cell * 2.0f, r * 1.6f + cell * 2.0f, half * 1.4f + (tail ? girth * 1.0f : 0.0f) + r};
        m_segments.push_back(MakePart(meshes, Sculpt(-reach, reach, std::max(r * 0.12f, 0.1f), field, m_look, girth * 0.6f, 2500), "segment", false));
        // Its lights down its flanks, two rows, more of them towards the head.
        std::vector<glm::vec4> beads;
        if (glm::length(m_look.glow) > 0.01f || i % 3 == 0)
        {
            for (const float side : {-1.0f, 1.0f})
            {
                for (int k = 0; k < 2; ++k)
                {
                    beads.push_back({side * r * 0.97f, -r * 0.1f + r * 0.3f * static_cast<float>(k), (static_cast<float>(k) - 0.5f) * half * 0.6f, r * 0.06f});
                }
            }
        }
        m_segmentGlows.push_back(beads.empty() ? Part{} : MakePart(meshes, Beads(beads), "lights", true));
        if (i % 3 == 1)
        {
            MakeHitbox(m_segmentLength * 1.4f, r);
            m_hitboxOn.push_back({i, 0.0f});
        }
    }
    m_trail.assign(1, m_position);
}

// --- The mind ------------------------------------------------------------------------------------------------------------------

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
        // In the water, through the water: the murk is its limit.
        if (!who.inWater || away > 80.0f)
        {
            return false;
        }
    }
    else if (away > 300.0f)
    {
        return false;
    }
    // Ahead of it and round to the sides; never straight behind.
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
    // Kept to its ground round the site: drawn back in past its reach.
    glm::vec3 wanted = goal;
    const glm::vec2 fromMiddle = glm::vec2(m_position.x, m_position.z) - world.middle;
    if (glm::length(fromMiddle) > world.reach)
    {
        wanted = glm::vec3(world.middle.x, m_position.y, world.middle.y);
    }
    const float bearing = std::atan2(-(wanted.x - m_position.x), -(wanted.z - m_position.z));
    const float turnRate = m_kind == GiantKind::Walker ? 0.45f : 0.7f;
    const float turn = std::clamp(Wrap(bearing - m_yaw), -turnRate * dt, turnRate * dt);
    m_yaw = Wrap(m_yaw + turn);
    const float facing = std::cos(Wrap(bearing - m_yaw));
    // Slows to turn: a thing that size does not wheel about on the spot at full stride.
    const float pace = speed * glm::mix(0.35f, 1.0f, std::max(facing, 0.0f));
    const glm::vec3 ahead = Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 velocity = ahead * pace;
    if (m_kind == GiantKind::Swimmer)
    {
        // Up or down towards the depth of what it goes for, between the sea's floor and its surface.
        velocity.y = std::clamp((wanted.y - m_position.y) * 0.5f, -pace * 0.6f, pace * 0.6f);
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
        float surface = 0.0f;
        const float floor = world.ground(m_position.x, m_position.z);
        const float r = m_segmentRadius.empty() ? 2.0f : m_segmentRadius.front();
        if (world.water(m_position, surface) > 0.0f)
        {
            m_position.y = std::clamp(m_position.y, floor + r + 1.0f, std::max(surface - r - 1.5f, floor + r + 1.0f));
        }
        else
        {
            m_position.y = std::max(m_position.y, floor + r + 1.0f);
        }
    }
}

void Giant::Think(const GiantWorld& world, const std::vector<GiantSensed>& people, float time, float dt)
{
    m_shownOnly = false;
    // Making them out: faster the nearer, the more they move, the more they are lit; slower crouched; gone again out of sight.
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
            // About three seconds for somebody walking in the open at a hundred metres: long enough to get to cover, seen first.
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
    for (const GiantSensed& who : people)
    {
        if (who.id == m_target && who.alive)
        {
            target = &who;
        }
    }
    if (target == nullptr && m_target >= 0)
    {
        m_target = -1;
    }
    m_annoyed = std::max(m_annoyed - dt * 0.01f, 0.0f);
    const auto enter = [&](Mode mode, const char* goal)
    {
        if (m_mode != mode)
        {
            m_mode = mode;
            m_modeSince = time;
        }
        m_goal = goal;
    };
    const float reachOver = m_kind == GiantKind::Walker ? m_bodyHeight * 0.55f : 7.0f;
    const bool seenNow = target != nullptr && time - m_lastSeen < 0.5f;
    if (target != nullptr && target->roofed)
    {
        // Under a roof: beyond it. It waits about the place, its head turned to it, and then loses interest.
        enter(Mode::Linger, "waiting about where they went in");
    }
    else if (target != nullptr && m_kind == GiantKind::Swimmer && !target->inWater)
    {
        enter(Mode::Linger, "circling where they left the water");
    }

    switch (m_mode)
    {
    case Mode::Roam:
    {
        m_goal = "going about";
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
        Steer(world, m_roamTo, m_speed, dt);
        m_lookAt = m_position + Yawed(m_yaw) * glm::vec3(0.0f, -5.0f, -60.0f);
        // Something half made out: it stops and looks.
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
                enter(Mode::Notice, "has seen something; looking");
            }
        }
        if (target != nullptr)
        {
            enter(Mode::Approach, "going to look at what it found");
            m_call = true;
        }
        break;
    }
    case Mode::Notice:
    {
        // Still, its head turned to it, making it out.
        m_velocity *= std::max(1.0f - dt * 1.5f, 0.0f);
        m_position += m_velocity * dt;
        m_lookAt = m_targetAt + glm::vec3(0.0f, 1.0f, 0.0f);
        if (target != nullptr)
        {
            enter(Mode::Approach, "going to look at what it found");
            m_call = true;
        }
        else if (time - m_modeSince > 8.0f)
        {
            enter(Mode::Roam, "going about");
        }
        break;
    }
    case Mode::Approach:
    {
        if (target == nullptr)
        {
            enter(Mode::Linger, "looking for where it went");
            break;
        }
        m_targetAt = seenNow ? target->feet : m_targetAt;
        m_lookAt = m_targetAt + glm::vec3(0.0f, 1.0f, 0.0f);
        // Not straight on top of them: the walker stands over them, the swimmer comes round them.
        const glm::vec3 goal = m_kind == GiantKind::Swimmer ? m_targetAt + glm::vec3(0.0f, 2.0f, 0.0f) : m_targetAt;
        Steer(world, goal, m_speed * (m_kind == GiantKind::Walker ? 1.7f : 1.4f), dt);
        if (Flat(m_kind == GiantKind::Walker ? m_position : m_head, m_targetAt) < reachOver * 1.6f)
        {
            enter(Mode::Play, "looking at what it found, playing with it");
            m_playUntil = time + (m_kind == GiantKind::Walker ? 3.0f : 7.0f) * (1.0f - 0.6f * m_annoyed);
        }
        break;
    }
    case Mode::Play:
    {
        if (target == nullptr)
        {
            enter(Mode::Linger, "looking for where it went");
            break;
        }
        m_targetAt = target->feet;
        m_lookAt = m_targetAt + glm::vec3(0.0f, 0.8f, 0.0f);
        if (m_kind == GiantKind::Walker)
        {
            // Over them, its head lowered to them, shifting about them as they move.
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
            // Round them, closer each time round: a toy in the water.
            const float round = time * 0.35f;
            const float radius = glm::mix(26.0f, 10.0f, std::clamp((time - m_modeSince) / 10.0f, 0.0f, 1.0f));
            Steer(world, m_targetAt + glm::vec3(std::cos(round) * radius, 1.0f, std::sin(round) * radius), m_speed * 1.2f, dt);
        }
        if (time >= m_playUntil && time >= m_nextStrikeAt)
        {
            enter(Mode::Strike, "goes for it");
            m_strike = 0.0f;
            m_struck = false;
            // Where it means to hit -- where they are going to be, near enough -- fixed now: a blow seen coming can be got out from under.
            m_strikeAt = target->feet + target->velocity * 0.6f;
        }
        break;
    }
    case Mode::Strike:
    {
        const float windup = m_kind == GiantKind::Walker ? 1.7f : 1.2f;
        const float duration = windup + 0.9f;
        m_strike = std::clamp((time - m_modeSince) / duration, 0.0f, 1.0f);
        m_lookAt = m_strikeAt + glm::vec3(0.0f, 0.5f, 0.0f);
        if (m_kind == GiantKind::Swimmer)
        {
            // Lined up, then a rush at it, jaws open.
            const bool rushing = time - m_modeSince > windup;
            Steer(world, rushing ? m_strikeAt + (m_strikeAt - m_position) * 0.5f : m_strikeAt, m_speed * (rushing ? 3.2f : 0.6f), dt);
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
            const glm::vec3 blowAt = m_kind == GiantKind::Walker ? m_strikeAt : m_head;
            const float radius = m_kind == GiantKind::Walker ? 3.6f : 5.0f;
            for (const GiantSensed& who : people)
            {
                if (!who.alive || who.roofed)
                {
                    continue;
                }
                const float off = m_kind == GiantKind::Walker ? Flat(who.feet, blowAt) : glm::distance(who.feet + glm::vec3(0.0f, 0.9f, 0.0f), blowAt);
                if (off < radius)
                {
                    Blow blow;
                    blow.player = who.id;
                    // Playing rough: the first blows knock somebody about; angered, it kills.
                    blow.damage = std::clamp((m_kind == GiantKind::Walker ? 45.0f : 55.0f) + 70.0f * m_annoyed + 25.0f * (1.0f - off / radius), 30.0f, 140.0f);
                    blow.at = blowAt;
                    const glm::vec3 away = who.feet - blowAt;
                    const glm::vec3 flat = glm::length(glm::vec2(away.x, away.z)) > 0.1f ? glm::normalize(glm::vec3(away.x, 0.0f, away.z)) : Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f);
                    blow.push = flat * 9.0f + glm::vec3(0.0f, 5.0f, 0.0f);
                    m_blows.push_back(blow);
                }
            }
            if (m_kind == GiantKind::Walker)
            {
                m_footfalls.push_back(m_strikeAt);
            }
        }
        if (m_strike >= 1.0f)
        {
            m_strike = 0.0f;
            m_nextStrikeAt = time + glm::mix(5.0f, 2.0f, m_annoyed);
            enter(target != nullptr ? Mode::Play : Mode::Linger, "looking at what it found, playing with it");
            m_playUntil = time + glm::mix(4.0f, 1.0f, m_annoyed);
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
        else
        {
            if (Flat(m_position, m_targetAt) > m_bodyHeight * 0.6f)
            {
                Steer(world, m_targetAt, m_speed, dt);
            }
            else
            {
                m_velocity *= std::max(1.0f - dt, 0.0f);
                m_position += m_velocity * dt;
            }
        }
        const bool back = target != nullptr && !target->roofed && (m_kind == GiantKind::Walker || target->inWater);
        if (back)
        {
            enter(Mode::Approach, "there it is again");
        }
        else if (time - m_modeSince > 30.0f)
        {
            m_target = -1;
            for (auto& entry : m_aware)
            {
                entry.second = 0.0f;
            }
            enter(Mode::Roam, "lost interest");
            m_haveRoam = false;
        }
        break;
    }
    case Mode::Count:
        break;
    }
    m_shownAt = m_position;
    m_shownYaw = m_yaw;
}

void Giant::TakeDamage(float amount, int byPlayer, float time)
{
    // Felt, not feared: it turns on whoever did it, and plays rougher.
    m_annoyed = std::min(m_annoyed + amount / 250.0f, 1.0f);
    m_health = std::max(m_health - amount / 40000.0f, 0.05f);
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
    return std::find(m_hitboxes.begin(), m_hitboxes.end(), body) != m_hitboxes.end();
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
    if (mode == Mode::Strike && m_mode != Mode::Strike)
    {
        m_struck = false;
    }
    m_shownAt = at;
    m_shownYaw = yaw;
    m_mode = mode;
    m_lookAt = lookAt;
    m_strike = strike;
    m_strikeAt = strikeAt;
}

// --- The body, posed -----------------------------------------------------------------------------------------------------------

void Giant::Animate(const GiantWorld& world, float time, float dt)
{
    if (m_shownOnly)
    {
        // Eased to where the host says, a tenth of a second behind.
        const glm::vec3 before = m_position;
        m_position += (m_shownAt - m_position) * std::min(dt * 6.0f, 1.0f);
        m_yaw = Wrap(m_yaw + Wrap(m_shownYaw - m_yaw) * std::min(dt * 6.0f, 1.0f));
        m_velocity = dt > 0.0f ? (m_position - before) / dt : glm::vec3(0.0f);
        // (A client sees its feet come down too, and hears them.)
        if (m_kind == GiantKind::Walker && m_mode == Mode::Strike && !m_struck && m_strike >= 0.65f)
        {
            m_struck = true;
            m_footfalls.push_back(m_strikeAt);
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

glm::vec3 Giant::FootFor(const GiantWorld& world, const Leg& leg, const glm::vec3& ahead) const
{
    // Where it would put this foot: out from its hip, on the ground, a step ahead of where it is going -- and never on a roof.
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
    const float speed = glm::length(glm::vec2(m_velocity.x, m_velocity.z));
    const float stepTime = 1.1f * std::sqrt(m_bodyHeight / 25.0f);
    const float stride = m_bodyHeight * 0.35f;
    const glm::vec3 ahead = glm::vec3(m_velocity.x, 0.0f, m_velocity.z) * (stepTime * 0.9f);
    // The leg it strikes with: the one nearest where the blow goes.
    int striking = -1;
    if (m_mode == Mode::Strike)
    {
        float nearest = 1.0e9f;
        for (size_t k = 0; k < m_legs.size(); ++k)
        {
            const float d = Flat(m_legs[k].foot, m_strikeAt);
            if (d < nearest)
            {
                nearest = d;
                striking = static_cast<int>(k);
            }
        }
    }
    // Its feet: one at a time (two, far apart, on a body with many), each lifted when it has fallen too far behind where it would
    // go, swung over and put down -- and heard, and felt.
    int stepping = 0;
    for (const Leg& leg : m_legs)
    {
        stepping += leg.step >= 0.0f ? 1 : 0;
    }
    const int atOnce = m_legs.size() >= 5 ? 2 : 1;
    for (size_t k = 0; k < m_legs.size(); ++k)
    {
        Leg& leg = m_legs[k];
        if (static_cast<int>(k) == striking)
        {
            continue;
        }
        if (leg.step >= 0.0f)
        {
            leg.step += dt / stepTime;
            const float s = std::min(leg.step, 1.0f);
            const float eased = s * s * (3.0f - 2.0f * s);
            leg.foot = glm::mix(leg.from, leg.to, eased) + glm::vec3(0.0f, std::sin(s * glm::pi<float>()) * m_bodyHeight * 0.12f, 0.0f);
            if (leg.step >= 1.0f)
            {
                leg.step = -1.0f;
                leg.foot = leg.to;
                m_footfalls.push_back(leg.foot);
            }
            continue;
        }
        const glm::vec3 wanted = FootFor(world, leg, ahead);
        if (stepping < atOnce && Flat(leg.foot, wanted) > (speed > 0.3f ? stride : stride * 0.5f))
        {
            leg.from = leg.foot;
            leg.to = wanted;
            leg.step = 0.0f;
            ++stepping;
        }
    }
    // The striking leg: raised high over where the blow is going, held (seen coming), and driven down onto it.
    if (striking >= 0)
    {
        Leg& leg = m_legs[static_cast<size_t>(striking)];
        const float windup = 1.7f / 2.6f;
        const glm::vec3 ground = glm::vec3(m_strikeAt.x, world.ground ? world.ground(m_strikeAt.x, m_strikeAt.z) : m_strikeAt.y, m_strikeAt.z);
        if (m_strike < windup)
        {
            const float s = m_strike / windup;
            leg.foot = glm::mix(leg.foot, ground + glm::vec3(0.0f, m_bodyHeight * 0.55f, 0.0f), std::min(dt * (1.5f + 3.0f * s), 1.0f));
        }
        else
        {
            leg.foot = glm::mix(leg.foot, ground, std::min(dt * 14.0f, 1.0f));
        }
        leg.from = leg.to = ground;
    }

    // The body over its feet: at its height over the ground under them, rising and dipping with its stride, leaning a little
    // towards the feet that are down, and lowering its front to look at what it plays with.
    glm::vec3 feet{0.0f};
    for (const Leg& leg : m_legs)
    {
        feet += leg.step < 0.0f ? leg.foot : leg.to;
    }
    feet /= static_cast<float>(std::max<size_t>(m_legs.size(), 1));
    m_bob += dt * speed / std::max(stride, 0.5f) * glm::pi<float>();
    const float crouch = (m_mode == Mode::Play || m_mode == Mode::Strike) ? 0.18f : 0.0f;
    glm::vec3 body = glm::vec3(m_position.x, feet.y + m_bodyHeight * (1.0f - crouch) + std::sin(m_bob) * m_bodyHeight * 0.015f, m_position.z);
    const float lean = (m_mode == Mode::Play || m_mode == Mode::Strike) ? 0.18f : 0.04f * std::sin(m_bob * 0.5f);
    const glm::quat bodyTurn = turn * glm::angleAxis(-lean, glm::vec3(1.0f, 0.0f, 0.0f));
    ShowPart(m_body, body, bodyTurn);
    ShowPart(m_bodyGlow, body, bodyTurn);

    // The legs from their hips to their feet, each knee raised high and out.
    for (Leg& leg : m_legs)
    {
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
        ShowPart(leg.thigh, hip, AlongY(knee - hip));
        ShowPart(leg.shin, knee, AlongY(hip + toFoot - knee));
    }

    // Its head, hung at its front: turned (and lowered) towards what it looks at, as far as its neck goes, slowly.
    const glm::vec3 neck = body + bodyTurn * glm::vec3(0.0f, -m_bodyHeight * 0.05f, -m_bodyHeight * 0.22f);
    glm::vec3 look = m_lookAt - neck;
    if (glm::length(look) < 0.5f)
    {
        look = bodyTurn * glm::vec3(0.0f, 0.0f, -1.0f);
    }
    const glm::quat wanted = glm::quatLookAtRH(glm::normalize(look), glm::vec3(0.0f, 1.0f, 0.0f));
    m_headTurn = glm::slerp(m_headTurn, wanted, std::min(dt * 1.6f, 1.0f));
    m_head = neck + m_headTurn * glm::vec3(0.0f, 0.0f, -m_bodyHeight * 0.06f);
    ShowPart(m_headPart, neck, m_headTurn);
    ShowPart(m_eyes, neck, m_headTurn);

    // Its strands, hanging and swinging (each link held to the next), and, playing, the nearest reaching down to what it found.
    for (Strand& strand : m_strands)
    {
        if (strand.points.empty())
        {
            continue;
        }
        strand.points[0] = body + bodyTurn * strand.root;
        for (size_t i = 1; i < strand.points.size(); ++i)
        {
            const glm::vec3 now = strand.points[i];
            strand.points[i] += (now - strand.last[i]) * 0.985f + glm::vec3(0.0f, -9.8f, 0.0f) * dt * dt;
            strand.last[i] = now;
        }
        if (m_mode == Mode::Play)
        {
            glm::vec3& tip = strand.points.back();
            tip += (m_lookAt - tip) * std::min(dt * 0.6f, 1.0f);
        }
        for (int pass = 0; pass < 3; ++pass)
        {
            for (size_t i = 1; i < strand.points.size(); ++i)
            {
                glm::vec3 apart = strand.points[i] - strand.points[i - 1];
                const float length = glm::length(apart);
                if (length > 1e-4f)
                {
                    strand.points[i] = strand.points[i - 1] + apart * (strand.length / length);
                }
            }
        }
        for (size_t i = 0; i < strand.parts.size() && i + 1 < strand.points.size(); ++i)
        {
            ShowPart(strand.parts[i], strand.points[i], AlongY(strand.points[i + 1] - strand.points[i]));
        }
    }

    // Where rounds find it.
    for (size_t i = 0; i < m_hitboxes.size() && i < m_hitboxOn.size(); ++i)
    {
        Transform at;
        const int on = m_hitboxOn[i].first;
        if (on == -2)
        {
            at.position = m_head;
            at.rotation = m_headTurn * glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        }
        else if (on == -1)
        {
            at.position = body;
            at.rotation = bodyTurn * glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        }
        else if (on < static_cast<int>(m_legs.size()))
        {
            const Leg& leg = m_legs[static_cast<size_t>(on)];
            const Transform* shin = m_scene.GetTransform(leg.shin.entity);
            if (shin == nullptr)
            {
                continue;
            }
            at.position = shin->position + shin->rotation * glm::vec3(0.0f, leg.lower * 0.5f, 0.0f);
            at.rotation = shin->rotation;
        }
        m_physics.SetTransform(m_hitboxes[i], at);
    }
    (void)time;
}

void Giant::AnimateSwimmer(const GiantWorld& world, float time, float dt)
{
    (void)world;
    // Its length following the way its head went, segment by segment, and swaying from side to side as it swims -- more towards
    // its tail, faster the faster it goes.
    if (m_trail.empty() || glm::distance(m_trail.front(), m_position) > m_segmentLength * 0.25f)
    {
        m_trail.push_front(m_position);
        while (m_trail.size() > 400)
        {
            m_trail.pop_back();
        }
    }
    const float speed = glm::length(m_velocity);
    m_swimPhase += dt * (0.6f + speed * 0.12f);
    const size_t count = m_segments.size();
    m_spine.assign(count + 1, m_position);
    {
        // Walked back along the trail, a segment's length at a time.
        size_t at = 0;
        glm::vec3 from = m_position;
        for (size_t i = 1; i <= count; ++i)
        {
            float left = m_segmentLength;
            glm::vec3 point = from;
            while (left > 0.0f)
            {
                if (at + 1 >= m_trail.size())
                {
                    // Past the end of where it has been: straight on behind it.
                    point -= Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f) * left;
                    left = 0.0f;
                    break;
                }
                const glm::vec3 next = m_trail[at + 1];
                const float gap = glm::distance(point, next);
                if (gap >= left)
                {
                    point += (next - point) * (left / std::max(gap, 1e-4f));
                    left = 0.0f;
                }
                else
                {
                    left -= gap;
                    point = next;
                    ++at;
                }
            }
            m_spine[i] = point;
            from = point;
        }
    }
    const glm::quat headTurnWanted = glm::quatLookAtRH(glm::length(m_velocity) > 0.2f ? glm::normalize(m_velocity) : Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f),
                                                       glm::vec3(0.0f, 1.0f, 0.0f));
    m_headTurn = glm::slerp(m_headTurn, headTurnWanted, std::min(dt * 2.0f, 1.0f));
    // Its jaw: open as it plays and wide as it strikes.
    const float open = m_mode == Mode::Strike ? glm::smoothstep(0.2f, 0.6f, m_strike) * (1.0f - glm::smoothstep(0.75f, 0.95f, m_strike))
                                              : (m_mode == Mode::Play ? 0.15f + 0.1f * std::sin(time * 1.3f) : 0.04f);
    m_head = m_position + m_headTurn * glm::vec3(0.0f, 0.0f, -m_segmentLength * 1.2f);
    ShowPart(m_headPart, m_position, m_headTurn);
    ShowPart(m_jaw, m_position, m_headTurn * glm::angleAxis(open * 0.7f, glm::vec3(1.0f, 0.0f, 0.0f)));
    ShowPart(m_eyes, m_position, m_headTurn);
    for (size_t i = 0; i < count; ++i)
    {
        const glm::vec3 a = m_spine[i];
        const glm::vec3 b = m_spine[i + 1];
        glm::vec3 along = a - b;
        if (glm::length(along) < 1e-4f)
        {
            along = Yawed(m_yaw) * glm::vec3(0.0f, 0.0f, -1.0f);
        }
        const glm::vec3 forward = glm::normalize(along);
        const glm::vec3 side = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)) + glm::vec3(1e-4f));
        const float t = static_cast<float>(i + 1) / static_cast<float>(count);
        const float sway = std::sin(m_swimPhase * kTau * 0.5f - t * 4.5f) * m_size * 0.03f * t;
        const glm::vec3 middle = (a + b) * 0.5f + side * sway;
        const glm::quat turn = glm::quatLookAtRH(forward, glm::vec3(0.0f, 1.0f, 0.0f));
        ShowPart(m_segments[i], middle, turn);
        ShowPart(m_segmentGlows[i], middle, turn);
    }
    // Where rounds find it.
    for (size_t i = 0; i < m_hitboxes.size() && i < m_hitboxOn.size(); ++i)
    {
        Transform at;
        const int on = m_hitboxOn[i].first;
        const Part* part = on == -2 ? &m_headPart : (on >= 0 && on < static_cast<int>(m_segments.size()) ? &m_segments[static_cast<size_t>(on)] : nullptr);
        const Transform* shown = part != nullptr ? m_scene.GetTransform(part->entity) : nullptr;
        if (shown == nullptr)
        {
            continue;
        }
        at.position = shown->position;
        at.rotation = shown->rotation * glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        m_physics.SetTransform(m_hitboxes[i], at);
    }
}

} // namespace pred
