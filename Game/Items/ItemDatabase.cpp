#include "Game/Items/ItemDatabase.h"
#include "Engine/Core/JsonText.h"

#include "Engine/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

namespace
{

// A motion as it is written: one array per key, [t, right, up, forward, turn x, turn y, turn z].
std::vector<ItemMotionKey> ReadMotion(const nlohmann::json& keys)
{
    std::vector<ItemMotionKey> motion;
    if (!keys.is_array())
    {
        return motion;
    }
    for (const nlohmann::json& key : keys)
    {
        if (!key.is_array() || key.size() < 7)
        {
            continue;
        }
        ItemMotionKey read;
        read.t = std::clamp(key[0].get<float>(), 0.0f, 1.0f);
        read.offset = {key[1].get<float>(), key[2].get<float>(), key[3].get<float>()};
        read.turn = {key[4].get<float>(), key[5].get<float>(), key[6].get<float>()};
        motion.push_back(read);
    }
    std::sort(motion.begin(), motion.end(), [](const ItemMotionKey& a, const ItemMotionKey& b) { return a.t < b.t; });
    return motion;
}

nlohmann::json WriteMotion(const std::vector<ItemMotionKey>& motion)
{
    nlohmann::json keys = nlohmann::json::array();
    for (const ItemMotionKey& key : motion)
    {
        keys.push_back({key.t, key.offset.x, key.offset.y, key.offset.z, key.turn.x, key.turn.y, key.turn.z});
    }
    return keys;
}

} // namespace

const char* ItemUseKindName(ItemUseKind kind)
{
    switch (kind)
    {
    case ItemUseKind::Heal:
        return "heal";
    case ItemUseKind::Recharge:
        return "recharge";
    case ItemUseKind::Unlock:
        return "unlock";
    case ItemUseKind::Flare:
        return "flare";
    case ItemUseKind::Inspect:
        return "inspect";
    case ItemUseKind::None:
        break;
    }
    return "none";
}

ItemUseKind ItemUseKindFromString(const std::string& name)
{
    for (const ItemUseKind kind : {ItemUseKind::Heal, ItemUseKind::Recharge, ItemUseKind::Unlock, ItemUseKind::Flare,
                                   ItemUseKind::Inspect})
    {
        if (name == ItemUseKindName(kind))
        {
            return kind;
        }
    }
    return ItemUseKind::None;
}

ItemMotionKey SampleMotion(const std::vector<ItemMotionKey>& keys, float t)
{
    if (keys.empty())
    {
        return ItemMotionKey{};
    }
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= keys.front().t)
    {
        return keys.front();
    }
    for (size_t i = 1; i < keys.size(); ++i)
    {
        if (t <= keys[i].t)
        {
            const ItemMotionKey& a = keys[i - 1];
            const ItemMotionKey& b = keys[i];
            const float span = std::max(b.t - a.t, 1e-4f);
            float u = (t - a.t) / span;
            u = u * u * (3.0f - 2.0f * u);
            ItemMotionKey out;
            out.t = t;
            out.offset = a.offset + (b.offset - a.offset) * u;
            out.turn = a.turn + (b.turn - a.turn) * u;
            return out;
        }
    }
    return keys.back();
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
        // Where it sits in the hand, placed by eye in the editor and written back here.
        if (entry.contains("hold_offset"))
        {
            definition.holdOffset = ReadVec3(entry["hold_offset"], definition.holdOffset);
        }
        if (entry.contains("hold_rotation"))
        {
            definition.holdRotation = ReadVec3(entry["hold_rotation"], definition.holdRotation);
        }
        definition.benchCount = std::max(0, entry.value("bench_count", 1));
        if (const auto use = entry.find("use"); use != entry.end() && use->is_object())
        {
            ItemUse& out = definition.use;
            out.kind = ItemUseKindFromString(use->value("kind", std::string("none")));
            out.seconds = std::max(use->value("seconds", 1.0f), 0.05f);
            out.amount = use->value("amount", 0.0f);
            out.consumed = use->value("consumed", false);
            out.startSound = use->value("start_sound", std::string());
            out.doneSound = use->value("done_sound", std::string());
            out.secondSeconds = std::max(use->value("second_seconds", 0.5f), 0.05f);
            out.secondSound = use->value("second_sound", std::string());
            out.motion = ReadMotion(use->value("motion", nlohmann::json::array()));
            out.secondMotion = ReadMotion(use->value("second_motion", nlohmann::json::array()));
            if (out.kind == ItemUseKind::None && use->contains("kind"))
            {
                PRED_LOG_WARN(Gameplay, "Item '{}' has a use of a kind nothing knows: '{}'", definition.key,
                              use->value("kind", std::string()));
            }
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

ItemDefinition* ItemDatabase::Mutable(ItemId id)
{
    if (id <= kInvalidItem || static_cast<size_t>(id) >= m_items.size())
    {
        return nullptr;
    }
    return &m_items[static_cast<size_t>(id)];
}

bool ItemDatabase::SaveHoldPlacements(const std::filesystem::path& file) const
{
    // Read, change two fields per item, write back. Not built from these structs, because this
    // loader reads a handful of the fields a person might put in that file and writing it out from
    // memory would silently delete the rest.
    nlohmann::json json;
    {
        std::ifstream stream(file);
        if (!stream.is_open())
        {
            PRED_LOG_ERROR(Gameplay, "Cannot open {} to write hold placements", file.string());
            return false;
        }
        try
        {
            stream >> json;
        }
        catch (const std::exception& error)
        {
            PRED_LOG_ERROR(Gameplay, "{} is not valid JSON: {}", file.string(), error.what());
            return false;
        }
    }

    const auto items = json.find("items");
    if (items == json.end() || !items->is_array())
    {
        PRED_LOG_ERROR(Gameplay, "{} has no 'items' array", file.string());
        return false;
    }

    const auto write = [](nlohmann::json& node, const char* key, const glm::vec3& value)
    { node[key] = nlohmann::json::array({value.x, value.y, value.z}); };

    for (nlohmann::json& entry : *items)
    {
        const std::string key = entry.value("key", std::string());
        const ItemDefinition* definition = Find(key);
        if (definition == nullptr)
        {
            continue;
        }
        write(entry, "hold_offset", definition->holdOffset);
        write(entry, "hold_rotation", definition->holdRotation);
        if (definition->use.kind != ItemUseKind::None && entry.contains("use"))
        {
            nlohmann::json& use = entry["use"];
            use["seconds"] = definition->use.seconds;
            use["motion"] = WriteMotion(definition->use.motion);
            if (!definition->use.secondMotion.empty())
            {
                use["second_seconds"] = definition->use.secondSeconds;
                use["second_motion"] = WriteMotion(definition->use.secondMotion);
            }
        }
    }

    std::ofstream out(file);
    if (!out.is_open())
    {
        PRED_LOG_ERROR(Gameplay, "Cannot write {}", file.string());
        return false;
    }
    out << JsonText(json);
    PRED_LOG_INFO(Gameplay, "Wrote hold placements to {}", file.string());
    return true;
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
