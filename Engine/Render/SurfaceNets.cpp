#include "Engine/Render/SurfaceNets.h"

#include "Engine/Core/ParallelFor.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pred
{

DistanceGrid SampleDistance(const glm::vec3& min, const glm::vec3& max, float cell,
                            const std::function<float(const glm::vec3&)>& distance)
{
    DistanceGrid grid;
    grid.cell = std::max(cell, 1e-4f);
    grid.origin = min;
    const glm::vec3 size = glm::max(max - min, glm::vec3(grid.cell));
    // Rounded up to a whole number of coarse cells, so every fine corner has a coarse cell round it.
    constexpr int kCoarse = 4;
    const auto count = [&](float extent)
    {
        const int cells = static_cast<int>(std::ceil(extent / grid.cell));
        return ((cells + kCoarse - 1) / kCoarse) * kCoarse + 1;
    };
    grid.nx = count(size.x);
    grid.ny = count(size.y);
    grid.nz = count(size.z);
    grid.values.assign(static_cast<size_t>(grid.nx) * static_cast<size_t>(grid.ny) * static_cast<size_t>(grid.nz), 0.0f);

    // The coarse pass.
    const int cx = (grid.nx - 1) / kCoarse + 1;
    const int cy = (grid.ny - 1) / kCoarse + 1;
    const int cz = (grid.nz - 1) / kCoarse + 1;
    const float coarseCell = grid.cell * static_cast<float>(kCoarse);
    std::vector<float> coarse(static_cast<size_t>(cx) * static_cast<size_t>(cy) * static_cast<size_t>(cz));
    const auto coarseAt = [&](int x, int y, int z) -> float&
    { return coarse[static_cast<size_t>(x) + static_cast<size_t>(cx) * (static_cast<size_t>(y) + static_cast<size_t>(cy) * static_cast<size_t>(z))]; };
    ParallelFor(static_cast<size_t>(cz), [&](size_t zFrom, size_t zTo) {
    for (int z = static_cast<int>(zFrom); z < static_cast<int>(zTo); ++z)
    {
        for (int y = 0; y < cy; ++y)
        {
            for (int x = 0; x < cx; ++x)
            {
                coarseAt(x, y, z) = distance(grid.origin + glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) * coarseCell);
            }
        }
    }
    }, 1);

    // The fine pass: exactly near the surface, blended from the coarse samples elsewhere. "Near" is
    // generous -- a coarse cell and a half -- because a blend of distances can put the surface a
    // little way from where the true one is, and a missed crossing is a hole.
    const float band = coarseCell * 1.5f;
    ParallelFor(static_cast<size_t>(grid.nz), [&](size_t zFrom, size_t zTo) {
    for (int z = static_cast<int>(zFrom); z < static_cast<int>(zTo); ++z)
    {
        const int z0 = std::min(z / kCoarse, cz - 2);
        const float tz = static_cast<float>(z - z0 * kCoarse) / static_cast<float>(kCoarse);
        for (int y = 0; y < grid.ny; ++y)
        {
            const int y0 = std::min(y / kCoarse, cy - 2);
            const float ty = static_cast<float>(y - y0 * kCoarse) / static_cast<float>(kCoarse);
            for (int x = 0; x < grid.nx; ++x)
            {
                const int x0 = std::min(x / kCoarse, cx - 2);
                const float tx = static_cast<float>(x - x0 * kCoarse) / static_cast<float>(kCoarse);
                const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
                const float c00 = lerp(coarseAt(x0, y0, z0), coarseAt(x0 + 1, y0, z0), tx);
                const float c10 = lerp(coarseAt(x0, y0 + 1, z0), coarseAt(x0 + 1, y0 + 1, z0), tx);
                const float c01 = lerp(coarseAt(x0, y0, z0 + 1), coarseAt(x0 + 1, y0, z0 + 1), tx);
                const float c11 = lerp(coarseAt(x0, y0 + 1, z0 + 1), coarseAt(x0 + 1, y0 + 1, z0 + 1), tx);
                const float blended = lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
                grid.values[grid.Index(x, y, z)] =
                    std::abs(blended) < band ? distance(grid.Corner(x, y, z)) : blended;
            }
        }
    }
    }, 1);
    return grid;
}

