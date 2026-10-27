#include "Game/World/FacilityMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/FacilityFurniture.h"
#include "Game/World/FacilityOutside.h"
#include "Game/World/MapBuilder.h"
#include "Game/World/Surfaces.h"

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <vector>
#include <cmath>
#include <map>
#include <tuple>

namespace pred
{
namespace
{

using Cell = FacilityLayout::Cell;
using Kind = FacilityMap::Piece::Kind;
using Blueprint = FacilityMap::Blueprint;

constexpr float kCell = FacilityLayout::kCell;
constexpr float kStorey = FacilityLayout::kStorey;
constexpr float kClear = FacilityLayout::kClearHeight;
constexpr float kSlab = FacilityLayout::kSlab;
constexpr float kDuct = FacilityLayout::kDuctHeight;
// Half a wall's thickness: a wall stands on the edge between two cells, half in each.
constexpr float kHalfWall = FacilitySpec::kHalfWall;
// The building's outside wall: thicker than any inside it, outward from where a perimeter room's wall face is, with
// a skin of cladding over it and a parapet round the roof capped with the same.
constexpr float kShellThickness = FacilitySpec::kShellThickness;
constexpr float kCladding = FacilitySpec::kCladding;
constexpr float kParapet = FacilitySpec::kParapet;
constexpr float kCapHeight = FacilitySpec::kCapHeight;
// How far a way out's sill stands above the floor.
constexpr float kSill = 0.012f;
// The sill's top over the doorway's foot: the wall and cladding left under a doorway end at its foot, so a sill whose top
// were there too would lie on them across the wall's thickness, and the two flicker.
constexpr float kSillTop = kSill + 0.006f;
// How far a piece reaches into the one it meets, where it cannot be seen: so a join is never a line with nothing
// behind it. Only ever into something, never along a face another piece shares, which is z-fighting.
constexpr float kSink = 0.02f;
// Outside the building, as a space (see Space).
constexpr int kOutside = -4;
// The top of a slab, drawn as floor, over the rest of it, drawn as ceiling.
constexpr float kFinish = 0.05f;
// The holes in the walls.
constexpr float kDoorWidth = FacilitySpec::kDoorWidth;
constexpr float kDoorHeight = FacilitySpec::kDoorHeight;
constexpr float kArchWidth = 1.6f;
constexpr float kArchHeight = 2.5f;
constexpr float kMouthWidth = 1.0f;
// A stairwell's way in and out: as wide and as tall as the corridor lets it be, for stairs this wide.
constexpr float kStairArchWidth = 2.2f;
constexpr float kStairArchHeight = 2.7f;
// The panel in a doorway: a little narrower and lower than the hole, so it swings clear of it.
constexpr float kPanelWidth = 1.18f;
constexpr float kPanelHeight = 2.18f;
// A stairwell, from its low end: a landing this long, then the flight, then the landing of the floor
// above, which the flight arrives at.
constexpr float kFoot = 1.0f;
constexpr float kFlight = static_cast<float>(FacilityMap::kSteps) * FacilityMap::kStepRun;
// How far a lamp reaches: across the cells round it to the next lamp's. (It cannot light the floor under or over it: each is
// kept to its own room or corridor by its box -- LevelLights::Bound -- and nothing here casts a lamp's shadow.)
constexpr float kCeilingLampRange = 6.5f;
constexpr float kEmergencyLampRange = 5.0f;
// The cabinet supplies are left on.
constexpr glm::vec3 kCabinet{0.9f, 0.85f, 0.45f};
constexpr float kLockerDepth = 0.88f;
constexpr float kAmmoCrateDepth = 0.46f;

const Material kFloorMaterial = Material::Diffuse({0.22f, 0.23f, 0.24f}, 0.9f);
const Material kCeilingMaterial = Material::Diffuse({0.30f, 0.30f, 0.31f}, 0.95f);
const Material kWallMaterial = Material::Diffuse({0.42f, 0.44f, 0.42f}, 0.9f);
const Material kDuctMaterial = Material::Metal({0.40f, 0.41f, 0.42f}, 0.5f);
const Material kStairMaterial = Material::Diffuse({0.34f, 0.33f, 0.31f}, 0.85f);
const Material kPillarMaterial = Material::Diffuse({0.36f, 0.36f, 0.35f}, 0.9f);
const Material kShelfMaterial = Material::Metal({0.30f, 0.33f, 0.36f}, 0.55f);
const Material kBenchMaterial = Material::Diffuse({0.28f, 0.27f, 0.25f}, 0.8f);
const Material kCrateMaterial = Material::Diffuse({0.38f, 0.30f, 0.22f}, 0.85f);
const Material kCabinetMaterial = Material::Metal({0.26f, 0.31f, 0.28f}, 0.6f);
// The outside: weathered panels, darker than anything inside.
const Material kCladdingMaterial = Material::Diffuse({0.24f, 0.25f, 0.25f}, 0.85f);
// A way out's threshold: worn tread plate, not the cladding (whose ribs, seen from above, read as flicker).
const Material kSillMaterial = Material::Metal({0.3f, 0.3f, 0.31f}, 0.5f);
// Under the ground, the outside wall's face: poured concrete.
const Material kFoundationMaterial = Material::Diffuse({0.4f, 0.4f, 0.38f}, 0.9f);
// What a room's furnishing is made of (FacilityFurniture).
const Material kWoodMaterial = Material::Diffuse({0.42f, 0.3f, 0.2f}, 0.6f);
const Material kCardboardMaterial = Material::Diffuse({0.55f, 0.42f, 0.27f}, 0.9f);
const Material kFabricMaterial = Material::Diffuse({0.2f, 0.26f, 0.32f}, 0.95f);
const Material kBeddingMaterial = Material::Diffuse({0.7f, 0.7f, 0.68f}, 0.95f);
const Material kSteelMaterial = Material::Metal({0.55f, 0.56f, 0.57f}, 0.4f);
const Material kDarkSteelMaterial = Material::Metal({0.2f, 0.21f, 0.22f}, 0.45f);
const Material kPaintMaterial = Material::Diffuse({0.75f, 0.42f, 0.1f}, 0.55f);
const Material kPlasticMaterial = Material::Diffuse({0.72f, 0.72f, 0.7f}, 0.45f);
const Material kDarkPlasticMaterial = Material::Diffuse({0.08f, 0.085f, 0.09f}, 0.4f);
const Material kPorcelainMaterial = Material::Diffuse({0.88f, 0.88f, 0.86f}, 0.2f);
const Material kLeafMaterial = Material::Diffuse({0.18f, 0.32f, 0.12f}, 0.8f);

// Where the building being drawn stands: the corner of its cell (0, 0), at ground-floor level. Set for the
// length of a Draw, from the plan; the facility's usual place otherwise.
thread_local glm::vec3 t_origin = FacilitySpec::kOrigin;

struct OriginScope
{
    explicit OriginScope(const glm::vec3& origin) : previous(t_origin) { t_origin = origin; }
    ~OriginScope() { t_origin = previous; }
    OriginScope(const OriginScope&) = delete;
    OriginScope& operator=(const OriginScope&) = delete;
    glm::vec3 previous;
};

float Base(int floor)
{
    return t_origin.y + static_cast<float>(floor) * kStorey;
}
float WorldX(float cells)
{
    return t_origin.x + cells * kCell;
}
float WorldZ(float cells)
{
    return t_origin.z + cells * kCell;
}
float WorldX(int cells)
{
    return WorldX(static_cast<float>(cells));
}
float WorldZ(int cells)
{
    return WorldZ(static_cast<float>(cells));
}

// A box between two corners, in the world.
void Box(Blueprint& out, Kind kind, glm::vec3 lo, glm::vec3 hi)
{
    const glm::vec3 a = glm::min(lo, hi);
    const glm::vec3 b = glm::max(lo, hi);
    if (b.x - a.x < 0.005f || b.y - a.y < 0.005f || b.z - a.z < 0.005f)
    {
        return;
    }
    FacilityMap::Piece piece;
    piece.kind = kind;
    piece.centre = (a + b) * 0.5f;
    piece.size = b - a;
    out.pieces.push_back(piece);
}

// Which space a cell is part of. A wall stands between two cells in different ones; -1 is the solid
// between everything, and -4 the outside, so the building has an outer wall all the way round.
int Space(const FacilityLayout& layout, int f, int x, int z)
{
    if (x < 0 || z < 0 || x >= layout.width || z >= layout.depth)
    {
        return kOutside;
    }
    switch (layout.At(f, x, z))
    {
    case Cell::Room:
    case Cell::Stair:
        return layout.roomOf[layout.Index(f, x, z)];
    case Cell::Corridor:
        return -2;
    case Cell::Duct:
        return -3; // every duct is one space: two that touch are joined
    case Cell::Outside:
        return kOutside; // past its walls, or over a lower roof
    case Cell::Solid:
        break;
    }
    return -1;
}

struct Span
{
    float a = 0.0f;
    float b = 0.0f;
};

// The union of some spans along a line, less some holes, as the pieces left.
std::vector<Span> Cut(std::vector<Span> solid, const std::vector<Span>& holes)
{
    std::sort(solid.begin(), solid.end(), [](const Span& l, const Span& r) { return l.a < r.a; });
    std::vector<Span> merged;
    for (const Span& s : solid)
    {
        if (!merged.empty() && s.a <= merged.back().b + 1e-4f)
        {
            merged.back().b = std::max(merged.back().b, s.b);
        }
        else
        {
            merged.push_back(s);
        }
    }
    for (const Span& hole : holes)
    {
        std::vector<Span> next;
        for (const Span& s : merged)
        {
            if (hole.b <= s.a || hole.a >= s.b)
            {
                next.push_back(s);
                continue;
            }
            if (hole.a > s.a)
            {
                next.push_back({s.a, hole.a});
            }
            if (hole.b < s.b)
            {
                next.push_back({hole.b, s.b});
            }
        }
        merged = std::move(next);
    }
    // (Nor a sliver left beside a doorway at a corner, a few centimetres inside the wall crossing it there: drawn in the same
    // place as that wall's faces, it flickered.)
    std::erase_if(merged, [](const Span& s) { return s.b - s.a < 0.05f; });
    return merged;
}

struct Opening
{
    float width = 0.0f;
    float height = 0.0f;
};
// By the edge they are in: floor, the cell on the low side, and which way the edge runs (0: between
// the cell and the one at +x; 1: the one at +z).
using Openings = std::map<std::tuple<int, int, int, int>, Opening>;

std::tuple<int, int, int, int> ExitKey(const FacilityLayout::Exit& exit, int ground)
{
    switch (exit.side)
    {
    case 0: return {ground, exit.cell.x - 1, exit.cell.y, 0};
    case 1: return {ground, exit.cell.x, exit.cell.y, 0};
    case 2: return {ground, exit.cell.x, exit.cell.y - 1, 1};
    default: return {ground, exit.cell.x, exit.cell.y, 1};
    }
}

Openings FindOpenings(const FacilityLayout& layout)
{
    Openings openings;
    for (const FacilityLayout::Door& door : layout.doors)
    {
        const bool stairs = door.room < 0;
        openings[{door.floor, door.cell.x, door.cell.y, door.side}] =
            door.hasDoor ? Opening{kDoorWidth, kDoorHeight} : stairs ? Opening{kStairArchWidth, kStairArchHeight} : Opening{kArchWidth, kArchHeight};
    }
    // The ways out: a door in the outer wall, on the edge between the cell inside and the one beyond it.
    for (const FacilityLayout::Exit& exit : layout.exits)
    {
        openings[ExitKey(exit, layout.ground)] = Opening{kDoorWidth, kDoorHeight};
    }
    for (const FacilityLayout::Exit& tunnel : layout.tunnels)
    {
        openings[ExitKey(tunnel, layout.ground - 1)] = Opening{kDoorWidth, kDoorHeight};
    }
    // A duct's mouth: a hole at the foot of the wall, as high as the crawlspace behind it.
    for (const FacilityLayout::Duct& duct : layout.ducts)
    {
        for (const FacilityLayout::Mouth& mouth : duct.mouths)
        {
            const glm::ivec2 low = glm::min(mouth.duct, mouth.open);
            const int side = mouth.duct.x != mouth.open.x ? 0 : 1;
            openings[{duct.floor, low.x, low.y, side}] = {kMouthWidth, kDuct};
        }
    }
    return openings;
}

// The walls of a floor, as few boxes as will do, and none of them inside another: the walls along x
// run through every corner they reach, and the walls along z stop at their faces.
void Walls(const FacilityLayout& layout, int f, const Openings& openings, Blueprint& out)
{
    const int w = layout.width;
    const int d = layout.depth;
    // A little into the slab under it and the one over it, where nothing sees: a wall that stops exactly at a
    // floor or a ceiling leaves a hairline of whatever is behind it along the join.
    const float y0 = Base(f) - kSink;
    const float y1 = Base(f) + kClear + kSink;
    // Round a stairwell, the whole storey: its walls go on up past the ceiling to the floor above, so the slab
    // between is never seen edge on from the stairs.
    const float tall = Base(f) + kStorey - kSink; // up to the next floor's walls, which start kSink under it: on, not into them
                                                  // (their sides and these would be one face drawn twice where they met)
    const auto stair = [&](int x, int z) { return layout.At(f, x, z) == Cell::Stair; };
    std::vector<bool> corner(static_cast<size_t>((w + 1) * (d + 1)), false);
    const auto cornerIndex = [&](int i, int j) { return static_cast<size_t>(j * (w + 1) + i); };

    // Along x, between row j - 1 and row j.
    for (int j = 0; j <= d; ++j)
    {
        std::vector<Span> solid;
        std::vector<Span> solidTall;
        std::vector<Span> holes;
        std::vector<std::pair<Span, float>> lintels;
        std::vector<std::pair<Span, float>> lintelsTall;
        for (int i = 0; i < w; ++i)
        {
            const int below = Space(layout, f, i, j - 1);
            const int above = Space(layout, f, i, j);
            if (below == above)
            {
                continue;
            }
            corner[cornerIndex(i, j)] = true;
            corner[cornerIndex(i + 1, j)] = true;
            // The outside of the building is its shell's (see Shell), not a wall a floor at a time.
            if (below == kOutside || above == kOutside)
            {
                continue;
            }
            const bool high = stair(i, j - 1) || stair(i, j);
            // (Each end reaching into the wall it meets -- but not on into the outside wall where that carries on along the same
            // line, round an inside corner of the building: the two would have one face, drawn twice.)
            const auto shellAt = [&](int along) { return (Space(layout, f, along, j - 1) == kOutside) != (Space(layout, f, along, j) == kOutside); };
            (high ? solidTall : solid).push_back({WorldX(i) - (shellAt(i - 1) ? 0.0f : kHalfWall), WorldX(i + 1) + (shellAt(i + 1) ? 0.0f : kHalfWall)});
            const auto found = openings.find({f, i, j - 1, 1});
            if (found != openings.end())
            {
                const float mid = WorldX(static_cast<float>(i) + 0.5f);
                const Span hole{mid - found->second.width * 0.5f, mid + found->second.width * 0.5f};
                holes.push_back(hole);
                (high ? lintelsTall : lintels).emplace_back(hole, found->second.height);
            }
        }
        const float z = WorldZ(j);
        // (Stopping where a stairwell's taller wall in the same line takes over, rather than running on along its faces.)
        std::vector<Span> notHere = holes;
        for (const Span& span : Cut(solidTall, {}))
        {
            notHere.push_back(span);
        }
        for (const Span& span : Cut(solid, notHere))
        {
            Box(out, Kind::Wall, {span.a, y0, z - kHalfWall}, {span.b, y1, z + kHalfWall});
        }
        for (const Span& span : Cut(solidTall, holes))
        {
            Box(out, Kind::Wall, {span.a, y0, z - kHalfWall}, {span.b, tall, z + kHalfWall});
        }
        // Over each hole, flush with the wall either side.
        for (const auto& [hole, height] : lintels)
        {
            Box(out, Kind::Wall, {hole.a, Base(f) + height, z - kHalfWall}, {hole.b, y1, z + kHalfWall});
        }
        for (const auto& [hole, height] : lintelsTall)
        {
            Box(out, Kind::Wall, {hole.a, Base(f) + height, z - kHalfWall}, {hole.b, tall, z + kHalfWall});
        }
    }

    // Along z, between column i - 1 and column i, stopping just inside every wall along x they meet.
    for (int i = 0; i <= w; ++i)
    {
        std::vector<Span> solid;
        std::vector<Span> solidTall;
        std::vector<Span> holes;
        std::vector<std::pair<Span, float>> lintels;
        std::vector<std::pair<Span, float>> lintelsTall;
        for (int k = 0; k < d; ++k)
        {
            const int left = Space(layout, f, i - 1, k);
            const int right = Space(layout, f, i, k);
            if (left == right || left == kOutside || right == kOutside)
            {
                continue;
            }
            const bool high = stair(i - 1, k) || stair(i, k);
            const auto shellAt = [&](int along) { return (Space(layout, f, i - 1, along) == kOutside) != (Space(layout, f, i, along) == kOutside); };
            (high ? solidTall : solid).push_back({WorldZ(k) - (shellAt(k - 1) ? 0.0f : kHalfWall), WorldZ(k + 1) + (shellAt(k + 1) ? 0.0f : kHalfWall)});
            for (const int v : {k, k + 1})
            {
                if (corner[cornerIndex(i, v)])
                {
                    // Into the wall across it only where that wall goes on past it both ways, and so is round it: where it
                    // ends here, its end and this wall's side would be one face drawn twice (kSink), so this one stops at it.
                    const bool through = Space(layout, f, i - 1, v - 1) != Space(layout, f, i - 1, v) && Space(layout, f, i, v - 1) != Space(layout, f, i, v);
                    const float sink = through ? kSink : 0.0f;
                    holes.push_back({WorldZ(v) - kHalfWall + sink, WorldZ(v) + kHalfWall - sink});
                }
            }
            const auto found = openings.find({f, i - 1, k, 0});
            if (found != openings.end())
            {
                const float mid = WorldZ(static_cast<float>(k) + 0.5f);
                const Span hole{mid - found->second.width * 0.5f, mid + found->second.width * 0.5f};
                holes.push_back(hole);
                (high ? lintelsTall : lintels).emplace_back(hole, found->second.height);
            }
        }
        const float x = WorldX(i);
        // (Stopping where a stairwell's taller wall in the same line takes over, rather than running on along its faces.)
        std::vector<Span> notHere = holes;
        for (const Span& span : Cut(solidTall, {}))
        {
            notHere.push_back(span);
        }
        for (const Span& span : Cut(solid, notHere))
        {
            Box(out, Kind::Wall, {x - kHalfWall, y0, span.a}, {x + kHalfWall, y1, span.b});
        }
        for (const Span& span : Cut(solidTall, holes))
        {
            Box(out, Kind::Wall, {x - kHalfWall, y0, span.a}, {x + kHalfWall, tall, span.b});
        }
        for (const auto& [hole, height] : lintels)
        {
            Box(out, Kind::Wall, {x - kHalfWall, Base(f) + height, hole.a}, {x + kHalfWall, y1, hole.b});
        }
        for (const auto& [hole, height] : lintelsTall)
        {
            Box(out, Kind::Wall, {x - kHalfWall, Base(f) + height, hole.a}, {x + kHalfWall, tall, hole.b});
        }
    }
}

// The building's outside: its walls in straight runs round its shape (FacilityLayout::OuterWalls), each from under the
// ground floor -- or from the lower roof it rises over -- to a parapet over the roof behind it, with the ways out cut through
// it: the whole building as one thing, not floors with walls stood between them. Its inside face is where every perimeter
// room's wall face was, and it covers every slab's edge. A skin of cladding over it gives the outside a finish of its own,
// and a post fills each outside corner.
void Shell(const FacilityLayout& layout, Blueprint& out)
{
    const float bottom = Base(0) - kSlab;
    struct Hole
    {
        Span along;
        float low = 0.0f;
        float high = 0.0f;
    };
    // A face as a box between two corners, its long side along x or z, less its holes: cut into upright strips
    // at every hole's edges, and each strip into what is above and below the holes in it.
    const auto face = [&](bool alongX, float fixedLo, float fixedHi, float from, float to, const std::vector<Hole>& holes,
                          Kind kind, float faceBottom, float faceTop)
    {
        std::vector<float> cuts{from, to};
        for (const Hole& hole : holes)
        {
            cuts.push_back(hole.along.a);
            cuts.push_back(hole.along.b);
        }
        std::sort(cuts.begin(), cuts.end());
        for (size_t c = 0; c + 1 < cuts.size(); ++c)
        {
            const float a = cuts[c];
            const float b = cuts[c + 1];
            if (b - a < 0.005f)
            {
                continue;
            }
            std::vector<Span> solid{{faceBottom, faceTop}};
            std::vector<Span> gaps;
            for (const Hole& hole : holes)
            {
                if (hole.along.a <= a + 1.0e-4f && hole.along.b >= b - 1.0e-4f)
                {
                    gaps.push_back({hole.low, hole.high});
                }
            }
            for (const Span& up : Cut(solid, gaps))
            {
                if (alongX)
                {
                    Box(out, kind, {a, up.a, fixedLo}, {b, up.b, fixedHi});
                }
                else
                {
                    Box(out, kind, {fixedLo, up.a, a}, {fixedHi, up.b, b});
                }
            }
        }
    };
    // The cladding's foot, where a wall goes down to the ground: under the ground, but the shell and its skin one thing down
    // the basements' walls where there are any.
    const float skinBottom = layout.ground > 0 ? bottom : Base(0) - 0.25f;
    // The windows that look out from somewhere: each an opening through the wall, inside its frame and clear of it by a hair
    // (an edge of the hole on a face of the frame would be one face drawn twice), with a pane in it to stop anybody through.
    const std::vector<FacilityOutside::Window> windows = FacilityOutside::Windows(layout);
    for (const FacilityLayout::OuterWall& wall : layout.OuterWalls())
    {
        const int side = wall.side;
        const bool alongX = side >= 2;
        std::vector<Hole> holes;
        // The ways out through it on the ground floor, and the doorways into a passage a floor under it (SitePlan's tunnels).
        const auto cut = [&](const std::vector<FacilityLayout::Exit>& ways, int floor)
        {
            if (floor < wall.low || floor >= wall.high)
            {
                return;
            }
            for (const FacilityLayout::Exit& exit : ways)
            {
                const int line = side == 0 ? exit.cell.x : side == 1 ? exit.cell.x + 1 : side == 2 ? exit.cell.y : exit.cell.y + 1;
                const int along = alongX ? exit.cell.x : exit.cell.y;
                if (exit.side != side || line != wall.line || along < wall.from || along >= wall.to)
                {
                    continue;
                }
                const float mid = alongX ? WorldX(static_cast<float>(along) + 0.5f) : WorldZ(static_cast<float>(along) + 0.5f);
                holes.push_back({{mid - kDoorWidth * 0.5f, mid + kDoorWidth * 0.5f}, Base(floor) + kSill, Base(floor) + kDoorHeight});
            }
        };
        cut(layout.exits, layout.ground);
        cut(layout.tunnels, layout.ground - 1);
        const size_t doorways = holes.size();
        for (const FacilityOutside::Window& window : windows)
        {
            if (!window.open || window.side != side || window.line != wall.line || window.bay < wall.from || window.bay >= wall.to ||
                window.floor < wall.low || window.floor >= wall.high)
            {
                continue;
            }
            constexpr float kGap = 0.01f;
            const Hole hole{{window.a + FacilityOutside::kWindowFrame + kGap, window.b - FacilityOutside::kWindowFrame - kGap},
                            window.low + 0.06f, window.high - FacilityOutside::kWindowFrame - kGap};
            holes.push_back(hole);
            const float at = alongX ? WorldZ(wall.line) : WorldX(wall.line);
            const float outward = (side == 0 || side == 2) ? -1.0f : 1.0f;
            const float middle = at + outward * (kShellThickness * 0.5f - kHalfWall);
            FacilityMap::Piece pane;
            pane.kind = Kind::Glazing;
            pane.centre = alongX ? glm::vec3((hole.along.a + hole.along.b) * 0.5f, (hole.low + hole.high) * 0.5f, middle)
                                 : glm::vec3(middle, (hole.low + hole.high) * 0.5f, (hole.along.a + hole.along.b) * 0.5f);
            pane.size = alongX ? glm::vec3(hole.along.b - hole.along.a, hole.high - hole.low, 0.04f)
                               : glm::vec3(0.04f, hole.high - hole.low, hole.along.b - hole.along.a);
            out.panes.push_back(pane);
        }
        // Inside face on the line every room's wall face along it is on; outwards from there.
        const float at = alongX ? WorldZ(wall.line) : WorldX(wall.line);
        const float outward = (side == 0 || side == 2) ? -1.0f : 1.0f;
        const float inner = at - outward * kHalfWall;
        const float outer = inner + outward * kShellThickness;
        const float skin = outer + outward * kCladding;
        const float from = alongX ? WorldX(wall.from) : WorldZ(wall.from);
        const float to = alongX ? WorldX(wall.to) : WorldZ(wall.to);
        // From the ground, or from a little into the lower roof it stands on.
        const float foot = wall.low == 0 ? bottom : Base(wall.low) - kSink;
        const float top = Base(wall.high) + kParapet;
        // (A cap along z a centimetre under one along x: where two meet round an inside corner they overlap, and two tops in
        // one place flicker.)
        const float capTop = top + kCapHeight - (alongX ? 0.0f : 0.01f);
        face(alongX, std::min(inner, outer), std::max(inner, outer), from, to, holes, Kind::Wall, foot, top);
        // (Down past basements, concrete under the ground and the cladding only over it.)
        const float under = Base(layout.ground) - 0.25f;
        if (wall.low == 0 && layout.ground > 0)
        {
            face(alongX, std::min(outer, skin), std::max(outer, skin), from, to, holes, Kind::Foundation, bottom, under);
            face(alongX, std::min(outer, skin), std::max(outer, skin), from, to, holes, Kind::Cladding, under, top);
        }
        else
        {
            face(alongX, std::min(outer, skin), std::max(outer, skin), from, to, holes, Kind::Cladding, wall.low == 0 ? skinBottom : foot, top);
        }
        face(alongX, std::min(inner, skin), std::max(inner, skin), from, to, {}, Kind::Cladding, top, capTop);
        // A sill across each way out, through the whole wall, a little above the floor inside and the ground outside:
        // the wall under the doorway otherwise ends exactly at the ground's height, and the two flicker. Standing a few
        // millimetres proud of the cladding outside, as a sill does -- flush, its face and the cladding's were one, drawn twice.
        for (size_t h = 0; h < doorways; ++h)
        {
            const Hole& hole = holes[h];
            const float proud = skin > inner ? 0.006f : -0.006f;
            const float a = std::min(inner, skin + proud);
            const float b = std::max(inner, skin + proud);
            if (alongX)
            {
                Box(out, Kind::Sill, {hole.along.a, hole.low - kSill - 0.12f, a}, {hole.along.b, hole.low - kSill + kSillTop, b});
            }
            else
            {
                Box(out, Kind::Sill, {a, hole.low - kSill - 0.12f, hole.along.a}, {b, hole.low - kSill + kSillTop, hole.along.b});
            }
        }
    }
    // Each outside corner filled by a post, from where the corner begins to just over the caps, a couple of centimetres proud
    // of the faces either side of it, so none of its faces is one of theirs.
    constexpr float kPostProud = 0.02f;
    const float reach = kShellThickness + kCladding - kHalfWall + kPostProud;
    for (const FacilityLayout::OuterCorner& corner : layout.OuterCorners())
    {
        const glm::vec2 at{WorldX(corner.at.x), WorldZ(corner.at.y)};
        const glm::vec2 far = at + glm::vec2(corner.out) * reach;
        const float foot = corner.low == 0 ? skinBottom : Base(corner.low) - kSink;
        const float top = Base(corner.high) + kParapet + kCapHeight + kPostProud;
        const float under = Base(layout.ground) - 0.25f;
        if (foot < under)
        {
            Box(out, Kind::Foundation, {std::min(at.x, far.x), foot, std::min(at.y, far.y)}, {std::max(at.x, far.x), under, std::max(at.y, far.y)});
        }
        Box(out, Kind::Cladding, {std::min(at.x, far.x), std::max(foot, under), std::min(at.y, far.y)}, {std::max(at.x, far.x), top, std::max(at.y, far.y)});
    }
}

// The roofs of the crawlspaces: every duct cell filled from the top of the crawlspace to the ceiling,
// inside its walls, and bridged to the duct cells beside it.
void DuctRoofs(const FacilityLayout& layout, int f, const Openings& openings, Blueprint& out)
{
    const float y0 = Base(f) + kDuct;
    const float y1 = Base(f) + kClear;
    const auto duct = [&](int x, int z) { return layout.At(f, x, z) == Cell::Duct; };
    // The sides of the shaft: every duct cell narrowed to the width of one, the walls its own on every side
    // that is not more duct or a mouth.
    const float infill = (kCell - 2.0f * kHalfWall - FacilityLayout::kDuctWidth) * 0.5f;
    const auto mouth = [&](int x, int z, int dx, int dz)
    {
        const int lx = dx > 0 ? x : x + dx;
        const int lz = dz > 0 ? z : z + dz;
        const auto found = openings.find({f, lx, lz, dx != 0 ? 0 : 1});
        return found != openings.end() && found->second.height <= kDuct + 0.01f;
    };
    for (int z = 0; z < layout.depth; ++z)
    {
        for (int x = 0; x < layout.width; ++x)
        {
            if (!duct(x, z))
            {
                continue;
            }
            const bool openW = duct(x - 1, z) || mouth(x, z, -1, 0);
            const bool openE = duct(x + 1, z) || mouth(x, z, 1, 0);
            const bool openN = duct(x, z - 1) || mouth(x, z, 0, -1);
            const bool openS = duct(x, z + 1) || mouth(x, z, 0, 1);
            const float xlo = WorldX(x) + (duct(x - 1, z) ? 0.0f : kHalfWall);
            const float xhi = WorldX(x + 1) - (duct(x + 1, z) ? 0.0f : kHalfWall);
            const float zlo = WorldZ(z) + (duct(x, z - 1) ? 0.0f : kHalfWall);
            const float zhi = WorldZ(z + 1) - (duct(x, z + 1) ? 0.0f : kHalfWall);
            const float base = Base(f);
            if (!openN)
            {
                Box(out, Kind::Wall, {xlo, base, zlo}, {xhi, y0, zlo + infill});
            }
            if (!openS)
            {
                Box(out, Kind::Wall, {xlo, base, zhi - infill}, {xhi, y0, zhi});
            }
            const float zfrom = zlo + (openN ? 0.0f : infill);
            const float zto = zhi - (openS ? 0.0f : infill);
            if (!openW)
            {
                Box(out, Kind::Wall, {xlo, base, zfrom}, {xlo + infill, y0, zto});
            }
            if (!openE)
            {
                Box(out, Kind::Wall, {xhi - infill, base, zfrom}, {xhi, y0, zto});
            }
        }
    }
    for (int z = 0; z < layout.depth; ++z)
    {
        for (int x = 0; x < layout.width; ++x)
        {
            if (!duct(x, z))
            {
                continue;
            }
            Box(out, Kind::Duct, {WorldX(x) + kHalfWall, y0, WorldZ(z) + kHalfWall},
                {WorldX(x + 1) - kHalfWall, y1, WorldZ(z + 1) - kHalfWall});
            if (duct(x + 1, z))
            {
                Box(out, Kind::Duct, {WorldX(x + 1) - kHalfWall, y0, WorldZ(z) + kHalfWall},
                    {WorldX(x + 1) + kHalfWall, y1, WorldZ(z + 1) - kHalfWall});
            }
            if (duct(x, z + 1))
            {
                Box(out, Kind::Duct, {WorldX(x) + kHalfWall, y0, WorldZ(z + 1) - kHalfWall},
                    {WorldX(x + 1) - kHalfWall, y1, WorldZ(z + 1) + kHalfWall});
            }
            if (duct(x + 1, z) && duct(x, z + 1) && duct(x + 1, z + 1))
            {
                Box(out, Kind::Duct, {WorldX(x + 1) - kHalfWall, y0, WorldZ(z + 1) - kHalfWall},
                    {WorldX(x + 1) + kHalfWall, y1, WorldZ(z + 1) + kHalfWall});
            }
        }
    }
}

// A point in a stairwell: how far along it from its low end, and how far across it from its low side.
glm::vec2 InWell(const FacilityLayout::Stairwell& well, float along, float across)
{
    if (well.alongX)
    {
        const float x = well.rising ? WorldX(well.min.x) + along : WorldX(well.max.x + 1) - along;
        return {x, WorldZ(well.min.y) + across};
    }
    const float z = well.rising ? WorldZ(well.min.y) + along : WorldZ(well.max.y + 1) - along;
    return {WorldX(well.min.x) + across, z};
}

// Which way a flight climbs: the stairs climb along their own +z, which turned by yaw is (sin, cos).
float ClimbYaw(const FacilityLayout::Stairwell& well)
{
    if (well.alongX)
    {
        return well.rising ? glm::half_pi<float>() : -glm::half_pi<float>();
    }
    return well.rising ? 0.0f : glm::pi<float>();
}

// A ladder's hatch, in the ladder's frame (LadderFrame): half as wide across as it is, and how far out from the wall's face.
constexpr float kHatchHalf = 0.6f;
constexpr float kHatchOut = 1.1f;
// Where the climber steps off at the top, out from the wall: past the hatch, with room for their body.
constexpr float kLadderTopOut = 1.55f;

// A ladder up the back wall of its cell (FacilityLayout::Stairwell::Way::Ladder), as a frame of its own: its origin the middle
// of the foot of that wall, on the wall's face (in the world, at no height); +x across it, +z into the wall, -z out of it.
struct LadderFrame
{
    glm::vec2 foot{0.0f};
    glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};

