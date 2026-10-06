#include "Game/World/SiteTerrain.h"

#include "Game/World/FacilityMap.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pred
{

namespace
{

// SplitMix64, as the sites are planned with: the same sequence on every machine.
class Random
{
public:
    explicit Random(uint64_t seed) : m_state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    float Range(float low, float high) { return low + (high - low) * Unit(); }

private:
    uint64_t m_state;
};

float Hash(int x, int z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(z) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

// Smooth noise, 0 to 1, a feature every unit.
float Noise(float x, float z, uint32_t seed)
{
    const float fx = std::floor(x);
    const float fz = std::floor(z);
    const int ix = static_cast<int>(fx);
    const int iz = static_cast<int>(fz);
    float tx = x - fx;
    float tz = z - fz;
    tx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    tz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);
    const float a = Hash(ix, iz, seed);
    const float b = Hash(ix + 1, iz, seed);
    const float c = Hash(ix, iz + 1, seed);
    const float d = Hash(ix + 1, iz + 1, seed);
    return glm::mix(glm::mix(a, b, tx), glm::mix(c, d, tx), tz);
}

// Several octaves of it, -1 to 1, its largest features `scale` metres across.
float Fbm(float x, float z, float scale, int octaves, uint32_t seed)
{
    float sum = 0.0f;
    float weight = 0.5f;
    float total = 0.0f;
    float frequency = 1.0f / scale;
    for (int i = 0; i < octaves; ++i)
    {
        sum += (Noise(x * frequency, z * frequency, seed + static_cast<uint32_t>(i) * 101u) * 2.0f - 1.0f) * weight;
        total += weight;
        weight *= 0.5f;
        frequency *= 2.03f;
    }
    return sum / total;
}

// Ridges: sharp crests where the noise crosses its middle, 0 to 1.
float Ridged(float x, float z, float scale, int octaves, uint32_t seed)
{
    const float n = 1.0f - std::abs(Fbm(x, z, scale, octaves, seed));
    return n * n;
}

float Smooth(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// How far a point is from a rectangle (0 inside it).
float FromRect(const glm::vec2& p, const glm::vec2& min, const glm::vec2& max)
{
    const glm::vec2 out = glm::max(glm::max(min - p, p - max), glm::vec2(0.0f));
    return glm::length(out);
}

float FromSegment(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b)
{
    const glm::vec2 ab = b - a;
    const float t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1.0e-6f), 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

// Levelled ground reaches this far past what is built, and blends into the world's own over this far beyond.
constexpr float kLevelled = 4.0f;
constexpr float kBlend = 12.0f;
// The walls round the edge: how high, starting this far inside the edge and reaching their height this far outside it.
constexpr float kWallHigh = 46.0f;
constexpr float kWallFrom = -6.0f;
constexpr float kWallTo = 30.0f;

} // namespace

SiteTerrain::Shape SiteTerrain::ShapeOf(const std::string& terrain)
{
    if (terrain == "rolling" || terrain == "islands")
    {
        return Shape::Rolling;
    }
    if (terrain == "mountainous" || terrain == "broken")
    {
        return Shape::Mountainous;
    }
    if (terrain == "canyons")
    {
        return Shape::Canyons;
    }
    if (terrain == "cratered")
    {
        return Shape::Cratered;
    }
    return Shape::Flat;
}

float SiteTerrain::Natural(float x, float z) const
{
    const uint32_t s = m_seed;
    float h = 0.0f;
    switch (m_shape)
    {
    case Shape::Flat:
        h = 1.2f * Fbm(x, z, 90.0f, 3, s);
        break;
    case Shape::Rolling:
        h = 8.0f * Fbm(x, z, 140.0f, 3, s) + 1.6f * Fbm(x, z, 35.0f, 2, s + 7u);
        break;
    case Shape::Mountainous:
        h = 24.0f * (Ridged(x, z, 170.0f, 4, s) - 0.45f) + 5.0f * Fbm(x, z, 45.0f, 3, s + 7u);
        break;
    case Shape::Canyons:
    {
        // Rolling ground, and channels wandering through it where a slow noise crosses its middle: steep-sided and deep.
        h = 4.0f * Fbm(x, z, 120.0f, 3, s);
        // Sides about as steep as can be climbed, so nobody is trapped in one.
        const float c = std::abs(Fbm(x, z, 150.0f, 2, s + 13u));
        h -= 10.0f * (1.0f - Smooth(0.03f, 0.22f, c));
        break;
    }
    case Shape::Cratered:
        h = 2.0f * Fbm(x, z, 80.0f, 3, s);
        for (const Crater& crater : m_craters)
        {
            const float d = glm::length(glm::vec2(x, z) - crater.at) / crater.radius;
            if (d < 1.0f)
            {
                h += crater.radius * 0.22f * (d * d - 1.0f);
            }
            h += crater.radius * 0.08f * std::exp(-(d - 1.0f) * (d - 1.0f) / 0.08f);
        }
        break;
    }
    if (m_dunes)
    {
        // Drifted into long ridges across the wind, wandering a little.
        const float across = x * std::cos(m_duneAngle) + z * std::sin(m_duneAngle);
        const float wave = std::sin(across / 16.0f + 3.0f * Fbm(x, z, 70.0f, 2, s + 29u)) * 0.5f + 0.5f;
        h += 3.2f * wave * wave;
    }
    return h;
}

void SiteTerrain::Plan(const SitePlan& plan, Shape shape, bool dunes)
{
    m_shape = shape;
    m_dunes = dunes;
    m_seed = plan.seed * 2654435761u + 0x7E44u;
    m_base = plan.origin.y - 0.02f;
    m_origin = {plan.origin.x, plan.origin.z};
    m_size = plan.size;
    m_min = m_origin - glm::vec2(kMargin);
    m_cells = static_cast<int>(std::ceil((plan.size + kMargin * 2.0f) / kCell));

    Random random((static_cast<uint64_t>(m_seed) << 8) ^ 0xC4A7E45ull);
    m_duneAngle = random.Range(0.0f, 3.14159265f);
    m_craters.clear();
    if (shape == Shape::Cratered)
    {
        const int count = 30 + static_cast<int>(random.Unit() * 20.0f);
        for (int i = 0; i < count; ++i)
        {
            m_craters.push_back({m_min + glm::vec2(random.Unit(), random.Unit()) * (plan.size + kMargin * 2.0f),
                                 random.Unit() < 0.25f ? random.Range(14.0f, 32.0f) : random.Range(3.0f, 10.0f)});
        }
    }

    // What people built, and the ways between: the buildings, the pad, the yards, a path from the pad to each building's door.
    std::vector<std::pair<glm::vec2, glm::vec2>> rects;
    std::vector<std::pair<glm::vec2, glm::vec2>> paths;
    for (const FacilityLayout& building : plan.buildings)
    {
        glm::vec2 min;
        glm::vec2 max;
        SitePlan::Footprint(building, 0.0f, min, max);
        rects.emplace_back(min, max);
        if (!building.exits.empty())
        {
            const glm::vec3 door = FacilityMap::ExitOutside(building, building.exits.front(), 2.0f);
            paths.emplace_back(glm::vec2(plan.landing.x, plan.landing.z), glm::vec2(door.x, door.z));
        }
    }
    // Off the foot of the ramp, where everybody gathers.
    {
        const glm::vec2 foot{plan.rampFoot.position.x, plan.rampFoot.position.z};
        rects.emplace_back(foot - glm::vec2(3.0f), foot + glm::vec2(3.0f));
    }
    for (const SitePlan::Block& block : plan.blocks)
    {
        using Kind = SitePlan::BlockKind;
        if (block.kind == Kind::Pad || block.kind == Kind::Container || block.kind == Kind::Tank || block.kind == Kind::Mast ||
            block.kind == Kind::PipeX || block.kind == Kind::PipeZ || block.kind == Kind::Support)
        {
            const float half = std::max(block.size.x, block.size.z) * 0.5f;
            const glm::vec2 at{block.centre.x, block.centre.z};
            rects.emplace_back(at - glm::vec2(half), at + glm::vec2(half));
        }
    }

    const size_t side = static_cast<size_t>(m_cells + 1);
    m_heights.assign(side * side, m_base);
    m_wild.assign(side * side, 1.0f);
    for (int j = 0; j <= m_cells; ++j)
    {
        for (int i = 0; i <= m_cells; ++i)
        {
            const glm::vec2 p = m_min + glm::vec2(static_cast<float>(i), static_cast<float>(j)) * kCell;
            float nearest = 1.0e9f;
            for (const auto& [min, max] : rects)
            {
                nearest = std::min(nearest, FromRect(p, min, max));
            }
            for (const auto& [a, b] : paths)
            {
                nearest = std::min(nearest, FromSegment(p, a, b) - 1.5f);
            }
            const float wild = Smooth(kLevelled, kLevelled + kBlend, nearest);
            // Out past the open ground every way, up into walls of rock.
            const glm::vec2 local = p - m_origin;
            // Its foot wanders in and out, and it is broken into buttresses and gullies rather than one even slope -- but it
            // never comes in over what was built.
            const float out = std::max(std::max(-local.x, local.x - m_size), std::max(-local.y, local.y - m_size)) +
                              10.0f * Fbm(p.x, p.y, 70.0f, 2, m_seed + 37u);
            const float rise = Smooth(kWallFrom, kWallTo, out) * std::max(wild, Smooth(kWallFrom + 12.0f, kWallFrom + 24.0f, out));
            const float wall = (kWallHigh + 7.0f * Fbm(p.x, p.y, 40.0f, 3, m_seed + 41u) + 14.0f * (Ridged(p.x, p.y, 45.0f, 3, m_seed + 43u) - 0.5f)) * rise;
            const size_t index = static_cast<size_t>(j) * side + static_cast<size_t>(i);
            m_wild[index] = std::max(wild, rise);
            m_heights[index] = m_base + Natural(p.x, p.y) * wild + wall;
        }
    }
}

float SiteTerrain::Height(float x, float z) const
{
    if (m_heights.empty())
    {
        return m_base;
    }
    const float u = std::clamp((x - m_min.x) / kCell, 0.0f, static_cast<float>(m_cells) - 1.0e-3f);
    const float v = std::clamp((z - m_min.y) / kCell, 0.0f, static_cast<float>(m_cells) - 1.0e-3f);
    const int i = static_cast<int>(u);
    const int j = static_cast<int>(v);
    const float tx = u - static_cast<float>(i);
    const float tz = v - static_cast<float>(j);
    // The same two triangles a cell is drawn as, so what is placed on the ground sits on what is drawn.
    const float a = At(i, j);
    const float b = At(i + 1, j);
    const float c = At(i, j + 1);
    const float d = At(i + 1, j + 1);
    return tx + tz <= 1.0f ? a + (b - a) * tx + (c - a) * tz : d + (c - d) * (1.0f - tx) + (b - d) * (1.0f - tz);
}

glm::vec3 SiteTerrain::Normal(float x, float z) const
{
    const float e = kCell;
    const float dx = Height(x + e, z) - Height(x - e, z);
    const float dz = Height(x, z + e) - Height(x, z - e);
    return glm::normalize(glm::vec3(-dx, 2.0f * e, -dz));
}

float SiteTerrain::Wildness(float x, float z) const
{
    if (m_wild.empty())
    {
        return 1.0f;
    }
    const int i = std::clamp(static_cast<int>(std::round((x - m_min.x) / kCell)), 0, m_cells);
    const int j = std::clamp(static_cast<int>(std::round((z - m_min.y) / kCell)), 0, m_cells);
    return m_wild[static_cast<size_t>(j) * static_cast<size_t>(m_cells + 1) + static_cast<size_t>(i)];
}

MeshData SiteTerrain::Mesh(int chunkX, int chunkZ, bool steep) const
{
    MeshData mesh;
    const int i0 = chunkX * kChunkCells;
    const int j0 = chunkZ * kChunkCells;
    const int i1 = std::min(i0 + kChunkCells, m_cells);
    const int j1 = std::min(j0 + kChunkCells, m_cells);
    if (i0 >= i1 || j0 >= j1)
    {
        return mesh;
    }
    const int across = i1 - i0 + 1;
    for (int j = j0; j <= j1; ++j)
    {
        for (int i = i0; i <= i1; ++i)
        {
            const float x = m_min.x + static_cast<float>(i) * kCell;
            const float z = m_min.y + static_cast<float>(j) * kCell;
            const int il = std::max(i - 1, 0);
            const int ir = std::min(i + 1, m_cells);
            const int jd = std::max(j - 1, 0);
            const int ju = std::min(j + 1, m_cells);
            const float dx = (At(ir, j) - At(il, j)) / (static_cast<float>(ir - il) * kCell);
            const float dz = (At(i, ju) - At(i, jd)) / (static_cast<float>(ju - jd) * kCell);
            MeshVertex vertex;
            vertex.position = {x, At(i, j), z};
            vertex.normal = glm::normalize(glm::vec3(-dx, 1.0f, -dz));
            vertex.uv = {x, z};
            mesh.vertices.push_back(vertex);
        }
    }
    for (int j = j0; j < j1; ++j)
    {
        for (int i = i0; i < i1; ++i)
        {
            const uint32_t a = static_cast<uint32_t>((j - j0) * across + (i - i0));
            const uint32_t b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(across);
            const uint32_t d = c + 1;
            // Two triangles, a-c-b and b-c-d (wound to face up), each kept with the gentle ground or the steep.
            for (const auto& tri : {std::array<uint32_t, 3>{a, c, b}, std::array<uint32_t, 3>{b, c, d}})
            {
                const glm::vec3 p0 = mesh.vertices[tri[0]].position;
                const glm::vec3 p1 = mesh.vertices[tri[1]].position;
                const glm::vec3 p2 = mesh.vertices[tri[2]].position;
                const glm::vec3 n = glm::normalize(glm::cross(p1 - p0, p2 - p0));
                if ((std::abs(n.y) < kSteepCosine) == steep)
                {
                    mesh.indices.insert(mesh.indices.end(), {tri[0], tri[1], tri[2]});
                }
            }
        }
    }
    return mesh;
}

} // namespace pred