MeshData SurfaceNets(const DistanceGrid& grid)
{
    MeshData mesh;
    if (grid.nx < 2 || grid.ny < 2 || grid.nz < 2)
    {
        return mesh;
    }
    const int cellsX = grid.nx - 1;
    const int cellsY = grid.ny - 1;
    const int cellsZ = grid.nz - 1;
    const auto cellIndex = [&](int x, int y, int z)
    { return static_cast<size_t>(x) + static_cast<size_t>(cellsX) * (static_cast<size_t>(y) + static_cast<size_t>(cellsY) * static_cast<size_t>(z)); };
    std::vector<int32_t> vertexOf(static_cast<size_t>(cellsX) * static_cast<size_t>(cellsY) * static_cast<size_t>(cellsZ), -1);

    // The twelve edges of a cell, as pairs of its eight corners (corner bit 0 is +x, 1 is +y, 2 is +z).
    static constexpr std::array<std::array<int, 2>, 12> kEdges = {{{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                                                   {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                                                   {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
    for (int z = 0; z < cellsZ; ++z)
    {
        for (int y = 0; y < cellsY; ++y)
        {
            for (int x = 0; x < cellsX; ++x)
            {
                std::array<float, 8> value{};
                int inside = 0;
                for (int c = 0; c < 8; ++c)
                {
                    value[static_cast<size_t>(c)] = grid.values[grid.Index(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1))];
                    inside += value[static_cast<size_t>(c)] < 0.0f ? 1 : 0;
                }
                if (inside == 0 || inside == 8)
                {
                    continue;
                }
                glm::vec3 sum{0.0f};
                int crossings = 0;
                for (const std::array<int, 2>& edge : kEdges)
                {
                    const float a = value[static_cast<size_t>(edge[0])];
                    const float b = value[static_cast<size_t>(edge[1])];
                    if ((a < 0.0f) == (b < 0.0f))
                    {
                        continue;
                    }
                    const float t = std::clamp(a / (a - b), 0.0f, 1.0f);
                    const glm::vec3 pa{static_cast<float>(edge[0] & 1), static_cast<float>((edge[0] >> 1) & 1), static_cast<float>((edge[0] >> 2) & 1)};
                    const glm::vec3 pb{static_cast<float>(edge[1] & 1), static_cast<float>((edge[1] >> 1) & 1), static_cast<float>((edge[1] >> 2) & 1)};
                    sum += pa + (pb - pa) * t;
                    ++crossings;
                }
                const glm::vec3 local = sum / static_cast<float>(std::max(crossings, 1));
                vertexOf[cellIndex(x, y, z)] = static_cast<int32_t>(mesh.vertices.size());
                MeshVertex vertex;
                vertex.position = grid.Corner(x, y, z) + local * grid.cell;
                mesh.vertices.push_back(vertex);
            }
        }
    }

    // A quad across every grid edge the surface crosses, joining the four cells that share that edge,
    // turned to face out of the shape.
    const auto quad = [&](size_t a, size_t b, size_t c, size_t d, const glm::vec3& outward)
    {
        const int32_t va = vertexOf[a];
        const int32_t vb = vertexOf[b];
        const int32_t vc = vertexOf[c];
        const int32_t vd = vertexOf[d];
        if (va < 0 || vb < 0 || vc < 0 || vd < 0)
        {
            return;
        }
        std::array<uint32_t, 4> corners{static_cast<uint32_t>(va), static_cast<uint32_t>(vb), static_cast<uint32_t>(vc),
                                        static_cast<uint32_t>(vd)};
        const glm::vec3& p0 = mesh.vertices[corners[0]].position;
        const glm::vec3& p1 = mesh.vertices[corners[1]].position;
        const glm::vec3& p2 = mesh.vertices[corners[2]].position;
        const glm::vec3& p3 = mesh.vertices[corners[3]].position;
        if (glm::dot(glm::cross(p1 - p0, p2 - p0) + glm::cross(p2 - p0, p3 - p0), outward) < 0.0f)
        {
            std::swap(corners[1], corners[3]);
        }
        // Split along the shorter diagonal, which keeps thin slivers out of curved places.
        const glm::vec3& q0 = mesh.vertices[corners[0]].position;
        const glm::vec3& q1 = mesh.vertices[corners[1]].position;
        const glm::vec3& q2 = mesh.vertices[corners[2]].position;
        const glm::vec3& q3 = mesh.vertices[corners[3]].position;
        if (glm::distance(q0, q2) <= glm::distance(q1, q3))
        {
            mesh.indices.insert(mesh.indices.end(), {corners[0], corners[1], corners[2], corners[0], corners[2], corners[3]});
        }
        else
        {
            mesh.indices.insert(mesh.indices.end(), {corners[0], corners[1], corners[3], corners[1], corners[2], corners[3]});
        }
    };
    for (int z = 0; z < grid.nz; ++z)
    {
        for (int y = 0; y < grid.ny; ++y)
        {
            for (int x = 0; x < grid.nx; ++x)
            {
                const float here = grid.values[grid.Index(x, y, z)];
                // Along +x.
                if (x < cellsX && y > 0 && z > 0 && y < grid.ny - 1 && z < grid.nz - 1)
                {
                    const float next = grid.values[grid.Index(x + 1, y, z)];
                    if ((here < 0.0f) != (next < 0.0f))
                    {
                        quad(cellIndex(x, y - 1, z - 1), cellIndex(x, y, z - 1), cellIndex(x, y, z),
                             cellIndex(x, y - 1, z), glm::vec3(here < 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f));
                    }
                }
                // Along +y.
                if (y + 1 < grid.ny && x > 0 && z > 0 && x < grid.nx - 1 && z < grid.nz - 1 && y < cellsY)
                {
                    const float next = grid.values[grid.Index(x, y + 1, z)];
                    if ((here < 0.0f) != (next < 0.0f))
                    {
                        quad(cellIndex(x - 1, y, z - 1), cellIndex(x, y, z - 1), cellIndex(x, y, z),
                             cellIndex(x - 1, y, z), glm::vec3(0.0f, here < 0.0f ? 1.0f : -1.0f, 0.0f));
                    }
                }
                // Along +z.
                if (z + 1 < grid.nz && x > 0 && y > 0 && x < grid.nx - 1 && y < grid.ny - 1 && z < cellsZ)
                {
                    const float next = grid.values[grid.Index(x, y, z + 1)];
                    if ((here < 0.0f) != (next < 0.0f))
                    {
                        quad(cellIndex(x - 1, y - 1, z), cellIndex(x, y - 1, z), cellIndex(x, y, z),
                             cellIndex(x - 1, y, z), glm::vec3(0.0f, 0.0f, here < 0.0f ? 1.0f : -1.0f));
                    }
                }
            }
        }
    }
    return mesh;
}

} // namespace pred