    glm::vec3 At(float base, const glm::vec3& local) const { return glm::vec3(foot.x, base, foot.y) + turn * local; }
};

LadderFrame FrameOf(const FacilityLayout::Stairwell& well)
{
    return {InWell(well, kCell - kHalfWall, kCell * 0.5f), glm::angleAxis(ClimbYaw(well), glm::vec3(0.0f, 1.0f, 0.0f))};
}

void Slab(Blueprint& out, float top, glm::vec2 a, glm::vec2 b, Kind finish = Kind::Floor)
{
    Box(out, finish, {a.x, top - kFinish, a.y}, {b.x, top, b.y});
    Box(out, Kind::Ceiling, {a.x, top - kSlab, a.y}, {b.x, top - kFinish, b.y});
}

// Every floor's slab, and the roof over the top of each part -- as high as that part goes, covered as its outside says
// (FacilityLayout::Outside::roofing): whole, but for the hole each stairwell climbs through, and the landing the flight
// arrives at.
void Slabs(const FacilityLayout& layout, Blueprint& out)
{
    const int w = layout.width;
    const int d = layout.depth;
    for (int f = 0; f <= layout.floors; ++f)
    {
        const float top = Base(f);
        // A floor under every cell with a storey here, a roof over every one whose storeys end under it.
        std::vector<int> kind(static_cast<size_t>(w * d), 0); // 0 none, 1 floor, 2 roof
        for (int z = 0; z < d; ++z)
        {
            for (int x = 0; x < w; ++x)
            {
                const int h = layout.Height(x, z);
                kind[static_cast<size_t>(z * w + x)] = h > f ? 1 : h == f && h > 0 ? 2 : 0;
            }
        }
        for (const FacilityLayout::Stairwell& well : layout.stairwells)
        {
            if (well.floor + 1 != f)
            {
                continue;
            }
            for (int z = well.min.y; z <= well.max.y; ++z)
            {
                for (int x = well.min.x; x <= well.max.x; ++x)
                {
                    kind[static_cast<size_t>(z * w + x)] = 0;
                }
            }
            if (well.way == FacilityLayout::Stairwell::Way::Ladder)
            {
                // The cell's floor round its hatch: in front of it, and either side of it back to the wall.
                const LadderFrame frame = FrameOf(well);
                const float edge = kCell * 0.5f;
                const auto slab = [&](float x0, float z0, float x1, float z1)
                {
                    const glm::vec3 a = frame.At(top, {x0, 0.0f, z0});
                    const glm::vec3 b = frame.At(top, {x1, 0.0f, z1});
                    Slab(out, top, {std::min(a.x, b.x), std::min(a.z, b.z)}, {std::max(a.x, b.x), std::max(a.z, b.z)});
                };
                slab(-edge, -(kCell - kHalfWall), edge, -kHatchOut);
                slab(-edge, -kHatchOut, -kHatchHalf, kHalfWall);
                slab(kHatchHalf, -kHatchOut, edge, kHalfWall);
                continue;
            }
            Slab(out, top, InWell(well, kFoot + kFlight, 0.0f), InWell(well, 3.0f * kCell, kCell));
        }
        // As few rectangles as a row-by-row sweep finds, floors and roofs apart.
        for (const int want : {1, 2})
        {
            const Kind finish = want == 2 && layout.outside.roofing ? Kind::Roofing : Kind::Floor;
            std::vector<bool> used(kind.size(), false);
            const auto open = [&](int x, int z)
            { return kind[static_cast<size_t>(z * w + x)] == want && !used[static_cast<size_t>(z * w + x)]; };
            for (int z = 0; z < d; ++z)
            {
                for (int x = 0; x < w; ++x)
                {
                    if (!open(x, z))
                    {
                        continue;
                    }
                    int x1 = x;
                    while (x1 + 1 < w && open(x1 + 1, z))
                    {
                        ++x1;
                    }
                    int z1 = z;
                    while (z1 + 1 < d)
                    {
                        bool whole = true;
                        for (int i = x; i <= x1 && whole; ++i)
                        {
                            whole = open(i, z1 + 1);
                        }
                        if (!whole)
                        {
                            break;
                        }
                        ++z1;
                    }
                    for (int k = z; k <= z1; ++k)
                    {
                        for (int i = x; i <= x1; ++i)
                        {
                            used[static_cast<size_t>(k * w + i)] = true;
                        }
                    }
                    Slab(out, top, {WorldX(x), WorldZ(z)}, {WorldX(x1 + 1), WorldZ(z1 + 1)}, finish);
                }
            }
        }
    }
}

glm::vec2 Forward(float yaw)
{
    // What a thing faces: its own -z, turned by yaw about y.
    return {-std::sin(yaw), -std::cos(yaw)};
}

// Whether a thing the plan put against a wall really is: the cell behind it is not the room's.
bool BacksOntoWall(const FacilityLayout& layout, const FacilityLayout::Placed& thing)
{
    const glm::ivec2 cell{static_cast<int>(std::floor(thing.at.x)), static_cast<int>(std::floor(thing.at.y))};
    const glm::vec2 back = -Forward(thing.yaw);
    const glm::ivec2 behind = cell + glm::ivec2(static_cast<int>(std::round(back.x)), static_cast<int>(std::round(back.y)));
    return layout.RoomAt(thing.floor, cell.x, cell.y) != layout.RoomAt(thing.floor, behind.x, behind.y);
}

// Where something `depth` deep stands against the wall behind it: its back a couple of centimetres off
// the wall's face, at floor level.
glm::vec3 AgainstWall(const FacilityLayout& layout, const FacilityLayout::Placed& thing, float depth)
{
    if (!BacksOntoWall(layout, thing))
    {
        return FacilityMap::ToWorld(thing.floor, thing.at);
    }
    const glm::vec2 cell = glm::floor(thing.at) + glm::vec2(0.5f);
    const glm::vec2 at = glm::vec2(WorldX(cell.x), WorldZ(cell.y)) -
                         Forward(thing.yaw) * (kCell * 0.5f - kHalfWall - depth * 0.5f - 0.02f);
    return {at.x, Base(thing.floor), at.y};
}

uint32_t Mix(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

// What is left in a cabinet: mostly light, sometimes something to patch yourself up with.
const char* SupplyItem(uint32_t roll)
{
    roll %= 100;
    return roll < 40 ? "battery" : roll < 75 ? "flare" : "medkit";
}

void AddPiece(Blueprint& out, Kind kind, glm::vec3 floorAt, glm::vec3 size, float yaw)
{
    FacilityMap::Piece piece;
    piece.kind = kind;
    piece.centre = floorAt + glm::vec3(0.0f, size.y * 0.5f, 0.0f);
    piece.size = size;
    piece.yaw = yaw;
    out.pieces.push_back(piece);
}

void Things(const FacilityLayout& layout, Blueprint& out)
{
    for (size_t i = 0; i < layout.things.size(); ++i)
    {
        const FacilityLayout::Placed& thing = layout.things[i];
        const uint32_t roll = Mix(layout.seed, static_cast<uint32_t>(i));
        switch (thing.thing)
        {
        case FacilityLayout::Thing::Locker:
            out.placements.lockers.push_back({AgainstWall(layout, thing, kLockerDepth), thing.yaw});
            break;
        case FacilityLayout::Thing::AmmoCrate:
            out.placements.ammoCrates.push_back({AgainstWall(layout, thing, kAmmoCrateDepth), thing.yaw});
            break;
        case FacilityLayout::Thing::Supplies:
        case FacilityLayout::Thing::Keycard:
        {
            const glm::vec3 at = AgainstWall(layout, thing, kCabinet.z);
            AddPiece(out, Kind::Cabinet, at, kCabinet, thing.yaw);
            const glm::vec3 top = at + glm::vec3(0.0f, kCabinet.y, 0.0f);
            if (thing.thing == FacilityLayout::Thing::Keycard)
            {
                out.placements.items.push_back({"keycard", 1, top});
                break;
            }
            // Along the top, from one end to the other.
            const glm::vec3 right{std::cos(thing.yaw), 0.0f, -std::sin(thing.yaw)};
            const int count = 1 + static_cast<int>((roll >> 8) % 2);
            for (int k = 0; k < count; ++k)
            {
                const float offset = count == 1 ? 0.0f : (k == 0 ? -0.22f : 0.22f);
                out.placements.items.push_back({SupplyItem(Mix(roll, static_cast<uint32_t>(k))), 1, top + right * offset});
            }
            break;
        }
        case FacilityLayout::Thing::Shelf:
            AddPiece(out, Kind::Shelf, AgainstWall(layout, thing, thing.size.y), {thing.size.x, thing.height, thing.size.y}, thing.yaw);
            break;
        case FacilityLayout::Thing::Bench:
            AddPiece(out, Kind::Bench, AgainstWall(layout, thing, thing.size.y), {thing.size.x, thing.height, thing.size.y}, thing.yaw);
            break;
        case FacilityLayout::Thing::Pillar:
            AddPiece(out, Kind::Pillar, FacilityMap::ToWorld(thing.floor, thing.at), {thing.size.x, thing.height, thing.size.y}, 0.0f);
            break;
        case FacilityLayout::Thing::Crate:
            AddPiece(out, Kind::Crate, FacilityMap::ToWorld(thing.floor, thing.at), {thing.size.x, thing.height, thing.size.y}, thing.yaw);
            break;
        case FacilityLayout::Thing::Breaker:
            break; // something to use, not part of the building: the mission puts it up (MissionProps)
        }
    }
}

// A door in every doorway the plan put one in, hung so it swings into the room it belongs to.
void Doors(const FacilityLayout& layout, Blueprint& out)
{
    const float swing = glm::radians(100.0f);
    for (const FacilityLayout::Door& door : layout.doors)
    {
        if (!door.hasDoor)
        {
            continue;
        }
        WorldObjects::PlacedDoor placed;
        placed.width = kPanelWidth;
        placed.height = kPanelHeight;
        placed.locked = door.locked;
        const float y = Base(door.floor) + 0.01f;
        // A panel reaches along its own +x from the hinge; turned by yaw about y that is (cos, -sin), so
        // turning it further swings it towards -z, and from -90 degrees, towards +x.
        if (door.side == 0)
        {
            const float mid = WorldZ(static_cast<float>(door.cell.y) + 0.5f);
            placed.hinge = {WorldX(door.cell.x + 1), y, mid - kPanelWidth * 0.5f};
            placed.closedYaw = -glm::half_pi<float>();
            const bool roomBeyond = layout.RoomAt(door.floor, door.cell.x + 1, door.cell.y) == door.room;
            placed.openYaw = placed.closedYaw + (roomBeyond ? swing : -swing);
        }
        else
        {
            const float mid = WorldX(static_cast<float>(door.cell.x) + 0.5f);
            placed.hinge = {mid - kPanelWidth * 0.5f, y, WorldZ(door.cell.y + 1)};
            placed.closedYaw = 0.0f;
            const bool roomBeyond = layout.RoomAt(door.floor, door.cell.x, door.cell.y + 1) == door.room;
            placed.openYaw = roomBeyond ? -swing : swing;
        }
        // Two doors that would swing into each other: the second hung from the other side of its doorway.
        const auto swingsTo = [](const WorldObjects::PlacedDoor& d)
        { return d.hinge + glm::vec3(std::cos(d.openYaw), 0.0f, -std::sin(d.openYaw)) * (d.width * 0.5f); };
        const auto clashes = [&](const WorldObjects::PlacedDoor& d)
        {
            for (const WorldObjects::PlacedDoor& other : out.placements.doors)
            {
                if (std::abs(other.hinge.y - d.hinge.y) < 0.5f && glm::distance(swingsTo(other), swingsTo(d)) < 1.3f)
                {
                    return true;
                }
            }
            return false;
        };
        if (clashes(placed))
        {
            WorldObjects::PlacedDoor flipped = placed;
            const glm::vec3 along{std::cos(placed.closedYaw), 0.0f, -std::sin(placed.closedYaw)};
            flipped.hinge = placed.hinge + along * kPanelWidth;
            flipped.closedYaw = placed.closedYaw + glm::pi<float>();
            flipped.openYaw = flipped.closedYaw - (placed.openYaw - placed.closedYaw);
            if (!clashes(flipped))
            {
                placed = flipped;
            }
        }
        out.placements.doors.push_back(placed);
    }
    // A door in each way out, and in each doorway a floor down into a passage to another building (SitePlan's tunnels).
    const auto outer = [&](const FacilityLayout::Exit& way, int at)
    {
        WorldObjects::PlacedDoor placed;
        placed.width = kPanelWidth;
        placed.height = kPanelHeight;
        const float y = Base(at) + kSillTop + 0.004f; // just clear of its sill
        const auto [floor, lowX, lowZ, alongZ] = ExitKey(way, at);
        (void)floor;
        if (alongZ == 0)
        {
            const float mid = WorldZ(static_cast<float>(lowZ) + 0.5f);
            placed.hinge = {WorldX(lowX + 1), y, mid - kPanelWidth * 0.5f};
            placed.closedYaw = -glm::half_pi<float>();
            // In, towards the building: +x through the west wall, -x through the east.
            placed.openYaw = placed.closedYaw + (way.side == 0 ? swing : -swing);
        }
        else
        {
            const float mid = WorldX(static_cast<float>(lowX) + 0.5f);
            placed.hinge = {mid - kPanelWidth * 0.5f, y, WorldZ(lowZ + 1)};
            placed.closedYaw = 0.0f;
            placed.openYaw = way.side == 2 ? -swing : swing;
        }
        out.placements.doors.push_back(placed);
    };
    for (const FacilityLayout::Exit& exit : layout.exits)
    {
        outer(exit, layout.ground);
    }
    for (const FacilityLayout::Exit& tunnel : layout.tunnels)
    {
        outer(tunnel, layout.ground - 1);
    }
}

void Lamps(const FacilityLayout& layout, Blueprint& out)
{
    for (const FacilityLayout::Lamp& lamp : layout.lamps)
    {
        FacilityMap::Lamp placed;
        placed.position = FacilityMap::ToWorld(lamp.floor, lamp.at) + glm::vec3(0.0f, kClear - 0.04f, 0.0f);
        placed.circuit = lamp.circuit;
        placed.range = kCeilingLampRange;
        switch (lamp.mood)
        {
        case FacilityLayout::LampMood::Steady:
            placed.mood = LightMood::Steady;
            break;
        case FacilityLayout::LampMood::Flicker:
            placed.mood = LightMood::Flicker;
            break;
        case FacilityLayout::LampMood::Failing:
            placed.mood = LightMood::Failing;
            break;
        case FacilityLayout::LampMood::Dead:
            placed.mood = LightMood::Dead;
            break;
        case FacilityLayout::LampMood::Emergency:
            // On its own battery, breathing red, where the strip lights have gone.
            placed.kind = LightKind::Emergency;
            placed.mood = LightMood::Pulse;
            placed.range = kEmergencyLampRange;
            break;
        }
        // The box it lights: its room, or the longer straight run of corridor it hangs over.
        const glm::ivec2 cell{static_cast<int>(std::floor(lamp.at.x)), static_cast<int>(std::floor(lamp.at.y))};
        glm::ivec2 low = cell;
        glm::ivec2 high = cell;
        const int room = layout.RoomAt(lamp.floor, cell.x, cell.y);
        const bool inWell = layout.At(lamp.floor, cell.x, cell.y) == Cell::Stair;
        if (room >= 0)
        {
            low = layout.rooms[static_cast<size_t>(room)].min;
            high = layout.rooms[static_cast<size_t>(room)].max;
        }
        else if (inWell)
        {
            // (A ladder's: its own cell.)
            for (const FacilityLayout::Stairwell& well : layout.stairwells)
            {
                if (cell.x >= well.min.x && cell.x <= well.max.x && cell.y >= well.min.y && cell.y <= well.max.y)
                {
                    low = well.min;
                    high = well.max;
                }
            }
        }
        else
        {
            const auto corridor = [&](int x, int z) { return layout.At(lamp.floor, x, z) == Cell::Corridor; };
            const auto run = [&](glm::ivec2 step, glm::ivec2& from, glm::ivec2& to)
            {
                from = cell;
                to = cell;
                while (corridor(from.x - step.x, from.y - step.y))
                {
                    from -= step;
                }
                while (corridor(to.x + step.x, to.y + step.y))
                {
                    to += step;
                }
                return std::abs(to.x - from.x) + std::abs(to.y - from.y);
            };
            glm::ivec2 xLow, xHigh, zLow, zHigh;
            const int alongX = run({1, 0}, xLow, xHigh);
            const int alongZ = run({0, 1}, zLow, zHigh);
            low = alongX >= alongZ ? xLow : zLow;
            high = alongX >= alongZ ? xHigh : zHigh;
        }
        placed.boundsMin = FacilityMap::ToWorld(lamp.floor, glm::vec2(low)) - glm::vec3(0.0f, 0.02f, 0.0f);
        placed.boundsMax = FacilityMap::ToWorld(lamp.floor, glm::vec2(high + glm::ivec2(1))) + glm::vec3(0.0f, kClear + 0.02f, 0.0f);
        const int own = static_cast<int>(out.lamps.size());
        out.lamps.push_back(placed);
        if (room >= 0 || inWell)
        {
            continue;
        }
        // Every corridor that turns off this run near the lamp -- the other leg of a corner, a side
        // corridor at a junction -- lit by it too, down its own length.
        const auto corridorCell = [&](glm::ivec2 c) { return layout.At(lamp.floor, c.x, c.y) == Cell::Corridor; };
        const bool runAlongX = high.x != low.x || (high.y == low.y);
        const glm::ivec2 along = runAlongX ? glm::ivec2(1, 0) : glm::ivec2(0, 1);
        const glm::ivec2 perpendicular = runAlongX ? glm::ivec2(0, 1) : glm::ivec2(1, 0);
        std::vector<std::pair<glm::ivec2, glm::ivec2>> made;
        for (glm::ivec2 c = low;; c += along)
        {
            const glm::vec3 centre = FacilityMap::ToWorld(lamp.floor, glm::vec2(c) + glm::vec2(0.5f));
            if (glm::distance(glm::vec2(centre.x, centre.z), glm::vec2(placed.position.x, placed.position.z)) < 7.0f)
            {
                for (const int sign : {-1, 1})
                {
                    const glm::ivec2 step = perpendicular * sign;
                    glm::ivec2 end = c + step;
                    if (!corridorCell(end))
                    {
                        continue;
                    }
                    int cells = 1;
                    while (corridorCell(end + step) && cells < 12)
                    {
                        end += step;
                        ++cells;
                    }
                    const glm::ivec2 branchLow = glm::min(c + step, end);
                    const glm::ivec2 branchHigh = glm::max(c + step, end);
                    if (std::find(made.begin(), made.end(), std::pair{branchLow, branchHigh}) != made.end())
                    {
                        continue;
                    }
                    made.emplace_back(branchLow, branchHigh);
                    FacilityMap::Lamp copy = placed;
                    copy.copyOf = own;
                    copy.boundsMin = FacilityMap::ToWorld(lamp.floor, glm::vec2(branchLow)) - glm::vec3(0.0f, 0.02f, 0.0f);
                    copy.boundsMax = FacilityMap::ToWorld(lamp.floor, glm::vec2(branchHigh + glm::ivec2(1))) + glm::vec3(0.0f, kClear + 0.02f, 0.0f);
                    out.lamps.push_back(copy);
                }
            }
            if (c == high)
            {
                break;
            }
        }
    }

    // Through every doorway and archway, each way: the nearest working lamp on one side lights a little way
    // into the other. Without it a lit room's doorway was a black hole into the corridor, and back.
    const size_t ownLamps = out.lamps.size();
    for (const FacilityLayout::Door& door : layout.doors)
    {
        const glm::ivec2 other = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
        const glm::vec2 edge = glm::vec2(door.cell) + glm::vec2(0.5f) + (door.side == 0 ? glm::vec2(0.5f, 0.0f) : glm::vec2(0.0f, 0.5f));
        for (const auto& [from, to] : {std::pair{door.cell, other}, std::pair{other, door.cell}})
        {
            const glm::vec3 fromCentre = FacilityMap::ToWorld(door.floor, glm::vec2(from) + glm::vec2(0.5f));
            int best = -1;
            float nearest = 7.0f;
            for (size_t i = 0; i < ownLamps; ++i)
            {
                const FacilityMap::Lamp& lamp = out.lamps[i];
                const bool inside = fromCentre.x >= lamp.boundsMin.x && fromCentre.x <= lamp.boundsMax.x && fromCentre.z >= lamp.boundsMin.z &&
                                    fromCentre.z <= lamp.boundsMax.z && fromCentre.y >= lamp.boundsMin.y - 0.1f && fromCentre.y <= lamp.boundsMax.y;
                const float gap = glm::distance(glm::vec2(lamp.position.x, lamp.position.z), glm::vec2(fromCentre.x, fromCentre.z));
                if (inside && lamp.mood != LightMood::Dead && gap < nearest)
                {
                    nearest = gap;
                    best = static_cast<int>(i);
                }
            }
            if (best < 0)
            {
                continue;
            }
            const glm::vec2 across = glm::vec2(to - from);
            FacilityMap::Lamp spill = out.lamps[static_cast<size_t>(best)];
            spill.spillOf = best;
            // The middle of the doorway, a little below its top: the light is put just through it, facing on.
            spill.position = FacilityMap::ToWorld(door.floor, edge) + glm::vec3(0.0f, 1.95f, 0.0f);
            spill.direction = glm::normalize(glm::vec3(across.x, -0.35f, across.y));
            // The cell beyond, and its neighbours along the doorway when they are the same kind of place.
            glm::ivec2 low = to;
            glm::ivec2 high = to;
            const glm::ivec2 along = door.side == 0 ? glm::ivec2(0, 1) : glm::ivec2(1, 0);
            const auto same = [&](glm::ivec2 c) { return layout.At(door.floor, c.x, c.y) == layout.At(door.floor, to.x, to.y) &&
                                                         layout.RoomAt(door.floor, c.x, c.y) == layout.RoomAt(door.floor, to.x, to.y); };
            if (same(to - along))
            {
                low -= along;
            }
            if (same(to + along))
            {
                high += along;
            }
            const glm::ivec2 deeper = to + glm::ivec2(across);
            if (same(deeper))
            {
                low = glm::min(low, deeper);
                high = glm::max(high, deeper);
            }
            spill.boundsMin = FacilityMap::ToWorld(door.floor, glm::vec2(low)) - glm::vec3(0.0f, 0.02f, 0.0f);
            spill.boundsMax = FacilityMap::ToWorld(door.floor, glm::vec2(high + glm::ivec2(1))) + glm::vec3(0.0f, kClear + 0.02f, 0.0f);
            out.lamps.push_back(spill);
        }
    }
}

const Material& MaterialOf(Kind kind)
{
    // What each is made of: its surface set, how big it repeats and how much of the set's own colour shows -- laid on the first
    // time it is asked for, once the textures are there to lay.
    struct Made
    {
        Kind kind;
        const Material* plain;
        const char* surface;
        float metres;
        float keep;
    };
    static const Made kMade[] = {
        {Kind::Floor, &kFloorMaterial, "facility_floor", 3.0f, 0.3f},     {Kind::Ceiling, &kCeilingMaterial, "facility_ceiling", 3.0f, 0.3f},
        {Kind::Wall, &kWallMaterial, "facility_wall", 3.0f, 0.3f},        {Kind::Fill, &kWallMaterial, "facility_wall", 3.0f, 0.3f},
        {Kind::Duct, &kDuctMaterial, "metal_grate", 1.0f, 0.2f},          {Kind::Pillar, &kPillarMaterial, "concrete_pillar", 2.0f, 0.3f},
        {Kind::Shelf, &kShelfMaterial, "shelf_steel", 1.5f, 0.2f},        {Kind::Bench, &kBenchMaterial, "worktop", 1.0f, 0.3f},
        {Kind::Crate, &kCrateMaterial, "cargo_crate", 1.5f, 0.3f},        {Kind::Cabinet, &kCabinetMaterial, "locker_metal", 1.5f, 0.2f},
        {Kind::Cladding, &kCladdingMaterial, "building_cladding", 4.0f, 0.2f}, {Kind::Sill, &kSillMaterial, "facility_stairs", 1.0f, 0.4f},
        {Kind::Foundation, &kFoundationMaterial, "concrete_slab", 3.0f, 0.3f},
        {Kind::Wood, &kWoodMaterial, "tabletop", 1.2f, 0.45f},          {Kind::Cardboard, &kCardboardMaterial, "cargo_crate", 0.8f, 0.2f},
        {Kind::Fabric, &kFabricMaterial, "fabric_cushion", 0.8f, 0.3f},  {Kind::Bedding, &kBeddingMaterial, "fabric_cushion", 0.8f, 0.15f},
        {Kind::Steel, &kSteelMaterial, "shelf_steel", 1.0f, 0.2f},       {Kind::DarkSteel, &kDarkSteelMaterial, "gun_metal", 1.0f, 0.2f},
        {Kind::Paint, &kPaintMaterial, "painted_metal", 1.5f, 0.1f},     {Kind::Plastic, &kPlasticMaterial, "console_casing", 1.0f, 0.15f},
        {Kind::DarkPlastic, &kDarkPlasticMaterial, "weapon_polymer", 1.0f, 0.2f}, {Kind::Porcelain, &kPorcelainMaterial, "worktop", 1.0f, 0.05f},
        {Kind::Leaf, &kLeafMaterial, "jungle_floor", 0.6f, 0.3f},
    };
    static std::vector<Material> laid;
    if (laid.empty())
    {
        for (const Made& made : kMade)
        {
            laid.push_back(Surfaces::Apply(*made.plain, made.surface, made.metres, made.keep));
            // (Snowed and rained on as the ground is, but not the ground: no footprint or wash up a shore drawn on a step.)
            laid.back().weathering = 0.99f;
        }
    }
    for (size_t i = 0; i < std::size(kMade); ++i)
    {
        if (kMade[i].kind == kind)
        {
            return laid[i];
        }
    }
    return kWallMaterial;
}

// What its outside is made of (FacilityLayout::Outside): its cladding in its own colour and finish, its trim, accent, roof
// plant and roof covering painted to suit, and dark glass.
Material OutsideMaterial(Kind kind, const FacilityLayout::Outside& look)
{
    Material made;
    switch (kind)
    {
    case Kind::Cladding: made = Surfaces::Apply(Material::Diffuse(look.cladding, 0.85f), look.surface, 4.0f, 0.2f); break;
    case Kind::Trim: made = Surfaces::ApplyWorn(Material::Diffuse(look.trim, 0.6f), "painted_metal", 2.0f, 0.35f); break;
    case Kind::Accent: made = Surfaces::ApplyWorn(Material::Diffuse(look.accent, 0.55f), "painted_metal", 2.0f, 0.4f); break;
    case Kind::Kit: made = Surfaces::ApplyWorn(Material::Diffuse({0.5f, 0.51f, 0.5f}, 0.6f), "painted_metal", 2.0f, 0.45f); break;
    case Kind::Roofing: made = Surfaces::Apply(Material::Diffuse({0.15f, 0.15f, 0.15f}, 0.95f), "asphalt", 3.0f, 0.3f); break;
    case Kind::Glazing:
        // Dark, and smooth enough to give back the sky; hardly wet or snowed on, upright as it is.
        made = Material::Metal({0.05f, 0.065f, 0.08f}, 0.12f);
        made.weathering = 0.3f;
        return made;
    case Kind::Screen:
        // A screen, lit: what is on it a cool glow.
        made = Material::Diffuse({0.04f, 0.06f, 0.08f}, 0.2f);
        made.emissive = {0.18f, 0.32f, 0.42f};
        made.weathering = 0.0f;
        return made;
    case Kind::Mirror:
        made = Material::Metal({0.82f, 0.84f, 0.86f}, 0.04f);
        made.weathering = 0.0f;
        return made;
    default: return MaterialOf(kind);
    }
    made.weathering = 0.99f;
    return made;
}

const char* NameOf(Kind kind)
{
    switch (kind)
    {
    case Kind::Floor:
        return "facility_floor";
    case Kind::Ceiling:
        return "facility_ceiling";
    case Kind::Wall:
        return "facility_wall";
    case Kind::Cladding:
        return "facility_cladding";
    case Kind::Duct:
        return "facility_duct";
    case Kind::Pillar:
        return "facility_pillar";
    case Kind::Shelf:
        return "facility_shelf";
    case Kind::Bench:
        return "facility_bench";
    case Kind::Crate:
        return "facility_crate";
    case Kind::Cabinet:
        return "facility_cabinet";
    case Kind::Fill:
        return "facility_stair_fill";
    case Kind::Sill:
        return "facility_sill";
    case Kind::Glazing:
        return "facility_glazing";
    case Kind::Trim:
        return "facility_trim";
    case Kind::Accent:
        return "facility_accent";
    case Kind::Kit:
        return "facility_kit";
    case Kind::Roofing:
        return "facility_roofing";
    case Kind::Foundation:
        return "facility_foundation";
    }
    return "facility";
}

// A ladder's cell (FacilityLayout::Stairwell::Way::Ladder): the ladder up its back wall from the floor it stands on, through the
// hatch in the floor above (cut by Slabs) and on up past it to hold on to stepping off; round the hatch up there a rail one
// side and its lid, stood open, the other, and the way on in front. What is climbed is the player's (Blueprint::ladders);
// what is drawn is drawn as furnishing, the rail and the lid felt as a box each.
void LadderWell(const FacilityLayout::Stairwell& well, Blueprint& out)
{
    using Form = FacilityMap::Dressing::Form;
    const LadderFrame frame = FrameOf(well);
    const float base = Base(well.floor);
    const float top = base + kStorey;
    const auto add = [&](Form form, Kind kind, float floor, const glm::vec3& centre, const glm::vec3& size,
                         const glm::quat& local = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        FacilityMap::Dressing dressing;
        dressing.form = form;
        dressing.kind = kind;
        dressing.centre = frame.At(floor, centre);
        dressing.size = size;
        dressing.rotation = frame.turn * local;
        out.furnishing.push_back(dressing);
    };
    const auto box = [&](Kind kind, float floor, const glm::vec3& lo, const glm::vec3& hi) { add(Form::Box, kind, floor, (lo + hi) * 0.5f, hi - lo); };
    // Felt: a box in the world, square to it (the frame is turned by quarter turns).
    const auto felt = [&](float floor, const glm::vec3& lo, const glm::vec3& hi)
    {
        const glm::vec3 a = frame.At(floor, lo);
        const glm::vec3 b = frame.At(floor, hi);
        Box(out, Kind::Furniture, glm::min(a, b), glm::max(a, b));
    };
    const glm::quat across = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::quat along = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));

