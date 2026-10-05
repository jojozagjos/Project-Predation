#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pred
{

// The universe a campaign is played in: star systems, their planets and moons, and the places on them a crew can
// land, all made from one seed.
//
// Nothing of it is stored. A system is worked out the first time something asks for it, from the universe's seed and
// where it is, and comes out the same every time and on every machine -- so a save holds the seed and what has been
// found and changed, never the planets themselves, and the host sends the seed, never a world. Only a handful of
// systems are ever looked at in a session, so the few that have been are kept and the rest never exist.
//
// What a planet can be is data, not code (Assets/Data/universe.json): biomes, atmospheres, kinds of ground, weathers,
// how settled it is and anything unusual about it, each a list of entries. A planet is one of each, picked by weight
// among those that suit how far it is from its star -- so a frozen, thin-aired, mountainous world under heavy snow
// with an abandoned research presence is a combination, and adding a biome is adding an entry.

// A small, fully specified random sequence (SplitMix64): the same for a seed on every compiler and machine, which
// the standard library's generators are not.
class UniverseRandom
{
public:
    explicit UniverseRandom(uint64_t seed) : m_state(seed) {}
    uint64_t Next();
    float Unit(); // [0, 1)
    float Range(float low, float high) { return low + (high - low) * Unit(); }
    int Int(int low, int high); // [low, high]
    bool Chance(float p) { return Unit() < p; }

private:
    uint64_t m_state;
};

// Mixes numbers into one well-spread seed: for a system from the universe's seed and its place, a planet from its
// system's, and so on down.
uint64_t MixSeed(uint64_t a, uint64_t b);

// --- What a planet can be (universe.json) --------------------------------------------------------------------------

// Something a planet has one of, picked by weight. `id` is what saves and other entries refer to it by; `name` what a
// person is shown. The rest is what that kind of thing does, per table.
struct TraitEntry
{
    std::string id;
    std::string name;
    float weight = 1.0f;
};

struct BiomeDef : TraitEntry
{
    // How warm the ground is, in degrees Celsius: a planet is only this biome where its warmth falls inside.
    glm::vec2 temperature{-273.0f, 1000.0f};
    // What else goes with it, by weight; ids from the other tables. Empty: any.
    std::map<std::string, float> atmospheres;
    std::map<std::string, float> terrains;
    std::map<std::string, float> weathers;
    // How it looks from space: two colours its ground is mixed from, its seas (how much of it, 0 to 1, and what colour),
    // ice at the poles (0 to 1), cloud (0 to 1) and the cloud's colour.
    glm::vec3 groundA{0.5f};
    glm::vec3 groundB{0.4f};
    glm::vec3 ocean{0.1f, 0.2f, 0.35f};
    glm::vec2 oceanAmount{0.0f, 0.0f};
    glm::vec2 ice{0.0f, 0.0f};
    glm::vec2 clouds{0.0f, 0.3f};
    glm::vec3 cloudColor{0.95f, 0.96f, 1.0f};
    // A gas giant: banded, nothing to stand on.
    bool gas = false;
    // And on the ground, landed: the texture set the ground is laid with (empty for none), its colour, the rock's, the
    // fog's, and what falls from the sky when the weather does not say.
    std::string surface;
    glm::vec3 siteGround{0.6f};
    glm::vec3 siteRock{0.35f};
    glm::vec3 siteFog{0.03f};
};

struct AtmosphereDef : TraitEntry
{
    // How much air there is to see: its glow at the planet's edge from space (0 none, 1 thick), its colour, and how far
    // a person can see in it on the ground, as a multiplier on the fog.
    float thickness = 0.5f;
    glm::vec3 color{0.35f, 0.55f, 0.95f};
    float visibility = 1.0f;
    // Degrees it adds to the ground's warmth (a thick one traps heat).
    float warming = 0.0f;
    bool breathable = false;
};

struct TerrainDef : TraitEntry
{
    // How rough the ground is landed on, 0 flat to 1 broken, for the landing region to build.
    float roughness = 0.3f;
};

struct WeatherDef : TraitEntry
{
    // What falls: "none", "snow", "ash", "rain", "dust". How much (0 to 1), how far it lets you see as a multiplier on
    // the fog, and wind in metres a second.
    std::string particles = "none";
    float intensity = 0.0f;
    float visibility = 1.0f;
    glm::vec2 wind{0.0f, 5.0f};
};

struct CivilizationDef : TraitEntry
{
    // Whether people are there now, keep records of it, and run services there; and which kinds of landing region it
    // has, by weight.
    bool inhabited = false;
    bool records = false;
    bool services = false;
    std::map<std::string, float> regions;
};

struct SpecialDef : TraitEntry
{
    // How likely any one planet is to have it (weight is unused for these).
    float chance = 0.05f;
    // Only on planets of these biomes; empty: any. Only where there is air; and whether a moon can have it.
    std::vector<std::string> biomes;
    bool needsAir = false;
    bool onMoons = true;
};

struct StarDef : TraitEntry
{
    glm::vec3 color{1.0f, 0.95f, 0.85f};
    // How bright, against the Sun (which sets where the warm band is), and how big it is drawn.
    glm::vec2 luminosity{1.0f, 1.0f};
    float radius = 1.0f;
    glm::ivec2 planets{3, 7};
};

struct RegionKindDef : TraitEntry
{
    // Whether one is on the charts before anybody has been (otherwise it is found); and whether the ship itself sets down
    // there (a hub's landing field) rather than sending the shuttle.
    bool charted = false;
    bool ship = false;
    // What it is called on the charts: the designation's kind ("RESEARCH FACILITY") and qualifiers to go before it.
    std::vector<std::string> designations;
};

class UniverseData
{
public:
    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);
    // A small built-in set, for tests and for when the file is missing: enough to make a universe.
    void UseDefaults();
    bool Empty() const { return biomes.empty(); }

    const BiomeDef* Biome(const std::string& id) const;
    const AtmosphereDef* Atmosphere(const std::string& id) const;
    const TerrainDef* Terrain(const std::string& id) const;
    const WeatherDef* Weather(const std::string& id) const;
    const CivilizationDef* Civilization(const std::string& id) const;
    const SpecialDef* Special(const std::string& id) const;
    const RegionKindDef* RegionKind(const std::string& id) const;

    std::vector<BiomeDef> biomes;
    std::vector<AtmosphereDef> atmospheres;
    std::vector<TerrainDef> terrains;
    std::vector<WeatherDef> weathers;
    std::vector<CivilizationDef> civilizations;
    std::vector<SpecialDef> specials;
    std::vector<StarDef> stars;
    std::vector<RegionKindDef> regionKinds;
    // What systems are called: a catalogue and a number ("KEPLER-91"); planets after it, by Roman numeral.
    std::vector<std::string> catalogues{"KEPLER"};
    glm::ivec2 catalogueNumbers{10, 999};
    // Moons: a designation of their own, a prefix and a number ("LV-426").
    std::vector<std::string> moonCatalogues{"LV"};
    glm::ivec2 moonNumbers{100, 999};
    // Region designations: the part of a planet it is in ("NORTH CRYOSPHERE"), by biome; "any" for every biome.
    std::map<std::string, std::vector<std::string>> regionAreas;
};

