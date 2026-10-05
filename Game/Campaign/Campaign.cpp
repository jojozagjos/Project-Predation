#include "Game/Campaign/Campaign.h"

#include <algorithm>
#include <set>

namespace pred
{

namespace
{

nlohmann::json Vec3(const glm::vec3& v)
{
    return nlohmann::json::array({v.x, v.y, v.z});
}

glm::vec3 ReadVec3(const nlohmann::json& object, const char* key, glm::vec3 fallback)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array() || it->size() < 3 || !(*it)[0].is_number())
    {
        return fallback;
    }
    return {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}

template <typename T>
std::map<std::string, T> ReadMap(const nlohmann::json& object, const char* key)
{
    std::map<std::string, T> out;
    const auto it = object.find(key);
    if (it != object.end() && it->is_object())
    {
        for (const auto& [name, value] : it->items())
        {
            try
            {
                out[name] = value.get<T>();
            }
            catch (const std::exception&)
            {
                // One bad value is skipped, not the whole save.
            }
        }
    }
    return out;
}

// The keys this version reads; everything else at the top level is kept in `extra`.
const std::set<std::string>& KnownKeys()
{
    static const std::set<std::string> kKeys{"version", "name",     "universeSeed",  "clock", "played",   "credits",     "components",
                                             "upgrades", "colors",  "location",      "travel", "known",   "regionsFound", "regionChanges",
                                             "log",     "nextLogId", "story",        "storyComplete", "cargo"};
    return kKeys;
}

} // namespace

std::string CampaignState::BodyKey(uint64_t system, int body)
{
    return std::to_string(system) + ":" + std::to_string(body);
}

std::string CampaignState::RegionKey(uint64_t system, int body, int region)
{
    return BodyKey(system, body) + ":" + std::to_string(region);
}

uint8_t CampaignState::Known(uint64_t systemId, int bodyIndex) const
{
    const auto it = known.find(BodyKey(systemId, bodyIndex));
    return it == known.end() ? 0 : it->second;
}

bool CampaignState::Learn(uint64_t systemId, int bodyIndex, uint8_t bits)
{
    uint8_t& had = known[BodyKey(systemId, bodyIndex)];
    const uint8_t now = static_cast<uint8_t>(had | bits);
    const bool news = now != had;
    had = now;
    return news;
}

bool CampaignState::RegionFound(uint64_t systemId, int bodyIndex, int regionIndex, bool charted) const
{
    if (charted)
    {
        return true;
    }
    const auto it = regionsFound.find(RegionKey(systemId, bodyIndex, regionIndex));
    return it != regionsFound.end() && it->second;
}

bool CampaignState::FindRegion(uint64_t systemId, int bodyIndex, int regionIndex)
{
    bool& found = regionsFound[RegionKey(systemId, bodyIndex, regionIndex)];
    const bool news = !found;
    found = true;
    return news;
}

int CampaignState::Upgrade(const std::string& category) const
{
    const auto it = upgrades.find(category);
    return it == upgrades.end() ? 0 : it->second;
}

int CampaignState::Components(const std::string& id) const
{
    const auto it = components.find(id);
    return it == components.end() ? 0 : it->second;
}

CampaignState::LogEntry& CampaignState::AddLog(const std::string& kind, const std::string& subject, const std::string& title,
                                               const std::string& text)
{
    for (LogEntry& entry : log)
    {
        if (entry.kind == kind && entry.subject == subject && entry.title == title)
        {
            return entry;
        }
    }
    LogEntry entry;
    entry.id = nextLogId++;
    entry.kind = kind;
    entry.subject = subject;
    entry.title = title;
    entry.text = text;
    entry.time = clock;
    log.push_back(entry);
    return log.back();
}