    // The ladder: two stiles from the floor to a metre over the one above, joined over the top; rungs a foot apart up to that
    // floor; held off the wall by brackets.
    constexpr float kStile = 0.25f;
    constexpr float kRungOut = 0.15f;
    const float reach = kStorey + 1.0f;
    for (const float side : {-1.0f, 1.0f})
    {
        const float x = side * kStile;
        box(Kind::Steel, base, {x - 0.03f, 0.0f, -kRungOut - 0.03f}, {x + 0.03f, reach, -kRungOut + 0.03f});
        for (const float y : {0.5f, 2.1f, kStorey + 0.75f})
        {
            box(Kind::DarkSteel, base, {x - 0.02f, y - 0.03f, -kRungOut + 0.03f}, {x + 0.02f, y + 0.03f, kHalfWall * 0.5f});
            box(Kind::DarkSteel, base, {x - 0.05f, y - 0.07f, -0.012f}, {x + 0.05f, y + 0.07f, kHalfWall * 0.5f});
        }
        box(Kind::DarkSteel, base, {x - 0.06f, 0.0f, -kRungOut - 0.06f}, {x + 0.06f, 0.012f, -kRungOut + 0.06f});
    }
    add(Form::Cylinder, Kind::Steel, base, {0.0f, reach - 0.03f, -kRungOut}, {0.05f, kStile * 2.0f + 0.06f, 0.05f}, across);
    for (float y = 0.3f; y < kStorey - 0.1f; y += 0.3f)
    {
        add(Form::Cylinder, Kind::Steel, base, {0.0f, y, -kRungOut}, {0.032f, kStile * 2.0f - 0.04f, 0.032f}, across);
    }