// --- A universe ----------------------------------------------------------------------------------------------------

// Where a system is: its cell of the galaxy and which of that cell's systems. Packed into one number for saves and
// the wire.
struct SystemId
{
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    uint8_t index = 0;

    uint64_t Packed() const;
    static SystemId Unpack(uint64_t packed);
    bool operator==(const SystemId& other) const = default;
    bool operator<(const SystemId& other) const { return Packed() < other.Packed(); }
};

// A place to land: its seed (what it is built from), its kind, what the charts call it, and where on the planet it is
// (latitude and longitude in degrees), which with the planet's turning sets whether it is day there.
struct LandingRegion
{
    uint32_t seed = 0;
    std::string kind;
    std::string designation;
    glm::vec2 latLon{0.0f};
    bool charted = false; // on the charts from the start; otherwise found
};

enum class BodyKind : uint8_t
{
    Planet,
    Moon
};

// A planet or a moon.
struct Body
{
    uint16_t index = 0;
    BodyKind kind = BodyKind::Planet;
    int parent = -1;   // for a moon, the index of the planet it goes round
    std::string name;  // "KEPLER-91 IV"; a moon "LV-426"
    uint64_t seed = 0;
    // Its orbit: how far out (in astronomical units for a planet; for a moon, in its planet's radii), how long a turn
    // takes (seconds of the campaign's clock), where in it it was at the clock's zero (radians), and the slight tilt of
    // its plane.
    float orbit = 1.0f;
    float period = 600.0f;
    float phase = 0.0f;
    float inclination = 0.0f;
    // How big it is, in Earth radii, how fast it turns (seconds a day) and its axis's tilt (radians).
    float radius = 1.0f;
    float day = 300.0f;
    float tilt = 0.0f;
    float temperature = 0.0f; // degrees Celsius at the ground, typical
    // What it is (ids into the data), and how it looks from space.
    std::string biome;
    std::string atmosphere;
    std::string terrain;
    std::string weather;
    std::string civilization;
    std::vector<std::string> specials;
    glm::vec3 groundA{0.5f};
    glm::vec3 groundB{0.4f};
    glm::vec3 ocean{0.1f, 0.2f, 0.35f};
    float oceanAmount = 0.0f;
    float ice = 0.0f;
    float clouds = 0.0f;
    glm::vec3 cloudColor{1.0f};
    glm::vec3 airColor{0.35f, 0.55f, 0.95f};
    float air = 0.0f;
    bool gas = false;
    // Rings: inside and outside, in the body's radii (0: none), and their colour.
    glm::vec2 rings{0.0f};
    glm::vec3 ringColor{0.7f, 0.66f, 0.6f};
    std::vector<LandingRegion> regions;

