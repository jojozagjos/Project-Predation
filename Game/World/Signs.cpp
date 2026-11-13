#include "Game/World/Signs.h"

#include "Engine/Render/MeshSimplify.h"
#include "Engine/Render/Sdf.h"
#include "Engine/Render/SurfaceNets.h"
#include "Game/World/SiteFauna.h"
#include "Game/World/SitePlan.h"
#include "Game/World/SiteTerrain.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace Signs
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

} // namespace

std::vector<Formation> Make(uint32_t seed, float strength, const SitePlan& plan, const SiteTerrain& terrain, const SiteFauna& fauna)
{
    std::vector<Formation> formations;
    if (strength <= 0.0f || !fauna.Any())
    {
        return formations;
    }
    Random random((static_cast<uint64_t>(seed) << 35) ^ 0x516A5ull);
    // Where they gather: round the lair -- out on the land, the lair itself; in a building or a cave, the ground outside its way in --
    // or, for one only passing, along a line across the site.
    glm::vec3 centre = plan.origin + glm::vec3(plan.size * 0.5f, 0.0f, plan.size * 0.5f);
    glm::vec3 along{1.0f, 0.0f, 0.0f};
    bool knot = false;
    if (fauna.lair == SiteFauna::Lair::Outside)
    {
        centre = fauna.lairAt;
        knot = true;
    }
    else if (fauna.lair == SiteFauna::Lair::Cave && fauna.lairCave >= 0 && fauna.lairCave < static_cast<int>(plan.caves.size()))
    {
        const SitePlan::Cave& cave = plan.caves[static_cast<size_t>(fauna.lairCave)];
        centre = cave.mouth + cave.out * 14.0f;
    }
    else if (fauna.lair == SiteFauna::Lair::Building && fauna.lairBuilding >= 0)
    {
        // Outside the building, the side towards its lair.
        const glm::vec3 lair = fauna.lairAt;
        const glm::vec3 away = glm::vec3(lair.x - centre.x, 0.0f, lair.z - centre.z);
        centre = lair + (glm::length(away) > 1.0f ? glm::normalize(away) : glm::vec3(1.0f, 0.0f, 0.0f)) * 22.0f;
    }
    else
    {
        const float angle = random.Range(0.0f, kTau);
        along = {std::cos(angle), 0.0f, std::sin(angle)};
    }
    const bool passing = fauna.presence == SiteFauna::Presence::Passing;
    const int wanted = passing ? 1 + static_cast<int>(random.Unit() * 2.0f) : 2 + static_cast<int>(strength * 2.5f * random.Unit() + strength);
    const float reach = SiteTerrain::kRoam + 10.0f;
    for (int tries = 0; tries < wanted * 40 && static_cast<int>(formations.size()) < wanted; ++tries)
    {
        glm::vec2 at;
        if (passing)
        {
            // Along its way across, a little either side of it.
            const float t = random.Range(-0.45f, 0.45f) * plan.size;
            at = glm::vec2(centre.x + along.x * t - along.z * random.Range(-8.0f, 8.0f), centre.z + along.z * t + along.x * random.Range(-8.0f, 8.0f));
        }
        else
        {
            // The first by where it lives (its knot, a few metres off: not over it), the rest round it.
            const float out = formations.empty() && knot ? random.Range(6.0f, 10.0f) : random.Range(12.0f, 38.0f);
            const float angle = random.Range(0.0f, kTau);
            at = glm::vec2(centre.x + std::cos(angle) * out, centre.z + std::sin(angle) * out);
        }
        if (at.x < plan.origin.x - reach || at.y < plan.origin.z - reach || at.x > plan.origin.x + plan.size + reach || at.y > plan.origin.z + plan.size + reach)
        {
            continue;
        }
        // (On the site's own ground as much as the wild round it -- it lives here -- but not on the shuttle's pad, nor up a slope, in
        // water, in a building or far past where anybody goes.)
        if (terrain.GroundNormal(at.x, at.y).y < 0.7f || terrain.WaterDepth(at.x, at.y) > -0.5f || terrain.PastRoam(at.x, at.y) > 10.0f ||
            glm::distance(at, glm::vec2(plan.landing.x, plan.landing.z)) < 22.0f ||
            plan.Indoors(glm::vec3(at.x, terrain.GroundHeight(at.x, at.y), at.y), 3.0f))
        {
            continue;
        }
        bool clear = true;
        for (const Formation& other : formations)
        {
            clear = clear && glm::distance(glm::vec2(other.at.x, other.at.z), at) > 14.0f;
        }
        if (!clear)
        {
            continue;
        }
        Formation formation;
        formation.at = {at.x, terrain.GroundHeight(at.x, at.y), at.y};
        // The knot at the lair the biggest; the rest smaller, the fainter the smaller.
        const float big = formations.empty() && knot ? 1.15f : 1.0f;
        formation.size = glm::mix(4.0f, 22.0f, strength) * random.Range(0.75f, 1.2f) * big * (passing ? 0.6f : 1.0f);
        formation.yaw = random.Range(0.0f, kTau);
        formation.seed = static_cast<uint32_t>(random.Next());
        formations.push_back(formation);
    }
    return formations;
}

