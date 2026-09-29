#include "Game/Items/Loadout.h"

#include <algorithm>
#include <cstdlib>

namespace pred
{

namespace Loadouts
{

namespace
{

bool IsIssued(const ItemDatabase& items, ItemId item)
{
    const ItemDefinition* definition = items.Get(item);
    return definition != nullptr && definition->loadoutMax > 0;
}

int StacksOf(const ItemDefinition& definition, int count)
{
    return count <= 0 ? 0 : (count + definition.maxStack - 1) / definition.maxStack;
}

} // namespace

std::vector<ItemId> Issued(const ItemDatabase& items)
{
    std::vector<ItemId> issued;
    for (const ItemDefinition& definition : items.All())
    {
        if (definition.id != kInvalidItem && definition.loadoutMax > 0)
        {
            issued.push_back(definition.id);
        }
    }
    return issued;
}

int SlotsFor(const ItemDatabase& items, const Loadout& kit)
{
    int slots = 0;
    for (const LoadoutPick& pick : kit)
    {
        if (const ItemDefinition* definition = items.Get(pick.item))
        {
            slots += StacksOf(*definition, pick.count);
        }
    }
    return slots;
}

Loadout Clamp(const ItemDatabase& items, const Loadout& kit, int slots)
{
    Loadout out;
    int used = 0;
    for (const LoadoutPick& pick : kit)
    {
        const ItemDefinition* definition = items.Get(pick.item);
        if (definition == nullptr || definition->loadoutMax <= 0 || pick.count <= 0)
        {
            continue;
        }
        // The same thing twice is one pick of both, held to the most there may be.
        auto same = std::find_if(out.begin(), out.end(), [&](const LoadoutPick& o) { return o.item == pick.item; });
        const int before = same != out.end() ? same->count : 0;
        int count = std::min(before + pick.count, definition->loadoutMax);
        // And only as much as there is room for: a stack at a time.
        while (count > before && used - StacksOf(*definition, before) + StacksOf(*definition, count) > slots)
        {
            --count;
        }
        if (count <= before)
        {
            continue;
        }
        used += StacksOf(*definition, count) - StacksOf(*definition, before);
        if (same != out.end())
        {
            same->count = count;
        }
        else
        {
            out.push_back({pick.item, count});
        }
    }
    return out;
}

int KeptSlots(const ItemDatabase& items, const Inventory& inventory)
{
    int kept = 0;
    for (int i = 0; i < inventory.SlotCount(); ++i)
    {
        const Inventory::Slot& slot = inventory.At(i);
        kept += !slot.IsEmpty() && !IsIssued(items, slot.item) ? 1 : 0;
    }
    return kept;
}

Loadout Draw(const ItemDatabase& items, Inventory& inventory, const Loadout& kit)
{
    for (int i = 0; i < inventory.SlotCount(); ++i)
    {
        const Inventory::Slot& slot = inventory.At(i);
        if (!slot.IsEmpty() && IsIssued(items, slot.item))
        {
            inventory.RemoveFromSlot(i, slot.count);
        }
    }
    const Loadout drawn = Clamp(items, kit, inventory.SlotCount() - KeptSlots(items, inventory));
    for (const LoadoutPick& pick : drawn)
    {
        // Fresh from the locker: a weapon's magazine full and its spares with it (-1: its own starting load).
        inventory.Add(items, pick.item, pick.count);
    }
    return drawn;
}

std::string ToText(const ItemDatabase& items, const Loadout& kit)
{
    std::string text;
    for (const LoadoutPick& pick : kit)
    {
        if (const ItemDefinition* definition = items.Get(pick.item); definition != nullptr && pick.count > 0)
        {
            text += (text.empty() ? "" : ",") + definition->key + ":" + std::to_string(pick.count);
        }
    }
    return text;
}

Loadout FromText(const ItemDatabase& items, const std::string& text)
{
    Loadout kit;
    size_t at = 0;
    while (at < text.size())
    {
        const size_t comma = text.find(',', at);
        const std::string part = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        const size_t colon = part.find(':');
        if (colon != std::string::npos)
        {
            const ItemId item = items.IdOf(part.substr(0, colon));
            const int count = std::atoi(part.c_str() + colon + 1);
            if (item != kInvalidItem && count > 0)
            {
                kit.push_back({item, count});
            }
        }
        if (comma == std::string::npos)
        {
            break;
        }
        at = comma + 1;
    }
    return kit;
}

} // namespace Loadouts

} // namespace pred
