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
#include <vector>

namespace pred
{

// The hive: a mound of something grown rather than built, heaped round a gaping hollow, with egg sacs
// half sunk in its sides and cords rising from it into the dark of the roof. Sculpted the way a
// creature is, as blended shapes, and drawn as one surface.
MeshData BuildHiveMesh(uint32_t seed, float radius, float ceiling)
{
    using namespace Sdf;
    const uint32_t kSeed = seed * 2654435761u + 17u;
    const float kHiveRadius = radius;
    const float kWallHeight = ceiling;
    struct Cord
    {
        glm::vec3 a, b, c;
        float r0, r1;
    };
    std::vector<glm::vec3> lumps;
    std::vector<float> lumpRadius;
    std::vector<glm::vec3> sacs;
    std::vector<Cord> cords;
    for (int i = 0; i < 14; ++i)
    {
        const float angle = static_cast<float>(i) * 2.39996f;
        const float r = 0.9f + 0.7f * Hash(i, 1, 0, kSeed);
        lumps.push_back({std::cos(angle) * r, 0.35f + 0.9f * Hash(i, 2, 0, kSeed) * (1.6f - r * 0.5f), std::sin(angle) * r});
        lumpRadius.push_back(0.35f + 0.35f * Hash(i, 3, 0, kSeed));
    }
    for (int i = 0; i < 7; ++i)
    {
        const float angle = static_cast<float>(i) * 0.897f + 0.3f;
        sacs.push_back({std::cos(angle) * 1.55f, 0.35f + 0.25f * Hash(i, 4, 0, kSeed), std::sin(angle) * 1.55f});
    }
    for (int i = 0; i < 8; ++i)
    {
        const float angle = static_cast<float>(i) * 0.785f + 0.2f;
        const float out = 0.6f + 1.8f * Hash(i, 5, 0, kSeed);
        const glm::vec3 foot{std::cos(angle) * 0.7f, 1.2f, std::sin(angle) * 0.7f};
        const glm::vec3 top{std::cos(angle) * out, kWallHeight + 0.2f, std::sin(angle) * out};
        const glm::vec3 bend = (foot + top) * 0.5f + glm::vec3(std::cos(angle + 1.3f), 0.0f, std::sin(angle + 1.3f)) * 0.5f;
        cords.push_back({foot, bend, top, 0.2f + 0.08f * Hash(i, 6, 0, kSeed), 0.07f});
    }

    // Which part of it a point is nearest: the mound, a sac, a cord, or the hollow.
    struct Parts
    {
        float mound;
        float sacs;
        float cords;
        float hollow;
    };
    const auto parts = [&](const glm::vec3& p)
    {
        Parts out{};
        out.mound = Ellipsoid(p - glm::vec3(0.0f, 0.45f, 0.0f), {kHiveRadius, 1.25f, kHiveRadius});
        for (size_t i = 0; i < lumps.size(); ++i)
        {
            out.mound = SmoothUnion(out.mound, glm::length(p - lumps[i]) - lumpRadius[i], 0.35f);
        }
        out.sacs = 1.0e9f;
        for (const glm::vec3& sac : sacs)
        {
            out.sacs = std::min(out.sacs, Ellipsoid(p - sac, {0.32f, 0.42f, 0.32f}));
        }
        out.cords = 1.0e9f;
        for (const Cord& cord : cords)
        {
            out.cords = std::min(out.cords, RoundCone(p, cord.a, cord.b, cord.r0, (cord.r0 + cord.r1) * 0.5f));
            out.cords = std::min(out.cords, RoundCone(p, cord.b, cord.c, (cord.r0 + cord.r1) * 0.5f, cord.r1));
        }
        out.hollow = glm::length(p - glm::vec3(0.0f, 1.65f, 0.0f)) - 0.65f;
        return out;
    };
    const auto distance = [&](const glm::vec3& p)
    {
        const Parts part = parts(p);
        float d = SmoothUnion(part.mound, part.sacs, 0.12f);
        d = SmoothUnion(d, part.cords, 0.25f);
        d = SmoothCut(d, part.hollow, 0.2f);
        // Lumpy, and the floor it grows from is the floor.
        d += (Fbm(p * 3.2f, kSeed, 3) - 0.5f) * 0.12f;
        return std::max(d, -p.y - 0.01f);
    };

    const auto started = std::chrono::steady_clock::now();
    const float reach = kHiveRadius + 2.6f;
    const DistanceGrid grid =
        SampleDistance({-reach, -0.05f, -reach}, {reach, kWallHeight + 0.35f, reach}, 0.06f, distance);
    MeshData surface = SurfaceNets(grid);
    ParallelFor(surface.vertices.size(), [&](size_t first, size_t last) {
        for (size_t v = first; v < last; ++v)
        {
            MeshVertex& vertex = surface.vertices[v];
            glm::vec3 p = vertex.position;
            const auto gradient = [&](const glm::vec3& at)
            {
                const float e = 0.02f;
                const glm::vec3 g{distance(at + glm::vec3(e, 0, 0)) - distance(at - glm::vec3(e, 0, 0)),
                                  distance(at + glm::vec3(0, e, 0)) - distance(at - glm::vec3(0, e, 0)),
                                  distance(at + glm::vec3(0, 0, e)) - distance(at - glm::vec3(0, 0, e))};
                const float length = glm::length(g);
                return length > 1e-8f ? g / length : glm::vec3(0.0f, 1.0f, 0.0f);
            };
            glm::vec3 normal = gradient(p);
            p -= normal * distance(p);
            normal = gradient(p);
            vertex.position = p;
            vertex.normal = normal;

            // Dark, wet flesh; pale, glossy sacs; black in the hollow; grey-brown cords drying out.
            const Parts part = parts(p);
            glm::vec3 colour{0.2f, 0.06f, 0.055f};
            float wet = 0.45f;
            const float nearest = std::min({part.mound, part.sacs, part.cords});
            if (part.sacs <= nearest + 0.02f)
            {
                colour = glm::vec3(0.52f, 0.46f, 0.3f);
                wet = 0.25f;
            }
            else if (part.cords <= nearest + 0.02f)
            {
                colour = glm::vec3(0.19f, 0.13f, 0.11f);
                wet = 0.6f;
            }
            if (part.hollow < 0.12f)
            {
                colour = glm::vec3(0.025f, 0.01f, 0.01f);
            }
            const float vein = 1.0f - std::abs(Fbm(p * 5.0f, kSeed + 9u, 3) * 2.0f - 1.0f);
            colour = glm::mix(colour, glm::vec3(0.38f, 0.08f, 0.07f), std::pow(vein, 10.0f) * 0.7f);
            colour *= 0.75f + 0.4f * Fbm(p * 2.0f, kSeed + 3u, 2);
            const auto channel = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
            vertex.color = channel(colour.r) | (channel(colour.g) << 8) | (channel(colour.b) << 16) | (channel(wet) << 24);
        }
    }, 256);
    std::vector<uint32_t> kept;
    MeshData hive = SimplifyMesh(surface, 16000, 0.004f, kept);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(AI, "Hive built: {} triangles in {:.0f} ms", hive.TriangleCount(), ms);
    return hive;
}

} // namespace pred
