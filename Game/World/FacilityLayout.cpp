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

    // Whether a rectangle, with a margin of `gap` cells round it, is all solid on these floors.
    bool Free(int floorLow, int floorHigh, glm::ivec2 min, glm::ivec2 max, int gap)
    {
        for (int f = floorLow; f <= floorHigh; ++f)
        {
            for (int z = min.y - gap; z <= max.y + gap; ++z)
            {
                for (int x = min.x - gap; x <= max.x + gap; ++x)
                {
                    if (!InBounds(x, z) || CellAt(f, x, z) != FacilityLayout::Cell::Solid)
                    {
                        return false;
                    }
                }
            }
        }
        return true;
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
                    placedHere += well.floor == f ? 1 : 0;
                }
                if (placedHere >= wanted)
                {
                    break;
                }
                FacilityLayout::Stairwell well;
                well.floor = f;
                well.alongX = random.Chance(0.5f);
                well.rising = random.Chance(0.5f);
                // One cell wide, three long: a flight as wide as the way in and out of it.
                const glm::ivec2 size = well.alongX ? glm::ivec2(3, 1) : glm::ivec2(1, 3);
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
            const float scale = static_cast<float>(plan.width * plan.depth) / 576.0f;
            const int wanted = std::max(3, static_cast<int>(std::lround(static_cast<float>(random.Int(8, 11)) * scale)));
            int placed = 0;
            for (int attempt = 0; attempt < 400 && placed < wanted; ++attempt)
            {
                const bool hall = random.Chance(0.15f);
                const glm::ivec2 size = hall ? glm::ivec2(random.Int(5, 7), random.Int(5, 7))
                                             : glm::ivec2(random.Int(2, 5), random.Int(2, 5));
                const glm::ivec2 min{random.Int(1, plan.width - size.x - 1), random.Int(1, plan.depth - size.y - 1)};
                const glm::ivec2 max = min + size - glm::ivec2(1);
                if (!Free(f, f, min, max, 1))
                {
                    continue;
                }
                FacilityLayout::Room room;
                room.floor = f;
                room.min = min;
                room.max = max;
                const int area = size.x * size.y;
                room.kind = area >= 25   ? FacilityLayout::RoomKind::Hall
                            : area >= 12 ? (random.Chance(0.35f) ? FacilityLayout::RoomKind::Plant : FacilityLayout::RoomKind::Lab)
                                         : (random.Chance(0.5f) ? FacilityLayout::RoomKind::Store : FacilityLayout::RoomKind::Office);
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
            const bool above = well.floor + 1 == floor;
            if (!below && !above)
            {
                continue;
            }
            // Below, the way in is at the low end; above, the way out at the high end.
            const bool lowEndAtMin = well.rising;
            const bool atMin = below ? lowEndAtMin : !lowEndAtMin;
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

    // A way out of the building on the ground floor, in the wall facing `side` (0 -x, 1 +x, 2 -z, 3 +z):
    // straight into a room or a corridor that already reaches the wall, or along a corridor dug in from the
    // wall to the nearest one.
    bool AddExit(int side)
    {
        const int along = side < 2 ? plan.depth : plan.width;
        const glm::ivec2 inward = side == 0 ? glm::ivec2(1, 0) : side == 1 ? glm::ivec2(-1, 0) : side == 2 ? glm::ivec2(0, 1) : glm::ivec2(0, -1);
        const auto edgeAt = [&](int p)
        {
            return side == 0 ? glm::ivec2(0, p) : side == 1 ? glm::ivec2(plan.width - 1, p) : side == 2 ? glm::ivec2(p, 0) : glm::ivec2(p, plan.depth - 1);
        };
        const auto crowded = [&](int p)
        {
            for (const FacilityLayout::Exit& other : plan.exits)
            {
                if (other.side == side && std::abs((side < 2 ? other.cell.y : other.cell.x) - p) < 3)
                {
                    return true;
                }
            }
            return false;
        };
        for (int attempt = 0; attempt < 32; ++attempt)
        {
            const int p = random.Int(2, along - 3);
            if (crowded(p))
            {
                continue;
            }
            const glm::ivec2 edge = edgeAt(p);
            const FacilityLayout::Cell at = CellAt(0, edge.x, edge.y);
            if (NearDoorway(0, edge, edge - inward))
            {
                continue;
            }
            if (at == FacilityLayout::Cell::Room || at == FacilityLayout::Cell::Corridor)
            {
                plan.exits.push_back({edge, side, at == FacilityLayout::Cell::Room ? OwnerAt(0, edge.x, edge.y) : -1});
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
                const FacilityLayout::Cell cell = CellAt(0, c.x, c.y);
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
                CellAt(0, d.x, d.y) = FacilityLayout::Cell::Corridor;
            }
            if (CellAt(0, c.x, c.y) == FacilityLayout::Cell::Room)
            {
                AddDoor(0, dug.back(), c, OwnerAt(0, c.x, c.y), random.Chance(0.72f));
            }
            plan.exits.push_back({edge, side, -1});
            return true;
        }
        // Nothing straight in: a corridor dug from the wall round to the nearest corridor there is.
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const int p = random.Int(2, along - 3);
            const glm::ivec2 edge = edgeAt(p);
            if (crowded(p) || CellAt(0, edge.x, edge.y) != FacilityLayout::Cell::Solid)
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
                    if (CellAt(0, x, z) == FacilityLayout::Cell::Corridor && d < best)
                    {
                        best = d;
                        nearest = {x, z};
                    }
                }
            }
            if (nearest.x >= 0 && Carve(0, edge, nearest))
            {
                plan.exits.push_back({edge, side, -1});
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

    void Furnish()
    {
        // The way in: the biggest room on the ground floor. The nest: a room far from it, dark.
        int bestArea = -1;
        for (size_t r = 0; r < plan.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = plan.rooms[r];
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            if (room.floor == 0 && size.x * size.y > bestArea)
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
            return roll < 0.5f   ? FacilityLayout::LampMood::Steady
                   : roll < 0.7f ? FacilityLayout::LampMood::Flicker
                   : roll < 0.85f ? FacilityLayout::LampMood::Failing
                                  : FacilityLayout::LampMood::Dead;
        };
        for (const FacilityLayout::Room& room : plan.rooms)
        {
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            const int across = std::max(1, size.x / 3);
            const int along = std::max(1, size.y / 3);
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

    void Fill()
    {
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
            // Cover and clutter, by what the room is.
            switch (room.kind)
            {
            case FacilityLayout::RoomKind::Hall:
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
                break;
            case FacilityLayout::RoomKind::Store:
                for (int i = 0; i < std::max(1, size.x * size.y / 4); ++i)
                {
                    if (AgainstWall(room, at, yaw))
                    {
                        placed.thing = FacilityLayout::Thing::Shelf;
                        placed.at = at;
                        placed.yaw = yaw;
                        placed.size = {2.0f, 0.55f};
                        placed.height = 2.2f;
                        plan.things.push_back(placed);
                    }
                }
                break;
            case FacilityLayout::RoomKind::Lab:
            case FacilityLayout::RoomKind::Office:
                for (int i = 0; i < random.Int(1, 2); ++i)
                {
                    if (AgainstWall(room, at, yaw))
                    {
                        placed.thing = FacilityLayout::Thing::Bench;
                        placed.at = at;
                        placed.yaw = yaw;
                        placed.size = {1.8f, 0.8f};
                        placed.height = 0.92f;
                        plan.things.push_back(placed);
                    }
                }
                break;
            case FacilityLayout::RoomKind::Plant:
                for (int i = 0; i < random.Int(2, 4) && !entrance; ++i)
                {
                    const int x = random.Int(room.min.x, room.max.x);
                    const int z = random.Int(room.min.y, room.max.y);
                    if (ByADoor(room.floor, {x, z}))
                    {
                        continue;
                    }
                    // Never next to another, and never where something stands against a wall: with a cell
                    // between every two there is always a way round them.
                    bool crowded = false;
                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            crowded = crowded || furnished.count({room.floor, x + dx, z + dz}) != 0;
                        }
                    }
                    if (crowded)
                    {
                        continue;
                    }
                    furnished.insert({room.floor, x, z});
                    placed.thing = FacilityLayout::Thing::Crate;
                    placed.at = {static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f};
                    placed.yaw = random.Unit() * glm::pi<float>();
                    // Small enough to stand clear of the walls however it is turned.
                    placed.size = {random.Unit() * 0.4f + 0.8f, random.Unit() * 0.4f + 0.8f};
                    placed.height = random.Unit() * 0.8f + 0.8f;
                    plan.things.push_back(placed);
                }
                break;
            }
        }
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
            // Somewhere reachable without it, always: against a wall of a room, or failing any free wall,
            // in the middle of one.
            FacilityLayout::Placed card;
            card.thing = FacilityLayout::Thing::Keycard;
            bool placed = false;
            const int first = random.Int(0, static_cast<int>(plan.rooms.size()) - 1);
            for (size_t i = 0; i < plan.rooms.size() && !placed; ++i)
            {
                const FacilityLayout::Room& room = plan.rooms[(static_cast<size_t>(first) + i) % plan.rooms.size()];
                if (!reach[plan.Index(room.floor, room.min.x, room.min.y)])
                {
                    continue;
                }
                glm::vec2 at;
                float yaw = 0.0f;
                if (!AgainstWall(room, at, yaw))
                {
                    at = glm::vec2(room.min + room.max) * 0.5f + glm::vec2(0.5f);
                }
                card.floor = room.floor;
                card.at = at;
                card.yaw = yaw;
                placed = true;
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
        plan.floors = planner.random.Int(options.minFloors, std::max(options.minFloors, options.maxFloors));
        plan.width = std::max(options.width, 10);
        plan.depth = std::max(options.depth, 10);
        plan.cells.assign(static_cast<size_t>(plan.floors * plan.width * plan.depth), Cell::Solid);
        plan.roomOf.assign(plan.cells.size(), -1);
        planner.PlaceStairwells();
        planner.PlaceRooms();
        planner.Furnish();
        planner.Connect();
        planner.WidenCorridors();
        if (options.exits > 0)
        {
            planner.AddExits(options.exits, options.exitSide);
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
        // And every way out, as many as were asked for, leads in.
        whole = whole && (options.exits == 0 || !plan.exits.empty());
        for (const Exit& exit : plan.exits)
        {
            whole = whole && reach[plan.Index(0, exit.cell.x, exit.cell.y)];
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
        planner.Fill();
        return plan;
    }
    return best;
}

} // namespace pred
