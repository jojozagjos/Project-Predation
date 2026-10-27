#include "Game/World/FacilityLayout.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace pred
{
namespace
{

// SplitMix64: a dozen lines, and the same sequence on every compiler, which a facility every machine
// has to build identically needs.
class Random
{
public:
    explicit Random(uint64_t seed) : m_state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    int Int(int low, int high) { return low + static_cast<int>(Next() % static_cast<uint64_t>(high - low + 1)); }
    bool Chance(float p) { return Unit() < p; }

private:
    uint64_t m_state;
};

constexpr int kWellBase = 1000;

// One end of a corridor: a room, or a stairwell's way in on the floor below or its way out above.
struct Node
{
    int floor = 0;
    glm::vec2 centre{0.0f};
    int room = -1;
    int well = -1;
    // For a stairwell, where its opening is: the cell inside, and the cell outside it.
    glm::ivec2 inside{0};
    glm::ivec2 outside{0};
};

struct Planner
{
    FacilityLayout& plan;
    Random random;
    // Cells something already stands against a wall of: one thing to a cell, or a shelf along one wall
    // and a bench along the next meet in the corner.
    std::set<std::tuple<int, int, int>> furnished;

    bool InBounds(int x, int z) const { return x >= 0 && z >= 0 && x < plan.width && z < plan.depth; }
    FacilityLayout::Cell& CellAt(int f, int x, int z) { return plan.cells[plan.Index(f, x, z)]; }
    int& OwnerAt(int f, int x, int z) { return plan.roomOf[plan.Index(f, x, z)]; }

    // Whether a rectangle, with a margin of `gap` cells round it, is all solid on these floors -- or, `againstOutside`, its
    // margin past the building's walls where it runs up to them: a room against the outside wall, so a window in that wall is
    // a window in the room.
    bool Free(int floorLow, int floorHigh, glm::ivec2 min, glm::ivec2 max, int gap, bool againstOutside = false)
    {
        for (int f = floorLow; f <= floorHigh; ++f)
        {
            for (int z = min.y - gap; z <= max.y + gap; ++z)
            {
                for (int x = min.x - gap; x <= max.x + gap; ++x)
                {
                    const bool margin = x < min.x || x > max.x || z < min.y || z > max.y;
                    const bool outside = !InBounds(x, z) || CellAt(f, x, z) == FacilityLayout::Cell::Outside;
                    if (margin && againstOutside && outside)
                    {
                        continue;
                    }
                    if (!InBounds(x, z) || CellAt(f, x, z) != FacilityLayout::Cell::Solid)
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // The building's shape (Options::massing): which cells are part of it, and how many storeys each has -- wings no narrower
    // than kArm cells, so a room and a stair fit inside one beside its walls. Cells past it are Outside on every floor, and
    // over a lower part on the floors above it. A plain block takes nothing from the draw, and is planned as it always was.
    void Shape(FacilityLayout::Massing massing)
    {
        using Massing = FacilityLayout::Massing;
        constexpr int kArm = 6;
        const int w = plan.width;
        const int d = plan.depth;
        const int top = plan.floors;
        const int lowest = plan.ground + 1; // every part of it has the ground floor
        plan.heights.assign(static_cast<size_t>(w * d), top);
        for (int attempt = 0; attempt < 16 && massing != Massing::Block; ++attempt)
        {
            std::vector<int> h(static_cast<size_t>(w * d), top);
            // A rectangle of cells measured from one side (0 -x, 1 +x, 2 -z, 3 +z): `u` along that side, `v` in from it.
            const int side = random.Int(0, 3);
            const int length = side < 2 ? d : w;
            const int deep = side < 2 ? w : d;
            const auto fromSide = [&](int s, int u0, int u1, int v0, int v1, int value)
            {
                for (int u = std::max(u0, 0); u < u1; ++u)
                {
                    for (int v = std::max(v0, 0); v < v1; ++v)
                    {
                        const int x = s == 0 ? v : s == 1 ? w - 1 - v : u;
                        const int z = s == 2 ? v : s == 3 ? d - 1 - v : u;
                        if (x >= 0 && z >= 0 && x < w && z < d)
                        {
                            int& cell = h[static_cast<size_t>(z * w + x)];
                            cell = std::min(cell, value);
                        }
                    }
                }
            };
            const auto cut = [&](int u0, int u1, int v0, int v1) { fromSide(side, u0, u1, v0, v1, 0); };
            const auto span = [&](int low, int high) { return high >= low ? random.Int(low, high) : -1; };
            bool shaped = true;
            switch (massing)
            {
            case Massing::Block: break;
            case Massing::L:
            {
                // A corner taken out: two wings at a right angle.
                const int a = span(std::max(4, length * 2 / 5), length - kArm);
                const int b = span(std::max(4, deep * 2 / 5), deep - kArm);
                shaped = a > 0 && b > 0;
                if (shaped)
                {
                    const bool far = random.Chance(0.5f);
                    cut(far ? length - a : 0, far ? length : a, 0, b);
                }
                break;
            }
            case Massing::T:
            {
                // Both corners of one side taken out: a stem out from a long bar.
                const int stem = span(kArm, length - 8);
                const int b = span(4, deep - kArm);
                shaped = stem > 0 && b > 0;
                if (shaped)
                {
                    const int left = (length - stem) / 2;
                    cut(0, left, 0, b);
                    cut(left + stem, length, 0, b);
                }
                break;
            }
            case Massing::U:
            case Massing::H:
            {
                // The middle of one side taken out -- and for an H, of the side across from it too.
                const int gap = span(4, length - 2 * kArm);
                const int b = span(4, massing == Massing::H ? (deep - kArm) / 2 : deep - kArm);
                shaped = gap > 0 && b > 0;
                if (shaped)
                {
                    const int left = (length - gap) / 2;
                    cut(left, left + gap, 0, b);
                    if (massing == Massing::H)
                    {
                        fromSide(side ^ 1, left, left + gap, 0, b, 0);
                    }
                }
                break;
            }
            case Massing::Cross:
            {
                // All four corners taken out.
                const int a = span(4, (length - kArm) / 2);
                const int b = span(4, (deep - kArm) / 2);
                shaped = a > 0 && b > 0;
                if (shaped)
                {
                    cut(0, a, 0, b);
                    cut(length - a, length, 0, b);
                    cut(0, a, deep - b, deep);
                    cut(length - a, length, deep - b, deep);
                }
                break;
            }
            case Massing::Court:
            {
                // A ring of wings round a courtyard open to the sky.
                const int t = span(kArm, std::min(w, d) / 2 - 3);
                shaped = t > 0 && w - 2 * t >= 4 && d - 2 * t >= 4;
                if (shaped)
                {
                    cut(t, length - t, t, deep - t);
                }
                break;
            }
            case Massing::Stepped:
            {
                // Stepping back as it rises: lower along one side, or two.
                shaped = top - lowest >= 1;
                const int sides = random.Int(1, 2);
                for (int k = 0; shaped && k < sides; ++k)
                {
                    const int s = k == 0 ? side : random.Int(0, 3);
                    const int band = random.Int(3, std::max(3, (s < 2 ? w : d) / 3));
                    fromSide(s, 0, s < 2 ? d : w, 0, band, random.Int(lowest, top - 1));
                }
                break;
            }
            case Massing::Tower:
            {
                // A lower hall, a storey or two, with a tower standing out of it.
                shaped = top - lowest >= 1;
                if (shaped)
                {
                    const int podium = random.Int(lowest, std::min(lowest + 1, top - 1));
                    const int tw = random.Int(std::max(kArm + 2, w * 3 / 10), std::max(kArm + 2, w * 3 / 5));
                    const int td = random.Int(std::max(kArm + 2, d * 3 / 10), std::max(kArm + 2, d * 3 / 5));
                    const int tx = random.Int(0, std::max(0, w - tw));
                    const int tz = random.Int(0, std::max(0, d - td));
                    for (int z = 0; z < d; ++z)
                    {
                        for (int x = 0; x < w; ++x)
                        {
                            if (x < tx || x >= tx + tw || z < tz || z >= tz + td)
                            {
                                h[static_cast<size_t>(z * w + x)] = podium;
                            }
                        }
                    }
                }
                break;
            }
            }
            // Now and then a wing of an L, T, U, H or cross lower than the rest.
            if (shaped && top - lowest >= 1 &&
                (massing == Massing::L || massing == Massing::T || massing == Massing::U || massing == Massing::H || massing == Massing::Cross) &&
                random.Chance(0.45f))
            {
                const int s = random.Int(0, 3);
                fromSide(s, 0, s < 2 ? d : w, 0, random.Int(kArm, std::max(kArm, (s < 2 ? w : d) / 2)), random.Int(lowest, top - 1));
            }
            if (shaped && Fits(h))
            {
                plan.heights = std::move(h);
                break;
            }
        }
        for (int f = 0; f < plan.floors; ++f)
        {
            for (int z = 0; z < d; ++z)
            {
                for (int x = 0; x < w; ++x)
                {
                    if (f >= plan.heights[static_cast<size_t>(z * w + x)])
                    {
                        CellAt(f, x, z) = FacilityLayout::Cell::Outside;
                    }
                }
            }
        }
    }

    // Whether a shape will do: every part of it at least the ground floor high; no two parts meeting only at a corner on
    // any floor (no wall can be built round that); each floor all one piece; and its top floor no mere sliver.
    bool Fits(const std::vector<int>& h) const
    {
        const int w = plan.width;
        const int d = plan.depth;
        const auto at = [&](int x, int z) { return x < 0 || z < 0 || x >= w || z >= d ? 0 : h[static_cast<size_t>(z * w + x)]; };
        for (int z = 0; z <= d; ++z)
        {
            for (int x = 0; x <= w; ++x)
            {
                const int a = at(x - 1, z - 1);
                const int b = at(x, z - 1);
                const int c = at(x - 1, z);
                const int e = at(x, z);
                if (std::min(a, e) > std::max(b, c) || std::min(b, c) > std::max(a, e))
                {
                    return false;
                }
                if (x < w && z < d && e > 0 && e <= plan.ground)
                {
                    return false;
                }
            }
        }
        for (int f = 0; f < plan.floors; ++f)
        {
            std::vector<bool> seen(static_cast<size_t>(w * d), false);
            int count = 0;
            int reached = 0;
            std::vector<glm::ivec2> todo;
            for (int z = 0; z < d; ++z)
            {
                for (int x = 0; x < w; ++x)
                {
                    if (at(x, z) > f && count++ == 0)
                    {
                        todo.push_back({x, z});
                        seen[static_cast<size_t>(z * w + x)] = true;
                    }
                }
            }
            while (!todo.empty())
            {
                const glm::ivec2 c = todo.back();
                todo.pop_back();
                ++reached;
                for (const glm::ivec2 step : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)})
                {
                    const glm::ivec2 n = c + step;
                    if (at(n.x, n.y) > f && !seen[static_cast<size_t>(n.y * w + n.x)])
                    {
                        seen[static_cast<size_t>(n.y * w + n.x)] = true;
                        todo.push_back(n);
                    }
                }
            }
            if (reached != count || count < 48)
            {
                return false;
            }
        }
        return true;
    }

    // A lift, in a building of three floors or more -- the taller, the likelier: its shaft a cell up through every floor from the
    // lowest to the top of the part it stands in, its doors on the same side on every one. Placed before the stairs, while there
    // is a column of cells free for it.
    void PlaceLift()
    {
        if (plan.floors < 3 || !random.Chance(std::min(0.45f + 0.15f * static_cast<float>(plan.floors - 3), 0.9f)))
        {
            return;
        }
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            FacilityLayout::Stairwell lift;
            lift.way = FacilityLayout::Stairwell::Way::Lift;
            lift.floor = 0;
            lift.alongX = random.Chance(0.5f);
            lift.rising = random.Chance(0.5f);
            lift.min = {random.Int(2, plan.width - 3), random.Int(2, plan.depth - 3)};
            lift.max = lift.min;
            const int height = plan.Height(lift.min.x, lift.min.y);
            if (height < 3 || !Free(0, height - 1, lift.min, lift.max, 1))
            {
                continue;
            }
            lift.levels = height - 1;
            const int index = static_cast<int>(plan.stairwells.size());
            for (int f = 0; f < height; ++f)
            {
                CellAt(f, lift.min.x, lift.min.y) = FacilityLayout::Cell::Stair;
                OwnerAt(f, lift.min.x, lift.min.y) = kWellBase + index;
            }
            plan.stairwells.push_back(lift);
            return;
        }
    }

    void PlaceStairwells()
    {
        for (int f = 0; f + 1 < plan.floors; ++f)
        {
            const int wanted = random.Int(1, 2);
            for (int attempt = 0; attempt < 200 && wanted > 0; ++attempt)
            {
                int placedHere = 0;
                for (const FacilityLayout::Stairwell& well : plan.stairwells)
                {
                    placedHere += well.floor == f && well.way != FacilityLayout::Stairwell::Way::Lift ? 1 : 0;
                }
                if (placedHere >= wanted)
                {
                    break;
                }
                FacilityLayout::Stairwell well;
                well.floor = f;
                well.alongX = random.Chance(0.5f);
                well.rising = random.Chance(0.5f);
                // The first between two floors a flight, so that whatever cannot climb a ladder can still get about; a second, as
                // often as not, a ladder through a hatch.
                if (placedHere > 0 && random.Chance(0.6f))
                {
                    well.way = FacilityLayout::Stairwell::Way::Ladder;
                }
                const bool ladder = well.way == FacilityLayout::Stairwell::Way::Ladder;
                // One cell wide, three long: a flight as wide as the way in and out of it. A ladder's one cell.
                const glm::ivec2 size = ladder ? glm::ivec2(1, 1) : well.alongX ? glm::ivec2(3, 1) : glm::ivec2(1, 3);
                // Room at the ends for the way in and the way out.
                well.min = {random.Int(2, plan.width - size.x - 2), random.Int(2, plan.depth - size.y - 2)};
                well.max = well.min + size - glm::ivec2(1);
                if (!Free(f, f + 1, well.min, well.max, 1))
                {
                    continue;
                }
                // Not right on top of another floor's stairwell either, or two flights would share a hole.
                bool clear = true;
                for (const FacilityLayout::Stairwell& other : plan.stairwells)
                {
                    clear = clear && !(other.min.x <= well.max.x + 1 && other.max.x >= well.min.x - 1 &&
                                       other.min.y <= well.max.y + 1 && other.max.y >= well.min.y - 1);
                }
                if (!clear)
                {
                    continue;
                }
                const int index = static_cast<int>(plan.stairwells.size());
                for (int ff = f; ff <= f + 1; ++ff)
                {
                    for (int z = well.min.y; z <= well.max.y; ++z)
                    {
                        for (int x = well.min.x; x <= well.max.x; ++x)
                        {
                            CellAt(ff, x, z) = FacilityLayout::Cell::Stair;
                            OwnerAt(ff, x, z) = kWellBase + index;
                        }
                    }
                }
                plan.stairwells.push_back(well);
            }
        }
    }

    void PlaceRooms()
    {
        for (int f = 0; f < plan.floors; ++f)
        {
            // As many as the floor is big -- the part of it there is on this floor, by its shape.
            const float scale = static_cast<float>(std::count_if(plan.heights.begin(), plan.heights.end(), [&](int h) { return h > f; })) / 576.0f;
            const int wanted = std::max(3, static_cast<int>(std::lround(static_cast<float>(random.Int(8, 11)) * scale)));
            int placed = 0;
            for (int attempt = 0; attempt < 400 && placed < wanted; ++attempt)
            {
                const bool hall = random.Chance(0.15f);
                const glm::ivec2 size = hall ? glm::ivec2(random.Int(5, 7), random.Int(5, 7))
                                             : glm::ivec2(random.Int(2, 5), random.Int(2, 5));
                // (Anywhere in it, against its outside walls too.)
                const glm::ivec2 min{random.Int(0, plan.width - size.x), random.Int(0, plan.depth - size.y)};
                const glm::ivec2 max = min + size - glm::ivec2(1);
                if (!Free(f, f, min, max, 1, true))
                {
                    continue;
                }
                FacilityLayout::Room room;
                room.floor = f;
                room.min = min;
                room.max = max;
                const int area = size.x * size.y;
                // (Under the ground, mostly machinery and storage, as a basement is.)
                const bool below = f < plan.ground;
                room.kind = area >= 25   ? FacilityLayout::RoomKind::Hall
                            : area >= 12 ? (random.Chance(below ? 0.75f : 0.35f) ? FacilityLayout::RoomKind::Plant : FacilityLayout::RoomKind::Lab)
                                         : (random.Chance(below ? 0.8f : 0.5f) ? FacilityLayout::RoomKind::Store : FacilityLayout::RoomKind::Office);
                room.dark = random.Chance(room.kind == FacilityLayout::RoomKind::Plant ? 0.5f : 0.18f);
                const int index = static_cast<int>(plan.rooms.size());
                for (int z = min.y; z <= max.y; ++z)
                {
                    for (int x = min.x; x <= max.x; ++x)
                    {
                        CellAt(f, x, z) = FacilityLayout::Cell::Room;
                        OwnerAt(f, x, z) = index;
                    }
                }
                plan.rooms.push_back(room);
                ++placed;
            }
        }
    }

    std::vector<Node> Nodes(int floor)
    {
        std::vector<Node> nodes;
        for (size_t r = 0; r < plan.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = plan.rooms[r];
            if (room.floor != floor)
            {
                continue;
            }
            Node node;
            node.floor = floor;
            node.room = static_cast<int>(r);
            node.centre = glm::vec2(room.min + room.max) * 0.5f;
            nodes.push_back(node);
        }
        for (size_t w = 0; w < plan.stairwells.size(); ++w)
        {
            const FacilityLayout::Stairwell& well = plan.stairwells[w];
            const bool below = well.floor == floor;
            if (floor < well.floor || floor > well.Top())
            {
                continue;
            }
            // Below, the way in is at the low end; above, the way out at the high end -- but for a ladder's, at the low end on both,
            // and a lift's on every floor it stops at.
            const bool lowEndAtMin = well.rising;
            const bool atMin = below || well.way != FacilityLayout::Stairwell::Way::Flight ? lowEndAtMin : !lowEndAtMin;
            Node node;
            node.floor = floor;
            node.well = static_cast<int>(w);
            const int across = 0;
            if (well.alongX)
            {
                const int x = atMin ? well.min.x : well.max.x;
                const int z = well.min.y + across;
                node.inside = {x, z};
                node.outside = {atMin ? x - 1 : x + 1, z};
            }
            else
            {
                const int z = atMin ? well.min.y : well.max.y;
                const int x = well.min.x + across;
                node.inside = {x, z};
                node.outside = {x, atMin ? z - 1 : z + 1};
            }
            node.centre = glm::vec2(node.outside);
            nodes.push_back(node);
        }
        return nodes;
    }

    // Records a doorway on the edge between two neighbouring cells.
    void AddDoor(int floor, glm::ivec2 a, glm::ivec2 b, int room, bool hasDoor)
    {
        FacilityLayout::Door door;
        door.floor = floor;
        if (b.x != a.x)
        {
            door.cell = b.x > a.x ? a : b;
            door.side = 0;
        }
        else
        {
            door.cell = b.y > a.y ? a : b;
            door.side = 1;
        }
        door.room = room;
        door.hasDoor = hasDoor;
        for (const FacilityLayout::Door& existing : plan.doors)
        {
            if (existing.floor == door.floor && existing.cell == door.cell && existing.side == door.side)
            {
                return;
            }
        }
        plan.doors.push_back(door);
    }

    // Whether a doorway on the edge between two cells would stand beside one there already is: within a cell and
    // a half of it, side by side or round a corner. Two doorways together read as a mistake, not a building.
    bool NearDoorway(int floor, glm::ivec2 a, glm::ivec2 b) const
    {
        const glm::vec2 middle = (glm::vec2(a) + glm::vec2(b)) * 0.5f;
        for (const FacilityLayout::Door& door : plan.doors)
        {
            if (door.floor != floor)
            {
                continue;
            }
            const glm::ivec2 other = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
            const glm::vec2 theirs = (glm::vec2(door.cell) + glm::vec2(other)) * 0.5f;
            const bool same = (door.cell == a && other == b) || (door.cell == b && other == a);
            if (!same && glm::length(theirs - middle) < 1.9f)
            {
                return true;
            }
        }
        return false;
    }

    // A way out of a room towards `towards`: a cell on its edge and the cell outside it.
    bool RoomOpening(const FacilityLayout::Room& room, glm::vec2 towards, glm::ivec2& inside, glm::ivec2& outside)
    {
        float best = 1.0e9f;
        bool found = false;
        const int index = static_cast<int>(&room - plan.rooms.data());
        // Out through a doorway the room already has, when one faces anything like the right way: a room with a
        // second door beside its first, into the same corridor, is the look of a plan nobody drew.
        for (const FacilityLayout::Door& door : plan.doors)
        {
            if (door.room != index || door.floor != room.floor)
            {
                continue;
            }
            const glm::ivec2 other = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
            const bool firstIn = OwnerAt(room.floor, door.cell.x, door.cell.y) == index &&
                                 CellAt(room.floor, door.cell.x, door.cell.y) == FacilityLayout::Cell::Room;
            const glm::ivec2 in = firstIn ? door.cell : other;
            const glm::ivec2 out = firstIn ? other : door.cell;
            const FacilityLayout::Cell there = CellAt(room.floor, out.x, out.y);
            if (there != FacilityLayout::Cell::Solid && there != FacilityLayout::Cell::Corridor)
            {
                continue;
            }
            const float score = glm::length(glm::vec2(out) - towards) - 3.0f + random.Unit() * 1.5f;
            if (score < best)
            {
                best = score;
                inside = in;
                outside = out;
                found = true;
            }
        }
        for (int z = room.min.y; z <= room.max.y; ++z)
        {
            for (int x = room.min.x; x <= room.max.x; ++x)
            {
                const glm::ivec2 steps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const glm::ivec2 step : steps)
                {
                    const glm::ivec2 out{x + step.x, z + step.y};
                    if (!InBounds(out.x, out.y))
                    {
                        continue;
                    }
                    const FacilityLayout::Cell there = CellAt(room.floor, out.x, out.y);
                    if (there != FacilityLayout::Cell::Solid && there != FacilityLayout::Cell::Corridor)
                    {
                        continue;
                    }
                    if (NearDoorway(room.floor, glm::ivec2(x, z), out))
                    {
                        continue;
                    }
                    // Not in a corner, where a door would open into the wall beside it.
                    const bool corner = (step.x != 0 && (z == room.min.y || z == room.max.y) && room.max.y > room.min.y) ||
                                        (step.y != 0 && (x == room.min.x || x == room.max.x) && room.max.x > room.min.x);
                    const float score = glm::length(glm::vec2(out) - towards) + (corner ? 3.0f : 0.0f) + random.Unit() * 1.5f;
                    if (score < best)
                    {
                        best = score;
                        inside = {x, z};
                        outside = out;
                        found = true;
                    }
                }
            }
        }
        return found;
    }

    // A corridor from one cell to another, through solid rock or existing corridor, preferring the
    // corridor that is already there.
    bool Carve(int floor, glm::ivec2 from, glm::ivec2 to)
    {
        const auto passable = [&](glm::ivec2 c)
        {
            if (!InBounds(c.x, c.y))
            {
                return false;
            }
            const FacilityLayout::Cell cell = CellAt(floor, c.x, c.y);
            return cell == FacilityLayout::Cell::Solid || cell == FacilityLayout::Cell::Corridor;
        };
        if (!passable(from) || !passable(to))
        {
            return false;
        }
        const int count = plan.width * plan.depth;
        std::vector<float> cost(static_cast<size_t>(count), 1.0e9f);
        std::vector<int> came(static_cast<size_t>(count), -1);
        const auto key = [&](glm::ivec2 c) { return c.y * plan.width + c.x; };
        using Entry = std::pair<float, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        cost[static_cast<size_t>(key(from))] = 0.0f;
        open.push({0.0f, key(from)});
        while (!open.empty())
        {
            const auto [estimate, current] = open.top();
            open.pop();
            const glm::ivec2 c{current % plan.width, current / plan.width};
            if (c == to)
            {
                break;
            }
            const glm::ivec2 steps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const glm::ivec2 step : steps)
            {
                const glm::ivec2 next = c + step;
                if (!passable(next))
                {
                    continue;
                }
                // Along corridor that is already there is cheap; new corridor dearer; and a turn dearer
                // still, so corridors run straight rather than stepping like a staircase.
                float step_cost = CellAt(floor, next.x, next.y) == FacilityLayout::Cell::Corridor ? 0.6f : 1.4f;
                const int previous = came[static_cast<size_t>(current)];
                if (previous >= 0)
                {
                    const glm::ivec2 p{previous % plan.width, previous / plan.width};
                    if (c - p != step)
                    {
                        step_cost += 0.8f;
                    }
                }
                const float total = cost[static_cast<size_t>(current)] + step_cost;
                if (total < cost[static_cast<size_t>(key(next))])
                {
                    cost[static_cast<size_t>(key(next))] = total;
                    came[static_cast<size_t>(key(next))] = current;
                    const float heuristic = static_cast<float>(std::abs(to.x - next.x) + std::abs(to.y - next.y)) * 0.6f;
                    open.push({total + heuristic, key(next)});
                }
            }
        }
        if (cost[static_cast<size_t>(key(to))] >= 1.0e9f)
        {
            return false;
        }
        for (int at = key(to); at >= 0; at = came[static_cast<size_t>(at)])
        {
            CellAt(floor, at % plan.width, at / plan.width) = FacilityLayout::Cell::Corridor;
            if (at == key(from))
            {
                break;
            }
        }
        return true;
    }

    // Joins one node to another: a doorway out of each and a corridor between, or a door straight
    // through when the two rooms are next door.
    void Join(const Node& a, const Node& b)
    {
        glm::ivec2 aIn, aOut, bIn, bOut;
        if (a.room >= 0)
        {
            if (!RoomOpening(plan.rooms[static_cast<size_t>(a.room)], b.centre, aIn, aOut))
            {
                return;
            }
        }
        else
        {
            aIn = a.inside;
            aOut = a.outside;
        }
        if (b.room >= 0)
        {
            if (!RoomOpening(plan.rooms[static_cast<size_t>(b.room)], glm::vec2(aOut), bIn, bOut))
            {
                return;
            }
        }
        else
        {
            bIn = b.inside;
            bOut = b.outside;
        }
        if (!Carve(a.floor, aOut, bOut))
        {
            return;
        }
        AddDoor(a.floor, aIn, aOut, a.room, a.room >= 0 && random.Chance(0.72f));
        AddDoor(b.floor, bIn, bOut, b.room, b.room >= 0 && random.Chance(0.72f));
    }

    void Connect()
    {
        for (int f = 0; f < plan.floors; ++f)
        {
            const std::vector<Node> nodes = Nodes(f);
            if (nodes.size() < 2)
            {
                continue;
            }
            // Every node joined to the tree the cheapest way (Prim), which reaches everything; then
            // some of the next-cheapest joins as well, which is what makes loops.
            const size_t n = nodes.size();
            std::vector<bool> inTree(n, false);
            std::vector<float> best(n, 1.0e9f);
            std::vector<int> parent(n, -1);
            best[0] = 0.0f;
            std::vector<std::pair<int, int>> edges;
            for (size_t step = 0; step < n; ++step)
            {
                int pick = -1;
                for (size_t i = 0; i < n; ++i)
                {
                    if (!inTree[i] && (pick < 0 || best[i] < best[static_cast<size_t>(pick)]))
                    {
                        pick = static_cast<int>(i);
                    }
                }
                inTree[static_cast<size_t>(pick)] = true;
                if (parent[static_cast<size_t>(pick)] >= 0)
                {
                    edges.push_back({parent[static_cast<size_t>(pick)], pick});
                }
                for (size_t i = 0; i < n; ++i)
                {
                    const float d = glm::length(nodes[i].centre - nodes[static_cast<size_t>(pick)].centre);
                    if (!inTree[i] && d < best[i])
                    {
                        best[i] = d;
                        parent[i] = pick;
                    }
                }
            }
            for (size_t i = 0; i < n; ++i)
            {
                // The second-nearest, now and then: a loop.
                int first = -1;
                int second = -1;
                float d1 = 1.0e9f;
                float d2 = 1.0e9f;
                for (size_t k = 0; k < n; ++k)
                {
                    if (k == i)
                    {
                        continue;
                    }
                    const float d = glm::length(nodes[i].centre - nodes[k].centre);
                    if (d < d1)
                    {
                        d2 = d1;
                        second = first;
                        d1 = d;
                        first = static_cast<int>(k);
                    }
                    else if (d < d2)
                    {
                        d2 = d;
                        second = static_cast<int>(k);
                    }
                }
                if (second >= 0 && random.Chance(0.45f))
                {
                    edges.push_back({static_cast<int>(i), second});
                }
            }
            for (const auto& [a, b] : edges)
            {
                Join(nodes[static_cast<size_t>(a)], nodes[static_cast<size_t>(b)]);
            }
        }
    }

    // A way out of the building in the wall facing `side` (0 -x, 1 +x, 2 -z, 3 +z): on the ground floor -- or, `underground`,
    // on the floor under it, into a passage to another building, with nothing of the building past it that way. Straight into
    // a room or a corridor that already reaches the wall, or along a corridor dug in from the wall to the nearest one.
    bool AddExit(int side, bool underground = false)
    {
        const int floor = underground ? plan.ground - 1 : plan.ground;
        std::vector<FacilityLayout::Exit>& made = underground ? plan.tunnels : plan.exits;
        const glm::ivec2 inward = side == 0 ? glm::ivec2(1, 0) : side == 1 ? glm::ivec2(-1, 0) : side == 2 ? glm::ivec2(0, 1) : glm::ivec2(0, -1);
        const glm::ivec2 along = side < 2 ? glm::ivec2(0, 1) : glm::ivec2(1, 0);
        // The open ground round the building, on the ground floor: past its edge, or beside a wing and out to the edge -- not a
        // courtyard shut in by it, where a way out would lead nowhere.
        std::vector<bool> open(static_cast<size_t>(plan.width * plan.depth), false);
        {
            std::vector<glm::ivec2> todo;
            for (int z = 0; z < plan.depth; ++z)
            {
                for (int x = 0; x < plan.width; ++x)
                {
                    if ((x == 0 || z == 0 || x == plan.width - 1 || z == plan.depth - 1) && CellAt(floor, x, z) == FacilityLayout::Cell::Outside)
                    {
                        open[static_cast<size_t>(z * plan.width + x)] = true;
                        todo.push_back({x, z});
                    }
                }
            }
            while (!todo.empty())
            {
                const glm::ivec2 c = todo.back();
                todo.pop_back();
                for (const glm::ivec2 step : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)})
                {
                    const glm::ivec2 n = c + step;
                    if (InBounds(n.x, n.y) && !open[static_cast<size_t>(n.y * plan.width + n.x)] &&
                        CellAt(floor, n.x, n.y) == FacilityLayout::Cell::Outside)
                    {
                        open[static_cast<size_t>(n.y * plan.width + n.x)] = true;
                        todo.push_back(n);
                    }
                }
            }
        }
        const auto outside = [&](glm::ivec2 c) { return !InBounds(c.x, c.y) || open[static_cast<size_t>(c.y * plan.width + c.x)]; };
        // (Under the ground, nothing of the building at all on the way out from it, so a passage can run straight off.)
        const auto clearOut = [&](glm::ivec2 c)
        {
            for (glm::ivec2 p = c - inward; InBounds(p.x, p.y); p -= inward)
            {
                if (CellAt(floor, p.x, p.y) != FacilityLayout::Cell::Outside)
                {
                    return false;
                }
            }
            return true;
        };
        const auto onWall = [&](glm::ivec2 c)
        { return InBounds(c.x, c.y) && CellAt(floor, c.x, c.y) != FacilityLayout::Cell::Outside && outside(c - inward) && (!underground || clearOut(c)); };
        // The cells inside the wall facing that way, along a straight stretch of it two cells either side: room for a door
        // and its frame, away from any corner.
        std::vector<glm::ivec2> edges;
        for (int z = 0; z < plan.depth; ++z)
        {
            for (int x = 0; x < plan.width; ++x)
            {
                const glm::ivec2 c{x, z};
                if (onWall(c) && onWall(c - along) && onWall(c + along) && onWall(c - along * 2) && onWall(c + along * 2))
                {
                    edges.push_back(c);
                }
            }
        }
        if (edges.empty())
        {
            return false;
        }
        const auto alongOf = [&](glm::ivec2 c) { return side < 2 ? c.y : c.x; };
        const auto crowded = [&](glm::ivec2 c)
        {
            for (const FacilityLayout::Exit& other : made)
            {
                if (other.side == side && std::abs(alongOf(other.cell) - alongOf(c)) < 3)
                {
                    return true;
                }
            }
            return false;
        };
        for (int attempt = 0; attempt < 32; ++attempt)
        {
            const glm::ivec2 edge = edges[static_cast<size_t>(random.Int(0, static_cast<int>(edges.size()) - 1))];
            if (crowded(edge))
            {
                continue;
            }
            const FacilityLayout::Cell at = CellAt(floor, edge.x, edge.y);
            if (NearDoorway(floor, edge, edge - inward))
            {
                continue;
            }
            if (at == FacilityLayout::Cell::Room || at == FacilityLayout::Cell::Corridor)
            {
                made.push_back({edge, side, at == FacilityLayout::Cell::Room ? OwnerAt(floor, edge.x, edge.y) : -1});
                return true;
            }
            if (at != FacilityLayout::Cell::Solid)
            {
                continue;
            }
            // Straight in, through solid, until something open.
            std::vector<glm::ivec2> dug;
            glm::ivec2 c = edge;
            bool reached = false;
            while (InBounds(c.x, c.y) && dug.size() < 12)
            {
                const FacilityLayout::Cell cell = CellAt(floor, c.x, c.y);
                if (cell == FacilityLayout::Cell::Solid)
                {
                    dug.push_back(c);
                    c += inward;
                    continue;
                }
                reached = cell == FacilityLayout::Cell::Corridor || cell == FacilityLayout::Cell::Room;
                break;
            }
            if (!reached || dug.empty())
            {
                continue;
            }
            for (const glm::ivec2 d : dug)
            {
                CellAt(floor, d.x, d.y) = FacilityLayout::Cell::Corridor;
            }
            if (CellAt(floor, c.x, c.y) == FacilityLayout::Cell::Room)
            {
                AddDoor(floor, dug.back(), c, OwnerAt(floor, c.x, c.y), random.Chance(0.72f));
            }
            made.push_back({edge, side, -1});
            return true;
        }
        // Nothing straight in: a corridor dug from the wall round to the nearest corridor there is.
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const glm::ivec2 edge = edges[static_cast<size_t>(random.Int(0, static_cast<int>(edges.size()) - 1))];
            if (crowded(edge) || CellAt(floor, edge.x, edge.y) != FacilityLayout::Cell::Solid)
            {
                continue;
            }
            glm::ivec2 nearest{-1};
            int best = 1 << 30;
            for (int z = 0; z < plan.depth; ++z)
            {
                for (int x = 0; x < plan.width; ++x)
                {
                    const int d = std::abs(x - edge.x) + std::abs(z - edge.y);
                    if (CellAt(floor, x, z) == FacilityLayout::Cell::Corridor && d < best)
                    {
                        best = d;
                        nearest = {x, z};
                    }
                }
            }
            if (nearest.x >= 0 && Carve(floor, edge, nearest))
            {
                made.push_back({edge, side, -1});
                return true;
            }
        }
        return false;
    }

    // `count` ways out, the first facing `firstSide` when there is one, each of the rest on another side.
    void AddExits(int count, int firstSide)
    {
        std::vector<int> sides{0, 1, 2, 3};
        for (int i = 3; i > 0; --i)
        {
            std::swap(sides[static_cast<size_t>(i)], sides[static_cast<size_t>(random.Int(0, i))]);
        }
        if (firstSide >= 0)
        {
            sides.erase(std::find(sides.begin(), sides.end(), firstSide));
            sides.insert(sides.begin(), firstSide);
        }
        size_t next = 0;
        for (int made = 0; made < count && next < sides.size() * 2; ++next)
        {
            made += AddExit(sides[next % sides.size()]) ? 1 : 0;
        }
        // Everybody comes in through the first, so that is the room the rest of the plan works out from.
        if (!plan.exits.empty() && plan.exits.front().room >= 0)
        {
            plan.entranceRoom = plan.exits.front().room;
            plan.rooms[static_cast<size_t>(plan.entranceRoom)].dark = false;
        }
    }

    // The long straight runs of corridor, two cells wide now and then: a hall to cross rather than a passage to
    // squeeze along. A run of four or more is widened to one side where everything along that side is solid rock
    // or corridor already, so nothing is cut into a room or a stairwell.
    void WidenCorridors()
    {
        for (int f = 0; f < plan.floors; ++f)
        {
            for (int axis = 0; axis < 2; ++axis)
            {
                const int lines = axis == 0 ? plan.depth : plan.width;
                const int length = axis == 0 ? plan.width : plan.depth;
                const auto at = [&](int along, int line) { return axis == 0 ? glm::ivec2(along, line) : glm::ivec2(line, along); };
                for (int line = 1; line + 1 < lines; ++line)
                {
                    int start = -1;
                    for (int i = 0; i <= length; ++i)
                    {
                        const glm::ivec2 c = at(i, line);
                        const bool corridor = i < length && CellAt(f, c.x, c.y) == FacilityLayout::Cell::Corridor;
                        if (corridor && start < 0)
                        {
                            start = i;
                        }
                        if (corridor || start < 0)
                        {
                            continue;
                        }
                        const int end = i - 1;
                        if (end - start + 1 >= 4 && random.Chance(0.55f))
                        {
                            const int side = random.Chance(0.5f) ? 1 : -1;
                            bool fits = true;
                            for (int k = start; k <= end && fits; ++k)
                            {
                                const glm::ivec2 n = at(k, line + side);
                                const FacilityLayout::Cell cell = InBounds(n.x, n.y) ? CellAt(f, n.x, n.y) : FacilityLayout::Cell::Room;
                                fits = cell == FacilityLayout::Cell::Solid || cell == FacilityLayout::Cell::Corridor;
                            }
                            for (int k = start; k <= end && fits; ++k)
                            {
                                const glm::ivec2 n = at(k, line + side);
                                CellAt(f, n.x, n.y) = FacilityLayout::Cell::Corridor;
                            }
                        }
                        start = -1;
                    }
                }
            }
        }
    }

    // Locks the one door of a few dead-end rooms, so the keycard is worth finding.
    void LockSome()
    {
        std::vector<int> doorsOf(plan.rooms.size(), 0);
        for (const FacilityLayout::Door& door : plan.doors)
        {
            if (door.room >= 0)
            {
                ++doorsOf[static_cast<size_t>(door.room)];
            }
        }
        int locked = 0;
        for (FacilityLayout::Door& door : plan.doors)
        {
            if (locked >= 2 || door.room < 0 || !door.hasDoor || door.room == plan.entranceRoom ||
                doorsOf[static_cast<size_t>(door.room)] != 1 || !random.Chance(0.5f))
            {
                continue;
            }
            door.locked = true;
            ++locked;
        }
    }

    enum class Goal
    {
        Room,
        Corridor,
        Duct
    };

    // Crawlspace ducts through the solid rock: from a room to another room, out to a corridor, or into
    // a duct already dug, so the ducts of a floor become a network rather than a handful of tunnels.
    void DigDucts()
    {
        for (int f = 0; f < plan.floors; ++f)
        {
            int dug = 0;
            const int wanted = random.Int(3, 5);
            for (int attempt = 0; attempt < 60 && dug < wanted; ++attempt)
            {
                const int a = random.Int(0, static_cast<int>(plan.rooms.size()) - 1);
                if (plan.rooms[static_cast<size_t>(a)].floor != f)
                {
                    continue;
                }
                const float roll = random.Unit();
                const Goal goal = roll < 0.45f ? Goal::Room : roll < 0.75f ? Goal::Corridor : Goal::Duct;
                int b = -1;
                if (goal == Goal::Room)
                {
                    b = random.Int(0, static_cast<int>(plan.rooms.size()) - 1);
                    if (b == a || plan.rooms[static_cast<size_t>(b)].floor != f)
                    {
                        continue;
                    }
                }
                if (goal == Goal::Duct &&
                    std::none_of(plan.ducts.begin(), plan.ducts.end(), [&](const FacilityLayout::Duct& d) { return d.floor == f; }))
                {
                    continue;
                }
                if (TryDuct(f, a, goal, b))
                {
                    ++dug;
                }
            }
        }
    }

    // How many steps it is to walk from one open cell to another on a floor, through doorways only, or -1 when
    // there is no way on this floor.
    int WalkDistance(int f, glm::ivec2 from, glm::ivec2 to) const
    {
        if (from.x < 0 || to.x < 0)
        {
            return -1;
        }
        const auto open = [&](glm::ivec2 c)
        {
            const FacilityLayout::Cell cell = plan.At(f, c.x, c.y);
            return cell == FacilityLayout::Cell::Room || cell == FacilityLayout::Cell::Corridor || cell == FacilityLayout::Cell::Stair;
        };
        const auto space = [&](glm::ivec2 c)
        {
            const FacilityLayout::Cell cell = plan.At(f, c.x, c.y);
            return cell == FacilityLayout::Cell::Corridor ? -2 : plan.roomOf[plan.Index(f, c.x, c.y)];
        };
        std::set<std::tuple<int, int, int>> doorways;
        for (const FacilityLayout::Door& door : plan.doors)
        {
            if (door.floor == f)
            {
                doorways.insert({door.cell.x, door.cell.y, door.side});
            }
        }
        const auto through = [&](glm::ivec2 a, glm::ivec2 b)
        {
            if (space(a) == space(b))
            {
                return true;
            }
            const glm::ivec2 low = glm::min(a, b);
            return doorways.count({low.x, low.y, a.x != b.x ? 0 : 1}) > 0;
        };
        std::vector<int> steps(static_cast<size_t>(plan.width * plan.depth), -1);
        std::queue<glm::ivec2> queue;
        steps[static_cast<size_t>(from.y * plan.width + from.x)] = 0;
        queue.push(from);
        while (!queue.empty())
        {
            const glm::ivec2 c = queue.front();
            queue.pop();
            const int here = steps[static_cast<size_t>(c.y * plan.width + c.x)];
            if (c == to)
            {
                return here;
            }
            const glm::ivec2 around[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const glm::ivec2 step : around)
            {
                const glm::ivec2 n = c + step;
                if (!InBounds(n.x, n.y) || !open(n) || steps[static_cast<size_t>(n.y * plan.width + n.x)] >= 0 || !through(c, n))
                {
                    continue;
                }
                steps[static_cast<size_t>(n.y * plan.width + n.x)] = here + 1;
                queue.push(n);
            }
        }
        return -1;
    }

    // The first open neighbour of a cell that the test accepts.
    template <typename Want>
    bool Neighbour(glm::ivec2 c, Want&& wanted, glm::ivec2& found)
    {
        const glm::ivec2 steps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const glm::ivec2 step : steps)
        {
            const glm::ivec2 n = c + step;
            if (InBounds(n.x, n.y) && wanted(n))
            {
                found = n;
                return true;
            }
        }
        return false;
    }

    // Whether a cell touches a duct already dug, sideways or corner to corner.
    bool NearDuct(int f, glm::ivec2 c)
    {
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (InBounds(c.x + dx, c.y + dz) && CellAt(f, c.x + dx, c.y + dz) == FacilityLayout::Cell::Duct)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Whether a vent's mouth here would be beside another: two vents side by side into one room are one too many.
    bool NearMouth(int f, glm::ivec2 duct, glm::ivec2 open) const
    {
        const glm::vec2 middle = (glm::vec2(duct) + glm::vec2(open)) * 0.5f;
        for (const FacilityLayout::Duct& other : plan.ducts)
        {
            if (other.floor != f)
            {
                continue;
            }
            for (const FacilityLayout::Mouth& mouth : other.mouths)
            {
                if (glm::length((glm::vec2(mouth.duct) + glm::vec2(mouth.open)) * 0.5f - middle) < 2.9f)
                {
                    return true;
                }
            }
        }
        return NearDoorway(f, duct, open);
    }

    bool TryDuct(int f, int a, Goal goal, int b)
    {
        const auto solid = [&](glm::ivec2 c) { return InBounds(c.x, c.y) && CellAt(f, c.x, c.y) == FacilityLayout::Cell::Solid; };
        // A new duct keeps clear of the ones there are, unless joining one is the point of it: side by side they read
        // as one wide duct, or two vents where one would do.
        const auto clear = [&](glm::ivec2 c) { return goal == Goal::Duct || !NearDuct(f, c); };
        const auto inA = [&](glm::ivec2 n) { return CellAt(f, n.x, n.y) == FacilityLayout::Cell::Room && OwnerAt(f, n.x, n.y) == a; };
        const auto arrived = [&](glm::ivec2 n)
        {
            switch (goal)
            {
            case Goal::Room:
                return CellAt(f, n.x, n.y) == FacilityLayout::Cell::Room && OwnerAt(f, n.x, n.y) == b;
            case Goal::Corridor:
                return CellAt(f, n.x, n.y) == FacilityLayout::Cell::Corridor;
            case Goal::Duct:
                return CellAt(f, n.x, n.y) == FacilityLayout::Cell::Duct;
            }
            return false;
        };
        const int count = plan.width * plan.depth;
        std::vector<int> came(static_cast<size_t>(count), -2);
        std::vector<int> depthOf(static_cast<size_t>(count), 0);
        std::queue<glm::ivec2> open;
        glm::ivec2 unused;
        for (int z = 0; z < plan.depth; ++z)
        {
            for (int x = 0; x < plan.width; ++x)
            {
                glm::ivec2 mouthOpen;
                if (solid({x, z}) && clear({x, z}) && Neighbour(glm::ivec2(x, z), inA, mouthOpen) &&
                    !NearMouth(f, glm::ivec2(x, z), mouthOpen) && random.Chance(0.5f))
                {
                    came[static_cast<size_t>(z * plan.width + x)] = -1;
                    open.push({x, z});
                }
            }
        }
        glm::ivec2 end{-1};
        glm::ivec2 endOpen{-1};
        while (!open.empty())
        {
            const glm::ivec2 c = open.front();
            open.pop();
            const int here = c.y * plan.width + c.x;
            // At least two cells long before it may arrive -- a duct a wall thick is only a hole -- and not
            // arriving somewhere still beside the room it set out from.
            if (depthOf[static_cast<size_t>(here)] >= 3 && Neighbour(c, arrived, endOpen) && !Neighbour(c, inA, unused) &&
                (goal == Goal::Duct || !NearMouth(f, c, endOpen)))
            {
                end = c;
                break;
            }
            if (depthOf[static_cast<size_t>(here)] >= 14)
            {
                continue;
            }
            const glm::ivec2 steps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const glm::ivec2 step : steps)
            {
                const glm::ivec2 n = c + step;
                if (solid(n) && clear(n) && came[static_cast<size_t>(n.y * plan.width + n.x)] == -2)
                {
                    came[static_cast<size_t>(n.y * plan.width + n.x)] = here;
                    depthOf[static_cast<size_t>(n.y * plan.width + n.x)] = depthOf[static_cast<size_t>(here)] + 1;
                    open.push(n);
                }
            }
        }
        if (end.x < 0)
        {
            return false;
        }
        // Worth crawling: walking from one end to the other has to be a good deal further than the duct. A duct
        // between two rooms a door apart is a hole in the wall that nobody would have dug.
        {
            int length = 0;
            for (int at = end.y * plan.width + end.x; at >= 0; at = came[static_cast<size_t>(at)])
            {
                ++length;
            }
            glm::ivec2 from{-1};
            for (int at = end.y * plan.width + end.x; at >= 0; at = came[static_cast<size_t>(at)])
            {
                if (came[static_cast<size_t>(at)] < 0)
                {
                    Neighbour(glm::ivec2(at % plan.width, at / plan.width), inA, from);
                }
            }
            const int walk = WalkDistance(f, from, endOpen);
            if (walk >= 0 && walk < length + 6)
            {
                return false;
            }
        }
        FacilityLayout::Duct duct;
        duct.floor = f;
        duct.fromRoom = a;
        duct.toRoom = goal == Goal::Room ? b : -1;
        for (int at = end.y * plan.width + end.x; at >= 0; at = came[static_cast<size_t>(at)])
        {
            duct.cells.push_back({at % plan.width, at / plan.width});
        }
        std::reverse(duct.cells.begin(), duct.cells.end());
        glm::ivec2 startOpen;
        Neighbour(duct.cells.front(), inA, startOpen);
        duct.mouths.push_back({duct.cells.front(), startOpen});
        if (goal != Goal::Duct)
        {
            duct.mouths.push_back({duct.cells.back(), endOpen});
        }
        for (const glm::ivec2 c : duct.cells)
        {
            CellAt(f, c.x, c.y) = FacilityLayout::Cell::Duct;
            OwnerAt(f, c.x, c.y) = static_cast<int>(plan.ducts.size());
        }
        plan.ducts.push_back(duct);
        return true;
    }

    // Whether a cell in a room touches a doorway, so nothing is put in front of one.
    bool ByADoor(int floor, glm::ivec2 c) const
    {
        for (const FacilityLayout::Door& door : plan.doors)
        {
            if (door.floor != floor)
            {
                continue;
            }
            const glm::ivec2 other = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
            if (std::abs(door.cell.x - c.x) + std::abs(door.cell.y - c.y) <= 1 || std::abs(other.x - c.x) + std::abs(other.y - c.y) <= 1)
            {
                return true;
            }
        }
        for (const FacilityLayout::Duct& duct : plan.ducts)
        {
            for (const FacilityLayout::Mouth& mouth : duct.mouths)
            {
                if (duct.floor == floor && std::abs(mouth.open.x - c.x) + std::abs(mouth.open.y - c.y) <= 1)
                {
                    return true;
                }
            }
        }
        // And the ways out of the building, on the ground floor.
        for (const FacilityLayout::Exit& exit : plan.exits)
        {
            if (floor == plan.ground && std::abs(exit.cell.x - c.x) + std::abs(exit.cell.y - c.y) <= 1)
            {
                return true;
            }
        }
        for (const FacilityLayout::Exit& tunnel : plan.tunnels)
        {
            if (floor == plan.ground - 1 && std::abs(tunnel.cell.x - c.x) + std::abs(tunnel.cell.y - c.y) <= 1)
            {
                return true;
            }
        }
        return false;
    }

    // Against a wall of a room, facing into it: a cell on the room's edge with solid beyond, and not
    // by a door. Out: where, and which way it faces (radians about y; 0 faces -z).
    bool AgainstWall(const FacilityLayout::Room& room, glm::vec2& at, float& yaw)
    {
        for (int attempt = 0; attempt < 30; ++attempt)
        {
            const int x = random.Int(room.min.x, room.max.x);
            const int z = random.Int(room.min.y, room.max.y);
            const glm::ivec2 steps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            const glm::ivec2 step = steps[random.Int(0, 3)];
            const glm::ivec2 out{x + step.x, z + step.y};
            if (InBounds(out.x, out.y) && CellAt(room.floor, out.x, out.y) == FacilityLayout::Cell::Room)
            {
                continue; // not an edge that way
            }
            if (ByADoor(room.floor, {x, z}) || furnished.count({room.floor, x, z}) != 0)
            {
                continue;
            }
            furnished.insert({room.floor, x, z});
            // Pushed to the wall, facing away from it: a thing faces its own -z, and turned by `yaw` about y
            // that is (-sin, -cos), which is minus the step towards the wall when yaw = atan2(step).
            at = glm::vec2(static_cast<float>(x) + 0.5f + static_cast<float>(step.x) * 0.3f,
                           static_cast<float>(z) + 0.5f + static_cast<float>(step.y) * 0.3f);
            yaw = std::atan2(static_cast<float>(step.x), static_cast<float>(step.y));
            return true;
        }
        return false;
    }

    // The panel the building's power is switched at, on a wall: in the plant room if there is one, or else a room on the
    // ground floor, or anywhere -- always somewhere that can be reached without the keycard. Planned last, so nothing
    // planned before it comes out any different for having it.
    void PlaceBreaker()
    {
        const std::vector<bool> reach = plan.Reachable(false);
        for (int pass = 0; pass < 3; ++pass)
        {
            std::vector<size_t> candidates;
            for (size_t r = 0; r < plan.rooms.size(); ++r)
            {
                const FacilityLayout::Room& room = plan.rooms[r];
                const bool fits = pass == 0 ? room.kind == FacilityLayout::RoomKind::Plant : pass == 1 ? room.floor == plan.ground : true;
                if (fits && static_cast<int>(r) != plan.nestRoom && reach[plan.Index(room.floor, room.min.x, room.min.y)])
                {
                    candidates.push_back(r);
                }
            }
            const size_t first = candidates.empty() ? 0 : static_cast<size_t>(random.Int(0, static_cast<int>(candidates.size()) - 1));
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                const FacilityLayout::Room& room = plan.rooms[candidates[(first + i) % candidates.size()]];
                glm::vec2 at;
                float yaw = 0.0f;
                if (AgainstWall(room, at, yaw))
                {
                    AddBreaker(room.floor, at, yaw);
                    return;
                }
            }
        }
        // A building so full, or so small, that no try found a free wall: every wall of every room reachable without the
        // keycard, in order, until one has nothing against it. A panel is on the wall at the height of a hand, so it only
        // has to keep clear of what stands against that same wall of that same cell.
        for (size_t r = 0; r < plan.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = plan.rooms[r];
            if (!reach[plan.Index(room.floor, room.min.x, room.min.y)])
            {
                continue;
            }
            for (int z = room.min.y; z <= room.max.y; ++z)
            {
                for (int x = room.min.x; x <= room.max.x; ++x)
                {
                    if (ByADoor(room.floor, {x, z}))
                    {
                        continue;
                    }
                    for (const glm::ivec2 step : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)})
                    {
                        const glm::ivec2 out{x + step.x, z + step.y};
                        if (InBounds(out.x, out.y) && CellAt(room.floor, out.x, out.y) == FacilityLayout::Cell::Room)
                        {
                            continue;
                        }
                        const float yaw = std::atan2(static_cast<float>(step.x), static_cast<float>(step.y));
                        const bool taken = std::any_of(plan.things.begin(), plan.things.end(), [&](const FacilityLayout::Placed& thing)
                        {
                            return thing.floor == room.floor && static_cast<int>(std::floor(thing.at.x)) == x &&
                                   static_cast<int>(std::floor(thing.at.y)) == z && std::abs(thing.yaw - yaw) < 0.01f;
                        }) ||
                        // Nor over anything up to the panel's foot, a metre up, against that wall there.
                        std::any_of(plan.fittings.begin(), plan.fittings.end(), [&](const FacilityLayout::Fitted& fitted)
                        {
                            return fitted.wall && fitted.floor == room.floor && static_cast<int>(std::floor(fitted.at.x)) == x &&
                                   static_cast<int>(std::floor(fitted.at.y)) == z && std::abs(fitted.yaw - yaw) < 0.01f &&
                                   FacilityLayout::FittingSize(fitted.kind).z > 1.0f;
                        });
                        if (!taken)
                        {
                            AddBreaker(room.floor, glm::vec2(static_cast<float>(x) + 0.5f + static_cast<float>(step.x) * 0.3f,
                                                             static_cast<float>(z) + 0.5f + static_cast<float>(step.y) * 0.3f),
                                       yaw);
                            return;
                        }
                    }
                }
            }
        }
    }

    void AddBreaker(int floor, glm::vec2 at, float yaw)
    {
        FacilityLayout::Placed panel;
        panel.thing = FacilityLayout::Thing::Breaker;
        panel.floor = floor;
        panel.at = at;
        panel.yaw = yaw;
        plan.things.push_back(panel);
    }

    void Furnish()
    {
        // The way in: the biggest room on the ground floor. The nest: a room far from it, dark.
        int bestArea = -1;
        for (size_t r = 0; r < plan.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = plan.rooms[r];
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            if (room.floor == plan.ground && size.x * size.y > bestArea)
            {
                bestArea = size.x * size.y;
                plan.entranceRoom = static_cast<int>(r);
            }
        }
        if (plan.entranceRoom >= 0)
        {
            plan.rooms[static_cast<size_t>(plan.entranceRoom)].dark = false;
        }
    }

    void ChooseNest()
    {
        if (plan.entranceRoom < 0)
        {
            return;
        }
        const FacilityLayout::Room& entrance = plan.rooms[static_cast<size_t>(plan.entranceRoom)];
        float furthest = -1.0f;
        for (size_t r = 0; r < plan.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = plan.rooms[r];
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            if (static_cast<int>(r) == plan.entranceRoom || size.x * size.y < 9)
            {
                continue;
            }
            const float away = glm::length(glm::vec2(room.min + room.max - entrance.min - entrance.max)) +
                               static_cast<float>(std::abs(room.floor - entrance.floor)) * 12.0f;
            if (away > furthest)
            {
                furthest = away;
                plan.nestRoom = static_cast<int>(r);
            }
        }
        if (plan.nestRoom >= 0)
        {
            plan.rooms[static_cast<size_t>(plan.nestRoom)].dark = true;
        }
    }

    void Light()
    {
        const auto mood = [&](bool dark)
        {
            if (dark)
            {
                return random.Chance(0.3f) ? FacilityLayout::LampMood::Emergency : FacilityLayout::LampMood::Dead;
            }
            const float roll = random.Unit();
            // Mostly steady: a few flicker or are failing, so those few are noticed.
            return roll < 0.7f   ? FacilityLayout::LampMood::Steady
                   : roll < 0.8f ? FacilityLayout::LampMood::Flicker
                   : roll < 0.88f ? FacilityLayout::LampMood::Failing
                                  : FacilityLayout::LampMood::Dead;
        };
        for (const FacilityLayout::Room& room : plan.rooms)
        {
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            // A lamp to every two cells or so each way -- six metres apart, as a ceiling's lights are -- so a room is lit to its
            // corners, not a pool under one lamp with the rest of it black.
            const int across = std::max(1, (size.x + 1) / 2);
            const int along = std::max(1, (size.y + 1) / 2);
            for (int i = 0; i < across; ++i)
            {
                for (int k = 0; k < along; ++k)
                {
                    FacilityLayout::Lamp lamp;
                    lamp.floor = room.floor;
                    lamp.at = glm::vec2(room.min) + glm::vec2((static_cast<float>(i) + 0.5f) * static_cast<float>(size.x) / static_cast<float>(across),
                                                              (static_cast<float>(k) + 0.5f) * static_cast<float>(size.y) / static_cast<float>(along));
                    lamp.mood = mood(room.dark);
                    lamp.circuit = 10 + room.floor;
                    plan.lamps.push_back(lamp);
                }
            }
        }
        // A ladder's cell, on both its floors: a lamp of its own over the floor in front of the hatch. A flight's, one over the
        // landing at its foot and one over the landing at its head.
        for (const FacilityLayout::Stairwell& well : plan.stairwells)
        {
            if (well.way == FacilityLayout::Stairwell::Way::Lift)
            {
                continue;
            }
            const bool flight = well.way == FacilityLayout::Stairwell::Way::Flight;
            for (const int f : {well.floor, well.floor + 1})
            {
                FacilityLayout::Lamp lamp;
                lamp.floor = f;
                lamp.at = glm::vec2(well.min) + glm::vec2(0.5f);
                if (flight)
                {
                    // The low end's cell below, the high end's above.
                    const bool lowEndAtMin = well.rising;
                    const bool atMin = f == well.floor ? lowEndAtMin : !lowEndAtMin;
                    lamp.at = atMin ? glm::vec2(well.min) + glm::vec2(0.5f) : glm::vec2(well.max) + glm::vec2(0.5f);
                }
                lamp.mood = mood(false);
                lamp.circuit = 10 + f;
                plan.lamps.push_back(lamp);
            }
        }
        // Corridors: a lamp every third cell or so, in a pattern that does not line up with anything.
        for (int f = 0; f < plan.floors; ++f)
        {
            for (int z = 0; z < plan.depth; ++z)
            {
                for (int x = 0; x < plan.width; ++x)
                {
                    if (plan.At(f, x, z) == FacilityLayout::Cell::Corridor && (x * 7 + z * 3) % 5 == 0)
                    {
                        FacilityLayout::Lamp lamp;
                        lamp.floor = f;
                        lamp.at = {static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f};
                        lamp.mood = mood(random.Chance(0.12f));
                        lamp.circuit = 10 + f;
                        plan.lamps.push_back(lamp);
                    }
                }
            }
        }
    }

    // What each room is for, by the building's program: the way in its lobby; the big halls the program's own; then the rooms
    // it needs, each given the room that suits it best -- by its size, its floor (no one sleeps or eats under the ground; the
    // plant and the stores go down there where they can), its neighbours (the kitchen by the mess, washrooms by the
    // quarters); the rest stores and offices.
    void AssignPurposes(FacilityLayout::Program program)
    {
        using P = FacilityLayout::Purpose;
        using G = FacilityLayout::Program;
        struct Need
        {
            P purpose;
            int count;
        };
        std::vector<Need> needs;
        P hall = P::Warehouse;
        switch (program)
        {
        case G::Research:
            hall = P::Lab;
            needs = {{P::Lab, 3}, {P::Control, 1}, {P::Office, 2}, {P::Servers, 1}, {P::Mess, 1}, {P::Kitchen, 1}, {P::Quarters, 2},
                     {P::Washroom, 2}, {P::Medbay, 1}, {P::Plant, 1}, {P::Store, 1}};
            break;
        case G::Industrial:
            hall = P::Plant;
            needs = {{P::Control, 1}, {P::Workshop, 2}, {P::Plant, 2}, {P::Store, 2}, {P::Office, 1}, {P::Mess, 1}, {P::Kitchen, 1},
                     {P::Quarters, 1}, {P::Washroom, 1}, {P::Medbay, 1}};
            break;
        case G::Depot:
            hall = P::Warehouse;
            needs = {{P::Store, 3}, {P::Office, 2}, {P::Control, 1}, {P::Workshop, 1}, {P::Mess, 1}, {P::Kitchen, 1}, {P::Quarters, 1},
                     {P::Washroom, 1}};
            break;
        case G::Living:
            hall = P::Mess;
            needs = {{P::Quarters, 5}, {P::Washroom, 3}, {P::Mess, 1}, {P::Kitchen, 1}, {P::Medbay, 1}, {P::Office, 1}, {P::Control, 1},
                     {P::Plant, 1}, {P::Store, 1}};
            break;
        case G::Comms:
            hall = P::Servers;
            needs = {{P::Control, 2}, {P::Servers, 2}, {P::Office, 1}, {P::Workshop, 1}, {P::Quarters, 2}, {P::Washroom, 1}, {P::Mess, 1},
                     {P::Kitchen, 1}, {P::Plant, 1}, {P::Store, 1}};
            break;
        case G::Observatory:
            hall = P::Control;
            needs = {{P::Control, 1}, {P::Lab, 2}, {P::Office, 2}, {P::Servers, 1}, {P::Quarters, 2}, {P::Washroom, 1}, {P::Mess, 1},
                     {P::Kitchen, 1}, {P::Store, 1}};
            break;
        case G::Field:
            hall = P::Workshop;
            needs = {{P::Office, 1}, {P::Quarters, 1}, {P::Workshop, 1}, {P::Washroom, 1}, {P::Store, 1}, {P::Mess, 1}};
            break;
        case G::Generic:
            hall = P::Warehouse;
            needs = {{P::Office, 2}, {P::Lab, 1}, {P::Store, 2}, {P::Washroom, 1}, {P::Plant, 1}};
            break;
        }
        const auto area = [](const FacilityLayout::Room& room)
        {
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            return size.x * size.y;
        };
        // How far apart two rooms are, in cells: nought if they touch.
        const auto apart = [](const FacilityLayout::Room& a, const FacilityLayout::Room& b)
        {
            const int dx = std::max({0, a.min.x - b.max.x - 1, b.min.x - a.max.x - 1});
            const int dz = std::max({0, a.min.y - b.max.y - 1, b.min.y - a.max.y - 1});
            return a.floor == b.floor ? dx + dz : 99;
        };
        for (FacilityLayout::Room& room : plan.rooms)
        {
            room.purpose = P::None;
        }
        if (plan.entranceRoom >= 0)
        {
            plan.rooms[static_cast<size_t>(plan.entranceRoom)].purpose = P::Lobby;
        }
        // The halls: the biggest over the ground the program's own, the next what else it would want a big room for; under the
        // ground, machinery and storage.
        std::vector<P> halls{hall};
        switch (program)
        {
        case G::Research: halls.insert(halls.end(), {P::Lab, P::Workshop}); break;
        case G::Industrial: halls.insert(halls.end(), {P::Workshop, P::Warehouse}); break;
        case G::Depot: halls.insert(halls.end(), {P::Warehouse, P::Workshop}); break;
        case G::Living: halls.insert(halls.end(), {P::Workshop, P::Store}); break;
        case G::Comms: halls.insert(halls.end(), {P::Control, P::Workshop}); break;
        case G::Observatory: halls.insert(halls.end(), {P::Lab, P::Workshop}); break;
        case G::Field:
        case G::Generic: halls.insert(halls.end(), {P::Warehouse, P::Store}); break;
        }
        std::vector<size_t> big;
        for (size_t r = 0; r < plan.rooms.size(); ++r)
        {
            if (plan.rooms[r].purpose == P::None && area(plan.rooms[r]) >= 25)
            {
                big.push_back(r);
            }
        }
        std::sort(big.begin(), big.end(), [&](size_t a, size_t b) { return area(plan.rooms[a]) > area(plan.rooms[b]); });
        int above = 0;
        int below = 0;
        for (const size_t r : big)
        {
            FacilityLayout::Room& room = plan.rooms[r];
            if (room.floor < plan.ground)
            {
                room.purpose = below++ % 2 == 0 ? P::Plant : P::Warehouse;
            }
            else
            {
                room.purpose = halls[static_cast<size_t>(std::min(above++, static_cast<int>(halls.size()) - 1))];
            }
        }
        const auto score = [&](const FacilityLayout::Room& room, P purpose)
        {
            // Its size, against what suits it.
            const auto [smallest, biggest] = [purpose]() -> std::pair<int, int>
            {
                switch (purpose)
                {
                case P::Kitchen:
                case P::Washroom: return {4, 9};
                case P::Office:
                case P::Quarters:
                case P::Store: return {4, 12};
                case P::Medbay:
                case P::Control:
                case P::Servers: return {6, 16};
                case P::Mess:
                case P::Lab:
                case P::Workshop: return {8, 25};
                case P::Plant: return {9, 30};
                default: return {4, 30};
                }
            }();
            const int size = area(room);
            float s = -0.4f * static_cast<float>(std::max({0, smallest - size, size - biggest}));
            // Its floor.
            const bool below = room.floor < plan.ground;
            const bool ground = room.floor == plan.ground;
            switch (purpose)
            {
            case P::Plant:
            case P::Store:
            case P::Servers: s += below ? 3.0f : 0.0f; break;
            case P::Mess:
            case P::Kitchen:
            case P::Workshop: s += below ? -6.0f : ground ? 2.0f : 0.0f; break;
            case P::Quarters:
            case P::Office:
            case P::Lab:
            case P::Medbay:
            case P::Control: s += below ? -6.0f : ground ? 0.0f : 1.5f; break;
            default: break;
            }
            // Its neighbours.
            for (const FacilityLayout::Room& other : plan.rooms)
            {
                const int d = apart(room, other);
                if ((purpose == P::Kitchen && other.purpose == P::Mess) || (purpose == P::Mess && other.purpose == P::Kitchen) ||
                    (purpose == P::Washroom && other.purpose == P::Quarters) || (purpose == P::Servers && other.purpose == P::Control))
                {
                    s += std::max(0.0f, 4.0f - static_cast<float>(d) * 0.8f);
                }
            }
            return s;
        };
        for (const Need& need : needs)
        {
            for (int k = 0; k < need.count; ++k)
            {
                int best = -1;
                float bestScore = -1.0e9f;
                for (size_t r = 0; r < plan.rooms.size(); ++r)
                {
                    if (plan.rooms[r].purpose != P::None)
                    {
                        continue;
                    }
                    const float s = score(plan.rooms[r], need.purpose);
                    if (s > bestScore)
                    {
                        bestScore = s;
                        best = static_cast<int>(r);
                    }
                }
                if (best >= 0)
                {
                    plan.rooms[static_cast<size_t>(best)].purpose = need.purpose;
                }
            }
        }
        // What is left: stores under the ground; over it, what the program would want more of.
        int turn = 0;
        for (FacilityLayout::Room& room : plan.rooms)
        {
            if (room.purpose != P::None)
            {
                continue;
            }
            // (The bigger ones as the program would use them; the smaller, by turns, a room the program has many of and a store.)
            const P bigger = program == G::Research || program == G::Observatory ? P::Lab
                             : program == G::Living                              ? P::Quarters
                             : program == G::Depot                               ? P::Store
                             : program == G::Comms                               ? P::Servers
                             : program == G::Generic                             ? P::Office
                                                                                 : P::Workshop;
            const P smaller = program == G::Living ? P::Quarters : P::Office;
            room.purpose = room.floor < plan.ground ? P::Store : area(room) >= 12 ? bigger : (turn++ % 2 == 0 ? smaller : P::Store);
        }
    }

    // Spots against a room's walls: a cell on its edge and the way to the wall, by no doorway, not furnished -- and, for
    // something tall, not against the building's outside wall, where its windows are. In no particular order.
    std::vector<std::pair<glm::ivec2, glm::ivec2>> WallSpots(const FacilityLayout::Room& room, bool tall)
    {
        std::vector<std::pair<glm::ivec2, glm::ivec2>> spots;
        for (int z = room.min.y; z <= room.max.y; ++z)
        {
            for (int x = room.min.x; x <= room.max.x; ++x)
            {
                const glm::ivec2 c{x, z};
                if (ByADoor(room.floor, c) || furnished.count({room.floor, x, z}) != 0)
                {
                    continue;
                }
                for (const glm::ivec2 step : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)})
                {
                    const glm::ivec2 out = c + step;
                    if (InBounds(out.x, out.y) && CellAt(room.floor, out.x, out.y) == FacilityLayout::Cell::Room && OwnerAt(room.floor, out.x, out.y) == OwnerAt(room.floor, x, z))
                    {
                        continue; // not an edge that way
                    }
                    const bool outer = !InBounds(out.x, out.y) || CellAt(room.floor, out.x, out.y) == FacilityLayout::Cell::Outside;
                    if (tall && outer)
                    {
                        continue;
                    }
                    spots.push_back({c, step});
                }
            }
        }
        for (size_t i = spots.size(); i > 1; --i)
        {
            std::swap(spots[i - 1], spots[static_cast<size_t>(random.Int(0, static_cast<int>(i) - 1))]);
        }
        return spots;
    }

    // Spots out in a room, clear of its walls -- every other column, so what stands there stands in rows with aisles between.
    std::vector<glm::ivec2> MiddleSpots(const FacilityLayout::Room& room)
    {
        std::vector<glm::ivec2> spots;
        for (int z = room.min.y + 1; z < room.max.y; ++z)
        {
            for (int x = room.min.x + 1; x < room.max.x; x += 2)
            {
                // (Clear of the walls, and so of the doorways in them: what stands here leaves the edge of the room to walk round.)
                if (furnished.count({room.floor, x, z}) == 0)
                {
                    spots.push_back({x, z});
                }
            }
        }
        return spots;
    }

    // Each room furnished as what it is for (Purpose): so many of each thing, against its walls or out in it, as many as there
    // is room for.
    // Something of `kind` against a wall of a room, at the first spot along its walls there is room for it: false when there is none.
    bool AgainstWall(const FacilityLayout::Room& room, FacilityLayout::Fitting kind)
    {
        const bool tall = FacilityLayout::FittingSize(kind).z > 1.15f;
        const auto spots = WallSpots(room, tall);
        if (spots.empty())
        {
            return false;
        }
        const auto [c, step] = spots.front();
        furnished.insert({room.floor, c.x, c.y});
        FacilityLayout::Fitted fitted;
        fitted.kind = kind;
        fitted.floor = room.floor;
        fitted.at = glm::vec2(static_cast<float>(c.x) + 0.5f + static_cast<float>(step.x) * 0.3f,
                              static_cast<float>(c.y) + 0.5f + static_cast<float>(step.y) * 0.3f);
        fitted.yaw = std::atan2(static_cast<float>(step.x), static_cast<float>(step.y));
        plan.fittings.push_back(fitted);
        return true;
    }

    void FitOut()
    {
        using P = FacilityLayout::Purpose;
        using F = FacilityLayout::Fitting;
        for (const FacilityLayout::Room& room : plan.rooms)
        {
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            const int edge = 2 * (size.x + size.y);
            const auto wall = [&](F kind, int count)
            {
                for (int i = 0; i < count; ++i)
                {
                    if (!AgainstWall(room, kind))
                    {
                        return;
                    }
                }
            };
            const auto middle = [&](F kind, int count)
            {
                std::vector<glm::ivec2> spots = MiddleSpots(room);
                const float yaw = size.x >= size.y ? glm::half_pi<float>() : 0.0f;
                for (int i = 0; i < count && i < static_cast<int>(spots.size()); ++i)
                {
                    const glm::ivec2 c = spots[static_cast<size_t>(i)];
                    furnished.insert({room.floor, c.x, c.y});
                    FacilityLayout::Fitted fitted;
                    fitted.kind = kind;
                    fitted.floor = room.floor;
                    fitted.at = glm::vec2(c) + glm::vec2(0.5f);
                    fitted.yaw = yaw;
                    fitted.wall = false;
                    plan.fittings.push_back(fitted);
                }
            };
            switch (room.purpose)
            {
            case P::Lobby:
                wall(F::Reception, 1);
                wall(F::Noticeboard, 1);
                wall(F::Seat, std::min(3, edge / 6));
                wall(F::PlantPot, 2);
                break;
            case P::Office:
                wall(F::Terminal, 1);
                wall(F::Desk, std::min(4, edge / 4));
                wall(F::Cabinet, 1 + random.Int(0, 1));
                break;
            case P::Control:
                wall(F::Terminal, 1);
                wall(F::Wallscreen, 1);
                wall(F::Console, std::min(5, edge / 3));
                wall(F::Cabinet, 1);
                break;
            case P::Lab:
                wall(F::Terminal, 1);
                wall(F::FumeHood, 1);
                wall(F::LabBench, std::min(5, edge / 3));
                wall(F::Basin, 1);
                middle(F::LabBench, 2);
                break;
            case P::Quarters:
                wall(random.Chance(0.5f) ? F::Bunk : F::Bed, std::min(4, edge / 4));
                wall(F::Cabinet, 1);
                wall(F::Desk, 1);
                wall(F::Mirror, 1);
                break;
            case P::Mess:
                middle(F::Table, 6);
                wall(F::Counter, 2);
                wall(F::PlantPot, 1);
                break;
            case P::Kitchen:
                wall(F::Fridge, 1);
                wall(F::Cooker, 1);
                wall(F::Counter, std::min(4, edge / 3));
                break;
            case P::Washroom:
                wall(F::Toilet, std::min(3, edge / 4));
                wall(F::Shower, 1);
                wall(F::Basin, 2);
                break;
            case P::Medbay:
                wall(F::MedBed, std::min(4, edge / 3));
                wall(F::Cabinet, 1);
                wall(F::Basin, 1);
                break;
            case P::Store:
                wall(F::Shelving, std::min(6, edge / 2));
                middle(F::Pallet, 2);
                break;
            case P::Workshop:
                wall(F::Workbench, std::min(3, edge / 4));
                wall(F::Shelving, 1);
                wall(F::Drum, 2);
                middle(F::Pallet, 1);
                break;
            case P::Plant:
                middle(F::Generator, 2);
                middle(F::Pump, 2);
                wall(F::Drum, 3);
                wall(F::Cabinet, 1);
                break;
            case P::Servers:
                wall(F::Terminal, 1);
                middle(F::Rack, 8);
                wall(F::Console, 1);
                break;
            case P::Warehouse:
                middle(F::Racking, 12);
                wall(F::Shelving, std::min(4, edge / 4));
                wall(F::Pallet, 2);
                break;
            case P::None: break;
            }
        }
        // And every building a bench to work at somewhere -- what a mission's terminal stands on (MissionPlan) -- when the rooms
        // that run things had no wall left for one, or it has none: in the first room that will take one, of those that run
        // things first, and not one behind a locked door or where a creature would build.
        if (std::any_of(plan.fittings.begin(), plan.fittings.end(), [](const FacilityLayout::Fitted& fitted) { return fitted.kind == F::Terminal; }))
        {
            return;
        }
        for (const P want : {P::Control, P::Office, P::Lab, P::Servers, P::Workshop, P::Lobby, P::Medbay, P::Store, P::Warehouse, P::Plant, P::Mess})
        {
            for (size_t r = 0; r < plan.rooms.size(); ++r)
            {
                const FacilityLayout::Room& room = plan.rooms[r];
                const bool locked = std::any_of(plan.doors.begin(), plan.doors.end(), [&](const FacilityLayout::Door& door)
                                                { return door.locked && door.room == static_cast<int>(r); });
                if (room.purpose == want && static_cast<int>(r) != plan.nestRoom && !locked && AgainstWall(room, F::Terminal))
                {
                    return;
                }
            }
        }
    }

    void Fill()
    {
        // Where everybody comes in -- the middle of the way in, where they stand when they arrive (FacilityMap::Draw) -- kept clear:
        // nothing put down within a stride of it.
        if (plan.entranceRoom >= 0)
        {
            const FacilityLayout::Room& room = plan.rooms[static_cast<size_t>(plan.entranceRoom)];
            const glm::vec2 middle = glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f;
            for (int z = static_cast<int>(std::floor(middle.y - 0.6f)); z <= static_cast<int>(std::floor(middle.y + 0.6f)); ++z)
            {
                for (int x = static_cast<int>(std::floor(middle.x - 0.6f)); x <= static_cast<int>(std::floor(middle.x + 0.6f)); ++x)
                {
                    furnished.insert({room.floor, x, z});
                }
            }
        }
        for (size_t r = 0; r < plan.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = plan.rooms[r];
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            const bool entrance = static_cast<int>(r) == plan.entranceRoom;
            FacilityLayout::Placed placed;
            placed.floor = room.floor;
            glm::vec2 at;
            float yaw = 0.0f;
            // Lockers, in the smaller rooms.
            if (!entrance && (room.kind == FacilityLayout::RoomKind::Office || room.kind == FacilityLayout::RoomKind::Store ||
                              room.kind == FacilityLayout::RoomKind::Lab) &&
                random.Chance(0.4f))
            {
                const int count = random.Int(1, 2);
                for (int i = 0; i < count; ++i)
                {
                    if (AgainstWall(room, at, yaw))
                    {
                        placed.thing = FacilityLayout::Thing::Locker;
                        placed.at = at;
                        placed.yaw = yaw;
                        plan.things.push_back(placed);
                    }
                }
            }
            // What there is to find.
            if (entrance || random.Chance(0.4f))
            {
                if (AgainstWall(room, at, yaw))
                {
                    placed.thing = FacilityLayout::Thing::Supplies;
                    placed.at = at;
                    placed.yaw = yaw;
                    plan.things.push_back(placed);
                }
            }
            // A hall's pillars, holding up its roof (what is in it, and in every other room, FitOut).
            if (room.kind == FacilityLayout::RoomKind::Hall)
            {
                for (int x = room.min.x + 1; x < room.max.x; x += 2)
                {
                    for (int z = room.min.y + 1; z < room.max.y; z += 2)
                    {
                        placed.thing = FacilityLayout::Thing::Pillar;
                        placed.at = {static_cast<float>(x) + 1.0f, static_cast<float>(z) + 1.0f};
                        placed.size = {0.6f, 0.6f};
                        placed.height = FacilityLayout::kClearHeight;
                        placed.yaw = 0.0f;
                        if (!ByADoor(room.floor, {x, z}))
                        {
                            plan.things.push_back(placed);
                        }
                    }
                }
            }
        }
        FitOut();
        // One ammunition crate a floor, and the keycard, when anything is locked, somewhere reachable.
        const std::vector<bool> reach = plan.Reachable(false);
        const auto reachableRoom = [&](int floor, int& chosen)
        {
            for (int attempt = 0; attempt < 60; ++attempt)
            {
                const int r = random.Int(0, static_cast<int>(plan.rooms.size()) - 1);
                const FacilityLayout::Room& room = plan.rooms[static_cast<size_t>(r)];
                if ((floor < 0 || room.floor == floor) && reach[plan.Index(room.floor, room.min.x, room.min.y)])
                {
                    chosen = r;
                    return true;
                }
            }
            return false;
        };
        for (int f = 0; f < plan.floors; ++f)
        {
            int r = -1;
            glm::vec2 at;
            float yaw = 0.0f;
            if (reachableRoom(f, r) && AgainstWall(plan.rooms[static_cast<size_t>(r)], at, yaw))
            {
                FacilityLayout::Placed crate;
                crate.thing = FacilityLayout::Thing::AmmoCrate;
                crate.floor = f;
                crate.at = at;
                crate.yaw = yaw;
                plan.things.push_back(crate);
            }
        }
        if (std::any_of(plan.doors.begin(), plan.doors.end(), [](const FacilityLayout::Door& d) { return d.locked; }))
        {
            // Somewhere reachable without it, always: against a wall of a room -- any of them, before -- in the middle of
            // one, where nothing else stands already.
            FacilityLayout::Placed card;
            card.thing = FacilityLayout::Thing::Keycard;
            bool placed = false;
            const int first = random.Int(0, static_cast<int>(plan.rooms.size()) - 1);
            for (int pass = 0; pass < 2 && !placed; ++pass)
            {
                for (size_t i = 0; i < plan.rooms.size() && !placed; ++i)
                {
                    const FacilityLayout::Room& room = plan.rooms[(static_cast<size_t>(first) + i) % plan.rooms.size()];
                    if (!reach[plan.Index(room.floor, room.min.x, room.min.y)])
                    {
                        continue;
                    }
                    glm::vec2 at{0.0f};
                    float yaw = 0.0f;
                    if (pass == 0 && !AgainstWall(room, at, yaw))
                    {
                        continue;
                    }
                    if (pass == 1)
                    {
                        const glm::ivec2 middle = (room.min + room.max) / 2;
                        if (furnished.count({room.floor, middle.x, middle.y}) != 0 || ByADoor(room.floor, middle))
                        {
                            continue;
                        }
                        furnished.insert({room.floor, middle.x, middle.y});
                        at = glm::vec2(middle) + glm::vec2(0.5f);
                    }
                    card.floor = room.floor;
                    card.at = at;
                    card.yaw = yaw;
                    placed = true;
                }
            }
            if (placed)
            {
                plan.things.push_back(card);
            }
        }
    }
};

} // namespace