    // Up there, the hatch's lip: a painted strip on the floor along its open front.
    box(Kind::Paint, top, {-kHatchHalf, 0.0f, -kHatchOut - 0.1f}, {kHatchHalf, 0.004f, -kHatchOut});
    // A rail on one side: two posts, a top rail and a knee rail, a toe board along the edge.
    const float rail = kHatchHalf + 0.06f;
    for (const float z : {-0.08f, -kHatchOut + 0.04f})
    {
        add(Form::Cylinder, Kind::Paint, top, {rail, 0.53f, z}, {0.045f, 1.06f, 0.045f});
    }
    for (const float y : {1.06f, 0.55f})
    {
        add(Form::Cylinder, Kind::Paint, top, {rail, y, (-0.08f - kHatchOut + 0.04f) * 0.5f}, {0.042f, kHatchOut - 0.04f, 0.042f}, along);
    }
    box(Kind::Paint, top, {rail - 0.012f, 0.0f, -kHatchOut + 0.02f}, {rail + 0.012f, 0.1f, -0.03f});
    felt(top, {rail - 0.05f, 0.0f, -kHatchOut}, {rail + 0.05f, 1.1f, 0.0f});
    // The lid on the other, stood up on its hinges and held there: a chequer plate with a frame under it.
    const float lid = -kHatchHalf - 0.04f;
    box(Kind::DarkSteel, top, {lid - 0.015f, 0.04f, -kHatchOut + 0.02f}, {lid + 0.015f, kHatchHalf * 2.0f, -0.03f});
    box(Kind::Steel, top, {lid - 0.03f, 0.04f, -kHatchOut + 0.02f}, {lid - 0.015f, 0.1f, -0.03f});
    box(Kind::Paint, top, {lid - 0.03f, kHatchHalf * 2.0f - 0.06f, -kHatchOut + 0.02f}, {lid - 0.015f, kHatchHalf * 2.0f, -0.03f});
    for (const float z : {-0.25f, -kHatchOut + 0.25f})
    {
        add(Form::Cylinder, Kind::Steel, top, {lid + 0.01f, 0.03f, z}, {0.05f, 0.14f, 0.05f}, along);
    }
    felt(top, {lid - 0.05f, 0.0f, -kHatchOut}, {lid + 0.05f, kHatchHalf * 2.0f, 0.0f});

