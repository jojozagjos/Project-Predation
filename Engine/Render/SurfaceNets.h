#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec3.hpp>

#include <functional>
#include <vector>

namespace pred
{

// A signed distance sampled on a regular grid: negative inside a shape, positive outside, zero on its
// surface. `values` holds one number per grid corner, x fastest, then y, then z.
struct DistanceGrid
{
    glm::vec3 origin{0.0f}; // where corner (0, 0, 0) is
    float cell = 0.02f;     // the spacing between corners
    int nx = 0;
    int ny = 0;
    int nz = 0;
    std::vector<float> values;

    size_t Index(int x, int y, int z) const
    {
        return static_cast<size_t>(x) + static_cast<size_t>(nx) * (static_cast<size_t>(y) + static_cast<size_t>(ny) * static_cast<size_t>(z));
    }
    glm::vec3 Corner(int x, int y, int z) const
    {
        return origin + glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) * cell;
    }
};

// Fills a grid covering `min` to `max` from a distance function, evaluating it exactly only near the
// surface.
//
// Most of a grid is well inside or well outside the shape, where nothing but the sign matters. So the
// function is first sampled on a grid four times coarser, and a fine corner is asked for exactly only
// when the coarse samples around it say the surface might be close; elsewhere the coarse samples are
// blended. For a creature that is about a tenth of the work of asking everywhere.
DistanceGrid SampleDistance(const glm::vec3& min, const glm::vec3& max, float cell,
                            const std::function<float(const glm::vec3&)>& distance);

// The surface through a distance grid, as triangles: one vertex in every cell the surface passes
// through, placed at the average of where it crosses that cell's edges, and a quad across every grid
// edge it crosses. This is "surface nets" -- smoother than marching cubes for the same grid, with a
// quarter of the vertices and no special cases.
//
// Faces point out of the shape, towards positive distance. Normals are left for the caller, which
// usually has something better than the grid to take them from.
MeshData SurfaceNets(const DistanceGrid& grid);

} // namespace pred
