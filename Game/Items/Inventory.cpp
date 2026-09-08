#include "Game/Items/Inventory.h"

#include <algorithm>

namespace pred
{

Inventory::Inventory(int slotCount)
{
    Resize(slotCount);
}

void Inventory::Resize(int slotCount)
{
    m_slots.assign(static_cast<size_t>(std::max(1, slotCount)), Slot{});
    m_selected = 0;
}

bool Inventory::CanAdd(const ItemDatabase& database, ItemId item, int count) const
{
    const ItemDefinition* definition = database.Get(item);
    if (definition == nullptr || count <= 0)
    {
        return false;
    }

    int remaining = count;
    for (const Slot& slot : m_slots)
    {
        if (slot.IsEmpty())
        {
            remaining -= definition->maxStack;
        }
        else if (slot.item == item)
        {
            remaining -= definition->maxStack - slot.count;
        }
        if (remaining <= 0)
        {
            return true;
        }
    }
    return false;
}

int Inventory::Add(const ItemDatabase& database, ItemId item, int count)
{
    const ItemDefinition* definition = database.Get(item);
    if (definition == nullptr || count <= 0)
    {
        return 0;
    }

    int remaining = count;

    // Top up existing stacks first, so picking things up does not fragment the bag.
    for (Slot& slot : m_slots)
    {
        if (remaining <= 0)
        {
            break;
        }
        if (!slot.IsEmpty() && slot.item == item && slot.count < definition->maxStack)
        {
            const int space = definition->maxStack - slot.count;
            const int moved = std::min(space, remaining);
            slot.count += moved;
            remaining -= moved;
        }
    }

    for (Slot& slot : m_slots)
    {
        if (remaining <= 0)
        {
            break;
        }
        if (slot.IsEmpty())
        {
            const int moved = std::min(definition->maxStack, remaining);
            slot.item = item;
            slot.count = moved;
            remaining -= moved;
        }
    }

    return count - remaining;
}

int Inventory::RemoveFromSlot(int index, int count)
{
    if (index < 0 || index >= SlotCount() || count <= 0)
    {
        return 0;
    }
    Slot& slot = m_slots[static_cast<size_t>(index)];
    if (slot.IsEmpty())
    {
        return 0;
    }

    const int removed = std::min(slot.count, count);
    slot.count -= removed;
    if (slot.count <= 0)
    {
        slot = Slot{};
    }
    return removed;
}

int Inventory::CountOf(ItemId item) const
{
    int total = 0;
    for (const Slot& slot : m_slots)
    {
        if (!slot.IsEmpty() && slot.item == item)
        {
            total += slot.count;
        }
    }
    return total;
}

bool Inventory::IsFull(const ItemDatabase& database) const
{
    for (const Slot& slot : m_slots)
    {
        if (slot.IsEmpty())
        {
            return false;
        }
        const ItemDefinition* definition = database.Get(slot.item);
        if (definition != nullptr && slot.count < definition->maxStack)
        {
            return false;
        }
    }
    return true;
}

void Inventory::Clear()
{
    for (Slot& slot : m_slots)
    {
        slot = Slot{};
    }
}

void Inventory::SelectSlot(int index)
{
    if (index >= 0 && index < SlotCount())
    {
        m_selected = index;
    }
}

void Inventory::SelectNext(int direction)
{
    if (direction == 0 || m_slots.empty())
    {
        return;
    }
    const int count = SlotCount();
    m_selected = ((m_selected + direction) % count + count) % count;
}

} // namespace pred
