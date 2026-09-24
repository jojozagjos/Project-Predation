#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred
{

using ItemId = int;
inline constexpr ItemId kInvalidItem = 0;

enum class ItemShape : uint8_t
{
    Box,
    Cylinder,
    Sphere
};

// What using an item does. Each is a whole mechanic, not a flag for one: see Docs/ITEMS.md.
enum class ItemUseKind : uint8_t
{
    None,
    Heal,     // patch up yourself, or the person right in front of you, by `amount`
    Recharge, // a fresh cell in the torch
    Unlock,   // open the locked door in front of you
    Flare,    // strike it; then throw it, and it burns for `amount` seconds
    Inspect   // turn it over in your hands and look at it
};
const char* ItemUseKindName(ItemUseKind kind);
ItemUseKind ItemUseKindFromString(const std::string& name);

// A moment in a use, as far through it as `t` (0 to 1): where the item is then, relative to where it is
// carried -- metres right, up and forward of the way you are looking -- and how it is turned, in degrees.
struct ItemMotionKey
{
    float t = 0.0f;
    glm::vec3 offset{0.0f};
    glm::vec3 turn{0.0f};
};
// Where a motion has got to at `t`, eased from one key to the next. At rest, with no keys.
ItemMotionKey SampleMotion(const std::vector<ItemMotionKey>& keys, float t);

// Using an item: what it does, how long it takes, and how the hand moves while it does it.
struct ItemUse
{
    ItemUseKind kind = ItemUseKind::None;
    float seconds = 1.0f;
    float amount = 0.0f;   // health healed, share of a torch recharged, seconds a flare burns
    bool consumed = false; // gone once the use is done
    std::vector<ItemMotionKey> motion;
    std::string startSound; // as it starts, by name, "Items/medkit_open"
    std::string doneSound;  // as it finishes
    // The second half of a use that has two, as a flare does: struck, then thrown.
    float secondSeconds = 0.5f;
    std::vector<ItemMotionKey> secondMotion;
    std::string secondSound;
};

// What an item is, independent of any particular copy of it in the world or in a bag.
struct ItemDefinition
{
    ItemId id = kInvalidItem;
    std::string key;  // stable string name used in data files
    std::string name; // shown to the player
    int maxStack = 1;
    float mass = 1.0f;

    // Placeholder appearance until there are real meshes.
    ItemShape shape = ItemShape::Box;
    glm::vec3 size{0.18f, 0.12f, 0.26f};
    glm::vec3 color{0.6f, 0.6f, 0.62f};
    float roughness = 0.7f;
    float metallic = 0.0f;
    float emissive = 0.0f;
    // How this item sits in the hand: an offset from the grip point and a turn, both in the hand's
    // own frame. Placed by eye in the editor, because what a keycard looks like held is not
    // something any rule about its box could work out.
    glm::vec3 holdOffset{0.0f};
    glm::vec3 holdRotation{0.0f}; // euler degrees, X then Y then Z
    ItemUse use;
    // How many are laid out on each equipment bench.
    int benchCount = 1;
};

// Item definitions loaded from Assets/Data/items.json.
//
// Ids are assigned on load and are only stable within a run; the string key is the stable identity
// and is what save files and network messages should carry.
class ItemDatabase
{
public:
    bool LoadFromFile(const std::filesystem::path& file);
    // Writes the hold placements back into the file they came from, leaving everything else in it
    // alone. The editor places an item in the hand by eye and this is how that survives a restart;
    // rewriting the whole file from these structs would throw away every field this loader does not
    // happen to read.
    // The use motions too, which the editor also shapes by eye.
    bool SaveHoldPlacements(const std::filesystem::path& file) const;
    void AddBuiltinDefaults();

    const ItemDefinition* Get(ItemId id) const;
    const ItemDefinition* Find(const std::string& key) const;
    ItemId IdOf(const std::string& key) const;

    const std::vector<ItemDefinition>& All() const { return m_items; }
    // Keys in the file this loader does not know, as "item: key": typos, which are otherwise ignored.
    const std::vector<std::string>& Warnings() const { return m_warnings; }
    // Mutable, so the editor can place an item in the hand and see it move as it does.
    ItemDefinition* Mutable(ItemId id);
    size_t Count() const { return m_items.size(); }

private:
    ItemId Add(ItemDefinition definition);

    std::vector<ItemDefinition> m_items; // index 0 is a placeholder for kInvalidItem
    std::unordered_map<std::string, ItemId> m_byKey;
    std::vector<std::string> m_warnings;
};

} // namespace pred
