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

namespace pred
{
namespace
{

// Every walkable polygon carries this flag, and every query includes it. One flag for now; the later
// phases add others for doors, vents and ledges, which is what flags are for.
constexpr unsigned short kWalkFlag = 0x01;
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

} // namespace

struct NavMesh::Impl
{
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtQueryFilter filter;
    size_t polygons = 0;

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

    bool Nearest(const glm::vec3& point, float reach, dtPolyRef& ref, float out[3]) const
    {
        float extents[3];
        Extents(reach, extents);
        ref = 0;
        const dtStatus status = query->findNearestPoly(&point.x, extents, &filter, &ref, out);
        return dtStatusSucceed(status) && ref != 0;
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
    config.walkableHeight = static_cast<int>(std::ceil(settings.agentHeight / config.ch));
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
    impl->polygons = static_cast<size_t>(polys->npolys);
    m_impl = std::move(impl);

    const float milliseconds =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(AI, "Navigation mesh built: {} polygons from {} triangles in {:.1f} ms",
                  m_impl->polygons, triangleCount, milliseconds);
    return true;
}

bool NavMesh::NearestPoint(const glm::vec3& near, float reach, glm::vec3& out) const
{
    if (!Valid())
    {
        return false;
    }
    dtPolyRef ref = 0;
    float point[3];
    if (!m_impl->Nearest(near, reach, ref, point))
    {
        return false;
    }
    out = {point[0], point[1], point[2]};
    return true;
}

bool NavMesh::FindPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners,
                       bool* reached) const
{
    corners.clear();
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
    if (!m_impl->Nearest(from, 2.0f, startRef, start))
    {
        return false;
    }
    // The destination is allowed to be further off the mesh than the start: a sound heard on the
    // far side of a crate is still worth walking towards, as far as the crate.
    if (!m_impl->Nearest(to, 4.0f, endRef, end))
    {
        return false;
    }

    dtPolyRef route[kMaxPolys];
    int routeCount = 0;
    if (dtStatusFailed(m_impl->query->findPath(startRef, endRef, start, end, &m_impl->filter, route,
                                               &routeCount, kMaxPolys)) ||
        routeCount == 0)
    {
        return false;
    }

    // A partial route -- the destination is somewhere not joined to here -- ends at the nearest
    // point of the last polygon it did reach.
    const bool whole = route[routeCount - 1] == endRef;
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

bool NavMesh::RandomPointNear(const glm::vec3& centre, float radius, uint32_t& seed,
                              glm::vec3& out) const
{
    if (!Valid())
    {
        return false;
    }
    dtPolyRef startRef = 0;
    float start[3];
    if (!m_impl->Nearest(centre, 2.0f, startRef, start))
    {
        return false;
    }
    t_randomState = seed != 0 ? seed : 0x9E3779B9u;
    dtPolyRef ref = 0;
    float point[3];
    const dtStatus status = m_impl->query->findRandomPointAroundCircle(
        startRef, start, radius, &m_impl->filter, &DetourRandom, &ref, point);
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
            for (int v = 0; v < poly.vertCount; ++v)
            {
                const float* a = &tile->verts[poly.verts[v] * 3];
                const float* b = &tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
                // Outer edges, the ones with nothing across them, drawn brighter: those are the
                // limits of where the creature can go, which is the thing worth reading.
                const bool edge = poly.neis[v] == 0;
                draw.Line(glm::vec3(a[0], a[1], a[2]) + lift, glm::vec3(b[0], b[1], b[2]) + lift,
                          edge ? Color::RGBA(90, 230, 255, 230) : Color::RGBA(40, 110, 140, 120));
            }
        }
    }
}

} // namespace pred