    Ladder ladder;
    ladder.foot = frame.At(base, glm::vec3(0.0f));
    ladder.out = frame.turn * glm::vec3(0.0f, 0.0f, -1.0f);
    ladder.height = kStorey;
    ladder.width = kStile * 2.0f + 0.05f;
    ladder.topInFront = true;
    ladder.topOut = kLadderTopOut;
    out.ladders.push_back(ladder);
}
} // namespace

glm::vec3 FacilityMap::ToWorld(int floor, glm::vec2 cells)
{
    return {WorldX(cells.x), Base(floor), WorldZ(cells.y)};
}

glm::vec3 FacilityMap::ToWorld(const FacilityLayout& layout, int floor, glm::vec2 cells)
{
    const OriginScope scope(layout.origin);
    return ToWorld(floor, cells);
}

glm::vec3 FacilityMap::PlacedAgainstWall(const FacilityLayout& layout, const FacilityLayout::Placed& thing, float depth)
{
    const OriginScope scope(layout.origin);
    return AgainstWall(layout, thing, depth);
}

glm::vec3 FacilityMap::ExitOutside(const FacilityLayout& layout, const FacilityLayout::Exit& exit, float distance)
{
    const glm::vec2 out = exit.side == 0 ? glm::vec2(-1.0f, 0.0f) : exit.side == 1 ? glm::vec2(1.0f, 0.0f)
                        : exit.side == 2 ? glm::vec2(0.0f, -1.0f) : glm::vec2(0.0f, 1.0f);
    const glm::vec2 centre = glm::vec2(exit.cell) + glm::vec2(0.5f) + out * (0.5f + distance / kCell);
    return ToWorld(layout, layout.ground, centre);
}

