#include "Game/World/HiveMesh.h"

#include <glm/vector_relational.hpp>

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

std::vector<MeshData> BuildNestSkin(const NestSkinPlan& plan)
{
    using namespace Sdf;
    const auto started = std::chrono::steady_clock::now();
    const uint32_t kSeed = plan.seed * 2246822519u + 71u;
    // Coarse enough to be cheap -- a nest can cover a whole room, floor, walls and ceiling -- and the lumps
    // on it are bigger than a cell anyway.
    constexpr float kCell = 0.07f;
    constexpr float kChunk = 4.0f;

    // Everything the skin is made of, each with a box round it, what it is, and how far it is from the
    // heart; and for measuring how far a point stands off the surface, the plane it lies on.
    enum class Kind : uint8_t
    {
        Membrane,
        Root,
        Egg,
        Strand
    };
    struct Part
    {
        Kind kind = Kind::Membrane;
        glm::vec3 a{0.0f};
        glm::vec3 b{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        glm::vec3 across{1.0f, 0.0f, 0.0f};
        glm::vec3 along{0.0f, 0.0f, 1.0f};
        glm::vec3 radii{0.1f};
        float r0 = 0.0f;
        float r1 = 0.0f;
        float fromHeartA = 0.0f;
        float fromHeartB = 0.0f;
        glm::vec3 lo{0.0f};
        glm::vec3 hi{0.0f};
    };
    std::vector<Part> parts;
    const auto bound = [](Part& part, float reach)
    {
        part.lo = glm::min(part.a, part.b) - glm::vec3(reach);
        part.hi = glm::max(part.a, part.b) + glm::vec3(reach);
    };
    const auto frame = [](const glm::vec3& normal, float spin, glm::vec3& across, glm::vec3& along)
    {
        const glm::vec3 helper = std::abs(normal.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 x = glm::normalize(glm::cross(helper, normal));
        const glm::vec3 z = glm::cross(normal, x);
        across = x * std::cos(spin) + z * std::sin(spin);
        along = glm::cross(normal, across);
    };

    int index = 0;
    for (const NestPad& pad : plan.pads)
    {
        ++index;
        const float edge = std::clamp(pad.fromHeart / 17.0f, 0.0f, 1.0f);
        // The membrane: a thin, wide, uneven sheet of flesh lying on the surface, thicker near the heart.
        Part sheet;
        sheet.kind = Kind::Membrane;
        sheet.a = sheet.b = pad.at;
        sheet.normal = pad.normal;
        frame(pad.normal, pad.spin, sheet.across, sheet.along);
        const float reach = pad.size * 0.75f;
        // Never thinner than about a cell: a sheet thinner than the grid it is sampled on breaks into shards at its edges.
        sheet.radii = {reach * pad.stretch, glm::mix(0.12f, 0.075f, edge), reach / pad.stretch};
        sheet.fromHeartA = sheet.fromHeartB = pad.fromHeart;
        bound(sheet, std::max({sheet.radii.x, sheet.radii.z}) + 0.3f);
        parts.push_back(sheet);

        // Egg sacs, in clutches on the floor near the heart.
        if (pad.normal.y > 0.8f && pad.fromHeart < 5.0f && Hash(index, 11, 0, kSeed) < 0.45f)
        {
            const int count = 2 + static_cast<int>(Hash(index, 12, 0, kSeed) * 4.0f);
            for (int k = 0; k < count; ++k)
            {
                const float angle = Hash(index, 13 + k, 0, kSeed) * 6.2831853f;
                const float out = 0.1f + 0.35f * Hash(index, 20 + k, 0, kSeed);
                const float r = 0.08f + 0.06f * Hash(index, 30 + k, 0, kSeed);
                Part egg;
                egg.kind = Kind::Egg;
                egg.a = egg.b = pad.at + (sheet.across * std::cos(angle) + sheet.along * std::sin(angle)) * out + pad.normal * (r * 0.9f);
                egg.normal = pad.normal;
                egg.radii = {r, r * 1.35f, r};
                egg.fromHeartA = egg.fromHeartB = pad.fromHeart;
                bound(egg, r * 1.4f + 0.1f);
                parts.push_back(egg);
            }
        }
        // Strands hanging from the ceiling, a drop at the end of each.
        if (pad.normal.y < -0.7f && Hash(index, 40, 0, kSeed) < 0.4f)
        {
            const int count = 1 + static_cast<int>(Hash(index, 41, 0, kSeed) * 3.0f);
            for (int k = 0; k < count; ++k)
            {
                const float angle = Hash(index, 42 + k, 0, kSeed) * 6.2831853f;
                const float out = 0.4f * Hash(index, 50 + k, 0, kSeed);
                const float length = 0.25f + 0.9f * Hash(index, 60 + k, 0, kSeed) * (1.0f - 0.6f * edge);
                Part strand;
                strand.kind = Kind::Strand;
                strand.a = pad.at + (sheet.across * std::cos(angle) + sheet.along * std::sin(angle)) * out;
                strand.b = strand.a + glm::vec3(0.0f, -length, 0.0f);
                strand.normal = pad.normal;
                strand.r0 = 0.018f;
                strand.r1 = 0.006f;
                strand.fromHeartA = pad.fromHeart;
                strand.fromHeartB = pad.fromHeart + length;
                bound(strand, 0.08f);
                parts.push_back(strand);
            }
        }
    }
    for (const NestRoot& root : plan.roots)
    {
        Part part;
        part.kind = Kind::Root;
        part.normal = root.normal;
        // Raised a little off the surface, so it stands out of the membrane round it.
        part.a = root.a + root.normal * (root.radiusA * 1.2f);
        part.b = root.b + root.normal * (root.radiusB * 1.2f);
        part.r0 = root.radiusA * 1.4f;
        part.r1 = root.radiusB * 1.4f;
        part.fromHeartA = root.fromHeartA;
        part.fromHeartB = root.fromHeartB;
        bound(part, std::max(root.radiusA, root.radiusB) + 0.1f);
        parts.push_back(part);
    }

    // What one part adds, and how far along the nest it is at a point.
    const auto partDistance = [&](const Part& part, const glm::vec3& p)
    {
        switch (part.kind)
        {
        case Kind::Membrane:
        {
            // Warped across the surface, so no sheet is an oval: an outline with lobes and inlets.
            glm::vec3 local = p - part.a;
            const float warpA = (Fbm(p * 1.3f, kSeed + 3u, 2) - 0.5f) * part.radii.x * 0.9f;
            const float warpB = (Fbm(p * 1.3f + glm::vec3(17.0f), kSeed + 5u, 2) - 0.5f) * part.radii.z * 0.9f;
            const glm::vec3 q{glm::dot(local, part.across) + warpA, glm::dot(local, part.normal), glm::dot(local, part.along) + warpB};
            return Ellipsoid(q, part.radii);
        }
        case Kind::Root:
        case Kind::Strand:
            return RoundCone(p, part.a, part.b, part.r0, part.r1);
        case Kind::Egg:
            return Ellipsoid(p - part.a, part.radii);
        }
        return 1.0e9f;
    };
    const auto along = [](const Part& part, const glm::vec3& p)
    {
        const glm::vec3 span = part.b - part.a;
        const float length2 = glm::dot(span, span);
        return length2 > 1e-8f ? std::clamp(glm::dot(p - part.a, span) / length2, 0.0f, 1.0f) : 0.0f;
    };

    // The chunks: a grid of boxes over everything, each sampled on the one lattice so that where two meet
    // they agree.
    glm::vec3 lo(1.0e9f);
    glm::vec3 hi(-1.0e9f);
    for (const Part& part : parts)
    {
        lo = glm::min(lo, part.lo);
        hi = glm::max(hi, part.hi);
    }
    std::vector<MeshData> chunks;
    if (parts.empty())
    {
        return chunks;
    }
    const glm::ivec3 first = glm::ivec3(glm::floor(lo / kChunk));
    const glm::ivec3 last = glm::ivec3(glm::floor(hi / kChunk));
    for (int cz = first.z; cz <= last.z; ++cz)
    {
        for (int cy = first.y; cy <= last.y; ++cy)
        {
            for (int cx = first.x; cx <= last.x; ++cx)
            {
                const glm::vec3 boxLo = glm::vec3(cx, cy, cz) * kChunk;
                const glm::vec3 boxHi = boxLo + glm::vec3(kChunk);
                std::vector<const Part*> here;
                for (const Part& part : parts)
                {
                    if (part.hi.x >= boxLo.x && part.lo.x <= boxHi.x && part.hi.y >= boxLo.y && part.lo.y <= boxHi.y &&
                        part.hi.z >= boxLo.z && part.lo.z <= boxHi.z)
                    {
                        here.push_back(&part);
                    }
                }
                if (here.empty())
                {
                    continue;
                }
                const auto distance = [&](const glm::vec3& p)
                {
                    float d = 1.0e9f;
                    float fleshy = 1.0e9f;
                    for (const Part* part : here)
                    {
                        if (p.x < part->lo.x || p.y < part->lo.y || p.z < part->lo.z || p.x > part->hi.x || p.y > part->hi.y ||
                            p.z > part->hi.z)
                        {
                            continue;
                        }
                        const float dd = partDistance(*part, p);
                        if (part->kind == Kind::Membrane)
                        {
                            fleshy = fleshy > 1.0e8f ? dd : SmoothUnion(fleshy, dd, 0.14f);
                        }
                        else
                        {
                            d = std::min(d, dd);
                        }
                    }
                    d = std::min(d, fleshy) ;
                    if (fleshy < 1.0e8f && d < 1.0e8f)
                    {
                        d = SmoothUnion(fleshy, d, 0.04f);
                    }
                    // Lumpy and veined: swellings a hand across, softer knots over them, and veins standing up out
                    // of it in a branching web -- each broad enough for the grid to hold, so they come out rounded
                    // rather than as the shards and slivers something finer than a cell breaks into.
                    if (d < 0.14f)
                    {
                        d -= (Fbm(p * 1.6f, kSeed + 17u, 2) - 0.45f) * 0.09f;
                        d += (Fbm(p * 3.5f, kSeed, 3) - 0.5f) * 0.025f;
                        const float vein = std::abs(Fbm(p * 2.3f, kSeed + 29u, 3) - 0.5f);
                        const float ridge = std::max(0.0f, 1.0f - vein / 0.07f);
                        d -= 0.016f * ridge * ridge * (3.0f - 2.0f * ridge);
                    }
                    return d;
                };
                // Which part a point on the surface belongs to: the nearest.
                const auto nearestPart = [&](const glm::vec3& p)
                {
                    const Part* best = nullptr;
                    float bestD = 1.0e9f;
                    for (const Part* part : here)
                    {
                        const float dd = partDistance(*part, p);
                        if (dd < bestD)
                        {
                            bestD = dd;
                            best = part;
                        }
                    }
                    return best;
                };
                // A box aligned to the lattice, a cell past the chunk each way so neighbours overlap.
                const glm::vec3 sampleLo = glm::floor(boxLo / kCell) * kCell - glm::vec3(kCell);
                const glm::vec3 sampleHi = glm::ceil(boxHi / kCell) * kCell + glm::vec3(kCell);
                const DistanceGrid grid = SampleDistance(sampleLo, sampleHi, kCell, distance);
                MeshData surface = SurfaceNets(grid);
                if (surface.vertices.empty())
                {
                    continue;
                }
                // Eased: each vertex twice drawn halfway to the middle of its neighbours, which rounds off what the
                // grid leaves angular. Not near the edge of the box sampled, where the next chunk has to meet it.
                {
                    std::vector<std::vector<uint32_t>> neighbours(surface.vertices.size());
                    for (size_t t = 0; t + 2 < surface.indices.size(); t += 3)
                    {
                        for (int k = 0; k < 3; ++k)
                        {
                            const uint32_t a = surface.indices[t + static_cast<size_t>(k)];
                            const uint32_t b = surface.indices[t + static_cast<size_t>((k + 1) % 3)];
                            neighbours[a].push_back(b);
                            neighbours[b].push_back(a);
                        }
                    }
                    const glm::vec3 keepLo = sampleLo + glm::vec3(kCell * 1.5f);
                    const glm::vec3 keepHi = sampleHi - glm::vec3(kCell * 1.5f);
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        std::vector<glm::vec3> eased(surface.vertices.size());
                        for (size_t v = 0; v < surface.vertices.size(); ++v)
                        {
                            const glm::vec3 p = surface.vertices[v].position;
                            eased[v] = p;
                            if (neighbours[v].empty() || glm::any(glm::lessThan(p, keepLo)) || glm::any(glm::greaterThan(p, keepHi)))
                            {
                                continue;
                            }
                            glm::vec3 middle(0.0f);
                            for (const uint32_t n : neighbours[v])
                            {
                                middle += surface.vertices[n].position;
                            }
                            middle /= static_cast<float>(neighbours[v].size());
                            eased[v] = p + (middle - p) * 0.5f;
                        }
                        for (size_t v = 0; v < surface.vertices.size(); ++v)
                        {
                            surface.vertices[v].position = eased[v];
                        }
                    }
                }
                ParallelFor(surface.vertices.size(), [&](size_t from, size_t to) {
                    for (size_t v = from; v < to; ++v)
                    {
                        MeshVertex& vertex = surface.vertices[v];
                        // Normals over most of a cell, so they follow the lumps and not every wrinkle in them.
                        const float e = kCell * 0.9f;
                        const auto gradient = [&](const glm::vec3& at)
                        {
                            const glm::vec3 g{distance(at + glm::vec3(e, 0, 0)) - distance(at - glm::vec3(e, 0, 0)),
                                              distance(at + glm::vec3(0, e, 0)) - distance(at - glm::vec3(0, e, 0)),
                                              distance(at + glm::vec3(0, 0, e)) - distance(at - glm::vec3(0, 0, e))};
                            const float length = glm::length(g);
                            return length > 1e-8f ? g / length : glm::vec3(0.0f, 1.0f, 0.0f);
                        };
                        // Onto the surface -- but only so far: pulled a long way, neighbours cross over and fold.
                        glm::vec3 p = vertex.position;
                        p -= gradient(p) * std::clamp(distance(p), -kCell * 0.5f, kCell * 0.5f);
                        vertex.position = p;
                        vertex.normal = gradient(p);

                        const Part* part = nearestPart(p);
                        float fromHeart = 0.0f;
                        float height = 0.0f;
                        glm::vec3 colour{0.2f, 0.05f, 0.05f};
                        float wet = 0.3f;
                        if (part != nullptr)
                        {
                            const float t = along(*part, p);
                            fromHeart = glm::mix(part->fromHeartA, part->fromHeartB, t);
                            const glm::vec3 base = glm::mix(part->a, part->b, t);
                            height = part->kind == Kind::Strand ? glm::distance(p, part->a)
                                                                : std::max(glm::dot(p - (part->kind == Kind::Membrane ? part->a : base), part->normal) +
                                                                               (part->kind == Kind::Root ? part->r0 * 0.6f : 0.0f),
                                                                           0.0f);
                            // Flesh: dark, veined, wet in the hollows; roots darker and glossier; eggs pale and
                            // waxy with a darker shape inside; strands a thin raw red.
                            const float vein = std::abs(Fbm(p * 5.5f, kSeed + 9u, 3) - 0.5f);
                            switch (part->kind)
                            {
                            case Kind::Membrane:
                            {
                                // Dark in the hollows, raw where it swells, the veins purple-black.
                                colour = glm::mix(glm::vec3(0.05f, 0.012f, 0.018f), glm::vec3(0.26f, 0.07f, 0.06f), std::clamp(height / 0.12f, 0.0f, 1.0f));
                                const float veinLine = std::abs(Fbm(p * 2.3f, kSeed + 29u, 3) - 0.5f);
                                colour = glm::mix(glm::vec3(0.08f, 0.015f, 0.05f), colour, std::clamp(veinLine / 0.04f, 0.0f, 1.0f));
                                colour = glm::mix(glm::vec3(0.34f, 0.04f, 0.05f), colour, std::clamp(vein * 9.0f, 0.0f, 1.0f));
                                wet = 0.2f + 0.25f * vein;
                                break;
                            }
                            case Kind::Root:
                                colour = glm::vec3(0.12f, 0.03f, 0.05f);
                                wet = 0.2f;
                                break;
                            case Kind::Egg:
                                colour = glm::mix(glm::vec3(0.52f, 0.47f, 0.3f), glm::vec3(0.3f, 0.22f, 0.12f), std::clamp(vein * 5.0f, 0.0f, 1.0f));
                                wet = 0.3f;
                                break;
                            case Kind::Strand:
                                colour = glm::vec3(0.25f, 0.04f, 0.04f);
                                wet = 0.15f;
                                break;
                            }
                        }
                        vertex.uv = {fromHeart, height};
                        vertex.color = PackColour(colour, wet);
                    }
                }, 256);
                std::vector<uint32_t> kept;
                // Thinned out, but not so far that the bumps are left as long slivers with creases between them.
                MeshData simple = SimplifyMesh(surface, std::max<size_t>(surface.indices.size() / 3 / 4, 300), 0.008f, kept);
                if (!simple.indices.empty())
                {
                    chunks.push_back(std::move(simple));
                }
            }
        }
    }
    size_t triangles = 0;
    for (const MeshData& chunk : chunks)
    {
        triangles += chunk.indices.size() / 3;
    }
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(AI, "Nest skin built: {} pieces, {} triangles, from {} parts, in {:.0f} ms", chunks.size(), triangles, parts.size(),
                  seconds * 1000.0f);
    return chunks;
}


} // namespace pred
