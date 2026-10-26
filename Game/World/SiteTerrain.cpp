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
    if (terrain == "icesheet")
    {
        return Shape::IceSheet;
    }
    if (terrain == "glacial")
    {
        return Shape::Glacial;
    }
    if (terrain == "lavaplain")
    {
        return Shape::LavaPlain;
    }
    if (terrain == "karst")
    {
        return Shape::Karst;
    }
    if (terrain == "highlands")
    {
        return Shape::Highlands;
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
    case Shape::IceSheet:
    {
        // Flat ice, its pressure ridges pushed up in long lines across the grain of the land (m_duneAngle), wandering; and
        // crevasses, narrow and shallow enough here to step down into and climb out of.
        h = 0.8f * Fbm(x, z, 90.0f, 3, s);
        const float across = x * std::cos(m_duneAngle) + z * std::sin(m_duneAngle);
        const float bend = 4.0f * Fbm(x, z, 160.0f, 2, s + 31u);
        const float ridge = 1.0f - std::abs(std::sin(across / 26.0f + bend));
        h += 1.8f * std::pow(ridge, 8.0f) * (0.5f + Fbm(x, z, 70.0f, 2, s + 32u));
        const float crack = std::abs(Fbm(x, z, 60.0f, 2, s + 33u));
        h -= 1.1f * (1.0f - Smooth(0.0f, 0.03f, crack)) * Smooth(0.45f, 0.6f, Noise(x / 120.0f, z / 120.0f, s + 34u));
        break;
    }
    case Shape::Glacial:
        // A valley floor the ice left: gentle, with low ridges of the rubble it dropped (moraines) along it.
        h = 2.5f * Fbm(x, z, 110.0f, 3, s) + 2.0f * std::pow(1.0f - std::abs(std::sin((x * std::cos(m_duneAngle) + z * std::sin(m_duneAngle)) / 60.0f +
                                                                                        2.0f * Fbm(x, z, 200.0f, 2, s + 41u))), 6.0f);
        break;
    case Shape::LavaPlain:
    {
        // Old flows gone cold: low hummocks, and here and there a blister of crust pushed up from under (a tumulus).
        h = 1.4f * Fbm(x, z, 26.0f, 3, s) + 2.0f * Fbm(x, z, 140.0f, 2, s + 51u);
        const float blister = std::max(Noise(x / 34.0f, z / 34.0f, s + 52u) - 0.62f, 0.0f) * 2.6f;
        h += 3.5f * blister * blister;
        break;
    }
    case Shape::Karst:
        // Rolling limestone, pitted with sinkholes: steep-sided funnels where the ground fell into the caves under it.
        h = 5.0f * Fbm(x, z, 120.0f, 3, s) + 1.0f * Fbm(x, z, 30.0f, 2, s + 61u);
        for (const Crater& hole : m_craters)
        {
            if (std::abs(x - hole.at.x) > hole.radius || std::abs(z - hole.at.y) > hole.radius)
            {
                continue;
            }
            const float d = glm::length(glm::vec2(x, z) - hole.at) / hole.radius;
            h -= hole.radius * 0.55f * (1.0f - Smooth(0.25f, 1.0f, d));
        }
        break;
    case Shape::Highlands:
    {
        // High moor: broad rolling ground, and tors -- knuckles of rock standing out of it here and there.
        h = 6.0f * Fbm(x, z, 150.0f, 3, s) + 1.2f * Fbm(x, z, 35.0f, 2, s + 71u);
        const float tor = std::max(Noise(x / 45.0f, z / 45.0f, s + 72u) - 0.7f, 0.0f) * 3.3f;
        h += 6.0f * tor * tor * tor;
        break;
    }
    case Shape::Cratered:
        h = 2.0f * Fbm(x, z, 80.0f, 3, s);
        for (const Crater& crater : m_craters)
        {
            // (Nothing of it this far off: its rim is gone by two and a half of its radius.)
            if (std::abs(x - crater.at.x) > crater.radius * 2.5f || std::abs(z - crater.at.y) > crater.radius * 2.5f)
            {
                continue;
            }
            const float d = glm::length(glm::vec2(x, z) - crater.at) / crater.radius;
            if (d < 1.0f)
            {
                // A bowl about a fifth as deep as it is wide, as a fresh crater is.
                h += crater.radius * 0.38f * (d * d - 1.0f);
            }
            // Its rim: steep inside, falling away gently outside over what was thrown out of it.
            h += crater.radius * 0.1f * std::exp(-(d - 1.0f) * (d - 1.0f) / (d < 1.0f ? 0.02f : 0.18f));
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
    // (Gentler where anybody may go -- out along the valleys -- than past the edge.)
    h += (Macro(x, z) - m_macroHere) * Smooth(150.0f, 700.0f, fromMiddle) * (0.3f + 0.7f * Smooth(-20.0f, 80.0f, PastRoam(x, z)));
    // And none of it standing up where the ground is kept low (the way a ship comes in): brought down to a little over the
    // ground round it, eased in over some tens of metres.
    for (const glm::vec4& low : m_keepLow)
    {
        const glm::vec2 out = glm::max(glm::max(glm::vec2(low.x, low.y) - glm::vec2(x, z), glm::vec2(x, z) - glm::vec2(low.z, low.w)), glm::vec2(0.0f));
        const float kept = 1.0f - Smooth(0.0f, 60.0f, glm::length(out));
        h = glm::mix(h, std::min(h, 2.0f), kept);
    }
    // And the land going down under the sea past the site: off one side at a coast, all round on an island. The water's
    // edge a little inside how far anybody may go, wandering, a beach down to it and the sea floor falling away past it, so
    // the deep water is the edge there.
    if (m_water == Water::Coast || m_water == Water::Island)
    {
        const glm::vec2 local = glm::vec2(x, z) - (m_origin + glm::vec2(m_size * 0.5f));
        // (An island's shore just past where anybody may go, all of it -- its valleys spits of land out into the sea -- above
        // the water.)
        const float out = m_water == Water::Coast ? glm::dot(local, m_coastWay) - m_size * 0.5f : PastRoam(x, z) + 20.0f;
        const float shore = 24.0f + 10.0f * Fbm(x, z, 140.0f, 2, s + 61u);
        const float floor = -4.0f - 0.15f * std::max(out - shore, 0.0f) - 34.0f * Smooth(shore, shore + 300.0f, out);
        const float sea = Smooth(shore - 30.0f, shore + 8.0f, out);
        h = glm::mix(h, floor, sea);
        // (And no cliffs out there: what was left of them where they gave way to the sea stood on its floor as steps, a
        // straight line across the shallows.)
        return h + Edge(x, z) * (1.0f - sea);
    }
    return h + Edge(x, z);
}

float SiteTerrain::PastRoam(float x, float z) const
{
    const glm::vec2 at{x, z};
    const glm::vec2 local = at - (m_origin + glm::vec2(m_size * 0.5f));
    // The square widened by kRoam every way (its corners rounded, as widening rounds them), its outline wandering in and out in
    // long bays and headlands.
    const glm::vec2 q = glm::abs(local) - glm::vec2(m_size * 0.5f);
    float past = glm::length(glm::max(q, glm::vec2(0.0f))) + std::min(std::max(q.x, q.y), 0.0f) - kRoam;
    const float angle = std::atan2(local.y, local.x);
    past -= 10.0f * std::sin(3.0f * angle + m_edgeWaves[0].y) + 6.0f * std::sin(5.0f * angle + m_edgeWaves[1].y);
    // And the valleys out of it.
    for (const Arm& arm : m_arms)
    {
        float nearest = glm::distance(at, arm.end) - arm.bowl;
        for (size_t i = 0; i + 1 < arm.line.size(); ++i)
        {
            const glm::vec2 a = arm.line[i];
            const glm::vec2 b = arm.line[i + 1];
            const glm::vec2 ab = b - a;
            const float t = std::clamp(glm::dot(at - a, ab) / std::max(glm::dot(ab, ab), 1.0e-4f), 0.0f, 1.0f);
            nearest = std::min(nearest, glm::distance(at, a + ab * t) - arm.halfWidth);
        }
        past = std::min(past, nearest);
    }
    return past;
}

float SiteTerrain::Edge(float x, float z) const
{
    // Round an island the sea is the edge all the way round; round an outpost, the land runs on (SitePlan::openEdge).
    if (m_water == Water::Island || m_openEdge)
    {
        return 0.0f;
    }
    const glm::vec2 local = glm::vec2(x, z) - (m_origin + glm::vec2(m_size * 0.5f));
    // How far past where anybody may go, measured as if from a line kFrom inside it: the cliffs' feet and the ravines' lips are
    // set out from there (PastRoam: the widened square, wandering, and the valleys out of it).
    constexpr float kFrom = 40.0f;
    const float past = PastRoam(x, z) + kFrom;
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
        case Shape::IceSheet: tall = 12.0f; break;
        case Shape::Glacial: tall = 30.0f; break;
        case Shape::LavaPlain: tall = 14.0f; break;
        case Shape::Karst: tall = 22.0f; break;
        case Shape::Highlands: tall = 20.0f; break;
        }
        // Higher and lower along it, never one even wall.
        tall *= (0.8f + 0.4f * Noise(angle * 3.0f + 11.0f, 0.5f, s + 1u)) * std::max(0.85f + 0.45f * Fbm(x, z, 90.0f, 3, s + 4u), 0.65f);
        // Its foot wandering in and out, in long bays and headlands and short ones.
        // (No further in than a few metres short of where anybody may go: the valleys out of the site are not much wider than
        // that, and cliffs coming further in closed them to a slot.)
        const float foot = std::max(24.0f + 18.0f * Fbm(x, z, 130.0f, 2, s + 5u) + 8.0f * wander + 6.0f * std::sin(angle * 5.0f + m_edgeWaves[0].y), kFrom - 4.0f);
        // The steep face of a ridge, its back falling away again past it: so where a stretch of it ends, it ends as a ridge
        // does, not as a step in the land running off to the horizon. (Back down to the land round it altogether: some of its
        // height left on for good was a step in the land along straight lines out from the site, and over low ground at a
        // coast a dead straight strip of land standing out of the sea to the horizon.)
        h += cliff * tall * (Smooth(foot, foot + 8.0f, past) - Smooth(foot + 30.0f, foot + 200.0f, past));
        // Its face broken into ledges and buttresses.
        const float face = Smooth(foot - 2.0f, foot + 4.0f, past) * (1.0f - Smooth(foot + 8.0f, foot + 18.0f, past));
        h += cliff * face * tall * 0.18f * Fbm(x, z, 9.0f, 2, s + 3u);
    }
    // And on canyon and mountain worlds, ravines between: the ground falling away at a lip just past how far anybody may go,
    // so it is seen, not fallen into, and rising again beyond.
    if (ravines)
    {
        const float ravine = (1.0f - Smooth(-0.25f, 0.2f, kind)) * open;
        const float lip = kFrom + 2.0f + 3.0f * std::abs(wander);
        // (Deep enough that its floor is lost in the dark down there: at twenty metres or so the floor was in plain sight.)
        const float deep = 40.0f + 25.0f * Noise(angle * 3.0f + 5.0f, 1.5f, s + 2u);
        h -= ravine * deep * (Smooth(lip, lip + 5.0f, past) - Smooth(lip + 22.0f, lip + 30.0f, past));
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
    case Shape::IceSheet:
    {
        // Ice to the horizon, nearly level, and now and then a peak standing up out of it (a nunatak).
        const float peak = std::max(Ridged(x, z, 1800.0f, 3, s) - 0.72f, 0.0f);
        return 5.0f * Fbm(x, z, 1500.0f, 2, s) + 900.0f * peak * peak;
    }
    case Shape::Glacial:
    {
        // The site on the floor of a broad valley the ice wore into a U: level across its floor, walls rising steeply either
        // side of it to ridged peaks, and the valley running on out of sight both ways, along the land's grain.
        const glm::vec2 local = glm::vec2(x, z) - (m_origin + glm::vec2(m_size * 0.5f));
        const glm::vec2 along{std::cos(m_duneAngle + 1.5707963f), std::sin(m_duneAngle + 1.5707963f)};
        const float wander = 180.0f * Fbm(glm::dot(local, along), 0.0f, 1600.0f, 2, s + 7u);
        const float out = std::abs(glm::dot(local, glm::vec2(-along.y, along.x)) - wander);
        const float wall = Smooth(380.0f, 1100.0f, out);
        return 340.0f * wall * wall * (0.75f + 0.5f * Ridged(x, z, 900.0f, 3, s)) + 12.0f * Fbm(x, z, 800.0f, 2, s + 9u);
    }
    case Shape::LavaPlain:
    {
        // Flows to the horizon, a little rising and falling, and the volcano they came from: a broad cone a few kilometres off,
        // its top broken open.
        const glm::vec2 middle = m_origin + glm::vec2(m_size * 0.5f);
        const glm::vec2 cone = middle + glm::vec2(std::cos(m_duneAngle * 2.0f), std::sin(m_duneAngle * 2.0f)) * 2600.0f;
        const float d = glm::length(glm::vec2(x, z) - cone);
        const float rise = 520.0f * std::exp(-d / 900.0f) - 160.0f * std::exp(-(d * d) / (220.0f * 220.0f));
        return 14.0f * Fbm(x, z, 1100.0f, 3, s) + rise;
    }
    case Shape::Karst:
    {
        // Rolling land, and out of it towers of limestone, steep-sided and round-topped, one now and then in each stretch
        // of ground some hundreds of metres across.
        constexpr float kSpan = 420.0f;
        const int cx = static_cast<int>(std::floor(x / kSpan));
        const int cz = static_cast<int>(std::floor(z / kSpan));
        float h = 25.0f * Fbm(x, z, 900.0f, 3, s);
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (Hash(cx + dx, cz + dz, s + 11u) > 0.55f)
                {
                    continue;
                }
                const glm::vec2 at{(static_cast<float>(cx + dx) + 0.2f + 0.6f * Hash(cx + dx, cz + dz, s + 12u)) * kSpan,
                                   (static_cast<float>(cz + dz) + 0.2f + 0.6f * Hash(cx + dx, cz + dz, s + 13u)) * kSpan};
                const float radius = 45.0f + 70.0f * Hash(cx + dx, cz + dz, s + 14u);
                const float tall = 70.0f + 160.0f * Hash(cx + dx, cz + dz, s + 15u);
                // (None standing where anybody may go: out in the land round it.)
                if (PastRoam(at.x, at.y) < radius + 40.0f)
                {
                    continue;
                }
                const float d = glm::length(glm::vec2(x, z) - at) / radius;
                h = std::max(h, tall * (1.0f - Smooth(0.55f, 1.0f, d)) * (1.0f - 0.25f * d * d));
            }
        }
        return h;
    }
    case Shape::Highlands:
    {
        // A high plateau: rolling, until its edge -- a kilometre or so out, wandering -- where the land falls away to the
        // low country far below, seen beyond it to the horizon.
        const glm::vec2 local = glm::vec2(x, z) - (m_origin + glm::vec2(m_size * 0.5f));
        const float edge = 1100.0f + 300.0f * Fbm(std::atan2(local.y, local.x) * 200.0f, 0.0f, 120.0f, 2, s + 17u);
        return 30.0f * Fbm(x, z, 700.0f, 3, s) - 260.0f * Smooth(edge, edge + 500.0f, glm::length(local));
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
        // The sea off one of the site's sides, its shore turned a little from square on to it -- a side no valley goes out on
        // (the one leaning least towards any, when every side has one), so no valley runs into the sea.
        float side = std::floor(random.Range(0.0f, 3.999f));
        const float turn = random.Range(-0.2f, 0.2f);
        const glm::vec2 middle = glm::vec2(plan.origin.x, plan.origin.z) + glm::vec2(plan.size * 0.5f);
        const auto worst = [&](float s)
        {
            const glm::vec2 out{std::cos(s * 1.5707963f), std::sin(s * 1.5707963f)};
            float most = -1.0f;
            for (const SitePlan::Valley& valley : plan.valleys)
            {
                const glm::vec2 to = valley.end - middle;
                most = std::max(most, glm::length(to) > 1.0e-3f ? glm::dot(out, to / glm::length(to)) : -1.0f);
            }
            return most;
        };
        if (worst(side) > 0.3f)
        {
            for (float other = 0.0f; other < 4.0f; other += 1.0f)
            {
                side = worst(other) < worst(side) ? other : side;
            }
        }
        const float way = side * 1.5707963f + turn;
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
    // And the valleys out into the land from the site's ground (PastRoam), as the plan has them.
    m_arms.clear();
    m_keepLow = plan.keepLow;
    m_openEdge = plan.openEdge;
    for (const SitePlan::Valley& valley : plan.valleys)
    {
        m_arms.push_back({valley.line, valley.halfWidth, valley.end, valley.bowl});
    }
    // A sea a little under the site; a lake's level a few metres under, in the low ground round it.
    m_waterLevel = water == Water::None ? -1.0e9f : m_base + (water == Water::Lakes ? -6.0f : -3.0f);
    m_craters.clear();
    m_macroHere = Macro(m_origin.x + m_size * 0.5f, m_origin.y + m_size * 0.5f);
    if (shape == Shape::Cratered)
    {
        // As a crater field is: many small, fewer middling, a few great -- each size some times as many as the next up.
        // (As many to the square metre however far the field reaches: nearly two hundred over a field 480 metres across.)
        const float across = (plan.size + kMargin * 2.0f) / 480.0f;
        const int count = static_cast<int>((160.0f + random.Unit() * 60.0f) * across * across);
        for (int i = 0; i < count; ++i)
        {
            const float size = std::min(4.0f * std::pow(std::max(random.Unit(), 0.004f), -0.55f), 45.0f);
            m_craters.push_back({m_min + glm::vec2(random.Unit(), random.Unit()) * (plan.size + kMargin * 2.0f), size});
        }
    }
    // Karst's sinkholes, as many to the square metre as fit: a few tens over the field, a few metres to a dozen across.
    if (shape == Shape::Karst)
    {
        const float across = (plan.size + kMargin * 2.0f) / 480.0f;
        const int count = static_cast<int>((22.0f + random.Unit() * 12.0f) * across * across);
        for (int i = 0; i < count; ++i)
        {
            m_craters.push_back({m_min + glm::vec2(random.Unit(), random.Unit()) * (plan.size + kMargin * 2.0f), random.Range(4.0f, 12.0f)});
        }
    }

    // What people built, and the ways between: the buildings, the pad, the yards, a path from the pad to each building's door.
    std::vector<std::pair<glm::vec2, glm::vec2>> rects;
    m_holes.clear();
    std::vector<std::pair<glm::vec2, glm::vec2>> paths;
    for (const FacilityLayout& building : plan.buildings)
    {
        glm::vec2 min;
        glm::vec2 max;
        SitePlan::Footprint(building, 0.0f, min, max);
        rects.emplace_back(min, max);
        if (building.ground > 0)
        {
            m_holes.emplace_back(min + glm::vec2(3.0f), max - glm::vec2(3.0f));
        }
        if (!building.exits.empty())
        {
            const glm::vec3 door = FacilityMap::ExitOutside(building, building.exits.front(), 2.0f);
            paths.emplace_back(glm::vec2(plan.landing.x, plan.landing.z), glm::vec2(door.x, door.z));
        }
    }
    // The trails worn in the ground: those paths, and from the pad out along each valley to its bowl (Worn).
    m_trails = paths;
    m_roads = plan.roads;
    for (const SitePlan::Valley& valley : plan.valleys)
    {
        glm::vec2 from{plan.landing.x, plan.landing.z};
        for (const glm::vec2& at : valley.line)
        {
            m_trails.emplace_back(from, at);
            from = at;
        }
        m_trails.emplace_back(from, valley.end);
    }
    // What the plan has levelled as it is (an outpost's poured ground).
    for (const glm::vec4& rect : plan.levelled)
    {
        rects.emplace_back(glm::vec2(rect.x, rect.y), glm::vec2(rect.z, rect.w));
    }
    // Off the foot of the ramp, where everybody gathers.
    {
        const glm::vec2 foot{plan.rampFoot.position.x, plan.rampFoot.position.z};
        rects.emplace_back(foot - glm::vec2(3.0f), foot + glm::vec2(3.0f));
    }
    for (const SitePlan::Block& block : plan.blocks)
    {
        using Kind = SitePlan::BlockKind;
        if (!block.outlying && (block.kind == Kind::Pad || block.kind == Kind::Container || block.kind == Kind::Tank || block.kind == Kind::Mast ||
            block.kind == Kind::PipeX || block.kind == Kind::PipeZ || block.kind == Kind::Support))
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

    // The roads graded into the land: along each, the ground's own height eased into a gentle grade -- level with the levelled
    // ground where it leaves it -- and across it level, the land either side blending down or up into it.
    for (const SitePlan::Road& road : m_roads)
    {
        std::vector<glm::vec2> points;
        for (size_t k = 0; k + 1 < road.line.size(); ++k)
        {
            const float length = glm::distance(road.line[k], road.line[k + 1]);
            for (float t = 0.0f; t < length; t += 4.0f)
            {
                points.push_back(road.line[k] + (road.line[k + 1] - road.line[k]) * (t / std::max(length, 1.0e-3f)));
            }
        }
        if (road.line.size() < 2 || points.empty())
        {
            continue;
        }
        points.push_back(road.line.back());
        const auto cellAt = [&](const glm::vec2& p, const std::vector<float>& grid)
        {
            const float u = std::clamp((p.x - m_min.x) / kCell, 0.0f, static_cast<float>(m_cells) - 1.0e-3f);
            const float v = std::clamp((p.y - m_min.y) / kCell, 0.0f, static_cast<float>(m_cells) - 1.0e-3f);
            const int i = static_cast<int>(u);
            const int j = static_cast<int>(v);
            const auto g = [&](int a, int b) { return grid[static_cast<size_t>(b) * side + static_cast<size_t>(a)]; };
            const float fx = u - static_cast<float>(i);
            const float fz = v - static_cast<float>(j);
            return glm::mix(glm::mix(g(i, j), g(i + 1, j), fx), glm::mix(g(i, j + 1), g(i + 1, j + 1), fx), fz);
        };
        std::vector<float> grade(points.size());
        for (size_t k = 0; k < points.size(); ++k)
        {
            grade[k] = cellAt(points[k], m_heights);
        }
        // (Eased over forty metres either way, three times over.)
        for (int pass = 0; pass < 3; ++pass)
        {
            std::vector<float> eased(grade.size());
            for (size_t k = 0; k < grade.size(); ++k)
            {
                float sum = 0.0f;
                float count = 0.0f;
                for (size_t m = k >= 10 ? k - 10 : 0; m <= std::min(k + 10, grade.size() - 1); ++m)
                {
                    sum += grade[m];
                    count += 1.0f;
                }
                eased[k] = sum / count;
            }
            grade.swap(eased);
        }
        for (size_t k = 0; k < points.size(); ++k)
        {
            grade[k] = m_base + (grade[k] - m_base) * cellAt(points[k], m_wild);
        }
        const float half = road.width * 0.5f;
        glm::vec2 low{1.0e9f};
        glm::vec2 high{-1.0e9f};
        for (const glm::vec2& p : points)
        {
            low = glm::min(low, p);
            high = glm::max(high, p);
        }
        const int i0 = std::max(0, static_cast<int>((low.x - half - 14.0f - m_min.x) / kCell));
        const int i1 = std::min(m_cells, static_cast<int>((high.x + half + 14.0f - m_min.x) / kCell) + 1);
        const int j0 = std::max(0, static_cast<int>((low.y - half - 14.0f - m_min.y) / kCell));
        const int j1 = std::min(m_cells, static_cast<int>((high.y + half + 14.0f - m_min.y) / kCell) + 1);
        for (int j = j0; j <= j1; ++j)
        {
            for (int i = i0; i <= i1; ++i)
            {
                const glm::vec2 p = m_min + glm::vec2(static_cast<float>(i), static_cast<float>(j)) * kCell;
                float nearest = 1.0e9f;
                float at = m_base;
                for (size_t k = 0; k + 1 < points.size(); ++k)
                {
                    const glm::vec2 ab = points[k + 1] - points[k];
                    const float t = std::clamp(glm::dot(p - points[k], ab) / std::max(glm::dot(ab, ab), 1.0e-4f), 0.0f, 1.0f);
                    const float d = glm::distance(p, points[k] + ab * t);
                    if (d < nearest)
                    {
                        nearest = d;
                        at = glm::mix(grade[k], grade[k + 1], t);
                    }
                }
                const size_t index = static_cast<size_t>(j) * side + static_cast<size_t>(i);
                m_heights[index] = glm::mix(at, m_heights[index], Smooth(half + 1.0f, half + 12.0f, nearest));
            }
        }
    }

    // Smoothed over a few metres, so nothing is sharper than ground worn by weather is -- and the levelled ground left exactly
    // level, as it was.
    std::vector<float> smoothed(m_heights.size());
    // (A crater field only lightly: nothing has worn it, and smoothed as much, its smaller craters went.)
    const int passes = m_shape == Shape::Cratered ? 1 : 3;
    for (int pass = 0; pass < passes; ++pass)
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

glm::vec3 SiteTerrain::VertexNormal(int i, int j) const
{
    const int il = std::max(i - 1, 0);
    const int ir = std::min(i + 1, m_cells);
    const int jd = std::max(j - 1, 0);
    const int ju = std::min(j + 1, m_cells);
    const float dx = (At(ir, j) - At(il, j)) / (static_cast<float>(ir - il) * kCell);
    const float dz = (At(i, ju) - At(i, jd)) / (static_cast<float>(ju - jd) * kCell);
    return glm::normalize(glm::vec3(-dx, 1.0f, -dz));
}

void SiteTerrain::DrawnCorners(float x, float z, int (&i)[3], int (&j)[3], float (&weight)[3]) const
{
    // As Height divides a cell: the triangle by the cell's low corner, or the one by its high corner.
    const float u = std::clamp((x - m_min.x) / kCell, 0.0f, static_cast<float>(m_cells) - 1.0e-3f);
    const float v = std::clamp((z - m_min.y) / kCell, 0.0f, static_cast<float>(m_cells) - 1.0e-3f);
    const int ci = static_cast<int>(u);
    const int cj = static_cast<int>(v);
    const float tx = u - static_cast<float>(ci);
    const float tz = v - static_cast<float>(cj);
    if (tx + tz <= 1.0f)
    {
        i[0] = ci;     j[0] = cj;     weight[0] = 1.0f - tx - tz;
        i[1] = ci + 1; j[1] = cj;     weight[1] = tx;
        i[2] = ci;     j[2] = cj + 1; weight[2] = tz;
    }
    else
    {
        i[0] = ci + 1; j[0] = cj + 1; weight[0] = tx + tz - 1.0f;
        i[1] = ci;     j[1] = cj + 1; weight[1] = 1.0f - tx;
        i[2] = ci + 1; j[2] = cj;     weight[2] = 1.0f - tz;
    }
}

glm::vec3 SiteTerrain::DrawnNormal(float x, float z) const
{
    if (m_heights.empty())
    {
        return {0.0f, 1.0f, 0.0f};
    }
    int i[3];
    int j[3];
    float weight[3];
    DrawnCorners(x, z, i, j, weight);
    glm::vec3 normal{0.0f};
    for (int k = 0; k < 3; ++k)
    {
        normal += VertexNormal(i[k], j[k]) * weight[k];
    }
    return glm::normalize(normal);
}

glm::vec3 SiteTerrain::DrawnColour(float x, float z, const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown) const
{
    if (m_heights.empty())
    {
        return Colour(x, z, Normal(x, z), ground, rock, grown);
    }
    int i[3];
    int j[3];
    float weight[3];
    DrawnCorners(x, z, i, j, weight);
    glm::vec3 colour{0.0f};
    for (int k = 0; k < 3; ++k)
    {
        const float cx = m_min.x + static_cast<float>(i[k]) * kCell;
        const float cz = m_min.y + static_cast<float>(j[k]) * kCell;
        colour += Colour(cx, cz, VertexNormal(i[k], j[k]), ground, rock, grown) * weight[k];
    }
    return colour;
}

float SiteTerrain::Wildness(float x, float z) const
{
    if (m_wild.empty())
    {
        return 1.0f;
    }
    const int i = std::clamp(static_cast<int>(std::round((x - m_min.x) / kCell)), 0, m_cells);
    const int j = std::clamp(static_cast<int>(std::round((z - m_min.y) / kCell)), 0, m_cells);
    const float wild = m_wild[static_cast<size_t>(j) * static_cast<size_t>(m_cells + 1) + static_cast<size_t>(i)];
    // (And none on a road: nothing grows there or lies about on it.)
    return m_roads.empty() ? wild : std::min(wild, Smooth(1.0f, 5.0f, FromRoad(x, z)));
}

bool SiteTerrain::LeftOut(int i, int j) const
{
    const glm::vec2 low = m_min + glm::vec2(static_cast<float>(i), static_cast<float>(j)) * kCell;
    const glm::vec2 high = low + glm::vec2(kCell);
    return std::any_of(m_holes.begin(), m_holes.end(), [&](const auto& hole)
                       { return low.x >= hole.first.x && low.y >= hole.first.y && high.x <= hole.second.x && high.y <= hole.second.y; });
}

bool SiteTerrain::InReach(int chunkX, int chunkZ) const
{
    // Anywhere anybody may go (PastRoam), with room to spare: looked at over the piece at a few metres apart, a little
    // further than they are apart round each.
    const glm::vec2 low = m_min + glm::vec2(static_cast<float>(chunkX), static_cast<float>(chunkZ)) * (kCell * kChunkCells);
    constexpr int kLooks = 6;
    const float apart = kCell * kChunkCells / static_cast<float>(kLooks);
    for (int j = 0; j <= kLooks; ++j)
    {
        for (int i = 0; i <= kLooks; ++i)
        {
            if (PastRoam(low.x + static_cast<float>(i) * apart, low.y + static_cast<float>(j) * apart) < 10.0f + apart)
            {
                return true;
            }
        }
    }
    return false;
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
            MeshVertex vertex;
            vertex.position = {x, At(i, j), z};
            vertex.normal = VertexNormal(i, j);
            vertex.uv = {x, z};
            vertex.color = pack(Colour(x, z, vertex.normal, ground, rock, grown));
            mesh.vertices.push_back(vertex);
        }
    }
    for (int j = j0; j < j1; ++j)
    {
        for (int i = i0; i < i1; ++i)
        {
            // None where a building with basements stands: wholly inside one's hole.
            if (LeftOut(i, j))
            {
                continue;
            }
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
    // (Not on the field's own edge, where the two meet: lowered there too -- as a point exactly on it rounds to inside -- the
    // land round it stepped down from the field's edge all along it, a straight line seen through the shallows.)
    const float under = local.x < inner - 1.0f && local.y < inner - 1.0f ? 0.8f : 0.0f;
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

float SiteTerrain::WaterLineSpacing(float x, float z) const
{
    // As WaterMesh lays them: three metres apart out to the fine reach, then three hundredths of the way out (4 to 220 m).
    const glm::vec2 off = glm::abs(glm::vec2(x, z) - WaterMiddle());
    const float away = std::max(off.x, off.y);
    return away < WaterFineReach() ? 3.0f : std::clamp(away * 0.03f, 4.0f, 220.0f);
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
        // (Coarser gently, so the waves on the open sea still have lines enough to move: Waves, vs_water.)
        o += o < inner ? 3.0f : std::clamp(o * 0.03f, 4.0f, 220.0f);
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

float SiteTerrain::FromRoad(float x, float z) const
{
    const glm::vec2 at{x, z};
    float nearest = 1.0e9f;
    for (const SitePlan::Road& road : m_roads)
    {
        for (size_t k = 0; k + 1 < road.line.size(); ++k)
        {
            const glm::vec2 a = road.line[k];
            const glm::vec2 ab = road.line[k + 1] - a;
            const float t = std::clamp(glm::dot(at - a, ab) / std::max(glm::dot(ab, ab), 1.0e-4f), 0.0f, 1.0f);
            nearest = std::min(nearest, glm::distance(at, a + ab * t) - road.width * 0.5f);
        }
    }
    return std::max(nearest, 0.0f);
}

float SiteTerrain::Road(float x, float z) const
{
    if (m_roads.empty())
    {
        return 0.0f;
    }
    // Gravel to its edges, ragged there, the verge a little worn.
    return 1.0f - Smooth(0.0f, 1.2f, FromRoad(x, z) + 0.6f * (Noise(x / 2.5f, z / 2.5f, m_seed + 93u) - 0.5f));
}

float SiteTerrain::Worn(float x, float z) const
{
    // A metre and a half or so across, its edges ragged: worn bare in the middle, grown back towards its sides.
    const glm::vec2 at{x, z};
    float nearest = 1.0e9f;
    for (const auto& [a, b] : m_trails)
    {
        const glm::vec2 ab = b - a;
        const float t = std::clamp(glm::dot(at - a, ab) / std::max(glm::dot(ab, ab), 1.0e-4f), 0.0f, 1.0f);
        nearest = std::min(nearest, glm::distance(at, a + ab * t));
    }
    // (Wandering a little either side of the straight line between, as a trail does.)
    const float wander = 0.9f * Fbm(x, z, 18.0f, 2, m_seed + 91u);
    return 1.0f - Smooth(0.5f, 1.6f + 0.5f * Noise(x / 3.0f, z / 3.0f, m_seed + 92u), std::abs(nearest + wander));
}

glm::vec3 SiteTerrain::Colour(float x, float z, const glm::vec3& normal, const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown) const
{
    // Grown over where it is the world's own ground, most of it and more in patches (seen past the tufts, the ground is what
    // says it is grass); rock where it is steep. As a share of the brighter of the
    // two (the material's colour, Paint), so neither is cut short.
    glm::vec3 soil = ground;
    if (grown != nullptr)
    {
        const float patchy = 0.55f + 0.45f * std::sin(x * 0.13f + std::sin(z * 0.09f) * 2.0f) * std::sin(z * 0.11f + 1.7f);
        soil = glm::mix(ground, *grown, std::clamp(Wildness(x, z) * 1.6f - 0.3f, 0.0f, 1.0f) * (0.6f + 0.35f * patchy));
    }
    if (OnField(x, z))
    {
        // By how the land lies: in the hollows, where the wet gathers, darker and (where it grows) greener; on the rises, paler
        // and drier. (Not on snow or ice: a damp hollow there is just more white.)
        float round = 0.0f;
        for (const glm::vec2 way : {glm::vec2(5.0f, 0.0f), glm::vec2(-5.0f, 0.0f), glm::vec2(0.0f, 5.0f), glm::vec2(0.0f, -5.0f)})
        {
            round += Height(x + way.x, z + way.y) * 0.25f;
        }
        const float hollow = round - Height(x, z);
        const float white = Smooth(0.55f, 0.8f, std::min({ground.r, ground.g, ground.b}));
        const float damp = Smooth(0.15f, 1.0f, hollow) * (1.0f - white);
        const float dry = Smooth(0.2f, 1.2f, -hollow) * (1.0f - white);
        soil *= glm::mix(1.0f, 0.78f, damp);
        if (grown != nullptr)
        {
            soil = glm::mix(soil, *grown * 0.9f, damp * 0.3f);
        }
        soil = glm::mix(soil, soil * 1.12f + glm::vec3(0.02f, 0.015f, 0.0f), dry * 0.6f);
        // And the trails worn bare by people going to and fro: the world's own ground, ungrown, a little darker with use; the roads
        // gravel, grey-brown and pale with dust.
        soil = glm::mix(soil, ground * 0.85f, Worn(x, z) * 0.85f);
        soil = glm::mix(soil, glm::mix(ground * 0.8f, rock * 0.9f, 0.45f) + glm::vec3(0.03f), Road(x, z) * 0.9f);
    }
    return glm::min(glm::mix(soil, rock, Rockiness(normal)) / Paint(ground, rock), glm::vec3(1.0f));
}

} // namespace pred
