#pragma once

#include "Game/Items/ItemDatabase.h"

#include <cstdint>
#include <vector>

namespace pred
{

// Slot-based, with no weight simulation, per the design: carrying more never slows the player down.
// Slots are fixed in number so the limit is legible at a glance rather than being a hidden total.
class Inventory
{
public:
    struct Slot
    {
        ItemId item = kInvalidItem;
        int count = 0;

        bool IsEmpty() const { return item == kInvalidItem || count <= 0; }
    };

    explicit Inventory(int slotCount = 6);

    void Resize(int slotCount);
    int SlotCount() const { return static_cast<int>(m_slots.size()); }
    const Slot& At(int index) const { return m_slots[static_cast<size_t>(index)]; }

    // Returns how many were actually stored, filling existing stacks before opening a new slot.
    int Add(const ItemDatabase& database, ItemId item, int count = 1);
    bool CanAdd(const ItemDatabase& database, ItemId item, int count = 1) const;

    // Removes up to `count` from a slot; returns how many came out.
    int RemoveFromSlot(int index, int count = 1);
    int CountOf(ItemId item) const;
    bool IsFull(const ItemDatabase& database) const;

    void Clear();

    int SelectedSlot() const { return m_selected; }
    void SelectSlot(int index);
    void SelectNext(int direction);
    const Slot& Selected() const { return m_slots[static_cast<size_t>(m_selected)]; }

private:
    std::vector<Slot> m_slots;
    int m_selected = 0;
};

} // namespace pred
