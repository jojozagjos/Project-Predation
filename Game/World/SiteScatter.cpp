#include "Game/World/SiteScatter.h"

#include "Game/World/SitePlan.h"
#include "Game/World/SiteTerrain.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pred
{

namespace SiteScatter
{

namespace
{

constexpr float kTau = 6.28318530718f;

// SplitMix64, as the sites are planned with: the same sequence on every machine.
struct Random
{
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    float Range(float a, float b) { return a + (b - a) * Unit(); }
    int Int(int a, int b) { return a + static_cast<int>(Unit() * static_cast<float>(b - a + 1) * 0.99999f); }
    bool Chance(float p) { return Unit() < p; }
    // About normal, mean 0 and spread 1.
    float Normal() { return (Unit() + Unit() + Unit() + Unit() - 2.0f) * 1.7f; }
};

// --- Shapes ------------------------------------------------------------------------------------------------------------

// A flat triangle, its face's own way out.
void Triangle(MeshData& mesh, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, uint32_t colour = 0xffffffffu)
{
    glm::vec3 n = glm::cross(b - a, c - a);
    const float length = glm::length(n);
    if (length < 1.0e-9f)
    {
        return;
    }
    n /= length;
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& p : {a, b, c})
    {
        MeshVertex vertex;
        vertex.position = p;
        vertex.normal = n;
        vertex.color = colour;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}

// A lump: a rough sphere of radius about 1 round `centre`, squashed by `squash`, each corner pushed in or out a little (by
// `rough`), its faces flat -- a boulder's, a shrub's, a canopy's. The bottom cut flat at `floor` (local, before squashing).
void Lump(MeshData& mesh, Random& random, const glm::vec3& centre, const glm::vec3& squash, float rough, float floor = -2.0f, uint32_t colour = 0xffffffffu)
{
    // An icosahedron, each face split in four once: 80 faces.
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<glm::vec3> points = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                     {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::array<int, 3>> faces = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                              {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                                              {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (glm::vec3& p : points)
    {
        p = glm::normalize(p);
    }
    std::vector<std::array<int, 3>> finer;
    const auto middle = [&](int a, int b)
    {
        points.push_back(glm::normalize(points[static_cast<size_t>(a)] + points[static_cast<size_t>(b)]));
        return static_cast<int>(points.size()) - 1;
    };
    for (const auto& f : faces)
    {
        const int ab = middle(f[0], f[1]);
        const int bc = middle(f[1], f[2]);
        const int ca = middle(f[2], f[0]);
        finer.push_back({f[0], ab, ca});
        finer.push_back({f[1], bc, ab});
        finer.push_back({f[2], ca, bc});
        finer.push_back({ab, bc, ca});
    }
    // Each point pushed by how far it is along a few random directions: a lump, not a ball with noise on it. Points made more
    // than once (the split edges' middles) come out the same, since this depends on where a point is, not which it is.
    std::array<glm::vec4, 5> pushes;
    for (glm::vec4& push : pushes)
    {
        push = glm::vec4(glm::normalize(glm::vec3(random.Normal(), random.Normal(), random.Normal()) + glm::vec3(0.0f, 1.0e-3f, 0.0f)), random.Range(-1.0f, 1.0f));
    }
    const auto shaped = [&](const glm::vec3& p)
    {
        float r = 1.0f;
        for (const glm::vec4& push : pushes)
        {
            r += rough * push.w * std::max(glm::dot(p, glm::vec3(push)), 0.0f);
        }
        glm::vec3 q = p * std::max(r, 0.4f);
        q.y = std::max(q.y, floor);
        return centre + q * squash;
    };
    for (const auto& f : finer)
    {
        Triangle(mesh, shaped(points[static_cast<size_t>(f[0])]), shaped(points[static_cast<size_t>(f[1])]), shaped(points[static_cast<size_t>(f[2])]), colour);
    }
}

// A prism of `sides` from `foot` along `up` (unit), `length` long, radius `r0` at the foot and `r1` at the top, a point
// `point` beyond its top (0 for a flat top), turned by `turn` about its axis.
void Prism(MeshData& mesh, const glm::vec3& foot, const glm::vec3& up, float length, float r0, float r1, int sides, float point, float turn,
           uint32_t colour = 0xffffffffu)
{
    const glm::vec3 across0 = glm::normalize(glm::cross(up, std::abs(up.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::vec3 across1 = glm::cross(up, across0);
    const glm::vec3 top = foot + up * length;
    std::vector<glm::vec3> low;
    std::vector<glm::vec3> high;
    for (int i = 0; i < sides; ++i)
    {
        const float a = turn + kTau * static_cast<float>(i) / static_cast<float>(sides);
        const glm::vec3 d = across0 * std::cos(a) + across1 * std::sin(a);
        low.push_back(foot + d * r0);
        high.push_back(top + d * r1);
    }
    for (int i = 0; i < sides; ++i)
    {
        const int j = (i + 1) % sides;
        Triangle(mesh, low[static_cast<size_t>(i)], low[static_cast<size_t>(j)], high[static_cast<size_t>(j)], colour);
        Triangle(mesh, low[static_cast<size_t>(i)], high[static_cast<size_t>(j)], high[static_cast<size_t>(i)], colour);
        if (point > 0.0f)
        {
            Triangle(mesh, high[static_cast<size_t>(i)], high[static_cast<size_t>(j)], top + up * point, colour);
        }
        else
        {
            Triangle(mesh, high[static_cast<size_t>(i)], high[static_cast<size_t>(j)], top, colour);
        }
        // Closed underneath, so a cone of leaves overhead is not seen through from below.
        Triangle(mesh, low[static_cast<size_t>(j)], low[static_cast<size_t>(i)], foot, colour);
    }
}

// Colours painted on a shape's vertices, in linear space, multiplied into its material's: for a tree's trunk and its leaves
// to be one mesh.
uint32_t Paint(const glm::vec3& c)
{
    const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(c.r) | (byte(c.g) << 8) | (byte(c.b) << 16) | (0xffu << 24);
}

// --- What lies where ---------------------------------------------------------------------------------------------------

// How much of each kind a world has, per hectare, in clusters of about so many.
struct Share
{
    Kind kind;
    float perHectare;
    int cluster;
};

std::vector<Share> SharesOf(const std::string& biome)
{
    if (biome == "frozen")
    {
        return {{Kind::IceShard, 22.0f, 8}, {Kind::Boulder, 22.0f, 6}, {Kind::Spire, 2.5f, 3}};
    }
    if (biome == "crystalline")
    {
        return {{Kind::Crystal, 26.0f, 7}, {Kind::Boulder, 18.0f, 5}, {Kind::Spire, 4.0f, 3}};
    }
    if (biome == "volcanic")
    {
        return {{Kind::Basalt, 12.0f, 4}, {Kind::Boulder, 28.0f, 7}, {Kind::Spire, 4.0f, 3}};
    }
    if (biome == "temperate")
    {
        return {{Kind::Tree, 9.0f, 6}, {Kind::Shrub, 24.0f, 8}, {Kind::Grass, 90.0f, 14}, {Kind::Boulder, 5.0f, 3}};
    }
    if (biome == "jungle")
    {
        return {{Kind::Tree, 20.0f, 9}, {Kind::Shrub, 40.0f, 10}, {Kind::Grass, 120.0f, 16}, {Kind::Boulder, 3.0f, 2}};
    }
    if (biome == "oceanic")
    {
        return {{Kind::Shrub, 16.0f, 7}, {Kind::Grass, 70.0f, 12}, {Kind::Boulder, 8.0f, 4}, {Kind::Tree, 3.0f, 3}};
    }
    if (biome == "desert")
    {
        return {{Kind::Boulder, 18.0f, 5}, {Kind::DeadTree, 4.0f, 2}, {Kind::Shrub, 8.0f, 4}, {Kind::Spire, 3.0f, 2}};
    }
    if (biome == "toxic")
    {
        return {{Kind::Spire, 9.0f, 5}, {Kind::DeadTree, 7.0f, 4}, {Kind::Boulder, 18.0f, 5}};
    }
    if (biome == "stormworld")
    {
        return {{Kind::Boulder, 34.0f, 7}, {Kind::Spire, 5.0f, 3}};
    }
    if (biome == "barren")
    {
        return {{Kind::Boulder, 40.0f, 8}, {Kind::Spire, 1.5f, 2}};
    }
    // Rocky, and anything not known.
    return {{Kind::Boulder, 34.0f, 7}, {Kind::Spire, 4.0f, 3}};
}

// How big one is (metres), and how much it may vary.
glm::vec3 SizeOf(Kind kind, Random& random)
{
    switch (kind)
    {
    case Kind::Boulder:
    {
        // Mostly stones and rocks, now and then something to hide behind.
        const float s = random.Chance(0.22f) ? random.Range(2.0f, 5.0f) : random.Range(0.5f, 1.8f);
        return {s * random.Range(0.8f, 1.3f), s * random.Range(0.55f, 0.95f), s * random.Range(0.8f, 1.3f)};
    }
    case Kind::Spire:
    {
        const float h = random.Range(3.0f, 9.0f);
        return {h * 0.35f, h, h * 0.35f};
    }
    case Kind::IceShard:
    {
        const float h = random.Range(1.2f, 6.0f);
        return {h * 0.4f, h, h * 0.4f};
    }
    case Kind::Crystal:
    {
        const float h = random.Range(1.0f, 4.0f);
        return glm::vec3(h);
    }
    case Kind::Basalt:
    {
        const float h = random.Range(1.5f, 4.5f);
        return {random.Range(2.0f, 3.5f), h, random.Range(2.0f, 3.5f)};
    }
    case Kind::Shrub:
    {
        const float s = random.Range(0.5f, 1.4f);
        return {s, s * random.Range(0.6f, 0.9f), s};
    }
    case Kind::Tree:
    {
        const float h = random.Range(5.0f, 12.0f);
        return {h, h, h};
    }
    case Kind::DeadTree:
    {
        const float h = random.Range(2.5f, 6.0f);
        return {h, h, h};
    }
    case Kind::Grass:
    {
        const float s = random.Range(0.3f, 0.7f);
        return {s, s * random.Range(0.8f, 1.3f), s};
    }
    default: return glm::vec3(1.0f);
    }
}

// How steep a slope it can stand on (the ground's normal's height at least this).
float SteepestFor(Kind kind)
{
    switch (kind)
    {
    case Kind::Tree:
    case Kind::Grass:
    case Kind::Shrub: return 0.85f;
    case Kind::Basalt:
    case Kind::Crystal: return 0.8f;
    default: return 0.7f;
    }
}

} // namespace

std::vector<Item> Plan(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain)
{
    std::vector<Item> items;
    Random random((static_cast<uint64_t>(seed) << 17) ^ 0x5CA77E2ull);
    const float hectares = plan.size * plan.size / 10000.0f;
    for (const Share& share : SharesOf(biome))
    {
        const int total = static_cast<int>(share.perHectare * hectares * random.Range(0.75f, 1.25f));
        const int clusters = std::max(1, total / std::max(share.cluster, 1));
        // The spread of a cluster: tight for grass, wide for trees.
        const float spread = share.kind == Kind::Grass ? 2.5f : share.kind == Kind::Tree ? 9.0f : share.kind == Kind::Boulder ? 6.0f : 4.0f;
        for (int c = 0; c < clusters; ++c)
        {
            const glm::vec2 centre{plan.origin.x + random.Range(0.0f, plan.size), plan.origin.z + random.Range(0.0f, plan.size)};
            const int count = std::max(1, static_cast<int>(static_cast<float>(share.cluster) * random.Range(0.5f, 1.5f)));
            for (int i = 0; i < count; ++i)
            {
                const glm::vec2 at = centre + glm::vec2(random.Normal(), random.Normal()) * spread;
                // On the world's own ground, well clear of anything levelled, and on a slope it could stand on.
                if (at.x < plan.origin.x || at.y < plan.origin.z || at.x > plan.origin.x + plan.size || at.y > plan.origin.z + plan.size ||
                    terrain.Wildness(at.x, at.y) < 0.6f || terrain.Normal(at.x, at.y).y < SteepestFor(share.kind))
                {
                    continue;
                }
                Item item;
                item.kind = share.kind;
                item.variant = random.Int(0, kVariants - 1);
                item.scale = SizeOf(share.kind, random);
                item.yaw = random.Range(0.0f, kTau);
                item.tip = share.kind == Kind::IceShard || share.kind == Kind::Spire ? random.Range(-0.25f, 0.25f) : 0.0f;
                // Sunk a little, so it sits in the ground rather than on it: the most where the ground falls away under it.
                const float half = std::max(item.scale.x, item.scale.z) * 0.45f;
                float low = terrain.Height(at.x, at.y);
                for (const glm::vec2 corner : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
                {
                    low = std::min(low, terrain.Height(at.x + corner.x * half, at.y + corner.y * half));
                }
                const float sink = share.kind == Kind::Boulder ? item.scale.y * 0.18f : 0.08f;
                item.at = {at.x, low - sink, at.y};
                items.push_back(item);
            }
        }
    }
    return items;
}

MeshData Shape(Kind kind, int variant, uint32_t seed, const glm::vec3& growth)
{
    MeshData mesh;
    Random random((static_cast<uint64_t>(seed) << 9) ^ (static_cast<uint64_t>(kind) << 4) ^ static_cast<uint64_t>(variant) ^ 0x5A4E5ull);
    switch (kind)
    {
    case Kind::Boulder:
        // About a metre across and as high as it is squashed (scale does the rest): half of it below its middle.
        Lump(mesh, random, {0.0f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.5f}, random.Range(0.35f, 0.6f), -0.55f);
        break;
    case Kind::Spire:
    {
        // A column of rock, leaning a little, a broken shoulder on it.
        const glm::vec3 lean = glm::normalize(glm::vec3(random.Range(-0.15f, 0.15f), 1.0f, random.Range(-0.15f, 0.15f)));
        Prism(mesh, {0.0f, -0.1f, 0.0f}, lean, 0.75f, 0.5f, 0.3f, random.Int(5, 7), random.Range(0.05f, 0.25f), random.Range(0.0f, kTau));
        Lump(mesh, random, glm::vec3(random.Range(-0.2f, 0.2f), 0.25f, random.Range(-0.2f, 0.2f)), {0.45f, 0.3f, 0.45f}, 0.5f, -1.0f);
        break;
    }
    case Kind::IceShard:
    {
        // A main blade and two or three smaller round it, all leaning out.
        const int blades = random.Int(2, 4);
        for (int i = 0; i < blades; ++i)
        {
            const float a = random.Range(0.0f, kTau);
            const float out = i == 0 ? 0.08f : random.Range(0.25f, 0.55f);
            const glm::vec3 up = glm::normalize(glm::vec3(std::cos(a) * out, 1.0f, std::sin(a) * out));
            const float length = i == 0 ? 1.0f : random.Range(0.35f, 0.7f);
            const float r = length * random.Range(0.18f, 0.26f);
            Prism(mesh, {std::cos(a) * 0.1f * static_cast<float>(i), -0.1f, std::sin(a) * 0.1f * static_cast<float>(i)}, up, length * 0.8f, r, r * 0.55f, 4,
                  length * 0.3f, random.Range(0.0f, kTau));
        }
        break;
    }
    case Kind::Crystal:
    {
        // Six-sided, pointed, from one root, fanned out.
        const int count = random.Int(4, 7);
        for (int i = 0; i < count; ++i)
        {
            const float a = random.Range(0.0f, kTau);
            const float out = i == 0 ? 0.1f : random.Range(0.3f, 0.9f);
            const glm::vec3 up = glm::normalize(glm::vec3(std::cos(a) * out, 1.0f, std::sin(a) * out));
            const float length = i == 0 ? 0.8f : random.Range(0.25f, 0.6f);
            const float r = length * random.Range(0.14f, 0.2f);
            Prism(mesh, {0.0f, -0.05f, 0.0f}, up, length, r, r, 6, r * 1.6f, random.Range(0.0f, kTau));
        }
        break;
    }
    case Kind::Basalt:
    {
        // Columns packed together, six-sided, flat-topped, their tops at different heights.
        const float r = 0.17f;
        for (int q = -2; q <= 2; ++q)
        {
            for (int s = -2; s <= 2; ++s)
            {
                const glm::vec2 at{(static_cast<float>(q) + static_cast<float>(s) * 0.5f) * r * 1.73f, static_cast<float>(s) * r * 1.5f};
                if (glm::length(at) > 0.5f || random.Chance(0.15f))
                {
                    continue;
                }
                const float high = random.Range(0.35f, 1.0f) * (1.0f - glm::length(at) * 0.8f);
                Prism(mesh, {at.x, -0.2f, at.y}, {0.0f, 1.0f, 0.0f}, high + 0.2f, r, r, 6, 0.0f, 0.5236f);
            }
        }
        break;
    }
    case Kind::Shrub:
    {
        // A few lumps run together, low, darker underneath.
        const uint32_t colour = Paint(growth * random.Range(0.85f, 1.05f));
        const int lumps = random.Int(3, 5);
        for (int i = 0; i < lumps; ++i)
        {
            const glm::vec3 at{random.Range(-0.25f, 0.25f), random.Range(0.2f, 0.4f), random.Range(-0.25f, 0.25f)};
            Lump(mesh, random, at, glm::vec3(random.Range(0.25f, 0.4f), random.Range(0.2f, 0.32f), random.Range(0.25f, 0.4f)), 0.4f, -0.6f, colour);
        }
        break;
    }
    case Kind::Tree:
    {
        // A trunk, and leaves on it: a broad crown of lumps, or (one in two) tiers of cones.
        const uint32_t bark = Paint({0.26f, 0.2f, 0.15f});
        const uint32_t leaves = Paint(growth * random.Range(0.85f, 1.05f));
        const float trunk = random.Range(0.035f, 0.05f);
        // Up into the crown, never short of it.
        Prism(mesh, {0.0f, -0.1f, 0.0f}, glm::normalize(glm::vec3(random.Range(-0.04f, 0.04f), 1.0f, random.Range(-0.04f, 0.04f))), 0.82f, trunk * 1.3f, trunk * 0.7f,
              6, 0.0f, 0.0f, bark);
        if (variant % 2 == 0)
        {
            for (int i = 0; i < random.Int(3, 5); ++i)
            {
                const glm::vec3 at{random.Range(-0.1f, 0.1f), random.Range(0.56f, 0.76f), random.Range(-0.1f, 0.1f)};
                Lump(mesh, random, at, glm::vec3(random.Range(0.16f, 0.24f), random.Range(0.13f, 0.18f), random.Range(0.16f, 0.24f)), 0.35f, -2.0f, leaves);
            }
        }
        else
        {
            for (int tier = 0; tier < 4; ++tier)
            {
                const float y = 0.28f + 0.17f * static_cast<float>(tier);
                const float r = 0.26f - 0.05f * static_cast<float>(tier);
                Prism(mesh, {0.0f, y, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.01f, r, r, 7, 0.24f, random.Range(0.0f, kTau), leaves);
            }
        }
        break;
    }
    case Kind::DeadTree:
    {
        // A bare trunk and a few bare branches, thin.
        const float trunk = random.Range(0.03f, 0.045f);
        const glm::vec3 lean = glm::normalize(glm::vec3(random.Range(-0.12f, 0.12f), 1.0f, random.Range(-0.12f, 0.12f)));
        Prism(mesh, {0.0f, -0.1f, 0.0f}, lean, 0.9f, trunk * 1.4f, trunk * 0.4f, 5, 0.0f, 0.0f);
        for (int i = 0; i < random.Int(3, 5); ++i)
        {
            const float a = random.Range(0.0f, kTau);
            const float y = random.Range(0.35f, 0.75f);
            const glm::vec3 from = lean * y;
            const glm::vec3 up = glm::normalize(glm::vec3(std::cos(a), random.Range(0.4f, 1.0f), std::sin(a)));
            Prism(mesh, from, up, random.Range(0.15f, 0.35f), trunk * 0.6f, trunk * 0.15f, 4, 0.0f, 0.0f);
        }
        break;
    }
    case Kind::Grass:
    {
        // Blades from one clump, leaning out: thin wedges, each with two faces so it shows from either side.
        const int blades = random.Int(7, 12);
        for (int i = 0; i < blades; ++i)
        {
            const float a = random.Range(0.0f, kTau);
            const glm::vec3 foot{std::cos(a) * random.Range(0.0f, 0.15f), -0.05f, std::sin(a) * random.Range(0.0f, 0.15f)};
            const glm::vec3 tip = foot + glm::vec3(std::cos(a) * random.Range(0.1f, 0.35f), random.Range(0.6f, 1.0f), std::sin(a) * random.Range(0.1f, 0.35f));
            const glm::vec3 side = glm::normalize(glm::vec3(-std::sin(a), 0.0f, std::cos(a))) * random.Range(0.02f, 0.04f);
            const uint32_t colour = Paint(growth * random.Range(0.75f, 1.05f));
            Triangle(mesh, foot - side, foot + side, tip, colour);
            Triangle(mesh, foot + side, foot - side, tip, colour);
        }
        break;
    }
    default: break;
    }
    return mesh;
}

bool Solid(Kind kind, const glm::vec3& scale)
{
    switch (kind)
    {
    case Kind::Boulder: return std::max(scale.x, scale.z) > 1.1f;
    case Kind::Spire:
    case Kind::Basalt: return true;
    case Kind::IceShard:
    case Kind::Crystal: return scale.y > 1.6f;
    case Kind::Tree: return true;
    default: return false;
    }
}

} // namespace SiteScatter

} // namespace pred