CampaignState::LogEntry* CampaignState::FindLog(uint32_t id)
{
    for (LogEntry& entry : log)
    {
        if (entry.id == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

nlohmann::json CampaignState::ToJson() const
{
    nlohmann::json out = extra.is_object() ? extra : nlohmann::json::object();
    out["version"] = version;
    out["name"] = name;
    out["universeSeed"] = universeSeed;
    out["clock"] = clock;
    out["played"] = played;
    out["credits"] = credits;
    out["components"] = components;
    out["upgrades"] = upgrades;
    out["colors"] = {{"primary", Vec3(colors.primary)}, {"secondary", Vec3(colors.secondary)}, {"accent", Vec3(colors.accent)}};
    out["location"] = {{"system", system}, {"body", body}, {"region", region}};
    out["travel"] = {{"underway", travel.underway}, {"position", Vec3(travel.position)}, {"velocity", Vec3(travel.velocity)},
                     {"target", travel.target}, {"region", travel.region},
                     {"interstellar", travel.interstellar}, {"toSystem", travel.toSystem}, {"fromGalaxy", Vec3(travel.fromGalaxy)},
                     {"toGalaxy", Vec3(travel.toGalaxy)}, {"departed", travel.departed}, {"duration", travel.duration}};
    nlohmann::json knownJson = nlohmann::json::object();
    for (const auto& [key, bits] : known)
    {
        knownJson[key] = bits;
    }
    out["known"] = knownJson;
    out["regionsFound"] = regionsFound;
    nlohmann::json changes = nlohmann::json::object();
    for (const auto& [key, change] : regionChanges)
    {
        changes[key] = change;
    }
    out["regionChanges"] = changes;
    nlohmann::json entries = nlohmann::json::array();
    for (const LogEntry& entry : log)
    {
        entries.push_back({{"id", entry.id},           {"kind", entry.kind},     {"subject", entry.subject}, {"title", entry.title},
                           {"text", entry.text},       {"time", entry.time},     {"important", entry.important},
                           {"pinned", entry.pinned},   {"links", entry.links}});
    }
    out["log"] = entries;
    out["nextLogId"] = nextLogId;
    out["story"] = story;
    out["storyComplete"] = storyComplete;
    out["cargo"] = cargo;
    return out;
}

bool CampaignState::FromJson(const nlohmann::json& json, CampaignState& out, std::string* error)
{
    if (!json.is_object())
    {
        if (error != nullptr)
        {
            *error = "a campaign is an object";
        }
        return false;
    }
    CampaignState state;
    try
    {
        state.version = json.value("version", kVersion);
        if (state.version > kVersion)
        {
            // Newer than this game: read what it can, keep the rest, and say so.
            if (error != nullptr)
            {
                *error = "made by a newer version (" + std::to_string(state.version) + ")";
            }
        }
        state.name = json.value("name", std::string("Campaign"));
        state.universeSeed = json.value("universeSeed", uint64_t{0});
        state.clock = json.value("clock", 0.0);
        state.played = json.value("played", 0.0);
        state.credits = json.value("credits", int64_t{0});
        state.components = ReadMap<int>(json, "components");
        state.upgrades = ReadMap<int>(json, "upgrades");
        if (const auto colors = json.find("colors"); colors != json.end() && colors->is_object())
        {
            state.colors.primary = ReadVec3(*colors, "primary", state.colors.primary);
            state.colors.secondary = ReadVec3(*colors, "secondary", state.colors.secondary);
            state.colors.accent = ReadVec3(*colors, "accent", state.colors.accent);
        }
        if (const auto location = json.find("location"); location != json.end() && location->is_object())
        {
            state.system = location->value("system", uint64_t{0});
            state.body = location->value("body", -1);
            state.region = location->value("region", -1);
        }
        if (const auto travel = json.find("travel"); travel != json.end() && travel->is_object())
        {
            state.travel.underway = travel->value("underway", false);
            state.travel.position = ReadVec3(*travel, "position", glm::vec3(0.0f));
            state.travel.velocity = ReadVec3(*travel, "velocity", glm::vec3(0.0f));
            state.travel.target = travel->value("target", -1);
            state.travel.region = travel->value("region", -1);
            state.travel.interstellar = travel->value("interstellar", false);
            state.travel.toSystem = travel->value("toSystem", uint64_t{0});
            state.travel.fromGalaxy = ReadVec3(*travel, "fromGalaxy", glm::vec3(0.0f));
            state.travel.toGalaxy = ReadVec3(*travel, "toGalaxy", glm::vec3(0.0f));
            state.travel.departed = travel->value("departed", 0.0);
            state.travel.duration = travel->value("duration", 0.0f);
        }
        for (const auto& [key, bits] : ReadMap<int>(json, "known"))
        {
            state.known[key] = static_cast<uint8_t>(bits);
        }
        state.regionsFound = ReadMap<bool>(json, "regionsFound");
        if (const auto changes = json.find("regionChanges"); changes != json.end() && changes->is_object())
        {
            for (const auto& [key, change] : changes->items())
            {
                state.regionChanges[key] = change;
            }
        }
        if (const auto entries = json.find("log"); entries != json.end() && entries->is_array())
        {
            for (const auto& item : *entries)
            {
                if (!item.is_object())
                {
                    continue;
                }
                LogEntry entry;
                entry.id = item.value("id", 0u);
                entry.kind = item.value("kind", std::string());
                entry.subject = item.value("subject", std::string());
                entry.title = item.value("title", std::string());
                entry.text = item.value("text", std::string());
                entry.time = item.value("time", 0.0);
                entry.important = item.value("important", false);
                entry.pinned = item.value("pinned", false);
                if (const auto links = item.find("links"); links != item.end() && links->is_array())
                {
                    for (const auto& link : *links)
                    {
                        if (link.is_number_unsigned())
                        {
                            entry.links.push_back(link.get<uint32_t>());
                        }
                    }
                }
                state.log.push_back(entry);
            }
        }
        state.nextLogId = json.value("nextLogId", 1u);
        for (const LogEntry& entry : state.log)
        {
            state.nextLogId = std::max(state.nextLogId, entry.id + 1);
        }
        if (const auto story = json.find("story"); story != json.end() && story->is_object())
        {
            state.story = *story;
        }
        state.storyComplete = json.value("storyComplete", false);
        state.cargo = ReadMap<int>(json, "cargo");
        for (const auto& [key, value] : json.items())
        {
            if (KnownKeys().count(key) == 0)
            {
                state.extra[key] = value;
            }
        }
    }
    catch (const std::exception& exception)
    {
        if (error != nullptr)
        {
            *error = exception.what();
        }
        return false;
    }
    out = std::move(state);
    return true;
}

CampaignState CampaignState::Begin(const std::string& campaignName, uint64_t seed, Universe& universe)
{
    CampaignState state;
    state.name = campaignName;
    state.universeSeed = seed;
    state.credits = 1500;
    state.system = universe.Home().Packed();
    const StarSystem* home = universe.System(universe.Home());
    if (home == nullptr)
    {
        return state;
    }
    // Docked at the station over the settled world (or over the world itself, if there were none).
    state.body = home->station >= 0 ? home->station : home->hub;
    state.region = -1;
    state.Learn(home->id.Packed(), -1, kKnownVisited);
    // Every body of the home system is on the charts by name; whatever has records is known by them; home is visited.
    for (const Body& body : home->bodies)
    {
        uint8_t bits = 0;
        if (const CivilizationDef* civ = universe.Data() != nullptr ? universe.Data()->Civilization(body.civilization) : nullptr;
            civ != nullptr && civ->records)
        {
            bits |= kKnownRecords;
        }
        if (body.index == home->hub || body.index == home->station)
        {
            bits |= kKnownRecords | kKnownScanned | kKnownVisited;
        }
        if (bits != 0)
        {
            state.Learn(home->id.Packed(), body.index, bits);
        }
    }
    return state;
}

} // namespace pred
