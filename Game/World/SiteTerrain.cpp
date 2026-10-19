#include "Game/World/SiteTerrain.h"

#include "Game/World/FacilityMap.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdint>
#include <array>
#include <cmath>
#include <future>
#include <thread>

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

// Ridges: crests where the noise crosses its middle, 0 to 1 -- rounded over rather than knife-edged.
float Ridged(float x, float z, float scale, int octaves, uint32_t seed)
{
    const float f = Fbm(x, z, scale, octaves, seed);
    const float n = 1.0f - std::sqrt(f * f + 0.012f);
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

// Runs `row` for every row from 0 to `last`, the rows shared out across the machine's threads: each row's work is its own,
// and the same whichever thread does it.
template <typename Row>
void ForRows(int last, const Row& row)
{
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 16);
    const int each = (last + threads) / threads;
    std::vector<std::future<void>> work;
    for (int from = 0; from <= last; from += each)
    {
        const int to = std::min(from + each - 1, last);
        work.push_back(std::async(std::launch::async, [&row, from, to]
                                  {
                                      for (int j = from; j <= to; ++j)
                                      {
                                          row(j);
                                      }
                                  }));
    }
    for (std::future<void>& done : work)
    {
        done.get();
    }
}

// Levelled ground reaches this far past what is built, and blends into the world's own over this far beyond.
constexpr float kLevelled = 4.0f;
constexpr float kBlend = 18.0f;
// The walls round the edge: how high, starting this far inside the edge and reaching their height this far outside it.

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
    // And further off, the land the site sits in: its world's landforms at their own size (Macro), coming in only past the
    // site's own ground, so that is as it was planned.
    const float fromMiddle = glm::length(glm::vec2(x, z) - (m_origin + glm::vec2(m_size * 0.5f)));
    h += (Macro(x, z) - m_macroHere) * Smooth(150.0f, 700.0f, fromMiddle);
    // And the land going down under the sea past the site: off one side at a coast, all round on an island. The water's
    // edge a little inside how far anybody may go, wandering, a beach down to it and the sea floor falling away past it, so
    // the deep water is the edge there.
    if (m_water == Water::Coast || m_water == Water::Island)
    {
        const glm::vec2 local = glm::vec2(x, z) - (m_origin + glm::vec2(m_size * 0.5f));
        // (An island's shore round the site's square, its corners rounded.)
        const glm::vec2 a = glm::abs(local);
        const float round = std::pow(std::pow(a.x, 4.0f) + std::pow(a.y, 4.0f), 0.25f);
        const float out = (m_water == Water::Coast ? glm::dot(local, m_coastWay) : round) - m_size * 0.5f;
        const float shore = 24.0f + 10.0f * Fbm(x, z, 140.0f, 2, s + 61u);
        const float floor = -4.0f - 0.15f * std::max(out - shore, 0.0f) - 34.0f * Smooth(shore, shore + 300.0f, out);
        h = glm::mix(h, floor, Smooth(shore - 30.0f, shore + 8.0f, out));
    }
    return h + Edge(x, z);
}

