#pragma once

#include "Game/Weapons/WeaponTypes.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred
{

// Every weapon the game knows about, loaded from weapons.json.
//
// Shaped like ItemDatabase on purpose: the key is the stable identity that data files and network
// messages carry, and the id is a compact index only valid within a run.
class WeaponDatabase
{
public:
    bool LoadFromFile(const std::filesystem::path& file);
    void AddBuiltinDefaults();

    const WeaponDefinition* Get(WeaponId id) const;
    const WeaponDefinition* Find(const std::string& key) const;
    WeaponId IdOf(const std::string& key) const;
    // The weapon an inventory item corresponds to, or invalid if the item is not a weapon.
    WeaponId ForItem(const std::string& itemKey) const;

    const std::vector<WeaponDefinition>& All() const { return m_weapons; }
    // Mutable, so the weapon bench can point a weapon at a model without a restart and an edit to
    // weapons.json. What is changed here is not written back: the bench has a button for that.
    WeaponDefinition* Mutable(WeaponId id);
    size_t Count() const { return m_weapons.size(); }

private:
    WeaponId Add(WeaponDefinition definition);

    std::vector<WeaponDefinition> m_weapons; // index 0 is a placeholder for kInvalidWeapon
    std::unordered_map<std::string, WeaponId> m_byKey;
    std::unordered_map<std::string, WeaponId> m_byItem;
};

} // namespace pred
