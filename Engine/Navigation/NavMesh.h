#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pred
{

class DebugDraw;

// Where something of a given size can walk, and how to get from one place to another.
//
// A navigation mesh: the floor of the level reduced to a set of convex polygons that a body of a
// particular height and width can stand on, with the walls, the gaps too narrow and the ceilings too
// low already taken out. Finding a route is then a search across polygons rather than across the
// raw world, which is fast enough to do many times a second, and a route found on it cannot walk
// into a wall because the wall is not on it.
//
// Built by Recast and searched by Detour, both from the recastnavigation library. Nothing of that
// library is visible in this header: the rest of the game asks for paths and points in its own
// terms, so the library can be replaced without touching anything that uses it.
struct NavSettings
{
    // The body the mesh is built for. A mesh belongs to one size of creature: a route that fits a
    // person may not fit something twice as wide, and the later phases build one per size class.
    float agentHeight = 1.5f;
    float agentRadius = 0.35f;
    // The tallest step it can walk up without climbing, and the steepest slope it can walk.
    float agentMaxClimb = 0.45f;
    float agentMaxSlopeDegrees = 46.0f;
    // How finely the level is sampled. Smaller is more exact and slower to build.
    float cellSize = 0.12f;
    float cellHeight = 0.08f;
};

class NavMesh
{
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Builds from a triangle soup in world space, three corners per triangle, replacing whatever
    // was built before. False, with the reason in `error`, when nothing walkable came out of it.
    bool Build(const std::vector<glm::vec3>& triangles, const NavSettings& settings,
               std::string* error = nullptr);
    bool Valid() const;
    const NavSettings& Settings() const { return m_settings; }
    size_t PolygonCount() const;

    // The nearest point on the mesh to `near`, looking no further than `reach` sideways and twice
    // that up and down. False when there is nothing that close.
    bool NearestPoint(const glm::vec3& near, float reach, glm::vec3& out) const;

    // A route from `from` to `to`, as the corners to walk between, starting at the mesh point
    // nearest `from`. When `to` cannot be reached the route ends at the nearest place that can be,
    // and `reached` is false -- a creature that cannot get somewhere should still go as far as it
    // can rather than stand still.
    bool FindPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners,
                  bool* reached = nullptr) const;

    // Whether walking in a straight line from `from` to `to` stays on the mesh the whole way. The
    // cheap question a creature asks before bothering with a whole route.
    bool StraightWalk(const glm::vec3& from, const glm::vec3& to) const;

    // Where a body at `from` ends up trying to walk to `to` this tick: slid along the walls rather
    // than through them, and at the height of the floor where it arrives, so stairs and ramps are
    // walked up rather than through. False when `from` is nowhere near the mesh.
    bool MoveAlongSurface(const glm::vec3& from, const glm::vec3& to, glm::vec3& out) const;

    // A random point on the mesh within `radius` of `centre` that can be walked to from it. For
    // wandering. `seed` is advanced, so the same seed gives the same wander.
    bool RandomPointNear(const glm::vec3& centre, float radius, uint32_t& seed, glm::vec3& out) const;

    // The mesh as lines, for the navigation debug view.
    void Draw(DebugDraw& draw) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    NavSettings m_settings;
};

} // namespace pred