MeshData Rock(const Formation& formation, float strength, const SiteTerrain& terrain)
{
    // From a knot half sunk in the ground, branches running out over it as roots do -- rising out of it, arching along and
    // dipping back under, forking and thinning as they go -- low over the ground, nothing standing up out of it. Too regular a
    // branching for rock, if anybody looked at it long enough.
    //
    // One surface, the knot and every branch and fork blended into each other as one thing grown (a distance field sculpted and
    // meshed: Sdf, SurfaceNets): made of tubes swept along each branch and a swell for the knot, it was pieces stuck into each
    // other -- seams where they met, and the swell running out along it broke at every one.
    Random random((static_cast<uint64_t>(formation.seed) << 13) ^ 0xB2A2Cull);
    const float thick = glm::mix(0.25f, 1.2f, strength);
    const float cell = std::clamp(thick * 0.22f, 0.06f, 0.3f);
    // A tube of the field between two rings, with how far out from the knot each end is.
    struct Segment
    {
        glm::vec3 a;
        glm::vec3 b;
        float ra;
        float rb;
        float alongA;
        float alongB;
    };
    std::vector<Segment> segments;
    struct Branch
    {
        glm::vec3 from;
        float heading;
        float length;
        float radius;
        int depth;
        float along; // how far out from the knot it starts, along the branches it grows from
    };
    std::vector<Branch> branches;
    const int arms = 3 + static_cast<int>(random.Unit() * 4.0f);
    for (int a = 0; a < arms; ++a)
    {
        const float heading = formation.yaw + kTau * (static_cast<float>(a) + random.Range(0.0f, 0.6f)) / static_cast<float>(arms);
        branches.push_back({formation.at, heading, formation.size * random.Range(0.6f, 1.0f), thick * random.Range(0.7f, 1.0f), 0, 0.0f});
    }
    for (size_t b = 0; b < branches.size() && b < 90; ++b)
    {
        const Branch branch = branches[b];
        glm::vec2 p{branch.from.x, branch.from.z};
        float heading = branch.heading;
        constexpr int kSteps = 12;
        // How many times along it it rises out of the ground and dips back: the branch's own.
        const float waves = random.Range(1.0f, 2.5f);
        glm::vec3 last{0.0f};
        float lastR = 0.0f;
        for (int i = 0; i <= kSteps; ++i)
        {
            const float t = static_cast<float>(i) / kSteps;
            // Never thinner than the field can hold (a third of a cell less would break into beads).
            const float r = std::max(branch.radius * (1.0f - 0.8f * t), cell * 0.75f);
            const float ground = terrain.GroundHeight(p.x, p.y);
            const float showing = std::sin(t * 3.14159265f * waves) * 0.5f + 0.2f;
            float y = ground + r * showing;
            if (branch.depth > 0)
            {
                // A fork grows out of the middle of the branch it leaves, and settles to its own height over its first steps.
                y = glm::mix(branch.from.y, y, glm::smoothstep(0.0f, 0.2f, t));
            }
            const glm::vec3 at{p.x, y, p.y};
            if (i > 0)
            {
                const float step = branch.length / kSteps;
                segments.push_back({last, at, lastR, r, branch.along + step * static_cast<float>(i - 1), branch.along + step * static_cast<float>(i)});
            }
            last = at;
            lastR = r;
            heading += random.Range(-0.35f, 0.35f) * std::min(1.0f, (branch.length / kSteps) / std::max(2.0f * r, 0.01f));
            p += glm::vec2(std::cos(heading), std::sin(heading)) * (branch.length / kSteps);
            if (branch.depth < 2 && i > 2 && i < kSteps - 1 && random.Unit() < 0.18f)
            {
                branches.push_back({at, heading + random.Range(0.5f, 1.1f) * (random.Unit() < 0.5f ? -1.0f : 1.0f), branch.length * random.Range(0.35f, 0.6f),
                                    r * 0.85f, branch.depth + 1, branch.along + branch.length * t});
            }
        }
    }

    // The knot: a swell half sunk, the branches growing out of it.
    const glm::vec3 knot = formation.at + glm::vec3(0.0f, -thick * 0.25f, 0.0f);
    const glm::vec3 knotRadii{thick * 2.2f, thick * 1.35f, thick * 2.2f};
    // How far into each other the parts melt: the forks and the knot's foot as one growth.
    const float blend = thick * 0.45f;

    // The segments by where they are, so a point asks only the few near it.
    glm::vec3 lo = knot - knotRadii;
    glm::vec3 hi = knot + knotRadii;
    float widest = 0.0f;
    for (const Segment& s : segments)
    {
        lo = glm::min(lo, glm::min(s.a, s.b) - glm::vec3(s.ra));
        hi = glm::max(hi, glm::max(s.a, s.b) + glm::vec3(s.ra));
        widest = std::max(widest, std::max(s.ra, s.rb));
    }
    lo -= glm::vec3(blend + cell * 3.0f);
    hi += glm::vec3(blend + cell * 3.0f);
    constexpr float kBucket = 2.0f;
    const int bucketsX = std::max(1, static_cast<int>(std::ceil((hi.x - lo.x) / kBucket)));
    const int bucketsZ = std::max(1, static_cast<int>(std::ceil((hi.z - lo.z) / kBucket)));
    std::vector<std::vector<uint32_t>> buckets(static_cast<size_t>(bucketsX * bucketsZ));
    // Asked of anything within this of a point: far enough that what is coarsely sampled between is still the right side.
    const float reach = widest + blend + cell * 6.0f;
    for (size_t i = 0; i < segments.size(); ++i)
    {
        const Segment& s = segments[i];
        const glm::vec3 a = glm::min(s.a, s.b) - glm::vec3(reach);
        const glm::vec3 b = glm::max(s.a, s.b) + glm::vec3(reach);
        for (int z = std::max(0, static_cast<int>((a.z - lo.z) / kBucket)); z <= std::min(bucketsZ - 1, static_cast<int>((b.z - lo.z) / kBucket)); ++z)
        {
            for (int x = std::max(0, static_cast<int>((a.x - lo.x) / kBucket)); x <= std::min(bucketsX - 1, static_cast<int>((b.x - lo.x) / kBucket)); ++x)
            {
                buckets[static_cast<size_t>(x + z * bucketsX)].push_back(static_cast<uint32_t>(i));
            }
        }
    }
    const auto near = [&](const glm::vec3& p) -> const std::vector<uint32_t>&
    {
        const int x = std::clamp(static_cast<int>((p.x - lo.x) / kBucket), 0, bucketsX - 1);
        const int z = std::clamp(static_cast<int>((p.z - lo.z) / kBucket), 0, bucketsZ - 1);
        return buckets[static_cast<size_t>(x + z * bucketsX)];
    };
    const uint32_t lumps = formation.seed * 7u + 3u;
    const auto field = [&](const glm::vec3& p)
    {
        float d = Sdf::Ellipsoid(p - knot, knotRadii);
        for (const uint32_t i : near(p))
        {
            const Segment& s = segments[i];
            d = Sdf::SmoothUnion(d, Sdf::RoundCone(p, s.a, s.b, s.ra, s.rb), blend * std::min(1.0f, (s.ra + s.rb) / thick));
        }
        // Lumped a little, as grown things are: never as much as a boulder.
        return d + (Sdf::Fbm(p / (thick * 1.6f), lumps, 2) - 0.5f) * thick * 0.18f;
    };

    const DistanceGrid grid = SampleDistance(lo, hi, cell, field);
    MeshData surface = SurfaceNets(grid);

    // Onto the surface itself, its normal from the shape; and for the swell running out along it (Material::organic), how far out
    // from the knot each point is (x) and how thick it is there (y), blended between the parts near it as the shape is.
    const auto normalAt = [&](const glm::vec3& p)
    {
        const float h = cell * 0.5f;
        const glm::vec3 n{field(p + glm::vec3(h, 0, 0)) - field(p - glm::vec3(h, 0, 0)), field(p + glm::vec3(0, h, 0)) - field(p - glm::vec3(0, h, 0)),
                          field(p + glm::vec3(0, 0, h)) - field(p - glm::vec3(0, 0, h))};
        const float length = glm::length(n);
        return length > 1e-6f ? n / length : glm::vec3(0.0f, 1.0f, 0.0f);
    };
    for (MeshVertex& vertex : surface.vertices)
    {
        glm::vec3 p = vertex.position;
        p -= normalAt(p) * field(p);
        vertex.position = p;
        vertex.normal = normalAt(p);
        const float knotDistance = std::max(Sdf::Ellipsoid(p - knot, knotRadii), 0.0f);
        float weight = std::exp(-knotDistance / std::max(blend * 0.5f, 0.01f));
        float along = 0.0f;
        float height = thick * 0.6f * weight;
        for (const uint32_t i : near(p))
        {
            const Segment& s = segments[i];
            const glm::vec3 ab = s.b - s.a;
            const float t = std::clamp(glm::dot(p - s.a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
            const float r = glm::mix(s.ra, s.rb, t);
            const float d = std::max(glm::distance(p, s.a + ab * t) - r, 0.0f);
            const float w = std::exp(-d / std::max(blend * 0.5f, 0.01f));
            weight += w;
            along += glm::mix(s.alongA, s.alongB, t) * w;
            height += r * w;
        }
        vertex.uv = weight > 0.0f ? glm::vec2(along / weight, height / weight) : glm::vec2(0.0f, thick * 0.6f);
        vertex.color = 0xffffffffu;
    }

    // What lies well under the ground is never seen: left out, before it is made simpler.
    {
        std::vector<uint32_t> kept;
        kept.reserve(surface.indices.size());
        for (size_t t = 0; t + 2 < surface.indices.size(); t += 3)
        {
            bool buried = true;
            for (size_t k = 0; k < 3 && buried; ++k)
            {
                const glm::vec3& v = surface.vertices[surface.indices[t + k]].position;
                buried = v.y < terrain.GroundHeight(v.x, v.z) - cell * 1.5f;
            }
            if (!buried)
            {
                kept.insert(kept.end(), {surface.indices[t], surface.indices[t + 1], surface.indices[t + 2]});
            }
        }
        surface.indices = std::move(kept);
    }
    std::vector<uint32_t> kept;
    const size_t target = static_cast<size_t>(std::clamp(formation.size * 450.0f, 3000.0f, 14000.0f));
    return SimplifyMesh(surface, target, cell * 0.08f, kept);
}

float Pulse(double seconds, float rhythm)
{
    // A soft swell, rising over a second or so and dying away over a few more, once in each `rhythm`.
    const double cycle = seconds / std::max(static_cast<double>(rhythm), 1.0);
    const auto p = static_cast<float>(cycle - std::floor(cycle));
    const float span = 5.0f / std::max(rhythm, 6.0f);
    return glm::smoothstep(0.0f, span * 0.25f, p) * (1.0f - glm::smoothstep(span * 0.25f, span, p));
}

} // namespace Signs

} // namespace pred
