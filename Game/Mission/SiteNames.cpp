#include "Game/Mission/SiteNames.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace pred
{
namespace
{

// SplitMix64: the same sequence on every machine.
class Random
{
public:
    explicit Random(uint64_t seed) : m_state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int Int(int low, int high) { return high <= low ? low : low + static_cast<int>(Next() % static_cast<uint64_t>(high - low + 1)); }
    const std::string& Of(const std::vector<std::string>& list)
    {
        static const std::string kNone;
        return list.empty() ? kNone : list[static_cast<size_t>(Next() % list.size())];
    }

private:
    uint64_t m_state;
};

bool Parse(const std::filesystem::path& file, nlohmann::json& out, std::string* error)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        if (error != nullptr)
        {
            *error = "not found: " + file.string();
        }
        return false;
    }
    out = nlohmann::json::parse(stream, nullptr, false, true);
    if (out.is_discarded() || !out.is_object())
    {
        if (error != nullptr)
        {
            *error = "not a JSON object: " + file.string();
        }
        return false;
    }
    return true;
}

void ReadList(const nlohmann::json& from, const char* key, std::vector<std::string>& into)
{
    if (!from.contains(key) || !from[key].is_array())
    {
        return;
    }
    std::vector<std::string> read;
    for (const nlohmann::json& entry : from[key])
    {
        if (entry.is_string() && !entry.get<std::string>().empty())
        {
            read.push_back(entry.get<std::string>());
        }
    }
    if (!read.empty())
    {
        into = std::move(read);
    }
}

void ReadRange(const nlohmann::json& from, const char* key, int& low, int& high)
{
    if (from.contains(key) && from[key].is_array() && from[key].size() == 2 && from[key][0].is_number_integer() &&
        from[key][1].is_number_integer())
    {
        low = from[key][0].get<int>();
        high = std::max(low, from[key][1].get<int>());
    }
}

} // namespace

bool SiteNames::LoadFromFile(const std::filesystem::path& file, std::string* error)
{
    nlohmann::json json;
    if (!Parse(file, json, error))
    {
        return false;
    }
    if (json.contains("planet") && json["planet"].is_object())
    {
        const nlohmann::json& planet = json["planet"];
        ReadList(planet, "catalogues", catalogues);
        ReadRange(planet, "numbers", catalogueLow, catalogueHigh);
        ReadList(planet, "planets", planets);
    }
    if (json.contains("site") && json["site"].is_object())
    {
        const nlohmann::json& site = json["site"];
        ReadList(site, "qualifiers", qualifiers);
        ReadList(site, "kinds", kinds);
        ReadRange(site, "numbers", siteLow, siteHigh);
        ReadList(site, "regions", regions);
    }
    return true;
}

SiteTitle SiteNames::For(uint32_t seed) const
{
    Random random((static_cast<uint64_t>(seed) << 24) ^ 0x517E7A3Eull);
    SiteTitle title;
    const std::string& catalogue = random.Of(catalogues);
    const int number = random.Int(catalogueLow, catalogueHigh);
    const std::string& planet = random.Of(planets);
    title.planet = catalogue + "-" + std::to_string(number) + (planet.empty() ? "" : " " + planet);

    const std::string& qualifier = random.Of(qualifiers);
    const std::string& kind = random.Of(kinds);
    char site[8];
    std::snprintf(site, sizeof(site), "%02d", random.Int(siteLow, siteHigh));
    const std::string& region = random.Of(regions);
    title.site = (qualifier.empty() ? "" : qualifier + " ") + (kind.empty() ? "SITE" : kind) + " " + site + (region.empty() ? "" : ", " + region);
    title.catalogue = catalogue;
    title.catalogueNumber = number;
    title.numeral = planet;
    title.qualifier = qualifier;
    title.kind = kind.empty() ? "SITE" : kind;
    title.siteNumber = std::atoi(site);
    title.region = region;
    return title;
}

int NumeralValue(const std::string& numeral)
{
    const auto value = [](char c)
    {
        switch (c)
        {
        case 'I': return 1;
        case 'V': return 5;
        case 'X': return 10;
        case 'L': return 50;
        case 'C': return 100;
        default: return 0;
        }
    };
    int total = 0;
    for (size_t i = 0; i < numeral.size(); ++i)
    {
        const int here = value(numeral[i]);
        if (here == 0)
        {
            return 0;
        }
        const int next = i + 1 < numeral.size() ? value(numeral[i + 1]) : 0;
        total += here < next ? -here : here;
    }
    return total;
}

SiteConditions ConditionsFor(uint32_t seed, float fogEnd)
{
    SiteConditions conditions;
    conditions.temperature = -(18 + static_cast<int>(seed % 23u));
    conditions.wind = 4 + static_cast<int>((seed / 3u) % 19u);
    conditions.visibility = fogEnd < 45.0f ? "POOR" : fogEnd < 70.0f ? "LOW" : "FAIR";
    return conditions;
}

const std::vector<std::string>& IntercomLines::Moments()
{
    static const std::vector<std::string> kMoments{"arrival",       "arrival_no_map", "power_out",     "download_started", "download_done",
                                                   "launch",        "recovered",      "not_recovered", "left_behind", "orders"};
    return kMoments;
}

bool IntercomLines::LoadFromFile(const std::filesystem::path& file, std::string* error)
{
    nlohmann::json json;
    if (!Parse(file, json, error))
    {
        return false;
    }
    m_lines.clear();
    if (!json.contains("lines") || !json["lines"].is_object())
    {
        return true;
    }
    for (const auto& [moment, lines] : json["lines"].items())
    {
        if (!lines.is_array())
        {
            continue;
        }
        for (const nlohmann::json& entry : lines)
        {
            if (!entry.is_object())
            {
                continue;
            }
            IntercomLine line;
            line.sound = entry.value("sound", std::string());
            line.subtitle = entry.value("subtitle", std::string());
            line.seconds = entry.value("seconds", 0.0f);
            if (!line.sound.empty() || !line.subtitle.empty())
            {
                m_lines[moment].push_back(std::move(line));
            }
        }
    }
    return true;
}

const IntercomLine* IntercomLines::Pick(const std::string& moment, uint32_t seed) const
{
    const auto found = m_lines.find(moment);
    if (found == m_lines.end() || found->second.empty())
    {
        return nullptr;
    }
    // FNV-1a of the moment rather than std::hash, which is not the same on every machine.
    uint64_t hash = 1469598103934665603ull;
    for (const char c : moment)
    {
        hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    }
    Random random((static_cast<uint64_t>(seed) << 16) ^ hash);
    return &found->second[static_cast<size_t>(random.Next() % found->second.size())];
}

size_t IntercomLines::Count() const
{
    size_t count = 0;
    for (const auto& [moment, lines] : m_lines)
    {
        count += lines.size();
    }
    return count;
}

float IntercomLines::SecondsFor(const IntercomLine& line)
{
    if (line.seconds > 0.0f)
    {
        return line.seconds;
    }
    // Long enough to read: a second and a half, and a little more a letter.
    return std::clamp(1.5f + 0.06f * static_cast<float>(line.subtitle.size()), 2.5f, 10.0f);
}

} // namespace pred
