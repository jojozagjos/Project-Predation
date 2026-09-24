#include "Engine/Navigation/NavMesh.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/DebugDraw.h"

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

namespace pred
{
namespace
{

// Every walkable polygon carries this flag, and every query includes it. One flag for now; the later
// phases add others for doors, vents and ledges, which is what flags are for.
constexpr unsigned short kWalkFlag = 0x01;
constexpr unsigned short kCrawlFlag = NavMesh::kCrawl;
// Recast's own area for floor that can be stood on is RC_WALKABLE_AREA; this is the one for floor that
// can only be crawled along.
constexpr unsigned char kCrawlArea = 2;
// Jumps: joined across a ledge by a link rather than by floor, flagged by how high the top is above the
// bottom, so each creature takes only the ones its body can make.
constexpr unsigned short kJumpLowFlag = NavMesh::kJumpLow;
constexpr unsigned short kJumpMidFlag = NavMesh::kJumpMid;
constexpr unsigned short kJumpHighFlag = NavMesh::kJumpHigh;
constexpr unsigned short kDropFlag = NavMesh::kDrop;
// The longest route a query will return, in polygons and in corners. A route across the whole test
// map is a few dozen polygons; this is far past that and still small.
constexpr int kMaxPolys = 512;
constexpr int kMaxCorners = 256;

// Detour's random point takes a plain function pointer, so the random state it draws on cannot be
// captured. It lives here, per thread, and is loaded from the caller's seed for the one call.
thread_local uint32_t t_randomState = 1;
float DetourRandom()
{
    // xorshift, returning [0, 1).
    t_randomState ^= t_randomState << 13;
    t_randomState ^= t_randomState >> 17;
    t_randomState ^= t_randomState << 5;
    return static_cast<float>(t_randomState & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

// The frees Recast asks for, as unique_ptr deleters, so an early return part way through a build
// cannot leak the pieces built so far.
struct HeightfieldDeleter
{
    void operator()(rcHeightfield* p) const { rcFreeHeightField(p); }
};
struct CompactDeleter
{
    void operator()(rcCompactHeightfield* p) const { rcFreeCompactHeightfield(p); }
};
struct ContourDeleter
{
    void operator()(rcContourSet* p) const { rcFreeContourSet(p); }
};
struct PolyMeshDeleter
{
    void operator()(rcPolyMesh* p) const { rcFreePolyMesh(p); }
};
struct DetailDeleter
{
    void operator()(rcPolyMeshDetail* p) const { rcFreePolyMeshDetail(p); }
};


// The level's triangles, bucketed on a grid across the floor plan, for asking whether a short segment
// passes through any of them without trying every triangle in the level.
class TriangleGrid
{
public:
    explicit TriangleGrid(const std::vector<glm::vec3>& triangles) : m_triangles(triangles)
    {
        for (size_t i = 0; i + 2 < triangles.size(); i += 3)
        {
            const glm::vec3 lo = glm::min(glm::min(triangles[i], triangles[i + 1]), triangles[i + 2]);
            const glm::vec3 hi = glm::max(glm::max(triangles[i], triangles[i + 1]), triangles[i + 2]);
            for (int x = Cell(lo.x); x <= Cell(hi.x); ++x)
            {
                for (int z = Cell(lo.z); z <= Cell(hi.z); ++z)
                {
                    m_cells[Key(x, z)].push_back(static_cast<uint32_t>(i));
                }
            }
        }
        m_stamp.assign(triangles.size() / 3 + 1, 0);
    }

    // Whether the segment from a to b passes through any triangle.
    bool Blocked(const glm::vec3& a, const glm::vec3& b)
    {
        ++m_visit;
        const glm::vec3 lo = glm::min(a, b);
        const glm::vec3 hi = glm::max(a, b);
        for (int x = Cell(lo.x); x <= Cell(hi.x); ++x)
        {
            for (int z = Cell(lo.z); z <= Cell(hi.z); ++z)
            {
                const auto found = m_cells.find(Key(x, z));
                if (found == m_cells.end())
                {
                    continue;
                }
                for (const uint32_t i : found->second)
                {
                    uint32_t& stamp = m_stamp[i / 3];
                    if (stamp == m_visit)
                    {
                        continue;
                    }
                    stamp = m_visit;
                    if (Crosses(a, b, m_triangles[i], m_triangles[i + 1], m_triangles[i + 2]))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

private:
    static int Cell(float v) { return static_cast<int>(std::floor(v / 2.0f)); }
    static int64_t Key(int x, int z) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z); }
    // Moller and Trumbore, for a segment rather than a ray.
    static bool Crosses(const glm::vec3& a, const glm::vec3& b, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2)
    {
        const glm::vec3 direction = b - a;
        const glm::vec3 e1 = v1 - v0;
        const glm::vec3 e2 = v2 - v0;
        const glm::vec3 p = glm::cross(direction, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-9f)
        {
            return false;
        }
        const float inverse = 1.0f / det;
        const glm::vec3 s = a - v0;
        const float u = glm::dot(s, p) * inverse;
        if (u < 0.0f || u > 1.0f)
        {
            return false;
        }
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(direction, q) * inverse;
        if (v < 0.0f || u + v > 1.0f)
        {
            return false;
        }
        const float t = glm::dot(e2, q) * inverse;
        return t > 0.0f && t < 1.0f;
    }

    const std::vector<glm::vec3>& m_triangles;
    std::unordered_map<int64_t, std::vector<uint32_t>> m_cells;
    std::vector<uint32_t> m_stamp;
    uint32_t m_visit = 0;
};

struct JumpLinks
{
    std::vector<float> verts;
    std::vector<float> radius;
    std::vector<unsigned char> direction;
    std::vector<unsigned char> areas;
    std::vector<unsigned short> flags;
    std::vector<unsigned int> ids;
    size_t Count() const { return radius.size(); }
};

// Where a body could jump between two floors: from every open edge of the mesh -- the lip of a crate top,
// the edge of a platform -- out over the drop to the floor below it, when there is floor below within
// jumping distance and nothing in the way. Found on the finished mesh, and added to it as links that a
// route may cross but that are not floor.
JumpLinks FindJumps(const dtNavMesh& mesh, const dtNavMeshQuery& query, const std::vector<glm::vec3>& triangles)
{
    JumpLinks links;
    TriangleGrid grid(triangles);
    dtQueryFilter walk;
    walk.setIncludeFlags(kWalkFlag);
    constexpr size_t kMaxLinks = 1024;
    constexpr float kHighest = 4.6f;   // no drop further than this
    constexpr float kLowest = 0.45f;   // anything less is a step, not a jump
    constexpr float kJumpableUp = 2.7f; // higher than this is only ever a drop

    for (int t = 0; t < mesh.getMaxTiles(); ++t)
    {
        const dtMeshTile* tile = mesh.getTile(t);
        if (tile == nullptr || tile->header == nullptr)
        {
            continue;
        }
        for (int p = 0; p < tile->header->polyCount && links.Count() < kMaxLinks; ++p)
        {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() != DT_POLYTYPE_GROUND)
            {
                continue;
            }
            glm::vec3 centre{0.0f};
            for (int v = 0; v < poly.vertCount; ++v)
            {
                const float* at = &tile->verts[poly.verts[v] * 3];
                centre += glm::vec3(at[0], at[1], at[2]);
            }
            centre /= static_cast<float>(std::max<int>(poly.vertCount, 1));
            for (int v = 0; v < poly.vertCount; ++v)
            {
                if (poly.neis[v] != 0)
                {
                    continue; // floor on the other side: not an edge
                }
                const float* pa = &tile->verts[poly.verts[v] * 3];
                const float* pb = &tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
                const glm::vec3 a{pa[0], pa[1], pa[2]};
                const glm::vec3 b{pb[0], pb[1], pb[2]};
                const glm::vec2 edge{b.x - a.x, b.z - a.z};
                const float length = glm::length(edge);
                if (length < 0.3f)
                {
                    continue;
                }
                glm::vec2 out = glm::vec2(edge.y, -edge.x) / length;
                const glm::vec2 fromCentre{0.5f * (a.x + b.x) - centre.x, 0.5f * (a.z + b.z) - centre.z};
                if (glm::dot(out, fromCentre) < 0.0f)
                {
                    out = -out;
                }
                const int samples = std::max(1, static_cast<int>(length / 0.8f));
                for (int s = 0; s < samples && links.Count() < kMaxLinks; ++s)
                {
                    const glm::vec3 top = glm::mix(a, b, (static_cast<float>(s) + 0.5f) / static_cast<float>(samples));
                    for (const float reach : {0.75f, 1.0f, 1.35f})
                    {
                        const glm::vec3 over{top.x + out.x * reach, top.y, top.z + out.y * reach};
                        // Floor below, somewhere between a step and the furthest drop.
                        const float centreY = top.y - (kLowest + kHighest) * 0.5f;
                        const float look[3] = {0.25f, (kHighest - kLowest) * 0.5f, 0.25f};
                        const float centreAt[3] = {over.x, centreY, over.z};
                        dtPolyRef ref = 0;
                        float landing[3];
                        if (dtStatusFailed(query.findNearestPoly(centreAt, look, &walk, &ref, landing)) || ref == 0)
                        {
                            continue;
                        }
                        const glm::vec3 bottom{landing[0], landing[1], landing[2]};
                        const float drop = top.y - bottom.y;
                        if (drop < kLowest || glm::length(glm::vec2(bottom.x - over.x, bottom.z - over.z)) > 0.35f)
                        {
                            continue;
                        }
                        // Clear all the way: out over the lip at head height, then down to the floor.
                        const glm::vec3 above{bottom.x, top.y + 0.5f, bottom.z};
                        if (grid.Blocked(top + glm::vec3(0.0f, 0.5f, 0.0f), above) ||
                            grid.Blocked(above, bottom + glm::vec3(0.0f, 0.3f, 0.0f)))
                        {
                            continue;
                        }
                        // Not one already made from right here to right there.
                        bool known = false;
                        for (size_t k = 0; k < links.Count() && !known; ++k)
                        {
                            const glm::vec3 from{links.verts[k * 6], links.verts[k * 6 + 1], links.verts[k * 6 + 2]};
                            const glm::vec3 to{links.verts[k * 6 + 3], links.verts[k * 6 + 4], links.verts[k * 6 + 5]};
                            known = glm::distance(from, top) < 1.1f && glm::distance(to, bottom) < 1.1f;
                        }
                        if (known)
                        {
                            break;
                        }
                        const glm::vec3 start = top - glm::vec3(out.x, 0.0f, out.y) * 0.05f;
                        links.verts.insert(links.verts.end(), {start.x, start.y, start.z, bottom.x, bottom.y, bottom.z});
                        links.radius.push_back(0.3f);
                        links.direction.push_back(drop <= kJumpableUp ? DT_OFFMESH_CON_BIDIR : 0);
                        links.areas.push_back(RC_WALKABLE_AREA);
                        links.flags.push_back(drop <= 1.1f ? kJumpLowFlag
                                              : drop <= 1.9f ? kJumpMidFlag
                                              : drop <= kJumpableUp ? kJumpHighFlag
                                                                    : kDropFlag);
                        links.ids.push_back(static_cast<unsigned int>(links.Count()));
                        break;
                    }
                }
            }
        }
    }
    return links;
}

// Every edge where crawlspace floor meets floor that can be stood on: the middle of the opening, pushed a
// little out onto the standing side. Openings that are really one wide mouth come out as several edges,
// so edges near one another are merged.
std::vector<glm::vec3> FindCrawlMouths(const dtNavMesh& mesh)
{
    std::vector<glm::vec3> mouths;
    std::vector<int> merged;
    for (int t = 0; t < mesh.getMaxTiles(); ++t)
    {
        const dtMeshTile* tile = mesh.getTile(t);
        if (tile == nullptr || tile->header == nullptr)
        {
            continue;
        }
        for (int p = 0; p < tile->header->polyCount; ++p)
        {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() != DT_POLYTYPE_GROUND || (poly.flags & kWalkFlag) == 0)
            {
                continue;
            }
            for (int v = 0; v < poly.vertCount; ++v)
            {
                const unsigned short across = poly.neis[v];
                if (across == 0 || (across & DT_EXT_LINK) != 0)
                {
                    continue;
                }
                const dtPoly& other = tile->polys[across - 1];
                if ((other.flags & kCrawlFlag) == 0)
                {
                    continue;
                }
                const float* a = &tile->verts[poly.verts[v] * 3];
                const float* b = &tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
                glm::vec3 middle{(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f};
                // Out onto the standing floor, towards the middle of this polygon.
                glm::vec3 centre{0.0f};
                for (int k = 0; k < poly.vertCount; ++k)
                {
                    const float* at = &tile->verts[poly.verts[k] * 3];
                    centre += glm::vec3(at[0], at[1], at[2]);
                }
                centre /= static_cast<float>(poly.vertCount);
                glm::vec3 out{centre.x - middle.x, 0.0f, centre.z - middle.z};
                if (glm::length(out) > 1e-3f)
                {
                    middle += glm::normalize(out) * std::min(0.4f, glm::length(out));
                }
                bool near = false;
                for (size_t k = 0; k < mouths.size() && !near; ++k)
                {
                    if (glm::distance(mouths[k], middle) < 2.5f)
                    {
                        // One opening: kept at the average of its pieces.
                        mouths[k] = (mouths[k] * static_cast<float>(merged[k]) + middle) / static_cast<float>(merged[k] + 1);
                        ++merged[k];
                        near = true;
                    }
                }
                if (!near)
                {
                    mouths.push_back(middle);
                    merged.push_back(1);
                }
            }
        }
    }
    return mouths;
}

} // namespace

struct NavMesh::Impl
{
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtQueryFilter filter;
    size_t polygons = 0;
    size_t jumps = 0;
    std::vector<glm::vec3> mouths;

    ~Impl()
    {
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(mesh);
    }

    // How far to look for the mesh around a point: sideways a little, up and down more, because a
    // point handed in is usually a body's position and that is somewhere above the floor.
    static void Extents(float reach, float out[3])
    {
        out[0] = reach;
        out[1] = reach * 2.0f;
        out[2] = reach;
    }

    bool Nearest(const glm::vec3& point, float reach, dtPolyRef& ref, float out[3], uint16_t allowed = 0) const
    {
        float extents[3];
        Extents(reach, extents);
        ref = 0;
        const dtQueryFilter chosen = With(allowed & kCrawlFlag);
        const dtStatus status = query->findNearestPoly(&point.x, extents, &chosen, &ref, out);
        return dtStatusSucceed(status) && ref != 0;
    }

    // The filter, with more kinds of floor or link allowed on it.
    dtQueryFilter With(uint16_t allowed) const
    {
        dtQueryFilter widened = filter;
        widened.setIncludeFlags(static_cast<unsigned short>(filter.getIncludeFlags() | allowed));
        return widened;
    }
};

NavMesh::NavMesh() = default;
NavMesh::~NavMesh() = default;

bool NavMesh::Valid() const
{
    return m_impl != nullptr && m_impl->mesh != nullptr && m_impl->query != nullptr;
}

size_t NavMesh::PolygonCount() const
{
    return m_impl != nullptr ? m_impl->polygons : 0;
}

bool NavMesh::Build(const std::vector<glm::vec3>& triangles, const NavSettings& settings,
                    std::string* error)
{
    const auto fail = [&](const char* why)
    {
        if (error != nullptr)
        {
            *error = why;
        }
        PRED_LOG_WARN(AI, "Navigation mesh not built: {}", why);
        return false;
    };

    m_impl.reset();
    m_settings = settings;
    if (triangles.size() < 3)
    {
        return fail("no level geometry to build from");
    }
    const auto started = std::chrono::steady_clock::now();

    // Recast wants flat arrays: every corner, and three indices per triangle. The soup is already
    // one corner per entry, so the indices are just counting.
    const int vertexCount = static_cast<int>(triangles.size());
    const int triangleCount = vertexCount / 3;
    const float* vertices = &triangles.front().x;
    std::vector<int> indices(static_cast<size_t>(triangleCount) * 3);
    for (size_t i = 0; i < indices.size(); ++i)
    {
        indices[i] = static_cast<int>(i);
    }

    // The standard single-mesh build, as Recast's own sample does it. Every number here is in
    // cells, so the metres the caller gave are converted once, rounding in whichever direction
    // keeps the creature out of trouble: up for its size, down for what it can climb.
    rcConfig config{};
    config.cs = settings.cellSize;
    config.ch = settings.cellHeight;
    config.walkableSlopeAngle = settings.agentMaxSlopeDegrees;
    // Built for the lowest body that uses it -- one crawling -- with the floor too low to stand on marked
    // below, so that a body standing up is never routed along it.
    const bool crawlspaces = settings.crawlHeight > 0.0f && settings.crawlHeight < settings.agentHeight;
    const float lowest = crawlspaces ? settings.crawlHeight : settings.agentHeight;
    config.walkableHeight = static_cast<int>(std::ceil(lowest / config.ch));
    const int standingCells = static_cast<int>(std::ceil(settings.agentHeight / config.ch));
    config.walkableClimb = static_cast<int>(std::floor(settings.agentMaxClimb / config.ch));
    config.walkableRadius = static_cast<int>(std::ceil(settings.agentRadius / config.cs));
    config.maxEdgeLen = static_cast<int>(12.0f / config.cs);
    config.maxSimplificationError = 1.3f;
    config.minRegionArea = 8 * 8;
    config.mergeRegionArea = 20 * 20;
    config.maxVertsPerPoly = 6;
    config.detailSampleDist = config.cs * 6.0f;
    config.detailSampleMaxError = config.ch * 1.0f;
    rcCalcBounds(vertices, vertexCount, config.bmin, config.bmax);
    rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width, &config.height);

    rcContext context(false);

    std::unique_ptr<rcHeightfield, HeightfieldDeleter> solid(rcAllocHeightfield());
    if (!solid || !rcCreateHeightfield(&context, *solid, config.width, config.height, config.bmin,
                                       config.bmax, config.cs, config.ch))
    {
        return fail("could not allocate the heightfield");
    }

    // Which triangles are floor rather than wall, then the level drawn into voxels.
    std::vector<unsigned char> areas(static_cast<size_t>(triangleCount), 0);
    rcMarkWalkableTriangles(&context, config.walkableSlopeAngle, vertices, vertexCount,
                            indices.data(), triangleCount, areas.data());
    if (!rcRasterizeTriangles(&context, vertices, vertexCount, indices.data(), areas.data(),
                              triangleCount, *solid, config.walkableClimb))
    {
        return fail("could not rasterise the level");
    }

    // Removing what a body cannot use: a kerb it can step onto is kept, the lip of a ledge it would
    // fall from is not, and anywhere the ceiling is lower than it is tall is not floor at all.
    rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, *solid);
    rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, *solid);

