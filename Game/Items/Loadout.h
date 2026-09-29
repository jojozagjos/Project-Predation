#pragma once

#include "Game/Items/Inventory.h"
#include "Game/Items/ItemDatabase.h"

#include <string>
#include <vector>

namespace pred
{

// What one person draws at the ship's loadout locker: how many of each thing. Everything issued comes from there -- the
// weapons loaded, with their spare magazines -- and a new kit is chosen before each deployment. What is only ever found
// (the mission's drive, a keycard) is not issued, and is kept when a kit is drawn.
struct LoadoutPick
{
    ItemId item = kInvalidItem;
    int count = 0;
};
using Loadout = std::vector<LoadoutPick>;

namespace Loadouts
{

// Everything the locker issues, in items.json's order.
std::vector<ItemId> Issued(const ItemDatabase& items);

// How many slots a kit takes: each thing its own, as many as it needs for its stacks.
int SlotsFor(const ItemDatabase& items, const Loadout& kit);

// Made into something that could be drawn: things that are not issued taken out, each held to the most one person may
// have, the same thing twice made into one, and whatever does not fit in `slots` left behind, from the end.
Loadout Clamp(const ItemDatabase& items, const Loadout& kit, int slots);

// The slots taken up by what did not come from the locker, which stays when a kit is drawn.
int KeptSlots(const ItemDatabase& items, const Inventory& inventory);

// Hands back everything that came from the locker and takes `kit` instead. Whatever was found stays where it is. The kit
// is clamped to the room there is, so a kit that does not fit loses its end rather than half of something.
Loadout Draw(const ItemDatabase& items, Inventory& inventory, const Loadout& kit);

// As text, for remembering between games: "medkit:2,battery:2". Unknown things are skipped when it is read.
std::string ToText(const ItemDatabase& items, const Loadout& kit);
Loadout FromText(const ItemDatabase& items, const std::string& text);

} // namespace Loadouts

} // namespace pred
