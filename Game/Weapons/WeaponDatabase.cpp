#include "Game/Weapons/WeaponDatabase.h"

#include "Engine/Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace pred
{
namespace
{

glm::vec3 ReadVec3(const nlohmann::json& node, const glm::vec3& fallback)
{
    if (!node.is_array() || node.size() != 3)
    {
        return fallback;
    }
    return glm::vec3(node[0].get<float>(), node[1].get<float>(), node[2].get<float>());
}

template <typename T>
void ReadField(const nlohmann::json& node, const char* name, T& target)
{
    if (const auto it = node.find(name); it != node.end() && !it->is_null())
    {
        target = it->get<T>();
    }
}

} // namespace

const char* FireModeName(FireMode mode)
{
    switch (mode)
    {
    case FireMode::Auto:
        return "auto";
    case FireMode::Burst:
        return "burst";
    case FireMode::Single:
    default:
        return "single";
    }
}

FireMode FireModeFromString(const std::string& name)
{
    if (name == "auto")
    {
        return FireMode::Auto;
    }
    if (name == "burst")
    {
        return FireMode::Burst;
    }
    return FireMode::Single;
}

WeaponId WeaponDatabase::Add(WeaponDefinition definition)
{
    if (m_weapons.empty())
    {
        // Slot 0 stands for "no weapon", so a default WeaponState is unarmed.
        m_weapons.push_back(WeaponDefinition{});
    }
    if (definition.key.empty())
    {
        PRED_LOG_ERROR(Gameplay, "Weapon definition has no key, ignored");
        return kInvalidWeapon;
    }
    if (const auto it = m_byKey.find(definition.key); it != m_byKey.end())
    {
        PRED_LOG_WARN(Gameplay, "Duplicate weapon key '{}', the later one replaces it", definition.key);
        definition.id = it->second;
        m_weapons[static_cast<size_t>(it->second)] = definition;
        return definition.id;
    }

    definition.id = static_cast<WeaponId>(m_weapons.size());
    m_byKey.emplace(definition.key, definition.id);
    if (!definition.item.empty())
    {
        m_byItem.emplace(definition.item, definition.id);
    }
    m_weapons.push_back(std::move(definition));
    return m_weapons.back().id;
}

bool WeaponDatabase::LoadFromFile(const std::filesystem::path& file)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_WARN(Gameplay, "No weapons file at {}, using built-in defaults", file.string());
        AddBuiltinDefaults();
        return false;
    }

    nlohmann::json json;
    try
    {
        stream >> json;
    }
    catch (const std::exception& error)
    {
        PRED_LOG_ERROR(Gameplay, "Weapons file {} is not valid JSON: {}", file.string(), error.what());
        AddBuiltinDefaults();
        return false;
    }

    const auto weapons = json.find("weapons");
    if (weapons == json.end() || !weapons->is_array())
    {
        PRED_LOG_ERROR(Gameplay, "Weapons file {} has no 'weapons' array", file.string());
        AddBuiltinDefaults();
        return false;
    }

    for (const auto& node : *weapons)
    {
        WeaponDefinition definition;
        ReadField(node, "key", definition.key);
        ReadField(node, "name", definition.name);
        ReadField(node, "item", definition.item);
        if (definition.name.empty())
        {
            definition.name = definition.key;
        }

        std::string mode = FireModeName(definition.mode);
        ReadField(node, "fire_mode", mode);
        definition.mode = FireModeFromString(mode);

        ReadField(node, "burst_count", definition.burstCount);
        ReadField(node, "rounds_per_minute", definition.roundsPerMinute);
        ReadField(node, "magazine", definition.magazineSize);
        ReadField(node, "reserve", definition.reserveOnPickup);
        ReadField(node, "reload_seconds", definition.reloadSeconds);
        ReadField(node, "damage", definition.damage);
        ReadField(node, "range", definition.range);
        ReadField(node, "spread_hip", definition.spreadHip);
        ReadField(node, "spread_aim", definition.spreadAim);
        ReadField(node, "spread_per_shot", definition.spreadPerShot);
        ReadField(node, "spread_max", definition.spreadMax);
        ReadField(node, "spread_recover", definition.spreadRecover);
        ReadField(node, "recoil_pitch", definition.recoilPitch);
        ReadField(node, "recoil_yaw", definition.recoilYaw);
        ReadField(node, "recoil_recover", definition.recoilRecover);
        ReadField(node, "aim_seconds", definition.aimSeconds);
        ReadField(node, "aim_speed_scale", definition.aimSpeedScale);
        ReadField(node, "muzzle_forward", definition.muzzleForward);

        if (const auto it = node.find("size"); it != node.end())
        {
            definition.size = ReadVec3(*it, definition.size);
        }
        if (const auto it = node.find("color"); it != node.end())
        {
            definition.color = ReadVec3(*it, definition.color);
        }

        Add(std::move(definition));
    }

    if (m_weapons.size() <= 1)
    {
        PRED_LOG_WARN(Gameplay, "Weapons file {} defined nothing usable", file.string());
        AddBuiltinDefaults();
        return false;
    }

    PRED_LOG_INFO(Gameplay, "Loaded {} weapons from {}", m_weapons.size() - 1, file.string());
    return true;
}

void WeaponDatabase::AddBuiltinDefaults()
{
    if (m_weapons.size() > 1)
    {
        return;
    }
    WeaponDefinition sidearm;
    sidearm.key = "sidearm";
    sidearm.name = "ACRD Sidearm";
    sidearm.item = "sidearm";
    Add(std::move(sidearm));
}

const WeaponDefinition* WeaponDatabase::Get(WeaponId id) const
{
    if (id <= kInvalidWeapon || static_cast<size_t>(id) >= m_weapons.size())
    {
        return nullptr;
    }
    return &m_weapons[static_cast<size_t>(id)];
}

const WeaponDefinition* WeaponDatabase::Find(const std::string& key) const
{
    return Get(IdOf(key));
}

WeaponId WeaponDatabase::IdOf(const std::string& key) const
{
    const auto it = m_byKey.find(key);
    return it == m_byKey.end() ? kInvalidWeapon : it->second;
}

WeaponId WeaponDatabase::ForItem(const std::string& itemKey) const
{
    const auto it = m_byItem.find(itemKey);
    return it == m_byItem.end() ? kInvalidWeapon : it->second;
}

} // namespace pred
