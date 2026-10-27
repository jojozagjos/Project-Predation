#include "Game/World/FacilityServices.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
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

    bool Corridor(int f, int x, int z) const
    {
        return x >= 0 && z >= 0 && x < layout.width && z < layout.depth && layout.At(f, x, z) == Cell::Corridor;
    }

    // Which way a corridor runs through a cell: through it along x and not z, or z and not x; or neither (a corner, a junction,
    // an end). -1 neither, 0 along x, 1 along z.
    int RunsAlong(int f, int x, int z) const
    {
        const bool xBoth = Corridor(f, x - 1, z) && Corridor(f, x + 1, z);
        const bool zBoth = Corridor(f, x, z - 1) && Corridor(f, x, z + 1);
        return xBoth == zBoth ? -1 : xBoth ? 0 : 1;
    }

    // One of what hangs in the stack: a pipe (or the cable bundle) so thick, its middle so high.
    struct Layer
    {
        Kind kind;
        float thick;
        float height;
        float bend; // the radius it turns a corner on
    };

    // Along every corridor, under its ceiling, a stack of services hung on rods -- a cable tray with its cables on top, a lagged
    // pipe under it and a smaller painted one under that -- one line through the whole corridor, a little to one side of its
    // middle (its lamps hang there), each in it turning every corner on a bend, the tray on a corner plate; meeting the run it
    // joins at a junction with a collar; capped where the corridor ends. One line, so nothing in it ever crosses anything else.
    void Corridors()
    {
        constexpr float kAside = 0.9f;                  // the line, off the middle of each cell along both ways
        constexpr float kTrayHalf = 0.15f;
        constexpr float kTray = 2.885f;                 // the tray's floor
        const Layer kCables{Kind::DarkPlastic, 0.07f, kTray + 0.047f, 0.18f};
        const Layer kLayers[] = {kCables, {Kind::Porcelain, 0.1f, 2.8f, 0.25f}, {Kind::Paint, 0.06f, 2.7f, 0.2f}};
        const glm::ivec2 kWays[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int f = 0; f < layout.floors; ++f)
        {
            for (int z = 0; z < layout.depth; ++z)
            {
                for (int x = 0; x < layout.width; ++x)
                {
                    if (!Corridor(f, x, z))
                    {
                        continue;
                    }
                    // Which ways the line goes on from here: to each corridor next door along the way both of them run (a wide
                    // corridor's two halves each with a line of their own, not one across between them every cell).
                    const int here = RunsAlong(f, x, z);
                    std::vector<glm::vec3> ways;
                    for (const glm::ivec2 way : kWays)
                    {
                        const int axis = way.x != 0 ? 0 : 1;
                        const int there = Corridor(f, x + way.x, z + way.y) ? RunsAlong(f, x + way.x, z + way.y) : -2;
                        if (there != -2 && (here == axis || here == -1) && (there == axis || there == -1))
                        {
                            ways.emplace_back(static_cast<float>(way.x), 0.0f, static_cast<float>(way.y));
                        }
                    }
                    if (ways.empty())
                    {
                        continue;
                    }
                    const glm::vec3 floor = World(f, {static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f});
                    const glm::vec3 node = floor + glm::vec3(kAside, 0.0f, kAside);
                    const bool straight = ways.size() == 2 && glm::dot(ways[0], ways[1]) < -0.5f;
                    const bool corner = ways.size() == 2 && !straight;
                    // At a junction, the line one way runs straight through and the others meet it: through along x if it goes
                    // both ways along x, or else along z.
                    const auto both = [&](const glm::vec3& axis)
                    {
                        return std::count_if(ways.begin(), ways.end(), [&](const glm::vec3& w) { return std::abs(glm::dot(w, axis)) > 0.5f; }) == 2;
                    };
                    const glm::vec3 main = both(glm::vec3(1.0f, 0.0f, 0.0f)) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);

                    // A rod down from the ceiling holding the stack, a bar under each of it.
                    Box(Kind::DarkSteel, node + glm::vec3(-0.012f, kLayers[2].height - 0.04f, -0.012f), node + glm::vec3(0.012f, kClear, 0.012f));
                    for (const Layer& layer : kLayers)
                    {
                        const float under = layer.height - layer.thick * 0.5f - 0.012f;
                        Box(Kind::DarkSteel, node + glm::vec3(-0.12f, under, -0.12f), node + glm::vec3(0.12f, under + 0.012f, 0.12f));
                    }

                    // The tray: along each way from the node to the cell's edge -- from its side, where it turns or meets
                    // another, a square plate over the node there.
                    const float trayFrom = straight ? 0.0f : kTrayHalf;
                    for (const glm::vec3& way : ways)
                    {
                        const glm::vec3 side{way.z, 0.0f, -way.x};
                        const glm::vec3 to = floor + glm::vec3(kAside, 0.0f, kAside) * (glm::vec3(1.0f) - glm::abs(way)) + way * (kCell * 0.5f);
                        const glm::vec3 from = node + way * trayFrom;
                        const float length = glm::dot(to - from, way);
                        if (length <= 0.001f)
                        {
                            continue;
                        }
                        const glm::vec3 mid = (from + to) * 0.5f;
                        const glm::vec3 halfAlong = way * (length * 0.5f);
                        Box(Kind::Steel, mid - halfAlong - side * kTrayHalf + glm::vec3(0.0f, kTray, 0.0f),
                            mid + halfAlong + side * kTrayHalf + glm::vec3(0.0f, kTray + 0.012f, 0.0f));
                        for (const float s : {-1.0f, 1.0f})
                        {
                            Box(Kind::Steel, mid - halfAlong + side * (s * kTrayHalf) + glm::vec3(0.0f, kTray + 0.012f, 0.0f),
                                mid + halfAlong + side * (s * (kTrayHalf - 0.012f)) + glm::vec3(0.0f, kTray + 0.06f, 0.0f));
                        }
                    }
                    if (!straight)
                    {
                        Box(Kind::Steel, node + glm::vec3(-kTrayHalf, kTray, -kTrayHalf), node + glm::vec3(kTrayHalf, kTray + 0.012f, kTrayHalf));
                    }

                    // The pipes and the cable bundle: each the same way, turning a corner on a bend, meeting a junction's
                    // through run with a collar, capped at an end.
                    for (const Layer& layer : kLayers)
                    {
                        const glm::vec3 at = node + glm::vec3(0.0f, layer.height, 0.0f);
                        if (corner)
                        {
                            // A quarter ring in from one way and out the other (FacilityOutside::PipeBend: in from -x, out along +z).
                            glm::vec3 enter = ways[0];
                            glm::vec3 leave = ways[1];
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
                        for (const glm::vec3& way : ways)
                        {
                            const bool through = !corner && std::abs(glm::dot(way, main)) > 0.5f;
                            const float start = corner ? layer.bend : ways.size() > 2 && !through ? layer.thick * 0.5f : 0.0f;
                            const glm::vec3 to = at + way * (kCell * 0.5f - kAside * (way.x + way.z)); // the cell's edge that way
                            const glm::vec3 from = at + way * start;
                            const float length = glm::dot(to - from, way);
                            if (length > 0.001f)
                            {
                                Rod(layer.kind, (from + to) * 0.5f, layer.thick, length, std::abs(way.x) > 0.5f);
                            }
                            // Where a branch meets the through run: a collar round it at the join.
                            if (ways.size() > 2 && !through)
                            {
                                Rod(Kind::DarkSteel, at + way * (layer.thick * 0.5f + 0.03f), layer.thick * 1.35f, 0.06f, std::abs(way.x) > 0.5f);
                            }
                        }
                        if (ways.size() == 1)
                        {
                            // An end: a cap on it.
                            Rod(Kind::DarkSteel, at - ways[0] * 0.01f, layer.thick * 1.3f, 0.04f, std::abs(ways[0].x) > 0.5f);
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
                // Hangers every cell or so.
                for (float t = -length * 0.5f + 1.0f; t < length * 0.5f; t += kCell)
                {
                    const glm::vec3 hang = middle + (alongX ? glm::vec3(t, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, t));
                    Box(Kind::DarkSteel, hang + glm::vec3(-0.015f, 0.15f, -0.015f), hang + glm::vec3(0.015f, kClear - kOver + 0.1f, 0.015f));
                    Rod(Kind::DarkSteel, hang + glm::vec3(0.0f, 0.16f, 0.0f), 0.025f, 0.34f, !alongX);
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
