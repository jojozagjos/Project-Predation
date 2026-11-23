#include "Game/World/Wildlife.h"

#include "Engine/Render/Material.h"
#include "Game/Creature/CreatureTraits.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pred
{

namespace
{

constexpr int kSides = 8;
// The fish drawn at most at once, round the eye; the rest of the mesh's room kept folded away to nothing.
constexpr int kMostFish = 240;
constexpr float kFishCell = 16.0f;

uint32_t Pack(const glm::vec3& c)
{
    const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(c.r) | (channel(c.g) << 8) | (channel(c.b) << 16) | (255u << 24);
}

float Hash(int x, int y, int z, uint32_t salt)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(y) * 0xD8163841u ^ static_cast<uint32_t>(z) * 0xCB1AB31Fu ^
                 salt * 0x9E3779B9u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

void Vertex(MeshData& mesh, const glm::vec3& at, const glm::vec3& normal, const glm::vec3& colour)
{
    MeshVertex vertex;
    vertex.position = at;
    vertex.normal = glm::length(normal) > 1e-5f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
    vertex.color = Pack(colour);
    mesh.vertices.push_back(vertex);
}

// A flat piece seen from both sides (a wing, a tail, a fin): its triangles twice, once each way, each with its own normal.
void TwoSided(MeshData& mesh, const std::vector<glm::vec3>& points, const glm::vec3& colour, const glm::vec3& under)
{
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    if (points.size() >= 3)
    {
        const glm::vec3 n = glm::cross(points[1] - points[0], points[2] - points[0]);
        normal = glm::length(n) > 1e-7f ? glm::normalize(n) : normal;
    }
    for (int side = 0; side < 2; ++side)
    {
        const auto first = static_cast<uint32_t>(mesh.vertices.size());
        for (const glm::vec3& p : points)
        {
            Vertex(mesh, p, side == 0 ? normal : -normal, side == 0 ? colour : under);
        }
        for (uint32_t k = 1; k + 1 < points.size(); ++k)
        {
            if (side == 0)
            {
                mesh.indices.insert(mesh.indices.end(), {first, first + k, first + k + 1});
            }
            else
            {
                mesh.indices.insert(mesh.indices.end(), {first, first + k + 1, first + k});
            }
        }
    }
}

// A body round a line down -z (its nose at the front): rings at `stations` (z, half width, half height, sideways offset), a point at
// each end; its back `dark`, its belly `light`, between them by which way each part faces.
void Body(MeshData& mesh, const std::vector<glm::vec4>& stations, const std::vector<float>& sideways, const glm::vec3& dark,
          const glm::vec3& light)
{
    const auto first = static_cast<uint32_t>(mesh.vertices.size());
    const size_t rings = stations.size();
    for (size_t r = 0; r < rings; ++r)
    {
        const glm::vec4& s = stations[r];
        for (int k = 0; k < kSides; ++k)
        {
            const float a = glm::two_pi<float>() * static_cast<float>(k) / static_cast<float>(kSides);
            const glm::vec3 n{std::cos(a) / std::max(s.y, 1e-4f), std::sin(a) / std::max(s.z, 1e-4f), 0.0f};
            const glm::vec3 at{std::cos(a) * s.y + sideways[r], std::sin(a) * s.z + s.w, s.x};
            const float up = glm::normalize(n + glm::vec3(1e-6f)).y;
            Vertex(mesh, at, n, glm::mix(light, dark, glm::smoothstep(-0.35f, 0.45f, up)));
        }
    }
    for (size_t r = 0; r + 1 < rings; ++r)
    {
        for (int k = 0; k < kSides; ++k)
        {
            const uint32_t a = first + static_cast<uint32_t>(r * kSides + static_cast<size_t>(k));
            const uint32_t b = first + static_cast<uint32_t>(r * kSides + static_cast<size_t>((k + 1) % kSides));
            const uint32_t c = a + kSides;
            const uint32_t d = b + kSides;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    // Its ends closed: a cap at each.
    for (int end = 0; end < 2; ++end)
    {
        const size_t r = end == 0 ? 0 : rings - 1;
        const glm::vec4& s = stations[r];
        const float out = end == 0 ? 1.0f : -1.0f;
        const auto tip = static_cast<uint32_t>(mesh.vertices.size());
        Vertex(mesh, {sideways[r], s.w, s.x + out * std::max(s.y, s.z) * 0.6f}, {0.0f, 0.0f, out}, dark);
        for (int k = 0; k < kSides; ++k)
        {
            const uint32_t a = first + static_cast<uint32_t>(r * kSides + static_cast<size_t>(k));
            const uint32_t b = first + static_cast<uint32_t>(r * kSides + static_cast<size_t>((k + 1) % kSides));
            if (end == 0)
            {
                mesh.indices.insert(mesh.indices.end(), {tip, b, a});
            }
            else
            {
                mesh.indices.insert(mesh.indices.end(), {tip, a, b});
            }
        }
    }
}

// A bird, `span` across its wings, forward down -z: its body and head, a short tail, and two wings each in two parts -- the inner
// raised by `flap` at the shoulder, the outer by half as much again at the elbow (radians, up positive), drawn in by `fold`.
MeshData Bird(float span, float flap, float fold, bool broad, const glm::vec3& dark, const glm::vec3& light)
{
    MeshData mesh;
    const float L = span * 0.4f;
    const float r = span * 0.06f;
    Body(mesh,
         {{0.5f * L, r * 0.25f, r * 0.2f, 0.0f},
          {0.25f * L, r * 0.75f, r * 0.7f, 0.0f},
          {0.0f, r, r * 0.95f, 0.0f},
          {-0.25f * L, r * 0.8f, r * 0.8f, r * 0.15f},
          {-0.4f * L, r * 0.55f, r * 0.6f, r * 0.35f},
          {-0.52f * L, r * 0.12f, r * 0.12f, r * 0.3f}},
         std::vector<float>(6, 0.0f), dark, light);
    // Its tail, fanned behind it.
    TwoSided(mesh, {{0.0f, r * 0.2f, 0.45f * L}, {-r * 1.3f, r * 0.15f, 0.85f * L}, {r * 1.3f, r * 0.15f, 0.85f * L}}, dark, light);
    const float chord = broad ? 0.26f : 0.18f;
    for (const float side : {-1.0f, 1.0f})
    {
        const glm::vec3 shoulder{side * r * 0.6f, r * 0.45f, -0.05f * L};
        const float inner = span * 0.24f * (1.0f - 0.35f * fold);
        const float outer = span * 0.27f * (1.0f - 0.55f * fold);
        const glm::quat up = glm::angleAxis(side * flap, glm::vec3(0.0f, 0.0f, -1.0f));
        const glm::vec3 elbow = shoulder + up * glm::vec3(side * inner, 0.0f, 0.0f);
        const glm::quat upMore = glm::angleAxis(side * flap * 1.5f, glm::vec3(0.0f, 0.0f, -1.0f));
        const glm::vec3 tip = elbow + upMore * glm::vec3(side * outer, 0.0f, span * 0.05f);
        const glm::vec3 lead = glm::vec3(0.0f, 0.0f, -span * chord * 0.45f);
        const glm::vec3 trail = glm::vec3(0.0f, 0.0f, span * chord * 0.55f);
        TwoSided(mesh, {shoulder + lead, elbow + lead * 0.85f, elbow + trail, shoulder + trail}, dark, light);
        TwoSided(mesh, {elbow + lead * 0.85f, tip + (broad ? lead * 0.5f : glm::vec3(0.0f)), tip + trail * (broad ? 0.6f : 0.15f), elbow + trail}, dark,
                 light);
    }
    return mesh;
}

// A fish, `length` long, forward down -z, its body bent by `wag` (a wave down it, more towards its tail) and its tail fin with it.
MeshData Fish(float length, float wag, const glm::vec3& dark, const glm::vec3& light)
{
    MeshData mesh;
    const float L = length;
    const std::vector<float> zs{0.5f * L, 0.32f * L, 0.1f * L, -0.15f * L, -0.35f * L, -0.48f * L};
    std::vector<float> sideways;
    for (const float z : zs)
    {
        const float back = std::clamp(z / L + 0.5f, 0.0f, 1.0f);
        sideways.push_back(std::sin(wag - back * 3.0f) * L * 0.09f * back * back);
    }
    Body(mesh,
         {{zs[0], L * 0.02f, L * 0.035f, 0.0f},
          {zs[1], L * 0.045f, L * 0.09f, 0.0f},
          {zs[2], L * 0.07f, L * 0.13f, 0.0f},
          {zs[3], L * 0.07f, L * 0.12f, 0.0f},
          {zs[4], L * 0.05f, L * 0.08f, 0.0f},
          {zs[5], L * 0.015f, L * 0.025f, 0.0f}},
         sideways, dark, light);
    const float tailSide = sideways[0] * 1.6f;
    const glm::vec3 root{sideways[0], 0.0f, zs[0]};
    TwoSided(mesh, {root, {tailSide, L * 0.16f, 0.72f * L}, {tailSide * 0.9f, 0.0f, 0.62f * L}, {tailSide, -L * 0.16f, 0.72f * L}}, dark, light);
    TwoSided(mesh, {{sideways[3], L * 0.11f, zs[3]}, {sideways[2], L * 0.2f, zs[2] + L * 0.05f}, {sideways[1], L * 0.08f, zs[1]}}, dark, dark);
    return mesh;
}

// Each bird's shape has as many vertices however it is posed: placed into the shared mesh's room for it.
void Place(std::vector<MeshVertex>& out, size_t at, const MeshData& shape, const glm::mat4& placed, AABB& bounds)
{
    const glm::mat3 turn(placed);
    for (size_t k = 0; k < shape.vertices.size() && at + k < out.size(); ++k)
    {
        MeshVertex vertex = shape.vertices[k];
        vertex.position = glm::vec3(placed * glm::vec4(vertex.position, 1.0f));
        vertex.normal = glm::normalize(turn * vertex.normal);
        bounds.Expand(vertex.position);
        out[at + k] = vertex;
    }
}

void Hide(std::vector<MeshVertex>& out, size_t at, size_t count, const glm::vec3& where)
{
    for (size_t k = 0; k < count && at + k < out.size(); ++k)
    {
        out[at + k].position = where;
    }
}

} // namespace

void Wildlife::Build(Scene& scene, MeshLibrary& meshes, const Setup& setup)
{
    Forget();
    m_setup = setup;
    SeededRandom random((static_cast<uint64_t>(setup.seed) << 11) ^ 0xB1D5F15Dull);
    const std::string& b = setup.biome;
    const float chance = !setup.air ? 0.0f
                         : b == "jungle" || b == "oceanic" ? 0.9f
                         : b == "temperate" ? 0.85f
                         : b == "frozen" || b == "rocky" ? 0.5f
                         : b == "desert" ? 0.4f
                         : b == "toxic" ? 0.25f
                                        : 0.0f;
    if (random.Unit() < chance)
    {
        const int flocks = 1 + static_cast<int>(random.Unit() * 2.6f);
        for (int f = 0; f < flocks; ++f)
        {
            Flock flock;
            const float pick = random.Unit();
            // Gulls over the sea and the shore, and over the ice; dark birds and small ones over the land; a hawk high over open country.
            if (setup.water && (b == "oceanic" || pick < 0.35f))
            {
                flock.kind = Kind::Gull;
            }
            else if (b == "frozen")
            {
                flock.kind = pick < 0.6f ? Kind::Gull : Kind::Hawk;
            }
            else if (b == "desert" || b == "rocky")
            {
                flock.kind = pick < 0.5f ? Kind::Hawk : Kind::Crow;
            }
            else
            {
                flock.kind = pick < 0.4f ? Kind::Finch : pick < 0.8f ? Kind::Crow : Kind::Hawk;
            }
            int count = 1;
            switch (flock.kind)
            {
            case Kind::Gull:
                count = 6 + static_cast<int>(random.Unit() * 9.0f);
                flock.wingspan = random.Range(1.1f, 1.4f);
                flock.height = random.Range(20.0f, 45.0f);
                flock.radius = random.Range(60.0f, 120.0f);
                flock.speed = 9.0f;
                flock.spread = 14.0f;
                flock.dark = {0.42f, 0.44f, 0.47f};
                flock.light = {0.86f, 0.86f, 0.84f};
                break;
            case Kind::Crow:
                count = 4 + static_cast<int>(random.Unit() * 7.0f);
                flock.wingspan = random.Range(0.8f, 1.0f);
                flock.height = random.Range(14.0f, 35.0f);
                flock.radius = random.Range(40.0f, 90.0f);
                flock.speed = 10.0f;
                flock.spread = 9.0f;
                flock.dark = {0.04f, 0.04f, 0.05f};
                flock.light = {0.08f, 0.08f, 0.09f};
                break;
            case Kind::Finch:
                count = 10 + static_cast<int>(random.Unit() * 11.0f);
                flock.wingspan = random.Range(0.22f, 0.28f);
                flock.height = random.Range(6.0f, 14.0f);
                flock.radius = random.Range(20.0f, 40.0f);
                flock.speed = 8.0f;
                flock.spread = 3.5f;
                flock.dark = {0.3f, 0.22f, 0.15f};
                flock.light = {0.62f, 0.55f, 0.45f};
                break;
            case Kind::Hawk:
                count = random.Unit() < 0.7f ? 1 : 2;
                flock.wingspan = random.Range(1.6f, 2.0f);
                flock.height = random.Range(60.0f, 110.0f);
                flock.radius = random.Range(70.0f, 140.0f);
                flock.speed = 7.0f;
                flock.spread = 30.0f;
                flock.dark = {0.24f, 0.16f, 0.1f};
                flock.light = {0.6f, 0.5f, 0.38f};
                break;
            }
            const float angle = random.Range(0.0f, glm::two_pi<float>());
            flock.home = setup.middle + glm::vec2(std::cos(angle), std::sin(angle)) * (setup.size * random.Range(0.0f, 0.4f));
            flock.phase = random.Range(0.0f, glm::two_pi<float>()) * (random.Unit() < 0.5f ? 1.0f : -1.0f);
            for (int k = 0; k < count; ++k)
            {
                flock.offsets.emplace_back(random.Range(-1.0f, 1.0f), random.Range(-0.35f, 0.35f), random.Range(-1.0f, 1.0f));
                flock.beats.push_back(random.Range(0.0f, glm::two_pi<float>()));
            }
            m_birdCount += count;
            m_flocks.push_back(std::move(flock));
        }
    }
    if (m_birdCount > 0)
    {
        MeshData all;
        for (const Flock& flock : m_flocks)
        {
            for (size_t k = 0; k < flock.offsets.size(); ++k)
            {
                all.Append(Bird(flock.wingspan, 0.0f, 0.0f, flock.kind == Kind::Hawk, flock.dark, flock.light), glm::mat4(1.0f));
            }
        }
        m_birdVertices = all.vertices;
        m_birdMesh = meshes.CreateDynamic(all, "wildlife_birds_" + std::to_string(setup.seed));
        Material feathers = Material::Diffuse(glm::vec3(1.0f), 0.9f);
        feathers.weathering = 0.0f;
        m_birdEntity = scene.CreateMeshEntity("wildlife_birds", Transform{}, m_birdMesh, feathers);
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_birdEntity))
        {
            renderer->castsShadow = false;
            renderer->farVisible = true;
            // (Spread over the whole site, its box's middle anywhere -- in a building, say, whose rooms are left out from outside.)
            renderer->ignoresHiddenRegions = true;
        }
    }
    if (setup.water)
    {
        MeshData all;
        for (int k = 0; k < kMostFish; ++k)
        {
            all.Append(Fish(1.0f, 0.0f, glm::vec3(0.2f), glm::vec3(0.7f)), glm::mat4(1.0f));
        }
        m_fishVertices = all.vertices;
        m_fishMesh = meshes.CreateDynamic(all, "wildlife_fish_" + std::to_string(setup.seed));
        Material scales = Material::Diffuse(glm::vec3(1.0f), 0.35f);
        scales.weathering = 0.0f;
        m_fishEntity = scene.CreateMeshEntity("wildlife_fish", Transform{}, m_fishMesh, scales);
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_fishEntity))
        {
            renderer->castsShadow = false;
            renderer->farVisible = true;
            renderer->ignoresHiddenRegions = true;
        }
    }
}

