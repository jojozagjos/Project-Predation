#include "Game/World/SiteScatter.h"

#include "Game/World/SitePlan.h"
#include "Game/World/SiteTerrain.h"
#include "Game/World/Foliage.h"

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

// A tube through `points`, its radius at each, `sides` round, smooth-shaded: a trunk, a bough, a branch.
void Tube(MeshData& mesh, const std::vector<glm::vec3>& points, const std::vector<float>& radii, int sides, uint32_t colour = 0xffffffffu)
{
    if (points.size() < 2)
    {
        return;
    }
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    glm::vec3 lastSide{0.0f};
    for (size_t i = 0; i < points.size(); ++i)
    {
        const glm::vec3 way = glm::normalize(i + 1 < points.size() ? points[i + 1] - points[i] : points[i] - points[i - 1]);
        // A frame carried along it, so the sides do not twist.
        glm::vec3 side = i == 0 ? glm::cross(way, std::abs(way.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)) : lastSide;
        side = glm::normalize(side - way * glm::dot(side, way));
        lastSide = side;
        const glm::vec3 other = glm::cross(way, side);
        for (int k = 0; k <= sides; ++k)
        {
            const float a = kTau * static_cast<float>(k) / static_cast<float>(sides);
            const glm::vec3 out = side * std::cos(a) + other * std::sin(a);
            MeshVertex vertex;
            vertex.position = points[i] + out * radii[i];
            vertex.normal = out;
            vertex.color = colour;
            vertex.uv = {static_cast<float>(k) / static_cast<float>(sides), static_cast<float>(i)};
            mesh.vertices.push_back(vertex);
        }
    }
    const auto ring = static_cast<uint32_t>(sides + 1);
    for (uint32_t i = 0; i + 1 < points.size(); ++i)
    {
        for (uint32_t k = 0; k < static_cast<uint32_t>(sides); ++k)
        {
            const uint32_t a = base + i * ring + k;
            const uint32_t b = a + ring;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
}

// A bough from `from` going `way`, `length` long, from radius `r0` to `r1`, bending as it goes by `bend` (added to its way
// per unit of its length, a gravity or a reach for light): its points, for Tube.
void Bough(MeshData& mesh, glm::vec3 from, glm::vec3 way, float length, float r0, float r1, const glm::vec3& bend, uint32_t colour,
           std::vector<glm::vec3>* ends = nullptr)
{
    std::vector<glm::vec3> points{from};
    std::vector<float> radii{r0};
    const int steps = 5;
    for (int i = 1; i <= steps; ++i)
    {
        way = glm::normalize(way + bend * (length / static_cast<float>(steps)));
        from += way * (length / static_cast<float>(steps));
        points.push_back(from);
        radii.push_back(r0 + (r1 - r0) * static_cast<float>(i) / static_cast<float>(steps));
    }
    Tube(mesh, points, radii, 6, colour);
    if (ends != nullptr)
    {
        ends->push_back(from);
    }
}

// A card: a flat quad round `centre`, `right` and `up` its half-sides, showing the atlas's `cell` (its top row up `up`),
// painted `colour`.
void Card(MeshData& mesh, const glm::vec3& centre, const glm::vec3& right, const glm::vec3& up, int cell, uint32_t colour)
{
    const glm::vec4 uv = Foliage::Cell(cell);
    const glm::vec3 normal = glm::normalize(glm::cross(right, up));
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    const glm::vec3 corners[4] = {centre - right - up, centre + right - up, centre + right + up, centre - right + up};
    const glm::vec2 uvs[4] = {{uv.x, uv.w}, {uv.z, uv.w}, {uv.z, uv.y}, {uv.x, uv.y}};
    for (int i = 0; i < 4; ++i)
    {
        MeshVertex vertex;
        vertex.position = corners[i];
        vertex.normal = normal;
        vertex.uv = uvs[i];
        vertex.color = colour;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

// A random way, as a unit vector.
glm::vec3 Somewhere(Random& random)
{
    return glm::normalize(glm::vec3(random.Normal(), random.Normal(), random.Normal()) + glm::vec3(0.0f, 1.0e-3f, 0.0f));
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
        // Woods: groves of trees, close together, with open ground between.
        return {{Kind::Tree, 26.0f, 16}, {Kind::Shrub, 30.0f, 8}, {Kind::Grass, 90.0f, 14}, {Kind::Boulder, 5.0f, 3}};
    }
    if (biome == "jungle")
    {
        // Dense forest.
        return {{Kind::Tree, 55.0f, 24}, {Kind::Shrub, 60.0f, 12}, {Kind::Grass, 120.0f, 16}, {Kind::Boulder, 3.0f, 2}};
    }
    if (biome == "oceanic")
    {
        return {{Kind::Shrub, 16.0f, 7}, {Kind::Grass, 70.0f, 12}, {Kind::Boulder, 8.0f, 4}, {Kind::Tree, 8.0f, 6}};
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

// --- Woods ---------------------------------------------------------------------------------------------------------------

// A value at each whole point from a hash of it, eased between: smooth noise, 0 to 1.
float Lattice(int x, int z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(z) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

float Smooth(float x, float z, uint32_t seed)
{
    const float fx = std::floor(x);
    const float fz = std::floor(z);
    const int ix = static_cast<int>(fx);
    const int iz = static_cast<int>(fz);
    float tx = x - fx;
    float tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float a = Lattice(ix, iz, seed);
    const float b = Lattice(ix + 1, iz, seed);
    const float c = Lattice(ix, iz + 1, seed);
    const float d = Lattice(ix + 1, iz + 1, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}

// How much of a world's ground is wooded (0: none grows trees), and how far apart the trees of a wood stand (metres).
struct Woods
{
    float cover = 0.0f;
    float spacing = 9.0f;
    float broadleaf = 0.5f; // the share of them that are broadleaves, the rest conifers
};

Woods WoodsOf(const std::string& biome)
{
    if (biome == "temperate")
    {
        return {0.42f, 8.0f, 0.5f};
    }
    if (biome == "jungle")
    {
        return {0.85f, 6.5f, 1.0f};
    }
    if (biome == "oceanic")
    {
        return {0.16f, 9.0f, 0.75f};
    }
    return {};
}

} // namespace

float Woodland(uint32_t seed, const std::string& biome, float x, float z)
{
    const Woods woods = WoodsOf(biome);
    if (woods.cover <= 0.0f)
    {
        return 0.0f;
    }
    // Woods some hundreds of metres across and the clearings between, their edges ragged with smaller stands and gaps.
    const uint32_t s = seed * 2246822519u + 0x900Du;
    const float n = Smooth(x / 260.0f, z / 260.0f, s) * 0.55f + Smooth(x / 80.0f, z / 80.0f, s + 1u) * 0.3f + Smooth(x / 26.0f, z / 26.0f, s + 2u) * 0.15f;
    // (The sum is about a half, mostly within a third of it: the line it must pass for about `cover` of the ground to.)
    const float line = 0.5f + (0.5f - woods.cover) * 0.55f;
    const float t = std::clamp((n - line + 0.05f) / 0.1f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

namespace
{

// Whether a tree could grow at a place: dry ground, not on the shore, not too steep.
bool TreeGround(const SiteTerrain& terrain, float x, float z, float up)
{
    return terrain.WaterDepth(x, z) < -1.2f && up >= 0.85f;
}

// Whether a place is in a landmark's clearing.
bool Cleared(const std::vector<glm::vec3>& clearings, const glm::vec2& at)
{
    for (const glm::vec3& clearing : clearings)
    {
        if (glm::distance(at, glm::vec2(clearing.x, clearing.y)) < clearing.z)
        {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<Item> Plan(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain, const std::vector<glm::vec3>& clearings)
{
    std::vector<Item> items;
    Random random((static_cast<uint64_t>(seed) << 17) ^ 0x5CA77E2ull);
    // Over the site and all the ground round it as far as the height field goes (FarTrees has the land past that): not just
    // round the buildings.
    const float reach = SiteTerrain::kMargin - 6.0f;
    const glm::vec2 low{plan.origin.x - reach, plan.origin.z - reach};
    const float side = plan.size + reach * 2.0f;
    const float hectares = side * side / 10000.0f;
    // (Whether it was placed: never in the clearing round a landmark.)
    const auto place = [&](Kind kind, const glm::vec2& at)
    {
        if (Cleared(clearings, at))
        {
            return false;
        }
        Item item;
        item.kind = kind;
        item.variant = random.Int(0, kVariants - 1);
        item.scale = SizeOf(kind, random);
        item.yaw = random.Range(0.0f, kTau);
        item.tip = kind == Kind::IceShard || kind == Kind::Spire ? random.Range(-0.25f, 0.25f) : 0.0f;
        // Sunk a little, so it sits in the ground rather than on it: the most where the ground falls away under it.
        const float half = std::max(item.scale.x, item.scale.z) * 0.45f;
        float under = terrain.GroundHeight(at.x, at.y);
        for (const glm::vec2 corner : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
        {
            under = std::min(under, terrain.GroundHeight(at.x + corner.x * half, at.y + corner.y * half));
        }
        const float sink = kind == Kind::Boulder ? item.scale.y * 0.18f : 0.08f;
        item.at = {at.x, under - sink, at.y};
        items.push_back(item);
        return true;
    };
    // Trees in woods, close together, with clearings between (Woodland): one a spot some metres apart, more or less, where
    // the woods are -- and fewer at their edges -- on dry ground they could stand on; out past the height field, as far as
    // kTreeReach (on the land round it there, as it is drawn).
    const Woods woods = WoodsOf(biome);
    if (woods.cover > 0.0f)
    {
        const glm::vec2 from{plan.origin.x - kTreeReach, plan.origin.z - kTreeReach};
        const int cells = static_cast<int>((plan.size + kTreeReach * 2.0f) / woods.spacing);
        for (int j = 0; j < cells; ++j)
        {
            for (int i = 0; i < cells; ++i)
            {
                const glm::vec2 at = from + (glm::vec2(static_cast<float>(i), static_cast<float>(j)) + glm::vec2(random.Range(0.1f, 0.9f), random.Range(0.1f, 0.9f))) * woods.spacing;
                const float chance = Woodland(seed, biome, at.x, at.y) * 0.85f;
                if (!random.Chance(chance) || terrain.Wildness(at.x, at.y) < 0.6f || !TreeGround(terrain, at.x, at.y, terrain.GroundNormal(at.x, at.y).y))
                {
                    continue;
                }
                if (!place(Kind::Tree, at))
                {
                    continue;
                }
                // Broadleaf or conifer by the world's share of them (the shapes alternate).
                items.back().variant = (random.Int(0, 1) * 2) + (random.Chance(woods.broadleaf) ? 0 : 1);
            }
        }
    }
    for (const Share& share : SharesOf(biome))
    {
        if (share.kind == Kind::Tree && woods.cover > 0.0f)
        {
            continue;
        }
        const int total = static_cast<int>(share.perHectare * hectares * random.Range(0.75f, 1.25f));
        const int clusters = std::max(1, total / std::max(share.cluster, 1));
        // The spread of a cluster: tight for grass, wide for trees.
        const float spread = share.kind == Kind::Grass ? 2.5f : share.kind == Kind::Tree ? 13.0f : share.kind == Kind::Boulder ? 6.0f : 4.0f;
        for (int c = 0; c < clusters; ++c)
        {
            const glm::vec2 centre = low + glm::vec2(random.Range(0.0f, side), random.Range(0.0f, side));
            // Shrubs thickest at the woods' edges and in their gaps, as undergrowth is where the light gets in.
            if (share.kind == Kind::Shrub && woods.cover > 0.0f)
            {
                const float wood = Woodland(seed, biome, centre.x, centre.y);
                if (!random.Chance(0.35f + 0.65f * (1.0f - std::abs(wood - 0.5f) * 2.0f)))
                {
                    continue;
                }
            }
            const int count = std::max(1, static_cast<int>(static_cast<float>(share.cluster) * random.Range(0.5f, 1.5f)));
            for (int i = 0; i < count; ++i)
            {
                const glm::vec2 at = centre + glm::vec2(random.Normal(), random.Normal()) * spread;
                // On the world's own ground, well clear of anything levelled, and on a slope it could stand on.
                if (at.x < low.x || at.y < low.y || at.x > low.x + side || at.y > low.y + side || terrain.Wildness(at.x, at.y) < 0.6f ||
                    terrain.Normal(at.x, at.y).y < SteepestFor(share.kind))
                {
                    continue;
                }
                place(share.kind, at);
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
    case Kind::Grass:
        // All leaves: Foliage.
        break;
    case Kind::Tree:
    {
        const uint32_t bark = Paint({0.3f, 0.24f, 0.19f});
        if (variant % 2 == 0)
        {
            // A broadleaf: a trunk, three or four boughs out of it into the crown, a branch or two off each.
            Bough(mesh, {0.0f, -0.1f, 0.0f}, {random.Range(-0.05f, 0.05f), 1.0f, random.Range(-0.05f, 0.05f)}, 0.55f, 0.045f, 0.026f, {0.0f, 0.0f, 0.0f},
                  bark);
            const int boughs = random.Int(3, 4);
            for (int i = 0; i < boughs; ++i)
            {
                const float a = kTau * (static_cast<float>(i) + random.Range(0.0f, 0.6f)) / static_cast<float>(boughs);
                const glm::vec3 from{0.0f, random.Range(0.36f, 0.44f), 0.0f};
                const glm::vec3 way{std::cos(a) * 0.7f, 1.0f, std::sin(a) * 0.7f};
                std::vector<glm::vec3> ends;
                Bough(mesh, from, way, random.Range(0.3f, 0.4f), 0.022f, 0.007f, {0.0f, 0.4f, 0.0f}, bark, &ends);
                const glm::vec3 mid = from + glm::normalize(way) * 0.16f;
                Bough(mesh, mid, glm::vec3(std::cos(a + 1.2f), 0.8f, std::sin(a + 1.2f)), 0.18f, 0.01f, 0.004f, {0.0f, 0.3f, 0.0f}, bark);
            }
        }
        else
        {
            // A conifer: one trunk to its top, its branches the needle cards (Foliage).
            Bough(mesh, {0.0f, -0.1f, 0.0f}, {random.Range(-0.03f, 0.03f), 1.0f, random.Range(-0.03f, 0.03f)}, 1.05f, 0.032f, 0.004f, {0.0f, 0.0f, 0.0f},
                  bark);
            // And inside the whorls, the dark of the needles behind needles: a cone, so the tree is a solid shape from any way
            // and any distance and the cards over it are its outline. Without it a spruce was a trunk with a few sprays on it,
            // the sky through it from below and from a little way off.
            Prism(mesh, {0.0f, 0.18f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.74f, 0.17f, 0.012f, 7, 0.0f, random.Range(0.0f, kTau), Paint(growth * 0.42f));
        }
        break;
    }
    case Kind::DeadTree:
    {
        // A bare trunk and a few bare branches, bent.
        const uint32_t grey = Paint({0.9f, 0.9f, 0.9f});
        Bough(mesh, {0.0f, -0.1f, 0.0f}, {random.Range(-0.15f, 0.15f), 1.0f, random.Range(-0.15f, 0.15f)}, 0.95f, 0.04f, 0.006f,
              {random.Range(-0.3f, 0.3f), 0.0f, random.Range(-0.3f, 0.3f)}, grey);
        for (int i = 0; i < random.Int(3, 5); ++i)
        {
            const float a = random.Range(0.0f, kTau);
            const glm::vec3 from{0.0f, random.Range(0.35f, 0.75f), 0.0f};
            Bough(mesh, from, {std::cos(a), random.Range(0.3f, 0.9f), std::sin(a)}, random.Range(0.15f, 0.35f), 0.015f, 0.003f,
                  {0.0f, random.Range(-0.5f, 0.5f), 0.0f}, grey);
        }
        break;
    }
    default: break;
    }
    return mesh;
}

MeshData Leaves(Kind kind, int variant, uint32_t seed, const glm::vec3& growth)
{
    MeshData mesh;
    Random random((static_cast<uint64_t>(seed) << 11) ^ (static_cast<uint64_t>(kind) << 5) ^ static_cast<uint64_t>(variant) ^ 0x1EA7E5ull);
    // Each card a little apart in colour, round the world's own.
    const auto tint = [&](float low, float high) { return Paint(growth * random.Range(low, high)); };
    switch (kind)
    {
    case Kind::Tree:
        if (variant % 2 == 0)
        {
            // A crown of sprays: clusters of three crossed cards, filling a rounded crown over the boughs, denser outside.
            const glm::vec3 centre{0.0f, 0.68f, 0.0f};
            const glm::vec3 reach{0.3f, 0.22f, 0.3f};
            const int clusters = random.Int(26, 36);
            for (int i = 0; i < clusters; ++i)
            {
                glm::vec3 d = Somewhere(random);
                d.y = d.y * 0.8f + 0.15f;
                const glm::vec3 at = centre + glm::normalize(d) * reach * random.Range(0.55f, 1.0f);
                const float size = random.Range(0.11f, 0.16f);
                const glm::vec3 axis = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) + Somewhere(random) * 0.6f);
                const glm::vec3 first = glm::normalize(glm::cross(axis, Somewhere(random)));
                const uint32_t colour = tint(0.75f, 1.15f);
                const int cell = random.Unit() < 0.5f ? Foliage::kBroad : Foliage::kBroadDense;
                for (int c = 0; c < 3; ++c)
                {
                    const glm::vec3 right = glm::angleAxis(1.0472f * static_cast<float>(c), axis) * first;
                    Card(mesh, at, right * size, axis * size, cell, colour);
                }
            }
        }
        else
        {
            // Whorls of branches up the trunk, drooping, shorter as they go up; each branch a card and a second across it.
            for (float y = 0.14f; y < 0.96f; y += random.Range(0.05f, 0.075f))
            {
                const float up = (y - 0.14f) / 0.82f;
                const float length = 0.36f * (1.0f - up) + 0.05f;
                const int branches = random.Int(6, 8);
                const float turn = random.Range(0.0f, kTau);
                for (int i = 0; i < branches; ++i)
                {
                    const float a = turn + kTau * static_cast<float>(i) / static_cast<float>(branches);
                    const float droop = random.Range(0.25f, 0.45f);
                    const glm::vec3 out = glm::normalize(glm::vec3(std::cos(a), -droop, std::sin(a)));
                    const glm::vec3 across = glm::normalize(glm::cross(out, glm::vec3(0.0f, 1.0f, 0.0f)));
                    const glm::vec3 lift = glm::normalize(glm::cross(across, out));
                    const glm::vec3 middle = glm::vec3(0.0f, y, 0.0f) + out * length * 0.5f;
                    const uint32_t colour = tint(0.7f, 1.05f);
                    // The picture's foot at the trunk, its top out at the tip.
                    Card(mesh, middle, across * length * 0.32f, out * length * 0.5f, Foliage::kNeedles, colour);
                    Card(mesh, middle, lift * length * 0.22f, out * length * 0.5f, Foliage::kNeedles, colour);
                }
            }
            // And the leader at the top.
            Card(mesh, {0.0f, 0.98f, 0.0f}, {0.04f, 0.0f, 0.0f}, {0.0f, 0.07f, 0.0f}, Foliage::kNeedles, tint(0.8f, 1.0f));
            Card(mesh, {0.0f, 0.98f, 0.0f}, {0.0f, 0.0f, 0.04f}, {0.0f, 0.07f, 0.0f}, Foliage::kNeedles, tint(0.8f, 1.0f));
        }
        break;
    case Kind::Shrub:
    {
        // Sprays crossed round a low mound.
        const int clusters = random.Int(6, 9);
        for (int i = 0; i < clusters; ++i)
        {
            glm::vec3 d = Somewhere(random);
            d.y = std::abs(d.y) * 0.6f + 0.2f;
            const glm::vec3 at = glm::vec3(0.0f, 0.3f, 0.0f) + glm::normalize(d) * glm::vec3(0.35f, 0.25f, 0.35f) * random.Range(0.3f, 1.0f);
            const float size = random.Range(0.28f, 0.4f);
            const glm::vec3 axis = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) + Somewhere(random) * 0.5f);
            const glm::vec3 first = glm::normalize(glm::cross(axis, Somewhere(random)));
            const uint32_t colour = tint(0.7f, 1.1f);
            for (int c = 0; c < 2; ++c)
            {
                const glm::vec3 right = glm::angleAxis(1.5708f * static_cast<float>(c), axis) * first;
                Card(mesh, at, right * size, axis * size, Foliage::kBroadDense, colour);
            }
        }
        break;
    }
    case Kind::Grass:
    {
        // Four tufts crossed from one root, each turned its own way, its own width, and leaning a little out from the others:
        // from any side, a clump -- never the one flat shape, nor a star of them.
        const float turn = random.Range(0.0f, kTau);
        const uint32_t colour = tint(0.8f, 1.1f);
        for (int c = 0; c < 4; ++c)
        {
            const float a = turn + 0.7854f * static_cast<float>(c) + random.Range(-0.3f, 0.3f);
            const glm::vec3 across{std::cos(a), 0.0f, std::sin(a)};
            const glm::vec3 lean = glm::vec3(-across.z, 0.0f, across.x) * random.Range(-0.18f, 0.18f);
            const glm::vec3 up = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) + lean) * random.Range(0.42f, 0.55f);
            Card(mesh, up - glm::vec3(0.0f, 0.05f, 0.0f), across * random.Range(0.42f, 0.6f), up, Foliage::kGrass, colour);
        }
        break;
    }
    default: break;
    }
    // A crown lit as the one rounded mass it reads as from any way off -- its sunny side bright, its far side in its own
    // shade -- rather than each card lit by the way it happens to face, which showed every card for what it is.
    if ((kind == Kind::Tree || kind == Kind::Shrub) && !mesh.vertices.empty())
    {
        glm::vec3 low = mesh.vertices.front().position;
        glm::vec3 high = low;
        for (const MeshVertex& vertex : mesh.vertices)
        {
            low = glm::min(low, vertex.position);
            high = glm::max(high, vertex.position);
        }
        const glm::vec3 middle = (low + high) * 0.5f;
        const glm::vec3 reach = glm::max((high - low) * 0.5f, glm::vec3(0.01f));
        for (MeshVertex& vertex : mesh.vertices)
        {
            const glm::vec3 outward = (vertex.position - middle) / reach + glm::vec3(0.0f, 0.3f, 0.0f);
            vertex.normal = glm::normalize(glm::mix(vertex.normal, glm::normalize(outward + glm::vec3(0.0f, 1.0e-3f, 0.0f)), 0.75f));
        }
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

MeshData FarTrees(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain, const glm::vec3& growth,
                  const std::vector<glm::vec3>& clearings)
{
    const glm::vec3 middle{plan.origin.x + plan.size * 0.5f, plan.origin.y, plan.origin.z + plan.size * 0.5f};
    const float nearest = plan.size * 0.5f + kTreeReach;
    MeshData mesh;
    const Woods woods = WoodsOf(biome);
    if (woods.cover <= 0.0f)
    {
        return mesh;
    }
    Random random((static_cast<uint64_t>(seed) << 21) ^ 0xFA77EE5ull);
    // Crowns of so many sides, smooth-shaded: a broadleaf's a few lumps on a trunk, a conifer's tiers to its point.
    constexpr int kSides = 8;
    const auto vertex = [&](const glm::vec3& at, const glm::vec3& normal, const glm::vec3& colour)
    {
        MeshVertex v;
        v.position = at;
        v.normal = glm::normalize(normal);
        v.color = Paint(colour);
        mesh.vertices.push_back(v);
        return static_cast<uint32_t>(mesh.vertices.size() - 1);
    };
    const auto ring = [&](uint32_t first, uint32_t second)
    {
        for (uint32_t s = 0; s < kSides; ++s)
        {
            const uint32_t n = (s + 1) % kSides;
            mesh.indices.insert(mesh.indices.end(), {first + s, second + s, first + n, first + n, second + s, second + n});
        }
    };
    const auto fan = [&](uint32_t centre, uint32_t first, bool up)
    {
        for (uint32_t s = 0; s < kSides; ++s)
        {
            const uint32_t n = (s + 1) % kSides;
            if (up)
            {
                mesh.indices.insert(mesh.indices.end(), {first + s, centre, first + n});
            }
            else
            {
                mesh.indices.insert(mesh.indices.end(), {first + s, first + n, centre});
            }
        }
    };
    // (`full`: the near band's; past it, where one is a few pixels, a single lump, no trunk, fewer tiers.)
    const auto tree = [&](const glm::vec3& foot, float h, bool broadleaf, bool full)
    {
        const float turn = random.Range(0.0f, kTau);
        // Its leaves' colour, a little apart from its neighbours' and a little greyer than the near trees' (their pictures'
        // own light and dark, averaged, at this distance): conifers darker, and darker below than above.
        const glm::vec3 grey(glm::dot(growth, glm::vec3(0.2126f, 0.7152f, 0.0722f)));
        const glm::vec3 leaf = glm::mix(grey, growth, 0.85f) * random.Range(0.75f, 1.1f) * (broadleaf ? 1.0f : 0.7f);
        const auto speckle = [&](const glm::vec3& colour) { return colour * random.Range(0.85f, 1.12f); };
        // Lit as one soft mass, as a crown of leaves is, not as a faceted stone: each normal out from the middle, leaning up.
        const auto soft = [](const glm::vec3& out) { return glm::normalize(glm::normalize(out) + glm::vec3(0.0f, 0.45f, 0.0f)); };
        const auto round = [&](const glm::vec3& at, float y, float r, const glm::vec3& centre, const glm::vec3& colour)
        {
            const auto first = static_cast<uint32_t>(mesh.vertices.size());
            for (int s = 0; s < kSides; ++s)
            {
                const float a = turn + kTau * static_cast<float>(s) / kSides;
                // Uneven all round, as no crown is a round: boughs out here, a gap there.
                const float wobble = (1.0f + 0.12f * std::sin(a * 3.0f + turn)) * random.Range(0.78f, 1.2f);
                const glm::vec3 p = at + glm::vec3(std::cos(a) * r * wobble, y + random.Range(-0.06f, 0.06f) * r, std::sin(a) * r * wobble);
                vertex(p, soft(p - centre), speckle(colour));
            }
            return first;
        };
        // One lump of leaves: its underside, its widest, narrower over that, its top.
        const auto lump = [&](const glm::vec3& at, float r, float tall)
        {
            const glm::vec3 centre = at + glm::vec3(0.0f, tall * 0.5f, 0.0f);
            const uint32_t bottom = vertex(at, {0.0f, -1.0f, 0.0f}, leaf * 0.4f);
            const uint32_t widest = round(at, tall * 0.42f, r, centre, leaf * 0.7f);
            const uint32_t upper = round(at, tall * 0.78f, r * 0.72f, centre, leaf * 0.95f);
            const uint32_t top = vertex(at + glm::vec3(0.0f, tall, 0.0f), {0.0f, 1.0f, 0.0f}, speckle(leaf * 1.08f));
            fan(bottom, widest, false);
            ring(widest, upper);
            fan(top, upper, true);
        };
        if (broadleaf)
        {
            // A crown of a few lumps -- its main mass and two or three smaller ones off its sides, lower -- so its outline is
            // ragged against the sky as a real one is, on a trunk.
            const float r = h * random.Range(0.26f, 0.34f);
            lump(foot + glm::vec3(0.0f, h * 0.36f, 0.0f), r, h * 0.62f);
            const int more = full ? 2 + static_cast<int>(random.Unit() * 1.99f) : 0;
            for (int i = 0; i < more; ++i)
            {
                const float a = turn + kTau * (static_cast<float>(i) + random.Range(0.0f, 0.5f)) / static_cast<float>(more);
                const float size = random.Range(0.55f, 0.75f);
                const glm::vec3 off{std::cos(a) * r * 0.75f, h * random.Range(0.3f, 0.48f), std::sin(a) * r * 0.75f};
                lump(foot + off, r * size, h * 0.62f * size);
            }
            // The trunk under it: three sides, bark.
            if (!full)
            {
                return;
            }
            const glm::vec3 bark{0.13f, 0.1f, 0.08f};
            const float t = h * 0.035f;
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            for (int s = 0; s < 3; ++s)
            {
                const float a = turn + kTau * static_cast<float>(s) / 3.0f;
                const glm::vec3 out{std::cos(a), 0.0f, std::sin(a)};
                vertex(foot + out * t - glm::vec3(0.0f, 0.3f, 0.0f), out, bark);
                vertex(foot + out * t * 0.7f + glm::vec3(0.0f, h * 0.45f, 0.0f), out, bark);
            }
            for (uint32_t s = 0; s < 3; ++s)
            {
                const uint32_t n = (s + 1) % 3;
                mesh.indices.insert(mesh.indices.end(), {base + s * 2, base + s * 2 + 1, base + n * 2, base + n * 2, base + s * 2 + 1, base + n * 2 + 1});
            }
        }
        else
        {
            // A conifer: tiers of boughs, each a skirt narrower than the one under it, up to its point -- its outline
            // stepped, as a spruce's is, not a smooth cone.
            const float r = h * random.Range(0.17f, 0.22f);
            const glm::vec3 centre = foot + glm::vec3(0.0f, h * 0.4f, 0.0f);
            const int tiers = full ? 3 : 2;
            for (int tier = 0; tier < tiers; ++tier)
            {
                const float t = static_cast<float>(tier) / static_cast<float>(tiers);
                const float low = h * (0.1f + 0.62f * t);
                const float high = tier + 1 == tiers ? h : h * (0.1f + 0.62f * t + 0.38f);
                const float spread = r * (1.0f - 0.68f * t);
                const uint32_t bottom = vertex(foot + glm::vec3(0.0f, low + (high - low) * 0.15f, 0.0f), {0.0f, -1.0f, 0.0f}, leaf * 0.35f);
                const uint32_t skirt = round(foot, low, spread, centre, leaf * (0.6f + 0.15f * t));
                const uint32_t tip = vertex(foot + glm::vec3(0.0f, high, 0.0f), {0.0f, 1.0f, 0.0f}, speckle(leaf));
                fan(bottom, skirt, false);
                fan(tip, skirt, true);
            }
        }
    };
    // From where Plan's trees stop (kTreeReach) out to where a tree is a pixel in the haze: a spot every
    // so many metres where the woods are, further apart and larger out past where one could be told from the next.
    constexpr float kOut = 1800.0f;
    for (int band = 0; band < 2; ++band)
    {
        const float spacing = woods.spacing * (band == 0 ? 1.0f : 1.8f);
        const float from = band == 0 ? 0.0f : 650.0f;
        const float to = band == 0 ? 650.0f : kOut;
        const int cells = static_cast<int>(to * 2.0f / spacing);
        for (int j = 0; j < cells; ++j)
        {
            for (int i = 0; i < cells; ++i)
            {
                const float x = middle.x - to + (static_cast<float>(i) + random.Range(0.1f, 0.9f)) * spacing;
                const float z = middle.z - to + (static_cast<float>(j) + random.Range(0.1f, 0.9f)) * spacing;
                const float size = random.Range(0.0f, 1.0f);
                const bool broadleaf = random.Chance(woods.broadleaf);
                const float chance = random.Unit();
                const float out = std::max(std::abs(x - middle.x), std::abs(z - middle.z));
                if (out < from || out < nearest || Cleared(clearings, glm::vec2(x, z)))
                {
                    continue;
                }
                // (As close as Plan's in the near band; further out, fewer and larger, standing for the rest.)
                if (chance >= Woodland(seed, biome, x, z) * 0.85f)
                {
                    continue;
                }
                const glm::vec3 normal = terrain.GroundNormal(x, z);
                if (!TreeGround(terrain, x, z, normal.y))
                {
                    continue;
                }
                const float h = (5.0f + 7.0f * size) * (band == 0 ? 1.0f : 1.25f);
                tree({x, terrain.GroundHeight(x, z) - 0.2f, z}, h, broadleaf, band == 0);
            }
        }
    }
    return mesh;
}

} // namespace SiteScatter

} // namespace pred
