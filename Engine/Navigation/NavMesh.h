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
    // Floor with at least this much room above it, but less than `agentHeight`, is a crawlspace: in the
    // mesh, and marked, so only a body small enough to crawl is routed through it. Nought leaves it out.
    float crawlHeight = 1.0f;
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
    // Jumps across a ledge, by how high: what a body can make is a question for the body. A route is
    // only ever found through the ones a caller says it can take.
    static constexpr uint16_t kJumpLow = 0x02;  // up or down a metre or so
    static constexpr uint16_t kJumpMid = 0x04;  // up to nearly two metres
    static constexpr uint16_t kJumpHigh = 0x08; // up to two and three quarters: climbing, really
    static constexpr uint16_t kDrop = 0x10;     // down from somewhere too high to get back up
    static constexpr uint16_t kAllJumps = kJumpLow | kJumpMid | kJumpHigh | kDrop;
    // Floor too low to stand on, only to crawl along: a crawlspace, a vent, under something. Allowed the
    // same way as a jump, by a body that fits.
    static constexpr uint16_t kCrawl = 0x20;

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
    // `allowed` adds kCrawl for a body that fits one; anything else in it is ignored.
    bool NearestPoint(const glm::vec3& near, float reach, glm::vec3& out, uint16_t allowed = 0) const;

    // A route from `from` to `to`, as the corners to walk between, starting at the mesh point
    // nearest `from`. When `to` cannot be reached the route ends at the nearest place that can be,
    // and `reached` is false -- a creature that cannot get somewhere should still go as far as it
    // can rather than stand still.
    //
    // `jumps`, when given, is filled with one entry per corner: true where that corner is the take-off of
    // a jump that lands on the next one. `allowed` is which jumps the route may use.
    bool FindPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners,
                  bool* reached = nullptr, std::vector<uint8_t>* jumps = nullptr, uint16_t allowed = kAllJumps) const;

    // How many jumps were found across ledges when it was built, for the log and the tests.
    size_t JumpCount() const;

    // Takes the other mesh's data, and gives it this one's. For rebuilding on a worker thread and
    // putting the result in place between ticks, so nothing holding a pointer to this mesh has to know.
    void Swap(NavMesh& other);

    // Whether walking in a straight line from `from` to `to` stays on the mesh the whole way. The
    // cheap question a creature asks before bothering with a whole route.
    bool StraightWalk(const glm::vec3& from, const glm::vec3& to) const;

    // Where a body at `from` ends up trying to walk to `to` this tick: slid along the walls rather
    // than through them, and at the height of the floor where it arrives, so stairs and ramps are
    // walked up rather than through. False when `from` is nowhere near the mesh.
    bool MoveAlongSurface(const glm::vec3& from, const glm::vec3& to, glm::vec3& out, uint16_t allowed = 0) const;

    // A random point on the mesh within `radius` of `centre` that can be walked to from it. For
    // wandering. `seed` is advanced, so the same seed gives the same wander.
    bool RandomPointNear(const glm::vec3& centre, float radius, uint32_t& seed, glm::vec3& out,
                         uint16_t allowed = 0) const;

    // Whether a point is on crawlspace floor: somewhere a person has to be on their belly, and only a
    // small body can follow.
    bool InCrawlspace(const glm::vec3& point) const;
    // Where the crawlspaces open onto floor that can be stood on: the middle of each opening, just
    // outside it. What anything too big to follow somebody in waits beside.
    const std::vector<glm::vec3>& CrawlMouths() const;

    // The mesh as lines, for the navigation debug view.
    void Draw(DebugDraw& draw) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    NavSettings m_settings;
};

} // namespace pred
