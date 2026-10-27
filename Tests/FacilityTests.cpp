#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/SitePlan.h"
#include "Engine/Navigation/NavMesh.h"
#include "Engine/Render/Primitives.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

using namespace pred;

// The generated facility, as a plan: the same seed, the same building; all of it reachable; loops;
// locked doors that cut nothing off; and vents that join things up.

TEST_CASE("The same seed plans the same facility, and another seed another one", "[facility]")
{
    const FacilityLayout a = FacilityLayout::Generate(1234);
    const FacilityLayout b = FacilityLayout::Generate(1234);
    const FacilityLayout c = FacilityLayout::Generate(1235);
    CHECK(a.cells == b.cells);
    CHECK(a.doors.size() == b.doors.size());
    CHECK(a.things.size() == b.things.size());
    CHECK(a.cells != c.cells);
}

TEST_CASE("Every facility can be walked from the way in, round loops, with nothing locked away that matters",
          "[facility]")
{
    int withDucts = 0;
    int withLoops = 0;
    for (uint32_t seed = 1; seed <= 40; ++seed)
    {
        INFO("seed " << seed);
        const FacilityLayout plan = FacilityLayout::Generate(seed);
        REQUIRE(plan.floors >= 2);
        REQUIRE(plan.entranceRoom >= 0);
        CHECK(plan.nestRoom >= 0);
        CHECK(plan.nestRoom != plan.entranceRoom);
        CHECK(plan.rooms.size() >= 12);

        // Every floor reached by a stairwell from the one below.
        for (int f = 0; f + 1 < plan.floors; ++f)
        {
            CHECK(std::any_of(plan.stairwells.begin(), plan.stairwells.end(),
                              [&](const FacilityLayout::Stairwell& well) { return well.floor == f; }));
        }

        // Through every door, everything; through only the unlocked ones, everything but the rooms
        // behind the locked ones.
        const std::vector<bool> all = plan.Reachable(true);
        const std::vector<bool> open = plan.Reachable(false);
        for (size_t r = 0; r < plan.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = plan.rooms[r];
            CHECK(all[plan.Index(room.floor, room.min.x, room.min.y)]);
            const bool lockedIn = std::any_of(plan.doors.begin(), plan.doors.end(), [&](const FacilityLayout::Door& d)
                                              { return d.locked && d.room == static_cast<int>(r); });
            if (!lockedIn)
            {
                CHECK(open[plan.Index(room.floor, room.min.x, room.min.y)]);
            }
        }
        for (const FacilityLayout::Stairwell& well : plan.stairwells)
        {
            CHECK(open[plan.Index(well.floor, well.min.x, well.min.y)]);
            CHECK(open[plan.Index(well.floor + 1, well.min.x, well.min.y)]);
        }
        // The keycard, when anything is locked, somewhere you can get to without it.
        if (std::any_of(plan.doors.begin(), plan.doors.end(), [](const FacilityLayout::Door& d) { return d.locked; }))
        {
            bool found = false;
            for (const FacilityLayout::Placed& thing : plan.things)
            {
                if (thing.thing == FacilityLayout::Thing::Keycard)
                {
                    found = true;
                    CHECK(open[plan.Index(thing.floor, static_cast<int>(thing.at.x), static_cast<int>(thing.at.y))]);
                }
            }
            CHECK(found);
        }

        // A loop: some doorway that can be taken away and everything is still reachable.
        for (size_t d = 0; d < plan.doors.size() && withLoops < static_cast<int>(seed); ++d)
        {
            FacilityLayout without = plan;
            without.doors.erase(without.doors.begin() + static_cast<long>(d));
            const std::vector<bool> still = without.Reachable(true);
            bool whole = true;
            for (const FacilityLayout::Room& room : plan.rooms)
            {
                whole = whole && still[plan.Index(room.floor, room.min.x, room.min.y)];
            }
            if (whole)
            {
                ++withLoops;
            }
        }

        // The vents: every duct cell is a duct, and every mouth opens onto a room or a corridor.
        for (const FacilityLayout::Duct& duct : plan.ducts)
        {
            CHECK(duct.cells.size() >= 2);
            CHECK_FALSE(duct.mouths.empty());
            for (const glm::ivec2 cell : duct.cells)
            {
                CHECK(plan.At(duct.floor, cell.x, cell.y) == FacilityLayout::Cell::Duct);
            }
            for (const FacilityLayout::Mouth& mouth : duct.mouths)
            {
                const FacilityLayout::Cell there = plan.At(duct.floor, mouth.open.x, mouth.open.y);
                CHECK((there == FacilityLayout::Cell::Room || there == FacilityLayout::Cell::Corridor));
                CHECK(std::abs(mouth.open.x - mouth.duct.x) + std::abs(mouth.open.y - mouth.duct.y) == 1);
            }
        }
        withDucts += plan.ducts.size() >= static_cast<size_t>(plan.floors) ? 1 : 0;
    }
    // Nearly every facility has loops and a vent or more a floor.
    CHECK(withLoops >= 36);
    CHECK(withDucts >= 32);
}