    std::unique_ptr<rcCompactHeightfield, CompactDeleter> compact(rcAllocCompactHeightfield());
    if (!compact || !rcBuildCompactHeightfield(&context, config.walkableHeight, config.walkableClimb,
                                               *solid, *compact))
    {
        return fail("could not compact the heightfield");
    }
    solid.reset();

    if (crawlspaces)
    {
        for (int y = 0; y < compact->height; ++y)
        {
            for (int x = 0; x < compact->width; ++x)
            {
                const rcCompactCell& cell = compact->cells[x + y * compact->width];
                for (unsigned i = cell.index, end = cell.index + cell.count; i < end; ++i)
                {
                    if (compact->areas[i] != RC_NULL_AREA && compact->spans[i].h < standingCells)
                    {
                        compact->areas[i] = kCrawlArea;
                    }
                }
            }
        }
    }

    // Shrunk away from the walls by the body's radius, so a route along the mesh is a route the
    // whole body fits along rather than one its centre fits along.
    if (!rcErodeWalkableArea(&context, config.walkableRadius, *compact) ||
        !rcBuildDistanceField(&context, *compact) ||
        !rcBuildRegions(&context, *compact, 0, config.minRegionArea, config.mergeRegionArea))
    {
        return fail("could not divide the floor into regions");
    }