FacilityLayout::Cell FacilityLayout::At(int floor, int x, int z) const
{
    if (floor < 0 || floor >= floors || x < 0 || z < 0 || x >= width || z >= depth)
    {
        return Cell::Solid;
    }
    return cells[Index(floor, x, z)];
}

int FacilityLayout::RoomAt(int floor, int x, int z) const
{
    if (At(floor, x, z) != Cell::Room)
    {
        return -1;
    }
    return roomOf[Index(floor, x, z)];
}

int FacilityLayout::WellAt(int floor, int x, int z) const
{
    for (size_t w = 0; w < stairwells.size(); ++w)
    {
        const Stairwell& well = stairwells[w];
        if (floor >= well.floor && floor <= well.floor + well.levels && x >= well.min.x && x <= well.max.x && z >= well.min.y && z <= well.max.y)
        {
            return static_cast<int>(w);
        }
    }
    return -1;
}

bool FacilityLayout::Open(int floor, int x, int z) const
{
    const Cell cell = At(floor, x, z);
    return cell == Cell::Room || cell == Cell::Corridor || cell == Cell::Stair;
}

std::vector<bool> FacilityLayout::Reachable(bool throughLocked) const
{
    std::vector<bool> reached(cells.size(), false);
    if (entranceRoom < 0)
    {
        return reached;
    }
    // The doorways, by the edge they are on.
    std::map<std::tuple<int, int, int, int>, bool> doorways;
    for (const Door& door : doors)
    {
        doorways[{door.floor, door.cell.x, door.cell.y, door.side}] = door.locked;
    }
    const auto space = [&](int f, int x, int z)
    {
        const Cell cell = At(f, x, z);
        return cell == Cell::Room ? roomOf[Index(f, x, z)] : cell == Cell::Corridor ? -2 : cell == Cell::Stair ? roomOf[Index(f, x, z)] : -1;
    };
    const auto passable = [&](int f, glm::ivec2 a, glm::ivec2 b)
    {
        if (!Open(f, b.x, b.y))
        {
            return false;
        }
        if (space(f, a.x, a.y) == space(f, b.x, b.y))
        {
            return true;
        }
        const glm::ivec2 low = (b.x > a.x || b.y > a.y) ? a : b;
        const int side = b.x != a.x ? 0 : 1;
        const auto found = doorways.find({f, low.x, low.y, side});
        return found != doorways.end() && (throughLocked || !found->second);
    };
    const Room& start = rooms[static_cast<size_t>(entranceRoom)];
    std::queue<std::tuple<int, int, int>> open;
    open.push({start.floor, start.min.x, start.min.y});
    reached[Index(start.floor, start.min.x, start.min.y)] = true;
    while (!open.empty())
    {
        const auto [f, x, z] = open.front();
        open.pop();
        const glm::ivec2 steps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const glm::ivec2 step : steps)
        {
            const glm::ivec2 next{x + step.x, z + step.y};
            if (next.x < 0 || next.y < 0 || next.x >= width || next.y >= depth || reached[Index(f, next.x, next.y)])
            {
                continue;
            }
            if (passable(f, {x, z}, next))
            {
                reached[Index(f, next.x, next.y)] = true;
                open.push({f, next.x, next.y});
            }
        }
        // Up or down a stairwell: the same well on the floor above or below.
        if (At(f, x, z) == Cell::Stair)
        {
            for (const int other : {f - 1, f + 1})
            {
                if (other >= 0 && other < floors && At(other, x, z) == Cell::Stair &&
                    roomOf[Index(other, x, z)] == roomOf[Index(f, x, z)] && !reached[Index(other, x, z)])
                {
                    reached[Index(other, x, z)] = true;
                    open.push({other, x, z});
                }
            }
        }
    }
    return reached;
}

