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
    bool SaveHoldPlacements(const std::filesystem::path& file) const;
    void AddBuiltinDefaults();

    const ItemDefinition* Get(ItemId id) const;
    const ItemDefinition* Find(const std::string& key) const;
    ItemId IdOf(const std::string& key) const;

    const std::vector<ItemDefinition>& All() const { return m_items; }
    // Mutable, so the editor can place an item in the hand and see it move as it does.
    ItemDefinition* Mutable(ItemId id);
    size_t Count() const { return m_items.size(); }

private:
    ItemId Add(ItemDefinition definition);

    std::vector<ItemDefinition> m_items; // index 0 is a placeholder for kInvalidItem
    std::unordered_map<std::string, ItemId> m_byKey;
};

} // namespace pred
