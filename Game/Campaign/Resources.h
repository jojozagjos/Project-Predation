#pragma once

#include "Game/World/SitePlan.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pred
{

// What can be taken out of the ground at a site (Assets/Data/resources.json): a reason to go to a world besides a contract,
// and different worlds for different things. Taken out of a deposit it is a sample to carry (an item, items.json); carried
// aboard the shuttle as it leaves, it goes into the ship's hold (CampaignState::cargo); sold at an outpost for credits -- and,
// when upgrades come (Phase 8), built from.
struct ResourceDef
{
    std::string key;  // its item's key, and its key in the hold
    std::string name;
    int value = 10;   // credits a unit, sold
    glm::vec3 color{0.5f};
    std::map<std::string, float> biomes; // where it is found, and how often
};

// A deposit on a site: where, of what, and how much is in it.
struct Deposit
{
    glm::vec3 at{0.0f};
    std::string key;
    int units = 0;
    float turn = 0.0f; // its rocks' way round, for its look
};

class ResourceRules
{
public:
    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);

    const std::vector<ResourceDef>& All() const { return m_resources; }
    const ResourceDef* Find(const std::string& key) const;
    // What the hold is worth, sold.
    int64_t ValueOf(const std::map<std::string, int>& cargo) const;

    // The deposits on a site of a world of `biome` (none on a gas giant's, nor where nothing is found): out on its open ground
    // among the buildings and round them, never in one, nor by the pad, nor on top of the mission's own markers -- from the
    // site's seed, the same on every machine.
    std::vector<Deposit> Place(const SitePlan& site, uint32_t seed, const std::string& biome, const std::vector<glm::vec3>& keepClear = {}) const;

private:
    std::vector<ResourceDef> m_resources;
    glm::ivec2 m_deposits{3, 5};
    glm::ivec2 m_units{2, 4};
};

} // namespace pred
