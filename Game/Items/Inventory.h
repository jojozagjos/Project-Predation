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
        // What state this particular one is in, for things that have any: a weapon's magazine and
        // the spare rounds carried with it. Negative means "never drawn", so the weapon's own
        // starting load is used. Held here rather than on the equipped weapon because the equipped
        // weapon is a view of whichever slot is selected, and putting one away and taking it out
        // again must not refill it.
        int rounds = -1;
        int reserve = -1;

        bool IsEmpty() const { return item == kInvalidItem || count <= 0; }
    };

    explicit Inventory(int slotCount = 6);

    void Resize(int slotCount);
    int SlotCount() const { return static_cast<int>(m_slots.size()); }
    const Slot& At(int index) const { return m_slots[static_cast<size_t>(index)]; }

    // Returns how many were actually stored, filling existing stacks before opening a new slot.
    // Ammunition travels with the item: a rifle picked up off the floor keeps whatever was left in
    // it. It only lands on a slot the item opens for itself, because a stack of identical things
    // has no room to say which of them the magazine belongs to.
    int Add(const ItemDatabase& database, ItemId item, int count = 1, int rounds = -1,
            int reserve = -1);
    // Writes the state of what is in a slot back into it: the magazine, when a weapon is put away.
    void SetSlotAmmo(int index, int rounds, int reserve);
    bool CanAdd(const ItemDatabase& database, ItemId item, int count = 1) const;

    // Removes up to `count` from a slot; returns how many came out.
    int RemoveFromSlot(int index, int count = 1);
    int CountOf(ItemId item) const;
    bool IsFull(const ItemDatabase& database) const;

    void Clear();

    // Nothing in the hands. A real state rather than a special slot, so pressing the key for what
    // you are already holding can put it away.
    static constexpr int kNoSlot = -1;

    int SelectedSlot() const { return m_selected; }
    void SelectSlot(int index);
    void SelectNext(int direction);
    // Empty when nothing is selected, so callers never have to check first.
    const Slot& Selected() const;

private:
    std::vector<Slot> m_slots;
    int m_selected = 0;
};

} // namespace pred