    std::unique_ptr<rcContourSet, ContourDeleter> contours(rcAllocContourSet());
    if (!contours || !rcBuildContours(&context, *compact, config.maxSimplificationError,
                                      config.maxEdgeLen, *contours))
    {
        return fail("could not trace the regions' outlines");
    }

    std::unique_ptr<rcPolyMesh, PolyMeshDeleter> polys(rcAllocPolyMesh());
    if (!polys || !rcBuildPolyMesh(&context, *contours, config.maxVertsPerPoly, *polys))
    {
        return fail("could not build polygons");
    }
    std::unique_ptr<rcPolyMeshDetail, DetailDeleter> detail(rcAllocPolyMeshDetail());
    if (!detail || !rcBuildPolyMeshDetail(&context, *polys, *compact, config.detailSampleDist,
                                          config.detailSampleMaxError, *detail))
    {
        return fail("could not build the height detail");
    }
    if (polys->npolys == 0)
    {
        return fail("nothing in the level is walkable for a body this size");
    }

    for (int i = 0; i < polys->npolys; ++i)
    {
        if (polys->areas[i] == RC_WALKABLE_AREA)
        {
            polys->flags[i] = kWalkFlag;
        }
        else if (polys->areas[i] == kCrawlArea)
        {
            polys->flags[i] = kCrawlFlag;
        }
    }

