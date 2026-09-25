#include "Game/World/FacilityLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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
