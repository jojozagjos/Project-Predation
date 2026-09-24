#include "Game/World/HiveMesh.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/ParallelFor.h"
#include "Engine/Render/MeshSimplify.h"
#include "Engine/Render/Sdf.h"
#include "Engine/Render/SurfaceNets.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <vector>

namespace pred
{

namespace
{

uint32_t PackColour(const glm::vec3& colour, float wet)
{
    const auto channel = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(colour.r) | (channel(colour.g) << 8) | (channel(colour.b) << 16) | (channel(wet) << 24);
}

// A shape given as a distance, turned into a surface: sampled on a grid, meshed, each vertex pulled
// onto the true surface and given the true normal there, painted, and thinned to `triangles`.
MeshData Sculpt(const glm::vec3& min, const glm::vec3& max, float cell,
                const std::function<float(const glm::vec3&)>& distance,
                const std::function<uint32_t(const glm::vec3&)>& paint, size_t triangles)
{
    const DistanceGrid grid = SampleDistance(min, max, cell, distance);
    MeshData surface = SurfaceNets(grid);
    ParallelFor(surface.vertices.size(), [&](size_t first, size_t last) {
        for (size_t v = first; v < last; ++v)
        {
            MeshVertex& vertex = surface.vertices[v];
            const auto gradient = [&](const glm::vec3& at)
            {
                const float e = cell * 0.35f;
                const glm::vec3 g{distance(at + glm::vec3(e, 0, 0)) - distance(at - glm::vec3(e, 0, 0)),
                                  distance(at + glm::vec3(0, e, 0)) - distance(at - glm::vec3(0, e, 0)),
                                  distance(at + glm::vec3(0, 0, e)) - distance(at - glm::vec3(0, 0, e))};
                const float length = glm::length(g);
                return length > 1e-8f ? g / length : glm::vec3(0.0f, 1.0f, 0.0f);
            };
            glm::vec3 p = vertex.position;
            glm::vec3 normal = gradient(p);
            p -= normal * distance(p);
            vertex.position = p;
            vertex.normal = gradient(p);
            vertex.color = paint(p);
        }
    }, 256);
    std::vector<uint32_t> kept;
    return SimplifyMesh(surface, triangles, 0.003f, kept);
}

// How much of a vein a point is on: a thin ridge of noise, 0 almost everywhere and 1 along the lines.
float Vein(const glm::vec3& p, float scale, uint32_t seed)
{
    const float ridge = 1.0f - std::abs(Sdf::Fbm(p * scale, seed, 3) * 2.0f - 1.0f);
    return std::pow(ridge, 9.0f);
}

} // namespace

NestHeartMeshes BuildNestHeart(uint32_t seed)
{
    using namespace Sdf;
    const auto started = std::chrono::steady_clock::now();
    const uint32_t kSeed = seed * 2654435761u + 29u;
    const glm::vec3 centre{0.0f, kNestHeartUp, kNestHeartOut};

    // The heart: a heavy body tapering to a point below, two swollen chambers on top, and the great
    // vessels leaving it and arching back into the wall above.
    struct Vessel
    {
        glm::vec3 a, b, c;
        float r0, r1, r2;
    };
    const std::vector<Vessel> vessels{
        {{-0.05f, 0.34f, 0.30f}, {-0.12f, 0.62f, 0.2f}, {-0.3f, 0.82f, 0.02f}, 0.095f, 0.07f, 0.05f},
        {{0.11f, 0.36f, 0.28f}, {0.29f, 0.58f, 0.14f}, {0.48f, 0.66f, 0.02f}, 0.075f, 0.055f, 0.04f},
        {{0.2f, -0.02f, 0.25f}, {0.42f, -0.1f, 0.12f}, {0.62f, -0.3f, 0.02f}, 0.06f, 0.045f, 0.03f},
    };
    const auto chambers = [&](const glm::vec3& p)
    {
        float d = Ellipsoid(p - centre, {0.3f, 0.38f, 0.27f});
        d = SmoothUnion(d, Ellipsoid(p - glm::vec3(0.06f, -0.3f, 0.3f), {0.16f, 0.2f, 0.15f}), 0.15f);
        return d;
    };
    const auto atria = [&](const glm::vec3& p)
    {
        return std::min(glm::length(p - glm::vec3(-0.13f, 0.3f, 0.27f)) - 0.16f,
                        glm::length(p - glm::vec3(0.15f, 0.27f, 0.24f)) - 0.13f);
    };
    const auto greatVessels = [&](const glm::vec3& p)
    {
        float d = 1.0e9f;
        for (const Vessel& vessel : vessels)
        {
            d = std::min(d, RoundCone(p, vessel.a, vessel.b, vessel.r0, vessel.r1));
            d = std::min(d, RoundCone(p, vessel.b, vessel.c, vessel.r1, vessel.r2));
        }
        return d;
    };
    const auto heartDistance = [&](const glm::vec3& p)
    {
        float d = SmoothUnion(chambers(p), atria(p), 0.09f);
        d = SmoothUnion(d, greatVessels(p), 0.07f);
        // A groove down the front between the two halves, and the knotted surface of something alive.
        const float groove = std::abs(p.x - 0.03f - (p.y - centre.y) * 0.25f);
        d += std::max(0.0f, 0.02f - groove) * 0.8f * (p.z > centre.z ? 1.0f : 0.0f);
        d += (Fbm(p * 9.0f, kSeed, 3) - 0.5f) * 0.03f;
        return std::max(d, -p.z - 0.02f);
    };
    const auto heartPaint = [&](const glm::vec3& p)
    {
        glm::vec3 colour{0.44f, 0.05f, 0.06f};
        float wet = 0.9f;
        if (atria(p) < chambers(p))
        {
            colour = glm::vec3(0.3f, 0.04f, 0.08f);
        }
        if (greatVessels(p) < std::min(chambers(p), atria(p)) - 0.01f)
        {
            colour = glm::vec3(0.36f, 0.12f, 0.12f);
            wet = 0.7f;
        }
        colour = glm::mix(colour, glm::vec3(0.18f, 0.02f, 0.1f), Vein(p, 6.0f, kSeed + 5u) * 0.85f);
        colour *= 0.8f + 0.35f * Fbm(p * 4.0f, kSeed + 7u, 2);
        return PackColour(colour, wet);
    };

    // What roots it: a pad of flesh on the wall behind it, and vessels running out across the wall
    // from under it in every direction, forking as they go.
    struct Root
    {
        glm::vec3 a, b, c;
        float r0, r1, r2;
    };
    std::vector<Root> roots;
    for (int i = 0; i < 10; ++i)
    {
        const float angle = static_cast<float>(i) * 0.6283f + 0.5f * Hash(i, 1, 0, kSeed);
        const glm::vec2 out{std::cos(angle), std::sin(angle)};
        const glm::vec2 side{-out.y, out.x};
        const float reach = 1.4f + 0.8f * Hash(i, 2, 0, kSeed);
        const float bend = (Hash(i, 3, 0, kSeed) - 0.5f) * 0.6f;
        const glm::vec2 mid = out * reach * 0.5f + side * bend;
        const glm::vec2 end = out * reach + side * bend * 1.6f;
        roots.push_back({{out.x * 0.2f, out.y * 0.25f, 0.2f}, {mid.x, mid.y, 0.05f}, {end.x, end.y, 0.015f},
                         0.075f, 0.05f, 0.022f});
        if (Hash(i, 4, 0, kSeed) > 0.4f)
        {
            // A fork, off the middle.
            const float turn = angle + (Hash(i, 5, 0, kSeed) > 0.5f ? 0.6f : -0.6f);
            const glm::vec2 fork = mid + glm::vec2(std::cos(turn), std::sin(turn)) * reach * 0.45f;
            roots.push_back({{mid.x, mid.y, 0.05f}, {(mid.x + fork.x) * 0.5f, (mid.y + fork.y) * 0.5f, 0.03f},
                             {fork.x, fork.y, 0.01f}, 0.045f, 0.03f, 0.016f});
        }
    }
    const auto pad = [&](const glm::vec3& p) { return Ellipsoid(p - glm::vec3(0.0f, -0.05f, 0.0f), {0.75f, 0.95f, 0.14f}); };
    const auto rootVessels = [&](const glm::vec3& p)
    {
        float d = 1.0e9f;
        for (const Root& root : roots)
        {
            d = std::min(d, RoundCone(p, root.a, root.b, root.r0, root.r1));
            d = std::min(d, RoundCone(p, root.b, root.c, root.r1, root.r2));
        }
        return d;
    };
    const auto rootDistance = [&](const glm::vec3& p)
    {
        float d = SmoothUnion(pad(p), rootVessels(p), 0.12f);
        d += (Fbm(p * 6.0f, kSeed + 11u, 3) - 0.5f) * 0.035f;
        return std::max(d, -p.z - 0.02f);
    };
    const auto rootPaint = [&](const glm::vec3& p)
    {
        glm::vec3 colour{0.17f, 0.07f, 0.06f};
        float wet = 0.55f;
        if (rootVessels(p) < pad(p))
        {
            colour = glm::vec3(0.27f, 0.05f, 0.08f);
            wet = 0.75f;
        }
        colour = glm::mix(colour, glm::vec3(0.35f, 0.06f, 0.07f), Vein(p, 5.0f, kSeed + 13u) * 0.7f);
        colour *= 0.75f + 0.4f * Fbm(p * 2.5f, kSeed + 17u, 2);
        return PackColour(colour, wet);
    };

    NestHeartMeshes meshes;
    meshes.heart = Sculpt({-0.5f, -0.6f, -0.05f}, {0.7f, 0.95f, 0.72f}, 0.025f, heartDistance, heartPaint, 9000);
    meshes.roots = Sculpt({-2.4f, -2.4f, -0.05f}, {2.4f, 2.4f, 0.36f}, 0.04f, rootDistance, rootPaint, 9000);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(AI, "Nest heart built: {} + {} triangles in {:.0f} ms", meshes.heart.TriangleCount(),
                  meshes.roots.TriangleCount(), ms);
    return meshes;
}

MeshData BuildNestGrowth(uint32_t seed, int variant)
{
    using namespace Sdf;
    const uint32_t kSeed = (seed * 2654435761u) ^ (static_cast<uint32_t>(variant) * 40503u + 101u);

    // A low spreading mat of flesh, knotted with lumps, reaching out in tendrils at its edges, and on
    // some of them the sacs of whatever it is growing.
    std::vector<glm::vec4> lumps; // centre and radius
    for (int i = 0; i < 8; ++i)
    {
        const float angle = static_cast<float>(i) * 2.39996f + Hash(i, 1, 0, kSeed);
        const float r = 0.6f * std::sqrt(Hash(i, 2, 0, kSeed));
        lumps.emplace_back(std::cos(angle) * r, -0.02f, std::sin(angle) * r, 0.12f + 0.15f * Hash(i, 3, 0, kSeed));
    }
    struct Tendril
    {
        glm::vec3 a, b, c;
    };
    std::vector<Tendril> tendrils;
    for (int i = 0; i < 6; ++i)
    {
        const float angle = static_cast<float>(i) * 1.047f + 0.5f * Hash(i, 4, 0, kSeed);
        const glm::vec3 out{std::cos(angle), 0.0f, std::sin(angle)};
        const glm::vec3 side{-out.z, 0.0f, out.x};
        const float bend = (Hash(i, 5, 0, kSeed) - 0.5f) * 0.5f;
        const float reach = 1.2f + 0.3f * Hash(i, 6, 0, kSeed);
        tendrils.push_back({out * 0.5f + glm::vec3(0.0f, 0.02f, 0.0f), out * (reach * 0.75f) + side * bend + glm::vec3(0.0f, 0.02f, 0.0f),
                            out * reach + side * bend * 1.8f + glm::vec3(0.0f, 0.01f, 0.0f)});
    }
    std::vector<glm::vec3> sacs;
    if (variant % 3 == 0)
    {
        const int count = 2 + static_cast<int>(Hash(0, 7, 0, kSeed) * 2.0f);
        for (int i = 0; i < count; ++i)
        {
            const float angle = static_cast<float>(i) * 2.1f + Hash(i, 8, 0, kSeed) * 1.5f;
            const float r = 0.25f + 0.35f * Hash(i, 9, 0, kSeed);
            sacs.emplace_back(std::cos(angle) * r, 0.12f, std::sin(angle) * r);
        }
    }

    const auto mat = [&](const glm::vec3& p)
    {
        float d = Ellipsoid(p, {1.0f, 0.12f, 0.9f});
        for (const glm::vec4& lump : lumps)
        {
            d = SmoothUnion(d, glm::length(p - glm::vec3(lump)) - lump.w, 0.18f);
        }
        return d;
    };
    const auto strands = [&](const glm::vec3& p)
    {
        float d = 1.0e9f;
        for (const Tendril& tendril : tendrils)
        {
            d = std::min(d, RoundCone(p, tendril.a, tendril.b, 0.06f, 0.035f));
            d = std::min(d, RoundCone(p, tendril.b, tendril.c, 0.035f, 0.014f));
        }
        return d;
    };
    const auto sacDistance = [&](const glm::vec3& p)
    {
        float d = 1.0e9f;
        for (const glm::vec3& sac : sacs)
        {
            d = std::min(d, Ellipsoid(p - sac, {0.13f, 0.17f, 0.13f}));
        }
        return d;
    };
    const auto distance = [&](const glm::vec3& p)
    {
        float d = SmoothUnion(mat(p), strands(p), 0.1f);
        d = SmoothUnion(d, sacDistance(p), 0.06f);
        d += (Fbm(p * 5.5f, kSeed, 3) - 0.5f) * 0.05f;
        return std::max(d, -p.y - 0.02f);
    };
    const auto paint = [&](const glm::vec3& p)
    {
        glm::vec3 colour{0.18f, 0.07f, 0.06f};
        float wet = 0.3f;
        const float sac = sacDistance(p);
        if (sac < mat(p) && sac < strands(p))
        {
            colour = glm::vec3(0.55f, 0.47f, 0.31f);
            wet = 0.35f;
        }
        else if (strands(p) < mat(p))
        {
            colour = glm::vec3(0.12f, 0.05f, 0.05f);
            wet = 0.4f;
        }
        colour = glm::mix(colour, glm::vec3(0.34f, 0.07f, 0.07f), Vein(p, 4.5f, kSeed + 3u) * 0.75f);
        colour *= 0.72f + 0.45f * Fbm(p * 2.2f, kSeed + 9u, 2);
        return PackColour(colour, wet);
    };
    return Sculpt({-1.6f, -0.05f, -1.6f}, {1.6f, 0.5f, 1.6f}, 0.045f, distance, paint, 3500);
}

} // namespace pred