TEST_CASE("Print a facility, floor by floor", "[.facilitymap]")
{
    const FacilityLayout plan = FacilityLayout::Generate(1);
    std::string text;
    for (int f = 0; f < plan.floors; ++f)
    {
        text += "floor " + std::to_string(f) + "\n";
        for (int z = 0; z < plan.depth; ++z)
        {
            for (int x = 0; x < plan.width; ++x)
            {
                char c = ' ';
                switch (plan.At(f, x, z))
                {
                case FacilityLayout::Cell::Solid: c = '#'; break;
                case FacilityLayout::Cell::Room:
                {
                    const int r = plan.RoomAt(f, x, z);
                    c = r == plan.entranceRoom ? 'E' : r == plan.nestRoom ? 'N' : static_cast<char>('a' + r % 26);
                    break;
                }
                case FacilityLayout::Cell::Corridor: c = '.'; break;
                case FacilityLayout::Cell::Stair: c = 'S'; break;
                case FacilityLayout::Cell::Duct: c = '~'; break;
                case FacilityLayout::Cell::Outside: c = ' '; break;
                }
                text += c;
                text += c;
            }
            text += "\n";
        }
    }
    WARN(text);
}

namespace
{

struct Solid
{
    glm::vec3 lo{0.0f};
    glm::vec3 hi{0.0f};
    std::string what;
};

// The box a thing turned by `yaw` takes up, axis-aligned. Exact for the quarter turns everything against
// a wall is at; for a crate turned any other way, the box round it, which is only ever bigger.
Solid Around(const glm::vec3& centre, const glm::vec3& size, float yaw, std::string what)
{
    const float c = std::abs(std::cos(yaw));
    const float s = std::abs(std::sin(yaw));
    const glm::vec3 half{(size.x * c + size.z * s) * 0.5f, size.y * 0.5f, (size.x * s + size.z * c) * 0.5f};
    // Quarter turns come out a hair bigger from the sine and cosine; not enough to count.
    const glm::vec3 slack{0.0005f, 0.0f, 0.0005f};
    return {centre - half + slack, centre + half - slack, std::move(what)};
}

bool Overlap(const Solid& a, const Solid& b, float by)
{
    return a.hi.x - b.lo.x > by && b.hi.x - a.lo.x > by && a.hi.y - b.lo.y > by && b.hi.y - a.lo.y > by &&
           a.hi.z - b.lo.z > by && b.hi.z - a.lo.z > by;
}

// The building's structure joining up is not anything being inside anything else (Piece::Structural). What must
// never overlap is everything else, with the building or with each other.
std::vector<Solid> SolidsOf(const FacilityMap::Blueprint& blueprint)
{
    std::vector<Solid> solids;
    for (const FacilityMap::Piece& piece : blueprint.pieces)
    {
        solids.push_back(Around(piece.centre, piece.size, piece.yaw,
                                (piece.Structural() ? "structure " : "piece ") + std::to_string(static_cast<int>(piece.kind))));
    }
    for (const FacilityMap::Flight& flight : blueprint.flights)
    {
        const float length = FacilityMap::kSteps * FacilityMap::kStepRun;
        const glm::vec3 along{std::sin(flight.yaw), 0.0f, std::cos(flight.yaw)};
        const glm::vec3 middle = flight.foot + along * (length * 0.5f) + glm::vec3(0.0f, FacilityLayout::kStorey * 0.5f, 0.0f);
        solids.push_back(Around(middle, {FacilityMap::kStairWidth, FacilityLayout::kStorey, length}, flight.yaw, "stairs"));
    }
    for (const WorldObjects::PlacedThing& locker : blueprint.placements.lockers)
    {
        solids.push_back(Around(locker.position + glm::vec3(0.0f, 1.025f, 0.0f), {1.06f, 2.05f, 0.88f}, locker.yaw, "locker"));
    }
    for (const WorldObjects::PlacedThing& crate : blueprint.placements.ammoCrates)
    {
        solids.push_back(Around(crate.position + glm::vec3(0.0f, 0.22f, 0.0f), {0.72f, 0.44f, 0.46f}, crate.yaw, "ammo crate"));
    }
    return solids;
}

} // namespace

TEST_CASE("A built facility has nothing solid inside anything else", "[facility]")
{
    for (uint32_t seed = 1; seed <= 12; ++seed)
    {
        INFO("seed " << seed);
        const FacilityMap::Blueprint blueprint = FacilityMap::Draw(FacilityLayout::Generate(seed));
        const std::vector<Solid> solids = SolidsOf(blueprint);
        CHECK(solids.size() > 200);
        int overlaps = 0;
        for (size_t i = 0; i < solids.size(); ++i)
        {
            for (size_t k = i + 1; k < solids.size(); ++k)
            {
                const bool bothStructure = solids[i].what.rfind("structure", 0) == 0 && solids[k].what.rfind("structure", 0) == 0;
                if (!bothStructure && Overlap(solids[i], solids[k], 0.002f))
                {
                    if (overlaps < 5)
                    {
                        UNSCOPED_INFO(solids[i].what << " into " << solids[k].what << " at " << solids[i].lo.x << " "
                                                     << solids[i].lo.y << " " << solids[i].lo.z);
                    }
                    ++overlaps;
                }
            }
        }
        CHECK(overlaps == 0);
    }
}