FacilityMap::Blueprint FacilityMap::Draw(const FacilityLayout& layout)
{
    const OriginScope scope(layout.origin);
    Blueprint out;
    const Openings openings = FindOpenings(layout);
    Slabs(layout, out);
    for (int f = 0; f < layout.floors; ++f)
    {
        Walls(layout, f, openings, out);
        DuctRoofs(layout, f, openings, out);
    }
    Shell(layout, out);
    FacilityOutside::Dress(layout, out);
    for (const FacilityLayout::Stairwell& well : layout.stairwells)
    {
        if (well.way == FacilityLayout::Stairwell::Way::Ladder)
        {
            LadderWell(well, out);
            continue;
        }
        const float base = Base(well.floor);
        const glm::vec2 foot = InWell(well, kFoot, kCell * 0.5f);
        out.flights.push_back({{foot.x, base, foot.y}, ClimbYaw(well)});
        // Solid under the landing at the top, down to the floor the flight starts from.
        const glm::vec2 a = InWell(well, kFoot + kFlight, kHalfWall);
        const glm::vec2 b = InWell(well, 3.0f * kCell - kHalfWall, kCell - kHalfWall);
        Box(out, Kind::Fill, {a.x, base, a.y}, {b.x, base + kClear, b.y});
    }
    Things(layout, out);
    FacilityFurniture::Furnish(layout, out);
    Doors(layout, out);
    Lamps(layout, out);
    if (layout.entranceRoom >= 0)
    {
        const FacilityLayout::Room& room = layout.rooms[static_cast<size_t>(layout.entranceRoom)];
        out.spawn = ToWorld(room.floor, glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f) + glm::vec3(0.0f, 0.5f, 0.0f);
    }
    return out;
}