    dtNavMeshCreateParams params{};
    params.verts = polys->verts;
    params.vertCount = polys->nverts;
    params.polys = polys->polys;
    params.polyAreas = polys->areas;
    params.polyFlags = polys->flags;
    params.polyCount = polys->npolys;
    params.nvp = polys->nvp;
    params.detailMeshes = detail->meshes;
    params.detailVerts = detail->verts;
    params.detailVertsCount = detail->nverts;
    params.detailTris = detail->tris;
    params.detailTriCount = detail->ntris;
    params.walkableHeight = settings.agentHeight;
    params.walkableRadius = settings.agentRadius;
    params.walkableClimb = settings.agentMaxClimb;
    rcVcopy(params.bmin, polys->bmin);
    rcVcopy(params.bmax, polys->bmax);
    params.cs = config.cs;
    params.ch = config.ch;
    params.buildBvTree = true;

    unsigned char* data = nullptr;
    int dataSize = 0;
    if (!dtCreateNavMeshData(&params, &data, &dataSize))
    {
        return fail("could not pack the navigation data");
    }

    auto impl = std::make_unique<Impl>();
    impl->mesh = dtAllocNavMesh();
    if (impl->mesh == nullptr ||
        dtStatusFailed(impl->mesh->init(data, dataSize, DT_TILE_FREE_DATA)))
    {
        dtFree(data);
        return fail("could not load the navigation data");
    }
    impl->query = dtAllocNavMeshQuery();
    if (impl->query == nullptr || dtStatusFailed(impl->query->init(impl->mesh, 2048)))
    {
        return fail("could not start a navigation query");
    }
    impl->filter.setIncludeFlags(kWalkFlag);
    impl->filter.setExcludeFlags(0);

