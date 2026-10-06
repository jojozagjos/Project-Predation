#include "Game/Campaign/Universe.h"

#include "Engine/Core/Log.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <glm/mat4x4.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace pred
{

namespace
{

constexpr float kTau = 6.28318530718f;
// One astronomical unit's year, in seconds of the campaign's clock: slow enough that a planet is where it was when
// you left it, fast enough that it has moved on when you come back.
constexpr float kYearSeconds = 3600.0f;
// An Earth radius in astronomical units, for where a moon is.
constexpr float kEarthRadiusAu = 4.26e-5f;

glm::vec3 ReadVec3(const nlohmann::json& object, const char* key, glm::vec3 fallback)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array() || it->size() < 3)
    {
        return fallback;
    }
    return {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}

glm::vec2 ReadVec2(const nlohmann::json& object, const char* key, glm::vec2 fallback)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array() || it->size() < 2)
    {
        return fallback;
    }
    return {(*it)[0].get<float>(), (*it)[1].get<float>()};
}

std::map<std::string, float> ReadWeights(const nlohmann::json& object, const char* key)
{
    std::map<std::string, float> out;
    const auto it = object.find(key);
    if (it != object.end() && it->is_object())
    {
        for (const auto& [id, weight] : it->items())
        {
            if (weight.is_number())
            {
                out[id] = weight.get<float>();
            }
        }
    }
    return out;
}

std::vector<std::string> ReadStrings(const nlohmann::json& object, const char* key)
{
    std::vector<std::string> out;
    const auto it = object.find(key);
    if (it != object.end() && it->is_array())
    {
        for (const auto& value : *it)
        {
            if (value.is_string())
            {
                out.push_back(value.get<std::string>());
            }
        }
    }
    return out;
}

void ReadEntry(const nlohmann::json& object, TraitEntry& entry)
{
    entry.id = object.value("id", std::string());
    entry.name = object.value("name", entry.id);
    entry.weight = object.value("weight", 1.0f);
}