void Wildlife::Forget()
{
    m_birdEntity = Entity{};
    m_fishEntity = Entity{};
    m_birdMesh = MeshHandle{};
    m_fishMesh = MeshHandle{};
    m_setup = Setup{};
    m_flocks.clear();
    m_birdCount = 0;
    m_birdVertices.clear();
    m_fishVertices.clear();
}

std::vector<Entity> Wildlife::Entities() const
{
    std::vector<Entity> out;
    for (const Entity entity : {m_birdEntity, m_fishEntity})
    {
        if (entity.IsValid())
        {
            out.push_back(entity);
        }
    }
    return out;
}

std::vector<MeshHandle> Wildlife::Meshes() const
{
    std::vector<MeshHandle> out;
    for (const MeshHandle mesh : {m_birdMesh, m_fishMesh})
    {
        if (mesh.IsValid())
        {
            out.push_back(mesh);
        }
    }
    return out;
}

int Wildlife::Birds() const
{
    return m_birdCount;
}

std::string Wildlife::Describe() const
{
    static const char* const kNames[] = {"gulls", "crows", "finches", "hawks"};
    std::string line;
    for (const Flock& flock : m_flocks)
    {
        line += (line.empty() ? "" : ", ") + std::to_string(flock.offsets.size()) + " " + kNames[static_cast<size_t>(flock.kind)];
    }
    if (m_setup.water)
    {
        line += line.empty() ? "fish" : ", fish";
    }
    return line.empty() ? "none" : line;
}