TEST_CASE("A built facility's way in is clear to stand in, and every door fits its doorway", "[facility]")
{
    for (uint32_t seed = 1; seed <= 12; ++seed)
    {
        INFO("seed " << seed);
        const FacilityLayout layout = FacilityLayout::Generate(seed);
        const FacilityMap::Blueprint blueprint = FacilityMap::Draw(layout);
        const std::vector<Solid> solids = SolidsOf(blueprint);

        // Somebody standing at the spawn: a column as wide as a person, from their feet to over their head.
        const glm::vec3 feet = blueprint.spawn - glm::vec3(0.0f, 0.5f, 0.0f);
        const Solid person{feet + glm::vec3(-0.35f, 0.05f, -0.35f), feet + glm::vec3(0.35f, 1.9f, 0.35f), "person"};
        for (const Solid& solid : solids)
        {
            INFO(solid.what);
            CHECK_FALSE(Overlap(person, solid, 0.0f));
        }

        // Every door, shut: its panel in its doorway, touching nothing.
        for (const WorldObjects::PlacedDoor& door : blueprint.placements.doors)
        {
            const glm::vec3 along{std::cos(door.closedYaw), 0.0f, -std::sin(door.closedYaw)};
            const glm::vec3 middle = door.hinge + along * (door.width * 0.5f) + glm::vec3(0.0f, door.height * 0.5f, 0.0f);
            const Solid panel = Around(middle, {door.width, door.height, 0.09f}, door.closedYaw, "door");
            for (const Solid& solid : solids)
            {
                INFO(solid.what);
                CHECK_FALSE(Overlap(panel, solid, 0.0f));
            }
        }
        CHECK(blueprint.placements.doors.size() + 40 < 256); // door numbers go over the network in a byte
    }
}

