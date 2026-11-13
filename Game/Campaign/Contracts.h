#pragma once

#include "Game/Campaign/Campaign.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pred
{

// Contracts (Phase 4): the work offered at the ship's terminal, each a landing area's mission and what it pays. The first few are
// set ones in the home system -- the same in every campaign, and easy -- one at a time, the next once the last is done; then
// others are offered every few minutes, in the system the ship is in (and those the drive reaches). Taking one on plots the
// course; coming back with its mission done pays it.
//
// Only the rules and the bookkeeping here, on the campaign's state; the game (PredationGameContracts.cpp) offers them, puts them
// on the terminal and the map, and pays them. What they say and pay is data (Assets/Data/contracts.json).
namespace Contracts
{

struct Rules
{
    // The set contracts, in order: the area (its designation, in the home system) and what it pays.
    struct Set
    {
        std::string area;
        int64_t reward = 0;
    };
    std::vector<Set> set;
    // What any other pays, by its mission's template, and how much more the further from home (a share of it a light year).
    std::map<std::string, int64_t> rewards;
    float rewardPerLightYear = 0.05f;
    // How often another is offered (seconds of the campaign's clock, between these), and how many may be waiting at once.
    float offerEvery[2]{240.0f, 480.0f};
    int maxOffered = 3;
    // How often a mission done turns up a lead to somewhere else.
    float leadChance = 0.5f;
    // What a mission's optional goal pays (MissionPlan::extra: its records drive brought back), contract or none.
    int64_t extraReward = 150;
    // By template: what the contract asks, in a line; and a lead's note ({place} the area, {body} the world it is on).
    std::map<std::string, std::string> wanted;
    std::map<std::string, std::string> leads;

    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);
    static Rules Default();

    std::string Wanted(const std::string& kind) const;
    std::string LeadNote(const std::string& kind, const std::string& place, const std::string& body) const;
    // What one pays: by its kind, more the further out it is, to the nearest ten.
    int64_t Reward(const std::string& kind, float lightYearsFromHome) const;
};

// A contract offered for an area, numbered.
CampaignState::Contract& Offer(CampaignState& campaign, uint64_t system, int body, int region, const std::string& kind, int64_t reward,
                               int set);
// Taken on. False if it is not there or not on offer.
bool Accept(CampaignState& campaign, uint32_t id);
// The contract offered or taken on for an area, or none.
CampaignState::Contract* OpenAt(CampaignState& campaign, uint64_t system, int body, int region);
// The crew back with an area's mission done: its contract (offered or taken on) done and paid, the set ones moved on; none if
// there was none.
CampaignState::Contract* Complete(CampaignState& campaign, uint64_t system, int body, int region);
// How many are on offer, not yet taken on.
int Offered(const CampaignState& campaign);

} // namespace Contracts

} // namespace pred