float SiteTerrain::Edge(float x, float z) const
{
    // Round an island the sea is the edge all the way round.
    if (m_water == Water::Island)
    {
        return 0.0f;
    }
    const glm::vec2 local = glm::vec2(x, z) - (m_origin + glm::vec2(m_size * 0.5f));
    // How far past the site's square, its corners rounded (so the edge does not turn a right angle round them).
    const glm::vec2 a = glm::abs(local) / (m_size * 0.5f);
    const float past = (std::pow(std::pow(a.x, 8.0f) + std::pow(a.y, 8.0f), 0.125f) - 1.0f) * m_size * 0.5f;
    if (past <= 0.0f)
    {
        return 0.0f;
    }
    // Which it is, going round: a few slow waves round the site, the same all the way out from it.
    const float angle = std::atan2(local.y, local.x);
    float kind = 0.0f;
    float sum = 0.0f;
    for (size_t i = 0; i < m_edgeWaves.size(); ++i)
    {
        kind += m_edgeWaves[i].x * std::sin(static_cast<float>(i + 1) * angle + m_edgeWaves[i].y);
        sum += m_edgeWaves[i].x;
    }
    kind /= std::max(sum, 1.0e-3f);
    // Not on the sea's side of a coast: the water is the edge there.
    // And always one pass through it somewhere -- never a ring all the way round -- where the land runs on open.
    const float fromPass = std::abs(std::remainder(angle - m_edgeWaves[2].y, 6.2831853f));
    const float open = Smooth(0.2f, 0.5f, fromPass) *
                       (m_water == Water::Coast ? 1.0f - Smooth(0.2f, 0.6f, glm::dot(glm::normalize(local), m_coastWay)) : 1.0f);
    const bool ravines = m_shape == Shape::Canyons || m_shape == Shape::Mountainous;
    const uint32_t s = m_seed + 211u;
    const float wander = Fbm(x, z, 70.0f, 2, s);
    float h = 0.0f;
    // A cliff most of the way round, its foot a little inside how far anybody may go (kRoam), wandering in and out along it:
    // too steep for anybody to climb, the face of a ridge. How high by the world: mountain walls, a scarp on
    // a plain. Where it gives out for a stretch, the soft limit is the edge.
    // (Each stretch coming in and dying away over some tens of metres along the edge, not stopping at a line.)
    const float cliff = (ravines ? Smooth(-0.2f, 0.25f, kind) : Smooth(-0.7f, -0.1f, kind)) * open;
    if (cliff > 0.0f)
    {
        float tall = 14.0f;
        switch (m_shape)
        {
        case Shape::Flat: tall = 17.0f; break;
        case Shape::Rolling: tall = 22.0f; break;
        case Shape::Mountainous: tall = 42.0f; break;
        case Shape::Canyons: tall = 28.0f; break;
        case Shape::Cratered: tall = 18.0f; break;
        }
        // Higher and lower along it, never one even wall.
        tall *= (0.8f + 0.4f * Noise(angle * 3.0f + 11.0f, 0.5f, s + 1u)) * std::max(0.85f + 0.45f * Fbm(x, z, 90.0f, 3, s + 4u), 0.65f);
        // Its foot wandering in and out, in long bays and headlands and short ones.
        const float foot = 24.0f + 18.0f * Fbm(x, z, 130.0f, 2, s + 5u) + 8.0f * wander + 6.0f * std::sin(angle * 5.0f + m_edgeWaves[0].y);
        // The steep face of a ridge, its back falling away again past it: so where a stretch of it ends, it ends as a ridge
        // does, not as a step in the land running off to the horizon.
        h += cliff * tall * (Smooth(foot, foot + 8.0f, past) - 0.85f * Smooth(foot + 30.0f, foot + 200.0f, past));
        // Its face broken into ledges and buttresses.
        const float face = Smooth(foot - 2.0f, foot + 4.0f, past) * (1.0f - Smooth(foot + 8.0f, foot + 18.0f, past));
        h += cliff * face * tall * 0.18f * Fbm(x, z, 9.0f, 2, s + 3u);
    }
    // And on canyon and mountain worlds, ravines between: the ground falling away at a lip just past how far anybody may go,
    // so it is seen, not fallen into, and rising again beyond.
    if (ravines)
    {
        const float ravine = (1.0f - Smooth(-0.25f, 0.2f, kind)) * open;
        const float lip = kRoam + 2.0f + 3.0f * std::abs(wander);
        const float deep = 16.0f + 10.0f * Noise(angle * 3.0f + 5.0f, 1.5f, s + 2u);
        // (Measured square, as how far anybody may go is: its corners as well.)
        const float square = std::max(std::abs(local.x), std::abs(local.y)) - m_size * 0.5f;
        h -= ravine * deep * (Smooth(lip, lip + 5.0f, square) - Smooth(lip + 22.0f, lip + 30.0f, square));
    }
    return h;
}