TEST_CASE("Every room of a built facility can be walked to from the way in, up and down its stairs", "[facility][nav]")
{
    for (const uint32_t seed : {1u, 7u})
    {
        INFO("seed " << seed);
        const FacilityLayout layout = FacilityLayout::Generate(seed);
        const FacilityMap::Blueprint blueprint = FacilityMap::Draw(layout);

        // The level as the navigation sees it: every solid piece and every flight, as triangles. The doors
        // are not in it -- they are moved, not walked round -- so this is the building with them all open.
        MeshData level;
        for (const FacilityMap::Piece& piece : blueprint.pieces)
        {
            const glm::mat4 transform = glm::translate(glm::mat4(1.0f), piece.centre) *
                                        glm::mat4_cast(glm::angleAxis(piece.yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
            level.Append(Primitives::Box(piece.size), transform);
        }
        const MeshData stairs = Primitives::Stairs(FacilityMap::kSteps, FacilityMap::kStairWidth,
                                                   FacilityLayout::kStorey / FacilityMap::kSteps, FacilityMap::kStepRun);
        for (const FacilityMap::Flight& flight : blueprint.flights)
        {
            const glm::mat4 transform = glm::translate(glm::mat4(1.0f), flight.foot) *
                                        glm::mat4_cast(glm::angleAxis(flight.yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
            level.Append(stairs, transform);
        }
        std::vector<glm::vec3> triangles;
        for (const uint32_t index : level.indices)
        {
            triangles.push_back(level.vertices[index].position);
        }
        NavMesh nav;
        std::string error;
        REQUIRE(nav.Build(triangles, NavSettings{}, &error));

        int unreachable = 0;
        for (size_t r = 0; r < layout.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = layout.rooms[r];
            // A cell of the room with nothing standing in it: its middle, or failing that its corners'.
            const glm::vec3 target = FacilityMap::ToWorld(room.floor, glm::vec2(room.min) + glm::vec2(0.5f)) + glm::vec3(0.0f, 0.1f, 0.0f);
            std::vector<glm::vec3> corners;
            bool reached = false;
            nav.FindPath(blueprint.spawn, target, corners, &reached, nullptr, 0);
            if (!reached || corners.empty() || glm::distance(corners.back(), target) > 1.5f)
            {
                UNSCOPED_INFO("room " << r << " on floor " << room.floor << " at " << room.min.x << "," << room.min.y);
                ++unreachable;
            }
        }
        CHECK(unreachable == 0);
    }
}

TEST_CASE("Print a facility's rooms and lamps", "[.facilitylamps]")
{
    const FacilityLayout plan = FacilityLayout::Generate(1);
    const FacilityMap::Blueprint blueprint = FacilityMap::Draw(plan);
    std::string text;
    for (size_t r = 0; r < plan.rooms.size(); ++r)
    {
        const FacilityLayout::Room& room = plan.rooms[r];
        const glm::vec3 a = FacilityMap::ToWorld(room.floor, glm::vec2(room.min));
        const glm::vec3 b = FacilityMap::ToWorld(room.floor, glm::vec2(room.max + glm::ivec2(1)));
        text += "room " + std::to_string(r) + " floor " + std::to_string(room.floor) + " x " + std::to_string(a.x) + ".." +
                std::to_string(b.x) + " z " + std::to_string(a.z) + ".." + std::to_string(b.z) + "\n";
    }
    for (const FacilityMap::Lamp& lamp : blueprint.lamps)
    {
        text += std::string(lamp.spillOf >= 0 ? "spill" : lamp.copyOf >= 0 ? "copy " : "lamp ") + " at " + std::to_string(lamp.position.x) + "," +
                std::to_string(lamp.position.y) + "," + std::to_string(lamp.position.z) + " box x " + std::to_string(lamp.boundsMin.x) + ".." +
                std::to_string(lamp.boundsMax.x) + " z " + std::to_string(lamp.boundsMin.z) + ".." + std::to_string(lamp.boundsMax.z) + "\n";
    }
    WARN(text);
}

TEST_CASE("Print a facility's doorways", "[.facilitydoors]")
{
    const FacilityLayout plan = FacilityLayout::Generate(1);
    std::string text;
    for (const FacilityLayout::Door& door : plan.doors)
    {
        const glm::vec2 edge = glm::vec2(door.cell) + glm::vec2(0.5f) + (door.side == 0 ? glm::vec2(0.5f, 0.0f) : glm::vec2(0.0f, 0.5f));
        const glm::vec3 at = FacilityMap::ToWorld(door.floor, edge);
        text += "floor " + std::to_string(door.floor) + " at " + std::to_string(at.x) + "," + std::to_string(at.z) +
                (door.hasDoor ? " door" : " arch") + (door.locked ? " locked" : "") + " room " + std::to_string(door.room) + "\n";
    }
    WARN(text);
}

TEST_CASE("A stairwell's walls run unbroken from its floor up to the floor over it, with nothing showing between", "[facility]")
{
    using Kind = FacilityMap::Piece::Kind;
    // How far a wall's face is from the middle of the cell it stands round: half a cell, less half the wall.
    constexpr float kToFace = FacilityLayout::kCell * 0.5f - 0.1f;
    int edges = 0;
    int cracks = 0;
    for (uint32_t seed = 1; seed <= 8; ++seed)
    {
        INFO("seed " << seed);
        const FacilityLayout layout = FacilityLayout::Generate(seed);
        const FacilityMap::Blueprint blueprint = FacilityMap::Draw(layout);
        std::vector<Solid> building;
        for (const FacilityMap::Piece& piece : blueprint.pieces)
        {
            if (piece.kind == Kind::Floor || piece.kind == Kind::Ceiling || piece.kind == Kind::Wall || piece.kind == Kind::Fill ||
                piece.kind == Kind::Cladding || piece.kind == Kind::Sill)
            {
                building.push_back(Around(piece.centre, piece.size, piece.yaw, ""));
            }
        }
        // Every edge with a way through it, by floor, the cell on its low side, and which way it runs.
        std::set<std::tuple<int, int, int, int>> openings;
        for (const FacilityLayout::Door& door : layout.doors)
        {
            openings.insert({door.floor, door.cell.x, door.cell.y, door.side});
        }
        for (const FacilityLayout::Exit& exit : layout.exits)
        {
            const int x = exit.side == 0 ? exit.cell.x - 1 : exit.cell.x;
            const int z = exit.side == 2 ? exit.cell.y - 1 : exit.cell.y;
            openings.insert({0, x, z, exit.side < 2 ? 0 : 1});
        }
        for (const FacilityLayout::Duct& duct : layout.ducts)
        {
            for (const FacilityLayout::Mouth& mouth : duct.mouths)
            {
                const glm::ivec2 low = glm::min(mouth.duct, mouth.open);
                openings.insert({duct.floor, low.x, low.y, mouth.duct.x != mouth.open.x ? 0 : 1});
            }
        }
        const glm::ivec2 ways[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int f = 0; f + 1 < layout.floors; ++f)
        {
            for (int z = 0; z < layout.depth; ++z)
            {
                for (int x = 0; x < layout.width; ++x)
                {
                    if (layout.At(f, x, z) != FacilityLayout::Cell::Stair)
                    {
                        continue;
                    }
                    for (const glm::ivec2 way : ways)
                    {
                        const glm::ivec2 next{x + way.x, z + way.y};
                        const bool inside = next.x >= 0 && next.y >= 0 && next.x < layout.width && next.y < layout.depth;
                        if (inside && layout.At(f, next.x, next.y) == FacilityLayout::Cell::Stair)
                        {
                            continue;
                        }
                        const glm::ivec2 low = glm::min(glm::ivec2(x, z), next);
                        const int side = way.x != 0 ? 0 : 1;
                        if (openings.contains({f, low.x, low.y, side}) || openings.contains({f + 1, low.x, low.y, side}))
                        {
                            continue;
                        }
                        ++edges;
                        // From the middle of the stairwell, straight at the wall, every few millimetres of the way up.
                        const glm::vec3 middle = FacilityMap::ToWorld(layout, f, glm::vec2(x, z) + glm::vec2(0.5f));
                        const glm::vec3 along{static_cast<float>(way.x), 0.0f, static_cast<float>(way.y)};
                        std::vector<const Solid*> near;
                        const glm::vec3 lo = glm::min(middle, middle + along * 2.0f);
                        const glm::vec3 hi = glm::max(middle, middle + along * 2.0f) + glm::vec3(0.0f, FacilityLayout::kStorey, 0.0f);
                        for (const Solid& solid : building)
                        {
                            if (solid.lo.x <= hi.x && solid.hi.x >= lo.x && solid.lo.y <= hi.y && solid.hi.y >= lo.y && solid.lo.z <= hi.z &&
                                solid.hi.z >= lo.z)
                            {
                                near.push_back(&solid);
                            }
                        }
                        for (float h = 0.05f; h < FacilityLayout::kStorey - 0.002f; h += 0.004f)
                        {
                            const glm::vec3 from = middle + glm::vec3(0.0f, h, 0.0f);
                            float nearest = 10.0f;
                            bool buried = false;
                            for (const Solid* solid : near)
                            {
                                if (from.y <= solid->lo.y || from.y >= solid->hi.y)
                                {
                                    continue;
                                }
                                const float a = way.x != 0 ? from.x : from.z;
                                const float sLo = way.x != 0 ? solid->lo.x : solid->lo.z;
                                const float sHi = way.x != 0 ? solid->hi.x : solid->hi.z;
                                const float across = way.x != 0 ? from.z : from.x;
                                const float cLo = way.x != 0 ? solid->lo.z : solid->lo.x;
                                const float cHi = way.x != 0 ? solid->hi.z : solid->hi.x;
                                if (across <= cLo || across >= cHi)
                                {
                                    continue;
                                }
                                if (a > sLo && a < sHi)
                                {
                                    buried = true; // inside a landing's slab: nothing to see from here
                                    break;
                                }
                                const float distance = (way.x + way.y) > 0 ? sLo - a : a - sHi;
                                if (distance >= 0.0f)
                                {
                                    nearest = std::min(nearest, distance);
                                }
                            }
                            if (!buried && std::abs(nearest - kToFace) > 0.002f)
                            {
                                if (cracks < 5)
                                {
                                    UNSCOPED_INFO("seed " << seed << " floor " << f << " cell " << x << "," << z << " looking " << way.x << ","
                                                          << way.y << ": " << h << " up, the wall is " << nearest << " away");
                                }
                                ++cracks;
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(edges > 20);
    CHECK(cracks == 0);
}

TEST_CASE("A building's outside is one flat face a side, from the ground to the roof, with no floor showing through it", "[facility]")
{
    for (uint32_t seed = 1; seed <= 6; ++seed)
    {
        INFO("seed " << seed);
        FacilityLayout::Options options;
        options.width = 14;
        options.depth = 12;
        options.minFloors = 2;
        options.maxFloors = 3;
        options.exits = 2;
        const FacilityLayout layout = FacilityLayout::Generate(seed, options);
        const FacilityMap::Blueprint blueprint = FacilityMap::Draw(layout);
        std::vector<Solid> building;
        for (const FacilityMap::Piece& piece : blueprint.pieces)
        {
            building.push_back(Around(piece.centre, piece.size, piece.yaw, ""));
        }
        const glm::vec3 corner = FacilityMap::ToWorld(layout, 0, glm::vec2(0.0f));
        const float bottom = corner.y + 0.05f;
        const float top = FacilityMap::ToWorld(layout, layout.floors, glm::vec2(0.0f)).y;
        const float w = static_cast<float>(layout.width) * FacilityLayout::kCell;
        const float d = static_cast<float>(layout.depth) * FacilityLayout::kCell;
        // Each side in turn: the ray comes in from 5 m out, square to it.
        for (int side = 0; side < 4; ++side)
        {
            const bool alongZ = side < 2; // the x sides run along z
            const float length = alongZ ? d : w;
            const float out = side == 0 ? corner.x - 5.0f : side == 1 ? corner.x + w + 5.0f : side == 2 ? corner.z - 5.0f : corner.z + d + 5.0f;
            const float inward = side == 0 || side == 2 ? 1.0f : -1.0f;
            float face = -1.0f;
            int dents = 0;
            for (float t = 0.3f; t < length - 0.3f; t += 0.35f)
            {
                // Not where a way out is: that is a hole on purpose.
                bool atExit = false;
                for (const FacilityLayout::Exit& exit : layout.exits)
                {
                    const float middle = (static_cast<float>(alongZ ? exit.cell.y : exit.cell.x) + 0.5f) * FacilityLayout::kCell;
                    atExit = atExit || (exit.side == side && std::abs(t - middle) < 1.0f);
                }
                if (atExit)
                {
                    continue;
                }
                for (float y = bottom; y < top; y += 0.01f)
                {
                    const float across = (alongZ ? corner.z : corner.x) + t;
                    float nearest = 100.0f;
                    for (const Solid& solid : building)
                    {
                        const float cLo = alongZ ? solid.lo.z : solid.lo.x;
                        const float cHi = alongZ ? solid.hi.z : solid.hi.x;
                        if (y <= solid.lo.y || y >= solid.hi.y || across <= cLo || across >= cHi)
                        {
                            continue;
                        }
                        const float sLo = alongZ ? solid.lo.x : solid.lo.z;
                        const float sHi = alongZ ? solid.hi.x : solid.hi.z;
                        nearest = std::min(nearest, inward > 0.0f ? sLo - out : out - sHi);
                    }
                    if (face < 0.0f)
                    {
                        face = nearest;
                    }
                    if (std::abs(nearest - face) > 0.002f)
                    {
                        if (dents < 3)
                        {
                            UNSCOPED_INFO("side " << side << ", " << t << " along and " << y - corner.y << " up: the face is " << nearest
                                                  << " in, not " << face);
                        }
                        ++dents;
                    }
                }
            }
            CHECK(face > 4.0f);
            CHECK(face < 5.0f);
            CHECK(dents == 0);
        }
    }
}

TEST_CASE("A facility with basements has them under the ground, its ways out at the ground, and all of it can be walked", "[facility]")
{
    int unwalkable = 0;
    for (uint32_t seed = 1; seed <= 24; ++seed)
    {
        FacilityLayout::Options options;
        options.width = 16;
        options.depth = 14;
        options.minFloors = 2;
        options.maxFloors = 4;
        options.exits = 3;
        options.basements = 1 + static_cast<int>(seed % 2u);
        options.origin = {0.0f, 10.0f, 0.0f};
        const FacilityLayout layout = FacilityLayout::Generate(seed, options);
        CHECK(layout.ground == options.basements);
        CHECK(layout.GroundLevel() == Catch::Approx(10.0f));
        CHECK(layout.origin.y == Catch::Approx(10.0f - static_cast<float>(options.basements) * FacilityLayout::kStorey));
        for (const FacilityLayout::Exit& exit : layout.exits)
        {
            CHECK(layout.Open(layout.ground, exit.cell.x, exit.cell.y));
        }
        // Rooms under the ground, and every room and stairwell reached from the way in.
        CHECK(std::any_of(layout.rooms.begin(), layout.rooms.end(), [&](const FacilityLayout::Room& room) { return room.floor < layout.ground; }));
        const std::vector<bool> reach = layout.Reachable(true);
        for (const FacilityLayout::Room& room : layout.rooms)
        {
            if (!reach[layout.Index(room.floor, room.min.x, room.min.y)])
            {
                UNSCOPED_INFO("seed " << seed << " room on floor " << room.floor << " of " << layout.floors);
                ++unwalkable;
            }
        }
    }
    CHECK(unwalkable == 0);
}

TEST_CASE("Every building a site plans, basements and all, can be walked from its ways in", "[facility][site]")
{
    int unwalkable = 0;
    for (uint32_t seed = 1; seed <= 12; ++seed)
    {
        // (By the raw seed too, as a test site is built.)
        const SitePlan plan = SitePlan::Generate(seed % 2u == 0u ? SitePlan::SeedFor(static_cast<uint16_t>(seed), SitePlan::SiteKind::Facility) : seed);
        for (const FacilityLayout& building : plan.buildings)
        {
            const std::vector<bool> reach = building.Reachable(true);
            for (const FacilityLayout::Room& room : building.rooms)
            {
                if (!reach[building.Index(room.floor, room.min.x, room.min.y)])
                {
                    UNSCOPED_INFO("seed " << seed << ": a room on floor " << room.floor << " of " << building.floors << " (ground " << building.ground << ")");
                    ++unwalkable;
                }
            }
        }
    }
    CHECK(unwalkable == 0);
}

TEST_CASE("Buildings of every shape can be walked, are walled all round on every floor, and are dressed only outside", "[facility]")
{
    using Massing = FacilityLayout::Massing;
    for (const Massing massing : {Massing::L, Massing::T, Massing::U, Massing::H, Massing::Cross, Massing::Court, Massing::Stepped, Massing::Tower})
    {
        int shaped = 0;
        for (uint32_t seed = 1; seed <= 4; ++seed)
        {
            INFO("massing " << static_cast<int>(massing) << " seed " << seed);
            FacilityLayout::Options options;
            options.width = 26;
            options.depth = 22;
            options.minFloors = 2;
            options.maxFloors = 4;
            options.exits = 2;
            options.massing = massing;
            FacilityLayout layout = FacilityLayout::Generate(seed, options);
            layout.outside.windows = FacilityLayout::Outside::Windows::Punched;
            layout.outside.bands = layout.outside.stripe = layout.outside.canopies = true;
            layout.outside.ribEvery = 2;
            layout.outside.units = layout.outside.vents = 3;
            layout.outside.antenna = layout.outside.dome = true;
            layout.outside.dishes = 1;
            const auto [lowest, highest] = std::minmax_element(layout.heights.begin(), layout.heights.end());
            shaped += *lowest != *highest ? 1 : 0;

            // Every room reachable, and every way out on the edge of its shape, leading in.
            const std::vector<bool> reach = layout.Reachable(true);
            for (const FacilityLayout::Room& room : layout.rooms)
            {
                CHECK(reach[layout.Index(room.floor, room.min.x, room.min.y)]);
            }
            CHECK_FALSE(layout.exits.empty());
            for (const FacilityLayout::Exit& exit : layout.exits)
            {
                const glm::ivec2 out = exit.side == 0 ? glm::ivec2(-1, 0) : exit.side == 1 ? glm::ivec2(1, 0) : exit.side == 2 ? glm::ivec2(0, -1) : glm::ivec2(0, 1);
                CHECK(layout.Height(exit.cell.x + out.x, exit.cell.y + out.y) == 0);
                CHECK(reach[layout.Index(layout.ground, exit.cell.x, exit.cell.y)]);
            }

            // Every edge between a part of it and what is past it, on every floor, has the outside wall along it.
            const std::vector<FacilityLayout::OuterWall> walls = layout.OuterWalls();
            int gaps = 0;
            for (int f = 0; f < layout.floors; ++f)
            {
                for (int z = 0; z < layout.depth; ++z)
                {
                    for (int x = 0; x < layout.width; ++x)
                    {
                        if (layout.Height(x, z) <= f)
                        {
                            continue;
                        }
                        for (int side = 0; side < 4; ++side)
                        {
                            const glm::ivec2 out = side == 0 ? glm::ivec2(-1, 0) : side == 1 ? glm::ivec2(1, 0) : side == 2 ? glm::ivec2(0, -1) : glm::ivec2(0, 1);
                            if (layout.Height(x + out.x, z + out.y) > f)
                            {
                                continue;
                            }
                            const int line = side == 0 ? x : side == 1 ? x + 1 : side == 2 ? z : z + 1;
                            const int along = side >= 2 ? x : z;
                            const bool walled = std::any_of(walls.begin(), walls.end(), [&](const FacilityLayout::OuterWall& wall)
                            { return wall.side == side && wall.line == line && along >= wall.from && along < wall.to && f >= wall.low && f < wall.high; });
                            gaps += walled ? 0 : 1;
                        }
                    }
                }
            }
            CHECK(gaps == 0);

            // Nothing solid inside anything else, whatever its shape.
            const FacilityMap::Blueprint blueprint = FacilityMap::Draw(layout);
            const std::vector<Solid> solids = SolidsOf(blueprint);
            int overlaps = 0;
            for (size_t i = 0; i < solids.size(); ++i)
            {
                for (size_t k = i + 1; k < solids.size(); ++k)
                {
                    const bool bothStructure = solids[i].what.rfind("structure", 0) == 0 && solids[k].what.rfind("structure", 0) == 0;
                    if (!bothStructure && Overlap(solids[i], solids[k], 0.002f))
                    {
                        UNSCOPED_INFO(solids[i].what << " into " << solids[k].what << " at " << solids[i].lo.x << " " << solids[i].lo.y << " " << solids[i].lo.z);
                        ++overlaps;
                    }
                }
            }
            CHECK(overlaps == 0);

            // And what dresses it reaches nowhere into it: no box of it into the space of any storey of any cell inside its walls.
            CHECK(blueprint.dressing.size() > 20);
            int inside = 0;
            for (const FacilityMap::Dressing& dressing : blueprint.dressing)
            {
                if (dressing.form != FacilityMap::Dressing::Form::Box || glm::abs(glm::dot(dressing.rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))) < 0.9999f)
                {
                    continue;
                }
                const Solid box{dressing.centre - dressing.size * 0.5f, dressing.centre + dressing.size * 0.5f, "dressing"};
                for (int z = 0; z < layout.depth; ++z)
                {
                    for (int x = 0; x < layout.width; ++x)
                    {
                        const int h = layout.Height(x, z);
                        if (h == 0)
                        {
                            continue;
                        }
                        const glm::vec3 lo = FacilityMap::ToWorld(layout, 0, glm::vec2(static_cast<float>(x), static_cast<float>(z))) + glm::vec3(0.1f, 0.0f, 0.1f);
                        const glm::vec3 hi = FacilityMap::ToWorld(layout, h, glm::vec2(static_cast<float>(x + 1), static_cast<float>(z + 1))) - glm::vec3(0.1f, 0.0f, 0.1f);
                        if (Overlap(box, Solid{lo, hi, "cell"}, 0.002f))
                        {
                            if (inside < 3)
                            {
                                UNSCOPED_INFO("dressing of kind " << static_cast<int>(dressing.kind) << " at " << dressing.centre.x << " " << dressing.centre.y << " "
                                                                   << dressing.centre.z << " into cell " << x << " " << z);
                            }
                            ++inside;
                        }
                    }
                }
            }
            CHECK(inside == 0);
        }
        // (A shape that cannot be made at this size falls back to a block now and then, but not every time.)
        CHECK(shaped > 0);
    }
}

TEST_CASE("A facility's signs: a plate at every room's door, its floors in its stairwells, EXIT over the ways out", "[facility][signs]")
{
    for (const uint16_t seed : {uint16_t{1}, uint16_t{7}})
    {
        INFO("seed " << seed);
        const FacilityLayout layout = FacilityLayout::Generate(seed);
        const FacilityMap::Blueprint blueprint = FacilityMap::Draw(layout);
        std::vector<const FacilityMap::Dressing*> words;
        for (const FacilityMap::Dressing& dressing : blueprint.furnishing)
        {
            if (dressing.form == FacilityMap::Dressing::Form::Text)
            {
                words.push_back(&dressing);
                CHECK_FALSE(dressing.text.empty());
                CHECK(dressing.size.y > 0.0f);
            }
        }
        const auto count = [&](const std::string& text)
        { return std::count_if(words.begin(), words.end(), [&](const FacilityMap::Dressing* each) { return each->text == text; }); };
        CHECK(count("EXIT") == static_cast<long>(layout.exits.size() + layout.tunnels.size()));
        size_t flights = 0;
        for (const FacilityLayout::Stairwell& well : layout.stairwells)
        {
            flights += well.way == FacilityLayout::Stairwell::Way::Flight ? 2 : 0;
        }
        CHECK(count("STAIRS") == static_cast<long>(flights));
        CHECK(count("G") >= 1);
        const size_t roomDoors = static_cast<size_t>(std::count_if(layout.doors.begin(), layout.doors.end(), [](const FacilityLayout::Door& d) { return d.room >= 0; }));
        CHECK(words.size() > roomDoors);
        // Each word on a face, not in the middle of nowhere: something solid just behind it.
        // (Its front is its -z: behind is +z.)
        int behind = 0;
        for (const FacilityMap::Dressing* each : words)
        {
            const glm::vec3 back = each->centre + each->rotation * glm::vec3(0.0f, 0.0f, 0.06f);
            const bool against = std::any_of(blueprint.pieces.begin(), blueprint.pieces.end(), [&](const FacilityMap::Piece& piece)
                                             { return glm::all(glm::lessThanEqual(glm::abs(back - piece.centre), piece.size * 0.5f + glm::vec3(0.005f))); }) ||
                                 std::any_of(blueprint.furnishing.begin(), blueprint.furnishing.end(), [&](const FacilityMap::Dressing& d)
                                             { return d.form == FacilityMap::Dressing::Form::Box && glm::all(glm::lessThanEqual(glm::abs(glm::inverse(d.rotation) * (back - d.centre)), d.size * 0.5f + glm::vec3(0.02f))); });
            behind += against ? 1 : 0;
            if (!against)
            {
                UNSCOPED_INFO("floating: " << each->text << " at " << each->centre.x << " " << each->centre.y << " " << each->centre.z);
            }
        }
        CHECK(behind == static_cast<int>(words.size()));
    }
}

TEST_CASE("A facility's corridor services are one joined line: every pipe's end meets another, a bend, or what it joins", "[facility][services]")
{
    for (const uint16_t seed : {uint16_t{1}, uint16_t{7}, uint16_t{12}})
    {
        INFO("seed " << seed);
        const FacilityLayout layout = FacilityLayout::Generate(seed);
        const FacilityMap::Blueprint blueprint = FacilityMap::Draw(layout);
        // The services' pipes and cable bundles: lying cylinders as thick as one of them, at its height over some floor.
        struct Segment
        {
            glm::vec3 a;
            glm::vec3 b;
            float thick;
        };
        std::vector<Segment> pipes;
        std::vector<Segment> caps;
        std::vector<glm::vec3> ends;
        const auto overFloor = [&](float y)
        {
            for (int f = 0; f < layout.floors; ++f)
            {
                const float above = y - FacilityMap::ToWorld(layout, f, glm::vec2(0.0f)).y;
                for (const float height : {2.932f, 2.8f, 2.7f})
                {
                    if (std::abs(above - height) < 0.002f)
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        int bends = 0;
        for (const FacilityMap::Dressing& d : blueprint.furnishing)
        {
            if (!overFloor(d.centre.y))
            {
                continue;
            }
            if (d.form == FacilityMap::Dressing::Form::Cylinder && (std::abs(d.size.x - 0.1f) < 1e-4f || std::abs(d.size.x - 0.06f) < 1e-4f || std::abs(d.size.x - 0.07f) < 1e-4f))
            {
                const glm::vec3 axis = d.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
                if (std::abs(axis.y) > 0.1f)
                {
                    continue;
                }
                pipes.push_back({d.centre - axis * (d.size.y * 0.5f), d.centre + axis * (d.size.y * 0.5f), d.size.x});
            }
            // And the caps on the ends (each a little wider than its pipe, short).
            if (d.form == FacilityMap::Dressing::Form::Cylinder && d.size.y < 0.05f)
            {
                const glm::vec3 axis = d.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
                caps.push_back({d.centre - axis * (d.size.y * 0.5f), d.centre + axis * (d.size.y * 0.5f), d.size.x});
            }
            if (d.form == FacilityMap::Dressing::Form::Bend)
            {
                ++bends;
                ends.push_back(d.centre + d.rotation * glm::vec3(-d.size.x, 0.0f, 0.0f));
                ends.push_back(d.centre + d.rotation * glm::vec3(0.0f, 0.0f, d.size.x));
            }
        }
        REQUIRE(pipes.size() > 20);
        CHECK(bends > 0);
        for (const Segment& pipe : pipes)
        {
            ends.push_back(pipe.a);
            ends.push_back(pipe.b);
        }
        // Each end: on another (a pipe's or a bend's), or on the line of a pipe it runs into -- a junction's through run, a cap.
        int loose = 0;
        for (size_t i = 0; i < ends.size(); ++i)
        {
            const glm::vec3 end = ends[i];
            bool met = false;
            for (size_t k = 0; k < ends.size() && !met; ++k)
            {
                met = k != i && glm::distance(end, ends[k]) < 0.002f;
            }
            for (const Segment& pipe : pipes)
            {
                if (met)
                {
                    break;
                }
                const glm::vec3 along = pipe.b - pipe.a;
                const float t = glm::clamp(glm::dot(end - pipe.a, along) / glm::dot(along, along), 0.0f, 1.0f);
                const float off = glm::distance(end, pipe.a + along * t);
                met = off < 0.002f ? (t > 0.001f && t < 0.999f) : off <= pipe.thick * 0.5f + 0.002f;
            }
            for (const Segment& cap : caps)
            {
                const glm::vec3 along = cap.b - cap.a;
                const float t = glm::clamp(glm::dot(end - cap.a, along) / glm::dot(along, along), 0.0f, 1.0f);
                met = met || glm::distance(end, cap.a + along * t) < 0.002f;
            }
            if (!met)
            {
                ++loose;
                UNSCOPED_INFO("loose end at " << end.x << " " << end.y << " " << end.z);
            }
        }
        CHECK(loose == 0);
    }
}