const char* FacilityLayout::PurposeName(Purpose purpose)
{
    switch (purpose)
    {
    case Purpose::None: return "room";
    case Purpose::Lobby: return "lobby";
    case Purpose::Office: return "office";
    case Purpose::Control: return "control room";
    case Purpose::Lab: return "lab";
    case Purpose::Quarters: return "quarters";
    case Purpose::Mess: return "mess";
    case Purpose::Kitchen: return "kitchen";
    case Purpose::Washroom: return "washroom";
    case Purpose::Medbay: return "medbay";
    case Purpose::Store: return "store";
    case Purpose::Workshop: return "workshop";
    case Purpose::Plant: return "plant room";
    case Purpose::Servers: return "server room";
    case Purpose::Warehouse: return "warehouse";
    }
    return "room";
}

glm::vec3 FacilityLayout::FittingSize(Fitting kind)
{
    // Across, deep and high: what each takes of a room. (Out in a room, nothing over 2.2 m across, so it keeps clear of a
    // hall's pillars at its cells' corners.)
    switch (kind)
    {
    case Fitting::Reception: return {2.4f, 0.9f, 1.1f};
    case Fitting::Seat: return {1.6f, 0.55f, 0.9f};
    case Fitting::PlantPot: return {0.6f, 0.6f, 1.1f};
    case Fitting::Noticeboard: return {1.4f, 0.06f, 2.0f};
    case Fitting::Desk: return {1.5f, 1.3f, 0.95f};
    case Fitting::Cabinet: return {0.9f, 0.6f, 1.9f};
    case Fitting::Console: return {1.8f, 1.4f, 1.15f};
    case Fitting::Wallscreen: return {2.2f, 0.08f, 2.4f};
    case Fitting::LabBench: return {2.2f, 0.8f, 1.1f};
    case Fitting::FumeHood: return {1.5f, 0.85f, 2.3f};
    case Fitting::Bed: return {0.95f, 2.05f, 0.75f};
    case Fitting::Bunk: return {0.95f, 2.05f, 1.8f};
    case Fitting::Table: return {1.8f, 2.0f, 0.76f};
    case Fitting::Counter: return {2.2f, 0.65f, 0.92f};
    case Fitting::Cooker: return {0.9f, 0.65f, 0.92f};
    case Fitting::Fridge: return {0.9f, 0.75f, 2.0f};
    case Fitting::Basin: return {0.7f, 0.5f, 2.0f};
    case Fitting::Toilet: return {1.0f, 1.5f, 2.0f};
    case Fitting::Shower: return {1.0f, 1.0f, 2.1f};
    case Fitting::MedBed: return {0.95f, 2.1f, 1.0f};
    case Fitting::Shelving: return {2.0f, 0.55f, 2.2f};
    case Fitting::Pallet: return {1.2f, 1.0f, 1.1f};
    case Fitting::Workbench: return {2.0f, 0.75f, 2.0f};
    case Fitting::Drum: return {0.6f, 0.6f, 0.9f};
    case Fitting::Generator: return {2.2f, 1.2f, 1.6f};
    case Fitting::Pump: return {1.2f, 0.8f, 1.0f};
    case Fitting::Rack: return {0.7f, 1.1f, 2.1f};
    case Fitting::Racking: return {2.2f, 1.1f, 2.6f};
    case Fitting::Mirror: return {0.5f, 0.04f, 1.85f};
    case Fitting::Terminal: return {1.6f, 0.8f, 0.92f};
    }
    return {1.0f, 1.0f, 1.0f};
}

