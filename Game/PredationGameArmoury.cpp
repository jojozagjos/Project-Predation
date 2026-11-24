// The ship's armoury and refit (Phase 8; Campaign/Armoury.h). The armoury holds what the crew draws at the loadout locker, in
// numbers: drawn, it is out of it until handed back there; used up or left on a site, it is gone. More of it is bought at an outpost
// from the ship's terminal (its STORES page), each outpost its own goods at its own prices; the ship's upgrades are fitted there too
// (REFIT), for credits, samples from the hold and a part found on a site. Losing the whole crew down a site costs a fee besides.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Game/PredationGameShared.h"

#include <algorithm>

namespace pred
{

void PredationGame::LoadArmouryRules()
{
    const std::filesystem::path file = Paths::AssetsRoot() / "Data" / "armoury.json";
    std::string error;
    if (!m_armouryRules.LoadFromFile(file, &error))
    {
        PRED_LOG_WARN(Gameplay, "Armoury: {} -- nothing in it, nothing sold, no upgrades", error);
    }
    m_shipTerminal.SetArmoury(&m_armouryRules);
    m_app->GetFileWatcher().Watch(file, [this](const std::filesystem::path& path)
                                  {
                                      std::string why;
                                      if (!m_armouryRules.LoadFromFile(path, &why))
                                      {
                                          PRED_LOG_WARN(Gameplay, "Armoury: {}", why);
                                      }
                                      m_terminalDirty = true;
                                  });
}

std::map<std::string, int>& PredationGame::Stock()
{
    // A campaign begun before there was an armoury, or just now: a new one's.
    if (!m_campaign.stocked)
    {
        m_campaign.stock = m_armouryRules.Starting();
        m_campaign.stocked = true;
    }
    return m_campaign.stock;
}

std::string PredationGame::OutpostHere() const
{
    // The outpost the ship stands at -- landed at a hub, not while it is filmed coming down or going -- by its area's key.
    if (!m_campaignOpen || !ShipLanded() || GroundCinematic() || m_campaign.travel.region < 0)
    {
        return std::string();
    }
    return CampaignState::RegionKey(m_campaign.system, m_campaign.body, m_campaign.travel.region);
}

bool PredationGame::KitWanted(ItemId item) const
{
    const ItemDefinition* definition = m_items.Get(item);
    return definition != nullptr && m_order != OrderState::None &&
           std::find(m_briefingFacts.kit.begin(), m_briefingFacts.kit.end(), definition->key) != m_briefingFacts.kit.end();
}

std::string PredationGame::NoRoomFor(ItemId item) const
{
    const ItemDefinition* wanted = m_items.Get(item);
    const int big = m_inventory.TwoHandedSlot(m_items);
    if (wanted != nullptr && wanted->twoHanded && big != Inventory::kNoSlot)
    {
        const ItemDefinition* held = m_items.Get(m_inventory.At(big).item);
        return "Both hands are full with the " + (held != nullptr ? held->name : std::string("load")) + ".";
    }
    return "Inventory full";
}

void PredationGame::FitBelt()
{
    const int slots = kBeltSlots + std::clamp(m_campaignOpen ? m_campaign.Upgrade("webbing") : 0, 0, 2);
    if (m_inventory.SlotCount() != slots)
    {
        m_inventory.Resize(slots);
    }
}

std::string PredationGame::StockName(const std::string& key) const
{
    if (key.rfind("ammo:", 0) == 0)
    {
        const ItemDefinition* weapon = m_items.Find(key.substr(5));
        return (weapon != nullptr ? weapon->name : key.substr(5)) + " rounds";
    }
    const ItemDefinition* item = m_items.Find(key);
    return item != nullptr ? item->name : key;
}

bool PredationGame::BuyGood(uint8_t player, int good)
{
    const std::string outpost = OutpostHere();
    if (!IsAuthority() || outpost.empty() || good < 0 || good >= static_cast<int>(m_armouryRules.Goods().size()))
    {
        return false;
    }
    Stock();
    const int64_t before = m_campaign.credits;
    if (!Armoury::Buy(m_campaign, m_armouryRules, outpost, good))
    {
        return false;
    }
    const ArmouryRules::Good& bought = m_armouryRules.Goods()[static_cast<size_t>(good)];
    PRED_LOG_INFO(Gameplay, "Player {} bought {} x{} for {} credits ({} in the armoury now)", player, bought.key, bought.pack, before - m_campaign.credits,
                  m_campaign.stock[bought.key]);
    CampaignChanged();
    return true;
}

bool PredationGame::FitUpgrade(uint8_t player, int upgrade)
{
    if (!IsAuthority() || OutpostHere().empty() || upgrade < 0 || upgrade >= static_cast<int>(m_armouryRules.Upgrades().size()))
    {
        return false;
    }
    const ArmouryRules::Upgrade& def = m_armouryRules.Upgrades()[static_cast<size_t>(upgrade)];
    if (!Armoury::Fit(m_campaign, m_armouryRules, def.key))
    {
        return false;
    }
    const int tier = m_campaign.Upgrade(def.key);
    CampaignState::LogEntry& entry = m_campaign.AddLog("refit", "", def.name + " tier " + std::to_string(tier),
                                                       def.tiers[static_cast<size_t>(tier - 1)].says);
    entry.time = m_campaign.clock;
    PRED_LOG_INFO(Gameplay, "Player {} fitted {} tier {}", player, def.key, tier);
    CampaignChanged();
    return true;
}

CampaignState PredationGame::CampaignToSave()
{
    // What the crew carries of the armoury's is put back into it as the campaign is written down: loaded again, nobody has it.
    CampaignState saved = m_campaign;
    if (!m_campaignOpen)
    {
        return saved;
    }
    Stock();
    saved.stock = m_campaign.stock;
    saved.stocked = true;
    Inventory mine = m_inventory;
    if (m_ammoSlot != Inventory::kNoSlot && m_weapon.HasWeapon())
    {
        mine.SetSlotAmmo(m_ammoSlot, m_weapon.rounds, m_weapon.reserve);
    }
    if (m_player.State().alive)
    {
        Armoury::HandBack(saved.stock, m_items, m_weaponData, mine);
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            std::map<ItemId, int> carried;
            for (const ItemId id : Loadouts::Issued(m_items))
            {
                carried[id] = m_host.CarriedCount(remote.id, static_cast<uint16_t>(id));
            }
            Armoury::HandBack(saved.stock, m_items, carried, {});
        }
    }
    return saved;
}

void PredationGame::ChargeWipe(CampaignState::Debrief& debrief, const std::string& area, const std::string& key)
{
    // Everybody lost down there: the crew is got back (Phase 8: your rule) at a cost -- a share of the credits, up to a most.
    debrief.fee = m_armouryRules.WipeFee(m_campaign.credits);
    if (debrief.fee <= 0)
    {
        return;
    }
    m_campaign.credits -= debrief.fee;
    CampaignState::LogEntry& entry =
        m_campaign.AddLog("contract", key, area, "Crew lost. Getting them back cost " + std::to_string(debrief.fee) + " credits; what they carried is gone.");
    entry.time = m_campaign.clock;
    debrief.entries.push_back(entry.id);
    PRED_LOG_INFO(Gameplay, "Everybody lost: {} credits", debrief.fee);
}

} // namespace pred
