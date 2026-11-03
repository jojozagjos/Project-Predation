#include "Game/World/FacilityMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/FacilityFurniture.h"
#include "Game/World/FacilityOutside.h"
#include "Game/World/FacilityCondition.h"
#include "Game/World/FacilityServices.h"
#include "Game/World/Lettering.h"
#include "Game/World/MapBuilder.h"
#include "Game/World/SiteScatter.h"
#include "Game/World/Surfaces.h"

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <vector>
#include <cmath>
#include <map>
#include <tuple>
#include <cctype>
#include <cstdio>
#include <string>

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
constexpr float kAirlockDepth = FacilitySpec::kAirlockDepth;
constexpr float kAirlockWidth = FacilitySpec::kAirlockWidth;
constexpr float kAirlockHeight = FacilitySpec::kAirlockHeight;
constexpr float kDoorHeight = FacilitySpec::kDoorHeight;
constexpr float kLiftPit = FacilitySpec::kLiftPit;
constexpr float kArchWidth = 1.6f;
constexpr float kArchHeight = 2.5f;
// A crawlspace's mouth: as wide as the duct behind it, so it runs straight on out through the wall.
constexpr float kMouthWidth = FacilityLayout::kDuctWidth;
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
// Galvanised sheet steel: pale, dull, a little rough.
const Material kDuctMaterial = Material::Metal({0.58f, 0.6f, 0.61f}, 0.55f);
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
const Material kRockMaterial = Material::Diffuse({0.36f, 0.33f, 0.3f}, 0.95f);
const Material kRockFloorMaterial = Material::Diffuse({0.3f, 0.28f, 0.26f}, 0.95f);
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
    case Cell::Void:
        return layout.roomOf[layout.Index(f, x, z)]; // the hall under it, walled round as it is
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

// Whether a doorway is a lift's: on the edge of its shaft.
bool LiftDoor(const FacilityLayout& layout, const FacilityLayout::Door& door)
{
    const glm::ivec2 next = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
    for (const glm::ivec2 cell : {door.cell, next})
    {
        const int well = layout.WellAt(door.floor, cell.x, cell.y);
        if (well >= 0 && layout.stairwells[static_cast<size_t>(well)].way == FacilityLayout::Stairwell::Way::Lift)
        {
            return true;
        }
    }
    return false;
}

Openings FindOpenings(const FacilityLayout& layout)
{
    Openings openings;
    for (const FacilityLayout::Door& door : layout.doors)
    {
        const bool stairs = door.room < 0;
        openings[{door.floor, door.cell.x, door.cell.y, door.side}] =
            door.hasDoor ? Opening{kDoorWidth, kDoorHeight} : stairs ? Opening{kStairArchWidth, kStairArchHeight} : Opening{kArchWidth, kArchHeight};
        // A lift's doorway: its doors' (Lifts), not an arch.
        if (LiftDoor(layout, door))
        {
            openings[{door.floor, door.cell.x, door.cell.y, door.side}] = Opening{FacilitySpec::kLiftDoorWidth, FacilitySpec::kLiftDoorHeight};
        }
    }
    // The ways out: a door in the outer wall, on the edge between the cell inside and the one beyond it.
    for (const FacilityLayout::Exit& exit : layout.exits)
    {
        openings[ExitKey(exit, layout.ground)] = Opening{FacilitySpec::ExitWidth(exit), FacilitySpec::ExitHeight(exit)};
    }
    for (const FacilityLayout::Exit& tunnel : layout.tunnels)
    {
        openings[ExitKey(tunnel, layout.ground - 1)] = Opening{kDoorWidth, kDoorHeight};
    }
    // And a mine's way into the rock, on its lowest floor.
    for (const FacilityLayout::Exit& cave : layout.caves)
    {
        openings[ExitKey(cave, 0)] = Opening{kDoorWidth, kDoorHeight};
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
    // (And round a hall two storeys high, the same: its walls on up past where its ceiling would be, through where the slab over
    // it is not -- on its own storey and the one over it, Cell::Void.)
    const auto stair = [&](int x, int z)
    {
        const Cell cell = layout.At(f, x, z);
        if (cell == Cell::Stair || cell == Cell::Void)
        {
            return true;
        }
        const int room = cell == Cell::Room ? layout.roomOf[layout.Index(f, x, z)] : -1;
        return room >= 0 && layout.rooms[static_cast<size_t>(room)].tall;
    };
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
                holes.push_back({{mid - FacilitySpec::ExitWidth(exit) * 0.5f, mid + FacilitySpec::ExitWidth(exit) * 0.5f}, Base(floor) + kSill, Base(floor) + FacilitySpec::ExitHeight(exit)});
            }
        };
        cut(layout.exits, layout.ground);
        cut(layout.tunnels, layout.ground - 1);
        cut(layout.caves, 0);
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
        // (A buried part's, its roof the ground there: no parapet round it -- FacilityLayout::Massing::Buried -- only a kerb a few
        // centimetres over its roof's top, so the two are not one face.)
        const bool buried = wall.high <= layout.ground;
        const float top = Base(wall.high) + (buried ? (alongX ? 0.03f : 0.02f) : kParapet); // (along z a centimetre under: corners)
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
        if (!buried)
        {
            face(alongX, std::min(inner, skin), std::max(inner, skin), from, to, {}, Kind::Cladding, top, capTop);
        }
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
        const float top = Base(corner.high) + (corner.high <= layout.ground ? 0.03f : kParapet + kCapHeight) + kPostProud;
        const float under = Base(layout.ground) - 0.25f;
        if (foot < under)
        {
            Box(out, Kind::Foundation, {std::min(at.x, far.x), foot, std::min(at.y, far.y)}, {std::max(at.x, far.x), under, std::max(at.y, far.y)});
        }
        Box(out, Kind::Cladding, {std::min(at.x, far.x), std::max(foot, under), std::min(at.y, far.y)}, {std::max(at.x, far.x), top, std::max(at.y, far.y)});
    }
}