    // Jumps between floors, found on the finished mesh, and the mesh made again with them in it.
    const JumpLinks links = FindJumps(*impl->mesh, *impl->query, triangles);
    if (links.Count() > 0)
    {
        params.offMeshConVerts = links.verts.data();
        params.offMeshConRad = links.radius.data();
        params.offMeshConDir = links.direction.data();
        params.offMeshConAreas = links.areas.data();
        params.offMeshConFlags = links.flags.data();
        params.offMeshConUserID = links.ids.data();
        params.offMeshConCount = static_cast<int>(links.Count());
        unsigned char* linked = nullptr;
        int linkedSize = 0;
        if (dtCreateNavMeshData(&params, &linked, &linkedSize))
        {
            auto withLinks = std::make_unique<Impl>();
            withLinks->mesh = dtAllocNavMesh();
            if (withLinks->mesh != nullptr && dtStatusSucceed(withLinks->mesh->init(linked, linkedSize, DT_TILE_FREE_DATA)))
            {
                withLinks->query = dtAllocNavMeshQuery();
                if (withLinks->query != nullptr && dtStatusSucceed(withLinks->query->init(withLinks->mesh, 2048)))
                {
                    withLinks->filter.setIncludeFlags(kWalkFlag);
                    withLinks->filter.setExcludeFlags(0);
                    withLinks->jumps = links.Count();
                    impl = std::move(withLinks);
                }
            }
            else
            {
                dtFree(linked);
            }
        }
    }
    impl->polygons = static_cast<size_t>(polys->npolys);
    impl->mouths = FindCrawlMouths(*impl->mesh);
    m_impl = std::move(impl);