float SiteTerrain::Macro(float x, float z) const
{
    const uint32_t s = m_seed + 101u;
    switch (m_shape)
    {
    case Shape::Flat:
        // Long low swells, a plain going away.
        return 7.0f * Fbm(x, z, 900.0f, 3, s);
    case Shape::Rolling:
        // Hills, and further, higher ones.
        return 40.0f * Fbm(x, z, 700.0f, 4, s) + 12.0f;
    case Shape::Mountainous:
        // A range: ridged peaks a couple of hundred metres high, valleys between.
        return 330.0f * (Ridged(x, z, 1400.0f, 4, s) - 0.3f);
    case Shape::Canyons:
    {
        // Mesas: flat-topped and steep-sided, the land between them low, cut by the canyons.
        const float v = Fbm(x, z, 900.0f, 3, s);
        return 75.0f * Smooth(0.04f, 0.1f, v) + 25.0f * Smooth(0.18f, 0.24f, v) - 15.0f;
    }
    case Shape::Cratered:
    {
        // Great craters, one now and then in each stretch of ground some hundreds of metres across: a bowl, and its rim.
        constexpr float kSpan = 800.0f;
        const int cx = static_cast<int>(std::floor(x / kSpan));
        const int cz = static_cast<int>(std::floor(z / kSpan));
        float h = 6.0f * Fbm(x, z, 700.0f, 3, s);
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (Hash(cx + dx, cz + dz, s + 1u) > 0.6f)
                {
                    continue;
                }
                const glm::vec2 at{(static_cast<float>(cx + dx) + Hash(cx + dx, cz + dz, s + 2u)) * kSpan,
                                   (static_cast<float>(cz + dz) + Hash(cx + dx, cz + dz, s + 3u)) * kSpan};
                const float radius = 140.0f + 220.0f * Hash(cx + dx, cz + dz, s + 4u);
                const float d = glm::length(glm::vec2(x, z) - at) / radius;
                if (d < 1.0f)
                {
                    h += radius * 0.12f * (d * d - 1.0f);
                }
                h += radius * 0.06f * std::exp(-(d - 1.0f) * (d - 1.0f) / 0.05f);
            }
        }
        return h;
    }
    }
    return 0.0f;
}

void SiteTerrain::Plan(const SitePlan& plan, Shape shape, bool dunes, Water water)
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
    {
        // The sea off one of the site's sides, its shore turned a little from square on to it.
        const float side = std::floor(random.Range(0.0f, 3.999f));
        const float way = side * 1.5707963f + random.Range(-0.2f, 0.2f);
        m_coastWay = {std::cos(way), std::sin(way)};
    }
    // Lakes lie only in some places, even where things grow.
    if (water == Water::Lakes && random.Unit() < 0.5f)
    {
        water = Water::None;
    }
    m_water = water;
    // Where the land stops anybody going further (Edge): how its cliffs and ravines go round the site.
    for (glm::vec2& wave : m_edgeWaves)
    {
        wave = {random.Range(0.6f, 1.0f), random.Range(0.0f, 6.2831853f)};
    }
    // A sea a little under the site; a lake's level a few metres under, in the low ground round it.
    m_waterLevel = water == Water::None ? -1.0e9f : m_base + (water == Water::Lakes ? -6.0f : -3.0f);
    m_craters.clear();
    m_macroHere = Macro(m_origin.x + m_size * 0.5f, m_origin.y + m_size * 0.5f);
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
    ForRows(m_cells, [&](int j)
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
            const size_t index = static_cast<size_t>(j) * side + static_cast<size_t>(i);
            m_wild[index] = wild;
            // (The edge left out of the smoothing below, so its cliffs stay too steep to climb.)
            m_heights[index] = m_base + (Natural(p.x, p.y) - Edge(p.x, p.y)) * wild;
        }
    });

    // Smoothed over a few metres, so nothing is sharper than ground worn by weather is -- and the levelled ground left exactly
    // level, as it was.
    std::vector<float> smoothed(m_heights.size());
    for (int pass = 0; pass < 3; ++pass)
    {
        ForRows(m_cells, [&](int j)
        {
            for (int i = 0; i <= m_cells; ++i)
            {
                float sum = 0.0f;
                float weight = 0.0f;
                for (int dj = -1; dj <= 1; ++dj)
                {
                    for (int di = -1; di <= 1; ++di)
                    {
                        const int u = std::clamp(i + di, 0, m_cells);
                        const int v = std::clamp(j + dj, 0, m_cells);
                        const float w = (di == 0 ? 2.0f : 1.0f) * (dj == 0 ? 2.0f : 1.0f);
                        sum += At(u, v) * w;
                        weight += w;
                    }
                }
                const size_t index = static_cast<size_t>(j) * side + static_cast<size_t>(i);
                smoothed[index] = m_base + (sum / weight - m_base) * Smooth(0.0f, 0.15f, m_wild[index]);
            }
        });
        m_heights.swap(smoothed);
    }
    ForRows(m_cells, [&](int j)
    {
        for (int i = 0; i <= m_cells; ++i)
        {
            const glm::vec2 p = m_min + glm::vec2(static_cast<float>(i), static_cast<float>(j)) * kCell;
            const size_t index = static_cast<size_t>(j) * side + static_cast<size_t>(i);
            m_heights[index] += Edge(p.x, p.y) * m_wild[index];
        }
    });
    m_farLines = FarLines();
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

