#include "Game/Items/Inventory.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/Items/Loadout.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace pred;

namespace
{

ItemDatabase LoadItems()
{
    ItemDatabase items;
    REQUIRE(items.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "items.json"));
    return items;
}

int CountIn(const Loadout& kit, ItemId item)
{
    for (const LoadoutPick& pick : kit)
    {
        if (pick.item == item)
        {
            return pick.count;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("The locker issues the kit and not what is only ever found", "[loadout]")
{
    const ItemDatabase items = LoadItems();
    for (const char* issued : {"sidearm", "carbine", "medkit", "battery", "flare", "site_map", "objective_tracker"})
    {
        INFO(issued);
        REQUIRE(items.Find(issued) != nullptr);
        CHECK(items.Find(issued)->loadoutMax > 0);
        CHECK_FALSE(items.Find(issued)->blurb.empty());
    }
    for (const char* found : {"data_drive", "keycard", "sample_container"})
    {
        INFO(found);
        REQUIRE(items.Find(found) != nullptr);
        CHECK(items.Find(found)->loadoutMax == 0);
    }
}

TEST_CASE("The standard kit fills the bag exactly, and everything cannot be taken at once", "[loadout]")
{
    const ItemDatabase items = LoadItems();
    const Inventory bag;
    const Loadout standard = Loadouts::Default(items);
    CHECK(Loadouts::SlotsFor(items, standard) == bag.SlotCount());
    CHECK(Loadouts::Clamp(items, standard, bag.SlotCount()).size() == standard.size());

    // Everything, as much as may be had: more than there is room for, so something has to be chosen.
    Loadout everything;
    for (const ItemId id : Loadouts::Issued(items))
    {
        everything.push_back({id, items.Get(id)->loadoutMax});
    }
    CHECK(Loadouts::SlotsFor(items, everything) > bag.SlotCount());
    const Loadout fitted = Loadouts::Clamp(items, everything, bag.SlotCount());
    CHECK(Loadouts::SlotsFor(items, fitted) <= bag.SlotCount());
}

TEST_CASE("A kit is held to what may be had and what fits", "[loadout]")
{
    const ItemDatabase items = LoadItems();
    const ItemId medkit = items.IdOf("medkit");
    const ItemId keycard = items.IdOf("keycard");
    const ItemId sidearm = items.IdOf("sidearm");

    // Too many, twice over, and something that is not issued at all.
    const Loadout greedy = Loadouts::Clamp(items, {{medkit, 5}, {medkit, 3}, {keycard, 1}, {sidearm, 2}}, 6);
    CHECK(CountIn(greedy, medkit) == items.Get(medkit)->loadoutMax);
    CHECK(CountIn(greedy, keycard) == 0);
    CHECK(CountIn(greedy, sidearm) == 1);
    CHECK(greedy.size() == 2);

    // No room: the end goes first.
    const Loadout tight = Loadouts::Clamp(items, {{sidearm, 1}, {medkit, 2}}, 1);
    CHECK(CountIn(tight, sidearm) == 1);
    CHECK(CountIn(tight, medkit) == 0);
}

TEST_CASE("Drawing a kit hands back the old one and keeps what was found", "[loadout]")
{
    const ItemDatabase items = LoadItems();
    const ItemId medkit = items.IdOf("medkit");
    const ItemId keycard = items.IdOf("keycard");
    const ItemId drive = items.IdOf("data_drive");
    const ItemId carbine = items.IdOf("carbine");
    const ItemId battery = items.IdOf("battery");

    Inventory bag;
    bag.Add(items, medkit, 1);
    bag.Add(items, keycard, 1);
    bag.Add(items, carbine, 1, 3, 0); // nearly empty
    bag.Add(items, drive, 1);
    CHECK(Loadouts::KeptSlots(items, bag) == 2);

    // Everything: only four slots are free, so the end of it is left behind.
    const Loadout drawn = Loadouts::Draw(items, bag, Loadouts::Default(items));
    CHECK(bag.CountOf(keycard) == 1);
    CHECK(bag.CountOf(drive) == 1);
    CHECK(bag.CountOf(carbine) == 0);
    CHECK(bag.CountOf(medkit) == CountIn(drawn, medkit));
    CHECK(Loadouts::SlotsFor(items, drawn) == 4);

    // A carbine from the locker is a fresh one, whatever state the last was handed back in.
    Loadouts::Draw(items, bag, {{carbine, 1}, {battery, 4}});
    CHECK(bag.CountOf(carbine) == 1);
    CHECK(bag.CountOf(battery) == 4);
    CHECK(bag.CountOf(medkit) == 0);
    for (int i = 0; i < bag.SlotCount(); ++i)
    {
        if (bag.At(i).item == carbine)
        {
            CHECK(bag.At(i).rounds < 0);
        }
    }
}

TEST_CASE("A kit is remembered as text and read back", "[loadout]")
{
    const ItemDatabase items = LoadItems();
    const Loadout kit{{items.IdOf("carbine"), 1}, {items.IdOf("flare"), 3}};
    const std::string text = Loadouts::ToText(items, kit);
    CHECK(text == "carbine:1,flare:3");
    const Loadout back = Loadouts::FromText(items, text + ",nonsense:2,:,medkit");
    REQUIRE(back.size() == 2);
    CHECK(back[0].item == kit[0].item);
    CHECK(back[1].count == 3);
    CHECK(Loadouts::FromText(items, "").empty());
}