template <typename T>
const T* FindById(const std::vector<T>& list, const std::string& id)
{
    for (const T& entry : list)
    {
        if (entry.id == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

// One of `list`, by weight; null when there is nothing with any.
template <typename T>
const T* PickWeighted(const std::vector<const T*>& list, UniverseRandom& random)
{
    float total = 0.0f;
    for (const T* entry : list)
    {
        total += std::max(entry->weight, 0.0f);
    }
    if (total <= 0.0f)
    {
        return list.empty() ? nullptr : list.front();
    }
    float pick = random.Unit() * total;
    for (const T* entry : list)
    {
        pick -= std::max(entry->weight, 0.0f);
        if (pick < 0.0f)
        {
            return entry;
        }
    }
    return list.back();
}

// One id of a weighted list; `fallback` when it is empty or names nothing in `table`.
template <typename T>
std::string PickFrom(const std::map<std::string, float>& weights, const std::vector<T>& table, UniverseRandom& random,
                     const std::string& fallback)
{
    float total = 0.0f;
    for (const auto& [id, weight] : weights)
    {
        if (FindById(table, id) != nullptr)
        {
            total += std::max(weight, 0.0f);
        }
    }
    if (total <= 0.0f)
    {
        // Nothing listed: any entry of the table, by its own weight.
        std::vector<const T*> all;
        for (const T& entry : table)
        {
            all.push_back(&entry);
        }
        const T* any = PickWeighted(all, random);
        return any != nullptr ? any->id : fallback;
    }
    float pick = random.Unit() * total;
    for (const auto& [id, weight] : weights)
    {
        if (FindById(table, id) == nullptr)
        {
            continue;
        }
        pick -= std::max(weight, 0.0f);
        if (pick < 0.0f)
        {
            return id;
        }
    }
    return fallback;
}

glm::vec3 Jitter(glm::vec3 colour, UniverseRandom& random, float amount)
{
    return glm::clamp(colour * random.Range(1.0f - amount, 1.0f + amount) +
                          glm::vec3(random.Range(-amount, amount), random.Range(-amount, amount), random.Range(-amount, amount)) * 0.25f,
                      glm::vec3(0.0f), glm::vec3(1.0f));
}

// Whether a biome can be a world with no air at all: one that lists no airs, or lists none among them.
bool CanBeAirless(const BiomeDef& biome)
{
    return biome.atmospheres.empty() || biome.atmospheres.count("none") != 0;
}

// Which biome a body at this warmth is, of the gas giants or of the rest; of those that can go without air, when it
// is too small to hold any.
const BiomeDef* PickBiome(const UniverseData& data, float temperature, bool gas, bool airless, UniverseRandom& random)
{
    std::vector<const BiomeDef*> fits;
    for (const BiomeDef& biome : data.biomes)
    {
        if (airless && !CanBeAirless(biome))
        {
            continue;
        }
        if (biome.gas == gas && temperature >= biome.temperature.x && temperature <= biome.temperature.y)
        {
            fits.push_back(&biome);
        }
    }
    if (fits.empty())
    {
        // Nothing listed for so warm or so cold: whichever comes nearest.
        const BiomeDef* nearest = nullptr;
        float best = 1.0e9f;
        for (const BiomeDef& biome : data.biomes)
        {
            if (biome.gas != gas || (airless && !CanBeAirless(biome)))
            {
                continue;
            }
            const float off = temperature < biome.temperature.x ? biome.temperature.x - temperature : temperature - biome.temperature.y;
            if (off < best)
            {
                best = off;
                nearest = &biome;
            }
        }
        return nearest;
    }
    return PickWeighted(fits, random);
}

std::string TwoFigures(int value)
{
    return value < 10 ? "0" + std::to_string(value) : std::to_string(value);
}

// The kind of thing a planet is made of, its look and its places, from its seed: for planets and moons alike.
void FillBody(Body& body, const UniverseData& data, bool gasAllowed, float gasChance)
{
    UniverseRandom random(body.seed);
    const bool gas = gasAllowed && random.Chance(gasChance);
    body.radius = gas ? random.Range(3.5f, 11.0f) : (body.kind == BodyKind::Moon ? random.Range(0.12f, 0.5f) : random.Range(0.3f, 1.8f));
    // Too small to hold air: only the kinds of world that can be airless.
    const bool airless = !gas && body.radius < 0.25f;
    const BiomeDef* biome = PickBiome(data, body.temperature, gas, airless, random);
    if (biome == nullptr)
    {
        biome = PickBiome(data, body.temperature, !gas, false, random);
    }
    if (biome == nullptr)
    {
        return;
    }
    body.biome = biome->id;
    body.gas = biome->gas;
    if (body.gas != gas)
    {
        body.radius = body.gas ? random.Range(3.5f, 11.0f) : random.Range(0.3f, 1.8f);
    }
    // Long enough that an evening at a hub lasts a while: twenty-five minutes to an hour and a quarter from noon to noon.
    body.day = random.Range(1500.0f, 4500.0f);
    body.tilt = random.Range(0.0f, 0.45f);

    if (!body.gas)
    {
        body.atmosphere = PickFrom(biome->atmospheres, data.atmospheres, random, "none");
        if (airless && data.Atmosphere("none") != nullptr)
        {
            body.atmosphere = "none";
        }
        body.terrain = PickFrom(biome->terrains, data.terrains, random, data.terrains.empty() ? std::string() : data.terrains.front().id);
        body.weather = PickFrom(biome->weathers, data.weathers, random, "clear");
        if (const AtmosphereDef* air = data.Atmosphere(body.atmosphere); air != nullptr)
        {
            body.temperature += air->warming;
            body.air = air->thickness;
            body.airColor = air->color;
            // No air, no weather but the clear kind.
            if (air->thickness <= 0.0f && data.Weather("clear") != nullptr)
            {
                body.weather = "clear";
            }
        }
        // Settled or not: only where people could plausibly be, which is anywhere with ground.
        std::vector<const CivilizationDef*> civs;
        for (const CivilizationDef& civ : data.civilizations)
        {
            // Nobody lives on a moon or a world on fire; the Company only keeps services where people live.
            if ((civ.inhabited || civ.services) && (body.kind == BodyKind::Moon || body.temperature > 120.0f || body.temperature < -150.0f))
            {
                continue;
            }
            if (civ.services)
            {
                continue; // service worlds are placed, not rolled
            }
            civs.push_back(&civ);
        }
        const CivilizationDef* civ = PickWeighted(civs, random);
        body.civilization = civ != nullptr ? civ->id : std::string();
    }
    else
    {
        body.atmosphere.clear();
        body.weather.clear();
        body.civilization.clear();
        body.air = 0.35f;
        body.airColor = glm::mix(biome->groundA, glm::vec3(0.6f, 0.75f, 1.0f), 0.5f);
    }

    for (const SpecialDef& special : data.specials)
    {
        if (!special.biomes.empty() && std::find(special.biomes.begin(), special.biomes.end(), body.biome) == special.biomes.end())
        {
            continue;
        }
        if ((special.needsAir && body.air <= 0.0f) || (!special.onMoons && body.kind == BodyKind::Moon))
        {
            continue;
        }
        const float chance = special.id == "rings" && body.gas ? special.chance * 4.0f : special.chance;
        if (random.Chance(chance))
        {
            body.specials.push_back(special.id);
        }
    }

    body.groundA = Jitter(biome->groundA, random, 0.08f);
    body.groundB = Jitter(biome->groundB, random, 0.08f);
    body.ocean = Jitter(biome->ocean, random, 0.05f);
    body.oceanAmount = random.Range(biome->oceanAmount.x, biome->oceanAmount.y);
    body.ice = random.Range(biome->ice.x, biome->ice.y);
    body.clouds = random.Range(biome->clouds.x, biome->clouds.y) * std::min(body.air * 1.6f + (body.gas ? 1.0f : 0.0f), 1.0f);
    body.cloudColor = biome->cloudColor;
    if (body.HasSpecial("rings"))
    {
        body.rings = {random.Range(1.3f, 1.7f), random.Range(1.9f, 2.6f)};
        body.ringColor = Jitter(glm::mix(body.groundA, glm::vec3(0.75f, 0.7f, 0.62f), 0.6f), random, 0.08f);
    }
}

// Its places to land, from what kind of world it is.
void PlaceRegions(Body& body, const UniverseData& data, int count)
{
    if (!body.Landable() || count <= 0)
    {
        return;
    }
    UniverseRandom random(MixSeed(body.seed, 0x5245474Eu)); // 'REGN'
    const CivilizationDef* civ = data.Civilization(body.civilization);
    std::vector<std::string> areas;
    if (const auto it = data.regionAreas.find(body.biome); it != data.regionAreas.end())
    {
        areas = it->second;
    }
    if (areas.empty())
    {
        if (const auto it = data.regionAreas.find("any"); it != data.regionAreas.end())
        {
            areas = it->second;
        }
    }
    std::map<std::string, int> numbers;
    for (int i = 0; i < count; ++i)
    {
        LandingRegion region;
        region.seed = static_cast<uint32_t>(MixSeed(body.seed, 1000u + static_cast<uint64_t>(i)) & 0xFFFFFFFFu);
        if (region.seed == 0)
        {
            region.seed = 1;
        }
        static const std::map<std::string, float> kNothing;
        region.kind = PickFrom(civ != nullptr ? civ->regions : kNothing, data.regionKinds, random,
                               data.regionKinds.empty() ? std::string() : data.regionKinds.front().id);
        const RegionKindDef* kind = data.RegionKind(region.kind);
        const std::string designation = kind != nullptr && !kind->designations.empty()
                                            ? kind->designations[static_cast<size_t>(random.Int(0, static_cast<int>(kind->designations.size()) - 1))]
                                            : std::string("SITE");
        const std::string area = areas.empty() ? std::string() : areas[static_cast<size_t>(random.Int(0, static_cast<int>(areas.size()) - 1))];
        const int number = random.Int(1, 24) + numbers[designation]++ * 24;
        region.designation = designation + " " + TwoFigures(number % 100) + (area.empty() ? std::string() : ", " + area);
        region.latLon = {random.Range(-65.0f, 65.0f), random.Range(-180.0f, 180.0f)};
        // Polar names go near the poles.
        if (area.find("NORTH") != std::string::npos)
        {
            region.latLon.x = random.Range(55.0f, 78.0f);
        }
        else if (area.find("SOUTH") != std::string::npos)
        {
            region.latLon.x = random.Range(-78.0f, -55.0f);
        }
        region.charted = kind != nullptr && kind->charted;
        body.regions.push_back(region);
    }
    // Somewhere the records cover is on the charts, even if nobody lives there.
    if (civ != nullptr && civ->records && !body.regions.empty())
    {
        body.regions.front().charted = true;
    }
}

} // namespace

// --- Randomness ------------------------------------------------------------------------------------------------------

uint64_t UniverseRandom::Next()
{
    uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float UniverseRandom::Unit()
{
    return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24);
}

int UniverseRandom::Int(int low, int high)
{
    if (high <= low)
    {
        return low;
    }
    const uint64_t span = static_cast<uint64_t>(high - low) + 1;
    return low + static_cast<int>(Next() % span);
}

uint64_t MixSeed(uint64_t a, uint64_t b)
{
    UniverseRandom random(a ^ (b * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull));
    random.Next();
    return random.Next();
}

std::string RomanNumeral(int value)
{
    static const std::pair<int, const char*> kParts[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"},
                                                         {40, "XL"},  {10, "X"},   {9, "IX"},   {5, "V"},    {4, "IV"},  {1, "I"}};
    std::string out;
    for (const auto& [amount, letters] : kParts)
    {
        while (value >= amount)
        {
            out += letters;
            value -= amount;
        }
    }
    return out;
}

// --- The data --------------------------------------------------------------------------------------------------------

const BiomeDef* UniverseData::Biome(const std::string& id) const { return FindById(biomes, id); }
const AtmosphereDef* UniverseData::Atmosphere(const std::string& id) const { return FindById(atmospheres, id); }
const TerrainDef* UniverseData::Terrain(const std::string& id) const { return FindById(terrains, id); }
const WeatherDef* UniverseData::Weather(const std::string& id) const { return FindById(weathers, id); }
const CivilizationDef* UniverseData::Civilization(const std::string& id) const { return FindById(civilizations, id); }
const SpecialDef* UniverseData::Special(const std::string& id) const { return FindById(specials, id); }
const RegionKindDef* UniverseData::RegionKind(const std::string& id) const { return FindById(regionKinds, id); }

bool UniverseData::LoadFromFile(const std::filesystem::path& file, std::string* error)
{
    std::ifstream stream(file);
    if (!stream)
    {
        if (error != nullptr)
        {
            *error = "could not open " + file.string();
        }
        return false;
    }
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(stream, nullptr, true, true);
    }
    catch (const std::exception& exception)
    {
        if (error != nullptr)
        {
            *error = exception.what();
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

    UniverseData loaded;
    try
    {
        if (const auto names = root.find("names"); names != root.end() && names->is_object())
        {
            const std::vector<std::string> listed = ReadStrings(*names, "catalogues");
            if (!listed.empty())
            {
                loaded.catalogues = listed;
            }
            const glm::vec2 numbers = ReadVec2(*names, "numbers", {10.0f, 999.0f});
            loaded.catalogueNumbers = {static_cast<int>(numbers.x), static_cast<int>(numbers.y)};
            const std::vector<std::string> moons = ReadStrings(*names, "moons");
            if (!moons.empty())
            {
                loaded.moonCatalogues = moons;
            }
            const glm::vec2 moonRange = ReadVec2(*names, "moonNumbers", {100.0f, 999.0f});
            loaded.moonNumbers = {static_cast<int>(moonRange.x), static_cast<int>(moonRange.y)};
            loaded.homeHubName = names->value("homeHub", loaded.homeHubName);
        }
        for (const auto& entry : root.value("stars", nlohmann::json::array()))
        {
            StarDef star;
            ReadEntry(entry, star);
            star.color = ReadVec3(entry, "color", star.color);
            star.luminosity = ReadVec2(entry, "luminosity", star.luminosity);
            star.radius = entry.value("radius", star.radius);
            const glm::vec2 planets = ReadVec2(entry, "planets", {3.0f, 7.0f});
            star.planets = {static_cast<int>(planets.x), static_cast<int>(planets.y)};
            loaded.stars.push_back(star);
        }
        for (const auto& entry : root.value("biomes", nlohmann::json::array()))
        {
            BiomeDef biome;
            ReadEntry(entry, biome);
            biome.temperature = ReadVec2(entry, "temperature", biome.temperature);
            biome.atmospheres = ReadWeights(entry, "atmospheres");
            biome.terrains = ReadWeights(entry, "terrains");
            biome.weathers = ReadWeights(entry, "weathers");
            if (const auto ground = entry.find("ground"); ground != entry.end() && ground->is_array() && ground->size() >= 2)
            {
                const nlohmann::json pair = {{"a", (*ground)[0]}, {"b", (*ground)[1]}};
                biome.groundA = ReadVec3(pair, "a", biome.groundA);
                biome.groundB = ReadVec3(pair, "b", biome.groundB);
            }
            biome.ocean = ReadVec3(entry, "ocean", biome.ocean);
            biome.oceanAmount = ReadVec2(entry, "oceanAmount", biome.oceanAmount);
            biome.ice = ReadVec2(entry, "ice", biome.ice);
            biome.clouds = ReadVec2(entry, "clouds", biome.clouds);
            biome.cloudColor = ReadVec3(entry, "cloudColor", biome.cloudColor);
            biome.gas = entry.value("gas", false);
            if (const auto site = entry.find("site"); site != entry.end() && site->is_object())
            {
                biome.surface = site->value("surface", std::string());
                biome.siteGround = ReadVec3(*site, "ground", biome.siteGround);
                biome.siteRock = ReadVec3(*site, "rock", biome.siteRock);
                biome.siteFog = ReadVec3(*site, "fog", biome.siteFog);
            }
            loaded.biomes.push_back(biome);
        }
        for (const auto& entry : root.value("atmospheres", nlohmann::json::array()))
        {
            AtmosphereDef air;
            ReadEntry(entry, air);
            air.thickness = entry.value("thickness", air.thickness);
            air.color = ReadVec3(entry, "color", air.color);
            air.visibility = entry.value("visibility", air.visibility);
            air.warming = entry.value("warming", air.warming);
            air.breathable = entry.value("breathable", air.breathable);
            loaded.atmospheres.push_back(air);
        }
        for (const auto& entry : root.value("terrains", nlohmann::json::array()))
        {
            TerrainDef terrain;
            ReadEntry(entry, terrain);
            terrain.roughness = entry.value("roughness", terrain.roughness);
            loaded.terrains.push_back(terrain);
        }
        for (const auto& entry : root.value("weathers", nlohmann::json::array()))
        {
            WeatherDef weather;
            ReadEntry(entry, weather);
            weather.particles = entry.value("particles", weather.particles);
            weather.intensity = entry.value("intensity", weather.intensity);
            weather.visibility = entry.value("visibility", weather.visibility);
            weather.wind = ReadVec2(entry, "wind", weather.wind);
            loaded.weathers.push_back(weather);
        }
        for (const auto& entry : root.value("civilizations", nlohmann::json::array()))
        {
            CivilizationDef civ;
            ReadEntry(entry, civ);
            civ.inhabited = entry.value("inhabited", false);
            civ.records = entry.value("records", false);
            civ.services = entry.value("services", false);
            civ.regions = ReadWeights(entry, "regions");
            loaded.civilizations.push_back(civ);
        }
        for (const auto& entry : root.value("specials", nlohmann::json::array()))
        {
            SpecialDef special;
            ReadEntry(entry, special);
            special.chance = entry.value("chance", special.chance);
            special.biomes = ReadStrings(entry, "biomes");
            special.needsAir = entry.value("needsAir", false);
            special.onMoons = entry.value("onMoons", true);
            loaded.specials.push_back(special);
        }
        for (const auto& entry : root.value("regions", nlohmann::json::array()))
        {
            RegionKindDef kind;
            ReadEntry(entry, kind);
            kind.charted = entry.value("charted", false);
            kind.ship = entry.value("ship", false);
            kind.designations = ReadStrings(entry, "designations");
            loaded.regionKinds.push_back(kind);
        }
        if (const auto areas = root.find("areas"); areas != root.end() && areas->is_object())
        {
            for (const auto& [biome, list] : areas->items())
            {
                loaded.regionAreas[biome] = ReadStrings(*areas, biome.c_str());
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
    if (loaded.biomes.empty() || loaded.stars.empty())
    {
        if (error != nullptr)
        {
            *error = "no biomes or no stars";
        }
        return false;
    }
    *this = std::move(loaded);
    return true;
}

void UniverseData::UseDefaults()
{
    *this = UniverseData{};
    StarDef star;
    star.id = "g_dwarf";
    star.name = "Yellow dwarf";
    star.planets = {4, 7};
    stars.push_back(star);

    BiomeDef frozen;
    frozen.id = "frozen";
    frozen.name = "Frozen";
    frozen.temperature = {-273.0f, -5.0f};
    frozen.groundA = {0.86f, 0.89f, 0.93f};
    frozen.groundB = {0.56f, 0.62f, 0.7f};
    frozen.ice = {0.5f, 1.0f};
    frozen.surface = "snow_02";
    biomes.push_back(frozen);
    BiomeDef rocky;
    rocky.id = "rocky";
    rocky.name = "Rocky";
    rocky.temperature = {-273.0f, 2000.0f};
    rocky.weight = 0.5f;
    biomes.push_back(rocky);
    BiomeDef temperate;
    temperate.id = "temperate";
    temperate.name = "Temperate";
    temperate.temperature = {-5.0f, 40.0f};
    temperate.oceanAmount = {0.4f, 0.7f};
    biomes.push_back(temperate);
    BiomeDef gasGiant;
    gasGiant.id = "gas";
    gasGiant.name = "Gas giant";
    gasGiant.gas = true;
    biomes.push_back(gasGiant);

    AtmosphereDef none;
    none.id = "none";
    none.thickness = 0.0f;
    atmospheres.push_back(none);
    AtmosphereDef thin;
    thin.id = "thin";
    thin.thickness = 0.3f;
    atmospheres.push_back(thin);
    TerrainDef flat;
    flat.id = "flat";
    terrains.push_back(flat);
    WeatherDef clear;
    clear.id = "clear";
    weathers.push_back(clear);
    WeatherDef snow;
    snow.id = "snow";
    snow.particles = "snow";
    snow.intensity = 0.5f;
    weathers.push_back(snow);

    CivilizationDef nobody;
    nobody.id = "none";
    nobody.weight = 5.0f;
    nobody.regions = {{"survey", 1.0f}};
    civilizations.push_back(nobody);
    CivilizationDef research;
    research.id = "abandoned_research";
    research.records = true;
    research.regions = {{"research_facility", 1.0f}};
    civilizations.push_back(research);
    CivilizationDef colony;
    colony.id = "colonized";
    colony.inhabited = true;
    colony.records = true;
    colony.services = true;
    colony.regions = {{"survey", 1.0f}};
    civilizations.push_back(colony);

    RegionKindDef facility;
    facility.id = "research_facility";
    facility.designations = {"RESEARCH FACILITY"};
    regionKinds.push_back(facility);
    RegionKindDef survey;
    survey.id = "survey";
    survey.designations = {"SURVEY SITE"};
    regionKinds.push_back(survey);
}

// --- Places ----------------------------------------------------------------------------------------------------------

uint64_t SystemId::Packed() const
{
    return static_cast<uint64_t>(static_cast<uint16_t>(x)) | (static_cast<uint64_t>(static_cast<uint16_t>(z)) << 16) |
           (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 32) | (static_cast<uint64_t>(index) << 48);
}

SystemId SystemId::Unpack(uint64_t packed)
{
    SystemId id;
    id.x = static_cast<int16_t>(static_cast<uint16_t>(packed & 0xFFFFu));
    id.z = static_cast<int16_t>(static_cast<uint16_t>((packed >> 16) & 0xFFFFu));
    id.y = static_cast<int16_t>(static_cast<uint16_t>((packed >> 32) & 0xFFFFu));
    id.index = static_cast<uint8_t>((packed >> 48) & 0xFFu);
    return id;
}

bool Body::HasSpecial(const std::string& id) const
{
    return std::find(specials.begin(), specials.end(), id) != specials.end();
}

glm::vec3 StarSystem::Position(int index, double time) const
{
    const Body* body = Find(index);
    if (body == nullptr)
    {
        return glm::vec3(0.0f);
    }
    const double turns = time / static_cast<double>(std::max(body->period, 1.0f));
    const float angle = body->phase + static_cast<float>(std::fmod(turns, 1.0) * static_cast<double>(kTau));
    if (body->kind == BodyKind::Moon)
    {
        const Body* planet = Find(body->parent);
        const glm::vec3 around = Position(body->parent, time);
        const float reach = body->orbit * (planet != nullptr ? planet->radius : 1.0f) * kEarthRadiusAu;
        return around + glm::vec3(std::cos(angle) * reach, std::sin(angle) * reach * body->inclination, std::sin(angle) * reach);
    }
    return {std::cos(angle) * body->orbit, std::sin(angle) * body->orbit * body->inclination, std::sin(angle) * body->orbit};
}

glm::vec3 StarSystem::SunOver(int index, double time, float* spin) const
{
    const Body* body = Find(index);
    if (body == nullptr)
    {
        return {0.0f, 1.0f, 0.0f};
    }
    const glm::vec3 at = Position(index, time);
    const glm::vec3 towardsStar = glm::length(at) > 1.0e-6f ? -glm::normalize(at) : glm::vec3(1.0f, 0.0f, 0.0f);
    // Into the body's frame: its axis tipped by its tilt, then turned about it -- by however far round the star is (so
    // going round the star does not slow or quicken the day, nor stop it when the day is as long as the year), and on by
    // the day's own turn.
    const glm::mat4 untilt = glm::rotate(glm::mat4(1.0f), -body->tilt, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 tipped = glm::vec3(untilt * glm::vec4(towardsStar, 0.0f));
    const float bearing = std::atan2(-tipped.z, tipped.x);
    const float turned = bearing + static_cast<float>(std::fmod(time / std::max(static_cast<double>(body->day), 1.0), 1.0)) * kTau;
    if (spin != nullptr)
    {
        *spin = turned;
    }
    return glm::vec3(glm::rotate(glm::mat4(1.0f), -turned, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::vec4(tipped, 0.0f));
}

float StarSystem::SunHeight(int index, const glm::vec2& latLon, double time) const
{
    if (Find(index) == nullptr)
    {
        return 0.0f;
    }
    const glm::vec3 sun = SunOver(index, time);
    const float lat = glm::radians(latLon.x);
    const float lon = glm::radians(latLon.y);
    const glm::vec3 up{std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)};
    return glm::dot(up, sun);
}

// --- The universe ----------------------------------------------------------------------------------------------------

void Universe::Reset(uint64_t seed, const UniverseData* data)
{
    m_seed = seed;
    m_data = data;
    m_systems.clear();
}

int Universe::SystemsInCell(int x, int y, int z) const
{
    if (x == 0 && y == 0 && z == 0)
    {
        return 1; // home is always there
    }
    // A thin disc: only a few cells deep.
    if (std::abs(y) > 2)
    {
        return 0;
    }
    UniverseRandom random(MixSeed(m_seed, SystemId{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), 0xFF}.Packed()));
    const float roll = random.Unit() * (std::abs(y) == 2 ? 2.5f : 1.0f);
    return roll < 0.35f ? 0 : roll < 0.85f ? 1 : 2;
}

glm::vec3 Universe::SystemPosition(SystemId id) const
{
    if (id == SystemId{})
    {
        return glm::vec3(0.0f);
    }
    UniverseRandom random(MixSeed(m_seed, id.Packed() ^ 0x504F53ull)); // 'POS'
    const glm::vec3 cell{static_cast<float>(id.x), static_cast<float>(id.y), static_cast<float>(id.z)};
    return (cell + glm::vec3(random.Range(0.1f, 0.9f), random.Range(0.1f, 0.9f), random.Range(0.1f, 0.9f)) - glm::vec3(0.5f)) * kCellSize;
}

std::vector<SystemId> Universe::Near(const glm::vec3& at, float lightYears)
{
    std::vector<std::pair<float, SystemId>> found;
    const int reach = static_cast<int>(std::ceil(lightYears / kCellSize)) + 1;
    const glm::ivec3 centre{static_cast<int>(std::round(at.x / kCellSize)), static_cast<int>(std::round(at.y / kCellSize)),
                            static_cast<int>(std::round(at.z / kCellSize))};
    for (int x = centre.x - reach; x <= centre.x + reach; ++x)
    {
        for (int y = std::max(centre.y - reach, -2); y <= std::min(centre.y + reach, 2); ++y)
        {
            for (int z = centre.z - reach; z <= centre.z + reach; ++z)
            {
                const int count = SystemsInCell(x, y, z);
                for (int i = 0; i < count; ++i)
                {
                    const SystemId id{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), static_cast<uint8_t>(i)};
                    const float distance = glm::length(SystemPosition(id) - at);
                    if (distance <= lightYears)
                    {
                        found.emplace_back(distance, id);
                    }
                }
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first || (a.first == b.first && a.second < b.second); });
    std::vector<SystemId> out;
    for (const auto& [distance, id] : found)
    {
        out.push_back(id);
    }
    return out;
}

const StarSystem* Universe::System(SystemId id)
{
    if (m_data == nullptr)
    {
        return nullptr;
    }
    const uint64_t key = id.Packed();
    if (const auto found = m_systems.find(key); found != m_systems.end())
    {
        return found->second.get();
    }
    const int count = SystemsInCell(id.x, id.y, id.z);
    if (static_cast<int>(id.index) >= count)
    {
        return nullptr;
    }
    auto system = std::make_unique<StarSystem>(Generate(m_seed, id, *m_data));
    system->galaxy = SystemPosition(id);
    const StarSystem* made = system.get();
    m_systems.emplace(key, std::move(system));
    return made;
}

namespace
{

// A system's name and star, the first of what its seed makes: the same whether the whole system is being made or only
// glanced at. Null when there are no stars to have.
const StarDef* BeginSystem(uint64_t universeSeed, SystemId id, const UniverseData& data, StarSystem& system, UniverseRandom& random)
{
    system.id = id;
    const bool home = id == SystemId{};
    const std::string catalogue = data.catalogues.empty() ? std::string("KEPLER")
                                                          : data.catalogues[static_cast<size_t>(random.Int(0, static_cast<int>(data.catalogues.size()) - 1))];
    system.name = catalogue + "-" + std::to_string(random.Int(data.catalogueNumbers.x, data.catalogueNumbers.y));
    // The star. Home is a calm, sunlike one, so where a campaign starts is somewhere people would settle.
    std::vector<const StarDef*> stars;
    for (const StarDef& star : data.stars)
    {
        stars.push_back(&star);
    }
    const StarDef* star = home && FindById(data.stars, "g_dwarf") != nullptr ? FindById(data.stars, "g_dwarf") : PickWeighted(stars, random);
    if (star == nullptr)
    {
        return nullptr;
    }
    system.star = star->id;
    system.starColor = star->color;
    system.luminosity = random.Range(star->luminosity.x, star->luminosity.y);
    system.starRadius = star->radius;
    (void)universeSeed;
    return star;
}

} // namespace

const SystemGlance& Universe::Glance(SystemId id)
{
    const uint64_t key = id.Packed();
    if (const auto found = m_glances.find(key); found != m_glances.end())
    {
        return found->second;
    }
    SystemGlance glance;
    glance.id = id;
    glance.position = SystemPosition(id);
    if (m_data != nullptr)
    {
        StarSystem system;
        system.seed = MixSeed(m_seed, id.Packed() + 1);
        UniverseRandom random(system.seed);
        BeginSystem(m_seed, id, *m_data, system, random);
        glance.name = system.name;
        glance.star = system.star;
        glance.starColor = system.starColor;
        glance.luminosity = system.luminosity;
    }
    return m_glances.emplace(key, glance).first->second;
}

StarSystem Universe::Generate(uint64_t universeSeed, SystemId id, const UniverseData& data)
{
    StarSystem system;
    system.seed = MixSeed(universeSeed, id.Packed() + 1);
    UniverseRandom random(system.seed);
    const bool home = id == SystemId{};
    const StarDef* star = BeginSystem(universeSeed, id, data, system, random);
    if (star == nullptr)
    {
        return system;
    }
    const float warmth = std::sqrt(std::max(system.luminosity, 0.001f));
    int planets = random.Int(star->planets.x, star->planets.y);
    if (home)
    {
        planets = std::max(planets, 5);
    }

    // Planets outwards, each a little further than the one before by about half again; past where water freezes, gas
    // giants are likely.
    float orbit = random.Range(0.28f, 0.5f) * warmth;
    const float snowLine = 2.7f * warmth;
    for (int i = 0; i < planets; ++i)
    {
        Body planet;
        planet.index = static_cast<uint16_t>(system.bodies.size());
        planet.kind = BodyKind::Planet;
        planet.name = system.name + " " + RomanNumeral(i + 1);
        planet.seed = MixSeed(system.seed, static_cast<uint64_t>(i) + 1);
        UniverseRandom own(MixSeed(planet.seed, 0x4F524254ull)); // 'ORBT'
        planet.orbit = orbit;
        orbit *= own.Range(1.45f, 1.95f);
        planet.period = kYearSeconds * std::pow(planet.orbit, 1.5f) / std::max(std::pow(system.luminosity, 0.125f), 0.2f);
        planet.phase = own.Range(0.0f, kTau);
        planet.inclination = own.Range(-0.05f, 0.05f);
        // How warm the ground is, from how much light reaches it (before any air), with a little of its own.
        planet.temperature = 278.0f * std::pow(system.luminosity, 0.25f) / std::sqrt(planet.orbit) - 273.0f + own.Range(-15.0f, 15.0f);
        FillBody(planet, data, planet.orbit > snowLine, 0.6f);
        const int regions = planet.gas ? 0 : own.Int(2, 4);
        PlaceRegions(planet, data, regions);
        system.bodies.push_back(planet);

        // Its moons: a gas giant has a few, a rocky world one or two at most.
        const int moons = planet.gas ? own.Int(1, 4) : (planet.radius > 0.8f ? own.Int(0, 2) : own.Int(0, 1));
        // Well outside any rings (which lie inside where a moon could hold together), each further than the last.
        float moonOrbit = std::max(own.Range(4.0f, 8.0f), planet.rings.y * 1.8f);
        // How fast the planet's moons go round, by how far out: the further, the slower, as Kepler had it, so an outer moon
        // never laps an inner one.
        const float moonPace = UniverseRandom(MixSeed(planet.seed, 0x50414345ull)).Range(80.0f, 160.0f); // 'PACE'
        const size_t planetIndex = system.bodies.size() - 1;
        for (int m = 0; m < moons; ++m)
        {
            Body moon;
            moon.index = static_cast<uint16_t>(system.bodies.size());
            moon.kind = BodyKind::Moon;
            moon.parent = static_cast<int>(planetIndex);
            moon.seed = MixSeed(system.bodies[planetIndex].seed, 100u + static_cast<uint64_t>(m));
            UniverseRandom moonRandom(MixSeed(moon.seed, 0x4F524254ull));
            // A designation of its own, as a moon in the Alien films has one: a prefix and a number.
            {
                UniverseRandom naming(MixSeed(moon.seed, 0x4E414D45ull)); // 'NAME'
                const std::string prefix = data.moonCatalogues.empty()
                                               ? std::string("LV")
                                               : data.moonCatalogues[static_cast<size_t>(naming.Int(0, static_cast<int>(data.moonCatalogues.size()) - 1))];
                moon.name = prefix + "-" + std::to_string(naming.Int(data.moonNumbers.x, data.moonNumbers.y));
            }
            moon.orbit = moonOrbit;
            moonOrbit *= moonRandom.Range(1.4f, 2.0f);
            moon.period = moonPace * std::pow(moon.orbit / 4.0f, 1.5f);
            moon.phase = moonRandom.Range(0.0f, kTau);
            moon.inclination = moonRandom.Range(-0.1f, 0.1f);
            // As warm as its planet's light makes it, before any air of its own.
            moon.temperature = 278.0f * std::pow(system.luminosity, 0.25f) / std::sqrt(planet.orbit) - 273.0f + moonRandom.Range(-20.0f, 5.0f);
            FillBody(moon, data, false, 0.0f);
            // Never bigger than half its planet: a pair the same size is two planets, not a planet and its moon.
            moon.radius = std::min(moon.radius, std::max(planet.radius * 0.45f, 0.08f));
            PlaceRegions(moon, data, moonRandom.Int(1, 2));
            system.bodies.push_back(moon);
        }
    }

    // A settled world with a hub on it: always at home (the planet nearest pleasant), sometimes elsewhere.
    const CivilizationDef* services = nullptr;
    for (const CivilizationDef& civ : data.civilizations)
    {
        if (civ.services)
        {
            services = &civ;
            break;
        }
    }
    if (services != nullptr && (home || random.Chance(0.25f)))
    {
        int best = -1;
        float bestScore = 1.0e9f;
        for (const Body& body : system.bodies)
        {
            if (body.kind != BodyKind::Planet || !body.Landable())
            {
                continue;
            }
            const float score = std::abs(body.temperature - 12.0f);
            if (score < bestScore)
            {
                bestScore = score;
                best = body.index;
            }
        }
        if (best >= 0)
        {
            Body& world = system.bodies[static_cast<size_t>(best)];
            world.civilization = services->id;
            // Somewhere to breathe, if a world of its kind can have such air at all.
            const BiomeDef* worldBiome = data.Biome(world.biome);
            const bool canBreathe = worldBiome != nullptr && worldBiome->atmospheres.count("breathable") != 0;
            if (canBreathe && data.Atmosphere("breathable") != nullptr && world.temperature > -60.0f && world.temperature < 80.0f && world.radius >= 0.25f)
            {
                world.atmosphere = "breathable";
                world.air = data.Atmosphere("breathable")->thickness;
                world.airColor = data.Atmosphere("breathable")->color;
            }
            for (LandingRegion& region : world.regions)
            {
                region.charted = region.charted || region.kind == "outpost";
            }
            system.hub = best;
            // Its hub: where people are, and where the ship itself sets down -- on the charts, not far from the equator,
            // the last of the world's places so no other place's number changes for having it.
            LandingRegion hub;
            hub.seed = static_cast<uint32_t>(MixSeed(world.seed, 0x48554242ull) & 0xFFFFFFFFu) | 1u; // 'HUBB'
            hub.kind = "hub";
            const RegionKindDef* hubKind = data.RegionKind("hub");
            // Kestrel at home; elsewhere an outpost by its number.
            hub.designation = home ? data.homeHubName
                                   : (hubKind != nullptr && !hubKind->designations.empty() ? hubKind->designations.front() : std::string("OUTPOST")) + " " +
                                         std::to_string(10 + hub.seed % 90u);
            hub.latLon = {random.Range(-20.0f, 20.0f), random.Range(-180.0f, 180.0f)};
            hub.charted = true;
            system.hubRegion = static_cast<int>(world.regions.size());
            world.regions.push_back(hub);
        }
    }
    return system;
}

} // namespace pred
