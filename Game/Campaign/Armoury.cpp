#include "Game/Campaign/Armoury.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace pred
{

namespace
{

// Its own numbers from an outpost's key: FNV-1a over it, then mixed a step at a time.
class Draws
{
public:
    explicit Draws(const std::string& key)
    {
        for (const char c : key)
        {
            m_state = (m_state ^ static_cast<uint8_t>(c)) * 0x100000001B3ull;
        }
    }
    float Unit()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / static_cast<float>(1u << 24);
    }

private:
    uint64_t m_state = 0xCBF29CE484222325ull;
};

// How many a weapon carries in all, drawn: its magazine and its spares.
const WeaponDefinition* WeaponFor(const ItemDatabase& items, const WeaponDatabase& weapons, ItemId item)
{
    const ItemDefinition* definition = items.Get(item);
    return definition != nullptr ? weapons.Get(weapons.ForItem(definition->key)) : nullptr;
}

} // namespace

bool ArmouryRules::LoadFromFile(const std::filesystem::path& file, std::string* error)
{
    std::ifstream in(file);
    if (!in)
    {
        if (error != nullptr)
        {
            *error = "cannot open " + file.string();
        }
        return false;
    }
    nlohmann::json root;
    try
    {
        in >> root;
    }
    catch (const std::exception& e)
    {
        if (error != nullptr)
        {
            *error = e.what();
        }
        return false;
    }
    m_starting.clear();
    m_goods.clear();
    m_upgrades.clear();
    m_parts.clear();
    if (const auto starting = root.find("starting"); starting != root.end() && starting->is_object())
    {
        for (const auto& [key, count] : starting->items())
        {
            m_starting[key] = std::max(count.get<int>(), 0);
        }
    }
    for (const nlohmann::json& entry : root.value("goods", nlohmann::json::array()))
    {
        Good good;
        good.key = entry.value("key", std::string());
        good.name = entry.value("name", std::string());
        good.price = std::max(entry.value("price", 0), 1);
        good.pack = std::max(entry.value("pack", 1), 1);
        good.always = entry.value("always", false);
        if (!good.key.empty())
        {
            m_goods.push_back(good);
        }
    }
    for (const nlohmann::json& entry : root.value("parts", nlohmann::json::array()))
    {
        Part part;
        part.key = entry.value("key", std::string());
        part.name = entry.value("name", part.key);
        if (!part.key.empty())
        {
            m_parts.push_back(part);
        }
    }
    for (const nlohmann::json& entry : root.value("upgrades", nlohmann::json::array()))
    {
        Upgrade upgrade;
        upgrade.key = entry.value("key", std::string());
        upgrade.name = entry.value("name", upgrade.key);
        upgrade.blurb = entry.value("blurb", std::string());
        for (const nlohmann::json& t : entry.value("tiers", nlohmann::json::array()))
        {
            Tier tier;
            tier.cost = std::max(t.value("cost", int64_t{0}), int64_t{0});
            tier.part = t.value("part", std::string());
            tier.says = t.value("says", std::string());
            if (const auto cargo = t.find("cargo"); cargo != t.end() && cargo->is_object())
            {
                for (const auto& [key, count] : cargo->items())
                {
                    tier.cargo[key] = std::max(count.get<int>(), 0);
                }
            }
            upgrade.tiers.push_back(tier);
        }
        if (!upgrade.key.empty())
        {
            m_upgrades.push_back(upgrade);
        }
    }
    m_wipeFee = std::clamp(root.value("wipe_fee", 0.15f), 0.0f, 1.0f);
    m_wipeFeeMost = std::max(root.value("wipe_fee_most", int64_t{2000}), int64_t{0});
    return true;
}

const ArmouryRules::Upgrade* ArmouryRules::FindUpgrade(const std::string& key) const
{
    const auto it = std::find_if(m_upgrades.begin(), m_upgrades.end(), [&](const Upgrade& u) { return u.key == key; });
    return it != m_upgrades.end() ? &*it : nullptr;
}