bool SiteTerrain::InReach(int chunkX, int chunkZ) const
{
    // The open ground and as far round it as anybody may go, with room to spare.
    constexpr float kFoot = kRoam + 10.0f;
    const glm::vec2 low = m_min + glm::vec2(static_cast<float>(chunkX), static_cast<float>(chunkZ)) * (kCell * kChunkCells);
    const glm::vec2 high = low + glm::vec2(kCell * kChunkCells);
    const glm::vec2 open0 = m_origin - glm::vec2(kFoot);
    const glm::vec2 open1 = m_origin + glm::vec2(m_size + kFoot);
    return high.x > open0.x && high.y > open0.y && low.x < open1.x && low.y < open1.y;
}

float SiteTerrain::Rockiness(const glm::vec3& normal)
{
    // Ground on the gentle, the rock showing through as it steepens, all rock by the time nobody could stand on it.
    return 1.0f - Smooth(kSteepCosine - 0.06f, 0.94f, normal.y);
}

MeshData SiteTerrain::Mesh(int chunkX, int chunkZ, const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown) const
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
    const auto pack = [](const glm::vec3& c)
    {
        const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return channel(c.r) | (channel(c.g) << 8) | (channel(c.b) << 16) | (255u << 24);
    };
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
            vertex.color = pack(Colour(x, z, vertex.normal, ground, rock, grown));
            mesh.vertices.push_back(vertex);
        }
    }
    for (int j = j0; j < j1; ++j)
    {
        for (int i = i0; i < i1; ++i)
        {
            // Two triangles, wound to face up.
            const uint32_t a = static_cast<uint32_t>((j - j0) * across + (i - i0));
            const uint32_t b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(across);
            const uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}


std::vector<float> SiteTerrain::FarLines() const
{
    // Lines across it a cell or so apart near the site, further and further apart out towards the horizon, where a hill a
    // kilometre off is a few pixels: out from the middle each way.
    const float inner = Extent() * 0.5f;
    std::vector<float> offsets{0.0f};
    for (float o = 0.0f; o < kFar;)
    {
        o += o < inner ? 16.0f : std::clamp(o * 0.06f, 16.0f, 220.0f);
        offsets.push_back(std::min(o, kFar));
    }
    std::vector<float> lines;
    for (auto it = offsets.rbegin(); it != offsets.rend(); ++it)
    {
        lines.push_back(-*it);
    }
    lines.insert(lines.end(), offsets.begin() + 1, offsets.end());
    return lines;
}

float SiteTerrain::FarHeight(float x, float z) const
{
    // Under the height field where they overlap, so it never shows through it.
    const glm::vec2 middle = m_origin + glm::vec2(m_size * 0.5f);
    const float inner = Extent() * 0.5f;
    const glm::vec2 local = glm::abs(glm::vec2(x, z) - middle);
    const float under = local.x < inner && local.y < inner ? 0.8f : 0.0f;
    return m_base + Natural(x, z) - under;
}

bool SiteTerrain::OnField(float x, float z) const
{
    const glm::vec2 local = glm::vec2(x, z) - m_min;
    return local.x >= 0.0f && local.y >= 0.0f && local.x <= Extent() && local.y <= Extent();
}

float SiteTerrain::GroundHeight(float x, float z) const
{
    if (m_heights.empty() || OnField(x, z))
    {
        return Height(x, z);
    }
    // Past the field, the land round it as it is drawn (FarMesh): its corners' heights, and the two triangles each square is.
    if (m_farLines.size() < 2)
    {
        return FarHeight(x, z);
    }
    const glm::vec2 middle = m_origin + glm::vec2(m_size * 0.5f);
    const auto cell = [&](float offset, float& t)
    {
        const auto above = std::upper_bound(m_farLines.begin(), m_farLines.end(), offset);
        const auto i = static_cast<size_t>(std::clamp<std::ptrdiff_t>(above - m_farLines.begin() - 1, 0, static_cast<std::ptrdiff_t>(m_farLines.size()) - 2));
        t = std::clamp((offset - m_farLines[i]) / std::max(m_farLines[i + 1] - m_farLines[i], 1.0e-3f), 0.0f, 1.0f);
        return i;
    };
    float tx = 0.0f;
    float tz = 0.0f;
    const size_t i = cell(x - middle.x, tx);
    const size_t j = cell(z - middle.y, tz);
    const float x0 = middle.x + m_farLines[i];
    const float x1 = middle.x + m_farLines[i + 1];
    const float z0 = middle.y + m_farLines[j];
    const float z1 = middle.y + m_farLines[j + 1];
    const float a = FarHeight(x0, z0);
    const float b = FarHeight(x1, z0);
    const float c = FarHeight(x0, z1);
    const float d = FarHeight(x1, z1);
    return tx + tz <= 1.0f ? a + (b - a) * tx + (c - a) * tz : d + (c - d) * (1.0f - tx) + (b - d) * (1.0f - tz);
}

glm::vec3 SiteTerrain::GroundNormal(float x, float z) const
{
    if (m_heights.empty() || OnField(x, z))
    {
        return Normal(x, z);
    }
    constexpr float e = 4.0f;
    return glm::normalize(glm::vec3(-(Natural(x + e, z) - Natural(x - e, z)), 2.0f * e, -(Natural(x, z + e) - Natural(x, z - e))));
}

MeshData SiteTerrain::FarMesh(const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown) const
{
    MeshData mesh;
    if (m_heights.empty())
    {
        return mesh;
    }
    const glm::vec2 middle = m_origin + glm::vec2(m_size * 0.5f);
    const float inner = Extent() * 0.5f;
    const std::vector<float> lines = FarLines();
    const auto n = static_cast<int>(lines.size());
    const auto pack = [](const glm::vec3& c)
    {
        const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return channel(c.r) | (channel(c.g) << 8) | (channel(c.b) << 16) | (255u << 24);
    };
    for (int j = 0; j < n; ++j)
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = middle.x + lines[static_cast<size_t>(i)];
            const float z = middle.y + lines[static_cast<size_t>(j)];
            const float e = std::max(4.0f, std::abs(lines[static_cast<size_t>(std::min(i + 1, n - 1))] - lines[static_cast<size_t>(i)]) * 0.5f);
            MeshVertex vertex;
            vertex.position = {x, FarHeight(x, z), z};
            vertex.normal = glm::normalize(glm::vec3(-(Natural(x + e, z) - Natural(x - e, z)), 2.0f * e, -(Natural(x, z + e) - Natural(x, z - e))));
            vertex.uv = {x, z};
            vertex.color = pack(Colour(x, z, vertex.normal, ground, rock, grown));
            mesh.vertices.push_back(vertex);
        }
    }
    // Every square of it but those wholly inside the height field (a little in from its edge: the rest overlaps it).
    const float hole = inner - 6.0f;
    for (int j = 0; j + 1 < n; ++j)
    {
        for (int i = 0; i + 1 < n; ++i)
        {
            const float x0 = lines[static_cast<size_t>(i)];
            const float x1 = lines[static_cast<size_t>(i + 1)];
            const float z0 = lines[static_cast<size_t>(j)];
            const float z1 = lines[static_cast<size_t>(j + 1)];
            if (x0 >= -hole && x1 <= hole && z0 >= -hole && z1 <= hole)
            {
                continue;
            }
            const auto a = static_cast<uint32_t>(j * n + i);
            const uint32_t b = a + 1;
            const auto c = static_cast<uint32_t>(a + static_cast<uint32_t>(n));
            const uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}


float SiteTerrain::WaterDepth(float x, float z) const
{
    if (m_water == Water::None)
    {
        return -1.0e9f;
    }
    // On the height field where it is (what is walked on), the land round it past that.
    return m_waterLevel - (OnField(x, z) ? Height(x, z) : m_base + Natural(x, z));
}

MeshData SiteTerrain::WaterMesh() const
{
    MeshData mesh;
    if (m_water == Water::None || m_heights.empty())
    {
        return mesh;
    }
    // Lines a few metres apart over the height field, so the shore follows the ground closely where it is walked; further
    // and further apart out to the horizon, as the land round it is (FarMesh).
    const glm::vec2 middle = m_origin + glm::vec2(m_size * 0.5f);
    const float inner = Extent() * 0.5f;
    std::vector<float> offsets{0.0f};
    for (float o = 0.0f; o < kFar;)
    {
        o += o < inner ? 3.0f : std::clamp(o * 0.06f, 16.0f, 220.0f);
        offsets.push_back(std::min(o, kFar));
    }
    std::vector<float> lines;
    for (auto it = offsets.rbegin(); it != offsets.rend(); ++it)
    {
        lines.push_back(-*it);
    }
    lines.insert(lines.end(), offsets.begin() + 1, offsets.end());
    const auto n = static_cast<int>(lines.size());
    std::vector<float> depths(static_cast<size_t>(n) * static_cast<size_t>(n));
    for (int j = 0; j < n; ++j)
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = middle.x + lines[static_cast<size_t>(i)];
            const float z = middle.y + lines[static_cast<size_t>(j)];
            depths[static_cast<size_t>(j * n + i)] = WaterDepth(x, z);
        }
    }
    // Only the squares with water under some of them, each corner made once.
    std::vector<uint32_t> made(depths.size(), UINT32_MAX);
    const auto corner = [&](int i, int j)
    {
        const size_t at = static_cast<size_t>(j * n + i);
        if (made[at] == UINT32_MAX)
        {
            made[at] = static_cast<uint32_t>(mesh.vertices.size());
            MeshVertex vertex;
            vertex.position = {middle.x + lines[static_cast<size_t>(i)], m_waterLevel, middle.y + lines[static_cast<size_t>(j)]};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.uv = {std::clamp(depths[at], -8.0f, 80.0f), 0.0f};
            mesh.vertices.push_back(vertex);
        }
        return made[at];
    };
    for (int j = 0; j + 1 < n; ++j)
    {
        for (int i = 0; i + 1 < n; ++i)
        {
            const float deepest = std::max(std::max(depths[static_cast<size_t>(j * n + i)], depths[static_cast<size_t>(j * n + i + 1)]),
                                           std::max(depths[static_cast<size_t>((j + 1) * n + i)], depths[static_cast<size_t>((j + 1) * n + i + 1)]));
            if (deepest <= 0.0f)
            {
                continue;
            }
            const uint32_t a = corner(i, j);
            const uint32_t b = corner(i + 1, j);
            const uint32_t c = corner(i, j + 1);
            const uint32_t d = corner(i + 1, j + 1);
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}

glm::vec3 SiteTerrain::Colour(float x, float z, const glm::vec3& normal, const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown) const
{
    // Grown over where it is the world's own ground, in patches; rock where it is steep. As a share of the brighter of the
    // two (the material's colour, Paint), so neither is cut short.
    glm::vec3 soil = ground;
    if (grown != nullptr)
    {
        const float patchy = 0.55f + 0.45f * std::sin(x * 0.13f + std::sin(z * 0.09f) * 2.0f) * std::sin(z * 0.11f + 1.7f);
        soil = glm::mix(ground, *grown, std::clamp(Wildness(x, z) * 1.6f - 0.3f, 0.0f, 1.0f) * patchy * 0.75f);
    }
    return glm::mix(soil, rock, Rockiness(normal)) / Paint(ground, rock);
}

} // namespace pred