    const float milliseconds =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(AI, "Navigation mesh built: {} polygons, {} jumps and {} crawlspace openings from {} triangles in {:.1f} ms",
                  m_impl->polygons, m_impl->jumps, m_impl->mouths.size(), triangleCount, milliseconds);
    return true;
}

bool NavMesh::NearestPoint(const glm::vec3& near, float reach, glm::vec3& out, uint16_t allowed) const
{
    if (!Valid())
    {
        return false;
    }
    dtPolyRef ref = 0;
    float point[3];
    if (!m_impl->Nearest(near, reach, ref, point, allowed))
    {
        return false;
    }
    out = {point[0], point[1], point[2]};
    return true;
}

size_t NavMesh::JumpCount() const
{
    return m_impl != nullptr ? m_impl->jumps : 0;
}

bool NavMesh::InCrawlspace(const glm::vec3& point) const
{
    if (!Valid())
    {
        return false;
    }
    // The nearest floor of either kind: crawlspace floor is only the answer when it is nearer than
    // standing floor, which rules out the roof of a tunnel for somebody standing on it.
    dtPolyRef ref = 0;
    float at[3];
    if (!m_impl->Nearest(point, 0.8f, ref, at, kCrawlFlag))
    {
        return false;
    }
    unsigned short flags = 0;
    return dtStatusSucceed(m_impl->mesh->getPolyFlags(ref, &flags)) && (flags & kCrawlFlag) != 0;
}

const std::vector<glm::vec3>& NavMesh::CrawlMouths() const
{
    static const std::vector<glm::vec3> none;
    return m_impl != nullptr ? m_impl->mouths : none;
}

void NavMesh::Swap(NavMesh& other)
{
    std::swap(m_impl, other.m_impl);
    std::swap(m_settings, other.m_settings);
}

bool NavMesh::FindPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners,
                       bool* reached, std::vector<uint8_t>* jumps, uint16_t allowed) const
{
    corners.clear();
    if (jumps != nullptr)
    {
        jumps->clear();
    }
    if (reached != nullptr)
    {
        *reached = false;
    }
    if (!Valid())
    {
        return false;
    }

    dtPolyRef startRef = 0;
    dtPolyRef endRef = 0;
    float start[3];
    float end[3];
    if (!m_impl->Nearest(from, 2.0f, startRef, start, allowed))
    {
        return false;
    }
    // The destination is allowed to be further off the mesh than the start: a sound heard on the
    // far side of a crate is still worth walking towards, as far as the crate.
    if (!m_impl->Nearest(to, 4.0f, endRef, end, allowed))
    {
        return false;
    }

    dtPolyRef route[kMaxPolys];
    int routeCount = 0;
    // Across the floor, and across whichever jumps this body can make.
    dtQueryFilter filter = m_impl->filter;
    filter.setIncludeFlags(static_cast<unsigned short>(kWalkFlag | allowed));
    if (dtStatusFailed(m_impl->query->findPath(startRef, endRef, start, end, &filter, route,
                                               &routeCount, kMaxPolys)) ||
        routeCount == 0)
    {
        return false;
    }

    // A partial route -- the destination is somewhere not joined to here -- ends at the nearest
    // point of the last polygon it did reach.
    // Nor is it the whole way when the floor nearest the destination is a floor above or below it: the
    // roof of a crawlspace is the standing floor nearest somebody lying in it, and a creature that took
    // reaching the roof for reaching them climbed on top of the tunnel and stayed there.
    const bool whole = route[routeCount - 1] == endRef && std::abs(end[1] - to.y) < 1.2f;
    if (!whole)
    {
        float closest[3];
        if (dtStatusSucceed(m_impl->query->closestPointOnPoly(route[routeCount - 1], end, closest,
                                                                nullptr)))
        {
            dtVcopy(end, closest);
        }
    }

    float straight[kMaxCorners * 3];
    unsigned char flags[kMaxCorners];
    dtPolyRef refs[kMaxCorners];
    int straightCount = 0;
    if (dtStatusFailed(m_impl->query->findStraightPath(start, end, route, routeCount, straight, flags,
                                                       refs, &straightCount, kMaxCorners)))
    {
        return false;
    }
    corners.reserve(static_cast<size_t>(straightCount));
    for (int i = 0; i < straightCount; ++i)
    {
        corners.emplace_back(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]);
        if (jumps != nullptr)
        {
            jumps->push_back((flags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) != 0 ? 1 : 0);
        }
    }
    if (reached != nullptr)
    {
        *reached = whole;
    }
    return !corners.empty();
}