const ArmouryRules::Part* ArmouryRules::FindPart(const std::string& key) const
{
    const auto it = std::find_if(m_parts.begin(), m_parts.end(), [&](const Part& p) { return p.key == key; });
    return it != m_parts.end() ? &*it : nullptr;
}

std::vector<ArmouryRules::Offer> ArmouryRules::Shelf(const std::string& outpost) const
{
    Draws draws(outpost);
    // The outpost dearer or cheaper on the whole, by up to a fifth; each good a little either way of that.
    const float overall = 0.8f + 0.4f * draws.Unit();
    std::vector<Offer> shelf;
    for (size_t g = 0; g < m_goods.size(); ++g)
    {
        const Good& good = m_goods[g];
        const bool stocked = draws.Unit() < 0.7f;
        const float each = 0.9f + 0.2f * draws.Unit();
        if (good.always || stocked)
        {
            shelf.push_back({static_cast<int>(g), std::max(static_cast<int>(static_cast<float>(good.price) * overall * each + 0.5f), 1)});
        }
    }
    return shelf;
}

int64_t ArmouryRules::WipeFee(int64_t credits) const
{
    return std::clamp(static_cast<int64_t>(static_cast<double>(std::max(credits, int64_t{0})) * m_wipeFee), int64_t{0}, m_wipeFeeMost);
}