void FacilityMap::Build(uint16_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights)
{
    Build(FacilityLayout::Generate(seed), seed, scene, meshes, physics, lights);
}

void FacilityMap::Build(FacilityLayout layout, uint32_t seed, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                        LevelLights* lights, const std::string& prefix, uint32_t structure)
{
    Clear(scene, physics, lights);
    m_seed = static_cast<uint16_t>(seed);
    m_layout = std::move(layout);
    const Blueprint blueprint = Draw(m_layout);

    MapBuilder builder(scene, meshes, &physics, prefix);
    m_meshLibrary = &meshes;
    builder.Track(&m_entities, &m_bodies, &m_meshHandles);
    builder.BeginBatching();
    if (structure == 0)
    {
        structure = physics.NewOverlapGroup();
    }
    const Material cladding = OutsideMaterial(Kind::Cladding, m_layout.outside);
    for (const Piece& piece : blueprint.pieces)
    {
        Transform transform;
        transform.position = piece.centre;
        transform.rotation = glm::angleAxis(piece.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        builder.SetStructure(piece.Structural() ? structure : 0);
        // (A room's furnishing is felt as its box, and drawn in its detail: FacilityFurniture.)
        if (piece.kind == Kind::Furniture)
        {
            const BodyHandle body = physics.CreateBox(piece.size * 0.5f, transform, BodyMotion::Static);
            m_bodies.push_back(body);
            continue;
        }
        builder.AddBox(NameOf(piece.kind), transform, piece.size, piece.kind == Kind::Cladding ? cladding : MaterialOf(piece.kind));
    }
    // Its outside's dressing: drawn, not collided with. What is thin on a face casts no shadow -- in a lamp's shadow map it
    // would be the face, and the two fight over which is in shadow.
    {
        // (In big cells: a building's windows and bands are many small things, and in its own cells they made hundreds of meshes.)
        builder.BeginBatching(64.0f, true);
        std::map<Kind, Material> dressed;
        const auto materialOf = [&](Kind kind) -> const Material&
        {
            const auto found = dressed.find(kind);
            return found != dressed.end() ? found->second : dressed.emplace(kind, OutsideMaterial(kind, m_layout.outside)).first->second;
        };
        for (const Kind kind : {Kind::Cladding, Kind::Glazing, Kind::Trim, Kind::Accent, Kind::Kit, Kind::Roofing})
        {
            dressed[kind] = OutsideMaterial(kind, m_layout.outside);
        }
        std::vector<const Dressing*> drawn;
        for (const std::vector<Dressing>* list : {&blueprint.dressing, &blueprint.furnishing})
        {
            for (const Dressing& each : *list)
            {
                drawn.push_back(&each);
            }
        }
        for (const Dressing* each : drawn)
        {
            const Dressing& dressing = *each;
            Transform transform;
            transform.position = dressing.centre;
            transform.rotation = dressing.rotation;
            const glm::vec3& s = dressing.size;
            MeshData mesh;
            switch (dressing.form)
            {
            case Dressing::Form::Box: mesh = Primitives::Box(s); break;
            case Dressing::Form::Cylinder: mesh = Primitives::Cylinder(s.x * 0.5f, s.y, s.x > 1.0f ? 32 : 14); break;
            case Dressing::Form::Cone: mesh = Primitives::Frustum(s.x * 0.5f, s.z * 0.5f, s.y, 14); break;
            case Dressing::Form::Dome: mesh = FacilityOutside::Dome(s.x, s.y); break;
            case Dressing::Form::Dish: mesh = FacilityOutside::Dish(s.x, s.y); break;
            case Dressing::Form::Ball: mesh = Primitives::Ellipsoid(s * 0.5f, 14, 9); break;
            }
            builder.SetShadows(std::min({s.x, s.y, s.z}) > 0.15f || dressing.form != Dressing::Form::Box);
            builder.AddMesh(NameOf(dressing.kind), transform, mesh, materialOf(dressing.kind), false);
        }
        builder.SetShadows(true);
    }
    // The panes in the windows open through the walls: felt, not seen -- part of the building as its walls are.
    for (const Piece& pane : blueprint.panes)
    {
        Transform transform;
        transform.position = pane.centre;
        const BodyHandle body = physics.CreateBox(pane.size * 0.5f, transform, BodyMotion::Static);
        physics.SetOverlapGroup(body, structure);
        m_bodies.push_back(body);
    }
    builder.SetStructure(structure);
    const MeshData stairs = Primitives::Stairs(kSteps, kStairWidth, kStorey / static_cast<float>(kSteps), kStepRun);
    for (const Flight& flight : blueprint.flights)
    {
        Transform transform;
        transform.position = flight.foot;
        transform.rotation = glm::angleAxis(flight.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        builder.AddMesh("facility_stairs", transform, stairs, kStairMaterial);
    }
    physics.OptimizeBroadPhase();

    if (lights != nullptr)
    {
        m_firstLight = lights->Count();
        // Every box here ends in the middle of a wall, a tenth of a metre inside it: no margin wanted.
        lights->SetBoundsMargin(0.0f);
        m_hasLights = true;
        const glm::vec3 down{0.0f, -1.0f, 0.0f};
        std::vector<int> made(blueprint.lamps.size(), -1);
        for (size_t i = 0; i < blueprint.lamps.size(); ++i)
        {
            const Lamp& lamp = blueprint.lamps[i];
            if (lamp.copyOf >= 0)
            {
                made[i] = lights->AddCopy(made[static_cast<size_t>(lamp.copyOf)], lamp.boundsMin, lamp.boundsMax);
                continue;
            }
            if (lamp.spillOf >= 0)
            {
                made[i] = lights->AddSpill(made[static_cast<size_t>(lamp.spillOf)], lamp.position, lamp.direction, lamp.boundsMin,
                                           lamp.boundsMax);
                continue;
            }
            made[i] = lights->Add(scene, meshes, lamp.kind, lamp.mood, lamp.position, down, lamp.circuit,
                                  Mix(static_cast<uint32_t>(seed), static_cast<uint32_t>(i)) | 1u, lamp.range);
            lights->Bound(made[i], lamp.boundsMin, lamp.boundsMax);
        }
        lights->SetBoundsMargin(0.05f);
    }

    m_placements = blueprint.placements;
    m_spawn = blueprint.spawn;
    m_ladders = blueprint.ladders;
    m_spawnYaw = blueprint.spawnYaw;
    m_built = true;
    for (const FacilityLayout::Room& room : m_layout.rooms)
    {
        const glm::vec3 middle = ToWorld(m_layout, room.floor, glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f);
        PRED_LOG_DEBUG(Gameplay, "Room: {} on floor {}, its middle at {:.1f} {:.1f} {:.1f}, {} by {} cells", FacilityLayout::PurposeName(room.purpose), room.floor, middle.x, middle.y, middle.z, room.max.x - room.min.x + 1, room.max.y - room.min.y + 1);
    }
    int locked = 0;
    for (const FacilityLayout::Door& door : m_layout.doors)
    {
        locked += door.locked ? 1 : 0;
    }
    PRED_LOG_INFO(Gameplay,
                  "Building {}: {} floors, {} rooms, {} doorways ({} locked), {} stairwells ({} ladders), {} ducts, {} lamps; "
                  "{} solid pieces",
                  seed, m_layout.floors, m_layout.rooms.size(), m_layout.doors.size(), locked, m_layout.stairwells.size(), blueprint.ladders.size(),
                  m_layout.ducts.size(), blueprint.lamps.size(), blueprint.pieces.size() + blueprint.flights.size());
}

void FacilityMap::Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights)
{
    for (const Entity entity : m_entities)
    {
        scene.Destroy(entity);
    }
    if (m_meshLibrary != nullptr)
    {
        for (const MeshHandle mesh : m_meshHandles)
        {
            m_meshLibrary->Release(mesh);
        }
    }
    m_meshHandles.clear();
    for (const BodyHandle body : m_bodies)
    {
        physics.DestroyBody(body);
    }
    m_entities.clear();
    m_bodies.clear();
    if (lights != nullptr && m_hasLights)
    {
        lights->RemoveFrom(scene, m_firstLight);
    }
    m_hasLights = false;
    m_ladders.clear();
    m_built = false;
}

} // namespace pred
