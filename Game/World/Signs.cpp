#include "Game/World/Signs.h"

#include "Game/World/Landmark.h"
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

std::vector<Formation> Make(uint32_t seed, float strength, const SitePlan& plan, const SiteTerrain& terrain)
{
    std::vector<Formation> formations;
    if (strength <= 0.0f)
    {
        return formations;
    }
    Random random((static_cast<uint64_t>(seed) << 35) ^ 0x516A5ull);
    // One where they are faint, a few where they are plain; on the world's own ground, in reach or just past it.
    const int wanted = 1 + static_cast<int>(strength * 3.0f * random.Unit() + strength * 1.5f);
    const float reach = SiteTerrain::kRoam + 10.0f;
    for (int tries = 0; tries < wanted * 20 && static_cast<int>(formations.size()) < wanted; ++tries)
    {
        const glm::vec2 at{plan.origin.x - reach + random.Range(0.0f, plan.size + reach * 2.0f), plan.origin.z - reach + random.Range(0.0f, plan.size + reach * 2.0f)};
        if (terrain.Wildness(at.x, at.y) < 0.9f || terrain.GroundNormal(at.x, at.y).y < 0.8f || terrain.WaterDepth(at.x, at.y) > -0.5f ||
            terrain.PastRoam(at.x, at.y) > 10.0f)
        {
            continue;
        }
        bool clear = true;
        for (const Formation& other : formations)
        {
            clear = clear && glm::distance(glm::vec2(other.at.x, other.at.z), at) > 40.0f;
        }
        if (!clear)
        {
            continue;
        }
        Formation formation;
        formation.at = {at.x, terrain.GroundHeight(at.x, at.y), at.y};
        formation.size = glm::mix(5.0f, 30.0f, strength) * random.Range(0.75f, 1.25f);
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
    MeshData mesh;
    Random random((static_cast<uint64_t>(formation.seed) << 13) ^ 0xB2A2Cull);
    const float thick = glm::mix(0.25f, 1.2f, strength);
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
    // Each point's texture coordinates say how far out from the knot it is (x) and how thick the rock is there (y), for the
    // swell that runs out along the branches (Material::organic): read from the ring nearest it.
    const auto mark = [&mesh](size_t first, const std::vector<glm::vec3>& path, const std::vector<float>& radii, float along, float height)
    {
        std::vector<float> out(path.size(), along);
        for (size_t i = 1; i < path.size(); ++i)
        {
            out[i] = out[i - 1] + glm::distance(path[i], path[i - 1]);
        }
        const size_t sides = path.empty() ? 1 : (mesh.vertices.size() - first) / path.size();
        for (size_t v = first; v < mesh.vertices.size(); ++v)
        {
            size_t ring = (v - first) / std::max(sides, size_t{1});
            if (ring >= path.size())
            {
                // (A cap's middle: whichever end it closes.)
                ring = glm::distance(mesh.vertices[v].position, path.front()) < glm::distance(mesh.vertices[v].position, path.back()) ? 0 : path.size() - 1;
            }
            mesh.vertices[v].uv = {out[ring], radii[ring] * height};
        }
    };
    for (size_t b = 0; b < branches.size() && b < 90; ++b)
    {
        const Branch branch = branches[b];
        std::vector<glm::vec3> path;
        std::vector<float> radii;
        glm::vec2 p{branch.from.x, branch.from.z};
        float heading = branch.heading;
        constexpr int kSteps = 12;
        // How many times along it it rises out of the ground and dips back: the branch's own, not picked again at each ring
        // (which threw it up and down from one ring to the next, folded and gapped).
        const float waves = random.Range(1.0f, 2.5f);
        for (int i = 0; i <= kSteps; ++i)
        {
            const float t = static_cast<float>(i) / kSteps;
            // (Out of the knot's side as it is, no flare into it: flared, its rings narrowed faster than they are apart where it
            // left the knot, and the tube folded over on itself into a fin standing up off the knot.)
            const float r = std::max(branch.radius * (1.0f - 0.8f * t), 0.04f);
            // Along the ground: in and out of it, the most of it showing in its middle.
            const float ground = terrain.GroundHeight(p.x, p.y);
            const float showing = std::sin(t * 3.14159265f * waves) * 0.5f + 0.2f;
            float y = ground + r * showing;
            if (branch.depth > 0)
            {
                // A fork grows out of the middle of the branch it leaves, and settles to its own height over its first
                // steps: begun a step further on, beside it, it left a gap at the fork.
                y = glm::mix(branch.from.y, y, glm::smoothstep(0.0f, 0.2f, t));
            }
            path.emplace_back(p.x, y, p.y);
            // Turning no tighter than it is thick: bent round less than its own width, its inside folded into a fin.
            heading += random.Range(-0.35f, 0.35f) * std::min(1.0f, (branch.length / kSteps) / std::max(2.0f * r, 0.01f));
            p += glm::vec2(std::cos(heading), std::sin(heading)) * (branch.length / kSteps);
            radii.push_back(r);
            if (branch.depth < 2 && i > 2 && i < kSteps - 1 && random.Unit() < 0.18f)
            {
                branches.push_back({path.back(), heading + random.Range(0.5f, 1.1f) * (random.Unit() < 0.5f ? -1.0f : 1.0f), branch.length * random.Range(0.35f, 0.6f),
                                    r * 0.85f, branch.depth + 1, branch.along + branch.length * t});
            }
        }
        // (Lumped a little, not as much as a boulder: lumps the size of rock on a branch a hand thick pushed some of its points
        // in through the others, and it was seen through. A fork closed where it starts, inside the branch it leaves.)
        const size_t first = mesh.vertices.size();
        Landmark::Sweep(mesh, path, radii, 10, glm::vec3(0.0f, 1.0f, 0.0f), 0.05f, 0.0f,
                        formation.seed + static_cast<uint32_t>(b), branch.depth > 0, true);
        mark(first, path, radii, branch.along, 1.0f);
    }
    // The knot: a swell of the same rock, closing over at its top, the branches growing out of it.
    std::vector<glm::vec3> knot;
    std::vector<float> knotRadii;
    // (Its rings closer together towards its top, where it turns over: evenly apart, its top was a flat cap of facets.)
    for (int i = 0; i <= 16; ++i)
    {
        const float t = std::sin(static_cast<float>(i) / 16.0f * 1.5707963f);
        knot.push_back(formation.at + glm::vec3(0.0f, -thick * 1.2f + thick * 2.5f * t, 0.0f));
        knotRadii.push_back(thick * 2.2f * std::sqrt(std::max(0.0f, 1.0f - t * t)) + 0.02f);
    }
    const size_t knotFirst = mesh.vertices.size();
    // (Round and many-sided, lumped little: lobed and lumpy, its facets caught the light where the branches met.)
    Landmark::Sweep(mesh, knot, knotRadii, 28, glm::vec3(1.0f, 0.0f, 0.0f), 0.04f, 0.0f, formation.seed + 7u, true, true, 0.1f);
    // (Swelling with the rest from where they all start, though less: it is a good deal thicker.)
    for (size_t v = knotFirst; v < mesh.vertices.size(); ++v)
    {
        mesh.vertices[v].uv = {0.0f, thick * 0.6f};
    }
    Landmark::SmoothNormals(mesh);
    for (MeshVertex& vertex : mesh.vertices)
    {
        vertex.color = 0xffffffffu;
    }
    return mesh;
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