namespace Armoury
{

std::string AmmoKey(const std::string& weaponItem)
{
    return "ammo:" + weaponItem;
}

bool Buy(CampaignState& campaign, const ArmouryRules& rules, const std::string& outpost, int good)
{
    for (const ArmouryRules::Offer& offer : rules.Shelf(outpost))
    {
        if (offer.good != good)
        {
            continue;
        }
        if (campaign.credits < offer.price)
        {
            return false;
        }
        const ArmouryRules::Good& bought = rules.Goods()[static_cast<size_t>(good)];
        campaign.credits -= offer.price;
        campaign.stock[bought.key] += bought.pack;
        return true;
    }
    return false;
}

std::string CannotFit(const CampaignState& campaign, const ArmouryRules& rules, const std::string& upgrade)
{
    const ArmouryRules::Upgrade* def = rules.FindUpgrade(upgrade);
    if (def == nullptr)
    {
        return "Not known";
    }
    const int tier = campaign.Upgrade(upgrade);
    if (tier >= static_cast<int>(def->tiers.size()))
    {
        return "Fully upgraded";
    }
    const ArmouryRules::Tier& next = def->tiers[static_cast<size_t>(tier)];
    if (campaign.credits < next.cost)
    {
        return "Not enough credits";
    }
    for (const auto& [key, count] : next.cargo)
    {
        const auto it = campaign.cargo.find(key);
        if ((it == campaign.cargo.end() ? 0 : it->second) < count)
        {
            return "Not enough samples in the hold";
        }
    }
    if (!next.part.empty() && campaign.Components(next.part) <= 0)
    {
        const ArmouryRules::Part* part = rules.FindPart(next.part);
        return "Needs a " + (part != nullptr ? part->name : next.part) + " (found on sites)";
    }
    return std::string();
}

bool Fit(CampaignState& campaign, const ArmouryRules& rules, const std::string& upgrade)
{
    if (!CannotFit(campaign, rules, upgrade).empty())
    {
        return false;
    }
    const ArmouryRules::Tier& next = rules.FindUpgrade(upgrade)->tiers[static_cast<size_t>(campaign.Upgrade(upgrade))];
    campaign.credits -= next.cost;
    for (const auto& [key, count] : next.cargo)
    {
        int& left = campaign.cargo[key];
        left -= count;
        if (left <= 0)
        {
            campaign.cargo.erase(key);
        }
    }
    if (!next.part.empty())
    {
        int& parts = campaign.components[next.part];
        if (--parts <= 0)
        {
            campaign.components.erase(next.part);
        }
    }
    ++campaign.upgrades[upgrade];
    return true;
}

void HandBack(std::map<std::string, int>& stock, const ItemDatabase& items, const WeaponDatabase& weapons, const Inventory& inventory)
{
    for (int i = 0; i < inventory.SlotCount(); ++i)
    {
        const Inventory::Slot& slot = inventory.At(i);
        const ItemDefinition* definition = items.Get(slot.item);
        if (slot.IsEmpty() || definition == nullptr || definition->loadoutMax <= 0)
        {
            continue;
        }
        stock[definition->key] += slot.count;
        if (const WeaponDefinition* weapon = WeaponFor(items, weapons, slot.item))
        {
            const int rounds = slot.rounds < 0 ? weapon->magazineSize : slot.rounds;
            const int reserve = slot.reserve < 0 ? weapon->reserveOnPickup : slot.reserve;
            stock[AmmoKey(definition->key)] += std::max(rounds, 0) + std::max(reserve, 0);
        }
    }
}

void HandBack(std::map<std::string, int>& stock, const ItemDatabase& items, const std::map<ItemId, int>& carried, const std::map<ItemId, int>& rounds)
{
    for (const auto& [item, count] : carried)
    {
        const ItemDefinition* definition = items.Get(item);
        if (definition != nullptr && definition->loadoutMax > 0 && count > 0)
        {
            stock[definition->key] += count;
        }
    }
    for (const auto& [item, count] : rounds)
    {
        const ItemDefinition* definition = items.Get(item);
        if (definition != nullptr && definition->loadoutMax > 0 && count > 0)
        {
            stock[AmmoKey(definition->key)] += count;
        }
    }
}

Loadout Within(const ItemDatabase& items, const std::map<std::string, int>& stock, const Loadout& kit)
{
    Loadout out;
    for (const LoadoutPick& pick : kit)
    {
        const ItemDefinition* definition = items.Get(pick.item);
        if (definition == nullptr)
        {
            continue;
        }
        const auto it = stock.find(definition->key);
        const int there = it == stock.end() ? 0 : it->second;
        // (The same thing twice: what the first took is not there for the second.)
        int taken = 0;
        for (const LoadoutPick& o : out)
        {
            taken += o.item == pick.item ? o.count : 0;
        }
        const int count = std::min(pick.count, there - taken);
        if (count > 0)
        {
            out.push_back({pick.item, count});
        }
    }
    return out;
}

Load LoadWeapon(std::map<std::string, int>& stock, const std::string& weaponItem, int magazine, int spares)
{
    int& there = stock[AmmoKey(weaponItem)];
    Load load;
    load.rounds = std::clamp(there, 0, std::max(magazine, 0));
    there -= load.rounds;
    load.reserve = std::clamp(there, 0, std::max(spares, 0));
    there -= load.reserve;
    return load;
}

void Issue(std::map<std::string, int>& stock, const ItemDatabase& items, const WeaponDatabase& weapons, Inventory& inventory, const Loadout& drawn)
{
    for (const LoadoutPick& pick : drawn)
    {
        if (const ItemDefinition* definition = items.Get(pick.item))
        {
            stock[definition->key] -= pick.count;
        }
    }
    for (int i = 0; i < inventory.SlotCount(); ++i)
    {
        const Inventory::Slot& slot = inventory.At(i);
        const ItemDefinition* definition = items.Get(slot.item);
        const WeaponDefinition* weapon = slot.IsEmpty() ? nullptr : WeaponFor(items, weapons, slot.item);
        if (weapon == nullptr || definition == nullptr || definition->loadoutMax <= 0)
        {
            continue;
        }
        const Load load = LoadWeapon(stock, definition->key, weapon->magazineSize, weapon->reserveOnPickup);
        inventory.SetSlotAmmo(i, load.rounds, load.reserve);
    }
}

void Issue(std::map<std::string, int>& stock, const ItemDatabase& items, const WeaponDatabase& weapons, const Loadout& drawn)
{
    for (const LoadoutPick& pick : drawn)
    {
        const ItemDefinition* definition = items.Get(pick.item);
        if (definition == nullptr)
        {
            continue;
        }
        stock[definition->key] -= pick.count;
        if (const WeaponDefinition* weapon = WeaponFor(items, weapons, pick.item))
        {
            for (int k = 0; k < pick.count; ++k)
            {
                LoadWeapon(stock, definition->key, weapon->magazineSize, weapon->reserveOnPickup);
            }
        }
    }
}

} // namespace Armoury

} // namespace pred
