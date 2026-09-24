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

int Inventory::Add(const ItemDatabase& database, ItemId item, int count, int rounds, int reserve)
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
            // The state travels with the item onto the slot it opens for itself. Anything that
            // stacked onto an existing pile above keeps that pile's state, because a stack of
            // identical things cannot say which of them a magazine belongs to.
            slot.rounds = rounds;
            slot.reserve = reserve;
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

void Inventory::SetSlotAmmo(int index, int rounds, int reserve)
{
    if (index >= 0 && index < SlotCount())
    {
        Slot& slot = m_slots[static_cast<size_t>(index)];
        slot.rounds = rounds;
        slot.reserve = reserve;
    }
}

bool Inventory::Move(const ItemDatabase& database, int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= SlotCount() || to >= SlotCount() || At(from).IsEmpty())
    {
        return false;
    }
    Slot& source = m_slots[static_cast<size_t>(from)];
    Slot& target = m_slots[static_cast<size_t>(to)];
    const ItemDefinition* definition = database.Get(source.item);
    if (!target.IsEmpty() && target.item == source.item && definition != nullptr && definition->maxStack > 1)
    {
        const int moved = std::min(source.count, definition->maxStack - target.count);
        if (moved <= 0)
        {
            return false;
        }
        target.count += moved;
        source.count -= moved;
        if (source.count <= 0)
        {
            source = Slot{};
            if (m_selected == from)
            {
                m_selected = to;
            }
        }
        return true;
    }
    std::swap(source, target);
    if (m_selected == from)
    {
        m_selected = to;
    }
    else if (m_selected == to)
    {
        m_selected = from;
    }
    return true;
}

void Inventory::SelectSlot(int index)
{
    if (index == kNoSlot || (index >= 0 && index < SlotCount()))
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
    // The wheel runs through the slots and through empty hands, so there is a way back to carrying
    // nothing without having to remember which slot you had out.
    const int count = SlotCount() + 1;
    const int from = m_selected == kNoSlot ? SlotCount() : m_selected;
    const int next = ((from + direction) % count + count) % count;
    m_selected = next == SlotCount() ? kNoSlot : next;
}

const Inventory::Slot& Inventory::Selected() const
{
    static const Slot empty;
    return m_selected >= 0 && m_selected < SlotCount() ? m_slots[static_cast<size_t>(m_selected)]
                                                      : empty;
}

} // namespace pred
