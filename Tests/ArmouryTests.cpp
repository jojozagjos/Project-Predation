#include "Game/Campaign/Armoury.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace pred;

namespace
{

const std::filesystem::path kData = std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data";

ArmouryRules LoadRules()
{
    ArmouryRules rules;
    REQUIRE(rules.LoadFromFile(kData / "armoury.json"));
    return rules;
}

int GoodIndex(const ArmouryRules& rules, const std::string& key)
{
    for (size_t g = 0; g < rules.Goods().size(); ++g)
    {
        if (rules.Goods()[g].key == key)
        {
            return static_cast<int>(g);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("The armoury's data names real things: items, weapons' rounds, parts, resources", "[armoury]")
{
    const ArmouryRules rules = LoadRules();
    ItemDatabase items;
    REQUIRE(items.LoadFromFile(kData / "items.json"));
    const auto real = [&](const std::string& key)
    { return key.rfind("ammo:", 0) == 0 ? items.Find(key.substr(5)) != nullptr : items.Find(key) != nullptr && items.Find(key)->loadoutMax > 0; };
    CHECK(!rules.Starting().empty());
    for (const auto& [key, count] : rules.Starting())
    {
        INFO(key);
        CHECK(real(key));
        CHECK(count > 0);
    }
    for (const ArmouryRules::Good& good : rules.Goods())
    {
        INFO(good.key);
        CHECK(real(good.key));
    }
    for (const ArmouryRules::Part& part : rules.Parts())
    {
        INFO(part.key);
        CHECK(items.Find(part.key) != nullptr);
    }
    CHECK(rules.Parts().size() == 3); // (MissionSpec::kPartKinds)
    for (const ArmouryRules::Upgrade& upgrade : rules.Upgrades())
    {
        for (const ArmouryRules::Tier& tier : upgrade.tiers)
        {
            INFO(upgrade.key);
            CHECK(tier.cost > 0);
            CHECK((tier.part.empty() || rules.FindPart(tier.part) != nullptr));
            for (const auto& [key, count] : tier.cargo)
            {
                CHECK(items.Find(key) != nullptr);
            }
        }
    }
    CHECK(rules.FindUpgrade("travel") != nullptr);
    CHECK(rules.FindUpgrade("sensors") != nullptr);
}

TEST_CASE("An outpost's shelf is its own, the same every visit, and always has the basics", "[armoury]")
{
    const ArmouryRules rules = LoadRules();
    const auto a = rules.Shelf("1:2:3");
    const auto b = rules.Shelf("1:2:3");
    REQUIRE(a.size() == b.size());
    for (size_t k = 0; k < a.size(); ++k)
    {
        CHECK(a[k].good == b[k].good);
        CHECK(a[k].price == b[k].price);
    }
    bool differ = false;
    for (int outpost = 0; outpost < 20 && !differ; ++outpost)
    {
        const auto other = rules.Shelf("9:" + std::to_string(outpost) + ":0");
        differ = other.size() != a.size() || other.front().price != a.front().price;
        for (size_t g = 0; g < rules.Goods().size(); ++g)
        {
            if (rules.Goods()[g].always)
            {
                bool there = false;
                for (const ArmouryRules::Offer& offer : other)
                {
                    there = there || offer.good == static_cast<int>(g);
                }
                CHECK(there);
            }
        }
    }
    CHECK(differ);
}

TEST_CASE("Buying at an outpost: credits for a pack into the armoury; not what it does not sell, nor short of credits", "[armoury]")
{
    const ArmouryRules rules = LoadRules();
    CampaignState campaign;
    campaign.credits = 1000;
    const int medkit = GoodIndex(rules, "medkit");
    REQUIRE(medkit >= 0);
    REQUIRE(Armoury::Buy(campaign, rules, "1:2:3", medkit));
    CHECK(campaign.stock["medkit"] == rules.Goods()[static_cast<size_t>(medkit)].pack);
    CHECK(campaign.credits < 1000);
    campaign.credits = 0;
    CHECK_FALSE(Armoury::Buy(campaign, rules, "1:2:3", medkit));
    CHECK_FALSE(Armoury::Buy(campaign, rules, "1:2:3", 999));
}

TEST_CASE("An upgrade is fitted a tier at a time for credits, samples from the hold and a found part", "[armoury]")
{
    const ArmouryRules rules = LoadRules();
    CampaignState campaign;
    campaign.credits = 100000;
    // The drive's first tier: credits only.
    REQUIRE(Armoury::CannotFit(campaign, rules, "travel").empty());
    REQUIRE(Armoury::Fit(campaign, rules, "travel"));
    CHECK(campaign.Upgrade("travel") == 1);
    // Its second wants helium-3 from the hold.
    CHECK_FALSE(Armoury::CannotFit(campaign, rules, "travel").empty());
    campaign.cargo["res_helium3"] = 10;
    REQUIRE(Armoury::Fit(campaign, rules, "travel"));
    CHECK(campaign.cargo["res_helium3"] == 6);
    // Its third a drive coil, found on a site.
    CHECK(Armoury::CannotFit(campaign, rules, "travel").find("Drive Coil") != std::string::npos);
    CHECK_FALSE(Armoury::Fit(campaign, rules, "travel"));
    campaign.components["drive_coil"] = 1;
    REQUIRE(Armoury::Fit(campaign, rules, "travel"));
    CHECK(campaign.Components("drive_coil") == 0);
    CHECK(campaign.Upgrade("travel") == 3);
    CHECK(campaign.cargo.count("res_helium3") == 0); // (all six used: the hold empty of it)
}

TEST_CASE("The locker draws out of the armoury: handed back first, held to what there is, weapons loaded from its rounds", "[armoury]")
{
    ItemDatabase items;
    REQUIRE(items.LoadFromFile(kData / "items.json"));
    WeaponDatabase weapons;
    REQUIRE(weapons.LoadFromFile(kData / "weapons.json"));
    const ItemId medkit = items.IdOf("medkit");
    const ItemId sidearm = items.IdOf("sidearm");
    const WeaponDefinition* pistol = weapons.Get(weapons.ForItem("sidearm"));
    REQUIRE(pistol != nullptr);

    std::map<std::string, int> stock{{"medkit", 1}, {"sidearm", 1}, {Armoury::AmmoKey("sidearm"), pistol->magazineSize + 5}};
    Inventory inventory;
    // Two wanted, one there.
    Loadout kit = Armoury::Within(items, stock, {{medkit, 2}, {sidearm, 1}});
    const Loadout drawn = Loadouts::Draw(items, inventory, kit);
    Armoury::Issue(stock, items, weapons, inventory, drawn);
    CHECK(inventory.CountOf(medkit) == 1);
    CHECK(stock["medkit"] == 0);
    CHECK(stock["sidearm"] == 0);
    // The pistol's magazine full, five spare: the armoury's rounds all gone.
    int slot = -1;
    for (int i = 0; i < inventory.SlotCount(); ++i)
    {
        slot = inventory.At(i).item == sidearm ? i : slot;
    }
    REQUIRE(slot >= 0);
    CHECK(inventory.At(slot).rounds == pistol->magazineSize);
    CHECK(inventory.At(slot).reserve == 5);
    CHECK(stock[Armoury::AmmoKey("sidearm")] == 0);

    // Three shots fired, then all of it handed back: the medkit, the pistol, and what is left in it.
    inventory.SetSlotAmmo(slot, pistol->magazineSize - 3, 5);
    Armoury::HandBack(stock, items, weapons, inventory);
    CHECK(stock["medkit"] == 1);
    CHECK(stock["sidearm"] == 1);
    CHECK(stock[Armoury::AmmoKey("sidearm")] == pistol->magazineSize + 2);

    // The host's way, for somebody else: the same counts.
    std::map<std::string, int> host{{"medkit", 1}, {"sidearm", 1}, {Armoury::AmmoKey("sidearm"), 0}};
    Armoury::HandBack(host, items, {{sidearm, 1}}, {{sidearm, 7}});
    CHECK(host["sidearm"] == 2);
    CHECK(host[Armoury::AmmoKey("sidearm")] == 7);
    Armoury::Issue(host, items, weapons, Loadout{{sidearm, 1}});
    CHECK(host["sidearm"] == 1);
    CHECK(host[Armoury::AmmoKey("sidearm")] == 0);
}

TEST_CASE("Something big takes both hands: one at a time; and the belt grows with the webbing, keeping what is on it", "[armoury][inventory]")
{
    ItemDatabase items;
    REQUIRE(items.LoadFromFile(kData / "items.json"));
    const ItemId copy = items.IdOf("copy_unit");
    const ItemId fuel = items.IdOf("fuel_can");
    const ItemId medkit = items.IdOf("medkit");
    REQUIRE(items.Get(copy)->twoHanded);
    REQUIRE_FALSE(items.Get(medkit)->twoHanded);
    Inventory inventory(6);
    CHECK(inventory.TwoHandedSlot(items) == Inventory::kNoSlot);
    CHECK(inventory.Add(items, medkit, 1) == 1);
    CHECK(inventory.Add(items, copy, 1) == 1);
    const int big = inventory.TwoHandedSlot(items);
    REQUIRE(big != Inventory::kNoSlot);
    CHECK(inventory.At(big).item == copy);
    // A second big thing: no -- though there is room on the belt. Small things still go on it.
    CHECK_FALSE(inventory.CanAdd(items, fuel, 1));
    CHECK(inventory.Add(items, fuel, 1) == 0);
    CHECK(inventory.CanAdd(items, medkit, 1));
    // Put down, the hands are free for the next.
    inventory.RemoveFromSlot(big, 1);
    CHECK(inventory.Add(items, fuel, 1) == 1);
    // Two more slots of webbing: what was on it kept where it was.
    inventory.Resize(8);
    CHECK(inventory.SlotCount() == 8);
    CHECK(inventory.CountOf(medkit) == 1);
    CHECK(inventory.CountOf(fuel) == 1);
}

TEST_CASE("A wipe costs a share of the credits, up to a most; the armoury is kept in a save", "[armoury]")
{
    const ArmouryRules rules = LoadRules();
    CHECK(rules.WipeFee(1000) == 150);
    CHECK(rules.WipeFee(1000000) == 2000);
    CHECK(rules.WipeFee(-5) == 0);
    CampaignState campaign;
    campaign.stock = {{"medkit", 3}};
    campaign.stocked = true;
    CampaignState back;
    REQUIRE(CampaignState::FromJson(campaign.ToJson(), back));
    CHECK(back.stocked);
    CHECK(back.stock["medkit"] == 3);
    // (One saved before there was an armoury: not stocked, given a new one's by the game.)
    nlohmann::json old = campaign.ToJson();
    old.erase("stock");
    CampaignState older;
    REQUIRE(CampaignState::FromJson(old, older));
    CHECK_FALSE(older.stocked);
}
