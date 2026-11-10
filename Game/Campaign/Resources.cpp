#include "Game/Campaign/Resources.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace pred
{

namespace
{

// Its own numbers from the site's seed, so placing deposits changes nothing else drawn from it.
class Draws
{
public:
    explicit Draws(uint64_t seed) : m_state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1u << 24); }
    float Range(float a, float b) { return a + (b - a) * Unit(); }
    int Int(int a, int b) { return a + static_cast<int>(Next() % static_cast<uint64_t>(std::max(b - a + 1, 1))); }

private:
    uint64_t m_state;
};

} // namespace

bool ResourceRules::LoadFromFile(const std::filesystem::path& file, std::string* error)
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
    m_resources.clear();
    if (const auto d = root.find("deposits"); d != root.end() && d->is_array() && d->size() == 2)
    {
        m_deposits = {(*d)[0].get<int>(), (*d)[1].get<int>()};
    }
    if (const auto u = root.find("units"); u != root.end() && u->is_array() && u->size() == 2)
    {
        m_units = {(*u)[0].get<int>(), (*u)[1].get<int>()};
    }
    for (const nlohmann::json& entry : root.value("resources", nlohmann::json::array()))
    {
        ResourceDef def;
        def.key = entry.value("key", std::string());
        def.name = entry.value("name", def.key);
        def.value = entry.value("value", 10);
        if (const auto c = entry.find("color"); c != entry.end() && c->is_array() && c->size() == 3)
        {
            def.color = {(*c)[0].get<float>(), (*c)[1].get<float>(), (*c)[2].get<float>()};
        }
        if (const auto b = entry.find("biomes"); b != entry.end() && b->is_object())
        {
            for (const auto& [biome, weight] : b->items())
            {
                def.biomes[biome] = weight.get<float>();
            }
        }
        if (!def.key.empty())
        {
            m_resources.push_back(std::move(def));
        }
    }
    return !m_resources.empty();
}

const ResourceDef* ResourceRules::Find(const std::string& key) const
{
    const auto it = std::find_if(m_resources.begin(), m_resources.end(), [&](const ResourceDef& def) { return def.key == key; });
    return it == m_resources.end() ? nullptr : &*it;
}

int64_t ResourceRules::ValueOf(const std::map<std::string, int>& cargo) const
{
    int64_t total = 0;
    for (const auto& [key, count] : cargo)
    {
        if (const ResourceDef* def = Find(key))
        {
            total += static_cast<int64_t>(def->value) * std::max(count, 0);
        }
    }
    return total;
}

std::vector<Deposit> ResourceRules::Place(const SitePlan& site, uint32_t seed, const std::string& biome, const std::vector<glm::vec3>& keepClear) const
{
    // What is found on this kind of world, and how often.
    std::vector<std::pair<const ResourceDef*, float>> found;
    float total = 0.0f;
    for (const ResourceDef& def : m_resources)
    {
        if (const auto it = def.biomes.find(biome); it != def.biomes.end() && it->second > 0.0f)
        {
            found.push_back({&def, it->second});
            total += it->second;
        }
    }
    std::vector<Deposit> deposits;
    if (found.empty())
    {
        return deposits;
    }
    Draws draws((static_cast<uint64_t>(seed) << 24) ^ 0x0DE905175ull);
    const int wanted = draws.Int(m_deposits.x, m_deposits.y);
    const glm::vec2 middle = glm::vec2(site.origin.x, site.origin.z) + glm::vec2(site.size * 0.5f);
    const glm::vec2 pad{site.landing.x, site.landing.z};
    for (int attempt = 0; attempt < 300 && static_cast<int>(deposits.size()) < wanted; ++attempt)
    {
        const float angle = draws.Range(0.0f, glm::two_pi<float>());
        const glm::vec2 at = middle + glm::vec2(std::cos(angle), std::sin(angle)) * draws.Range(0.12f, 0.46f) * site.size;
        const float pick = draws.Range(0.0f, total);
        const int units = draws.Int(m_units.x, m_units.y);
        const float turn = draws.Range(0.0f, glm::two_pi<float>());
        bool fits = !site.InBuilding(at, 6.0f) && glm::distance(at, pad) > 26.0f;
        for (const Deposit& other : deposits)
        {
            fits = fits && glm::distance(at, glm::vec2(other.at.x, other.at.z)) > site.size * 0.12f;
        }
        for (const glm::vec3& other : keepClear)
        {
            fits = fits && glm::distance(at, glm::vec2(other.x, other.z)) > 8.0f;
        }
        if (!fits)
        {
            continue;
        }
        float upTo = 0.0f;
        const ResourceDef* chosen = found.back().first;
        for (const auto& [def, weight] : found)
        {
            upTo += weight;
            if (pick <= upTo)
            {
                chosen = def;
                break;
            }
        }
        deposits.push_back({{at.x, site.origin.y, at.y}, chosen->key, units, turn});
    }
    return deposits;
}

} // namespace pred