int FacilityLayout::Height(int x, int z) const
{
    if (x < 0 || z < 0 || x >= width || z >= depth)
    {
        return 0;
    }
    return heights.empty() ? floors : heights[static_cast<size_t>(z * width + x)];
}

std::vector<FacilityLayout::OuterWall> FacilityLayout::OuterWalls() const
{
    // Every edge between a part of it and something lower -- the ground past it, or a lower roof -- is outside wall of the
    // part that stands higher, from the top of the lower up to its own; edges side by side along one line, the same storeys
    // high, one run.
    std::vector<OuterWall> walls;
    for (int side = 0; side < 4; ++side)
    {
        const glm::ivec2 out = side == 0 ? glm::ivec2(-1, 0) : side == 1 ? glm::ivec2(1, 0) : side == 2 ? glm::ivec2(0, -1) : glm::ivec2(0, 1);
        const bool alongX = side >= 2;
        const int lines = alongX ? depth : width;
        const int length = alongX ? width : depth;
        for (int k = 0; k < lines; ++k)
        {
            OuterWall run;
            bool open = false;
            for (int i = 0; i <= length; ++i)
            {
                const glm::ivec2 c = alongX ? glm::ivec2(i, k) : glm::ivec2(k, i);
                const int high = i < length ? Height(c.x, c.y) : 0;
                const int low = i < length ? Height(c.x + out.x, c.y + out.y) : 0;
                const bool wall = high > low;
                if (open && (!wall || low != run.low || high != run.high))
                {
                    run.to = i;
                    walls.push_back(run);
                    open = false;
                }
                if (wall && !open)
                {
                    run = {};
                    run.side = side;
                    run.line = side == 1 ? k + 1 : side == 3 ? k + 1 : k;
                    run.from = i;
                    run.low = low;
                    run.high = high;
                    open = true;
                }
            }
        }
    }
    return walls;
}

