#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"
#include "Engine/Navigation/NavMesh.h"
#include "Engine/Render/Primitives.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
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
    const FacilityLayout plan = FacilityLayout::Generate(7);
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

std::vector<Solid> SolidsOf(const FacilityMap::Blueprint& blueprint)
{
    std::vector<Solid> solids;
    for (const FacilityMap::Piece& piece : blueprint.pieces)
    {
        solids.push_back(Around(piece.centre, piece.size, piece.yaw, "piece " + std::to_string(static_cast<int>(piece.kind))));
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
                if (Overlap(solids[i], solids[k], 0.002f))
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
