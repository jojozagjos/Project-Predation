#include "Game/World/Landmark.h"

#include "Game/World/SitePlan.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pred
{

namespace Landmark
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
};

float Lattice(int x, int y, int z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(y) * 0xD8163841u ^ static_cast<uint32_t>(z) * 0xCB1AB31Fu ^
                 seed * 0x165667B1u;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// Smooth noise in three dimensions, 0 to 1: a value at each whole point, eased between.
float Noise(const glm::vec3& p, uint32_t seed)
{
    const glm::vec3 i = glm::floor(p);
    glm::vec3 f = p - i;
    f = f * f * (3.0f - 2.0f * f);
    const int x = static_cast<int>(i.x);
    const int y = static_cast<int>(i.y);
    const int z = static_cast<int>(i.z);
    const auto at = [&](int dx, int dy, int dz) { return Lattice(x + dx, y + dy, z + dz, seed); };
    const float x00 = glm::mix(at(0, 0, 0), at(1, 0, 0), f.x);
    const float x10 = glm::mix(at(0, 1, 0), at(1, 1, 0), f.x);
    const float x01 = glm::mix(at(0, 0, 1), at(1, 0, 1), f.x);
    const float x11 = glm::mix(at(0, 1, 1), at(1, 1, 1), f.x);
    return glm::mix(glm::mix(x00, x10, f.y), glm::mix(x01, x11, f.y), f.z);
}

// Weathered rock: lumps at a few sizes, about nought on average.
float Rough(const glm::vec3& p, float scale, uint32_t seed)
{
    return (Noise(p / scale, seed) - 0.5f) + 0.5f * (Noise(p / (scale * 0.4f), seed + 1u) - 0.5f) +
           0.25f * (Noise(p / (scale * 0.15f), seed + 2u) - 0.5f);
}

} // namespace

// A body of rock swept along a path: a ring round each point of it, `radii` across, `sides` round, pushed in and out by
// `rough` of its radius where the rock is weathered -- and `strata` of it in bands by height, as layered rock wears --
// closed at either end. Its rings face along the path, turned about `across` (any way not along it).
void Sweep(MeshData& mesh, const std::vector<glm::vec3>& path, const std::vector<float>& radii, int sides, const glm::vec3& across,
           float rough, float strata, uint32_t seed, bool capStart, bool capEnd, float outline)
{
    const auto first = static_cast<uint32_t>(mesh.vertices.size());
    const size_t rings = path.size();
    for (size_t i = 0; i < rings; ++i)
    {
        const glm::vec3 step = i + 1 < rings ? path[i + 1] - path[i] : path[i] - path[i - 1];
        const glm::vec3 along = glm::length(step) > 1.0e-5f ? glm::normalize(step) : glm::vec3(0.0f, 1.0f, 0.0f);
        // (Square to the path: about `across`, or -- where the path runs along that -- about another way.)
        glm::vec3 square = glm::cross(along, across);
        if (glm::length(square) < 1.0e-3f)
        {
            square = glm::cross(along, std::abs(along.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f));
        }
        const glm::vec3 u = glm::normalize(square);
        const glm::vec3 v = glm::cross(u, along);
        for (int s = 0; s < sides; ++s)
        {
            const float a = kTau * static_cast<float>(s) / static_cast<float>(sides);
            // Its outline the same all the way along (so a cliff stays a cliff): lobed and drawn out one way, not a circle.
            const float shape = 1.0f + outline * (0.6f * std::cos(2.0f * a) + 1.6f * (Noise(glm::vec3(std::cos(a) * 1.6f, std::sin(a) * 1.6f, 3.0f), seed + 20u) - 0.5f));
            const glm::vec3 out = u * std::cos(a) + v * std::sin(a);
            const glm::vec3 surface = path[i] + out * radii[i] * shape;
            // Layers a few metres thick, each standing out a little at its top as the softer rock under it wears back.
            float bands = 0.0f;
            if (strata > 0.0f)
            {
                const float layer = surface.y / 6.5f + 0.6f * Noise(surface * 0.03f, seed + 7u);
                bands = strata * glm::smoothstep(0.55f, 0.85f, layer - std::floor(layer));
            }
            // Weathered at the size of rock, not of the hill: lumps some metres across, streaked down its face as rain runs.
            const float scale = glm::clamp(radii[i] * 0.9f, 3.0f, 12.0f);
            const glm::vec3 streaked = surface * glm::vec3(1.0f, 0.4f, 1.0f);
            const float push = 1.0f + (rough * Rough(streaked, scale, seed) * 2.0f * scale + bands * radii[i]) / std::max(radii[i], 1.0f);
            MeshVertex vertex;
            vertex.position = path[i] + out * radii[i] * shape * std::max(push, 0.2f);
            vertex.color = 0xffffffffu;
            mesh.vertices.push_back(vertex);
        }
    }
    for (uint32_t i = 0; i + 1 < rings; ++i)
    {
        for (uint32_t s = 0; s < static_cast<uint32_t>(sides); ++s)
        {
            const uint32_t n = (s + 1) % static_cast<uint32_t>(sides);
            const uint32_t a = first + i * static_cast<uint32_t>(sides);
            const uint32_t b = a + static_cast<uint32_t>(sides);
            mesh.indices.insert(mesh.indices.end(), {a + s, b + s, a + n, a + n, b + s, b + n});
        }
    }
    const auto cap = [&](size_t ring, bool atEnd)
    {
        glm::vec3 centre{0.0f};
        const uint32_t start = first + static_cast<uint32_t>(ring) * static_cast<uint32_t>(sides);
        for (int s = 0; s < sides; ++s)
        {
            centre += mesh.vertices[start + static_cast<uint32_t>(s)].position;
        }
        MeshVertex middle;
        middle.position = centre / static_cast<float>(sides);
        middle.color = 0xffffffffu;
        const auto c = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(middle);
        for (uint32_t s = 0; s < static_cast<uint32_t>(sides); ++s)
        {
            const uint32_t n = (s + 1) % static_cast<uint32_t>(sides);
            if (atEnd)
            {
                mesh.indices.insert(mesh.indices.end(), {start + s, c, start + n});
            }
            else
            {
                mesh.indices.insert(mesh.indices.end(), {start + s, start + n, c});
            }
        }
    };
    if (capStart)
    {
        cap(0, false);
    }
    if (capEnd)
    {
        cap(rings - 1, true);
    }
}

