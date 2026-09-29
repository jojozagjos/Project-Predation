#include "Game/World/HiveMesh.h"

#include <glm/vector_relational.hpp>

#include "Engine/Core/Log.h"
#include "Engine/Core/ParallelFor.h"
#include "Engine/Render/MeshSimplify.h"
#include "Engine/Render/Primitives.h"
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
                const float r = 0.11f + 0.07f * Hash(index, 30 + k, 0, kSeed);
                Part egg;
                egg.kind = Kind::Egg;
                egg.a = egg.b = pad.at + (sheet.across * std::cos(angle) + sheet.along * std::sin(angle)) * out + pad.normal * (r * 0.6f);
                egg.normal = pad.normal;
                egg.radii = {r, r * 1.25f, r * 0.9f};
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
                    float eggs = 1.0e9f;
                    for (const Part* part : here)
                    {
                        if (p.x < part->lo.x || p.y < part->lo.y || p.z < part->lo.z || p.x > part->hi.x || p.y > part->hi.y ||
                            p.z > part->hi.z)
                        {
                            continue;
                        }
                        if (part->kind == Kind::Strand)
                        {
                            continue; // made whole and smooth on its own (Smooth eggs and strands, below)
                        }
                        if (part->kind == Kind::Egg)
                        {
                            // Grown out of the flesh: the egg itself is made smooth on its own (below), and here, a little
                            // inside it, the shape the flesh swells up round to grip its base.
                            const float egg = Ellipsoid(p - part->a, part->radii * 0.8f);
                            eggs = std::min(eggs, egg);
                            fleshy = fleshy > 1.0e8f ? egg : SmoothUnion(fleshy, egg, 0.07f);
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
                        // Not on an egg: it is smooth and tight, and roughened like the flesh round it its edge broke up.
                        const float rough = std::clamp(eggs / 0.1f, 0.0f, 1.0f);
                        d -= (Fbm(p * 1.6f, kSeed + 17u, 2) - 0.45f) * 0.07f * rough;
                        d += (Fbm(p * 3.5f, kSeed, 3) - 0.5f) * 0.012f * rough;
                        const float vein = std::abs(Fbm(p * 2.3f, kSeed + 29u, 3) - 0.5f);
                        const float ridge = std::max(0.0f, 1.0f - vein / 0.07f);
                        d -= 0.016f * ridge * ridge * (3.0f - 2.0f * ridge) * rough;
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
                        if (part->kind == Kind::Strand || part->kind == Kind::Egg)
                        {
                            continue; // the flesh round an egg is flesh
                        }
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
                                // Broad and soft: the surface is thinned to triangles bigger than a fine line, and a line
                                // painted at their corners smeared into dark triangles. The veins' shape is in the flesh.
                                colour = glm::mix(glm::vec3(0.1f, 0.02f, 0.05f), colour, 0.35f + 0.65f * std::clamp(veinLine / 0.12f, 0.0f, 1.0f));
                                colour = glm::mix(glm::vec3(0.3f, 0.05f, 0.05f), colour, std::clamp(vein * 4.0f, 0.0f, 1.0f));
                                wet = 0.2f + 0.25f * vein;
                                break;
                            }
                            case Kind::Root:
                                colour = glm::vec3(0.12f, 0.03f, 0.05f);
                                wet = 0.2f;
                                break;
                            case Kind::Egg:
                            {
                                // The flesh's red where it has grown round the base, pale and waxy above, veined.
                                const float up = glm::dot(p - part->a, part->normal) / std::max(part->radii.y, 1e-3f);
                                colour = glm::mix(glm::vec3(0.5f, 0.45f, 0.29f), glm::vec3(0.34f, 0.24f, 0.13f), std::clamp(1.0f - vein * 6.0f, 0.0f, 1.0f) * 0.5f);
                                colour = glm::mix(glm::vec3(0.26f, 0.07f, 0.06f), colour, glm::smoothstep(-0.85f, -0.35f, up));
                                wet = 0.35f;
                                break;
                            }
                            case Kind::Strand:
                                colour = glm::vec3(0.25f, 0.04f, 0.04f);
                                wet = 0.15f;
                                break;
                            }
                        }
                        // Held still when the nest beats (a height below nothing), all but what lies on a floor: flesh
                        // hanging on a wall or from a ceiling throbbing looked wrong.
                        const bool still = part != nullptr && part->kind == Kind::Membrane && part->normal.y < 0.6f;
                        vertex.uv = {fromHeart, still ? -std::max(height, 0.001f) : height};
                        vertex.color = PackColour(colour, wet);
                    }
                }, 256);
                std::vector<uint32_t> kept;
                // Thinned out, but not so far that the bumps are left as long slivers with creases between them.
                MeshData simple = SimplifyMesh(surface, std::max<size_t>(surface.indices.size() / 3 / 4, 300), 0.008f, kept);
                if (!simple.indices.empty())
                {
                    // Shaded smooth, from the surface as it now is: each corner's normal the average of the triangles
                    // round it, weighted by their size. The field's own normals follow every bump in it, and on the
                    // thinned surface -- bigger triangles over the same bumps -- that shaded it in facets and shards.
                    // Near the edge of the piece the field's are kept, where the next piece has to shade the same.
                    std::vector<glm::vec3> summed(simple.vertices.size(), glm::vec3(0.0f));
                    for (size_t t = 0; t + 2 < simple.indices.size(); t += 3)
                    {
                        const uint32_t a = simple.indices[t];
                        const uint32_t b = simple.indices[t + 1];
                        const uint32_t c = simple.indices[t + 2];
                        const glm::vec3 face = glm::cross(simple.vertices[b].position - simple.vertices[a].position,
                                                          simple.vertices[c].position - simple.vertices[a].position);
                        summed[a] += face;
                        summed[b] += face;
                        summed[c] += face;
                    }
                    const glm::vec3 innerLo = sampleLo + glm::vec3(kCell * 2.5f);
                    const glm::vec3 innerHi = sampleHi - glm::vec3(kCell * 2.5f);
                    for (size_t v = 0; v < simple.vertices.size(); ++v)
                    {
                        MeshVertex& vertex = simple.vertices[v];
                        const float length = glm::length(summed[v]);
                        if (length < 1e-10f)
                        {
                            continue;
                        }
                        glm::vec3 smooth = summed[v] / length;
                        if (glm::dot(smooth, vertex.normal) < 0.0f)
                        {
                            smooth = -smooth; // wound the other way round: the field says which way is out
                        }
                        const glm::vec3 inside = glm::min(vertex.position - innerLo, innerHi - vertex.position);
                        const float edge = std::clamp(std::min({inside.x, inside.y, inside.z}) / kCell, 0.0f, 1.0f);
                        vertex.normal = glm::normalize(glm::mix(vertex.normal, smooth, 0.85f * edge));
                    }
                    chunks.push_back(std::move(simple));
                }
            }
        }
    }
    // Smooth eggs and strands. Smaller than the grid the flesh is sampled on can hold -- an egg a hand across is two or three
    // cells -- and sampled they came out as spiky lumps and broken threads. Made as shapes of their own they are whole; the
    // eggs and strands are held still when the nest beats (a height given as less than nothing).
    {
        MeshData extras;
        const auto frameOf = [](const glm::vec3& normal, glm::vec3& across, glm::vec3& along)
        {
            const glm::vec3 helper = std::abs(normal.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            across = glm::normalize(glm::cross(helper, normal));
            along = glm::cross(normal, across);
        };
        const MeshData ball = Primitives::Sphere(1.0f, 18, 12);
        int eggCount = 0;
        for (const Part& part : parts)
        {
            if (part.kind == Kind::Egg)
            {
                // Smooth and whole, sitting in the lip of flesh grown up round it.
                ++eggCount;
                glm::vec3 across;
                glm::vec3 unused;
                frameOf(part.normal, across, unused);
                // Right-handed, (across, up, sideways): the other way round mirrors the ball and turns it inside out.
                const glm::vec3 sideways = glm::cross(across, part.normal);
                const uint32_t base = static_cast<uint32_t>(extras.vertices.size());
                const float shade = Hash(eggCount, 71, 0, kSeed);
                for (const MeshVertex& unit : ball.vertices)
                {
                    MeshVertex vertex = unit;
                    const glm::vec3 r = part.radii;
                    vertex.position = part.a + across * (unit.position.x * r.x) + part.normal * (unit.position.y * r.y) + sideways * (unit.position.z * r.z);
                    vertex.normal = glm::normalize(across * (unit.normal.x / r.x) + part.normal * (unit.normal.y / r.y) + sideways * (unit.normal.z / r.z));
                    // A sac, not a shell: a thin wet skin the light comes through -- amber where it is thinnest, round
                    // its sides -- with the dark curled shape of what is inside showing through the middle, dark veins
                    // over it, and the flesh's red where it is grown into it. Nothing is drawn see-through here, so it
                    // is painted as if it were.
                    const float up = unit.position.y * 0.5f + 0.5f;
                    const float veins = std::clamp(std::abs(Fbm(vertex.position * 12.0f, kSeed + 91u, 2) - 0.5f) * 7.0f, 0.0f, 1.0f);
                    const float radial = glm::length(glm::vec2(unit.position.x, unit.position.z));
                    const float curl = Fbm(unit.position * 2.2f + glm::vec3(shade * 9.0f), kSeed + 97u, 2) - 0.5f;
                    const float inside = glm::smoothstep(0.8f, 0.45f, radial + curl * 0.6f) * glm::smoothstep(-0.4f, 0.2f, unit.position.y);
                    glm::vec3 colour = glm::mix(glm::vec3(0.46f, 0.2f, 0.07f), glm::vec3(0.36f, 0.1f, 0.06f), shade * 0.6f);
                    colour = glm::mix(colour, glm::vec3(0.06f, 0.015f, 0.02f), inside);
                    colour = glm::mix(glm::vec3(0.16f, 0.03f, 0.04f), colour, 0.45f + 0.55f * veins);
                    colour = glm::mix(glm::vec3(0.26f, 0.07f, 0.06f), colour, glm::smoothstep(0.05f, 0.4f, up));
                    vertex.color = PackColour(colour, 0.95f);
                    vertex.uv = {part.fromHeartA, -std::max((unit.position.y + 1.0f) * r.y, 0.001f)}; // held still when the nest beats
                    extras.vertices.push_back(vertex);
                }
                for (const uint32_t corner : ball.indices)
                {
                    extras.indices.push_back(base + corner);
                }
            }
            else if (part.kind == Kind::Strand)
            {
                // A tapering thread, and a drop gathered at its end.
                constexpr int kSides = 8;
                constexpr int kRings = 7;
                glm::vec3 across;
                glm::vec3 around;
                const glm::vec3 down = glm::normalize(part.b - part.a);
                frameOf(down, across, around);
                const float length = glm::distance(part.a, part.b);
                const uint32_t base = static_cast<uint32_t>(extras.vertices.size());
                for (int ring = 0; ring <= kRings; ++ring)
                {
                    const float t = static_cast<float>(ring) / kRings;
                    const float radius = glm::mix(part.r0, part.r1, t);
                    const glm::vec3 centre = glm::mix(part.a, part.b, t);
                    for (int side = 0; side < kSides; ++side)
                    {
                        const float angle = static_cast<float>(side) / kSides * 6.2831853f;
                        const glm::vec3 out = across * std::cos(angle) + around * std::sin(angle);
                        MeshVertex vertex;
                        vertex.position = centre + out * radius;
                        vertex.normal = out;
                        vertex.color = PackColour(glm::vec3(0.25f, 0.04f, 0.04f), 0.5f);
                        vertex.uv = {glm::mix(part.fromHeartA, part.fromHeartB, t), -std::max(t * length, 0.001f)};
                        extras.vertices.push_back(vertex);
                    }
                }
                for (int ring = 0; ring < kRings; ++ring)
                {
                    for (int side = 0; side < kSides; ++side)
                    {
                        const uint32_t a = base + static_cast<uint32_t>(ring * kSides + side);
                        const uint32_t b = base + static_cast<uint32_t>(ring * kSides + (side + 1) % kSides);
                        const uint32_t c = a + kSides;
                        const uint32_t d = b + kSides;
                        extras.indices.insert(extras.indices.end(), {a, b, c, b, d, c}); // facing out
                    }
                }
                const uint32_t dropBase = static_cast<uint32_t>(extras.vertices.size());
                const float drop = part.r0 * 1.6f;
                for (const MeshVertex& unit : ball.vertices)
                {
                    MeshVertex vertex = unit;
                    vertex.position = part.b + unit.position * drop * glm::vec3(1.0f, 1.3f, 1.0f);
                    vertex.color = PackColour(glm::vec3(0.3f, 0.05f, 0.05f), 0.85f);
                    vertex.uv = {part.fromHeartB, -length};
                    extras.vertices.push_back(vertex);
                }
                for (const uint32_t corner : ball.indices)
                {
                    extras.indices.push_back(dropBase + corner);
                }
            }
        }
        if (!extras.indices.empty())
        {
            chunks.push_back(std::move(extras));
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
