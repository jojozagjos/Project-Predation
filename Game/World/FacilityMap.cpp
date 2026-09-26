#include "Game/World/FacilityMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/MapBuilder.h"

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
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
constexpr float kHalfWall = 0.1f;
// The top of a slab, drawn as floor, over the rest of it, drawn as ceiling.
constexpr float kFinish = 0.05f;
// The holes in the walls.
constexpr float kDoorWidth = 1.2f;
constexpr float kDoorHeight = 2.2f;
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
// How far a lamp reaches, which is short of the floor below: nothing here casts a lamp's shadow, and a
// lamp that reached through the slab would light the room underneath it.
constexpr float kCeilingLampRange = 5.4f;
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

float Base(int floor)
{
    return FacilitySpec::kOrigin.y + static_cast<float>(floor) * kStorey;
}
float WorldX(float cells)
{
    return FacilitySpec::kOrigin.x + cells * kCell;
}
float WorldZ(float cells)
{
    return FacilitySpec::kOrigin.z + cells * kCell;
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
// between everything.
int Space(const FacilityLayout& layout, int f, int x, int z)
{
    switch (layout.At(f, x, z))
    {
    case Cell::Room:
    case Cell::Stair:
        return layout.roomOf[layout.Index(f, x, z)];
    case Cell::Corridor:
        return -2;
    case Cell::Duct:
        return -3; // every duct is one space: two that touch are joined
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
    std::erase_if(merged, [](const Span& s) { return s.b - s.a < 0.01f; });
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

Openings FindOpenings(const FacilityLayout& layout)
{
    Openings openings;
    for (const FacilityLayout::Door& door : layout.doors)
    {
        const bool stairs = door.room < 0;
        openings[{door.floor, door.cell.x, door.cell.y, door.side}] =
            door.hasDoor ? Opening{kDoorWidth, kDoorHeight} : stairs ? Opening{kStairArchWidth, kStairArchHeight} : Opening{kArchWidth, kArchHeight};
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
    const float y0 = Base(f);
    const float y1 = y0 + kClear;
    std::vector<bool> corner(static_cast<size_t>((w + 1) * (d + 1)), false);
    const auto cornerIndex = [&](int i, int j) { return static_cast<size_t>(j * (w + 1) + i); };

    // Along x, between row j - 1 and row j.
    for (int j = 0; j <= d; ++j)
    {
        std::vector<Span> solid;
        std::vector<Span> holes;
        std::vector<std::pair<Span, float>> lintels;
        for (int i = 0; i < w; ++i)
        {
            if (Space(layout, f, i, j - 1) == Space(layout, f, i, j))
            {
                continue;
            }
            solid.push_back({WorldX(i) - kHalfWall, WorldX(i + 1) + kHalfWall});
            corner[cornerIndex(i, j)] = true;
            corner[cornerIndex(i + 1, j)] = true;
            const auto found = openings.find({f, i, j - 1, 1});
            if (found != openings.end())
            {
                const float mid = WorldX(static_cast<float>(i) + 0.5f);
                const Span hole{mid - found->second.width * 0.5f, mid + found->second.width * 0.5f};
                holes.push_back(hole);
                lintels.emplace_back(hole, found->second.height);
            }
        }
        const float z = WorldZ(j);
        for (const Span& s : Cut(solid, holes))
        {
            Box(out, Kind::Wall, {s.a, y0, z - kHalfWall}, {s.b, y1, z + kHalfWall});
        }
        for (const auto& [hole, height] : lintels)
        {
            Box(out, Kind::Wall, {hole.a, y0 + height, z - kHalfWall}, {hole.b, y1, z + kHalfWall});
        }
    }

    // Along z, between column i - 1 and column i, stopping short of every corner a wall along x has.
    for (int i = 0; i <= w; ++i)
    {
        std::vector<Span> solid;
        std::vector<Span> holes;
        std::vector<std::pair<Span, float>> lintels;
        for (int k = 0; k < d; ++k)
        {
            if (Space(layout, f, i - 1, k) == Space(layout, f, i, k))
            {
                continue;
            }
            solid.push_back({WorldZ(k) - kHalfWall, WorldZ(k + 1) + kHalfWall});
            for (const int v : {k, k + 1})
            {
                if (corner[cornerIndex(i, v)])
                {
                    holes.push_back({WorldZ(v) - kHalfWall, WorldZ(v) + kHalfWall});
                }
            }
            const auto found = openings.find({f, i - 1, k, 0});
            if (found != openings.end())
            {
                const float mid = WorldZ(static_cast<float>(k) + 0.5f);
                const Span hole{mid - found->second.width * 0.5f, mid + found->second.width * 0.5f};
                holes.push_back(hole);
                lintels.emplace_back(hole, found->second.height);
            }
        }
        const float x = WorldX(i);
        for (const Span& s : Cut(solid, holes))
        {
            Box(out, Kind::Wall, {x - kHalfWall, y0, s.a}, {x + kHalfWall, y1, s.b});
        }
        for (const auto& [hole, height] : lintels)
        {
            Box(out, Kind::Wall, {x - kHalfWall, y0 + height, hole.a}, {x + kHalfWall, y1, hole.b});
        }
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

void Slab(Blueprint& out, float top, glm::vec2 a, glm::vec2 b)
{
    Box(out, Kind::Floor, {a.x, top - kFinish, a.y}, {b.x, top, b.y});
    Box(out, Kind::Ceiling, {a.x, top - kSlab, a.y}, {b.x, top - kFinish, b.y});
}

// Every floor's slab, and the roof over the top one: whole, but for the hole each stairwell climbs
// through, and the landing the flight arrives at.
void Slabs(const FacilityLayout& layout, Blueprint& out)
{
    const int w = layout.width;
    const int d = layout.depth;
    for (int f = 0; f <= layout.floors; ++f)
    {
        const float top = Base(f);
        std::vector<bool> present(static_cast<size_t>(w * d), true);
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
                    present[static_cast<size_t>(z * w + x)] = false;
                }
            }
            Slab(out, top, InWell(well, kFoot + kFlight, 0.0f), InWell(well, 3.0f * kCell, 2.0f * kCell));
        }
        // As few rectangles as a row-by-row sweep finds.
        std::vector<bool> used(present.size(), false);
        const auto open = [&](int x, int z) { return present[static_cast<size_t>(z * w + x)] && !used[static_cast<size_t>(z * w + x)]; };
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
                Slab(out, top, {WorldX(x), WorldZ(z)}, {WorldX(x1 + 1), WorldZ(z1 + 1)});
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
        if (room >= 0)
        {
            low = layout.rooms[static_cast<size_t>(room)].min;
            high = layout.rooms[static_cast<size_t>(room)].max;
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
        out.lamps.push_back(placed);
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
            spill.position = FacilityMap::ToWorld(door.floor, edge - across * 0.15f) + glm::vec3(0.0f, 2.1f, 0.0f);
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
    switch (kind)
    {
    case Kind::Floor:
        return kFloorMaterial;
    case Kind::Ceiling:
        return kCeilingMaterial;
    case Kind::Wall:
    case Kind::Fill:
        return kWallMaterial;
    case Kind::Duct:
        return kDuctMaterial;
    case Kind::Pillar:
        return kPillarMaterial;
    case Kind::Shelf:
        return kShelfMaterial;
    case Kind::Bench:
        return kBenchMaterial;
    case Kind::Crate:
        return kCrateMaterial;
    case Kind::Cabinet:
        return kCabinetMaterial;
    }
    return kWallMaterial;
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
    }
    return "facility";
}

} // namespace

glm::vec3 FacilityMap::ToWorld(int floor, glm::vec2 cells)
{
    return {WorldX(cells.x), Base(floor), WorldZ(cells.y)};
}

FacilityMap::Blueprint FacilityMap::Draw(const FacilityLayout& layout)
{
    Blueprint out;
    const Openings openings = FindOpenings(layout);
    Slabs(layout, out);
    for (int f = 0; f < layout.floors; ++f)
    {
        Walls(layout, f, openings, out);
        DuctRoofs(layout, f, openings, out);
    }
    for (const FacilityLayout::Stairwell& well : layout.stairwells)
    {
        const float base = Base(well.floor);
        const glm::vec2 foot = InWell(well, kFoot, kCell);
        out.flights.push_back({{foot.x, base, foot.y}, ClimbYaw(well)});
        // Solid under the landing at the top, down to the floor the flight starts from.
        const glm::vec2 a = InWell(well, kFoot + kFlight, kHalfWall);
        const glm::vec2 b = InWell(well, 3.0f * kCell - kHalfWall, 2.0f * kCell - kHalfWall);
        Box(out, Kind::Fill, {a.x, base, a.y}, {b.x, base + kClear, b.y});
    }
    Things(layout, out);
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
    Clear(scene, physics, lights);
    m_seed = seed;
    m_layout = FacilityLayout::Generate(seed);
    const Blueprint blueprint = Draw(m_layout);

    MapBuilder builder(scene, meshes, &physics);
    builder.Track(&m_entities, &m_bodies);
    for (const Piece& piece : blueprint.pieces)
    {
        Transform transform;
        transform.position = piece.centre;
        transform.rotation = glm::angleAxis(piece.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        builder.AddBox(NameOf(piece.kind), transform, piece.size, MaterialOf(piece.kind));
    }
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
        m_hasLights = true;
        const glm::vec3 down{0.0f, -1.0f, 0.0f};
        std::vector<int> made(blueprint.lamps.size(), -1);
        for (size_t i = 0; i < blueprint.lamps.size(); ++i)
        {
            const Lamp& lamp = blueprint.lamps[i];
            if (lamp.spillOf >= 0)
            {
                made[i] = lights->AddSpill(made[static_cast<size_t>(lamp.spillOf)], lamp.position, lamp.direction, lamp.boundsMin,
                                           lamp.boundsMax);
                continue;
            }
            made[i] = lights->Add(scene, meshes, lamp.kind, lamp.mood, lamp.position, down, lamp.circuit,
                                  Mix(seed, static_cast<uint32_t>(i)) | 1u, lamp.range);
            lights->Bound(made[i], lamp.boundsMin, lamp.boundsMax);
        }
    }

    m_placements = blueprint.placements;
    m_spawn = blueprint.spawn;
    m_spawnYaw = blueprint.spawnYaw;
    m_built = true;
    int locked = 0;
    for (const FacilityLayout::Door& door : m_layout.doors)
    {
        locked += door.locked ? 1 : 0;
    }
    PRED_LOG_INFO(Gameplay,
                  "Facility {}: {} floors, {} rooms, {} doorways ({} locked), {} stairwells, {} ducts, {} lamps; "
                  "{} solid pieces",
                  seed, m_layout.floors, m_layout.rooms.size(), m_layout.doors.size(), locked, m_layout.stairwells.size(),
                  m_layout.ducts.size(), blueprint.lamps.size(), blueprint.pieces.size() + blueprint.flights.size());
}

void FacilityMap::Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights)
{
    for (const Entity entity : m_entities)
    {
        scene.Destroy(entity);
    }
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
    m_built = false;
}

} // namespace pred