bool NavMesh::StraightWalk(const glm::vec3& from, const glm::vec3& to) const
{
    if (!Valid())
    {
        return false;
    }
    dtPolyRef startRef = 0;
    float start[3];
    if (!m_impl->Nearest(from, 2.0f, startRef, start))
    {
        return false;
    }
    float hit = 0.0f;
    float normal[3];
    dtPolyRef visited[kMaxPolys];
    int visitedCount = 0;
    const float end[3] = {to.x, to.y, to.z};
    if (dtStatusFailed(m_impl->query->raycast(startRef, start, end, &m_impl->filter, &hit, normal,
                                              visited, &visitedCount, kMaxPolys)))
    {
        return false;
    }
    // Detour reports a ray that reached its end without touching a wall as FLT_MAX.
    return hit > 1.0f;
}

bool NavMesh::MoveAlongSurface(const glm::vec3& from, const glm::vec3& to, glm::vec3& out, uint16_t allowed) const
{
    if (!Valid())
    {
        return false;
    }
    dtPolyRef startRef = 0;
    float start[3];
    if (!m_impl->Nearest(from, 1.0f, startRef, start, allowed))
    {
        return false;
    }
    const dtQueryFilter filter = m_impl->With(allowed & kCrawlFlag);
    const float end[3] = {to.x, to.y, to.z};
    float result[3];
    dtPolyRef visited[16];
    int visitedCount = 0;
    if (dtStatusFailed(m_impl->query->moveAlongSurface(startRef, start, end, &filter, result,
                                                       visited, &visitedCount, 16)) ||
        visitedCount == 0)
    {
        return false;
    }
    // The surface move keeps its height from where it started; the floor under where it arrived
    // is asked for separately, which is what carries it up a staircase.
    float height = result[1];
    if (dtStatusSucceed(m_impl->query->getPolyHeight(visited[visitedCount - 1], result, &height)))
    {
        result[1] = height;
    }
    out = {result[0], result[1], result[2]};
    return true;
}

