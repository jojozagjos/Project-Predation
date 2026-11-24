#pragma once

#include "Game/Campaign/Campaign.h"
#include "Game/Items/Inventory.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/Items/Loadout.h"
#include "Game/Weapons/WeaponDatabase.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pred
{

// The ship's armoury and its refit (Phase 8; Assets/Data/armoury.json). The armoury holds a number of each thing the crew draws
// at the loadout locker (CampaignState::stock) -- medical kits, cells, flares, tools, weapons, and their rounds -- and what is
// drawn is out of it until it is handed back at the locker; whatever is used up or left on a site is gone. Outposts sell more into
// it, each its own goods at its own prices. The ship's upgrades are fitted at an outpost too, a tier at a time, for credits, samples
// out of the hold and now and then a part found on a site.
class ArmouryRules
{
public:
    struct Good
    {
        std::string key; // an item's key, or ammo:<weapon item key> (rounds)
        std::string name; // its own (rounds have one); empty: the item's
        int price = 0;   // credits for a pack
        int pack = 1;    // how many come in one
        bool always = false; // on every outpost's shelf
    };
    struct Tier
    {
        int64_t cost = 0;
        std::map<std::string, int> cargo; // samples out of the hold, by resource key
        std::string part;                 // a part found on a site (a key of `parts`), or none
        std::string says;                 // what it does
    };
    struct Upgrade
    {
        std::string key; // its category in CampaignState::upgrades
        std::string name;
        std::string blurb;
        std::vector<Tier> tiers;
    };
    struct Part
    {
        std::string key; // its item's key, and its key in CampaignState::components
        std::string name;
    };
    // An outpost's shelf: a good it sells (an index into Goods) and what a pack costs there.
    struct Offer
    {
        int good = 0;
        int price = 0;
    };

    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);

    const std::map<std::string, int>& Starting() const { return m_starting; }
    const std::vector<Good>& Goods() const { return m_goods; }
    const std::vector<Upgrade>& Upgrades() const { return m_upgrades; }
    const std::vector<Part>& Parts() const { return m_parts; }
    const Upgrade* FindUpgrade(const std::string& key) const;
    const Part* FindPart(const std::string& key) const;

    // What an outpost sells and for how much, from its key (CampaignState::RegionKey): the same on every visit and every machine.
    // The goods always sold, and most of the rest; each priced a little above or below the list, the outpost dearer or cheaper.
    std::vector<Offer> Shelf(const std::string& outpost) const;
    // What losing the whole crew costs: a share of the credits, up to a most.
    int64_t WipeFee(int64_t credits) const;

private:
    std::map<std::string, int> m_starting;
    std::vector<Good> m_goods;
    std::vector<Upgrade> m_upgrades;
    std::vector<Part> m_parts;
    float m_wipeFee = 0.15f;
    int64_t m_wipeFeeMost = 2000;
};

namespace Armoury
{

// The armoury's key for a weapon's rounds.
std::string AmmoKey(const std::string& weaponItem);

// A pack of a good on an outpost's shelf bought into the armoury: false (nothing changed) when it is not sold there, or the
// credits are short.
bool Buy(CampaignState& campaign, const ArmouryRules& rules, const std::string& outpost, int good);

// An upgrade's next tier: why it cannot be fitted now (empty: it can) -- every tier fitted, short of credits, of samples in the
// hold, of the part.
std::string CannotFit(const CampaignState& campaign, const ArmouryRules& rules, const std::string& upgrade);
// Fitted: paid for out of the credits, the hold and the parts. False (nothing changed) when it cannot be.
bool Fit(CampaignState& campaign, const ArmouryRules& rules, const std::string& upgrade);

// What somebody hands back at the locker before drawing: everything issued that they carry, and the rounds in its weapons (a
// weapon never drawn from its slot has its full load). Added to `stock`.
void HandBack(std::map<std::string, int>& stock, const ItemDatabase& items, const WeaponDatabase& weapons, const Inventory& inventory);
// The same from the counts the host keeps of what somebody carries and the rounds they say they hand back.
void HandBack(std::map<std::string, int>& stock, const ItemDatabase& items, const std::map<ItemId, int>& carried, const std::map<ItemId, int>& rounds);
// A kit as far as the armoury has it: each thing held to how many are in `stock`.
Loadout Within(const ItemDatabase& items, const std::map<std::string, int>& stock, const Loadout& kit);
// What a weapon drawn has in it, out of the armoury's rounds: its magazine first, then its spares; taken from `stock`.
struct Load
{
    int rounds = 0;
    int reserve = 0;
};
Load LoadWeapon(std::map<std::string, int>& stock, const std::string& weaponItem, int magazine, int spares);
// A kit drawn taken out of `stock`, and every weapon in `inventory` from it loaded from the armoury's rounds (the slots' ammunition
// set). Weapons come loaded in the order they lie in the bag.
void Issue(std::map<std::string, int>& stock, const ItemDatabase& items, const WeaponDatabase& weapons, Inventory& inventory, const Loadout& drawn);
// The same for somebody else's (the host, for a client's draw): the rounds that would have gone into each weapon taken, no bag to
// load.
void Issue(std::map<std::string, int>& stock, const ItemDatabase& items, const WeaponDatabase& weapons, const Loadout& drawn);

} // namespace Armoury

} // namespace pred