    bool Landable() const { return !gas; }
    bool HasSpecial(const std::string& id) const;
};

struct StarSystem
{
    SystemId id;
    uint64_t seed = 0;
    std::string name;      // "KEPLER-91"
    glm::vec3 galaxy{0.0f}; // where it is in the galaxy, in light years
    std::string star;       // a StarDef id
    glm::vec3 starColor{1.0f};
    float luminosity = 1.0f;
    float starRadius = 1.0f;
    std::vector<Body> bodies; // planets in order outwards, each followed by its moons
    // The settled world with a hub, and the hub's place among its landing regions (where the ship itself sets down), or -1.
    int hub = -1;
    int hubRegion = -1;

    const Body* Find(int index) const { return index >= 0 && index < static_cast<int>(bodies.size()) ? &bodies[static_cast<size_t>(index)] : nullptr; }
    // Where a body is at `time` on the campaign's clock, in astronomical units from the star. A moon's place is its
    // planet's plus its own (drawn larger than life, so it is not lost inside its planet on the map).
    glm::vec3 Position(int index, double time) const;
    // How high the star stands over a place on a body at `time`: the sine of its height over the horizon there (1
    // overhead, 0 on the horizon, below 0 night), from where the body is and how far round it has turned.
    float SunHeight(int index, const glm::vec2& latLon, double time) const;
};

// What a system is from far off, without working all of it out: its name, its star and where it is. For the galaxy
// map, which shows a great many at once.
struct SystemGlance
{
    SystemId id;
    std::string name;
    std::string star;
    glm::vec3 starColor{1.0f};
    float luminosity = 1.0f;
    glm::vec3 position{0.0f};
};

// A place in the universe: a system, and a body in it (-1: the system itself, out between its planets).
struct Place
{
    uint64_t system = 0;
    int body = -1;
    bool operator==(const Place& other) const = default;
};

class Universe
{
public:
    // Galaxy cells are this many light years a side; a cell has a few systems in it, or none.
    static constexpr float kCellSize = 12.0f;

    void Reset(uint64_t seed, const UniverseData* data);
    uint64_t Seed() const { return m_seed; }
    const UniverseData* Data() const { return m_data; }

    // The system a campaign starts in: the cell at the middle, which always has one, with a settled world and a
    // shipyard on it.
    SystemId Home() const { return SystemId{}; }
    // A system, worked out the first time it is asked for. Null for a place with no system.
    const StarSystem* System(SystemId id);
    // A system from far off; cheap, and kept.
    const SystemGlance& Glance(SystemId id);
    const StarSystem* System(uint64_t packed) { return System(SystemId::Unpack(packed)); }
    // Every system within `lightYears` of a point, nearest first.
    std::vector<SystemId> Near(const glm::vec3& at, float lightYears);
    // How many systems this cell of the galaxy holds, and where (without working them out).
    int SystemsInCell(int x, int y, int z) const;
    glm::vec3 SystemPosition(SystemId id) const;

    // Built fresh, without the cache: for tests that want to be sure two buildings agree.
    static StarSystem Generate(uint64_t universeSeed, SystemId id, const UniverseData& data);

private:
    uint64_t m_seed = 0;
    const UniverseData* m_data = nullptr;
    std::map<uint64_t, std::unique_ptr<StarSystem>> m_systems;
    std::map<uint64_t, SystemGlance> m_glances;
};

// A planet's name and number: "IV" for 4.
std::string RomanNumeral(int value);

} // namespace pred