bool NavMesh::RandomPointNear(const glm::vec3& centre, float radius, uint32_t& seed,
                              glm::vec3& out, uint16_t allowed) const
{
    if (!Valid())
    {
        return false;
    }
    dtPolyRef startRef = 0;
    float start[3];
    if (!m_impl->Nearest(centre, 2.0f, startRef, start, allowed))
    {
        return false;
    }
    const dtQueryFilter filter = m_impl->With(allowed & kCrawlFlag);
    t_randomState = seed != 0 ? seed : 0x9E3779B9u;
    dtPolyRef ref = 0;
    float point[3];
    const dtStatus status = m_impl->query->findRandomPointAroundCircle(
        startRef, start, radius, &filter, &DetourRandom, &ref, point);
    seed = t_randomState;
    if (dtStatusFailed(status) || ref == 0)
    {
        return false;
    }
    out = {point[0], point[1], point[2]};
    return true;
}

void NavMesh::Draw(DebugDraw& draw) const
{
    if (!Valid())
    {
        return;
    }
    const dtNavMesh& mesh = *m_impl->mesh;
    // Lifted a couple of centimetres, or the lines sink into the floor they lie on.
    const glm::vec3 lift{0.0f, 0.03f, 0.0f};
    for (int t = 0; t < mesh.getMaxTiles(); ++t)
    {
        const dtMeshTile* tile = mesh.getTile(t);
        if (tile == nullptr || tile->header == nullptr)
        {
            continue;
        }
        for (int p = 0; p < tile->header->polyCount; ++p)
        {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
            {
                // A jump, drawn as the arc it is taken along.
                const float* a = &tile->verts[poly.verts[0] * 3];
                const float* b = &tile->verts[poly.verts[1] * 3];
                const glm::vec3 from{a[0], a[1], a[2]};
                const glm::vec3 to{b[0], b[1], b[2]};
                const float arc = 0.3f + std::abs(from.y - to.y) * 0.3f;
                glm::vec3 last = from;
                for (int k = 1; k <= 8; ++k)
                {
                    const float along = static_cast<float>(k) / 8.0f;
                    const glm::vec3 point = glm::mix(from, to, along) + glm::vec3(0.0f, arc * 4.0f * along * (1.0f - along), 0.0f);
                    draw.Line(last, point, Color::RGBA(255, 170, 60, 230));
                    last = point;
                }
                continue;
            }
            for (int v = 0; v < poly.vertCount; ++v)
            {
                const float* a = &tile->verts[poly.verts[v] * 3];
                const float* b = &tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
                // Outer edges, the ones with nothing across them, drawn brighter: those are the
                // limits of where the creature can go, which is the thing worth reading.
                const bool edge = poly.neis[v] == 0;
                const bool crawl = (poly.flags & kCrawlFlag) != 0;
                draw.Line(glm::vec3(a[0], a[1], a[2]) + lift, glm::vec3(b[0], b[1], b[2]) + lift,
                          crawl ? (edge ? Color::RGBA(255, 120, 220, 230) : Color::RGBA(150, 60, 130, 120))
                                : (edge ? Color::RGBA(90, 230, 255, 230) : Color::RGBA(40, 110, 140, 120)));
            }
        }
    }
}

} // namespace pred
