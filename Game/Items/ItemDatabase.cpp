#include "Game/Items/ItemDatabase.h"

#include "Engine/Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace pred
{
namespace
{

ItemShape ParseShape(const std::string& text)
{
    if (text == "cylinder")
    {
        return ItemShape::Cylinder;
    }
    if (text == "sphere")
    {
        return ItemShape::Sphere;
    }
    return ItemShape::Box;
}

glm::vec3 ReadVec3(const nlohmann::json& node, const glm::vec3& fallback)
{
    if (!node.is_array() || node.size() != 3)
    {
        return fallback;
    }
    return glm::vec3(node[0].get<float>(), node[1].get<float>(), node[2].get<float>());
}

} // namespace

ItemId ItemDatabase::Add(ItemDefinition definition)
{
    if (m_items.empty())
    {
        // Slot 0 stands for "nothing", so a default-constructed inventory slot is empty.
        m_items.push_back(ItemDefinition{});
    }
    if (definition.key.empty())
    {
        PRED_LOG_ERROR(Gameplay, "Item definition has no key, ignored");
        return kInvalidItem;
    }
    if (const auto it = m_byKey.find(definition.key); it != m_byKey.end())
    {
        PRED_LOG_WARN(Gameplay, "Duplicate item key '{}', replacing the earlier definition",
                      definition.key);
        definition.id = it->second;
        m_items[static_cast<size_t>(definition.id)] = definition;
        return definition.id;
    }

    definition.id = static_cast<ItemId>(m_items.size());
    m_byKey[definition.key] = definition.id;
    m_items.push_back(std::move(definition));
    return m_items.back().id;
}

bool ItemDatabase::LoadFromFile(const std::filesystem::path& file)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_WARN(Gameplay, "Item database not found, using built-in items: {}", file.string());
        AddBuiltinDefaults();
        return false;
    }

    const nlohmann::json json = nlohmann::json::parse(stream, nullptr, false, true);
    if (json.is_discarded() || !json.contains("items") || !json["items"].is_array())
    {
        PRED_LOG_ERROR(Gameplay, "Item database is invalid (expected {{\"items\": [...]}}): {}",
                       file.string());
        AddBuiltinDefaults();
        return false;
    }

    m_items.clear();
    m_byKey.clear();

    for (const auto& entry : json["items"])
    {
        if (!entry.is_object() || !entry.contains("key"))
        {
            PRED_LOG_WARN(Gameplay, "Item entry without a key, ignored");
            continue;
        }

        ItemDefinition definition;
        definition.key = entry["key"].get<std::string>();
        definition.name = entry.value("name", definition.key);
        definition.maxStack = std::max(1, entry.value("max_stack", 1));
        definition.mass = entry.value("mass", 1.0f);
        definition.shape = ParseShape(entry.value("shape", std::string("box")));
        definition.roughness = entry.value("roughness", 0.7f);
        definition.metallic = entry.value("metallic", 0.0f);
        definition.emissive = entry.value("emissive", 0.0f);
        if (entry.contains("size"))
        {
            definition.size = ReadVec3(entry["size"], definition.size);
        }
        if (entry.contains("color"))
        {
            definition.color = ReadVec3(entry["color"], definition.color);
        }
        Add(std::move(definition));
    }

    PRED_LOG_INFO(Gameplay, "Loaded {} item definitions from {}", Count(), file.string());
    return Count() > 0;
}

void ItemDatabase::AddBuiltinDefaults()
{
    if (Count() > 0)
    {
        return;
    }
    ItemDefinition sample;
    sample.key = "sample_container";
    sample.name = "Sample Container";
    sample.color = {0.55f, 0.62f, 0.45f};
    Add(sample);
}

const ItemDefinition* ItemDatabase::Get(ItemId id) const
{
    if (id <= kInvalidItem || static_cast<size_t>(id) >= m_items.size())
    {
        return nullptr;
    }
    return &m_items[static_cast<size_t>(id)];
}

const ItemDefinition* ItemDatabase::Find(const std::string& key) const
{
    const auto it = m_byKey.find(key);
    return it == m_byKey.end() ? nullptr : Get(it->second);
}

ItemId ItemDatabase::IdOf(const std::string& key) const
{
    const auto it = m_byKey.find(key);
    return it == m_byKey.end() ? kInvalidItem : it->second;
}

} // namespace pred