// The crawlspaces, lined in sheet metal (their floors are Slabs'): every duct cell filled from the top of the crawlspace to
// the ceiling, inside its walls, and bridged to the duct cells beside it; and below that its sides.
void DuctRoofs(const FacilityLayout& layout, int f, const Openings& openings, Blueprint& out)
{
    const float y0 = Base(f) + kDuct;
    const float y1 = Base(f) + kClear;
    const auto duct = [&](int x, int z) { return layout.At(f, x, z) == Cell::Duct; };
    // Whether a side of a duct cell is a mouth: the opening through the wall out of it into a room.
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
            // The duct the one size all along: a square of its width in the middle of the cell, and an arm of the same width out
            // to each side that is open -- more duct, or a mouth -- and all the rest of the cell up to its top solid sheet steel,
            // so a corner turns at its width and a junction meets at it (closing only the sides that were not open left a corner
            // a square chamber wider than the duct).
            const float cx = (WorldX(x) + WorldX(x + 1)) * 0.5f;
            const float cz = (WorldZ(z) + WorldZ(z + 1)) * 0.5f;
            const float half = FacilityLayout::kDuctWidth * 0.5f;
            const float base = Base(f);
            const auto fill = [&](float x0, float z0, float x1, float z1)
            {
                if (x1 - x0 > 0.001f && z1 - z0 > 0.001f)
                {
                    Box(out, Kind::Duct, {x0, base, z0}, {x1, y0, z1});
                }
            };
            fill(xlo, zlo, cx - half, cz - half);
            fill(cx + half, zlo, xhi, cz - half);
            fill(xlo, cz + half, cx - half, zhi);
            fill(cx + half, cz + half, xhi, zhi);
            if (!openN)
            {
                fill(cx - half, zlo, cx + half, cz - half);
            }
            if (!openS)
            {
                fill(cx - half, cz + half, cx + half, zhi);
            }
            if (!openW)
            {
                fill(xlo, cz - half, cx - half, cz + half);
            }
            if (!openE)
            {
                fill(cx + half, cz - half, xhi, cz + half);
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
constexpr float kLadderTopOut = 1.65f;

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
        std::vector<int> kind(static_cast<size_t>(w * d), 0); // 0 none, 1 floor, 2 roof, 3 a crawlspace's floor
        for (int z = 0; z < d; ++z)
        {
            for (int x = 0; x < w; ++x)
            {
                const int h = layout.Height(x, z);
                // (None over a hall two storeys high, Cell::Void: open from its floor up to the roof over the storey above.)
                kind[static_cast<size_t>(z * w + x)] = h > f ? (layout.At(f, x, z) == Cell::Duct ? 3 : layout.At(f, x, z) == Cell::Void ? 0 : 1) : h == f && h > 0 ? 2 : 0;
            }
        }
        for (const FacilityLayout::Stairwell& well : layout.stairwells)
        {
            if (well.way == FacilityLayout::Stairwell::Way::Lift && f >= well.floor && f <= well.Top())
            {
                // A lift's shaft open from its pit up through every floor it stops at; the car is its floor (Lifts).
                kind[static_cast<size_t>(well.min.y * w + well.min.x)] = 0;
                if (f == well.floor)
                {
                    const glm::vec3 lo = FacilityMap::ToWorld(f, glm::vec2(well.min));
                    const glm::vec3 hi = FacilityMap::ToWorld(f, glm::vec2(well.max + glm::ivec2(1)));
                    Slab(out, top - kLiftPit, {lo.x, lo.z}, {hi.x, hi.z});
                }
                continue;
            }
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
        // As few rectangles as a row-by-row sweep finds, floors, roofs and crawlspaces apart.
        for (const int want : {1, 2, 3})
        {
            const Kind finish = want == 3 ? Kind::Duct : want == 2 && layout.outside.roofing ? Kind::Roofing : Kind::Floor;
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
        {
            // (A crate floats, in the water standing in a flooded building: as FacilityFurniture's light things do, a third of it
            // under, drifted a little -- or pressed up under the ceiling.)
            glm::vec3 at = FacilityMap::ToWorld(thing.floor, thing.at);
            if (layout.flooded >= thing.floor)
            {
                const float water = static_cast<float>(layout.flooded - thing.floor) * FacilityLayout::kStorey + layout.floodDepth;
                const float bottom = std::min(water - thing.height * 0.35f, FacilityLayout::kClearHeight - 0.03f - thing.height);
                if (bottom > 0.03f)
                {
                    const float drift = static_cast<float>((static_cast<uint32_t>(i) * 2654435761u >> 8) & 0xFFu) / 255.0f;
                    at += glm::vec3(std::cos(drift * 6.283f) * 0.25f, bottom, std::sin(drift * 6.283f) * 0.25f);
                }
            }
            AddPiece(out, Kind::Crate, at, {thing.size.x, thing.height, thing.size.y}, thing.yaw);
            break;
        }
        case FacilityLayout::Thing::Breaker:
            break; // something to use, not part of the building: the mission puts it up (MissionProps)
        }
    }
}

// An airlock built out from a way out (FacilityLayout::Outside::airlocks): a steel vestibule, its own door at its far end opening out,
// the building's door now its inner one -- the two never open together (WorldObjects::PlacedDoor::partner). Walls, roof and floor
// felt; lined inside with steel, a lamp in its roof, a panel by the inner door to cycle it, hazard stripes across both thresholds,
// AIRLOCK over its door outside.
// Its vestibule, from a doorway in a wall on a floor: `line` the wall's line at the doorway's middle, `outward` the way it is built
// out from it, `skin` how far out from the line the wall's face is. Outside the building (a way out's): clad, on a foundation, its
// far door swinging out. Or inside it -- a clean room's (FacilityLayout::Room::clean), built into the room from its door: plain
// walls, a lower roof under the room's lamps, its far door swinging on on into the room. `inside` how wide and high it is inside,
// `doorway` its doors' (a vehicle bay's bigger: FacilityLayout::Exit::bay).
void Vestibule(const FacilityLayout& layout, int floor, glm::vec2 line, glm::vec2 outward, float skin, bool outside, glm::vec2 inside, glm::vec2 doorway, size_t inner, Blueprint& out)
{
    const float base = Base(floor);
    const glm::vec2 along{std::abs(outward.y), std::abs(outward.x)};
    const float front = skin + kAirlockDepth; // its far wall's inside face
    const float half = inside.x * 0.5f;
    const float top = base + (outside ? inside.y : inside.y - 0.2f);
    const float kThick = outside ? FacilitySpec::kAirlockWall : 0.1f;
    const Kind shell = outside ? Kind::Cladding : Kind::Wall;
    const auto world = [&](float a, float o, float y)
    {
        const glm::vec2 p = line + along * a + outward * o;
        return glm::vec3(p.x, y, p.y);
    };
    // A box from `a0` to `a1` along, `o0` to `o1` out, `y0` to `y1` up: felt (a piece of the building) or only drawn.
    const auto piece = [&](Kind kind, float a0, float a1, float o0, float o1, float y0, float y1)
    {
        const glm::vec3 lo = glm::min(world(a0, o0, y0), world(a1, o1, y1));
        const glm::vec3 hi = glm::max(world(a0, o0, y0), world(a1, o1, y1));
        AddPiece(out, kind, {(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f}, hi - lo, 0.0f);
    };
    const auto dressed = [&](Kind kind, float a0, float a1, float o0, float o1, float y0, float y1)
    {
        const glm::vec3 p = world(a0, o0, y0);
        const glm::vec3 q = world(a1, o1, y1);
        FacilityMap::Dressing box;
        box.kind = kind;
        box.centre = (p + q) * 0.5f;
        box.size = glm::abs(q - p);
        out.dressing.push_back(box);
    };
    // Its floor (down into the ground as a foundation is), its sides, its roof, and its far wall round its door.
    if (outside)
    {
        piece(Kind::Foundation, -half - kThick, half + kThick, skin, front + kThick, base - 0.6f, base);
    }
    piece(shell, -half - kThick, -half, skin, front + kThick, base, top + kThick);
    piece(shell, half, half + kThick, skin, front + kThick, base, top + kThick);
    piece(shell, -half, half, skin, front + kThick, top, top + kThick);
    const float door = doorway.x * 0.5f;
    piece(shell, -half, -door, front, front + kThick, base, top);
    piece(shell, door, half, front, front + kThick, base, top);
    piece(shell, -door, door, front, front + kThick, base + doorway.y, top);
    // Lined inside with steel.
    constexpr float kLine = 0.012f;
    dressed(Kind::Steel, -half, -half + kLine, skin + 0.01f, front - 0.01f, base + 0.02f, top - 0.02f);
    dressed(Kind::Steel, half - kLine, half, skin + 0.01f, front - 0.01f, base + 0.02f, top - 0.02f);
    dressed(Kind::Steel, -half + kLine, half - kLine, skin + 0.01f, front - 0.01f, top - kLine, top);
    dressed(Kind::Steel, -half + kLine, -door, front - kLine, front, base + 0.02f, top - kLine);
    dressed(Kind::Steel, door, half - kLine, front - kLine, front, base + 0.02f, top - kLine);
    dressed(Kind::Steel, -door, door, front - kLine, front, base + doorway.y, top - kLine);
    // And the building's wall it is built against, round the inner door's frame: steel, not the cladding of the outside.
    const float jamb = door + 0.12f;
    dressed(Kind::Steel, -half + kLine, -jamb, skin, skin + kLine, base + 0.02f, top - kLine);
    dressed(Kind::Steel, jamb, half - kLine, skin, skin + kLine, base + 0.02f, top - kLine);
    dressed(Kind::Steel, -jamb, jamb, skin, skin + kLine, base + doorway.y + 0.14f, top - kLine);
    // Its floor a steel plate; hazard stripes across both its thresholds.
    dressed(Kind::Steel, -half + kLine, half - kLine, skin + 0.01f, front - 0.01f, base, base + 0.006f);
    for (const float o : {skin + 0.06f, front - 0.18f})
    {
        for (int i = 0; i < 10; ++i)
        {
            const float a = -door + doorway.x * static_cast<float>(i) / 10.0f;
            dressed(i % 2 == 0 ? Kind::Accent : Kind::DarkSteel, a, a + doorway.x / 10.0f, o, o + 0.12f, base + 0.006f, base + 0.01f);
        }
    }
    // The panel it is cycled at, beside the inner door: a steel box, its screen lit.
    dressed(Kind::Steel, door + 0.18f, door + 0.42f, skin + 0.01f, skin + 0.07f, base + 1.15f, base + 1.55f);
    dressed(Kind::Screen, door + 0.21f, door + 0.39f, skin + 0.07f, skin + 0.075f, base + 1.3f, base + 1.5f);
    // Its name over its door, outside.
    {
        FacilityMap::Dressing words;
        words.form = FacilityMap::Dressing::Form::Text;
        words.kind = Kind::Accent;
        words.text = "AIRLOCK";
        words.centre = world(0.0f, front + kThick + 0.006f, base + doorway.y + 0.22f);
        words.size = {Lettering::Width(words.text, 0.18f), 0.18f, 0.0f};
        const glm::vec3 facing{outward.x, 0.0f, outward.y};
        words.rotation = glm::angleAxis(std::atan2(-facing.x, -facing.z), glm::vec3(0.0f, 1.0f, 0.0f));
        out.dressing.push_back(words);
    }
    // A lamp in its roof, on the ground floor's circuit, lighting it and nothing past its walls.
    {
        FacilityMap::Lamp lamp;
        lamp.position = world(0.0f, (skin + front) * 0.5f, top - 0.08f);
        lamp.kind = LightKind::Ceiling;
        lamp.circuit = 10 + floor;
        for (const FacilityLayout::Lamp& planned : layout.lamps)
        {
            if (planned.floor == floor)
            {
                lamp.circuit = planned.circuit;
                break;
            }
        }
        lamp.range = 4.0f;
        lamp.boundsMin = glm::min(world(-half, skin, base), world(half, front, top));
        lamp.boundsMax = glm::max(world(-half, skin, base), world(half, front, top));
        out.lamps.push_back(lamp);
    }
    // Its own door, in its far wall, opening out; the building's door its inner one, the two never open together.
    WorldObjects::PlacedDoor outer = out.placements.doors[inner];
    const glm::vec3 shift = world(0.0f, front + kThick * 0.5f, 0.0f) - world(0.0f, 0.0f, 0.0f);
    outer.hinge += glm::vec3(shift.x, 0.0f, shift.z);
    // (Outside, turned the other way: it swings out, as the inner one swings in. Inside, on into the room, as the inner one.)
    if (outside)
    {
        outer.openYaw = outer.closedYaw - (outer.openYaw - outer.closedYaw);
    }
    outer.style = WorldObjects::PlacedDoor::Style::Airlock;
    outer.partner = static_cast<int>(inner);
    out.placements.doors[inner].style = WorldObjects::PlacedDoor::Style::Airlock;
    out.placements.doors[inner].partner = static_cast<int>(out.placements.doors.size());
    out.placements.doors.push_back(outer);
}

// A way out's airlock (FacilityLayout::Outside::airlocks): its vestibule built out from the building's outside face.
void Airlock(const FacilityLayout& layout, const FacilityLayout::Exit& exit, size_t inner, Blueprint& out)
{
    const glm::vec2 outward = exit.side == 0 ? glm::vec2(-1.0f, 0.0f) : exit.side == 1 ? glm::vec2(1.0f, 0.0f)
                            : exit.side == 2 ? glm::vec2(0.0f, -1.0f) : glm::vec2(0.0f, 1.0f);
    const glm::vec2 line = exit.side == 0 ? glm::vec2(WorldX(exit.cell.x), WorldZ(static_cast<float>(exit.cell.y) + 0.5f))
                         : exit.side == 1 ? glm::vec2(WorldX(exit.cell.x + 1), WorldZ(static_cast<float>(exit.cell.y) + 0.5f))
                         : exit.side == 2 ? glm::vec2(WorldX(static_cast<float>(exit.cell.x) + 0.5f), WorldZ(exit.cell.y))
                                          : glm::vec2(WorldX(static_cast<float>(exit.cell.x) + 0.5f), WorldZ(exit.cell.y + 1));
    // (A vehicle bay's wide enough and high enough round its shutter: a vehicle's airlock.)
    const glm::vec2 inside = exit.bay ? glm::vec2(FacilitySpec::kBayWidth + 0.6f, FacilitySpec::kBayHeight + 0.4f) : glm::vec2(kAirlockWidth, kAirlockHeight);
    Vestibule(layout, layout.ground, line, outward, kShellThickness + kCladding - kHalfWall, true, inside,
              {FacilitySpec::ExitWidth(exit), FacilitySpec::ExitHeight(exit)}, inner, out);
}

// A clean room's (FacilityLayout::Room::clean): its vestibule built into the room from its door, the room's door its inner one.
void CleanRoomAirlock(const FacilityLayout& layout, const FacilityLayout::Door& door, size_t inner, Blueprint& out)
{
    const glm::ivec2 other = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
    const bool beyond = layout.RoomAt(door.floor, other.x, other.y) == door.room;
    const glm::vec2 into = door.side == 0 ? glm::vec2(beyond ? 1.0f : -1.0f, 0.0f) : glm::vec2(0.0f, beyond ? 1.0f : -1.0f);
    const glm::vec2 line = door.side == 0 ? glm::vec2(WorldX(door.cell.x + 1), WorldZ(static_cast<float>(door.cell.y) + 0.5f))
                                          : glm::vec2(WorldX(static_cast<float>(door.cell.x) + 0.5f), WorldZ(door.cell.y + 1));
    Vestibule(layout, door.floor, line, into, kHalfWall, false, {kAirlockWidth, kAirlockHeight}, {kDoorWidth, kDoorHeight}, inner, out);
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
        // Its kind by the room it opens into: kept locked, or a room kept shut -- the servers, the control room -- a security door;
        // the machinery's, a heavy one; any other room's own.
        {
            using Style = WorldObjects::PlacedDoor::Style;
            using P = FacilityLayout::Purpose;
            const P purpose = door.room >= 0 ? layout.rooms[static_cast<size_t>(door.room)].purpose : P::None;
            // (And where the place is worked, its workshops', stores' and warehouses' too: FacilityLayout::heavyDoors.)
            const bool works = layout.heavyDoors && (purpose == P::Workshop || purpose == P::Store || purpose == P::Warehouse);
            placed.style = door.locked || purpose == P::Servers || purpose == P::Control ? Style::Security : purpose == P::Plant || works ? Style::Heavy : Style::Room;
        }
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
        // (A clean room's: an airlock built into it from its door.)
        if (door.room >= 0 && layout.rooms[static_cast<size_t>(door.room)].clean)
        {
            CleanRoomAirlock(layout, door, out.placements.doors.size() - 1, out);
        }
    }
    // A door in each way out, and in each doorway a floor down into a passage to another building (SitePlan's tunnels).
    const auto outer = [&](const FacilityLayout::Exit& way, int at, WorldObjects::PlacedDoor::Style style)
    {
        WorldObjects::PlacedDoor placed;
        placed.width = way.bay ? FacilitySpec::kBayWidth - 0.04f : kPanelWidth;
        placed.height = way.bay ? FacilitySpec::kBayHeight - 0.02f : kPanelHeight;
        placed.style = style;
        const float y = Base(at) + kSillTop + 0.004f; // just clear of its sill
        const auto [floor, lowX, lowZ, alongZ] = ExitKey(way, at);
        (void)floor;
        if (alongZ == 0)
        {
            const float mid = WorldZ(static_cast<float>(lowZ) + 0.5f);
            placed.hinge = {WorldX(lowX + 1), y, mid - placed.width * 0.5f};
            placed.closedYaw = -glm::half_pi<float>();
            // In, towards the building: +x through the west wall, -x through the east.
            placed.openYaw = placed.closedYaw + (way.side == 0 ? swing : -swing);
        }
        else
        {
            const float mid = WorldX(static_cast<float>(lowX) + 0.5f);
            placed.hinge = {mid - placed.width * 0.5f, y, WorldZ(lowZ + 1)};
            placed.closedYaw = 0.0f;
            placed.openYaw = way.side == 2 ? -swing : swing;
        }
        // A vehicle bay's: a roller shutter, rolled up into its drum on the wall inside over the doorway, running in a channel up
        // either side of it.
        if (way.bay)
        {
            placed.rollUp = true;
            placed.openYaw = placed.closedYaw + 1.0f;
            const glm::vec2 inward = way.side == 0 ? glm::vec2(1.0f, 0.0f) : way.side == 1 ? glm::vec2(-1.0f, 0.0f)
                                   : way.side == 2 ? glm::vec2(0.0f, 1.0f) : glm::vec2(0.0f, -1.0f);
            const glm::vec2 along{std::abs(inward.y), std::abs(inward.x)};
            const glm::vec2 middle = alongZ == 0 ? glm::vec2(WorldX(lowX + 1), WorldZ(static_cast<float>(lowZ) + 0.5f))
                                                 : glm::vec2(WorldX(static_cast<float>(lowX) + 0.5f), WorldZ(lowZ + 1));
            const float base = Base(at);
            const auto inside = [&](float a0, float a1, float d0, float d1, float y0, float y1, Kind kind)
            {
                const glm::vec2 p = middle + along * a0 + inward * (kHalfWall + d0);
                const glm::vec2 q = middle + along * a1 + inward * (kHalfWall + d1);
                FacilityMap::Dressing box;
                box.kind = kind;
                box.centre = glm::vec3((p.x + q.x) * 0.5f, base + (y0 + y1) * 0.5f, (p.y + q.y) * 0.5f);
                box.size = glm::abs(glm::vec3(q.x - p.x, y1 - y0, q.y - p.y));
                out.furnishing.push_back(box);
            };
            const float half = FacilitySpec::kBayWidth * 0.5f;
            inside(-half - 0.12f, half + 0.12f, 0.004f, 0.32f, FacilitySpec::kBayHeight + 0.01f, kClear - 0.03f, Kind::DarkSteel);
            for (const float s : {-1.0f, 1.0f})
            {
                inside(s * half, s * (half + 0.07f), 0.004f, 0.11f, 0.0f, FacilitySpec::kBayHeight + 0.01f, Kind::Steel);
            }
        }
        out.placements.doors.push_back(placed);
    };
    for (const FacilityLayout::Exit& exit : layout.exits)
    {
        outer(exit, layout.ground, exit.bay ? WorldObjects::PlacedDoor::Style::Heavy : WorldObjects::PlacedDoor::Style::Exit);
        // (Through an airlock built out from it, where there is no air outside to breathe.)
        if (layout.outside.airlocks)
        {
            Airlock(layout, exit, out.placements.doors.size() - 1, out);
        }
    }
    for (const FacilityLayout::Exit& tunnel : layout.tunnels)
    {
        outer(tunnel, layout.ground - 1, WorldObjects::PlacedDoor::Style::Heavy);
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
        int fromFloor = lamp.floor;
        if (room >= 0)
        {
            low = layout.rooms[static_cast<size_t>(room)].min;
            high = layout.rooms[static_cast<size_t>(room)].max;
        }
        else if (inWell)
        {
            // (A ladder's: its own cell. A flight's: its whole well, from the landing at its foot a floor down.)
            for (const FacilityLayout::Stairwell& well : layout.stairwells)
            {
                if (cell.x >= well.min.x && cell.x <= well.max.x && cell.y >= well.min.y && cell.y <= well.max.y)
                {
                    low = well.min;
                    high = well.max;
                    fromFloor = well.way == FacilityLayout::Stairwell::Way::Flight ? well.floor : lamp.floor;
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
        placed.boundsMin = FacilityMap::ToWorld(fromFloor, glm::vec2(low)) - glm::vec3(0.0f, 0.02f, 0.0f);
        // (In a hall two storeys high, up under its roof and lighting the whole height of it.)
        const bool tall = room >= 0 && layout.rooms[static_cast<size_t>(room)].tall;
        if (tall)
        {
            placed.position.y += kStorey;
        }
        placed.boundsMax = FacilityMap::ToWorld(tall ? lamp.floor + 1 : lamp.floor, glm::vec2(high + glm::ivec2(1))) + glm::vec3(0.0f, kClear + 0.02f, 0.0f);
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
        {Kind::Duct, &kDuctMaterial, "pipe_steel", 1.2f, 0.35f},          {Kind::Pillar, &kPillarMaterial, "concrete_slab", 3.0f, 0.3f},
        {Kind::Shelf, &kShelfMaterial, "gun_metal", 1.0f, 0.2f},        {Kind::Bench, &kBenchMaterial, "worktop", 1.0f, 0.3f},
        {Kind::Crate, &kCrateMaterial, "cargo_crate", 1.5f, 0.3f},        {Kind::Cabinet, &kCabinetMaterial, "locker_metal", 1.5f, 0.2f},
        {Kind::Cladding, &kCladdingMaterial, "building_cladding", 4.0f, 0.2f}, {Kind::Sill, &kSillMaterial, "facility_stairs", 1.0f, 0.4f},
        {Kind::Foundation, &kFoundationMaterial, "concrete_slab", 3.0f, 0.3f},
        {Kind::Rock, &kRockMaterial, "rock_cliff", 4.0f, 0.55f},        {Kind::RockFloor, &kRockFloorMaterial, "rock_ground", 3.0f, 0.55f},
        {Kind::Wood, &kWoodMaterial, "tabletop", 1.2f, 0.45f},          {Kind::Cardboard, &kCardboardMaterial, "facility_ceiling", 0.6f, 0.0f},
        {Kind::Fabric, &kFabricMaterial, "fabric_cushion", 0.8f, 0.3f},  {Kind::Bedding, &kBeddingMaterial, "fabric_cushion", 0.8f, 0.15f},
        {Kind::Steel, &kSteelMaterial, "gun_metal", 1.0f, 0.2f},       {Kind::DarkSteel, &kDarkSteelMaterial, "gun_metal", 1.0f, 0.2f},
        {Kind::Paint, &kPaintMaterial, "gun_metal", 1.0f, 0.05f},     {Kind::Plastic, &kPlasticMaterial, "console_casing", 1.0f, 0.15f},
        {Kind::DarkPlastic, &kDarkPlasticMaterial, "weapon_polymer", 1.0f, 0.2f}, {Kind::Porcelain, &kPorcelainMaterial, "worktop", 1.0f, 0.05f},
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
        // Silvered glass, a little hazy: with no reflection of the room to show (only the test map's mirrors have one), a mirror
        // as sharp as a mirror gave back the sky over half of it and nothing under, black and white.
        made = Material::Metal({0.52f, 0.54f, 0.57f}, 0.32f);
        made.weathering = 0.0f;
        return made;
    case Kind::Lettering:
        // Paint, near white: on a dark plate, or straight on a wall.
        made = Material::Diffuse({0.86f, 0.86f, 0.82f}, 0.6f);
        made.weathering = 0.6f;
        return made;
    case Kind::Paper:
        made = Material::Diffuse({0.82f, 0.81f, 0.76f}, 0.9f);
        made.weathering = 0.5f;
        return made;
    case Kind::Soot:
        made = Material::Diffuse({0.03f, 0.028f, 0.026f}, 1.0f);
        made.weathering = 0.6f;
        return made;
    case Kind::Leaf:
        // Leaves as the land's are: cards of the foliage picture, cut out by it, each tinted on its own corners.
        made = Material::Diffuse(glm::vec3(1.0f), 0.7f);
        made.baseColorTexture = Surfaces::FoliageAtlas();
        made.cutout = true;
        return made;
    case Kind::Frost:
        // Rime and ice: pale, a little blue, a little glossy -- crusted as snow is, not a flat white cut out of the wall.
        made = Surfaces::Apply(Material::Diffuse({0.8f, 0.86f, 0.93f}, 0.3f), "snow_02", 1.2f, 0.6f);
        made.weathering = 0.2f;
        return made;
    case Kind::Rubble: made = Surfaces::Apply(Material::Diffuse({0.45f, 0.44f, 0.42f}, 0.95f), "concrete_slab", 1.0f, 0.3f); break;
    case Kind::ExitSign:
        // Lit from behind, green: always on, on its own battery.
        made = Material::Diffuse({0.05f, 0.35f, 0.12f}, 0.4f);
        made.emissive = {0.08f, 0.9f, 0.3f};
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
    case Kind::Rock:
        return "facility_rock";
    case Kind::RockFloor:
        return "facility_rock_floor";
    }
    return "facility";
}

// A lift's shaft (FacilityLayout::Stairwell::Way::Lift), all that stands still of it: the pit under its lowest floor lined, and on
// every floor it stops at a sill across the shaft's side of its doorway, a steel frame round the doorway, a lit indicator over it
// and the plate its call button is on (Lifts makes the car, the doors and the buttons). And the lift itself, for Lifts to run.
void LiftShaft(const FacilityLayout& layout, const FacilityLayout::Stairwell& well, Blueprint& out)
{
    using Form = FacilityMap::Dressing::Form;
    // Its own frame: the middle of the shaft, +x across its doorway, -z out through it.
    const glm::vec2 middle = InWell(well, kCell * 0.5f, kCell * 0.5f);
    const glm::quat turn = glm::angleAxis(ClimbYaw(well), glm::vec3(0.0f, 1.0f, 0.0f));
    const auto at = [&](float base, const glm::vec3& local) { return glm::vec3(middle.x, base, middle.y) + turn * local; };
    const auto solid = [&](Kind kind, float base, const glm::vec3& lo, const glm::vec3& hi)
    {
        const glm::vec3 a = at(base, lo);
        const glm::vec3 b = at(base, hi);
        Box(out, kind, glm::min(a, b), glm::max(a, b));
    };
    const auto drawn = [&](Kind kind, float base, const glm::vec3& lo, const glm::vec3& hi)
    {
        FacilityMap::Dressing dressing;
        dressing.form = Form::Box;
        dressing.kind = kind;
        dressing.centre = at(base, (lo + hi) * 0.5f);
        dressing.size = hi - lo;
        dressing.rotation = turn;
        out.furnishing.push_back(dressing);
    };
    const float edge = kCell * 0.5f;
    const float inner = edge - kHalfWall;
    const float bottom = Base(well.floor);

    // The pit's sides, in the shaft's halves of its walls, from the pit's floor (Slabs) up to the walls.
    solid(Kind::Wall, bottom, {-edge, -kLiftPit, -edge}, {edge, -kSink, -inner});
    solid(Kind::Wall, bottom, {-edge, -kLiftPit, inner}, {edge, -kSink, edge});
    solid(Kind::Wall, bottom, {-edge, -kLiftPit, -inner}, {-inner, -kSink, inner});
    solid(Kind::Wall, bottom, {inner, -kLiftPit, -inner}, {edge, -kSink, inner});

    FacilityMap::Lift lift;
    lift.centre = at(bottom, glm::vec3(0.0f));
    lift.out = turn * glm::vec3(0.0f, 0.0f, -1.0f);
    lift.lowest = well.floor;
    lift.ground = layout.ground;
    const float halfDoor = FacilitySpec::kLiftDoorWidth * 0.5f;
    const float face = -edge - kHalfWall; // the wall's face on the landing
    for (int f = well.floor; f <= well.Top(); ++f)
    {
        const float base = Base(f);
        lift.stops.push_back(base);
        // A sill over the shaft's half of the wall under the doorway (the landing's floor reaches the wall's middle), a few
        // millimetres proud.
        solid(Kind::Sill, base, {-halfDoor, -kSink, -edge}, {halfDoor, kSill * 0.5f, -inner});
        // The frame: jambs and a head, standing a little proud of the wall's face.
        const float top = FacilitySpec::kLiftDoorHeight;
        drawn(Kind::Steel, base, {-halfDoor - 0.08f, 0.0f, face - 0.02f}, {-halfDoor, top + 0.08f, face + 0.01f});
        drawn(Kind::Steel, base, {halfDoor, 0.0f, face - 0.02f}, {halfDoor + 0.08f, top + 0.08f, face + 0.01f});
        drawn(Kind::Steel, base, {-halfDoor, top, face - 0.02f}, {halfDoor, top + 0.08f, face + 0.01f});
        // The indicator over it -- a steel housing, its dark glass the floor is lit on -- and the plate beside it its call button is on.
        drawn(Kind::Steel, base, {-0.22f, top + 0.16f, face - 0.025f}, {0.22f, top + 0.34f, face + 0.01f});
        drawn(Kind::DarkPlastic, base, {-0.18f, top + 0.19f, face - 0.03f}, {0.18f, top + 0.31f, face - 0.02f});
        drawn(Kind::Steel, base, {halfDoor + 0.22f, 1.0f, face - 0.015f}, {halfDoor + 0.4f, 1.36f, face + 0.01f});
    }
    out.lifts.push_back(lift);
}

// What a floor is called on its signs: B1 (and on down) under the ground, G at it, 1 and on up over it.
std::string FloorLabel(const FacilityLayout& layout, int floor)
{
    const int over = floor - layout.ground;
    return over < 0 ? "B" + std::to_string(-over) : over == 0 ? std::string("G") : std::to_string(over);
}

std::string Upper(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

// Words on a face, `facing` out of it (flat), their middle at `at`: drawn, so far proud of whatever is behind them.
void Words(Blueprint& out, Kind kind, const std::string& text, float height, const glm::vec3& at, const glm::vec3& facing)
{
    FacilityMap::Dressing words;
    words.form = FacilityMap::Dressing::Form::Text;
    words.kind = kind;
    words.text = text;
    words.centre = at;
    words.size = {Lettering::Width(text, height), height, 0.0f};
    words.rotation = glm::angleAxis(std::atan2(-facing.x, -facing.z), glm::vec3(0.0f, 1.0f, 0.0f));
    out.furnishing.push_back(words);
}

// A flat plate on a face, its back on it: `size` across, high and thick.
void Plate(Blueprint& out, Kind kind, const glm::vec3& at, const glm::vec3& facing, const glm::vec3& size)
{
    FacilityMap::Dressing plate;
    plate.kind = kind;
    plate.centre = at + facing * (size.z * 0.5f);
    plate.size = size;
    plate.rotation = glm::angleAxis(std::atan2(-facing.x, -facing.z), glm::vec3(0.0f, 1.0f, 0.0f));
    out.furnishing.push_back(plate);
}

// What tells you where you are (all of it functional, nothing of the story): beside every room's door, on the side you come to
// it from, a plate with its number -- its floor and where it comes on that floor -- what it is, and, locked, that it takes the
// keycard; in every stairwell and ladder room, on each floor, its floor painted big on the wall; over every way out, inside,
// a lit green EXIT.
void Signage(const FacilityLayout& layout, const Openings& openings, Blueprint& out)
{
    // Each room's number on its floor, in the order the plan has them.
    std::vector<int> numberOf(layout.rooms.size(), 0);
    std::vector<int> onFloor(static_cast<size_t>(layout.floors), 0);
    for (size_t r = 0; r < layout.rooms.size(); ++r)
    {
        numberOf[r] = ++onFloor[static_cast<size_t>(layout.rooms[r].floor)];
    }
    for (const FacilityLayout::Door& door : layout.doors)
    {
        if (door.room < 0 || door.room >= static_cast<int>(layout.rooms.size()))
        {
            continue;
        }
        const glm::ivec2 a = door.cell;
        const glm::ivec2 b = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
        // On the side that is not the room's.
        const bool fromA = layout.RoomAt(door.floor, b.x, b.y) == door.room;
        const glm::ivec2 near = fromA ? a : b;
        if (!layout.Open(door.floor, near.x, near.y))
        {
            continue;
        }
        const glm::vec3 along = door.side == 0 ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 facing = (door.side == 0 ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f)) * (fromA ? -1.0f : 1.0f);
        const glm::vec3 middle = door.side == 0 ? glm::vec3(WorldX(a.x + 1), Base(door.floor), WorldZ(static_cast<float>(a.y) + 0.5f))
                                                : glm::vec3(WorldX(static_cast<float>(a.x) + 0.5f), Base(door.floor), WorldZ(a.y + 1));
        const auto found = openings.find({door.floor, a.x, a.y, door.side});
        const float hole = found != openings.end() ? found->second.width * 0.5f : kDoorWidth * 0.5f;

        const FacilityLayout::Room& room = layout.rooms[static_cast<size_t>(door.room)];
        char number[16];
        std::snprintf(number, sizeof(number), "%s.%02d", FloorLabel(layout, room.floor).c_str(), numberOf[static_cast<size_t>(door.room)]);
        const std::string what = room.clean ? std::string("CLEAN ROOM") : Upper(FacilityLayout::PurposeName(room.purpose));
        constexpr float kNumber = 0.05f;
        constexpr float kName = 0.028f;
        constexpr float kNote = 0.022f;
        const float wide = std::max({Lettering::Width(number, kNumber), Lettering::Width(what, kName),
                                     door.locked ? Lettering::Width("KEYCARD ONLY", kNote) : 0.0f}) +
                           0.06f;
        const float high = door.locked ? 0.2f : 0.15f;
        const glm::vec3 face = middle + facing * (kHalfWall + 0.001f);
        const glm::vec3 at = face + along * (hole + 0.12f + wide * 0.5f) + glm::vec3(0.0f, 1.55f, 0.0f);
        Plate(out, Kind::DarkPlastic, at, facing, {wide, high, 0.012f});
        const glm::vec3 front = at + facing * 0.0135f;
        const float top = high * 0.5f;
        Words(out, Kind::Lettering, number, kNumber, front + glm::vec3(0.0f, top - 0.025f - kNumber * 0.5f, 0.0f), facing);
        Words(out, Kind::Lettering, what, kName, front + glm::vec3(0.0f, top - 0.025f - kNumber - 0.02f - kName * 0.5f, 0.0f), facing);
        if (door.locked)
        {
            Words(out, Kind::Paint, "KEYCARD ONLY", kNote, front + glm::vec3(0.0f, -top + 0.02f + kNote * 0.5f, 0.0f), facing);
        }
    }

    // Each stairwell's and ladder room's floors, painted big where you come into it: in a stairwell on its side wall by the landing at
    // the bottom and at the top; in a ladder room on its side wall.
    for (const FacilityLayout::Stairwell& well : layout.stairwells)
    {
        if (well.way == FacilityLayout::Stairwell::Way::Lift)
        {
            continue;
        }
        const bool ladder = well.way == FacilityLayout::Stairwell::Way::Ladder;
        // The side wall at across nought, facing across the well.
        const glm::vec2 a0 = InWell(well, 0.0f, 0.0f);
        const glm::vec2 a1 = InWell(well, 0.0f, 1.0f);
        const glm::vec3 facing = glm::normalize(glm::vec3(a1.x - a0.x, 0.0f, a1.y - a0.y));
        for (int f = well.floor; f <= well.Top(); ++f)
        {
            // Along the well: at its low end's landing below, by the landing at the high end above; a ladder's by its way in on both.
            const float along = ladder || f == well.floor ? 0.55f : kFoot + kFlight + 1.4f;
            const glm::vec2 spot = InWell(well, along, kHalfWall + 0.002f);
            const glm::vec3 at{spot.x, Base(f) + 1.75f, spot.y};
            Words(out, Kind::Accent, FloorLabel(layout, f), 0.34f, at, facing);
            Words(out, Kind::Accent, ladder ? "LADDER" : "STAIRS", 0.06f, at - glm::vec3(0.0f, 0.3f, 0.0f), facing);
        }
    }

    // Over every way out, inside, a lit green EXIT: over the doors out at the ground and into the passages under it.
    const auto exitSign = [&](const FacilityLayout::Exit& exit, int floor)
    {
        const glm::vec2 outward = exit.side == 0 ? glm::vec2(-1.0f, 0.0f) : exit.side == 1 ? glm::vec2(1.0f, 0.0f)
                                : exit.side == 2 ? glm::vec2(0.0f, -1.0f) : glm::vec2(0.0f, 1.0f);
        const glm::vec2 edge = glm::vec2(exit.cell) + glm::vec2(0.5f) + outward * 0.5f;
        const glm::vec3 facing{-outward.x, 0.0f, -outward.y};
        const glm::vec3 at = glm::vec3(WorldX(edge.x), Base(floor) + kDoorHeight + 0.24f, WorldZ(edge.y)) + facing * (kHalfWall + 0.001f);
        Plate(out, Kind::ExitSign, at, facing, {0.42f, 0.16f, 0.03f});
        Words(out, Kind::Lettering, "EXIT", 0.08f, at + facing * 0.0315f, facing);
    };
    for (const FacilityLayout::Exit& exit : layout.exits)
    {
        if (!exit.bay) // (a vehicle bay is no way out marked as one)
        {
            exitSign(exit, layout.ground);
        }
    }
    for (const FacilityLayout::Exit& tunnel : layout.tunnels)
    {
        exitSign(tunnel, layout.ground - 1);
    }
}

// A ladder's cell (FacilityLayout::Stairwell::Way::Ladder): the ladder up its back wall from the floor it stands on, through the
// hatch in the floor above (cut by Slabs) and on up past it to hold on to stepping off; round the hatch up there a rail either
// side, and the way on open in front. What is climbed is the player's (Blueprint::ladders);
// what is drawn is drawn as furnishing, each rail felt as a box.
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

    // The ladder: two stiles from the floor to a metre over the one above, joined over the top; rungs a foot apart all the way
    // up, past that floor to just under the top (with a bare metre of stiles over the hatch, it looked a ladder missing its
    // steps); held off the wall by brackets.
    constexpr float kStile = 0.25f;
    constexpr float kRungOut = Ladder::kRungOut;
    const float reach = kStorey + 1.0f;
    for (const float side : {-1.0f, 1.0f})
    {
        const float x = side * kStile;
        box(Kind::Steel, base, {x - 0.03f, 0.012f, -kRungOut - 0.03f}, {x + 0.03f, reach, -kRungOut + 0.03f}); // (on its foot plate)
        for (const float y : {0.5f, 2.1f, kStorey + 0.75f})
        {
            box(Kind::DarkSteel, base, {x - 0.02f, y - 0.03f, -kRungOut + 0.03f}, {x + 0.02f, y + 0.03f, 0.0f}); // (into its plate)
            box(Kind::DarkSteel, base, {x - 0.05f, y - 0.07f, -0.012f}, {x + 0.05f, y + 0.07f, kHalfWall * 0.5f});
        }
        box(Kind::DarkSteel, base, {x - 0.06f, 0.0f, -kRungOut - 0.06f}, {x + 0.06f, 0.012f, -kRungOut + 0.06f});
    }
    add(Form::Cylinder, Kind::Steel, base, {0.0f, reach - 0.03f, -kRungOut}, {0.05f, kStile * 2.0f + 0.06f, 0.05f}, across);
    for (float y = Ladder::kRungSpacing; y < reach - 0.2f; y += Ladder::kRungSpacing)
    {
        add(Form::Cylinder, Kind::Steel, base, {0.0f, y, -kRungOut}, {0.032f, kStile * 2.0f - 0.04f, 0.032f}, across);
    }

    // Up there, the hatch's lip: a painted strip on the floor along its open front.
    box(Kind::Paint, top, {-kHatchHalf, 0.0f, -kHatchOut - 0.1f}, {kHatchHalf, 0.004f, -kHatchOut});
    // A rail either side: two posts, a top rail and a knee rail, a toe board along the edge.
    for (const float side : {-1.0f, 1.0f})
    {
        const float rail = side * (kHatchHalf + 0.06f);
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
    }

    Ladder ladder;
    ladder.foot = frame.At(base, glm::vec3(0.0f));
    ladder.out = frame.turn * glm::vec3(0.0f, 0.0f, -1.0f);
    ladder.height = kStorey;
    ladder.width = kStile * 2.0f + 0.05f;
    ladder.topInFront = true;
    ladder.topOut = kLadderTopOut;
    out.ladders.push_back(ladder);
}

// A hall two storeys high (FacilityLayout::Room::tall): a catwalk along one of its long walls at the height of the floor over it --
// a steel deck on brackets into the wall, a rail along its open side, toe boards -- widening at one end into a landing on two
// posts, a hatch in it and a ladder up the wall through that from the floor, railed as a ladder well's is. Climbed (Blueprint::
// ladders), walked and felt. At the end of whichever long wall the ladder's foot is clear there -- of doorways, vents, windows and
// whatever stands on the floor; where neither end of either is, none.
void Catwalk(const FacilityLayout& layout, size_t index, Blueprint& out)
{
    using Form = FacilityMap::Dressing::Form;
    const FacilityLayout::Room& room = layout.rooms[index];
    const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
    if (!room.tall || std::max(size.x, size.y) < 3)
    {
        return;
    }
    constexpr float kDeck = 1.2f;    // out from the wall, along it
    constexpr float kLanding = 2.2f; // out from the wall, at the ladder's end
    constexpr float kLandingLong = 2.1f;
    constexpr float kHoleFrom = 0.45f;
    constexpr float kHoleTo = 1.65f;
    constexpr float kHoleOut = 1.11f;
    constexpr float kThick = 0.08f;
    const float base = Base(room.floor);
    // The walls it could run along -- the long ones, each from either end: where along it starts, which way it runs, which way
    // is out into the room, how long it is, and the cell the ladder stands in and the one through the wall from it.
    struct Run
    {
        glm::vec2 start;
        glm::vec2 along;
        glm::vec2 into;
        float length;
        glm::ivec2 ladderCell;
        glm::ivec2 beyond;
    };
    std::vector<Run> runs;
    const bool alongX = size.x >= size.y;
    const glm::vec2 lo{WorldX(room.min.x) + kHalfWall + 0.01f, WorldZ(room.min.y) + kHalfWall + 0.01f};
    const glm::vec2 hi{WorldX(room.max.x + 1) - kHalfWall - 0.01f, WorldZ(room.max.y + 1) - kHalfWall - 0.01f};
    for (const int side : {0, 1})
    {
        for (const int end : {0, 1})
        {
            Run run;
            if (alongX)
            {
                run.start = {end == 0 ? lo.x : hi.x, side == 0 ? lo.y : hi.y};
                run.along = {end == 0 ? 1.0f : -1.0f, 0.0f};
                run.into = {0.0f, side == 0 ? 1.0f : -1.0f};
                run.length = hi.x - lo.x;
                run.ladderCell = {end == 0 ? room.min.x : room.max.x, side == 0 ? room.min.y : room.max.y};
                run.beyond = run.ladderCell + glm::ivec2(0, side == 0 ? -1 : 1);
            }
            else
            {
                run.start = {side == 0 ? lo.x : hi.x, end == 0 ? lo.y : hi.y};
                run.along = {0.0f, end == 0 ? 1.0f : -1.0f};
                run.into = {side == 0 ? 1.0f : -1.0f, 0.0f};
                run.length = hi.y - lo.y;
                run.ladderCell = {side == 0 ? room.min.x : room.max.x, end == 0 ? room.min.y : room.max.y};
                run.beyond = run.ladderCell + glm::ivec2(side == 0 ? -1 : 1, 0);
            }
            runs.push_back(run);
        }
    }
    const auto at = [](const Run& run, float a, float d) { return run.start + run.along * a + run.into * d; };
    // Clear at the ladder's end: no doorway, vent or way out within reach of the ladder or the landing's posts; not an outside wall
    // (its windows); nothing standing there.
    const auto clear = [&](const Run& run)
    {
        const glm::vec2 foot = at(run, (kHoleFrom + kHoleTo) * 0.5f, 0.0f);
        const glm::vec2 post = at(run, kLandingLong - 0.06f, kLanding - 0.06f);
        const auto near = [&](glm::vec2 p, float reach) { return glm::distance(p, foot) < reach || glm::distance(p, post) < reach * 0.7f; };
        if (run.beyond.x < 0 || run.beyond.y < 0 || run.beyond.x >= layout.width || run.beyond.y >= layout.depth ||
            layout.At(room.floor, run.beyond.x, run.beyond.y) == FacilityLayout::Cell::Outside)
        {
            return false;
        }
        for (const FacilityLayout::Door& door : layout.doors)
        {
            const glm::vec3 w = FacilityMap::ToWorld(door.floor, glm::vec2(door.cell) + glm::vec2(0.5f) + (door.side == 0 ? glm::vec2(0.5f, 0.0f) : glm::vec2(0.0f, 0.5f)));
            if (door.floor == room.floor && near({w.x, w.z}, 2.2f))
            {
                return false;
            }
        }
        for (const FacilityLayout::Exit& exit : layout.exits)
        {
            const glm::vec3 w = FacilityMap::ToWorld(layout.ground, glm::vec2(exit.cell) + glm::vec2(0.5f));
            if (room.floor == layout.ground && near({w.x, w.z}, 2.6f))
            {
                return false;
            }
        }
        for (const FacilityLayout::Duct& duct : layout.ducts)
        {
            for (const FacilityLayout::Mouth& mouth : duct.mouths)
            {
                const glm::vec3 w = FacilityMap::ToWorld(duct.floor, (glm::vec2(mouth.open) + glm::vec2(mouth.duct)) * 0.5f + glm::vec2(0.5f));
                if (duct.floor == room.floor && near({w.x, w.z}, 2.2f))
                {
                    return false;
                }
            }
        }
        for (const FacilityLayout::Fitted& fitted : layout.fittings)
        {
            const glm::vec3 w = FacilityMap::ToWorld(fitted.floor, fitted.at);
            if (fitted.floor == room.floor && near({w.x, w.z}, 2.0f))
            {
                return false;
            }
        }
        for (const FacilityLayout::Placed& thing : layout.things)
        {
            const glm::vec3 w = FacilityMap::ToWorld(thing.floor, thing.at);
            if (thing.floor == room.floor && near({w.x, w.z}, 1.6f))
            {
                return false;
            }
        }
        return true;
    };
    const size_t first = Mix(layout.seed, static_cast<uint32_t>(index) * 7u + 3u) % runs.size();
    const Run* chosen = nullptr;
    for (size_t k = 0; k < runs.size() && chosen == nullptr; ++k)
    {
        if (clear(runs[(first + k) % runs.size()]))
        {
            chosen = &runs[(first + k) % runs.size()];
        }
    }
    if (chosen == nullptr)
    {
        return;
    }
    const Run& run = *chosen;

    // In its own terms: a along the wall from its start, y up from the hall's floor, d out from the wall -- every box square to the
    // building, the wall running along x or z.
    const auto point = [&](float a, float y, float d)
    {
        const glm::vec2 p = at(run, a, d);
        return glm::vec3(p.x, base + y, p.y);
    };
    const auto draw = [&](Kind kind, glm::vec3 p, glm::vec3 q)
    {
        FacilityMap::Dressing box;
        box.kind = kind;
        box.centre = (p + q) * 0.5f;
        box.size = glm::abs(q - p);
        out.furnishing.push_back(box);
    };
    // A deck's plate: drawn as steel, felt.
    const auto plate = [&](float a0, float a1, float d0, float d1)
    {
        draw(Kind::DarkSteel, point(a0, kStorey - kThick, d0), point(a1, kStorey, d1));
        Box(out, Kind::Furniture, point(a0, kStorey - kThick, d0), point(a1, kStorey, d1));
    };
    // A rod from one point to another.
    const auto rod = [&](Kind kind, glm::vec3 p, glm::vec3 q, float thick)
    {
        const glm::vec3 length = q - p;
        FacilityMap::Dressing bar;
        bar.form = Form::Cylinder;
        bar.kind = kind;
        bar.centre = (p + q) * 0.5f;
        bar.size = {thick, glm::length(length), thick};
        bar.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(length));
        out.furnishing.push_back(bar);
    };
    // A rail along an edge, square to the wall or along it: posts, a top and a knee rail, a toe board -- felt as a low wall.
    const auto rail = [&](float a0, float d0, float a1, float d1)
    {
        const float length = std::abs(a1 - a0) + std::abs(d1 - d0);
        const int posts = std::max(1, static_cast<int>(std::ceil(length / 1.6f)));
        for (int k = 0; k <= posts; ++k)
        {
            const float t = static_cast<float>(k) / static_cast<float>(posts);
            rod(Kind::Paint, point(glm::mix(a0, a1, t), kStorey, glm::mix(d0, d1, t)), point(glm::mix(a0, a1, t), kStorey + 1.06f, glm::mix(d0, d1, t)), 0.045f);
        }
        for (const float y : {1.06f, 0.55f})
        {
            rod(Kind::Paint, point(a0, kStorey + y, d0), point(a1, kStorey + y, d1), 0.042f);
        }
        const bool across = std::abs(d1 - d0) > std::abs(a1 - a0); // (running out from the wall)
        const float wa = across ? 0.012f : 0.0f;
        const float wd = across ? 0.0f : 0.012f;
        draw(Kind::Paint, point(std::min(a0, a1) - wa, kStorey, std::min(d0, d1) - wd), point(std::max(a0, a1) + wa, kStorey + 0.1f, std::max(d0, d1) + wd));
        const float fa = across ? 0.05f : 0.0f;
        const float fd = across ? 0.0f : 0.05f;
        Box(out, Kind::Furniture, point(std::min(a0, a1) - fa, kStorey, std::min(d0, d1) - fd), point(std::max(a0, a1) + fa, kStorey + 1.1f, std::max(d0, d1) + fd));
    };

    // The deck: the landing round the hatch, then the catwalk on along the wall.
    plate(0.0f, kHoleFrom, 0.0f, kLanding);
    plate(kHoleTo, kLandingLong, 0.0f, kLanding);
    plate(kHoleFrom, kHoleTo, kHoleOut, kLanding);
    plate(kLandingLong, run.length, 0.0f, kDeck);
    // Its open edges railed: along the catwalk, the landing's front and side, and either side of the hatch.
    rail(kLandingLong, kDeck - 0.06f, run.length - 0.03f, kDeck - 0.06f);
    rail(0.03f, kLanding - 0.06f, kLandingLong - 0.06f, kLanding - 0.06f);
    rail(kLandingLong - 0.06f, kDeck - 0.06f, kLandingLong - 0.06f, kLanding - 0.12f);
    rail(kHoleFrom - 0.06f, 0.08f, kHoleFrom - 0.06f, kHoleOut - 0.04f);
    rail(kHoleTo + 0.06f, 0.08f, kHoleTo + 0.06f, kHoleOut - 0.04f);
    // Held up: a bracket into the wall every two metres or so -- an arm under the deck, a strut from low on the wall up to its end --
    // and the landing's two posts at its front corners.
    for (float a = kLandingLong + 0.6f; a < run.length - 0.3f; a += 2.0f)
    {
        draw(Kind::DarkSteel, point(a - 0.04f, kStorey - kThick - 0.08f, 0.0f), point(a + 0.04f, kStorey - kThick, kDeck - 0.05f));
        rod(Kind::DarkSteel, point(a, kStorey - 1.0f, 0.02f), point(a, kStorey - kThick - 0.06f, kDeck - 0.1f), 0.05f);
    }
    for (const float a : {0.07f, kLandingLong - 0.07f})
    {
        draw(Kind::DarkSteel, point(a - 0.05f, 0.0f, kLanding - 0.12f), point(a + 0.05f, kStorey - kThick, kLanding - 0.02f));
        Box(out, Kind::Furniture, point(a - 0.05f, 0.0f, kLanding - 0.12f), point(a + 0.05f, kStorey - kThick, kLanding - 0.02f));
    }

    // The ladder up the wall through the hatch: stiles from the floor to a metre over the deck on their foot plates, held off the
    // wall by brackets; rungs a foot apart.
    constexpr float kStile = 0.25f;
    constexpr float kRungOut = Ladder::kRungOut;
    const float middle = (kHoleFrom + kHoleTo) * 0.5f;
    for (const float side : {-1.0f, 1.0f})
    {
        const float a = middle + side * kStile;
        draw(Kind::Steel, point(a - 0.03f, 0.012f, kRungOut - 0.03f), point(a + 0.03f, kStorey + 1.0f, kRungOut + 0.03f));
        draw(Kind::DarkSteel, point(a - 0.06f, 0.0f, kRungOut - 0.06f), point(a + 0.06f, 0.012f, kRungOut + 0.06f));
        for (const float y : {0.5f, 2.1f, kStorey + 0.75f})
        {
            draw(Kind::DarkSteel, point(a - 0.02f, y - 0.03f, 0.012f), point(a + 0.02f, y + 0.03f, kRungOut - 0.03f));
            draw(Kind::DarkSteel, point(a - 0.05f, y - 0.07f, 0.0f), point(a + 0.05f, y + 0.07f, 0.012f));
        }
    }
    for (float y = Ladder::kRungSpacing; y < kStorey + 0.8f; y += Ladder::kRungSpacing)
    {
        rod(Kind::Steel, point(middle - kStile + 0.02f, y, kRungOut), point(middle + kStile - 0.02f, y, kRungOut), 0.032f);
    }
    Ladder ladder;
    ladder.foot = point(middle, 0.0f, 0.0f);
    ladder.out = glm::vec3(run.into.x, 0.0f, run.into.y);
    ladder.height = kStorey;
    ladder.width = kStile * 2.0f + 0.05f;
    ladder.topInFront = true;
    ladder.topOut = kLadderTopOut;
    out.ladders.push_back(ladder);
}

// A mine's workings (FacilityLayout::workings): the floor cut in the rock rather than built -- its walls and ceilings rock (the
// building's outside wall round it left concrete, a wall holding the ground back), its floor the rock of the drive; along every
// straight run of corridor a timber set each cell (two posts against its sides, a cap across), and rails down the middle on
// their sleepers.
void Workings(const FacilityLayout& layout, Blueprint& out)
{
    const int f = layout.workings;
    if (f < 0 || f >= layout.floors)
    {
        return;
    }
    const float base = Base(f);
    const float over = Base(f + 1);
    // The building's own pieces on that floor, in the rock's: what lies wholly within its storey.
    for (FacilityMap::Piece& piece : out.pieces)
    {
        const float low = piece.centre.y - piece.size.y * 0.5f;
        const float high = piece.centre.y + piece.size.y * 0.5f;
        if (piece.kind == Kind::Wall && low >= base - kSlab - 0.01f && high <= over + 0.01f)
        {
            piece.kind = Kind::Rock;
        }
        else if (piece.kind == Kind::Floor && std::abs(high - base) < 0.01f)
        {
            piece.kind = Kind::RockFloor;
        }
        else if (piece.kind == Kind::Ceiling && std::abs(high - (over - kFinish)) < 0.01f)
        {
            piece.kind = Kind::Rock;
        }
    }
    const auto draw = [&](Kind kind, glm::vec3 p, glm::vec3 q)
    {
        FacilityMap::Dressing box;
        box.kind = kind;
        box.centre = (p + q) * 0.5f;
        box.size = glm::abs(q - p);
        out.furnishing.push_back(box);
    };
    // Along each straight run of corridor, a cell at a time: a timber set, and the rails.
    const auto corridor = [&](int x, int z) { return x >= 0 && z >= 0 && x < layout.width && z < layout.depth && layout.At(f, x, z) == Cell::Corridor; };
    for (int z = 0; z < layout.depth; ++z)
    {
        for (int x = 0; x < layout.width; ++x)
        {
            if (!corridor(x, z))
            {
                continue;
            }
            const bool alongX = corridor(x - 1, z) && corridor(x + 1, z) && !corridor(x, z - 1) && !corridor(x, z + 1);
            const bool alongZ = corridor(x, z - 1) && corridor(x, z + 1) && !corridor(x - 1, z) && !corridor(x + 1, z);
            if (!alongX && !alongZ)
            {
                continue;
            }
            const glm::vec3 middle = FacilityMap::ToWorld(f, glm::vec2(static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f));
            const glm::vec3 along = alongX ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 across{along.z, 0.0f, along.x};
            const float side = kCell * 0.5f - kHalfWall - 0.13f; // a post's middle, out from the corridor's
            const float cap = kClear - 0.62f; // (under what runs along the ceiling)
            for (const float s : {-1.0f, 1.0f})
            {
                // (Not across a doorway out of the corridor on that side.)
                const glm::ivec2 beside = glm::ivec2(x, z) + (alongX ? glm::ivec2(0, s > 0.0f ? 1 : -1) : glm::ivec2(s > 0.0f ? 1 : -1, 0));
                const bool doorway = std::any_of(layout.doors.begin(), layout.doors.end(), [&](const FacilityLayout::Door& door)
                {
                    const glm::ivec2 other = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
                    return door.floor == f && ((door.cell == glm::ivec2(x, z) && other == beside) || (door.cell == beside && other == glm::ivec2(x, z)));
                });
                if (doorway)
                {
                    continue;
                }
                const glm::vec3 post = middle + across * (s * side);
                draw(Kind::Wood, post - across * 0.1f - along * 0.1f, post + across * 0.1f + along * 0.1f + glm::vec3(0.0f, cap, 0.0f));
            }
            draw(Kind::Wood, middle - across * (side + 0.1f) - along * 0.12f + glm::vec3(0.0f, cap, 0.0f),
                 middle + across * (side + 0.1f) + along * 0.12f + glm::vec3(0.0f, cap + 0.24f, 0.0f));
            for (float t = -kCell * 0.5f + 0.3f; t < kCell * 0.5f; t += 0.6f)
            {
                const glm::vec3 at = middle + along * t;
                draw(Kind::Wood, at - across * 0.7f - along * 0.08f + glm::vec3(0.0f, 0.002f, 0.0f), at + across * 0.7f + along * 0.08f + glm::vec3(0.0f, 0.07f, 0.0f));
            }
            for (const float s : {-1.0f, 1.0f})
            {
                const glm::vec3 rail = middle + across * (s * 0.45f);
                draw(Kind::Steel, rail - across * 0.03f - along * (kCell * 0.5f) + glm::vec3(0.0f, 0.07f, 0.0f),
                     rail + across * 0.03f + along * (kCell * 0.5f) + glm::vec3(0.0f, 0.15f, 0.0f));
            }
        }
    }
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
    // (Past an airlock built out from it, where it has one: its far door is the way out.)
    const float past = layout.outside.airlocks && distance > 0.0f ? kAirlockDepth + FacilitySpec::kAirlockWall + 0.3f : 0.0f;
    const glm::vec2 centre = glm::vec2(exit.cell) + glm::vec2(0.5f) + out * (0.5f + (distance + past) / kCell);
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
        if (well.way == FacilityLayout::Stairwell::Way::Lift)
        {
            LiftShaft(layout, well, out);
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
    for (size_t r = 0; r < layout.rooms.size(); ++r)
    {
        Catwalk(layout, r, out);
    }
    Doors(layout, out);
    Signage(layout, openings, out);
    FacilityServices::Fit(layout, out);
    FacilityCondition::Dress(layout, out);
    Lamps(layout, out);
    Workings(layout, out);
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
            case Dressing::Form::Text: mesh = Lettering::Mesh(dressing.text, s.y); break;
            case Dressing::Form::Bend: mesh = FacilityOutside::PipeBend(s.x, s.y); break;
            case Dressing::Form::Patch: mesh = FacilityCondition::PatchMesh(s.x, s.y, dressing.tint, s.z, dressing.shape); break;
            case Dressing::Form::Pane: mesh = FacilityOutside::Pane(s.x, s.y); break;
            case Dressing::Form::Shrub:
            case Dressing::Form::Tuft:
            case Dressing::Form::Sprig:
            {
                mesh = dressing.form == Dressing::Form::Sprig ? SiteScatter::Sprig(dressing.shape, dressing.tint)
                                                              : SiteScatter::Leaves(dressing.form == Dressing::Form::Shrub ? SiteScatter::Kind::Shrub
                                                                                                                            : SiteScatter::Kind::Grass,
                                                                                    static_cast<int>(dressing.shape % SiteScatter::kVariants), dressing.shape,
                                                                                    dressing.tint);
                for (MeshVertex& vertex : mesh.vertices)
                {
                    vertex.position *= s;
                }
                break;
            }
            }
            builder.SetShadows((std::min({s.x, s.y, s.z}) > 0.15f || dressing.form != Dressing::Form::Box) && dressing.form != Dressing::Form::Text &&
                               dressing.form != Dressing::Form::Patch && dressing.form != Dressing::Form::Pane && dressing.form != Dressing::Form::Shrub &&
                               dressing.form != Dressing::Form::Tuft && dressing.form != Dressing::Form::Sprig);
            // A stain on a wall, a floor or a ceiling: a mark laid on it (Material::decal), darkening what is under it, not a solid
            // colour over it. (Frost on them is a crust, drawn as one.)
            const bool mark = dressing.form == Dressing::Form::Patch && dressing.kind != Kind::Frost;
            static const Material kMark = []
            {
                Material decal;
                decal.decal = true;
                return decal;
            }();
            builder.AddMesh(NameOf(dressing.kind), transform, mesh, mark ? kMark : materialOf(dressing.kind), false);
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
    m_lifts = blueprint.lifts;
    m_spawnYaw = blueprint.spawnYaw;
    m_built = true;
    for (const FacilityLayout::Room& room : m_layout.rooms)
    {
        const glm::vec3 middle = ToWorld(m_layout, room.floor, glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f);
        PRED_LOG_DEBUG(Gameplay, "Room: {}{} on floor {}, its middle at {:.1f} {:.1f} {:.1f}, {} by {} cells", FacilityLayout::PurposeName(room.purpose), room.clean ? " (clean)" : "", room.floor, middle.x, middle.y, middle.z, room.max.x - room.min.x + 1, room.max.y - room.min.y + 1);
    }
    int locked = 0;
    for (const FacilityLayout::Door& door : m_layout.doors)
    {
        locked += door.locked ? 1 : 0;
    }
    PRED_LOG_INFO(Gameplay,
                  "Building {}: {} floors, {} rooms, {} doorways ({} locked), {} stairwells ({} ladders, {} lifts), {} ducts, {} lamps; "
                  "{} solid pieces; {}{}{}",
                  seed, m_layout.floors, m_layout.rooms.size(), m_layout.doors.size(), locked, m_layout.stairwells.size(), blueprint.ladders.size(), blueprint.lifts.size(),
                  m_layout.ducts.size(), blueprint.lamps.size(), blueprint.pieces.size() + blueprint.flights.size(),
                  m_layout.condition == FacilityLayout::Condition::Kept ? "kept" : m_layout.condition == FacilityLayout::Condition::Abandoned ? "abandoned"
                  : m_layout.condition == FacilityLayout::Condition::Hurried ? "left in a hurry" : "burnt",
                  m_layout.frozen ? ", frozen" : "", m_layout.overgrown ? ", overgrown" : "");
    for (size_t r = 0; r < m_layout.rooms.size(); ++r)
    {
        if (m_layout.rooms[r].tall)
        {
            PRED_LOG_INFO(Gameplay, "Building {}: room {} on floor {} a hall two storeys high", seed, r, m_layout.rooms[r].floor);
        }
    }
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
    m_lifts.clear();
    m_built = false;
}

} // namespace pred
