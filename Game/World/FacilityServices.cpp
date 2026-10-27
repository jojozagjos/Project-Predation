#include "Game/World/FacilityServices.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace pred
{

namespace FacilityServices
{

namespace
{

using Kind = FacilityMap::Piece::Kind;
using Form = FacilityMap::Dressing::Form;
using Cell = FacilityLayout::Cell;
constexpr float kCell = FacilityLayout::kCell;
constexpr float kHalfWall = FacilitySpec::kHalfWall;
constexpr float kClear = FacilityLayout::kClearHeight;
// What runs under a ceiling keeps above the tallest doorway there is, a stairwell's arch.
constexpr float kOver = 2.74f;

struct Fitter
{
    const FacilityLayout& layout;
    FacilityMap::Blueprint& out;

    glm::vec3 World(int floor, glm::vec2 cells) const { return FacilityMap::ToWorld(layout, floor, cells); }

    // A box between two corners in the world (everything here is square to the building).
    void Box(Kind kind, const glm::vec3& a, const glm::vec3& b)
    {
        FacilityMap::Dressing box;
        box.kind = kind;
        box.centre = (a + b) * 0.5f;
        box.size = glm::abs(b - a);
        out.furnishing.push_back(box);
    }
    // A rod lying along x (or z), its middle at `at`.
    void Rod(Kind kind, const glm::vec3& at, float across, float length, bool alongX)
    {
        FacilityMap::Dressing rod;
        rod.form = Form::Cylinder;
        rod.kind = kind;
        rod.centre = at;
        rod.size = {across, length, across};
        rod.rotation = glm::angleAxis(glm::half_pi<float>(), alongX ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
        out.furnishing.push_back(rod);
    }

    bool Corridor(int f, glm::ivec2 c) const
    {
        return c.x >= 0 && c.y >= 0 && c.x < layout.width && c.y < layout.depth && layout.At(f, c.x, c.y) == Cell::Corridor;
    }

    // The four ways out of a cell, as the bits of a cell's links: each way's opposite is its bit with the lowest flipped.
    static constexpr glm::ivec2 kSteps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

    static int Count(uint8_t links)
    {
        return (links & 1) + ((links >> 1) & 1) + ((links >> 2) & 1) + ((links >> 3) & 1);
    }

    // The line the services take through a floor's corridors, as which ways each cell's part of it joins: one line through every
    // corridor, never two side by side down a wide one nor round and round a hall. Grown from an end along the straightest
    // ways there are, cut back from wherever it ends in a wide part, and run on from there straight ahead to the wall.
    std::vector<uint8_t> Line(int f) const
    {
        const auto index = [&](glm::ivec2 c) { return static_cast<size_t>(c.y * layout.width + c.x); };
        const auto open = [&](glm::ivec2 c) { return Corridor(f, c); };
        const auto wide = [&](glm::ivec2 c)
        {
            for (int dz = -1; dz <= 0; ++dz)
            {
                for (int dx = -1; dx <= 0; ++dx)
                {
                    const glm::ivec2 o = c + glm::ivec2(dx, dz);
                    if (open(o) && open(o + glm::ivec2(1, 0)) && open(o + glm::ivec2(0, 1)) && open(o + glm::ivec2(1, 1)))
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        std::vector<uint8_t> links(static_cast<size_t>(layout.width * layout.depth), 0);
        std::vector<int> turns(links.size(), std::numeric_limits<int>::max());
        std::vector<int> came(links.size(), -1);
        // Grown from a dead end where its corridors have one, the fewest turns to every cell (a breadth-first search where going
        // on the way it came costs nothing and turning costs one).
        std::vector<glm::ivec2> starts;
        for (const bool dead : {true, false})
        {
            for (int z = 0; z < layout.depth; ++z)
            {
                for (int x = 0; x < layout.width; ++x)
                {
                    const glm::ivec2 c{x, z};
                    int ways = 0;
                    for (const glm::ivec2 step : kSteps)
                    {
                        ways += open(c + step) ? 1 : 0;
                    }
                    if (open(c) && (ways == 1) == dead)
                    {
                        starts.push_back(c);
                    }
                }
            }
        }
        for (const glm::ivec2 start : starts)
        {
            if (turns[index(start)] != std::numeric_limits<int>::max())
            {
                continue;
            }
            turns[index(start)] = 0;
            std::deque<glm::ivec2> queue{start};
            while (!queue.empty())
            {
                const glm::ivec2 c = queue.front();
                queue.pop_front();
                for (int i = 0; i < 4; ++i)
                {
                    const glm::ivec2 n = c + kSteps[i];
                    if (!open(n))
                    {
                        continue;
                    }
                    const int turn = came[index(c)] < 0 || came[index(c)] == i ? 0 : 1;
                    if (turns[index(c)] + turn < turns[index(n)])
                    {
                        turns[index(n)] = turns[index(c)] + turn;
                        came[index(n)] = i;
                        turn == 0 ? queue.push_front(n) : queue.push_back(n);
                    }
                }
            }
        }
        for (int z = 0; z < layout.depth; ++z)
        {
            for (int x = 0; x < layout.width; ++x)
            {
                const glm::ivec2 c{x, z};
                const int i = came[index(c)];
                if (open(c) && i >= 0)
                {
                    links[index(c)] |= static_cast<uint8_t>(1 << (i ^ 1));
                    links[index(c - kSteps[i])] |= static_cast<uint8_t>(1 << i);
                }
            }
        }
        // Cut back: an end of it in a wide part of a corridor is only a second line beside the first.
        for (bool cut = true; cut;)
        {
            cut = false;
            for (int z = 0; z < layout.depth; ++z)
            {
                for (int x = 0; x < layout.width; ++x)
                {
                    const glm::ivec2 c{x, z};
                    const uint8_t here = links[index(c)];
                    if (Count(here) == 1 && wide(c))
                    {
                        const int i = here & 1 ? 0 : here & 2 ? 1 : here & 4 ? 2 : 3;
                        links[index(c)] = 0;
                        links[index(c + kSteps[i])] &= static_cast<uint8_t>(~(1 << (i ^ 1)));
                        cut = true;
                    }
                }
            }
        }
        // Run on: from every end with more corridor ahead of it, straight on until the wall or the line again.
        for (int z = 0; z < layout.depth; ++z)
        {
            for (int x = 0; x < layout.width; ++x)
            {
                glm::ivec2 c{x, z};
                const uint8_t here = links[index(c)];
                if (!open(c) || Count(here) != 1)
                {
                    continue;
                }
                const int ahead = (here & 1 ? 0 : here & 2 ? 1 : here & 4 ? 2 : 3) ^ 1;
                for (glm::ivec2 n = c + kSteps[ahead]; open(n); c = n, n += kSteps[ahead])
                {
                    const bool joined = links[index(n)] != 0;
                    links[index(c)] |= static_cast<uint8_t>(1 << ahead);
                    links[index(n)] |= static_cast<uint8_t>(1 << (ahead ^ 1));
                    if (joined)
                    {
                        break;
                    }
                }
            }
        }
        return links;
    }

    // What hangs in the stack: a pipe (or the cable bundle) so thick, its middle so high, turning a corner on a bend so round.
    struct Layer
    {
        Kind kind;
        float thick;
        float height;
        float bend;
    };
    static constexpr float kAside = 0.9f;  // the line, off the middle of each cell along both ways
    static constexpr float kTrayHalf = 0.15f;
    static constexpr float kTray = 2.885f; // the tray's floor
    static constexpr Layer kLayers[] = {{Kind::DarkPlastic, 0.07f, kTray + 0.047f, 0.18f}, {Kind::Porcelain, 0.1f, 2.8f, 0.25f}, {Kind::Paint, 0.06f, 2.69f, 0.2f}};

    // A trapeze holding the stack up where it runs along `along`: a threaded rod down from the ceiling either side of it, and
    // between them a channel under the tray and under each pipe, each resting on its own.
    void Trapeze(const glm::vec3& at, const glm::vec3& along)
    {
        constexpr float kChannel = 0.015f;
        const glm::vec3 across{along.z, 0.0f, -along.x};
        const float lowest = kLayers[2].height - kLayers[2].thick * 0.5f;
        for (const float s : {-1.0f, 1.0f})
        {
            const glm::vec3 rod = at + across * (s * 0.18f);
            Box(Kind::DarkSteel, rod + glm::vec3(-0.008f, lowest - kChannel * 0.5f, -0.008f), rod + glm::vec3(0.008f, kClear, 0.008f));
        }
        for (const float top : {kTray, kLayers[1].height - kLayers[1].thick * 0.5f, lowest})
        {
            Box(Kind::DarkSteel, at - across * 0.2f - along * 0.02f + glm::vec3(0.0f, top - kChannel, 0.0f), at + across * 0.2f + along * 0.02f + glm::vec3(0.0f, top, 0.0f));
        }
    }

    // Along every corridor, under its ceiling, a stack of services -- a cable tray with its cables in it, a lagged pipe under it
    // and a smaller painted one under that -- on the one line through them all (Line), a little to one side of each cell's
    // middle (its lamps hang there), held up on trapezes from the ceiling. Each in it turns every corner on a bend, the tray on
    // a corner plate; a run joins another at a junction with a collar; and where the line ends it goes on into the wall ahead
    // through a plate on it. One line, so nothing in it ever crosses anything else.
    void Corridors()
    {
        struct Way
        {
            glm::vec3 to;
            bool wall; // into the wall ahead, not on into the next cell
        };
        for (int f = 0; f < layout.floors; ++f)
        {
            const std::vector<uint8_t> links = Line(f);
            for (int z = 0; z < layout.depth; ++z)
            {
                for (int x = 0; x < layout.width; ++x)
                {
                    const uint8_t here = links[static_cast<size_t>(z * layout.width + x)];
                    if (!Corridor(f, {x, z}) || here == 0)
                    {
                        continue;
                    }
                    std::vector<Way> ways;
                    for (int i = 0; i < 4; ++i)
                    {
                        if (here & (1 << i))
                        {
                            ways.push_back({glm::vec3(static_cast<float>(kSteps[i].x), 0.0f, static_cast<float>(kSteps[i].y)), false});
                        }
                    }
                    if (ways.size() == 1)
                    {
                        ways.push_back({-ways[0].to, true});
                    }
                    const glm::vec3 floor = World(f, {static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f});
                    const glm::vec3 node = floor + glm::vec3(kAside, 0.0f, kAside);
                    const bool straight = ways.size() == 2 && glm::dot(ways[0].to, ways[1].to) < -0.5f;
                    const bool corner = ways.size() == 2 && !straight;
                    // How far each way goes from the node: to the cell's edge, or to the face of the wall there.
                    const auto reach = [&](const Way& way) { return kCell * 0.5f - (way.wall ? kHalfWall : 0.0f) - kAside * (way.to.x + way.to.z); };
                    // At a junction, the line one way runs straight through and the others meet it: through along x if it goes
                    // both ways along x, or else along z.
                    const auto both = [&](const glm::vec3& axis)
                    {
                        return std::count_if(ways.begin(), ways.end(), [&](const Way& w) { return std::abs(glm::dot(w.to, axis)) > 0.5f; }) == 2;
                    };
                    const glm::vec3 main = both(glm::vec3(1.0f, 0.0f, 0.0f)) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);

                    // Held up: on a straight run over the node; round a corner, or either side of a junction, on the runs out of
                    // it, clear of the bends and of the run that joins.
                    if (straight)
                    {
                        Trapeze(node, glm::abs(ways[0].to));
                    }
                    else
                    {
                        for (const Way& way : ways)
                        {
                            if ((corner || std::abs(glm::dot(way.to, main)) > 0.5f) && reach(way) >= 0.6f)
                            {
                                Trapeze(node + way.to * 0.45f, glm::abs(way.to));
                            }
                        }
                    }

                    // The tray: along each way from the node -- from its side, where it turns or meets another, a square plate
                    // over the node there.
                    const float trayFrom = straight ? 0.0f : kTrayHalf;
                    for (const Way& way : ways)
                    {
                        const glm::vec3 side{way.to.z, 0.0f, -way.to.x};
                        const glm::vec3 from = node + way.to * trayFrom;
                        const float length = reach(way) - trayFrom;
                        if (length <= 0.001f)
                        {
                            continue;
                        }
                        const glm::vec3 mid = from + way.to * (length * 0.5f);
                        const glm::vec3 halfAlong = way.to * (length * 0.5f);
                        Box(Kind::Steel, mid - halfAlong - side * kTrayHalf + glm::vec3(0.0f, kTray, 0.0f),
                            mid + halfAlong + side * kTrayHalf + glm::vec3(0.0f, kTray + 0.012f, 0.0f));
                        for (const float s : {-1.0f, 1.0f})
                        {
                            Box(Kind::Steel, mid - halfAlong + side * (s * kTrayHalf) + glm::vec3(0.0f, kTray + 0.012f, 0.0f),
                                mid + halfAlong + side * (s * (kTrayHalf - 0.012f)) + glm::vec3(0.0f, kTray + 0.06f, 0.0f));
                        }
                        // Into the wall: through a plate on its face round all of it.
                        if (way.wall)
                        {
                            const glm::vec3 face = node + way.to * reach(way);
                            const float low = kLayers[2].height - kLayers[2].thick * 0.5f - 0.04f;
                            Box(Kind::DarkSteel, face + way.to * 0.01f - side * 0.22f + glm::vec3(0.0f, low, 0.0f),
                                face - way.to * 0.012f + side * 0.22f + glm::vec3(0.0f, kTray + 0.1f, 0.0f));
                        }
                    }
                    if (!straight)
                    {
                        Box(Kind::Steel, node + glm::vec3(-kTrayHalf, kTray, -kTrayHalf), node + glm::vec3(kTrayHalf, kTray + 0.012f, kTrayHalf));
                    }

                    // The pipes and the cable bundle: each the same way, turning a corner on a bend, meeting a junction's
                    // through run with a collar.
                    for (const Layer& layer : kLayers)
                    {
                        const glm::vec3 at = node + glm::vec3(0.0f, layer.height, 0.0f);
                        if (corner)
                        {
                            // A quarter ring in from one way and out the other (FacilityOutside::PipeBend: in from -x, out along +z).
                            glm::vec3 enter = ways[0].to;
                            glm::vec3 leave = ways[1].to;
                            if (glm::length(leave - glm::vec3(enter.z, 0.0f, -enter.x)) > 0.1f)
                            {
                                std::swap(enter, leave);
                            }
                            FacilityMap::Dressing bend;
                            bend.form = Form::Bend;
                            bend.kind = layer.kind;
                            bend.centre = at;
                            bend.size = {layer.bend, layer.thick, layer.thick};
                            bend.rotation = glm::angleAxis(std::atan2(enter.z, -enter.x), glm::vec3(0.0f, 1.0f, 0.0f));
                            out.furnishing.push_back(bend);
                        }
                        for (const Way& way : ways)
                        {
                            const bool through = !corner && std::abs(glm::dot(way.to, main)) > 0.5f;
                            const float start = corner ? layer.bend : ways.size() > 2 && !through ? layer.thick * 0.5f : 0.0f;
                            const float length = reach(way) - start;
                            if (length > 0.001f)
                            {
                                Rod(layer.kind, at + way.to * (start + length * 0.5f), layer.thick, length, std::abs(way.to.x) > 0.5f);
                            }
                            // Where a branch meets the through run: a collar round it at the join.
                            if (ways.size() > 2 && !through)
                            {
                                Rod(Kind::DarkSteel, at + way.to * (layer.thick * 0.5f + 0.03f), layer.thick * 1.35f, 0.06f, std::abs(way.to.x) > 0.5f);
                            }
                        }
                    }
                }
            }
        }
    }

    void Rooms()
    {
        for (const FacilityLayout::Room& room : layout.rooms)
        {
            const glm::ivec2 size = room.max - room.min + glm::ivec2(1);
            // An air diffuser in its ceiling, at whichever half-cell mark of it is furthest from its lamps.
            glm::vec2 spot = glm::vec2(room.min) + glm::vec2(0.5f);
            float clearest = -1.0f;
            for (int i = 1; i < size.x * 2; ++i)
            {
                for (int k = 1; k < size.y * 2; ++k)
                {
                    const glm::vec2 here = glm::vec2(room.min) + glm::vec2(static_cast<float>(i), static_cast<float>(k)) * 0.5f;
                    float nearest = 1.0e9f;
                    for (const FacilityLayout::Lamp& lamp : layout.lamps)
                    {
                        if (lamp.floor == room.floor && layout.RoomAt(lamp.floor, static_cast<int>(lamp.at.x), static_cast<int>(lamp.at.y)) >= 0 &&
                            lamp.at.x >= static_cast<float>(room.min.x) && lamp.at.x <= static_cast<float>(room.max.x + 1) &&
                            lamp.at.y >= static_cast<float>(room.min.y) && lamp.at.y <= static_cast<float>(room.max.y + 1))
                        {
                            nearest = std::min(nearest, glm::length(here - lamp.at));
                        }
                    }
                    if (nearest > clearest)
                    {
                        clearest = nearest;
                        spot = here;
                    }
                }
            }
            const glm::vec3 at = World(room.floor, spot) + glm::vec3(0.0f, kClear, 0.0f);
            Box(Kind::Steel, at - glm::vec3(0.3f, 0.025f, 0.3f), at + glm::vec3(0.3f, 0.0f, 0.3f));
            for (int s = 0; s < 4; ++s)
            {
                const float u = -0.21f + 0.14f * static_cast<float>(s);
                Box(Kind::DarkSteel, at + glm::vec3(u - 0.04f, -0.03f, -0.24f), at + glm::vec3(u + 0.04f, -0.025f, 0.24f));
            }
            // A plant room's big pipes, along its longer way, on hangers from its ceiling.
            if (room.purpose != FacilityLayout::Purpose::Plant)
            {
                continue;
            }
            const bool alongX = size.x >= size.y;
            const glm::vec3 lo = World(room.floor, glm::vec2(room.min));
            const glm::vec3 hi = World(room.floor, glm::vec2(room.max + glm::ivec2(1)));
            const float length = (alongX ? hi.x - lo.x : hi.z - lo.z) - 2.0f * kHalfWall;
            for (const float offset : {0.7f, 1.2f})
            {
                const glm::vec3 middle = alongX ? glm::vec3((lo.x + hi.x) * 0.5f, lo.y + kOver - 0.1f, lo.z + kHalfWall + offset)
                                                : glm::vec3(lo.x + kHalfWall + offset, lo.y + kOver - 0.1f, (lo.z + hi.z) * 0.5f);
                Rod(offset < 1.0f ? Kind::Paint : Kind::Steel, middle, 0.3f, length, alongX);
                // Hung every cell or so: a clamp round it, and a rod from the top of that up to the ceiling.
                for (float t = -length * 0.5f + 1.0f; t < length * 0.5f; t += kCell)
                {
                    const glm::vec3 hang = middle + (alongX ? glm::vec3(t, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, t));
                    Rod(Kind::DarkSteel, hang, 0.33f, 0.05f, alongX);
                    Box(Kind::DarkSteel, hang + glm::vec3(-0.012f, 0.16f, -0.012f), hang + glm::vec3(0.012f, kClear - kOver + 0.1f, 0.012f));
                }
            }
        }
    }

    void Vents()
    {
        // Each mouth of the crawlspace: a frame round it on the open side, its grille taken off and lying on the floor beside it.
        for (const FacilityLayout::Duct& duct : layout.ducts)
        {
            for (const FacilityLayout::Mouth& mouth : duct.mouths)
            {
                const glm::vec2 step = glm::vec2(mouth.open - mouth.duct);
                const glm::vec3 n{step.x, 0.0f, step.y};
                const glm::vec3 a{-step.y, 0.0f, step.x};
                const glm::vec3 edge = World(duct.floor, (glm::vec2(mouth.duct) + glm::vec2(mouth.open)) * 0.5f + glm::vec2(0.5f));
                const glm::vec3 face = edge + n * kHalfWall;
                const float high = FacilityLayout::kDuctHeight;
                constexpr float kHalf = 0.5f;
                constexpr float kBar = 0.06f;
                for (const float s : {-1.0f, 1.0f})
                {
                    Box(Kind::Steel, face + a * (s * kHalf), face + a * (s * (kHalf + kBar)) + n * 0.03f + glm::vec3(0.0f, high + kBar, 0.0f));
                }
                Box(Kind::Steel, face - a * kHalf + glm::vec3(0.0f, high, 0.0f), face + a * kHalf + n * 0.03f + glm::vec3(0.0f, high + kBar, 0.0f));
                // The grille, on the floor beside it -- whichever side is floor, inside the same space -- a frame and its slats.
                const auto onFloor = [&](const glm::vec3& at)
                {
                    const glm::vec3 local = at - World(duct.floor, glm::vec2(0.0f));
                    const int cx = static_cast<int>(std::floor(local.x / kCell));
                    const int cz = static_cast<int>(std::floor(local.z / kCell));
                    return cx >= 0 && cz >= 0 && cx < layout.width && cz < layout.depth && layout.At(duct.floor, cx, cz) == layout.At(duct.floor, mouth.open.x, mouth.open.y) &&
                           layout.roomOf[layout.Index(duct.floor, cx, cz)] == layout.roomOf[layout.Index(duct.floor, mouth.open.x, mouth.open.y)];
                };
                glm::vec3 lying = face + n * 0.55f + a * (kHalf + 0.75f);
                if (!onFloor(lying + a * 0.5f))
                {
                    lying = face + n * 0.55f - a * (kHalf + 0.75f);
                    if (!onFloor(lying - a * 0.5f))
                    {
                        continue;
                    }
                }
                Box(Kind::DarkSteel, lying - a * 0.48f - n * 0.4f, lying + a * 0.48f + n * 0.4f + glm::vec3(0.0f, 0.012f, 0.0f));
                for (int s = 0; s < 6; ++s)
                {
                    const float u = -0.36f + 0.144f * static_cast<float>(s);
                    Box(Kind::Steel, lying + a * (u - 0.025f) - n * 0.36f + glm::vec3(0.0f, 0.012f, 0.0f), lying + a * (u + 0.025f) + n * 0.36f + glm::vec3(0.0f, 0.03f, 0.0f));
                }
            }
        }
    }
};

} // namespace

void Fit(const FacilityLayout& layout, FacilityMap::Blueprint& out)
{
    Fitter fitter{layout, out};
    fitter.Corridors();
    fitter.Rooms();
    fitter.Vents();
}

} // namespace FacilityServices

} // namespace pred