// Each corner's normal from the faces round it, so the rock is lit as one weathered mass.
void SmoothNormals(MeshData& mesh)
{
    std::vector<glm::vec3> sums(mesh.vertices.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const uint32_t a = mesh.indices[i];
        const uint32_t b = mesh.indices[i + 1];
        const uint32_t c = mesh.indices[i + 2];
        const glm::vec3 n = glm::cross(mesh.vertices[b].position - mesh.vertices[a].position, mesh.vertices[c].position - mesh.vertices[a].position);
        sums[a] += n;
        sums[b] += n;
        sums[c] += n;
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i)
    {
        mesh.vertices[i].normal = glm::length(sums[i]) > 1.0e-9f ? glm::normalize(sums[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

namespace
{

// A column standing up from `foot`, `high`, `rings` rings, its radius at each height (0 at its foot, 1 at its top) by
// `profile`, leaning a little and wandering as it rises.
template <typename Profile>
void Column(MeshData& mesh, const glm::vec3& foot, float high, int rings, int sides, Profile profile, float lean, float yaw, float rough,
            float strata, uint32_t seed, bool pointed, float outline = 0.0f)
{
    std::vector<glm::vec3> path;
    std::vector<float> radii;
    const glm::vec3 leanWay{std::cos(yaw), 0.0f, std::sin(yaw)};
    for (int i = 0; i <= rings; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(rings);
        // From below the ground (so it stands in it) to its top.
        const float y = -12.0f + (high + 12.0f) * t;
        const glm::vec3 wander{Noise(glm::vec3(t * 3.0f, 0.0f, 0.0f), seed + 11u) - 0.5f, 0.0f, Noise(glm::vec3(0.0f, t * 3.0f, 0.0f), seed + 12u) - 0.5f};
        path.push_back(foot + glm::vec3(0.0f, y, 0.0f) + leanWay * (lean * t * t * high) + wander * (high * 0.06f));
        radii.push_back(std::max(profile(t), 0.05f));
    }
    if (pointed)
    {
        radii.back() = 0.05f;
    }
    Sweep(mesh, path, radii, sides, glm::vec3(1.0f, 0.0f, 0.0f), rough, strata, seed, true, true, outline);
}

} // namespace

const char* Name(Kind kind)
{
    switch (kind)
    {
    case Kind::Arch: return "arch";
    case Kind::Spire: return "spire";
    case Kind::Butte: return "butte";
    case Kind::BalancedRock: return "balanced rock";
    case Kind::GreatTree: return "giant tree";
    case Kind::Crystals: return "crystals";
    case Kind::IceTower: return "ice tower";
    case Kind::Mast: return "mast";
    case Kind::Icefall: return "frozen waterfall";
    default: return "none";
    }
}

std::vector<Placed> Scatter(uint32_t seed, const std::string& biome, SiteTerrain::Shape shape, const SitePlan& plan, const SiteTerrain& terrain)
{
    // What the world's land could hold: by its biome, and the shape of its land -- and how much of it does (the share of
    // stretches some hundreds of metres across with something in them), and whether they come in groups, as buttes in mesa
    // country and pillars in a field of them do.
    struct Option
    {
        Kind kind;
        float weight;
    };
    std::vector<Option> options;
    float share = 0.35f;
    const bool canyons = shape == SiteTerrain::Shape::Canyons;
    if (biome == "frozen")
    {
        options = {{Kind::IceTower, 3.0f}, {Kind::Spire, 1.0f}, {Kind::Arch, 0.5f}};
        share = 0.3f;
    }
    else if (biome == "crystalline")
    {
        options = {{Kind::Crystals, 4.0f}, {Kind::Spire, 1.0f}};
        share = 0.4f;
    }
    else if (biome == "temperate" || biome == "jungle")
    {
        // Green worlds: now and then a tree older and greater than the rest, rarely rock standing out of the hills.
        options = {{Kind::GreatTree, 3.0f}, {Kind::Spire, 0.6f}, {Kind::Arch, 0.4f}};
        share = 0.14f;
    }
    else if (biome == "desert")
    {
        options = {{Kind::Butte, 2.5f}, {Kind::Spire, 1.5f}, {Kind::Arch, 1.0f}, {Kind::BalancedRock, 1.0f}};
        share = 0.45f;
    }
    else if (biome == "oceanic")
    {
        options = {{Kind::Spire, 1.0f}, {Kind::Arch, 1.0f}};
        share = 0.15f;
    }
    else
    {
        // Rock: barren, rocky, volcanic, toxic, storm-beaten.
        options = {{Kind::Spire, 2.0f}, {Kind::Arch, 1.0f}, {Kind::BalancedRock, 1.0f}, {Kind::Butte, 1.0f}};
        share = 0.3f;
    }
    // And on any world, now and then, an old mast somebody put up and left.
    options.push_back({Kind::Mast, 0.35f});
    if (canyons)
    {
        // Mesa country: the buttes the canyons were cut round.
        options.push_back({Kind::Butte, 3.0f});
        share = std::max(share, 0.5f);
    }
    float total = 0.0f;
    for (const Option& option : options)
    {
        total += option.weight;
    }

    // Stretches of land some hundreds of metres across, out to where the land is drawn, each with its own numbers: so they
    // are where they are on the land, not set round the site -- some near, some on the horizon, sometimes none close.
    constexpr float kSpan = 650.0f;
    constexpr float kReach = 2600.0f;
    // (Nothing on the site's own ground or the land just round it, which the site's own things fill.)
    const float clear = plan.size * 0.5f + 420.0f;
    const glm::vec2 middle{plan.origin.x + plan.size * 0.5f, plan.origin.z + plan.size * 0.5f};
    const int cells = static_cast<int>(std::ceil(kReach / kSpan));
    const int cx0 = static_cast<int>(std::floor(middle.x / kSpan));
    const int cz0 = static_cast<int>(std::floor(middle.y / kSpan));
    std::vector<Placed> placed;
    for (int dz = -cells; dz <= cells; ++dz)
    {
        for (int dx = -cells; dx <= cells; ++dx)
        {
            const int cx = cx0 + dx;
            const int cz = cz0 + dz;
            Random random((static_cast<uint64_t>(seed) << 32) ^ (static_cast<uint64_t>(static_cast<uint32_t>(cx)) * 0x9E3779B1ull) ^
                          (static_cast<uint64_t>(static_cast<uint32_t>(cz)) * 0x85EBCA77ull << 16) ^ 0x1A4D3A2Cull);
            if (random.Unit() >= share)
            {
                continue;
            }
            float pick = random.Range(0.0f, total);
            Kind kind = options.back().kind;
            for (const Option& option : options)
            {
                pick -= option.weight;
                if (pick <= 0.0f)
                {
                    kind = option.kind;
                    break;
                }
            }
            // A group of them, or one alone.
            const bool groups = kind == Kind::Butte || kind == Kind::Spire || kind == Kind::Crystals || kind == Kind::IceTower;
            const int count = groups ? 1 + static_cast<int>(random.Unit() * 2.99f) : 1;
            const glm::vec2 centre{(static_cast<float>(cx) + random.Range(0.2f, 0.8f)) * kSpan, (static_cast<float>(cz) + random.Range(0.2f, 0.8f)) * kSpan};
            for (int k = 0; k < count; ++k)
            {
                Placed one;
                one.kind = kind;
                one.seed = static_cast<uint32_t>(random.Next());
                // The first of a group the greatest.
                const float grown = k == 0 ? 1.0f : random.Range(0.45f, 0.8f);
                switch (kind)
                {
                case Kind::Arch: one.size = random.Range(35.0f, 75.0f); one.radius = one.size * 1.1f; break;
                case Kind::Spire: one.size = random.Range(50.0f, 130.0f) * grown; one.radius = one.size * 0.25f; break;
                case Kind::Butte: one.size = random.Range(55.0f, 120.0f) * grown; one.radius = one.size * 1.6f; break;
                case Kind::BalancedRock: one.size = random.Range(28.0f, 55.0f); one.radius = one.size * 0.6f; break;
                case Kind::GreatTree: one.size = random.Range(50.0f, 75.0f); one.radius = one.size * 0.5f; break;
                case Kind::Crystals: one.size = random.Range(35.0f, 80.0f) * grown; one.radius = one.size * 0.7f; break;
                case Kind::IceTower: one.size = random.Range(50.0f, 105.0f) * grown; one.radius = one.size * 0.35f; break;
                case Kind::Mast: one.size = random.Range(28.0f, 60.0f); one.radius = one.size * 0.3f; break;
                default: break;
                }
                one.yaw = random.Range(0.0f, kTau);
                // Its rock a little apart in colour from the next one's.
                one.tint = glm::vec3(random.Range(0.85f, 1.12f)) * glm::vec3(random.Range(0.94f, 1.06f), 1.0f, random.Range(0.94f, 1.06f));
                const float a = random.Range(0.0f, kTau);
                const glm::vec2 at = centre + glm::vec2(std::cos(a), std::sin(a)) * (k == 0 ? 0.0f : random.Range(1.4f, 2.6f) * one.radius);
                const float fromMiddle = glm::length(at - middle);
                if (fromMiddle < clear + one.radius || fromMiddle > kReach)
                {
                    continue;
                }
                // On dry ground it could stand on, clear of the others.
                bool fits = terrain.WaterDepth(at.x, at.y) < -0.5f && terrain.GroundNormal(at.x, at.y).y > 0.7f;
                float lowest = terrain.GroundHeight(at.x, at.y);
                for (int r = 0; r < 8 && fits; ++r)
                {
                    const float b = kTau * static_cast<float>(r) / 8.0f;
                    const glm::vec2 round = at + glm::vec2(std::cos(b), std::sin(b)) * one.radius * 0.6f;
                    fits = terrain.WaterDepth(round.x, round.y) < -0.5f;
                    lowest = std::min(lowest, terrain.GroundHeight(round.x, round.y));
                }
                for (const Placed& other : placed)
                {
                    fits = fits && glm::distance(glm::vec2(other.at.x, other.at.z), at) > (other.radius + one.radius) * 0.8f;
                }
                if (!fits)
                {
                    continue;
                }
                one.at = {at.x, lowest, at.y};
                placed.push_back(one);
            }
        }
    }
    // On a frozen world with cliffs at its edge, often a waterfall frozen where it fell, down the tallest stretch of them in
    // sight of the site: found by looking out from the site's edge every way for where the ground rises most, most steeply.
    if (biome == "frozen")
    {
        Random fall((static_cast<uint64_t>(seed) << 31) ^ 0x1CEFA11ull);
        if (fall.Unit() < 0.7f)
        {
            float best = 12.0f;
            Placed icefall;
            constexpr int kWays = 48;
            for (int w = 0; w < kWays; ++w)
            {
                const float a = kTau * (static_cast<float>(w) + 0.5f) / kWays;
                const glm::vec2 way{std::cos(a), std::sin(a)};
                const glm::vec2 edge = middle + way * (plan.size * 0.5f / std::max(std::abs(way.x), std::abs(way.y)));
                float heights[41];
                for (int i = 0; i <= 40; ++i)
                {
                    const glm::vec2 p = edge + way * (static_cast<float>(i) * 2.0f);
                    heights[i] = terrain.GroundHeight(p.x, p.y);
                }
                for (int i = 0; i + 6 <= 40; ++i)
                {
                    const float rise = heights[i + 6] - heights[i];
                    if (rise <= best)
                    {
                        continue;
                    }
                    // Its lip: the highest point a little past the steepest rise.
                    int lip = i + 6;
                    for (int k = i + 6; k <= std::min(i + 12, 40); ++k)
                    {
                        lip = heights[k] > heights[lip] ? k : lip;
                    }
                    const glm::vec2 footAt = edge + way * (static_cast<float>(i) * 2.0f);
                    best = rise;
                    icefall.kind = Kind::Icefall;
                    icefall.seed = static_cast<uint32_t>(fall.Next());
                    icefall.at = {footAt.x, heights[i], footAt.y};
                    icefall.size = heights[lip] - heights[i];
                    icefall.radius = static_cast<float>(lip - i) * 2.0f; // how far back its lip is from its foot
                    icefall.yaw = std::atan2(-way.y, -way.x);            // facing the site
                }
            }
            if (icefall.kind == Kind::Icefall)
            {
                placed.push_back(icefall);
            }
        }
    }
    return placed;
}

std::vector<glm::vec3> Strikes(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain)
{
    std::vector<glm::vec3> strikes;
    if (biome != "stormworld")
    {
        return strikes;
    }
    Random random((static_cast<uint64_t>(seed) << 33) ^ 0x57121CEull);
    const float reach = SiteTerrain::kRoam + 20.0f;
    const int wanted = 12 + static_cast<int>(random.Unit() * 10.0f);
    for (int tries = 0; tries < wanted * 10 && static_cast<int>(strikes.size()) < wanted; ++tries)
    {
        const glm::vec2 at{plan.origin.x - reach + random.Range(0.0f, plan.size + reach * 2.0f), plan.origin.z - reach + random.Range(0.0f, plan.size + reach * 2.0f)};
        if (terrain.Wildness(at.x, at.y) < 0.8f || terrain.GroundNormal(at.x, at.y).y < 0.9f || terrain.WaterDepth(at.x, at.y) > -0.3f)
        {
            continue;
        }
        strikes.emplace_back(at.x, terrain.GroundHeight(at.x, at.y), at.y);
    }
    return strikes;
}

MeshData Strike(const glm::vec3& at, uint32_t seed, const SiteTerrain& terrain)
{
    // Branches of fused glass running out over the ground from where the bolt went in, forking as they go and thinning to
    // nothing, half sunk in it; and the glassed pit at the middle.
    MeshData mesh;
    Random random((static_cast<uint64_t>(seed) << 11) ^ static_cast<uint64_t>(at.x * 31.0f) ^ (static_cast<uint64_t>(at.z * 17.0f) << 20));
    struct Branch
    {
        glm::vec2 from;
        float heading;
        float length;
        float radius;
        int depth;
    };
    std::vector<Branch> branches;
    const int arms = 4 + static_cast<int>(random.Unit() * 4.0f);
    for (int a = 0; a < arms; ++a)
    {
        branches.push_back({{at.x, at.z}, kTau * (static_cast<float>(a) + random.Range(0.0f, 0.7f)) / static_cast<float>(arms), random.Range(2.5f, 6.0f),
                            random.Range(0.08f, 0.14f), 0});
    }
    for (size_t b = 0; b < branches.size() && b < 60; ++b)
    {
        const Branch branch = branches[b];
        std::vector<glm::vec3> path;
        std::vector<float> radii;
        glm::vec2 p = branch.from;
        float heading = branch.heading;
        constexpr int kSteps = 8;
        for (int i = 0; i <= kSteps; ++i)
        {
            const float t = static_cast<float>(i) / kSteps;
            path.emplace_back(p.x, terrain.GroundHeight(p.x, p.y) + 0.01f, p.y);
            radii.push_back(std::max(branch.radius * (1.0f - 0.85f * t), 0.015f));
            heading += random.Range(-0.45f, 0.45f);
            p += glm::vec2(std::cos(heading), std::sin(heading)) * (branch.length / kSteps);
            // Forking now and then, the forks thinner and shorter.
            if (branch.depth < 2 && i > 1 && i < kSteps && random.Unit() < 0.22f)
            {
                branches.push_back({p, heading + random.Range(0.4f, 1.0f) * (random.Unit() < 0.5f ? -1.0f : 1.0f), branch.length * random.Range(0.4f, 0.7f),
                                    radii.back() * 0.8f, branch.depth + 1});
            }
        }
        Sweep(mesh, path, radii, 5, glm::vec3(0.0f, 1.0f, 0.0f), 0.25f, 0.0f, seed + static_cast<uint32_t>(b), false, true);
    }
    // The pit: a low glassy rim round where it went in.
    std::vector<glm::vec3> rim;
    std::vector<float> rimRadii;
    for (int i = 0; i <= 4; ++i)
    {
        const float t = static_cast<float>(i) / 4.0f;
        rim.push_back(at + glm::vec3(0.0f, -0.2f + 0.3f * t, 0.0f));
        rimRadii.push_back(0.9f * (1.0f - 0.5f * t * t));
    }
    Sweep(mesh, rim, rimRadii, 10, glm::vec3(1.0f, 0.0f, 0.0f), 0.3f, 0.0f, seed + 999u);
    SmoothNormals(mesh);
    for (MeshVertex& vertex : mesh.vertices)
    {
        vertex.color = 0xffffffffu;
    }
    return mesh;
}

MeshData Shape(const Placed& placed)
{
    MeshData mesh;
    // Each from its own numbers: no two the same shape, however many a world has.
    const uint32_t seed = placed.seed;
    Random random((static_cast<uint64_t>(seed) << 19) ^ 0x5A4D3Bull ^ static_cast<uint64_t>(placed.kind));
    const float h = placed.size;
    const glm::vec3 foot = placed.at;
    const glm::vec3 facing{std::cos(placed.yaw), 0.0f, std::sin(placed.yaw)};
    const glm::vec3 side{-facing.z, 0.0f, facing.x};
    switch (placed.kind)
    {
    case Kind::Arch:
    {
        // A span of rock over open air, standing in the ground at both ends: wide or narrow, round or flattened, one foot
        // thicker than the other, its span bent a little out of line.
        const float span = h * random.Range(0.8f, 1.8f);
        const float thick = h * random.Range(0.1f, 0.2f);
        const float round = random.Range(0.5f, 1.0f);
        const float lopsided = random.Range(-0.5f, 0.5f);
        const float bend = h * random.Range(-0.15f, 0.15f);
        std::vector<glm::vec3> path;
        std::vector<float> radii;
        constexpr int kSteps = 40;
        for (int i = 0; i <= kSteps; ++i)
        {
            const float t = static_cast<float>(i) / kSteps;
            const float a = 3.14159265f * t;
            const float up = std::pow(std::sin(a), round) * h - 6.0f * (1.0f - std::sin(a));
            path.push_back(foot + facing * (-0.5f * span * std::cos(a)) + side * (bend * std::sin(a)) + glm::vec3(0.0f, up, 0.0f));
            const float feet = 0.8f * std::pow(1.0f - std::sin(a), 2.0f) * (1.0f + lopsided * (t < 0.5f ? 1.0f : -1.0f));
            radii.push_back(thick * (1.0f + feet));
        }
        Sweep(mesh, path, radii, 20, side, 0.3f, random.Range(0.0f, 0.05f), seed, true, true, random.Range(0.1f, 0.35f));
        break;
    }
    case Kind::Spire:
    {
        // A pillar of rock worn from softer and harder layers: bulging and pinched a few times up its height (or barely at
        // all), wide at its foot, its top a point or a broken stump.
        const float base = h * random.Range(0.1f, 0.2f);
        const float bulges = random.Range(1.0f, 4.5f);
        const float pinch = random.Range(0.0f, 0.3f);
        const float phase = random.Range(0.0f, kTau);
        const bool pointed = random.Unit() < 0.5f;
        Column(mesh, foot, h, 54, 26,
               [&](float t)
               {
                   const float taper = 1.25f - (pointed ? 0.95f : 0.6f) * std::pow(t, 0.8f);
                   const float worn = 1.0f + pinch * std::sin(t * bulges * kTau + phase);
                   return base * taper * worn * (1.0f + 0.3f * std::exp(-t * 10.0f));
               },
               random.Range(0.0f, 0.15f), placed.yaw, random.Range(0.2f, 0.35f), random.Range(0.02f, 0.07f), seed, pointed, random.Range(0.1f, 0.4f));
        break;
    }
    case Kind::Butte:
    {
        // A flat-topped hill of layered rock: broad or tall, sometimes a smaller tier on its top, its sides sheer, a skirt of
        // fallen rock round its foot.
        const float wide = h * random.Range(0.6f, 1.6f);
        const bool tiered = random.Unit() < 0.4f;
        const float step = random.Range(0.55f, 0.75f);
        const float upper = random.Range(0.45f, 0.7f);
        const float skirtSize = random.Range(0.3f, 0.7f);
        Column(mesh, foot, h, 50, 72,
               [&](float t)
               {
                   const float skirt = 1.0f + skirtSize * std::pow(std::max(0.0f, 1.0f - t / 0.3f), 1.4f);
                   const float tier = tiered ? glm::mix(1.0f, upper, glm::smoothstep(step - 0.03f, step + 0.03f, t)) : 1.0f;
                   const float rim = 1.0f - 0.08f * std::pow(std::max(0.0f, (t - 0.92f) / 0.08f), 2.0f);
                   return wide * skirt * tier * rim;
               },
               random.Range(0.0f, 0.05f), placed.yaw, random.Range(0.25f, 0.4f), random.Range(0.015f, 0.04f), seed, false, random.Range(0.25f, 0.55f));
        break;
    }
    case Kind::BalancedRock:
    {
        // A worn pillar, narrowing to a neck, and a great block of harder rock sitting on it, off its middle.
        const float neck = random.Range(0.45f, 0.75f);
        const float stand = random.Range(0.5f, 0.68f);
        const float base = h * random.Range(0.16f, 0.24f);
        Column(mesh, foot, h * stand, 26, 18, [&](float t) { return base * (1.2f - (1.2f - neck) * t); }, 0.0f, placed.yaw, 0.25f, 0.06f, seed, false,
               0.2f);
        const float cap = h * random.Range(0.24f, 0.36f);
        const glm::vec3 top = foot + glm::vec3(0.0f, h * stand - 2.0f, 0.0f) + side * (cap * random.Range(-0.25f, 0.25f));
        std::vector<glm::vec3> path;
        std::vector<float> radii;
        constexpr int kSteps = 16;
        for (int i = 0; i <= kSteps; ++i)
        {
            const float t = static_cast<float>(i) / kSteps;
            path.push_back(top + glm::vec3(0.0f, t * h * (1.0f - stand), 0.0f));
            radii.push_back(cap * std::sqrt(std::max(0.0f, std::sin(3.14159265f * (0.08f + 0.84f * t)))));
        }
        Sweep(mesh, path, radii, 20, glm::vec3(1.0f, 0.0f, 0.0f), 0.3f, 0.0f, seed + 5u, true, true, random.Range(0.2f, 0.5f));
        break;
    }
    case Kind::IceTower:
    {
        // A tower of blue ice, broken-sided, leaning, its faces flatter than rock's.
        const float base = h * random.Range(0.12f, 0.2f);
        const int sides = 5 + static_cast<int>(random.Unit() * 4.0f);
        const float shrink = random.Range(0.6f, 0.85f);
        Column(mesh, foot, h, 24, sides, [&](float t) { return base * (1.15f - shrink * t); }, random.Range(0.05f, 0.22f), placed.yaw,
               0.12f, 0.0f, seed, random.Unit() < 0.6f, 0.2f);
        break;
    }
    case Kind::Mast:
    {
        // An old lattice mast left standing: four legs closing in as it rises, braced at every few metres across and from
        // corner to corner; sometimes leaning where its footings gave, sometimes its top gone.
        const float base = h * random.Range(0.08f, 0.12f);
        const float top = std::max(base * 0.18f, 0.6f);
        const float lean = random.Range(0.0f, 0.06f);
        const bool broken = random.Unit() < 0.35f;
        const float reach = broken ? h * random.Range(0.55f, 0.8f) : h;
        const auto corner = [&](int c, float y)
        {
            const float t = y / h;
            const float half = glm::mix(base, top, t) * 0.5f;
            const glm::vec3 local{(c == 0 || c == 3 ? -half : half), y, (c < 2 ? -half : half)};
            // Turned to its own way, and leaning over.
            const glm::vec3 turned = facing * local.x + side * local.z + glm::vec3(0.0f, local.y, 0.0f);
            return foot + turned + facing * (lean * y);
        };
        const auto strut = [&](const glm::vec3& a, const glm::vec3& b, float r)
        {
            const glm::vec3 way = glm::normalize(b - a);
            Sweep(mesh, {a, b}, {r, r}, 4, std::abs(way.y) > 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, 0.0f, seed);
        };
        const float level = std::max(3.0f, base * 0.8f);
        for (int c = 0; c < 4; ++c)
        {
            strut(corner(c, -1.0f), corner(c, reach), 0.14f + h * 0.0015f);
        }
        for (float y = level; y < reach - 0.5f; y += level)
        {
            for (int c = 0; c < 4; ++c)
            {
                const int n = (c + 1) % 4;
                strut(corner(c, y), corner(n, y), 0.06f);
                // Each face crossed from one level to the next.
                strut(corner(c, y - level), corner(n, y), 0.045f);
                strut(corner(n, y - level), corner(c, y), 0.045f);
            }
        }
        if (!broken)
        {
            // A whip aerial and a pair of arms at its head.
            const glm::vec3 head = corner(0, reach) * 0.25f + corner(1, reach) * 0.25f + corner(2, reach) * 0.25f + corner(3, reach) * 0.25f;
            strut(head, head + glm::vec3(0.0f, h * 0.12f, 0.0f), 0.05f);
            strut(head - side * (top * 1.6f), head + side * (top * 1.6f), 0.07f);
        }
        break;
    }
    case Kind::Icefall:
    {
        // Water frozen as it fell, hanging down the face from its lip to its foot: columns side by side, the widest in its
        // middle, some reaching the ground and spreading where they land, some ending in icicles partway; standing a little
        // off the rock all the way down, bulging where the face does.
        const glm::vec3 lip = foot - facing * placed.radius + glm::vec3(0.0f, h, 0.0f);
        const float wide = random.Range(12.0f, 24.0f);
        const int columns = 11 + static_cast<int>(random.Unit() * 7.0f);
        for (int c = 0; c < columns; ++c)
        {
            const float across = (static_cast<float>(c) / static_cast<float>(columns - 1) - 0.5f + random.Range(-0.03f, 0.03f)) * wide;
            const float middleness = 1.0f - std::abs(across) / (wide * 0.5f + 1.0e-3f);
            const float reach = middleness > 0.35f || random.Unit() < 0.4f ? 1.0f : random.Range(0.35f, 0.8f);
            const float r = random.Range(0.9f, 2.0f) * (0.6f + 0.6f * middleness);
            std::vector<glm::vec3> path;
            std::vector<float> radii;
            constexpr int kSteps = 16;
            for (int i = 0; i <= kSteps; ++i)
            {
                const float t = static_cast<float>(i) / kSteps * reach;
                // Down the face, the face itself curving out from lip to foot; off it by the ice's own thickness.
                const glm::vec3 onFace = glm::mix(lip, foot, glm::vec3(std::pow(t, 0.7f), t, std::pow(t, 0.7f)));
                path.push_back(onFace + side * across + facing * (r * 0.8f + 0.6f * std::sin(t * 3.14159265f)) +
                               side * (0.5f * std::sin(t * 7.0f + static_cast<float>(c))));
                const float flare = reach >= 1.0f ? 1.0f + 1.6f * glm::smoothstep(0.85f, 1.0f, t) : 1.0f - 0.9f * glm::smoothstep(0.6f, 1.0f, t / reach);
                radii.push_back(r * flare);
            }
            Sweep(mesh, path, radii, 7, side, 0.06f, 0.0f, seed + static_cast<uint32_t>(c));
        }
        // And the ice heaped where it lands.
        Column(mesh, foot + facing * 2.0f, 2.5f, 6, 14, [&](float t) { return wide * 0.6f * (1.0f - 0.6f * t); }, 0.0f, placed.yaw, 0.15f, 0.0f, seed + 99u,
               false, 0.3f);
        break;
    }
    case Kind::Crystals:
    {
        // Great six-sided crystals growing out of one place, the largest near upright, the rest leaning out round it.
        const int count = 4 + static_cast<int>(random.Unit() * 6.0f);
        for (int i = 0; i < count; ++i)
        {
            const float size = i == 0 ? h : h * random.Range(0.3f, 0.75f);
            const float a = random.Range(0.0f, kTau);
            const float tilt = i == 0 ? random.Range(0.0f, 0.2f) : random.Range(0.3f, 0.8f);
            const glm::vec3 way = glm::normalize(glm::vec3(std::cos(a) * std::sin(tilt), std::cos(tilt), std::sin(a) * std::sin(tilt)));
            const glm::vec3 from = foot + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * (i == 0 ? 0.0f : h * 0.12f) - way * 4.0f;
            const float r = size * random.Range(0.09f, 0.15f);
            std::vector<glm::vec3> path{from, from + way * size * random.Range(0.75f, 0.88f), from + way * size};
            std::vector<float> radii{r, r, 0.05f};
            Sweep(mesh, path, radii, 6, glm::abs(way.y) > 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, 0.0f,
                  seed + static_cast<uint32_t>(i));
        }
        break;
    }
    default: break;
    }
    SmoothNormals(mesh);
    // Its own shade of the world's rock.
    const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const uint32_t colour = byte(placed.tint.r * 0.88f) | (byte(placed.tint.g * 0.88f) << 8) | (byte(placed.tint.b * 0.88f) << 16) | (255u << 24);
    for (MeshVertex& vertex : mesh.vertices)
    {
        vertex.color = colour;
    }
    return mesh;
}

} // namespace Landmark

} // namespace pred