std::vector<FacilityLayout::OuterCorner> FacilityLayout::OuterCorners() const
{
    // At each corner of cells, a part standing higher than the three others round it turns its walls there, from the top of
    // the highest of those up to its own.
    std::vector<OuterCorner> corners;
    for (int z = 0; z <= depth; ++z)
    {
        for (int x = 0; x <= width; ++x)
        {
            const glm::ivec2 quarter[] = {{-1, -1}, {0, -1}, {-1, 0}, {0, 0}};
            for (int q = 0; q < 4; ++q)
            {
                const int mine = Height(x + quarter[q].x, z + quarter[q].y);
                int others = 0;
                for (int r = 0; r < 4; ++r)
                {
                    if (r != q)
                    {
                        others = std::max(others, Height(x + quarter[r].x, z + quarter[r].y));
                    }
                }
                if (mine > others)
                {
                    OuterCorner corner;
                    corner.at = {x, z};
                    corner.out = {quarter[q].x == 0 ? -1 : 1, quarter[q].y == 0 ? -1 : 1};
                    corner.low = others;
                    corner.high = mine;
                    corners.push_back(corner);
                }
            }
        }
    }
    return corners;
}

FacilityLayout FacilityLayout::Generate(uint32_t seed, const Options& options)
{
    // Tried again, from the same seed and a counted attempt, until the whole of it can be walked from
    // the way in. Nearly always the first time.
    FacilityLayout best;
    for (uint32_t attempt = 0; attempt < 40; ++attempt)
    {
        FacilityLayout plan;
        plan.seed = seed;
        plan.origin = options.origin;
        Planner planner{plan, Random((static_cast<uint64_t>(seed) << 8) ^ (attempt * 0x9E3779B9u) ^ 0xFAC11177ull)};
        // (Any basements under the floors asked for, the whole of it let down into the ground by them.)
        plan.ground = std::max(options.basements, 0);
        plan.floors = planner.random.Int(options.minFloors, std::max(options.minFloors, options.maxFloors)) + plan.ground;
        plan.origin.y -= static_cast<float>(plan.ground) * kStorey;
        plan.width = std::max(options.width, 10);
        plan.depth = std::max(options.depth, 10);
        plan.cells.assign(static_cast<size_t>(plan.floors * plan.width * plan.depth), Cell::Solid);
        plan.roomOf.assign(plan.cells.size(), -1);
        planner.Shape(options.massing);
        planner.PlaceLift();
        planner.PlaceStairwells();
        planner.PlaceRooms();
        planner.Furnish();
        planner.Connect();
        planner.WidenCorridors();
        if (options.exits > 0)
        {
            planner.AddExits(options.exits, options.exitSide);
        }
        if (options.tunnelSide >= 0 && plan.ground > 0)
        {
            planner.AddExit(options.tunnelSide, true);
        }
        // Everything reachable from the way in, through every door, or it is tried again.
        const std::vector<bool> reach = plan.Reachable(true);
        bool whole = !plan.rooms.empty() && plan.entranceRoom >= 0;
        for (const Room& room : plan.rooms)
        {
            whole = whole && reach[plan.Index(room.floor, room.min.x, room.min.y)];
        }
        for (const Stairwell& well : plan.stairwells)
        {
            whole = whole && reach[plan.Index(well.floor, well.min.x, well.min.y)];
        }
        // And every floor up to the next by stairs or a ladder, not by the lift alone: a lift with its power out goes nowhere.
        for (int f = 0; f + 1 < plan.floors; ++f)
        {
            whole = whole && std::any_of(plan.stairwells.begin(), plan.stairwells.end(), [&](const Stairwell& well)
                                         { return well.floor == f && well.way != Stairwell::Way::Lift; });
        }
        // And every way out, as many as were asked for, leads in.
        whole = whole && (options.exits == 0 || !plan.exits.empty());
        for (const Exit& exit : plan.exits)
        {
            whole = whole && reach[plan.Index(plan.ground, exit.cell.x, exit.cell.y)];
        }
        // And the way down to a passage, when one was asked for, leads in too.
        whole = whole && (options.tunnelSide < 0 || plan.ground == 0 || !plan.tunnels.empty());
        for (const Exit& tunnel : plan.tunnels)
        {
            whole = whole && reach[plan.Index(plan.ground - 1, tunnel.cell.x, tunnel.cell.y)];
        }
        if (!whole)
        {
            best = std::move(plan);
            continue;
        }
        planner.LockSome();
        planner.ChooseNest();
        planner.DigDucts();
        planner.Light();
        planner.AssignPurposes(options.program);
        planner.Fill();
        planner.PlaceBreaker();
        return plan;
    }
    return best;
}

} // namespace pred