void Wildlife::Update(Scene& scene, MeshLibrary& meshes, double clock, float dt, const glm::vec3& eye, float daylight, float storm,
                      const std::vector<glm::vec3>& shots)
{
    (void)scene;
    const float t = static_cast<float>(std::fmod(clock, 100000.0));
    // The birds: out by day and not in a storm -- in their roosts then.
    if (m_birdMesh.IsValid())
    {
        const bool out = daylight > 0.15f && storm < 0.6f;
        m_out = out;
        AABB bounds;
        size_t at = 0;
        m_centres.clear();
        for (Flock& flock : m_flocks)
        {
            // A shot near them: up and away, all at once, coming back over the next half a minute.
            for (const glm::vec3& shot : shots)
            {
                const glm::vec3 centre{flock.home.x, shot.y, flock.home.y};
                if (glm::distance(glm::vec2(shot.x, shot.z), flock.home) < flock.radius + 140.0f)
                {
                    flock.scare = 1.0f;
                    glm::vec3 away = centre - shot;
                    away.y = 0.0f;
                    flock.away = glm::length(away) > 1.0f ? glm::normalize(away) : glm::vec3(1.0f, 0.0f, 0.0f);
                }
            }
            flock.scare = std::max(flock.scare - dt / 30.0f, 0.0f);
            const float scared = glm::smoothstep(0.0f, 0.6f, flock.scare);
            // Round its own circle, drifting about the site, a little up and down.
            const float turnRate = flock.speed / flock.radius * (flock.phase < 0.0f ? -1.0f : 1.0f);
            const glm::vec2 drift = flock.home + glm::vec2(std::sin(t * 0.004f + flock.phase), std::cos(t * 0.003f + flock.phase * 1.7f)) * (m_setup.size * 0.25f);
            const auto centreAt = [&](float when)
            {
                const float a = flock.phase + turnRate * when;
                glm::vec3 c{drift.x + std::cos(a) * flock.radius, 0.0f, drift.y + std::sin(a) * flock.radius};
                const float ground = m_setup.ground ? m_setup.ground(c.x, c.z) : 0.0f;
                c.y = ground + flock.height + 5.0f * std::sin(when * 0.05f + flock.phase);
                return c + flock.away * (70.0f * scared) + glm::vec3(0.0f, 25.0f * scared, 0.0f);
            };
            const glm::vec3 centre = centreAt(t);
            m_centres.push_back(centre);
            glm::vec3 heading = centreAt(t + 0.25f) - centre;
            heading = glm::length(heading) > 1e-4f ? glm::normalize(heading) : glm::vec3(0.0f, 0.0f, -1.0f);
            const float yaw = std::atan2(-heading.x, -heading.z);
            const glm::quat facing = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            for (size_t k = 0; k < flock.offsets.size(); ++k)
            {
                const float i = static_cast<float>(k);
                const glm::vec3 wander{std::sin(t * 0.3f + i * 1.3f), 0.5f * std::sin(t * 0.4f + i * 2.1f), std::cos(t * 0.35f + i * 0.7f)};
                const glm::vec3 offset = (flock.offsets[k] + wander * 0.3f) * (flock.spread * (1.0f + 1.5f * scared));
                const glm::vec3 position = centre + facing * offset;
                // Its wings: gulls and hawks gliding as much as beating, crows beating steadily, small birds in quick bursts; all of them
                // beating hard when put up.
                float flap = 0.0f;
                float fold = 0.0f;
                const float beat = flock.beats[k];
                switch (flock.kind)
                {
                case Kind::Gull:
                    flap = 0.1f + 0.55f * std::sin(t * 6.0f + beat) * glm::smoothstep(0.2f, 0.7f, std::sin(t * 0.3f + beat) + scared);
                    break;
                case Kind::Crow:
                    flap = 0.05f + 0.6f * std::sin(t * 7.5f + beat);
                    break;
                case Kind::Finch:
                {
                    const bool burst = std::sin(t * 2.2f + beat) > -0.2f || scared > 0.3f;
                    flap = burst ? 0.1f + 0.7f * std::sin(t * 18.0f + beat) : 0.0f;
                    fold = burst ? 0.0f : 0.9f;
                    break;
                }
                case Kind::Hawk:
                    flap = 0.14f + 0.04f * std::sin(t * 0.8f + beat) + scared * 0.5f * std::sin(t * 5.0f + beat);
                    break;
                }
                // (Banked into its turn.)
                const glm::quat bank = glm::angleAxis(-0.35f * (turnRate > 0.0f ? 1.0f : -1.0f), glm::vec3(0.0f, 0.0f, -1.0f));
                const glm::mat4 placed = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(facing * bank);
                const MeshData shape = Bird(flock.wingspan, flap, fold, flock.kind == Kind::Hawk, flock.dark, flock.light);
                if (out)
                {
                    Place(m_birdVertices, at, shape, placed, bounds);
                }
                else
                {
                    Hide(m_birdVertices, at, shape.vertices.size(), position + glm::vec3(0.0f, -1000.0f, 0.0f));
                    bounds.Expand(position);
                }
                at += shape.vertices.size();
            }
        }
        meshes.UpdateDynamic(m_birdMesh, m_birdVertices, bounds);
    }
    // The fish: schools in the water round the eye, a school to some cells of a grid there, each circling its own spot, turning
    // together; each fish sent off a little way from somebody swimming at it.
    if (m_fishMesh.IsValid())
    {
        AABB bounds;
        size_t at = 0;
        int drawn = 0;
        const size_t each = m_fishVertices.size() / static_cast<size_t>(kMostFish);
        const glm::ivec3 middle = glm::ivec3(glm::floor(eye / kFishCell));
        for (int z = -2; z <= 2 && drawn < kMostFish; ++z)
        {
            for (int y = -1; y <= 1 && drawn < kMostFish; ++y)
            {
                for (int x = -2; x <= 2 && drawn < kMostFish; ++x)
                {
                    const glm::ivec3 cell = middle + glm::ivec3(x, y, z);
                    if (Hash(cell.x, cell.y, cell.z, 1u) > 0.25f)
                    {
                        continue;
                    }
                    const glm::vec3 home = (glm::vec3(cell) + glm::vec3(Hash(cell.x, cell.y, cell.z, 2u), Hash(cell.x, cell.y, cell.z, 3u), Hash(cell.x, cell.y, cell.z, 4u))) * kFishCell;
                    float surface = 0.0f;
                    const float depth = m_setup.depth ? m_setup.depth(home, surface) : 0.0f;
                    const float floor = m_setup.ground ? m_setup.ground(home.x, home.z) : home.y - 10.0f;
                    if (depth < 1.5f || home.y > surface - 1.0f || home.y < floor + 1.0f)
                    {
                        continue;
                    }
                    const float length = 0.2f + 0.45f * Hash(cell.x, cell.y, cell.z, 5u);
                    const glm::vec3 dark = glm::mix(glm::vec3(0.12f, 0.16f, 0.2f), glm::vec3(0.22f, 0.2f, 0.12f), Hash(cell.x, cell.y, cell.z, 6u));
                    const glm::vec3 light{0.72f, 0.75f, 0.76f};
                    const float radius = 1.5f + 3.0f * Hash(cell.x, cell.y, cell.z, 7u);
                    const float speed = (0.2f + 0.3f * Hash(cell.x, cell.y, cell.z, 8u)) * (Hash(cell.x, cell.y, cell.z, 9u) < 0.5f ? 1.0f : -1.0f);
                    const int count = 8 + static_cast<int>(Hash(cell.x, cell.y, cell.z, 10u) * 12.0f);
                    for (int f = 0; f < count && drawn < kMostFish; ++f)
                    {
                        const auto own = static_cast<uint32_t>(f) * 7919u + 11u;
                        const float lane = radius * (0.6f + 0.7f * Hash(cell.x, cell.y, cell.z, own));
                        const float angle = t * speed / std::max(lane, 0.5f) * 1.5f + Hash(cell.x, cell.y, cell.z, own + 1u) * 1.2f;
                        glm::vec3 position = home + glm::vec3(std::cos(angle) * lane, 0.8f * (Hash(cell.x, cell.y, cell.z, own + 2u) - 0.5f) + 0.2f * std::sin(t * 0.6f + static_cast<float>(f)),
                                                              std::sin(angle) * lane);
                        glm::vec3 way = glm::normalize(glm::vec3(-std::sin(angle), 0.0f, std::cos(angle)) * (speed > 0.0f ? 1.0f : -1.0f));
                        // (Off from somebody close: pushed away, turned away, quicker.)
                        const glm::vec3 fromEye = position - eye;
                        const float near = glm::length(fromEye);
                        float quick = 1.0f;
                        if (near < 4.0f && near > 1e-3f)
                        {
                            position += fromEye / near * (4.0f - near) * 1.2f;
                            way = glm::normalize(glm::mix(way, fromEye / near, 0.7f));
                            quick = 2.5f;
                        }
                        if (glm::length(position - eye) > kFishCell * 2.5f)
                        {
                            continue;
                        }
                        const float yaw = std::atan2(-way.x, -way.z);
                        const glm::mat4 placed = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
                        const MeshData shape = Fish(length, t * 9.0f * quick + static_cast<float>(f), dark, light);
                        Place(m_fishVertices, at, shape, placed, bounds);
                        at += each;
                        ++drawn;
                    }
                }
            }
        }
        // The rest of its room folded away out of sight, under the eye.
        Hide(m_fishVertices, at, m_fishVertices.size() - at, eye + glm::vec3(0.0f, -500.0f, 0.0f));
        bounds.Expand(eye);
        meshes.UpdateDynamic(m_fishMesh, m_fishVertices, bounds);
    }
}

} // namespace pred
