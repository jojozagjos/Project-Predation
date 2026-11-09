#include "Game/Campaign/Contracts.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace pred
{
namespace Contracts
{
namespace
{

std::string Replace(std::string text, const std::string& key, const std::string& with)
{
    for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + with.size()))
    {
        text.replace(at, key.size(), with);
    }
    return text;
}

void ReadLines(const nlohmann::json& root, const char* name, std::map<std::string, std::string>& into)
{
    if (const auto lines = root.find(name); lines != root.end() && lines->is_object())
    {
        for (const auto& [kind, line] : lines->items())
        {
            if (line.is_string())
            {
                into[kind] = line.get<std::string>();
            }
        }
    }
}

} // namespace

bool Rules::LoadFromFile(const std::filesystem::path& file, std::string* error)
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
    if (!root.is_object())
    {
        if (error != nullptr)
        {
            *error = "not an object";
        }
        return false;
    }
    Rules loaded = Default();
    loaded.set.clear();
    for (const auto& entry : root.value("set", nlohmann::json::array()))
    {
        if (entry.is_object())
        {
            loaded.set.push_back({entry.value("area", std::string()), entry.value("reward", int64_t{0})});
        }
    }
    if (const auto amounts = root.find("rewards"); amounts != root.end() && amounts->is_object())
    {
        for (const auto& [kind, amount] : amounts->items())
        {
            if (amount.is_number())
            {
                loaded.rewards[kind] = amount.get<int64_t>();
            }
        }
    }
    loaded.rewardPerLightYear = root.value("reward_per_light_year", loaded.rewardPerLightYear);
    if (const auto every = root.find("offer_every"); every != root.end() && every->is_array() && every->size() == 2 && (*every)[0].is_number() &&
                                                     (*every)[1].is_number())
    {
        loaded.offerEvery[0] = (*every)[0].get<float>();
        loaded.offerEvery[1] = std::max((*every)[1].get<float>(), loaded.offerEvery[0]);
    }
    loaded.maxOffered = std::max(root.value("max_offered", loaded.maxOffered), 1);
    loaded.leadChance = std::clamp(root.value("lead_chance", loaded.leadChance), 0.0f, 1.0f);
    ReadLines(root, "wanted", loaded.wanted);
    ReadLines(root, "leads", loaded.leads);
    *this = loaded;
    return true;
}

Rules Rules::Default()
{
    Rules rules;
    rules.rewards = {{"download_data", 600}, {"restore", 800}, {"survey", 450}, {"signal", 550}};
    return rules;
}

std::string Rules::Wanted(const std::string& kind) const
{
    const auto found = wanted.find(kind);
    return found != wanted.end() ? found->second : std::string();
}

std::string Rules::LeadNote(const std::string& kind, const std::string& place, const std::string& body) const
{
    const auto found = leads.find(kind);
    const std::string note = found != leads.end() ? found->second : std::string("{place}, {body}.");
    return Replace(Replace(note, "{place}", place), "{body}", body);
}

int64_t Rules::Reward(const std::string& kind, float lightYearsFromHome) const
{
    const auto found = rewards.find(kind);
    const double base = found != rewards.end() ? static_cast<double>(found->second) : 500.0;
    const double more = 1.0 + static_cast<double>(rewardPerLightYear) * std::max(static_cast<double>(lightYearsFromHome), 0.0);
    return static_cast<int64_t>(std::llround(base * more / 10.0)) * 10;
}

CampaignState::Contract& Offer(CampaignState& campaign, uint64_t system, int body, int region, const std::string& kind, int64_t reward, int set)
{
    CampaignState::Contract contract;
    contract.id = campaign.nextContractId++;
    contract.system = system;
    contract.body = body;
    contract.region = region;
    contract.kind = kind;
    contract.reward = reward;
    contract.offered = campaign.clock;
    contract.set = set;
    campaign.contracts.push_back(contract);
    return campaign.contracts.back();
}

bool Accept(CampaignState& campaign, uint32_t id)
{
    CampaignState::Contract* contract = campaign.FindContract(id);
    if (contract == nullptr || contract->status != CampaignState::Contract::Status::Offered)
    {
        return false;
    }
    contract->status = CampaignState::Contract::Status::Accepted;
    return true;
}

CampaignState::Contract* OpenAt(CampaignState& campaign, uint64_t system, int body, int region)
{
    // (One taken on before one only offered, should there be both.)
    CampaignState::Contract* offered = nullptr;
    for (CampaignState::Contract& contract : campaign.contracts)
    {
        if (contract.system != system || contract.body != body || contract.region != region || contract.status == CampaignState::Contract::Status::Done)
        {
            continue;
        }
        if (contract.status == CampaignState::Contract::Status::Accepted)
        {
            return &contract;
        }
        offered = offered != nullptr ? offered : &contract;
    }
    return offered;
}

CampaignState::Contract* Complete(CampaignState& campaign, uint64_t system, int body, int region)
{
    CampaignState::Contract* contract = OpenAt(campaign, system, body, region);
    if (contract == nullptr)
    {
        return nullptr;
    }
    contract->status = CampaignState::Contract::Status::Done;
    contract->done = campaign.clock;
    campaign.credits += contract->reward;
    if (contract->set >= 0)
    {
        campaign.setContractsDone = std::max(campaign.setContractsDone, contract->set + 1);
    }
    return contract;
}

int Offered(const CampaignState& campaign)
{
    return static_cast<int>(std::count_if(campaign.contracts.begin(), campaign.contracts.end(), [](const CampaignState::Contract& contract)
                                          { return contract.status == CampaignState::Contract::Status::Offered; }));
}

} // namespace Contracts
} // namespace pred
