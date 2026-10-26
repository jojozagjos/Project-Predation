#include "Game/World/Outpost.h"

#include "Game/World/Cargo.h"
#include "Game/World/PartBuilder.h"
#include "Game/World/ShipMap.h"
#include "Game/World/Vehicles.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace pred
{

namespace Outpost
{

namespace
{

using KestrelStation::Lamp;
using KestrelStation::Sign;

// SplitMix64, as the sites are planned with: the same sequence on every machine.
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
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    float Range(float low, float high) { return low + (high - low) * Unit(); }
    int Int(int low, int high) { return low + static_cast<int>(Next() % static_cast<uint64_t>(high - low + 1)); }
    bool Chance(float p) { return Unit() < p; }

private:
    uint64_t m_state;
};

constexpr float kG = ShipSpec::kFieldGround;
constexpr float kS = KestrelStation::kSlabTop;
constexpr float kPaint = 0.05f;
constexpr float kKerb = 0.15f;
constexpr float kDegrees = 57.2957795f;

// The landing field: the pad the ship stands on, the middle of it, with room all round for a ship twice this one's length
// and more than twice its beam -- the bigger ships to come set down on the same pads -- and poured concrete round it, the
// apron, the service vehicles' way round the pad and what is on the far side of it from everything else.
constexpr glm::vec4 kPad{-38.0f, -70.0f, 38.0f, 62.0f};
constexpr float kApron = 8.0f;
constexpr glm::vec4 kField{kPad.x - kApron, kPad.y - kApron, kPad.z + kApron, kPad.w + kApron};
// The way across from the stair to the street and the operations block's door, its crossing on the street striped.
constexpr float kCross = (KestrelStation::kCrossFrom + KestrelStation::kCrossTo) * 0.5f;
// None of the north cross street's lots further to starboard than this: the ship comes in low over the pad's front.
constexpr float kCrossLotsTo = -34.0f;
// The yards round the field, beyond the apron: to starboard (the hangars, their doors onto it), ahead and off to starboard
// (clear of the way in), and behind.
constexpr float kStarboardFrom = kField.y + 6.0f;
constexpr float kStarboardTo = kField.w;
constexpr float kAheadFrom = 32.0f;
constexpr float kBehindFrom = -30.0f;
constexpr float kBehindTo = 30.0f;
// Where the cinematics of setting down and taking off stand their cameras (ship_land, ship_takeoff), as (x, z): nothing is put
// within a few metres of them.
const glm::vec2 kCameras[] = {{-40.0f, -22.0f}, {-38.0f, -18.0f}, {-19.0f, -18.0f}, {-18.5f, -14.0f},
                              {-42.0f, -78.0f}, {-44.0f, -74.0f}, {18.0f, 42.0f},   {16.0f, 30.0f}};
// The streets, one after another out to port from the field, each a road between two pavements and the buildings along its
// far side fronting the far pavement; a service alley behind each row of buildings but the last, the next street beyond it.
constexpr float kPavement = 3.0f;
constexpr float kRoad = 16.0f;
constexpr float kBackRoad = 13.0f;
constexpr float kFrontage = 4.0f; // the far pavement, up to the buildings' fronts
constexpr float kRowDeepest = 30.0f;
constexpr float kAlley = 7.0f;
// The cross streets at the streets' ends, beyond them: a road this wide and a pavement on the outer side.
constexpr float kCrossRoad = 9.0f;
constexpr float kCrossPavement = 3.0f;

// Concrete, asphalt, steel, lamps and windows, as Kestrel's.
const glm::vec3 kConcrete{0.42f, 0.41f, 0.38f};
const glm::vec3 kPadColour{0.23f, 0.23f, 0.23f};
const glm::vec3 kAsphalt{0.12f, 0.12f, 0.13f};
const glm::vec3 kSteel{0.36f, 0.38f, 0.40f};
const glm::vec3 kSteelDark{0.17f, 0.18f, 0.19f};
const glm::vec3 kRust{0.42f, 0.24f, 0.14f};
const glm::vec3 kWhitePaint{0.80f, 0.80f, 0.76f};
const glm::vec3 kGrime{0.16f, 0.15f, 0.14f};
const glm::vec3 kSodium{1.0f, 0.72f, 0.36f};
const glm::vec3 kFlood{0.92f, 0.95f, 1.0f};
const glm::vec3 kWindow{1.0f, 0.86f, 0.62f};
const glm::vec3 kEdgeLight{0.35f, 0.55f, 1.0f};

// What an outpost's buildings are painted, one scheme to an outpost: the panels, the trim, and the colour of its warning paint.
struct Scheme
{
    glm::vec3 panel;
    glm::vec3 trim;
    glm::vec3 warning;
};
const Scheme kSchemes[] = {
    {{0.46f, 0.46f, 0.44f}, {0.17f, 0.18f, 0.19f}, {0.80f, 0.62f, 0.12f}}, // grey, dark steel, yellow
    {{0.38f, 0.42f, 0.38f}, {0.20f, 0.21f, 0.19f}, {0.80f, 0.62f, 0.12f}}, // green-grey
    {{0.52f, 0.48f, 0.40f}, {0.24f, 0.21f, 0.17f}, {0.86f, 0.42f, 0.12f}}, // sand, orange warning
    {{0.36f, 0.40f, 0.46f}, {0.15f, 0.17f, 0.20f}, {0.82f, 0.80f, 0.74f}}, // blue-grey, white warning
    {{0.58f, 0.57f, 0.53f}, {0.22f, 0.23f, 0.24f}, {0.70f, 0.20f, 0.14f}}, // off-white, red warning
};
const glm::vec3 kBoxes[] = {kRust, {0.18f, 0.32f, 0.36f}, {0.79f, 0.38f, 0.10f}, {0.32f, 0.33f, 0.30f}, {0.55f, 0.18f, 0.12f}, {0.30f, 0.36f, 0.24f}};

// What stands on a piece of ground.
enum class Use : uint8_t
{
    Operations, // the outpost's own block, across the street from the stair, its name over the door
    Block,      // a block of offices, stores or quarters
    Shed,       // a great shed, its door onto the street or the pad
    Hangar,     // a hangar on the field, its doors as wide as a ship, onto the apron
    Habitat,    // pressurised modules on legs, side by side
    Tanks,      // fuel or water, standing in a bund
    Containers, // a yard of freight containers, stacked
    Comms,      // a lattice mast, its dish, and the hut at its foot
    Open,       // a few crates on open ground
};

// A piece of ground something stands on: its front along the street or the pad, `length` along it and `depth` back from it.
// Things are placed in it by how far along the front (u), how high (y) and how far back (v); out of the front is -v.
struct Lot
{
    glm::vec2 origin{0.0f}; // where u and v are 0, as (x, z)
    int facing = 0;         // which way its front faces: 0 +x, 1 -x, 2 +z, 3 -z
    float length = 20.0f;
    float depth = 20.0f;

    glm::vec3 At(float u, float y, float v) const
    {
        switch (facing)
        {
        case 0: return {origin.x - v, y, origin.y + u};
        case 1: return {origin.x + v, y, origin.y + u};
        case 2: return {origin.x + u, y, origin.y - v};
        default: return {origin.x + u, y, origin.y + v};
        }
    }
    glm::vec3 Out() const
    {
        return facing == 0 ? glm::vec3(1.0f, 0.0f, 0.0f) : facing == 1 ? glm::vec3(-1.0f, 0.0f, 0.0f) : facing == 2 ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                                                                    : glm::vec3(0.0f, 0.0f, -1.0f);
    }
    // Which way a sign on its front faces, in degrees about up (0 faces +z).
    float Yaw() const { return facing == 0 ? 90.0f : facing == 1 ? -90.0f : facing == 2 ? 0.0f : 180.0f; }
    // A cylinder's turn to lie along the front.
    glm::vec3 AlongFront() const { return facing < 2 ? glm::vec3(90.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 90.0f); }
    // Its ground, as (x0, z0, x1, z1).
    glm::vec4 Rect(float grow = 0.0f) const
    {
        const glm::vec3 a = At(-grow, 0.0f, -grow);
        const glm::vec3 b = At(length + grow, 0.0f, depth + grow);
        return {std::min(a.x, b.x), std::min(a.z, b.z), std::max(a.x, b.x), std::max(a.z, b.z)};
    }
};

struct Placed
{
    Lot lot;
    Use use = Use::Open;
    std::string label;
    float door = -1.0f; // the operations block's door, along its front
    bool street = false; // fronting a street's pavement (or the field's apron, or open ground)
};

// A street along z, the field's side of it east: its east pavement from `pavementEast` to the road, the road from `roadEast` to
// `roadWest`, its west pavement out to `facade`, where the buildings along it front it; the doors along it, which its lamp
// posts keep clear of.
struct Street
{
    float pavementEast = 0.0f;
    float roadEast = 0.0f;
    float roadWest = 0.0f;
    float facade = 0.0f;
    std::vector<float> doors;
};

bool Overlaps(const glm::vec4& a, const glm::vec4& b)
{
    return a.x < b.z && a.z > b.x && a.y < b.w && a.w > b.y;
}

class Planner
{
public:
    Planner(uint32_t seed, Layout& out, const glm::vec3& ground, const glm::vec3& rock, const OutpostStyle& style)
        : m_random((static_cast<uint64_t>(seed) << 20) ^ 0x0B7905700ull), m_out(out), m_solid{out.solid}, m_dress{out.dressing}, m_ground(ground),
          m_rock(rock), m_style(style), m_seed(seed)
    {
        out.solid.name = "outpost";
        out.dressing.name = "outpost_dressing";
    }

    void Run()
    {
        // CIRRA's standard outpost in CIRRA's own colours, Kestrel's; another in one of the schemes; one nobody keeps faded and
        // rusting.
        m_scheme = kSchemes[m_random.Int(0, static_cast<int>(std::size(kSchemes)) - 1)];
        m_concrete = kConcrete * m_random.Range(0.85f, 1.1f);
        if (m_style.standard)
        {
            m_scheme = kSchemes[0];
            m_concrete = kConcrete;
        }
        if (m_style.abandoned)
        {
            m_scheme.panel = glm::mix(m_scheme.panel * 0.72f, kRust, 0.15f);
            m_scheme.warning *= 0.6f;
            m_concrete *= 0.8f;
        }
        // How big: one nobody keeps any more small or middling; CIRRA's middling or large; the rest anything. As many streets,
        // one behind another, and longer the bigger it is.
        const float big = m_random.Unit();
        m_size = m_style.abandoned ? (big < 0.55f ? 0 : 1) : m_style.standard ? (big < 0.45f ? 1 : 2) : (big < 0.3f ? 0 : big < 0.75f ? 1 : 2);
        m_streetNorth = -std::round(m_random.Range(150.0f, 200.0f) + (m_size == 2 ? 60.0f : 0.0f));
        m_streetSouth = std::round(m_random.Range(125.0f, 170.0f) + (m_size == 2 ? 50.0f : 0.0f));
        // The landing field: the ship's pad, and beside it to starboard as many more as the outpost is big, a taxiway between
        // each, the apron round them all.
        for (int p = 0; p <= m_size; ++p)
        {
            m_pads.push_back(kPad + glm::vec4(102.0f * static_cast<float>(p), 0.0f, 102.0f * static_cast<float>(p), 0.0f));
        }
        m_field = {kField.x, kField.y, m_pads.back().z + kApron, kField.w};
        float east = kField.x;
        for (int i = 0; i <= m_size; ++i)
        {
            Street street;
            street.pavementEast = east;
            street.roadEast = east - kPavement;
            street.roadWest = street.roadEast - (i == 0 ? kRoad : kBackRoad);
            street.facade = street.roadWest - kFrontage;
            m_streets.push_back(street);
            east = street.facade - kRowDeepest - kAlley;
        }
        PlanLots();
        PlanRoad();
        Clusters();
        Ground();
        Pad();
        for (const Placed& placed : m_lots)
        {
            Fill(placed);
        }
        // After the buildings: the lamp posts stand clear of their doors.
        Streets();
        CrossStreets();
        Alleys();
        Parked();
        Service();
        Fence();
        Outskirts();
        // As many lamps as there are fittings for (ShipMap's), every one with its head -- and of those only as many work as its
        // owner keeps working; the rest are dark heads. Nobody there: its lit signs dark too.
        if (m_out.lamps.size() > kMaxLamps)
        {
            m_out.lamps.resize(kMaxLamps);
        }
        std::vector<Lamp> working;
        std::vector<Lamp> dead;
        for (size_t i = 0; i < m_out.lamps.size(); ++i)
        {
            Random which((static_cast<uint64_t>(m_seed) << 24) ^ (0x4C414D50ull + i)); // 'LAMP'
            (which.Unit() < m_style.lit ? working : dead).push_back(m_out.lamps[i]);
        }
        m_out.lamps = working;
        KestrelStation::LampHeads(m_dress, working);
        KestrelStation::LampHeads(m_dress, dead, false);
        if (m_style.abandoned)
        {
            for (Sign& sign : m_out.signs)
            {
                sign.dark = true;
            }
        }
    }

private:
    // --- What goes where -------------------------------------------------------------------------------------------------

    // The west-most road's west edge: where the cross streets reach to.
    float WestRoad() const { return m_streets.back().roadWest; }

    void PlanLots()
    {
        // The operations block across the first street from the stair, the crossing at its door.
        const Street& first = m_streets.front();
        const float mainLength = std::round(m_random.Range(30.0f, 44.0f));
        const float before = std::round(m_random.Range(11.0f, mainLength - 11.0f));
        const float mainFrom = kCross - before;
        const float deepest = m_size >= 1 ? kRowDeepest : 34.0f;
        Placed main{Lot{{first.facade, mainFrom}, 0, mainLength, std::min(std::round(m_random.Range(22.0f, 32.0f)), deepest)}, Use::Operations,
                    m_style.operations, before, true};
        m_lots.push_back(main);
        // Along each street, both ways from it on the first, with alleys between, as far as the street runs.
        for (size_t i = 0; i < m_streets.size(); ++i)
        {
            const Street& street = m_streets[i];
            const bool last = i + 1 == m_streets.size();
            const auto deep = [&]() { return last ? std::round(m_random.Range(18.0f, 36.0f)) : std::min(std::round(m_random.Range(16.0f, 30.0f)), deepest); };
            if (i == 0)
            {
                float cursor = mainFrom - m_random.Range(5.0f, 10.0f);
                while (cursor - (m_streetNorth + 4.0f) >= 14.0f)
                {
                    const float length = std::min(std::round(m_random.Range(18.0f, 40.0f)), cursor - (m_streetNorth + 4.0f));
                    AlongStreet({{street.facade, cursor - length}, 0, length, deep()});
                    cursor -= length + m_random.Range(5.0f, 10.0f);
                }
                cursor = mainFrom + mainLength + m_random.Range(5.0f, 10.0f);
                while ((m_streetSouth - 4.0f) - cursor >= 14.0f)
                {
                    const float length = std::min(std::round(m_random.Range(18.0f, 40.0f)), (m_streetSouth - 4.0f) - cursor);
                    AlongStreet({{street.facade, cursor}, 0, length, deep()});
                    cursor += length + m_random.Range(5.0f, 10.0f);
                }
                continue;
            }
            for (float at = m_streetNorth + m_random.Range(3.0f, 7.0f); m_streetSouth - 3.0f - at >= 14.0f;)
            {
                const float length = std::min(std::round(m_random.Range(18.0f, 42.0f)), m_streetSouth - 3.0f - at);
                AlongStreet({{street.facade, at}, 0, length, deep()});
                at += length + m_random.Range(5.0f, 10.0f);
            }
        }
        // And the cross streets': facing them from beyond, as far to starboard as the way the ship comes in allows.
        for (const bool north : {true, false})
        {
            const float front = north ? m_streetNorth - kCrossRoad - kCrossPavement : m_streetSouth + kCrossRoad + kCrossPavement;
            const float to = north ? kCrossLotsTo : first.roadEast;
            for (float at = WestRoad() + m_random.Range(0.0f, 5.0f); to - at >= 16.0f;)
            {
                const float length = std::min(std::round(m_random.Range(20.0f, 40.0f)), to - at);
                AlongStreet({{at, front}, north ? 2 : 3, length, std::round(m_random.Range(18.0f, 34.0f))});
                at += length + m_random.Range(6.0f, 12.0f);
            }
        }
        // To starboard, across the field from the street: hangars, their doors onto the apron, with a fuel farm or a yard between.
        for (float at = kStarboardFrom; kStarboardTo - at >= 30.0f;)
        {
            const float length = std::min(std::round(m_random.Range(44.0f, 64.0f)), kStarboardTo - at);
            const float roll = m_random.Unit();
            const Use use = roll < 0.65f ? Use::Hangar : roll < 0.85f ? Use::Tanks : Use::Containers;
            Add({{m_field.z, at}, 1, length, std::round(m_random.Range(42.0f, 60.0f))}, use);
            at += length + m_random.Range(6.0f, 12.0f);
        }
        // Ahead, off to starboard (clear of the way in), and behind.
        {
            const float roll = m_random.Unit();
            const Use use = roll < 0.35f ? Use::Containers : roll < 0.55f ? Use::Tanks : roll < 0.7f ? Use::Block : roll < 0.85f ? Use::Shed : Use::Open;
            const float length = std::round(m_random.Range(30.0f, 60.0f));
            Add({{kAheadFrom, kField.y}, 2, length, std::round(m_random.Range(24.0f, 40.0f))}, use);
        }
        {
            const float roll = m_random.Unit();
            const Use use = roll < 0.25f ? Use::Block : roll < 0.45f ? Use::Habitat : roll < 0.6f ? Use::Tanks : roll < 0.8f ? (m_comms ? Use::Containers : Use::Comms)
                                                                                                                              : Use::Open;
            // (Behind a service road across the back of the apron, where cargo is staged.)
            Add({{kBehindFrom, kField.w + 6.0f}, 3, kBehindTo - kBehindFrom, std::round(m_random.Range(24.0f, 38.0f))}, use);
        }
        m_sternWall = m_style.standard || m_random.Chance(0.5f);
    }

    void AlongStreet(const Lot& lot)
    {
        // As often as its owner has each kind (OutpostStyle::uses), or a mix of everything.
        static const std::pair<const char*, Use> kinds[] = {{"block", Use::Block},           {"shed", Use::Shed},   {"habitat", Use::Habitat},
                                                            {"tanks", Use::Tanks},           {"containers", Use::Containers},
                                                            {"comms", Use::Comms},           {"open", Use::Open}};
        static const float fallback[] = {34.0f, 15.0f, 13.0f, 9.0f, 14.0f, 8.0f, 7.0f};
        float weights[std::size(kinds)];
        float total = 0.0f;
        for (size_t i = 0; i < std::size(kinds); ++i)
        {
            const auto found = m_style.uses.find(kinds[i].first);
            weights[i] = m_style.uses.empty() ? fallback[i] : found != m_style.uses.end() ? std::max(found->second, 0.0f) : 0.0f;
            total += weights[i];
        }
        float pick = m_random.Unit() * std::max(total, 1.0e-6f);
        Use use = Use::Block;
        for (size_t i = 0; i < std::size(kinds); ++i)
        {
            pick -= weights[i];
            if (pick < 0.0f)
            {
                use = kinds[i].second;
                break;
            }
        }
        Add(lot, use, true);
    }

    void Add(const Lot& lot, Use use, bool street = false)
    {
        // One mast to an outpost, and one of each name.
        if (use == Use::Comms && (m_comms || lot.length < 20.0f || lot.depth < 14.0f))
        {
            use = Use::Block;
        }
        if ((use == Use::Tanks && lot.depth < 12.0f) || (use == Use::Habitat && lot.depth < 9.0f))
        {
            use = Use::Block;
        }
        m_comms = m_comms || use == Use::Comms;
        Placed placed{lot, use, Label(use), -1.0f, street};
        m_lots.push_back(placed);
    }

    std::string Label(Use use)
    {
        // What its owner calls each kind of building, one of each name to an outpost.
        const auto pick = [this](const std::vector<std::string>& names) -> std::string
        {
            if (names.empty())
            {
                return {};
            }
            const size_t start = static_cast<size_t>(m_random.Int(0, static_cast<int>(names.size()) - 1));
            for (size_t i = 0; i < names.size(); ++i)
            {
                const std::string& name = names[(start + i) % names.size()];
                if (std::find(m_labels.begin(), m_labels.end(), name) == m_labels.end())
                {
                    m_labels.push_back(name);
                    return name;
                }
            }
            return {};
        };
        switch (use)
        {
        case Use::Block: return pick(m_style.blocks);
        case Use::Shed:
        case Use::Hangar: return pick(m_style.sheds);
        case Use::Habitat: return pick(m_style.habitats);
        case Use::Tanks: return m_style.tanks.empty() ? std::string() : m_style.tanks[static_cast<size_t>(m_random.Int(0, static_cast<int>(m_style.tanks.size()) - 1))];
        case Use::Comms: return m_style.comms;
        default: return {};
        }
    }

    // Whether a unit on a roof (u0, v0, u1, v1) would stand into one already there (with a hand's breadth between); if not,
    // it is there now.
    static bool RoofTaken(std::vector<glm::vec4>& units, const glm::vec4& unit)
    {
        for (const glm::vec4& other : units)
        {
            if (unit.x < other.z + 0.1f && unit.z > other.x - 0.1f && unit.y < other.w + 0.1f && unit.w > other.y - 0.1f)
            {
                return true;
            }
        }
        units.push_back(unit);
        return false;
    }

    // Whether a place (x, z) is within `reach` of where a cinematic stands a camera.
    static bool NearCamera(float x, float z, float reach)
    {
        return std::any_of(std::begin(kCameras), std::end(kCameras), [&](const glm::vec2& camera) { return glm::distance(camera, glm::vec2(x, z)) < reach; });
    }

    // --- The ground: what is poured, and the levelled ground round it ---------------------------------------------------

    // Poured flat into the levelled ground (from it up to the concrete's top, or higher), never over anything poured before it:
    // the pad, then the roads, the pavements, the alleys, the apron, the yards under the buildings -- so no two share a face.
    void Pour(const char* stem, const glm::vec4& rect, float top, const glm::vec3& colour, float rough, bool squares = false)
    {
        for (const glm::vec4& piece : SubtractRects(rect, m_poured))
        {
            if (!squares)
            {
                m_solid.Box(stem, {piece.x, kG, piece.y}, {piece.z, top, piece.w}, colour, rough);
                continue;
            }
            // In twenty-metre squares, a shade apart, as concrete is poured.
            for (float x = std::floor(piece.x / 20.0f) * 20.0f; x < piece.z - 0.01f; x += 20.0f)
            {
                for (float z = std::floor(piece.y / 20.0f) * 20.0f; z < piece.w - 0.01f; z += 20.0f)
                {
                    const glm::vec4 square{std::max(x, piece.x), std::max(z, piece.y), std::min(x + 20.0f, piece.z), std::min(z + 20.0f, piece.w)};
                    if (square.z - square.x < 0.05f || square.w - square.y < 0.05f)
                    {
                        continue;
                    }
                    const glm::vec3 shade = colour * glm::mix(0.88f, 1.0f, Hash(x, z));
                    m_solid.Box(stem, {square.x, kG, square.y}, {square.z, top, square.w}, shade, rough);
                }
            }
        }
        m_poured.push_back(rect);
    }
    // A number from 0 to 1 for a place, the same every time.
    float Hash(float x, float z) const
    {
        uint32_t h = static_cast<uint32_t>(static_cast<int32_t>(std::floor(x)) * 73856093) ^ static_cast<uint32_t>(static_cast<int32_t>(std::floor(z)) * 19349663) ^
                     m_seed;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return static_cast<float>(h & 0xFFFFu) / 65535.0f;
    }

    void Ground()
    {
        // The pads, each in four, their seams down the middle (under the ship, where nothing of the ship's own lies on them);
        // darker than the apron.
        for (const glm::vec4& pad : m_pads)
        {
            const float cx = (pad.x + pad.z) * 0.5f;
            for (const glm::vec2 x : {glm::vec2{pad.x, cx}, glm::vec2{cx, pad.z}})
            {
                for (const glm::vec2 z : {glm::vec2{pad.y, 2.0f}, glm::vec2{2.0f, pad.w}})
                {
                    Pour("pad", {x.x, z.x, x.y, z.y}, kS, kPadColour * (m_style.abandoned ? 0.85f : 1.0f), 0.85f);
                }
            }
        }
        // The roads and their pavements, the cross streets and theirs, the alleys behind the rows.
        for (size_t i = 0; i < m_streets.size(); ++i)
        {
            const Street& street = m_streets[i];
            Pour("road", {street.roadWest, m_streetNorth, street.roadEast, m_streetSouth}, kS, kAsphalt, 0.95f);
        }
        for (const bool north : {true, false})
        {
            const float z0 = north ? m_streetNorth - kCrossRoad : m_streetSouth;
            Pour("road", {WestRoad(), z0, m_streets.front().roadEast, z0 + kCrossRoad}, kS, kAsphalt, 0.95f);
        }
        const glm::vec3 pavement = m_concrete * 0.86f;
        for (const Street& street : m_streets)
        {
            Pour("pavement", {street.roadEast, m_streetNorth, street.pavementEast, m_streetSouth}, kS + kKerb, pavement, 0.9f);
            Pour("pavement", {street.facade, m_streetNorth, street.roadWest, m_streetSouth}, kS + kKerb, pavement, 0.9f);
        }
        for (const bool north : {true, false})
        {
            const float z0 = north ? m_streetNorth - kCrossRoad - kCrossPavement : m_streetSouth + kCrossRoad;
            Pour("pavement", {WestRoad(), z0, m_streets.front().pavementEast, z0 + kCrossPavement}, kS + kKerb, pavement, 0.9f);
        }
        for (size_t i = 0; i + 1 < m_streets.size(); ++i)
        {
            Pour("alley", {m_streets[i + 1].pavementEast, m_streetNorth, m_streets[i].facade - kRowDeepest, m_streetSouth}, kS, m_concrete * 0.8f, 0.9f, true);
        }
        // The apron round the pad, out to the street's pavement.
        Pour("apron", m_field, kS, m_concrete, 0.9f, true);
        // The lanes out along the road in to the buildings there.
        for (const glm::vec4& lane : m_lanes)
        {
            Pour("lane", lane, kS, m_concrete * 0.8f, 0.9f, true);
        }
        // And the yards under the buildings: the lot and a little round it, poured to the concrete's top.
        for (const Placed& placed : m_lots)
        {
            Pour("yard", placed.lot.Rect(placed.street ? 0.0f : 6.0f), kS, m_concrete * 0.94f, 0.9f, true);
        }

        // How far the outpost reaches: everything poured, and the cinematics' cameras' places.
        glm::vec2 lo{1.0e9f};
        glm::vec2 hi{-1.0e9f};
        for (const glm::vec4& rect : m_poured)
        {
            lo = glm::min(lo, glm::vec2(rect.x, rect.y));
            hi = glm::max(hi, glm::vec2(rect.z, rect.w));
        }
        for (const glm::vec2 camera : kCameras)
        {
            lo = glm::min(lo, camera - glm::vec2(6.0f));
            hi = glm::max(hi, camera + glm::vec2(6.0f));
        }
        m_lo = glm::floor(lo / 10.0f) * 10.0f - glm::vec2(10.0f);
        m_hi = glm::ceil(hi / 10.0f) * 10.0f + glm::vec2(10.0f);
        // (The world's own ground round it is made by the game, as a site's is: SiteMap::BuildFrom, levelled under all of it.)
        m_out.slab = {m_lo.x, m_lo.y, m_hi.x, m_hi.y};
        m_out.levelled = m_poured;
    }

    void Pad()
    {
        const float s = kS;
        const float paint = s + kPaint;
        const glm::vec3 hazard = m_scheme.warning;
        // A guide line out ahead of the ship's pad across the apron, the way it comes in.
        for (float z = kField.y + 1.0f; z < kPad.y - 1.2f; z += 6.0f)
        {
            m_dress.Box("pad_line", {-0.2f, s, z}, {0.2f, paint, std::min(z + 3.0f, kPad.y - 1.2f)}, kWhitePaint * 0.9f, 0.7f);
        }
        for (size_t p = 0; p < m_pads.size(); ++p)
        {
            const glm::vec4 pad = m_pads[p];
            const float cx = (pad.x + pad.z) * 0.5f;
            // A hazard border round each pad, its sides whole and its ends between them; a box at each corner of where a ship
            // of this one's size stands, and the pad's number ahead of it.
            m_dress.Box("pad_edge", {pad.x + 0.5f, s, pad.y + 0.5f}, {pad.x + 1.4f, paint, pad.w - 0.5f}, hazard, 0.7f);
            m_dress.Box("pad_edge", {pad.z - 1.4f, s, pad.y + 0.5f}, {pad.z - 0.5f, paint, pad.w - 0.5f}, hazard, 0.7f);
            m_dress.Box("pad_edge", {pad.x + 1.4f, s, pad.y + 0.5f}, {pad.z - 1.4f, paint, pad.y + 1.4f}, hazard, 0.7f);
            m_dress.Box("pad_edge", {pad.x + 1.4f, s, pad.w - 1.4f}, {pad.z - 1.4f, paint, pad.w - 0.5f}, hazard, 0.7f);
            for (const float sx : {-1.0f, 1.0f})
            {
                for (const float sz : {-1.0f, 1.0f})
                {
                    // An L at each corner: along x and along z from the corner.
                    const glm::vec2 corner{cx + sx * 24.0f, sz < 0.0f ? -44.0f : 40.0f};
                    const float ax = corner.x - sx * 5.0f;
                    const float az = corner.y - sz * 5.0f;
                    m_dress.Box("stand_mark", {std::min(corner.x, ax), s, corner.y - 0.2f}, {std::max(corner.x, ax), paint, corner.y + 0.2f}, kWhitePaint * 0.85f,
                                0.7f);
                    m_dress.Box("stand_mark", {corner.x - 0.2f, s, std::min(corner.y, az) + (sz < 0.0f ? 0.2f : 0.0f)},
                                {corner.x + 0.2f, paint, std::max(corner.y, az) - (sz < 0.0f ? 0.0f : 0.2f)}, kWhitePaint * 0.85f, 0.7f);
                }
            }
            const int number = (p == 0 ? m_random.Int(1, 7) : m_padNumber + static_cast<int>(p));
            m_padNumber = p == 0 ? number : m_padNumber;
            m_out.signs.push_back({p == 0 ? std::string("bay_number") : "bay_number_" + std::to_string(p), {"0" + std::to_string(number)},
                                   {cx, s + kPaint + 0.01f, -54.0f}, 180.0f, 12.0f, 7.5f, Sign::Style::Floor});
            // Lights sunk round its edge, blue, a few metres apart, out on the apron; nobody there, dead.
            for (float z = pad.y + 4.0f; z < pad.w - 2.0f; z += 8.0f)
            {
                for (const float x : {pad.x - 0.6f, pad.z + 0.6f})
                {
                    m_dress.Box("fx_pad_lamp", {x - 0.2f, s, z - 0.2f}, {x + 0.2f, s + 0.1f, z + 0.2f}, Glass(kEdgeLight), 0.3f, 0.0f, Glow(3.0f));
                }
            }
            for (float x = pad.x + 6.0f; x < pad.z - 4.0f; x += 8.0f)
            {
                for (const float z : {pad.y - 0.6f, pad.w + 0.6f})
                {
                    m_dress.Box("fx_pad_lamp", {x - 0.2f, s, z - 0.2f}, {x + 0.2f, s + 0.1f, z + 0.2f}, Glass(kEdgeLight), 0.3f, 0.0f, Glow(3.0f));
                }
            }
            // Floodlight masts along both sides on the apron, each with a crossbar and lamps on it aimed in across the pad (the
            // ship's two each, the others' one).
            for (const float x : {pad.x - 4.0f, pad.z + 4.0f})
            {
                for (const float z : {-46.0f, 6.0f, 50.0f})
                {
                    const float high = 22.0f;
                    m_solid.Box("bay_mast", {x - 0.35f, s, z - 0.35f}, {x + 0.35f, s + high, z + 0.35f}, kSteelDark, 0.5f, 0.7f);
                    m_dress.Box("mast_bar", {x - 0.12f, s + high, z - 1.75f}, {x + 0.12f, s + high + 0.25f, z + 1.75f}, kSteelDark, 0.5f, 0.7f);
                    Beacon({x, s + high + 0.25f, z});
                    for (const float side : p == 0 ? std::vector<float>{-1.0f, 1.0f} : std::vector<float>{1.0f})
                    {
                        const glm::vec3 at{x, s + high - 0.2f, z + side * 1.2f};
                        const glm::vec3 aim{x < cx ? x + 26.0f : x - 26.0f, s, z + side * 8.0f};
                        m_out.lamps.push_back({at, glm::normalize(aim - at), kFlood, 520.0f, 75.0f, 40.0f, 80.0f});
                    }
                }
            }
            // On some of the others, a craft standing: a shuttle set down there, its ramp up.
            if (p > 0 && m_random.Chance(m_style.abandoned ? 0.3f : 0.6f))
            {
                const ModelAsset shuttle = Vehicles::ShuttleModel();
                const glm::vec3 at{cx + m_random.Range(-10.0f, 10.0f), s, m_random.Range(-20.0f, 20.0f)};
                for (ModelPart part : shuttle.parts)
                {
                    if (!part.parent.empty() || part.name.rfind("ramp", 0) == 0)
                    {
                        continue;
                    }
                    part.position += at;
                    part.name = "parked_" + part.name;
                    m_dress.model.parts.push_back(part);
                }
                glm::vec3 low{1.0e9f};
                glm::vec3 high{-1.0e9f};
                for (const ModelPart& part : shuttle.parts)
                {
                    low = glm::min(low, part.position - part.size * 0.5f);
                    high = glm::max(high, part.position + part.size * 0.5f);
                }
                m_solid.Box("solid_parked", at + glm::vec3(low.x, std::max(low.y, 0.0f), low.z), at + high, kSteelDark);
            }
        }
        // The ground supply the ship is plugged into, beside where its stair comes down, and its cable across to the ship.
        KestrelStation::Supply(m_solid);
        m_dress.Box("bay_cable", {-10.8f, s + 0.02f, 3.6f}, {-5.6f, s + 0.12f, 3.75f}, kSteelDark, 0.7f, 0.2f);
        // A blast wall behind the ship's pad, as CIRRA's outposts have it, buttressed, the mark on it; out on the apron's back edge.
        if (m_sternWall)
        {
            const float high = std::round(m_random.Range(6.0f, 8.0f));
            const float z0 = kPad.w + 2.0f;
            m_solid.Box("bay_wall", {-30.0f, s, z0}, {30.0f, s + high, z0 + 1.0f}, m_concrete * 0.78f, 0.9f);
            m_dress.Box("coping", {-30.15f, s + high - 0.05f, z0 - 0.15f}, {30.15f, s + high + 0.3f, z0 + 1.15f}, m_concrete, 0.85f);
            m_dress.Box("grime", {-29.95f, s, z0 - 0.05f}, {29.95f, s + 1.3f, z0 + 0.05f}, kGrime, 0.95f);
            for (float x = -24.0f; x < 26.0f; x += 8.0f)
            {
                m_solid.Box("bay_buttress", {x - 0.35f, s, z0 - 0.6f}, {x + 0.35f, s + high - 0.4f, z0}, kSteel, 0.6f, 0.5f);
            }
            if (m_style.mark)
            {
                // In the gap between the two buttresses nearest the middle.
                m_out.signs.push_back({"bay_logo", {}, {4.0f, s + high - 2.6f, z0 - 0.7f}, 180.0f, 6.0f, 1.5f, Sign::Style::Logo});
            }
        }
    }

    // --- The streets -------------------------------------------------------------------------------------------------------

    // A lamp post, its arm out over the road: not where a cinematic stands a camera.
    void StreetLamp(const glm::vec3& foot, const glm::vec3& reach)
    {
        if (NearCamera(foot.x, foot.z, 4.0f))
        {
            return;
        }
        m_solid.Box("street_post", foot + glm::vec3(-0.13f, 0.0f, -0.13f), foot + glm::vec3(0.13f, 9.8f, 0.13f), kSteelDark, 0.5f, 0.7f);
        const glm::vec3 end = foot + reach;
        m_dress.Box("street_arm", glm::min(foot, end) + glm::vec3(-0.06f, 9.6f, -0.06f), glm::max(foot, end) + glm::vec3(0.06f, 9.75f, 0.06f), kSteelDark,
                    0.5f, 0.7f);
        m_out.lamps.push_back({end + glm::vec3(0.0f, 9.45f, 0.0f), {0.0f, -1.0f, 0.0f}, kSodium, 48.0f, 22.0f, 70.0f, 120.0f});
    }

    void Streets()
    {
        const float s = kS;
        const float paint = s + kPaint;
        for (size_t i = 0; i < m_streets.size(); ++i)
        {
            const Street& street = m_streets[i];
            const float middle = (street.roadWest + street.roadEast) * 0.5f;
            // The first street's crossing from the field to the operations block's door, striped; its lines broken for it.
            const bool crossing = i == 0;
            const auto onCrossing = [&](float z0, float z1) { return crossing && z1 > KestrelStation::kCrossFrom - 1.0f && z0 < KestrelStation::kCrossTo + 1.0f; };
            for (float z = m_streetNorth + 2.0f; z < m_streetSouth - 3.0f; z += 8.0f)
            {
                if (!onCrossing(z, z + 3.0f))
                {
                    m_dress.Box("road_line", {middle - 0.1f, s, z}, {middle + 0.1f, paint, z + 3.0f}, kWhitePaint, 0.7f);
                }
            }
            for (const float x : {street.roadWest + 0.5f, street.roadEast - 0.5f})
            {
                if (crossing)
                {
                    m_dress.Box("road_line", {x - 0.1f, s, m_streetNorth + 0.2f}, {x + 0.1f, paint, KestrelStation::kCrossFrom - 0.6f}, kWhitePaint * 0.9f, 0.7f);
                    m_dress.Box("road_line", {x - 0.1f, s, KestrelStation::kCrossTo + 0.6f}, {x + 0.1f, paint, m_streetSouth - 0.2f}, kWhitePaint * 0.9f, 0.7f);
                }
                else
                {
                    m_dress.Box("road_line", {x - 0.1f, s, m_streetNorth + 0.2f}, {x + 0.1f, paint, m_streetSouth - 0.2f}, kWhitePaint * 0.9f, 0.7f);
                }
            }
            if (crossing)
            {
                for (float x = street.roadWest + 0.4f; x < street.roadEast - 0.5f; x += 1.1f)
                {
                    m_dress.Box("crossing", {x, s, KestrelStation::kCrossFrom}, {x + 0.55f, paint, KestrelStation::kCrossTo}, kWhitePaint, 0.7f);
                }
            }
            // Lamp posts along both pavements, their arms out over the road, staggered -- not on the crossing, nor in front of a
            // door.
            const auto byDoor = [&](float z)
            {
                return std::any_of(street.doors.begin(), street.doors.end(), [z](float door) { return std::abs(door - z) < 5.0f; }) ||
                       (crossing && std::abs(z - kCross) < 5.0f);
            };
            for (float z = m_streetNorth + 10.0f; z < m_streetSouth - 6.0f; z += 30.0f)
            {
                for (const float side : {1.0f, -1.0f})
                {
                    const float at = side > 0.0f ? z : z + 15.0f;
                    if (at > m_streetSouth - 4.0f || byDoor(at))
                    {
                        continue;
                    }
                    const float x = side > 0.0f ? street.roadEast + 1.4f : street.roadWest - 1.2f;
                    StreetLamp({x, s + kKerb, at}, {side > 0.0f ? -2.0f : 2.0f, 0.0f, 0.0f});
                }
            }
        }
    }

    // A cross street's lines and lamps (its road and pavement are poured: Ground).
    void CrossStreets()
    {
        const float s = kS;
        const float paint = s + kPaint;
        const float x0 = WestRoad();
        const float x1 = m_streets.front().roadEast;
        for (const bool north : {true, false})
        {
            const float z0 = north ? m_streetNorth - kCrossRoad : m_streetSouth;
            const float middle = z0 + kCrossRoad * 0.5f;
            for (float x = x0 + 2.0f; x < x1 - 3.0f; x += 8.0f)
            {
                m_dress.Box("road_line", {x, s, middle - 0.1f}, {x + 3.0f, paint, middle + 0.1f}, kWhitePaint, 0.7f);
            }
            const float pavement = north ? z0 - kCrossPavement * 0.5f : z0 + kCrossRoad + kCrossPavement * 0.5f;
            for (float x = x0 + 9.0f; x < x1 - 9.0f; x += 28.0f)
            {
                StreetLamp({x, s + kKerb, pavement}, {0.0f, 0.0f, north ? 2.0f : -2.0f});
            }
        }
    }

    void Alleys()
    {
        // Behind each row but the last: bins, pallets, a container or two, pipes along the backs -- what a street keeps out of
        // sight.
        const float s = kS;
        for (size_t i = 0; i + 1 < m_streets.size(); ++i)
        {
            const float east = m_streets[i].facade - kRowDeepest;
            for (float z = m_streetNorth + 4.0f; z < m_streetSouth - 8.0f; z += m_random.Range(8.0f, 16.0f))
            {
                const float roll = m_random.Unit();
                const float x = east - m_random.Range(0.6f, 1.4f);
                if (roll < 0.3f)
                {
                    Bin({x - 1.0f, s, z});
                }
                else if (roll < 0.55f)
                {
                    Pallets({x - 1.4f, s, z}, m_random.Int(1, 3));
                }
                else if (roll < 0.7f)
                {
                    Container({east - 4.4f, s, z}, {east - 1.96f, s + 2.59f, z + 6.06f}, kBoxes[m_random.Int(0, 5)]);
                    z += 4.0f;
                }
                else if (roll < 0.85f)
                {
                    Barrels({x - 1.2f, s, z}, m_random.Int(2, 5));
                }
            }
            m_dress.Cylinder("pipe", {east - 0.4f, s + 3.6f, (m_streetNorth + m_streetSouth) * 0.5f}, 0.4f, m_streetSouth - m_streetNorth - 6.0f,
                             {90.0f, 0.0f, 0.0f}, kSteel * 1.05f, 0.4f, 0.7f);
            for (float z = m_streetNorth + 6.0f; z < m_streetSouth - 4.0f; z += 10.0f)
            {
                m_solid.Box("pipe_support", {east - 0.6f, s, z - 0.1f}, {east - 0.2f, s + 3.4f, z + 0.1f}, kSteelDark, 0.5f, 0.7f);
            }
        }
    }

    // --- Out on the land round it --------------------------------------------------------------------------------------------

    // Something standing out on the land: its parts made about its foot (up from the ground) by `make`, stood where the ground is
    // there (SiteMap), on the lowest of it within `reach`.
    template <typename Make> void Stand(const glm::vec2& at, float reach, Make make)
    {
        ModelAsset solid;
        ModelAsset dressing;
        PartBuilder s{solid};
        PartBuilder d{dressing};
        make(s, d);
        SitePlan::Outlying piece;
        piece.at = at;
        piece.reach = reach;
        piece.solid = std::move(solid.parts);
        piece.dressing = std::move(dressing.parts);
        m_out.outlying.push_back(std::move(piece));
        m_standing.emplace_back(at.x, at.y, reach);
    }
    // How far a place is from the road in, beyond its half width.
    float FromRoad(const glm::vec2& at) const
    {
        float nearest = 1.0e9f;
        for (const SitePlan::Road& road : m_out.roads)
        {
            for (size_t k = 0; k + 1 < road.line.size(); ++k)
            {
                const glm::vec2 ab = road.line[k + 1] - road.line[k];
                const float t = std::clamp(glm::dot(at - road.line[k], ab) / std::max(glm::dot(ab, ab), 1.0e-4f), 0.0f, 1.0f);
                nearest = std::min(nearest, glm::distance(at, road.line[k] + ab * t) - road.width * 0.5f);
            }
        }
        return nearest;
    }
    // Whether a place out on the land is clear for something so wide: of the town, its yards and its fence, of the road, of the
    // way the ship comes in, of what already stands out there -- and on the land the outpost's ground is made over.
    bool ClearOut(const glm::vec2& at, float reach) const
    {
        for (const glm::vec4& rect : m_poured)
        {
            if (at.x > rect.x - reach - 20.0f && at.x < rect.z + reach + 20.0f && at.y > rect.y - reach - 20.0f && at.y < rect.w + reach + 20.0f)
            {
                return false;
            }
        }
        if (std::abs(at.x) < 90.0f + reach && at.y < 0.0f)
        {
            return false;
        }
        if (FromRoad(at) < reach + 4.0f)
        {
            return false;
        }
        const glm::vec2 middle = (m_lo + m_hi) * 0.5f;
        const glm::vec2 half = (m_hi - m_lo) * 0.5f;
        if (glm::any(glm::greaterThan(glm::abs(at - middle), half + glm::vec2(200.0f - reach))))
        {
            return false;
        }
        return std::none_of(m_standing.begin(), m_standing.end(),
                            [&](const glm::vec3& other) { return glm::distance(glm::vec2(other.x, other.y), at) < other.z + reach + 3.0f; });
    }
    // A clear place out on the land, so far out from the town at most, somewhere round it; false when none was found.
    bool FindOut(float reach, glm::vec2& at)
    {
        const glm::vec2 middle = (m_lo + m_hi) * 0.5f;
        const glm::vec2 half = (m_hi - m_lo) * 0.5f;
        for (int tries = 0; tries < 60; ++tries)
        {
            const float a = m_random.Range(0.0f, 6.2831853f);
            const glm::vec2 way{std::cos(a), std::sin(a)};
            const float edge = std::min(half.x / std::max(std::abs(way.x), 1.0e-3f), half.y / std::max(std::abs(way.y), 1.0e-3f));
            at = middle + way * (edge + m_random.Range(40.0f, 150.0f));
            if (ClearOut(at, reach))
            {
                return true;
            }
        }
        return false;
    }

    void PlanRoad()
    {
        // The road in: on from the west end of a cross street, out across the land, wandering a little, gravel graded into the
        // ground (SiteTerrain), two hundred metres and more of it.
        const bool north = m_random.Chance(0.5f);
        SitePlan::Road road;
        road.width = 8.0f;
        glm::vec2 at{WestRoad(), north ? m_streetNorth - kCrossRoad * 0.5f : m_streetSouth + kCrossRoad * 0.5f};
        road.line.push_back(at);
        float heading = 3.14159265f;
        const float bend = north ? 1.0f : -1.0f;
        const float length = m_random.Range(200.0f, 250.0f);
        for (float run = 0.0f; run < length; run += 20.0f)
        {
            heading += m_random.Range(-0.16f, 0.16f) + bend * 0.035f;
            at += glm::vec2(std::cos(heading), std::sin(heading)) * 20.0f;
            road.line.push_back(at);
        }
        m_out.roads.push_back(road);
    }

    void Clusters()
    {
        // Out along the road in, a few buildings on yards of their own off it -- quarters, a works yard, somebody's science --
        // so the outpost is not all one block: a lane in from the road, the buildings either side of it facing onto it.
        const SitePlan::Road& road = m_out.roads.front();
        // (Where the town and its field are, and the way the ship comes in: none of it.)
        glm::vec4 town{m_field.x, m_field.y, m_field.z, m_field.w};
        for (const Placed& placed : m_lots)
        {
            const glm::vec4 rect = placed.lot.Rect();
            town = {std::min(town.x, rect.x), std::min(town.y, rect.y), std::max(town.z, rect.z), std::max(town.w, rect.w)};
        }
        town = {std::min(town.x, WestRoad()), std::min(town.y, m_streetNorth - kCrossRoad - kCrossPavement - 40.0f), town.z,
                std::max(town.w, m_streetSouth + kCrossRoad + kCrossPavement + 40.0f)};
        const int count = m_style.abandoned ? m_random.Int(0, 1) : m_random.Int(1, 1 + m_size);
        std::vector<glm::vec4> made;
        for (int c = 0; c < count; ++c)
        {
            for (int tries = 0; tries < 30; ++tries)
            {
                // A place along the road some way out, to one side of it.
                const size_t k = static_cast<size_t>(m_random.Int(3, std::max(3, static_cast<int>(road.line.size()) - 3)));
                if (k + 1 >= road.line.size())
                {
                    break;
                }
                const glm::vec2 a = road.line[k];
                const glm::vec2 way = glm::normalize(road.line[k + 1] - a);
                const float sideSign = m_random.Chance(0.5f) ? 1.0f : -1.0f;
                const glm::vec2 out{-way.y * sideSign, way.x * sideSign};
                const float wide = std::round(m_random.Range(46.0f, 64.0f));
                const float deep = std::round(m_random.Range(40.0f, 56.0f));
                // Its lane runs out from the road square to it -- along z for a road going along x, along x for one along z.
                const bool laneAlongZ = std::abs(out.y) > std::abs(out.x);
                const glm::vec2 size = laneAlongZ ? glm::vec2(wide, deep) : glm::vec2(deep, wide);
                const glm::vec2 outAxis = laneAlongZ ? glm::vec2(0.0f, out.y > 0.0f ? 1.0f : -1.0f) : glm::vec2(out.x > 0.0f ? 1.0f : -1.0f, 0.0f);
                const glm::vec2 centre = a + outAxis * (road.width * 0.5f + 8.0f + (laneAlongZ ? size.y : size.x) * 0.5f);
                const glm::vec4 rect{centre.x - size.x * 0.5f, centre.y - size.y * 0.5f, centre.x + size.x * 0.5f, centre.y + size.y * 0.5f};
                const auto overlaps = [&](const glm::vec4& other, float grow)
                { return rect.x < other.z + grow && rect.z > other.x - grow && rect.y < other.w + grow && rect.w > other.y - grow; };
                bool fits = !overlaps(town, 30.0f) && std::none_of(made.begin(), made.end(), [&](const glm::vec4& other) { return overlaps(other, 25.0f); });
                // (Not in the way the ship comes in; clear of the road everywhere but where its lane meets it.)
                fits = fits && !(rect.z > -100.0f && rect.x < 100.0f && rect.y < 0.0f);
                for (const glm::vec2 corner : {glm::vec2(rect.x, rect.y), glm::vec2(rect.z, rect.y), glm::vec2(rect.x, rect.w), glm::vec2(rect.z, rect.w), centre})
                {
                    fits = fits && FromRoad(corner) > 4.0f;
                }
                if (!fits)
                {
                    continue;
                }
                made.push_back(rect);
                // The lane, poured, from the road's side to the far end; the buildings either side of it, fronting it.
                const float lane = 12.0f;
                const float laneFrom = laneAlongZ ? (outAxis.y > 0.0f ? rect.y : rect.w) : (outAxis.x > 0.0f ? rect.x : rect.z);
                const float laneTo = laneAlongZ ? (outAxis.y > 0.0f ? rect.w : rect.y) : (outAxis.x > 0.0f ? rect.z : rect.x);
                const float mid = laneAlongZ ? centre.x : centre.y;
                m_lanes.push_back(laneAlongZ ? glm::vec4(mid - lane * 0.5f, std::min(laneFrom, laneTo), mid + lane * 0.5f, std::max(laneFrom, laneTo))
                                             : glm::vec4(std::min(laneFrom, laneTo), mid - lane * 0.5f, std::max(laneFrom, laneTo), mid + lane * 0.5f));
                // What it is: quarters, works, or a station of some kind.
                const float theme = m_random.Unit();
                const auto pick = [&]()
                {
                    const float roll = m_random.Unit();
                    return theme < 0.4f ? (roll < 0.6f ? Use::Habitat : Use::Block)
                           : theme < 0.75f ? (roll < 0.4f ? Use::Tanks : roll < 0.75f ? Use::Shed : Use::Containers)
                                           : (roll < 0.5f ? Use::Block : roll < 0.8f ? Use::Comms : Use::Open);
                };
                const float along0 = std::min(laneFrom, laneTo) + 3.0f;
                const float along1 = std::max(laneFrom, laneTo) - 2.0f;
                const float sideDepth = ((laneAlongZ ? size.x : size.y) - lane) * 0.5f - 2.0f;
                for (const float side : {-1.0f, 1.0f})
                {
                    for (float at = along0; along1 - at >= 14.0f;)
                    {
                        const float length = std::min(std::round(m_random.Range(16.0f, 26.0f)), along1 - at);
                        const float front = mid + side * lane * 0.5f;
                        // Fronting the lane: along z it faces +x from its west side (0) or -x from its east (1); along x, +z from
                        // its north side (2) or -z from its south (3).
                        const int facing = laneAlongZ ? (side < 0.0f ? 0 : 1) : (side < 0.0f ? 2 : 3);
                        const glm::vec2 origin = laneAlongZ ? glm::vec2(front, at) : glm::vec2(at, front);
                        Add({origin, facing, length, std::round(std::min(m_random.Range(14.0f, 24.0f), sideDepth))}, pick());
                        at += length + m_random.Range(4.0f, 8.0f);
                    }
                }
                break;
            }
        }
    }

    void Outskirts()
    {
        // Lamps along the first of the road in (PlanRoad), on alternate sides.
        {
            const SitePlan::Road& road = m_out.roads.front();
            float along = 0.0f;
            int side = 0;
            for (size_t k = 0; k + 1 < road.line.size() && along < 160.0f; ++k, along += 20.0f)
            {
                if (k % 2 == 1 || k == 0)
                {
                    continue;
                }
                const glm::vec2 a = road.line[k];
                const glm::vec2 way = glm::normalize(road.line[k + 1] - a);
                const float sideSign = (side++ % 2 == 0) ? 1.0f : -1.0f;
                const glm::vec2 out{-way.y * sideSign, way.x * sideSign};
                const glm::vec2 foot = a + out * (road.width * 0.5f + 1.5f);
                Stand(foot, 0.3f,
                      [&](PartBuilder& s, PartBuilder& d)
                      {
                          s.Box("road_post", {-0.12f, -0.6f, -0.12f}, {0.12f, 8.2f, 0.12f}, kSteelDark, 0.5f, 0.7f);
                          const glm::vec3 reach{-out.x * 1.6f, 0.0f, -out.y * 1.6f};
                          d.Box("road_arm", glm::min(glm::vec3(0.0f), reach) + glm::vec3(-0.06f, 8.0f, -0.06f), glm::max(glm::vec3(0.0f), reach) + glm::vec3(0.06f, 8.12f, 0.06f),
                                kSteelDark, 0.5f, 0.7f);
                          d.Box("fx_road_lamp", reach + glm::vec3(-0.35f, 7.8f, -0.25f), reach + glm::vec3(0.35f, 7.98f, 0.25f), Glass(kSodium), 0.3f, 0.0f, Glow(3.0f));
                      });
            }
        }

        // The approach lights: a line of them on short posts out ahead of the pad along the way the ship comes in, a bar of them
        // across it every so often.
        for (float z = kField.y - 14.0f; z > -260.0f; z -= 15.0f)
        {
            const bool bar = static_cast<int>((kField.y - 14.0f - z) / 15.0f) % 4 == 3;
            Stand({0.0f, z}, bar ? 6.0f : 0.4f,
                  [&](PartBuilder& s, PartBuilder& d)
                  {
                      const float high = 1.1f;
                      for (const float x : bar ? std::vector<float>{-6.0f, 0.0f, 6.0f} : std::vector<float>{0.0f})
                      {
                          s.Box("approach_post", {x - 0.08f, -0.6f, -0.08f}, {x + 0.08f, high, 0.08f}, kSteelDark, 0.5f, 0.7f);
                      }
                      if (bar)
                      {
                          d.Box("approach_bar", {-6.2f, high, -0.08f}, {6.2f, high + 0.1f, 0.08f}, kSteelDark, 0.5f, 0.7f);
                      }
                      for (float x = bar ? -6.0f : 0.0f; x <= (bar ? 6.01f : 0.0f); x += 2.0f)
                      {
                          d.Box("fx_approach_light", {x - 0.15f, high + 0.1f, -0.15f}, {x + 0.15f, high + 0.35f, 0.15f}, Glass(kFlood), 0.3f, 0.0f, Glow(5.0f));
                      }
                  });
        }

        // A water tower by the road where it leaves the town.
        {
            const SitePlan::Road& road = m_out.roads.front();
            const glm::vec2 a = road.line[std::min<size_t>(2, road.line.size() - 1)];
            for (const float side : {1.0f, -1.0f})
            {
                const glm::vec2 at = a + glm::vec2(0.0f, side * 18.0f);
                if (!ClearOut(at, 4.5f))
                {
                    continue;
                }
                const glm::vec3 shell = m_random.Chance(0.5f) ? kWhitePaint * 0.85f : m_scheme.panel;
                Stand(at, 4.5f,
                      [&](PartBuilder& s, PartBuilder& d)
                      {
                          for (const float x : {-2.4f, 2.4f})
                          {
                              for (const float z : {-2.4f, 2.4f})
                              {
                                  s.Box("tower_leg", {x - 0.2f, -0.8f, z - 0.2f}, {x + 0.2f, 14.0f, z + 0.2f}, kSteelDark, 0.5f, 0.7f);
                              }
                          }
                          for (float y = 3.0f; y < 13.0f; y += 4.0f)
                          {
                              d.Box("tower_brace", {-2.4f, y, -2.5f}, {2.4f, y + 0.18f, -2.3f}, kSteelDark, 0.5f, 0.7f);
                              d.Box("tower_brace", {-2.4f, y, 2.3f}, {2.4f, y + 0.18f, 2.5f}, kSteelDark, 0.5f, 0.7f);
                          }
                          s.Cylinder("tower_tank", {0.0f, 16.5f, 0.0f}, 7.0f, 5.0f, glm::vec3(0.0f), shell, 0.5f, 0.3f);
                          d.Cylinder("tower_cap", {0.0f, 19.3f, 0.0f}, 4.0f, 0.6f, glm::vec3(0.0f), shell * 0.85f, 0.5f, 0.3f);
                      });
                break;
            }
        }

        // A radar on a lattice tower out on the land, its hut at its foot.
        if (glm::vec2 at; FindOut(6.0f, at))
        {
            const float high = std::round(m_random.Range(20.0f, 30.0f));
            Stand(at, 6.0f,
                  [&](PartBuilder& s, PartBuilder& d)
                  {
                      for (const float x : {-1.8f, 1.8f})
                      {
                          for (const float z : {-1.8f, 1.8f})
                          {
                              s.Box("radar_leg", {x - 0.2f, -0.8f, z - 0.2f}, {x + 0.2f, high, z + 0.2f}, kSteel, 0.5f, 0.7f);
                          }
                      }
                      for (float y = 4.0f; y < high - 1.0f; y += 5.0f)
                      {
                          d.Box("radar_brace", {-1.8f, y, -1.95f}, {1.8f, y + 0.18f, -1.65f}, kSteel, 0.5f, 0.7f);
                          d.Box("radar_brace", {-1.8f, y, 1.65f}, {1.8f, y + 0.18f, 1.95f}, kSteel, 0.5f, 0.7f);
                      }
                      s.Box("radar_deck", {-2.4f, high, -2.4f}, {2.4f, high + 0.3f, 2.4f}, kSteelDark, 0.5f, 0.7f);
                      d.Box("radar_mount", {-0.3f, high + 0.3f, -0.3f}, {0.3f, high + 1.6f, 0.3f}, kSteelDark, 0.5f, 0.7f);
                      ModelPart& dish = d.Box("radar_dish", {-3.2f, high + 1.6f, -0.2f}, {3.2f, high + 3.4f, 0.2f}, m_scheme.panel * 1.1f, 0.4f, 0.4f);
                      dish.rotation = {-15.0f, m_random.Range(0.0f, 360.0f), 0.0f};
                      d.Box("fx_radar_light", {-0.15f, high + 3.5f, -0.15f}, {0.15f, high + 3.8f, 0.15f}, Glass({1.0f, 0.12f, 0.08f}), 0.3f, 0.0f, Glow(6.0f));
                      s.Box("radar_hut", {3.2f, -0.8f, -2.0f}, {8.2f, 3.0f, 2.0f}, m_scheme.panel * 0.8f, 0.7f, 0.2f);
                      d.Box("radar_hut_roof", {3.0f, 2.95f, -2.2f}, {8.4f, 3.25f, 2.2f}, m_scheme.trim, 0.6f, 0.4f);
                  });
        }

        // A field of solar panels in rows, each on its legs, tipped to the sky.
        if (glm::vec2 at; FindOut(22.0f, at))
        {
            m_standing.emplace_back(at.x, at.y, 22.0f);
            const glm::vec3 panel = Glass({0.08f, 0.1f, 0.16f});
            for (int row = 0; row < 5; ++row)
            {
                for (int i = 0; i < 6; ++i)
                {
                    const glm::vec2 foot = at + glm::vec2(-12.5f + 5.0f * static_cast<float>(i), -12.0f + 6.0f * static_cast<float>(row));
                    if (m_style.abandoned && m_random.Chance(0.25f))
                    {
                        continue;
                    }
                    SitePlan::Outlying piece;
                    piece.at = foot;
                    piece.reach = 2.2f;
                    ModelAsset solid;
                    ModelAsset dressing;
                    PartBuilder s{solid};
                    PartBuilder d{dressing};
                    s.Box("solar_leg", {-1.9f, -0.6f, -0.1f}, {-1.7f, 1.0f, 0.1f}, kSteelDark, 0.5f, 0.7f);
                    s.Box("solar_leg", {1.7f, -0.6f, -0.1f}, {1.9f, 1.0f, 0.1f}, kSteelDark, 0.5f, 0.7f);
                    ModelPart& face = d.Box("solar_panel", {-2.2f, 1.1f, -1.1f}, {2.2f, 1.18f, 1.1f}, panel, 0.15f, 0.5f);
                    face.rotation = {-25.0f, 0.0f, 0.0f};
                    piece.solid = std::move(solid.parts);
                    piece.dressing = std::move(dressing.parts);
                    m_out.outlying.push_back(std::move(piece));
                }
            }
        }

        // Wind turbines out on the land, one to three (where nobody keeps them, one with a blade gone).
        for (int t = 0; t < m_random.Int(1, 3); ++t)
        {
            glm::vec2 at;
            if (!FindOut(20.0f, at))
            {
                break;
            }
            const float spin = m_random.Range(0.0f, 120.0f);
            const int blades = m_style.abandoned && m_random.Chance(0.5f) ? 2 : 3;
            Stand(at, 2.0f,
                  [&](PartBuilder& s, PartBuilder& d)
                  {
                      s.Cylinder("turbine_tower", {0.0f, 10.0f, 0.0f}, 2.6f, 22.0f, glm::vec3(0.0f), kWhitePaint * 0.9f, 0.5f, 0.2f);
                      s.Cylinder("turbine_tower", {0.0f, 32.5f, 0.0f}, 1.8f, 23.0f, glm::vec3(0.0f), kWhitePaint * 0.9f, 0.5f, 0.2f);
                      d.Box("turbine_nacelle", {-1.4f, 43.5f, -2.5f}, {1.4f, 46.3f, 4.5f}, kWhitePaint * 0.85f, 0.5f, 0.2f);
                      d.Cylinder("turbine_hub", {0.0f, 44.9f, 5.2f}, 1.6f, 1.6f, {90.0f, 0.0f, 0.0f}, kWhitePaint * 0.8f, 0.5f, 0.2f);
                      for (int b = 0; b < blades; ++b)
                      {
                          const float angle = spin + 120.0f * static_cast<float>(b);
                          const float r = glm::radians(angle);
                          const glm::vec3 middle{std::sin(r) * 10.0f, 44.9f + std::cos(r) * 10.0f, 5.6f};
                          ModelPart& blade = d.Box("turbine_blade", middle - glm::vec3(0.45f, 9.2f, 0.12f), middle + glm::vec3(0.45f, 9.2f, 0.12f),
                                                   kWhitePaint * 0.92f, 0.45f, 0.1f);
                          blade.rotation = {0.0f, 0.0f, -angle};
                      }
                      d.Box("fx_turbine_light", {-0.15f, 46.3f, -2.2f}, {0.15f, 46.6f, -1.9f}, Glass({1.0f, 0.12f, 0.08f}), 0.3f, 0.0f, Glow(6.0f));
                  });
        }
    }

    void Fence()
    {
        if (!m_style.standard && !m_random.Chance(0.6f))
        {
            return;
        }
        // Round the field's far sides -- ahead, to starboard past the hangars, behind -- a wire fence on posts: posts every few
        // metres, a rail at the top, the middle and the foot, wire over the top. (Not along the street: the town is the field's
        // other side.) On the levelled ground, out past the yards.
        glm::vec4 field = m_field;
        for (const Placed& placed : m_lots)
        {
            if (!placed.street)
            {
                const glm::vec4 rect = placed.lot.Rect(2.0f);
                field = {std::min(field.x, rect.x), std::min(field.y, rect.y), std::max(field.z, rect.z), std::max(field.w, rect.w)};
            }
        }
        field += glm::vec4(-10.0f, -10.0f, 10.0f, 10.0f);
        const float g = kG;
        const auto run = [&](const glm::vec2& a, const glm::vec2& b)
        {
            const float length = glm::distance(a, b);
            const glm::vec2 along = (b - a) / std::max(length, 1.0e-3f);
            const bool alongX = std::abs(along.x) > 0.5f;
            for (float t = 0.0f; t < length - 0.01f; t += 3.0f)
            {
                if (m_style.abandoned && m_random.Chance(0.15f))
                {
                    continue;
                }
                const float t1 = std::min(t + 3.0f, length);
                if (t1 - t < 0.5f)
                {
                    break; // (the last scrap of a run: its post is the next run's)
                }
                const glm::vec2 p = a + along * t;
                const glm::vec2 q = a + along * t1;
                // (Not over anything poured: a road out through it is a gap in it.)
                const auto poured = [&](const glm::vec2& at)
                {
                    return std::any_of(m_poured.begin(), m_poured.end(), [&](const glm::vec4& r)
                                       { return at.x > r.x - 0.3f && at.x < r.z + 0.3f && at.y > r.y - 0.3f && at.y < r.w + 0.3f; });
                };
                if (poured(p) || poured(q))
                {
                    continue;
                }
                m_solid.Box("fence_post", {p.x - 0.06f, g, p.y - 0.06f}, {p.x + 0.06f, g + 2.6f, p.y + 0.06f}, kSteelDark * 1.3f, 0.6f, 0.5f);
                for (const float y : {0.15f, 1.3f, 2.45f})
                {
                    const glm::vec3 lo = alongX ? glm::vec3(std::min(p.x, q.x) + 0.06f, g + y, p.y - 0.025f) : glm::vec3(p.x - 0.025f, g + y, std::min(p.y, q.y) + 0.06f);
                    const glm::vec3 hi = alongX ? glm::vec3(std::max(p.x, q.x) - 0.06f, g + y + 0.05f, p.y + 0.025f)
                                                : glm::vec3(p.x + 0.025f, g + y + 0.05f, std::max(p.y, q.y) - 0.06f);
                    m_dress.Box("fence_rail", lo, hi, kSteelDark * 1.4f, 0.6f, 0.5f);
                }
            }
        };
        // Ahead, to starboard of the way in and to port of it; to starboard; behind.
        run({-30.0f, field.y}, {kField.x + 1.5f, field.y});
        run({field.z, field.y}, {30.0f, field.y});
        run({field.z, field.y + 3.0f}, {field.z, field.w}); // (its corner post is the one ahead's)
        run({kField.x + 1.5f, field.w}, {field.z, field.w});
    }

    // --- What is about: vehicles, pallets, barrels, bins ------------------------------------------------------------------

    enum class Vehicle : uint8_t
    {
        Truck,   // a cab and a box behind it, three axles
        Loader,  // a forklift: a small body, a mast and forks ahead
        Crawler, // on tracks: a body over two long tracks, a cab on it
        Tanker,  // a fuel bowser: a cab and a tank lying along behind it
        Tug,     // low and wide, a towbar ahead and a cab at the back
    };
    // A vehicle standing at `at` (on the ground), nose along `forward` (one of the four ways), in this colour; rusted and with
    // a wheel off if nobody keeps it.
    void Park(Vehicle kind, const glm::vec3& at, const glm::vec3& forward, const glm::vec3& colour)
    {
        const glm::vec3 f = glm::normalize(forward);
        const glm::vec3 rt = glm::normalize(glm::cross(f, glm::vec3(0.0f, 1.0f, 0.0f)));
        const bool alongX = std::abs(f.x) > 0.5f;
        const glm::vec3 paint = m_style.abandoned ? glm::mix(colour * 0.6f, kRust, 0.35f) : colour;
        const auto place = [&](const glm::vec3& local) { return at + rt * local.x + glm::vec3(0.0f, local.y, 0.0f) + f * local.z; };
        const auto box = [&](PartBuilder& b, const char* stem, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& c, float rough, float metal,
                             float glow = 0.0f) -> ModelPart&
        {
            const glm::vec3 a = place(lo);
            const glm::vec3 d = place(hi);
            return b.Box(stem, glm::min(a, d), glm::max(a, d), c, rough, metal, glow);
        };
        // Its axle across it: along x for a vehicle going along z, along z for one going along x.
        const glm::vec3 axle = alongX ? glm::vec3(90.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 90.0f);
        const auto wheel = [&](float x, float z, float diameter, float wide)
        {
            m_dress.Cylinder("wheel", place({x, diameter * 0.5f, z}), diameter, wide, axle, glm::vec3(0.06f), 0.95f, 0.0f);
        };
        const glm::vec3 glass = Glass(glm::vec3(0.06f, 0.08f, 0.1f));
        const float lamp = Glow(1.2f);
        switch (kind)
        {
        case Vehicle::Truck:
        {
            box(m_solid, "truck_chassis", {-1.05f, 0.55f, -4.2f}, {1.05f, 1.05f, 4.0f}, kSteelDark, 0.6f, 0.6f);
            box(m_solid, "truck_cab", {-1.2f, 1.0f, 2.0f}, {1.2f, 3.5f, 4.2f}, paint, 0.6f, 0.3f);
            box(m_dress, "truck_screen", {-1.05f, 2.3f, 4.2f}, {1.05f, 3.3f, 4.26f}, glass, 0.15f, 0.3f);
            box(m_solid, "truck_box", {-1.25f, 1.05f, -4.4f}, {1.25f, 3.9f, 1.8f}, m_random.Chance(0.5f) ? kWhitePaint * 0.85f : paint * 0.8f, 0.7f, 0.2f);
            for (const float x : {-0.8f, 0.8f})
            {
                box(m_dress, "fx_truck_lamp", {x - 0.25f, 1.3f, 4.2f}, {x + 0.25f, 1.55f, 4.26f}, {1.0f, 0.95f, 0.8f}, 0.3f, 0.0f, lamp);
            }
            for (const float z : {3.2f, -2.2f, -3.5f})
            {
                for (const float x : {-1.0f, 1.0f})
                {
                    if (!(m_style.abandoned && z < 0.0f && x > 0.0f && m_random.Chance(0.4f)))
                    {
                        wheel(x, z, 1.1f, 0.45f);
                    }
                }
            }
            break;
        }
        case Vehicle::Loader:
        {
            box(m_solid, "loader_body", {-0.8f, 0.35f, -1.4f}, {0.8f, 1.5f, 1.0f}, paint, 0.6f, 0.3f);
            box(m_dress, "loader_cage", {-0.75f, 1.5f, -0.6f}, {0.75f, 2.3f, 0.6f}, kSteelDark, 0.5f, 0.6f);
            box(m_dress, "loader_roof", {-0.8f, 2.25f, -0.7f}, {0.8f, 2.35f, 0.7f}, kSteelDark, 0.5f, 0.6f);
            for (const float x : {-0.55f, 0.55f})
            {
                box(m_solid, "loader_mast", {x - 0.08f, 0.2f, 1.0f}, {x + 0.08f, 3.4f, 1.2f}, kSteelDark, 0.5f, 0.7f);
                box(m_dress, "loader_fork", {x - 0.08f, 0.1f, 1.2f}, {x + 0.08f, 0.18f, 2.4f}, kSteelDark, 0.5f, 0.7f);
            }
            for (const float z : {0.6f, -1.0f})
            {
                for (const float x : {-0.75f, 0.75f})
                {
                    wheel(x, z, 0.7f, 0.3f);
                }
            }
            break;
        }
        case Vehicle::Crawler:
        {
            for (const float x : {-1.5f, 1.5f})
            {
                box(m_solid, "crawler_track", {x - 0.45f, 0.0f, -2.6f}, {x + 0.45f, 1.0f, 2.6f}, glm::vec3(0.08f), 0.9f, 0.2f);
            }
            box(m_solid, "crawler_body", {-1.4f, 0.9f, -2.3f}, {1.4f, 2.3f, 2.0f}, paint, 0.6f, 0.3f);
            box(m_solid, "crawler_cab", {-1.1f, 2.3f, 0.2f}, {1.1f, 3.7f, 2.0f}, paint * 0.9f, 0.6f, 0.3f);
            box(m_dress, "crawler_screen", {-0.95f, 2.8f, 2.0f}, {0.95f, 3.5f, 2.06f}, glass, 0.15f, 0.3f);
            box(m_dress, "fx_crawler_lamps", {-0.9f, 3.75f, 1.4f}, {0.9f, 3.95f, 1.7f}, {1.0f, 0.95f, 0.8f}, 0.3f, 0.0f, lamp);
            break;
        }
        case Vehicle::Tanker:
        {
            box(m_solid, "truck_chassis", {-1.05f, 0.55f, -4.4f}, {1.05f, 1.05f, 4.0f}, kSteelDark, 0.6f, 0.6f);
            box(m_solid, "truck_cab", {-1.2f, 1.0f, 2.0f}, {1.2f, 3.4f, 4.2f}, m_scheme.warning, 0.6f, 0.3f);
            box(m_dress, "truck_screen", {-1.05f, 2.3f, 4.2f}, {1.05f, 3.2f, 4.26f}, glass, 0.15f, 0.3f);
            // The tank lying along behind the cab, a walkway along its top, its hose reel at the back.
            const glm::vec3 along = alongX ? glm::vec3(0.0f, 0.0f, 90.0f) : glm::vec3(90.0f, 0.0f, 0.0f);
            m_solid.Cylinder("tanker_tank", place({0.0f, 2.25f, -1.3f}), 2.3f, 5.8f, along, paint, 0.4f, 0.4f);
            box(m_dress, "tanker_walk", {-0.4f, 3.38f, -4.0f}, {0.4f, 3.46f, 1.4f}, kSteelDark, 0.5f, 0.6f);
            box(m_dress, "tanker_reel", {-0.9f, 0.9f, -4.7f}, {0.9f, 1.9f, -4.4f}, kSteelDark, 0.5f, 0.6f);
            box(m_dress, "tanker_band", {-1.17f, 1.6f, -3.9f}, {1.17f, 1.9f, 1.3f}, m_scheme.warning, 0.6f, 0.2f);
            for (const float z : {3.2f, -2.4f, -3.6f})
            {
                for (const float x : {-1.0f, 1.0f})
                {
                    wheel(x, z, 1.1f, 0.45f);
                }
            }
            break;
        }
        case Vehicle::Tug:
        {
            box(m_solid, "tug_body", {-1.3f, 0.3f, -2.2f}, {1.3f, 1.2f, 2.2f}, paint, 0.6f, 0.3f);
            box(m_solid, "tug_cab", {-1.1f, 1.2f, -2.1f}, {1.1f, 2.6f, -0.6f}, paint * 0.85f, 0.6f, 0.3f);
            box(m_dress, "tug_screen", {-0.95f, 1.7f, -0.6f}, {0.95f, 2.4f, -0.55f}, glass, 0.15f, 0.3f);
            box(m_dress, "tug_bar", {-0.12f, 0.45f, 2.2f}, {0.12f, 0.65f, 3.6f}, kSteelDark, 0.5f, 0.7f);
            box(m_dress, "fx_tug_beacon", {-0.15f, 2.6f, -1.5f}, {0.15f, 2.85f, -1.2f}, {1.0f, 0.6f, 0.1f}, 0.3f, 0.0f, lamp);
            for (const float z : {1.5f, -1.5f})
            {
                for (const float x : {-1.15f, 1.15f})
                {
                    wheel(x, z, 0.9f, 0.4f);
                }
            }
            break;
        }
        }
    }
    // What a vehicle at the outpost is painted.
    glm::vec3 VehicleColour()
    {
        static const glm::vec3 kVehiclePaint[] = {{0.80f, 0.62f, 0.12f}, {0.86f, 0.42f, 0.12f}, {0.78f, 0.78f, 0.74f}, {0.30f, 0.36f, 0.42f}, {0.55f, 0.18f, 0.12f}};
        return m_random.Chance(0.4f) ? m_scheme.warning : kVehiclePaint[m_random.Int(0, static_cast<int>(std::size(kVehiclePaint)) - 1)];
    }
    // A stack of pallets, each with a load on it.
    void Pallets(const glm::vec3& at, int high)
    {
        // Each a pallet of strapped cartons (Cargo), one on another; solid as the stack.
        const float load = 0.95f;
        const float each = 0.144f + load + 0.006f;
        for (int i = 0; i < high; ++i)
        {
            Cargo::Place(m_dress.model, Cargo::WrappedPallet(load), at + glm::vec3(0.0f, each * static_cast<float>(i), 0.0f),
                         m_random.Range(-4.0f, 4.0f), "pallet_");
        }
        m_solid.Box("solid_pallets", at + glm::vec3(-0.6f, 0.0f, -0.5f), at + glm::vec3(0.6f, each * static_cast<float>(high), 0.5f), kSteelDark);
    }
    // Drums standing together (Cargo), each solid as itself.
    void Barrels(const glm::vec3& at, int count)
    {
        const glm::vec3 colour = m_random.Chance(0.5f) ? m_scheme.warning * 0.9f : glm::vec3(0.18f, 0.28f, 0.4f);
        for (int i = 0; i < count; ++i)
        {
            const glm::vec3 c = at + glm::vec3(0.65f * static_cast<float>(i % 3), 0.0f, 0.65f * static_cast<float>(i / 3));
            Cargo::Place(m_dress.model, Cargo::Drum(m_style.abandoned ? glm::mix(colour, kRust, 0.5f) : colour), c, m_random.Range(0.0f, 360.0f),
                         "barrel_");
            m_solid.Cylinder("solid_barrel", c + glm::vec3(0.0f, 0.44f, 0.0f), 0.6f, 0.88f, glm::vec3(0.0f), colour, 0.6f, 0.4f);
        }
    }
    // A freight container (Cargo) filling the box lo..hi, its length along the box's longer side and its doors either end:
    // drawn as one, solid as the box.
    void Container(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour)
    {
        const glm::vec3 size = hi - lo;
        const bool alongZ = size.z >= size.x;
        const float yaw = (alongZ ? 0.0f : 90.0f) + (m_random.Chance(0.5f) ? 180.0f : 0.0f);
        Cargo::Place(m_dress.model, Cargo::ShippingContainer(alongZ ? size.z : size.x, colour), {(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f}, yaw,
                     "container_");
        m_solid.Box("solid_container", lo, hi, colour);
    }
    // A wheeled bin, its lid shut.
    void Bin(const glm::vec3& at)
    {
        const glm::vec3 colour = m_random.Chance(0.5f) ? glm::vec3(0.16f, 0.3f, 0.2f) : m_scheme.trim * 1.4f;
        m_solid.Box("bin", at + glm::vec3(-0.9f, 0.15f, -0.65f), at + glm::vec3(0.9f, 1.35f, 0.65f), colour, 0.7f, 0.3f);
        m_dress.Box("bin_lid", at + glm::vec3(-0.95f, 1.35f, -0.7f), at + glm::vec3(0.95f, 1.42f, 0.7f), colour * 0.8f, 0.7f, 0.3f);
    }
    // A red light high on something tall, as anything tall at a landing field has: its housing here, and the light itself, which
    // flashes, for ShipMap to put in it -- if anybody keeps it lit.
    void Beacon(const glm::vec3& at)
    {
        m_dress.Box("beacon_housing", at - glm::vec3(0.22f, 0.0f, 0.22f), at + glm::vec3(0.22f, 0.12f, 0.22f), kSteelDark, 0.5f, 0.6f);
        m_dress.Box("beacon_lens", at + glm::vec3(-0.17f, 0.12f, -0.17f), at + glm::vec3(0.17f, 0.4f, 0.17f), {0.35f, 0.05f, 0.04f}, 0.3f);
        if (Glow(1.0f) > 0.0f)
        {
            m_out.beacons.push_back(at + glm::vec3(0.0f, 0.26f, 0.0f));
        }
    }

    void Parked()
    {
        // Along the kerbs of the streets, nose the way the traffic goes on that side: not on the crossing, nor by a door, nor
        // where a cinematic stands a camera.
        const int kerbs = m_style.abandoned ? m_random.Int(1, 3) : m_random.Int(3, 6) + 2 * m_size;
        // Where each lane already has one: never two in the same place.
        std::vector<glm::vec2> taken;
        for (int i = 0; i < kerbs; ++i)
        {
            const size_t which = static_cast<size_t>(m_random.Int(0, static_cast<int>(m_streets.size()) - 1));
            const Street& street = m_streets[which];
            const bool westSide = m_random.Chance(0.5f);
            const float z = m_random.Range(m_streetNorth + 10.0f, m_streetSouth - 12.0f);
            const float x = westSide ? street.roadWest + 1.6f : street.roadEast - 1.6f;
            if ((which == 0 && std::abs(z - kCross) < 9.0f) || NearCamera(x, z, 9.0f) ||
                std::any_of(street.doors.begin(), street.doors.end(), [z](float door) { return std::abs(door - z) < 6.0f; }) ||
                std::any_of(taken.begin(), taken.end(), [&](const glm::vec2& other) { return std::abs(other.x - x) < 1.0f && std::abs(other.y - z) < 11.0f; }))
            {
                continue;
            }
            taken.push_back({x, z});
            Park(m_random.Chance(0.75f) ? Vehicle::Truck : Vehicle::Crawler, {x, kS, z}, {0.0f, 0.0f, westSide ? -1.0f : 1.0f}, VehicleColour());
        }
    }

    void Service()
    {
        // What looks after a ship on the field, parked out of its way on the apron: a fuel bowser by its starboard side, a tug,
        // the ground power cart, a loader by the cargo staged behind it. Nobody there: one of them left, rusting.
        struct Stand
        {
            Vehicle kind;
            glm::vec3 at;
            glm::vec3 nose;
        };
        const float s = kS;
        const Stand stands[] = {{Vehicle::Tanker, {kPad.z + 4.0f, s, -16.0f}, {0.0f, 0.0f, -1.0f}},
                                {Vehicle::Tug, {kPad.z + 4.0f, s, 22.0f}, {0.0f, 0.0f, 1.0f}},
                                {Vehicle::Loader, {-24.0f, s, kPad.w + 5.0f}, {1.0f, 0.0f, 0.0f}},
                                {Vehicle::Truck, {kPad.x - 4.0f, s, 34.0f}, {0.0f, 0.0f, 1.0f}}};
        int left = m_style.abandoned ? 1 : static_cast<int>(std::size(stands));
        for (const Stand& stand : stands)
        {
            if (left-- <= 0 || NearCamera(stand.at.x, stand.at.z, 9.0f))
            {
                continue;
            }
            Park(stand.kind, stand.at, stand.nose, stand.kind == Vehicle::Tanker ? kWhitePaint * 0.9f : m_scheme.warning);
        }
        // Cargo staged behind the pad for the next ship, on pallets, and a stack of containers.
        if (!m_style.abandoned)
        {
            for (int i = 0; i < m_random.Int(2, 5); ++i)
            {
                Pallets({-14.0f + 3.0f * static_cast<float>(i), s, kPad.w + 5.0f}, m_random.Int(1, 2));
            }
            for (int i = 0; i < m_random.Int(1, 3); ++i)
            {
                const float x = 4.0f + 7.0f * static_cast<float>(i);
                Container({x, s, kPad.w + 4.0f}, {x + 6.06f, s + 2.59f, kPad.w + 6.44f}, kBoxes[m_random.Int(0, 5)]);
            }
        }
    }

    // --- In a lot -----------------------------------------------------------------------------------------------------------

    ModelPart& Box(PartBuilder& b, const Lot& lot, const char* stem, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour, float rough = 0.8f,
                   float metal = 0.0f, float emissive = 0.0f)
    {
        const glm::vec3 a = lot.At(lo.x, lo.y, lo.z);
        const glm::vec3 c = lot.At(hi.x, hi.y, hi.z);
        return b.Box(stem, glm::min(a, c), glm::max(a, c), colour, rough, metal, emissive);
    }
    // A roof over part of a lot (u0, v0 to u1, v1), up to `top`: under it is indoors -- no weather falls there (Layout::roofs).
    void Roof(const Lot& lot, float u0, float v0, float u1, float v1, float top)
    {
        const glm::vec3 a = lot.At(u0, 0.0f, v0);
        const glm::vec3 b = lot.At(u1, 0.0f, v1);
        m_out.roofs.push_back({{std::min(a.x, b.x), std::min(a.z, b.z), std::max(a.x, b.x), std::max(a.z, b.z)}, top});
    }
    // A parapet round the top of a block (u0..u1 by v0..v1), standing proud of its walls by `over`.
    void Parapet(const Lot& lot, float u0, float u1, float v0, float v1, float top, const glm::vec3& colour)
    {
        const glm::vec3 a = lot.At(u0 - 0.15f, top - 0.1f, v0 - 0.15f);
        const glm::vec3 c = lot.At(u1 + 0.15f, top + 0.6f, v1 + 0.15f);
        m_solid.Parapet("building_parapet", glm::min(a, c), glm::max(a, c), 0.35f, colour * 0.82f);
    }
    // A door onto the street, which its lamp posts keep clear of.
    void StreetDoor(const Lot& lot, float u)
    {
        for (Street& street : m_streets)
        {
            if (lot.facing == 0 && std::abs(lot.origin.x - street.facade) < 0.01f)
            {
                street.doors.push_back(lot.At(u, 0.0f, 0.0f).z);
            }
        }
    }
    // A lamp over a door, out from the wall and down.
    void DoorLamp(const Lot& lot, float u, float y, float v)
    {
        m_out.lamps.push_back({lot.At(u, y, v), glm::normalize(lot.Out() + glm::vec3(0.0f, -1.2f, 0.0f)), kSodium, 16.0f, 11.0f, 80.0f, 140.0f});
    }
    void LitSign(const Lot& lot, const std::string& label, float u, float y, float v, float height = 0.8f)
    {
        if (label.empty())
        {
            return;
        }
        const float width = std::max(3.0f, 0.62f * static_cast<float>(label.size()) + 1.4f) * height / 0.8f;
        m_out.signs.push_back({"sign_" + std::to_string(m_out.signs.size()), {label}, lot.At(u, y, v), lot.Yaw(), width, height, Sign::Style::Lit});
    }
    // Lit windows along the front, rows of them from `y0` up to below `top`, so wide and so far apart, leaving out what is between
    // `skipFrom` and `skipTo` along the front on the rows below `skipBelow`.
    void Windows(const Lot& lot, float u0, float u1, float y0, float top, float wide, float apart, float skipFrom, float skipTo, float skipBelow)
    {
        for (float y = y0; y + 1.3f <= top; y += 3.6f)
        {
            for (float u = u0; u + wide <= u1 + 0.01f; u += apart)
            {
                if (y < skipBelow && u + wide > skipFrom && u < skipTo)
                {
                    continue;
                }
                Box(m_dress, lot, "fx_window", {u, y, -0.07f}, {u + wide, y + 1.3f, 0.05f}, Glass(kWindow), 0.3f, 0.0f, Glow(m_random.Chance(0.15f) ? 0.0f : 0.8f));
            }
        }
    }

    void Fill(const Placed& placed)
    {
        switch (placed.use)
        {
        case Use::Operations: Operations(placed); break;
        case Use::Block: Block(placed); break;
        case Use::Shed: Shed(placed); break;
        case Use::Habitat: Habitat(placed); break;
        case Use::Tanks: Tanks(placed); break;
        case Use::Containers: Containers(placed); break;
        case Use::Comms: Comms(placed); break;
        case Use::Open: Open(placed); break;
        case Use::Hangar: Hangar(placed); break;
        }
    }

    void Operations(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        const float H = std::round(m_random.Range(14.0f, 22.0f));
        const float door = placed.door;
        StreetDoor(lot, door);
        Box(m_solid, lot, "ops", {0.0f, s, 0.0f}, {L, s + H, D}, m_scheme.panel * 0.9f, 0.85f);
        Roof(lot, 0.0f, 0.0f, L, D, s + H);
        Parapet(lot, 0.0f, L, 0.0f, D, s + H, m_scheme.panel * 0.9f);
        // Its door and the canopy over it, on posts, lit underneath; what it is over the canopy, and its name high above.
        Box(m_dress, lot, "fx_door", {door - 1.6f, s, -0.08f}, {door + 1.6f, s + 2.8f, 0.05f}, Glass(kWindow * 0.6f), 0.3f, 0.0f, Glow(0.8f));
        Box(m_solid, lot, "ops_canopy", {door - 3.0f, s + 3.4f, -2.4f}, {door + 3.0f, s + 3.7f, 0.05f}, kSteel, 0.6f, 0.5f);
        for (const float u : {door - 2.7f, door + 2.7f})
        {
            Box(m_solid, lot, "ops_canopy_post", {u - 0.15f, s, -2.3f}, {u + 0.15f, s + 3.4f, -2.0f}, kSteelDark, 0.5f, 0.7f);
        }
        m_out.lamps.push_back({lot.At(door, s + 3.2f, -1.2f), {0.0f, -1.0f, 0.0f}, kSodium, 26.0f, 13.0f, 90.0f, 150.0f});
        LitSign(lot, placed.label, door, s + 4.3f, -0.14f);
        if (m_style.mark)
        {
            m_out.signs.push_back({"logo_ops", {}, lot.At(door, s + 5.75f, -0.14f), lot.Yaw(), 7.0f, 1.5f, Sign::Style::Logo});
        }
        const float nameWidth = std::min(L - 2.0f, 14.0f);
        m_out.signs.push_back({"outpost_name", {}, lot.At(door, s + H - 1.9f, -0.12f), lot.Yaw(), nameWidth, 1.6f, Sign::Style::Painted});
        // Windows in rows below the name, none where the door, the canopy and its sign are; a dark plinth along its foot.
        Windows(lot, 1.0f, L - 1.0f, s + 1.1f, s + H - 3.0f, 2.8f, 4.0f, door - 4.2f, door + 4.2f, s + 6.0f);
        Box(m_dress, lot, "ops_plinth", {0.05f, s, -0.12f}, {door - 3.4f, s + 0.8f, 0.05f}, m_scheme.trim, 0.9f);
        Box(m_dress, lot, "ops_plinth", {door + 3.4f, s, -0.12f}, {L - 0.05f, s + 0.8f, 0.05f}, m_scheme.trim, 0.9f);
        // Plant on its roof, each unit its own place on it (not into another); a stack, perhaps a dish.
        std::vector<glm::vec4> units;
        for (int i = 0; i < m_random.Int(2, 5); ++i)
        {
            const float u = m_random.Range(2.0f, L - 6.0f);
            const float v = m_random.Range(2.0f, D - 6.0f);
            const float high = m_random.Range(1.4f, 2.4f);
            if (RoofTaken(units, {u, v, u + 4.0f, v + 3.2f}))
            {
                continue;
            }
            Box(m_solid, lot, "roof_unit", {u, s + H - 0.05f, v}, {u + 4.0f, s + H + high, v + 3.2f}, kSteel, 0.6f, 0.5f);
        }
        m_solid.Cylinder("stack", lot.At(L - 2.5f, s + H + 3.5f, D - 2.5f), 1.2f, 7.0f, glm::vec3(0.0f), kRust, 0.7f, 0.4f);
        Beacon(lot.At(L - 2.5f, s + H + 7.0f, D - 2.5f));
        // An aerial or two on the roof.
        for (int i = 0; i < m_random.Int(1, 3); ++i)
        {
            const glm::vec3 foot = lot.At(m_random.Range(2.0f, L - 2.0f), s + H, m_random.Range(D * 0.5f, D - 1.0f));
            const float tall = m_random.Range(3.0f, 7.0f);
            m_dress.Box("aerial", foot - glm::vec3(0.05f, 0.0f, 0.05f), foot + glm::vec3(0.05f, tall, 0.05f), kSteelDark, 0.5f, 0.7f);
        }
        if (m_random.Chance(0.6f))
        {
            m_solid.Cylinder("dish", lot.At(3.0f, s + H + 2.2f, D - 3.0f), 3.6f, 0.3f, {55.0f, m_random.Range(0.0f, 360.0f), 0.0f}, m_scheme.panel, 0.4f, 0.3f);
        }
    }

    void Block(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        // Two to five floors.
        const float H = 3.6f * static_cast<float>(m_random.Int(2, 5)) + 0.6f;
        const glm::vec3 colour = m_scheme.panel * m_random.Range(0.72f, 0.95f);
        Box(m_solid, lot, "building", {0.0f, s, 0.0f}, {L, s + H, D}, colour, 0.85f);
        Roof(lot, 0.0f, 0.0f, L, D, s + H);
        Parapet(lot, 0.0f, L, 0.0f, D, s + H, colour);
        const float door = std::round(m_random.Range(3.0f, L - 3.0f));
        StreetDoor(lot, door);
        Box(m_dress, lot, "fx_door", {door - 1.5f, s, -0.08f}, {door + 1.5f, s + 2.8f, 0.05f}, Glass(kWindow * 0.6f), 0.3f, 0.0f, Glow(0.8f));
        Box(m_solid, lot, "door_canopy", {door - 2.0f, s + 3.1f, -1.4f}, {door + 2.0f, s + 3.3f, 0.05f}, kSteel, 0.6f, 0.5f);
        DoorLamp(lot, door, s + 3.0f, -0.6f);
        LitSign(lot, placed.label, door, s + 4.2f, -0.12f);
        Windows(lot, 1.0f, L - 1.0f, s + 1.1f, s + H - 0.8f, 2.4f, 3.6f, door - 4.0f, door + 4.0f, s + 6.0f);
        // Something left out beside the door, now and then.
        if (m_random.Chance(0.55f))
        {
            const float u = door + (door > L * 0.5f ? -m_random.Range(3.4f, 5.0f) : m_random.Range(3.4f, 5.0f));
            if (u > 1.2f && u < L - 1.2f)
            {
                const float roll = m_random.Unit();
                // On the pavement where the lot fronts a street, on the slab where it does not.
                const bool street = placed.street;
                const glm::vec3 at = lot.At(u, street ? s + kKerb : s, -1.2f);
                roll < 0.4f ? Pallets(at, m_random.Int(1, 2)) : roll < 0.75f ? Barrels(at, m_random.Int(2, 4)) : Bin(at);
            }
        }
        // Something added on top, now and then.
        if (H < 12.0f && L > 14.0f && D > 12.0f && m_random.Chance(0.5f))
        {
            const float u = m_random.Range(1.5f, L * 0.4f);
            const float v = m_random.Range(1.5f, D * 0.3f);
            const glm::vec3 module = m_scheme.panel * 0.75f;
            Box(m_solid, lot, "module", {u, s + H - 0.05f, v}, {u + L * 0.45f, s + H + 3.4f, v + D * 0.5f}, module, 0.8f, 0.2f);
        }
        std::vector<glm::vec4> units;
        for (int i = 0; i < m_random.Int(0, 3); ++i)
        {
            const float u = m_random.Range(L * 0.55f, L - 4.0f);
            const float v = m_random.Range(D * 0.6f, D - 3.0f);
            const float high = m_random.Range(1.2f, 2.2f);
            if (RoofTaken(units, {u, v, u + 2.6f, v + 2.0f}))
            {
                continue;
            }
            Box(m_solid, lot, "roof_unit", {u, s + H - 0.05f, v}, {u + 2.6f, s + H + high, v + 2.0f}, kSteel, 0.6f, 0.5f);
        }
    }

    void Shed(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        const float H = std::round(m_random.Range(13.0f, 22.0f));
        Box(m_solid, lot, "shed", {0.0f, s, 0.0f}, {L, s + H, D}, m_scheme.panel * 0.85f, 0.75f, 0.2f);
        Roof(lot, 0.0f, 0.0f, L, D, s + H);
        Box(m_dress, lot, "shed_roof", {-0.3f, s + H - 0.05f, -0.3f}, {L + 0.3f, s + H + 0.8f, D + 0.3f}, m_scheme.trim, 0.6f, 0.4f);
        // Its great door, hazard stripes along its foot, floodlights over it; a door for people beside it, lit; what it is over the
        // great door; windows high along the front.
        const float wide = std::min(L * 0.6f, 30.0f);
        const float d0 = std::round((L - wide) * 0.5f + m_random.Range(-1.0f, 1.0f) * (L - wide) * 0.3f);
        const float d1 = d0 + wide;
        const float high = std::round(H * 0.72f);
        Box(m_dress, lot, "shed_door", {d0, s, -0.3f}, {d1, s + high, 0.05f}, kSteel * 0.95f, 0.6f, 0.5f);
        for (float u = d0 + 1.0f; u + 1.6f < d1; u += 4.0f)
        {
            Box(m_dress, lot, "shed_stripe", {u, s + 0.2f, -0.38f}, {u + 1.6f, s + 1.6f, -0.3f}, m_scheme.warning, 0.6f);
        }
        for (const float u : {d0 + 1.2f, d1 - 1.2f})
        {
            m_out.lamps.push_back({lot.At(u, s + high + 0.8f, -0.9f), glm::normalize(lot.Out() + glm::vec3(0.0f, -1.4f, 0.0f)), kFlood, 60.0f, 30.0f, 45.0f,
                                   85.0f});
        }
        const float side = d0 > 5.0f ? d0 - 2.5f : d1 + 2.5f;
        if (side + 1.0f < L)
        {
            Box(m_dress, lot, "fx_door", {side - 1.0f, s, -0.08f}, {side + 1.0f, s + 2.4f, 0.05f}, Glass(kWindow * 0.6f), 0.3f, 0.0f, Glow(0.8f));
            DoorLamp(lot, side, s + 2.9f, -0.6f);
        }
        if (!placed.label.empty())
        {
            // Over the floodlights' hoods, under the roof's edge.
            const float tall = std::min(1.8f, H - 0.4f - (high + 1.2f));
            m_out.signs.push_back({"sign_" + std::to_string(m_out.signs.size()), {placed.label}, lot.At((d0 + d1) * 0.5f, s + high + 1.2f + tall * 0.5f, -0.14f),
                                   lot.Yaw(), std::min(wide, 4.0f + 1.2f * static_cast<float>(placed.label.size())), tall, Sign::Style::Painted});
        }
        Windows(lot, 1.0f, L - 1.0f, s + H - 3.6f, s + H - 0.6f, 3.0f, 5.0f, d0 - 0.5f, d1 + 0.5f, s + H);
        // A red light on each front corner of the roof; and, out in front of the great door, often something to go in or out
        // of it.
        Beacon(lot.At(0.4f, s + H + 0.8f, 0.4f));
        Beacon(lot.At(L - 0.4f, s + H + 0.8f, 0.4f));
        // (Only on a cross street: round the pad that would be where the ship sets down.)
        if ((lot.facing == 2 || lot.facing == 3) && placed.street && m_random.Chance(m_style.abandoned ? 0.3f : 0.7f))
        {
            const float u = m_random.Range(d0 + 3.0f, d1 - 3.0f);
            Park(m_random.Chance(0.5f) ? Vehicle::Crawler : Vehicle::Truck, lot.At(u, s, -6.5f), -lot.Out(), VehicleColour());
        }
    }

    void Hangar(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        const float H = std::round(m_random.Range(18.0f, 26.0f));
        const glm::vec3 skin = m_scheme.panel * m_random.Range(0.78f, 0.92f);
        // Its walls, and a shallow roof over them with its edge trimmed.
        Box(m_solid, lot, "hangar", {0.0f, s, 0.0f}, {L, s + H, D}, skin, 0.75f, 0.2f);
        Roof(lot, 0.0f, 0.0f, L, D, s + H);
        Box(m_dress, lot, "hangar_roof", {-0.4f, s + H - 0.05f, -0.4f}, {L + 0.4f, s + H + 1.2f, D + 0.4f}, m_scheme.trim, 0.6f, 0.4f);
        // Its doors: all of the front but its corners, as high as a ship, in leaves that slide -- one of them back a little, the
        // dark inside showing (nobody there: all shut) -- a beam over them, hazard paint along their foot.
        const float d0 = 2.5f;
        const float d1 = L - 2.5f;
        const float high = H - 3.5f;
        Box(m_dress, lot, "hangar_header", {d0 - 0.4f, s + high, -0.4f}, {d1 + 0.4f, s + high + 1.0f, -0.02f}, m_scheme.trim * 1.2f, 0.6f, 0.4f);
        const int leaves = std::max(4, static_cast<int>((d1 - d0) / 8.0f));
        const float leaf = (d1 - d0) / static_cast<float>(leaves);
        const int open = m_style.abandoned ? -1 : m_random.Int(0, leaves - 1);
        for (int i = 0; i < leaves; ++i)
        {
            const float u0 = d0 + leaf * static_cast<float>(i);
            const float u1 = u0 + leaf;
            if (i == open)
            {
                Box(m_dress, lot, "hangar_inside", {u0, s, -0.03f}, {u1, s + high, -0.01f}, glm::vec3(0.02f), 0.95f);
                continue;
            }
            Box(m_dress, lot, "hangar_leaf", {u0 + 0.05f, s, -0.3f}, {u1 - 0.05f, s + high, -0.04f}, kSteel * 0.95f, 0.6f, 0.5f);
            for (float u = u0 + 0.6f; u + 1.6f < u1 - 0.4f; u += 3.6f)
            {
                Box(m_dress, lot, "shed_stripe", {u, s + 0.2f, -0.38f}, {u + 1.6f, s + 1.6f, -0.3f}, m_scheme.warning, 0.6f);
            }
        }
        // Floodlights over the doors, aimed out across the apron and down; a red light on each front corner of the roof.
        for (int i = 0; i < 3; ++i)
        {
            const float u = d0 + (d1 - d0) * (0.15f + 0.35f * static_cast<float>(i));
            m_out.lamps.push_back({lot.At(u, s + high + 1.6f, -1.0f), glm::normalize(lot.Out() + glm::vec3(0.0f, -1.1f, 0.0f)), kFlood, 160.0f, 40.0f, 50.0f,
                                   95.0f});
        }
        Beacon(lot.At(0.4f, s + H + 1.2f, 0.4f));
        Beacon(lot.At(L - 0.4f, s + H + 1.2f, 0.4f));
        // A door for people in the corner, lit; what it is over the doors.
        Box(m_dress, lot, "fx_door", {0.7f, s, -0.08f}, {1.8f, s + 2.3f, 0.05f}, Glass(kWindow * 0.6f), 0.3f, 0.0f, Glow(0.8f));
        DoorLamp(lot, 1.25f, s + 2.8f, -0.6f);
        if (!placed.label.empty())
        {
            const float tall = std::min(1.8f, H - high - 1.6f);
            m_out.signs.push_back({"sign_" + std::to_string(m_out.signs.size()), {placed.label}, lot.At((d0 + d1) * 0.5f, s + high + 1.2f + tall * 0.5f, -0.14f),
                                   lot.Yaw(), std::min(d1 - d0, 4.0f + 1.4f * static_cast<float>(placed.label.size())), tall, Sign::Style::Painted});
        }
        // Plant on its roof.
        std::vector<glm::vec4> units;
        for (int i = 0; i < m_random.Int(2, 5); ++i)
        {
            const float u = m_random.Range(3.0f, L - 7.0f);
            const float v = m_random.Range(4.0f, D - 6.0f);
            if (RoofTaken(units, {u, v, u + 4.0f, v + 3.0f}))
            {
                continue;
            }
            Box(m_solid, lot, "roof_unit", {u, s + H + 1.1f, v}, {u + 4.0f, s + H + 2.8f, v + 3.0f}, kSteel, 0.6f, 0.5f);
        }
    }

    void Habitat(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        // Modules side by side, lying along the front on legs, a corridor through them; the way in by steps to a porch on the
        // front one.
        const int count = std::clamp(static_cast<int>((D - 1.0f) / 6.0f), 1, 3);
        const float radius = 2.6f;
        const float centreY = s + 1.5f + radius;
        const glm::vec3 shell = m_scheme.panel * 1.1f;
        for (int i = 0; i < count; ++i)
        {
            const float v = 3.2f + 6.0f * static_cast<float>(i);
            m_solid.Cylinder("habitat", lot.At(L * 0.5f, centreY, v), radius * 2.0f, L - 2.0f, lot.AlongFront(), shell, 0.6f, 0.2f);
            Roof(lot, 1.0f, v - radius, L - 1.0f, v + radius, centreY + radius);
            for (float u = 2.5f; u < L - 2.0f; u += 4.0f)
            {
                for (const float side : {-1.7f, 1.7f})
                {
                    Box(m_solid, lot, "habitat_leg", {u - 0.15f, s, v + side - 0.15f}, {u + 0.15f, s + 2.4f, v + side + 0.15f}, kSteelDark, 0.5f, 0.7f);
                }
            }
        }
        const float door = std::round(m_random.Range(4.0f, L - 4.0f));
        StreetDoor(lot, door);
        if (count > 1)
        {
            Box(m_solid, lot, "habitat_corridor", {door - 1.2f, s + 2.8f, 3.2f}, {door + 1.2f, s + 5.4f, 3.2f + 6.0f * static_cast<float>(count - 1)}, shell * 0.85f,
                0.7f, 0.2f);
        }
        Box(m_solid, lot, "habitat_porch", {door - 1.4f, s + 1.6f, -0.6f}, {door + 1.4f, s + 4.8f, 2.0f}, shell * 0.8f, 0.7f, 0.2f);
        Box(m_dress, lot, "fx_door", {door - 0.8f, s + 1.7f, -0.68f}, {door + 0.8f, s + 4.0f, -0.55f}, Glass(kWindow * 0.6f), 0.3f, 0.0f, Glow(0.8f));
        for (int step = 0; step < 4; ++step)
        {
            const float v0 = -0.6f - 0.45f * static_cast<float>(4 - step);
            Box(m_solid, lot, "habitat_step", {door - 1.0f, s, v0}, {door + 1.0f, s + 0.4f * static_cast<float>(step + 1), v0 + 0.45f}, kSteel, 0.6f, 0.6f);
        }
        m_out.lamps.push_back({lot.At(door, s + 5.05f, -0.1f), glm::normalize(lot.Out() + glm::vec3(0.0f, -1.2f, 0.0f)), kSodium, 16.0f, 11.0f, 80.0f, 140.0f});
        LitSign(lot, placed.label, door, s + 4.4f, -0.66f, 0.55f);
        // Its portholes, lit, along the front module.
        for (float u = 2.0f; u < L - 2.5f; u += 3.0f)
        {
            if (std::abs(u + 0.4f - door) < 2.2f)
            {
                continue;
            }
            Box(m_dress, lot, "fx_window", {u, centreY - 0.35f, 0.52f}, {u + 0.8f, centreY + 0.35f, 0.7f}, Glass(kWindow), 0.3f, 0.0f, Glow(0.8f));
        }
    }

    void Tanks(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        // In a bund on a plinth: tanks standing in a row, pipes along the front on supports, a floodlight on a pole.
        Box(m_solid, lot, "tank_plinth", {0.0f, s, 0.0f}, {L, s + 0.4f, D}, m_concrete * 0.7f, 0.9f);
        {
            // Inside the plinth's edges, so its sides and the bund's are never one on the other.
            const glm::vec3 a = lot.At(0.15f, s + 0.35f, 0.15f);
            const glm::vec3 c = lot.At(L - 0.15f, s + 1.1f, D - 0.15f);
            m_solid.Parapet("tank_bund", glm::min(a, c), glm::max(a, c), 0.3f, m_concrete * 0.8f);
        }
        const float diameter = std::min(D - 4.0f, m_random.Range(6.0f, 10.0f));
        const int count = std::max(1, static_cast<int>((L - 2.0f) / (diameter + 2.0f)));
        const float high = m_random.Range(8.0f, 14.0f);
        const glm::vec3 shell = m_random.Chance(0.5f) ? m_scheme.panel * 1.1f : kWhitePaint * 0.85f;
        for (int i = 0; i < count; ++i)
        {
            const float u = L / static_cast<float>(count) * (static_cast<float>(i) + 0.5f);
            m_solid.Cylinder("tank", lot.At(u, s + 0.4f + high * 0.5f, D * 0.5f + 0.6f), diameter, high, glm::vec3(0.0f), shell, 0.5f, 0.2f);
        }
        m_dress.Cylinder("pipe", lot.At(L * 0.5f, s + 2.6f, 1.2f), 0.5f, L - 1.0f, lot.AlongFront(), kSteel * 1.1f, 0.4f, 0.8f);
        m_dress.Cylinder("pipe", lot.At(L * 0.5f, s + 3.2f, 1.2f), 0.35f, L - 1.0f, lot.AlongFront(), kRust, 0.6f, 0.5f);
        for (float u = 1.5f; u < L - 1.0f; u += 6.0f)
        {
            Box(m_solid, lot, "pipe_support", {u - 0.15f, s + 0.4f, 0.9f}, {u + 0.15f, s + 2.4f, 1.5f}, kSteelDark, 0.5f, 0.7f);
        }
        Box(m_solid, lot, "lamp_pole", {0.55f, s, -1.75f}, {1.05f, s + 9.0f, -1.25f}, kSteelDark, 0.5f, 0.7f);
        const glm::vec3 at = lot.At(0.8f, s + 8.8f, -1.5f);
        m_out.lamps.push_back({at, glm::normalize(lot.At(L * 0.5f, s, D * 0.5f) - at), kFlood, 140.0f, 40.0f, 60.0f, 110.0f});
        if (!placed.label.empty())
        {
            m_out.signs.push_back({"sign_" + std::to_string(m_out.signs.size()), {placed.label}, lot.At(L * 0.5f, s + 0.75f, 0.09f), lot.Yaw(), 3.2f, 0.6f,
                                   Sign::Style::Painted});
        }
    }

    void Containers(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        // Stacks in rows, their long sides along the lot's depth where it is deep enough; a gantry crane over a long yard; a
        // floodlight mast at its corner.
        const bool crane = L >= 30.0f && D >= 16.0f && m_random.Chance(0.5f);
        const float margin = crane ? 2.5f : 1.0f;
        const bool deep = D >= 15.0f;
        const glm::vec2 box = deep ? glm::vec2(2.44f, 12.19f) : glm::vec2(12.19f, 2.44f); // along u, along v
        const glm::vec2 step = deep ? glm::vec2(3.2f, 14.0f) : glm::vec2(13.4f, 3.2f);
        for (float u = margin; u + box.x <= L - margin; u += step.x)
        {
            for (float v = 1.5f; v + box.y <= D - 1.0f; v += step.y)
            {
                if (m_random.Chance(0.2f))
                {
                    continue;
                }
                const int high = m_random.Int(1, 3);
                for (int level = 0; level < high; ++level)
                {
                    const float y = s + 2.62f * static_cast<float>(level);
                    const glm::vec3 a = lot.At(u, y, v);
                    const glm::vec3 c = lot.At(u + box.x, y + 2.59f, v + box.y);
                    Container(glm::min(a, c), glm::max(a, c), kBoxes[m_random.Int(0, static_cast<int>(std::size(kBoxes)) - 1)]);
                }
            }
        }
        if (crane)
        {
            const glm::vec3 orange = m_scheme.warning * 0.95f;
            for (const float u : {0.8f, L - 0.8f})
            {
                for (const float v : {0.6f, D - 0.6f})
                {
                    Box(m_solid, lot, "crane_leg", {u - 0.5f, s, v - 0.5f}, {u + 0.5f, s + 12.0f, v + 0.5f}, orange, 0.6f, 0.4f);
                }
                Box(m_dress, lot, "crane_beam", {u - 0.5f, s + 12.0f, 0.1f}, {u + 0.5f, s + 13.0f, D - 0.1f}, orange, 0.6f, 0.4f);
            }
            const float v = m_random.Range(3.0f, D - 3.0f);
            Box(m_dress, lot, "crane_bridge", {1.3f, s + 13.0f, v - 0.75f}, {L - 1.3f, s + 14.0f, v + 0.75f}, orange, 0.6f, 0.4f);
            const float u = m_random.Range(4.0f, L - 4.0f);
            Box(m_dress, lot, "crane_hoist", {u - 0.7f, s + 8.0f, v - 0.35f}, {u + 0.7f, s + 13.0f, v + 0.35f}, kSteelDark, 0.5f, 0.6f);
        }
        // Something to move them about, between the stacks and the front.
        if (m_random.Chance(m_style.abandoned ? 0.3f : 0.8f))
        {
            // Just inside the yard's front, clear of the pavement's lamp posts and of a crane's legs.
            Park(Vehicle::Loader, lot.At(m_random.Range(std::min(4.0f, L * 0.5f), std::max(L - 4.0f, L * 0.5f)), s, 0.0f), lot.Out().x != 0.0f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f),
                 m_scheme.warning);
        }
        Box(m_solid, lot, "yard_mast", {-1.85f, s, 0.75f}, {-1.35f, s + 14.0f, 1.25f}, kSteelDark, 0.5f, 0.7f);
        Beacon(lot.At(-1.6f, s + 14.0f, 1.0f));
        const glm::vec3 at = lot.At(-1.6f, s + 13.8f, 1.0f);
        m_out.lamps.push_back({at, glm::normalize(lot.At(L * 0.5f, s, D * 0.5f) - at), kFlood, 220.0f, 45.0f, 70.0f, 120.0f});
    }

    void Comms(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        // A lattice mast, four legs braced every few metres, a dish high on it and a red light on top; the hut at its foot.
        const float cu = L * 0.5f - 2.0f;
        const float cv = D * 0.5f;
        const float high = std::round(m_random.Range(34.0f, 52.0f));
        for (const float du : {-3.0f, 3.0f})
        {
            for (const float dv : {-3.0f, 3.0f})
            {
                Box(m_solid, lot, "mast_leg", {cu + du - 0.3f, s, cv + dv - 0.3f}, {cu + du + 0.3f, s + high, cv + dv + 0.3f}, kSteel, 0.5f, 0.7f);
            }
        }
        for (float y = s + 5.0f; y < s + high - 1.0f; y += 5.0f)
        {
            for (const float dv : {-3.0f, 3.0f})
            {
                Box(m_dress, lot, "mast_brace", {cu - 3.0f, y, cv + dv - 0.2f}, {cu + 3.0f, y + 0.25f, cv + dv + 0.2f}, kSteel, 0.5f, 0.7f);
            }
            for (const float du : {-3.0f, 3.0f})
            {
                Box(m_dress, lot, "mast_brace", {cu + du - 0.2f, y + 0.3f, cv - 3.0f}, {cu + du + 0.2f, y + 0.55f, cv + 3.0f}, kSteel, 0.5f, 0.7f);
            }
        }
        m_dress.Cylinder("mast_dish", lot.At(cu, s + high - 3.0f, cv - 3.8f), 6.0f, 0.4f, {60.0f, lot.Yaw(), 0.0f}, m_scheme.panel, 0.4f, 0.3f);
        Box(m_dress, lot, "fx_mast_light", {cu - 0.3f, s + high, cv - 0.3f}, {cu + 0.3f, s + high + 0.6f, cv + 0.3f}, {1.0f, 0.12f, 0.08f}, 0.3f, 0.0f, 6.0f);
        // The hut, to one side of the mast's legs.
        const float h0 = cu + 4.2f;
        const float h1 = std::min(L - 0.5f, h0 + 6.0f);
        Box(m_solid, lot, "comms_hut", {h0, s, 1.0f}, {h1, s + 3.2f, 5.0f}, m_scheme.panel * 0.8f, 0.7f, 0.2f);
        Roof(lot, h0, 1.0f, h1, 5.0f, s + 3.2f);
        Box(m_dress, lot, "comms_hut_roof", {h0 - 0.2f, s + 3.15f, 0.8f}, {h1 + 0.2f, s + 3.45f, 5.2f}, m_scheme.trim, 0.6f, 0.4f);
        const float door = (h0 + h1) * 0.5f;
        StreetDoor(lot, door);
        // Its lamp to one side of the door, what it is over the door.
        Box(m_dress, lot, "fx_door", {door - 0.6f, s, 0.92f}, {door + 0.6f, s + 2.2f, 1.05f}, Glass(kWindow * 0.6f), 0.3f, 0.0f, Glow(0.8f));
        DoorLamp(lot, door - 2.2f, s + 2.75f, 0.4f);
        LitSign(lot, placed.label, door, s + 2.65f, 0.94f, 0.5f);
    }

    void Open(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        // Open ground is where things are left: a vehicle in the middle of it, along its front, pallets and drums at its front
        // corners -- and then no crates, which would be where the vehicle is.
        if (m_random.Chance(0.6f))
        {
            Pallets(lot.At(2.0f, s, 2.0f), m_random.Int(1, 3));
        }
        if (m_random.Chance(0.5f))
        {
            Barrels(lot.At(lot.length - 3.0f, s, 1.5f), m_random.Int(2, 6));
        }
        if (lot.length > 17.0f && lot.depth > 14.0f && m_random.Chance(0.75f))
        {
            const Vehicle kind = m_random.Chance(0.5f) ? Vehicle::Truck : m_random.Chance(0.5f) ? Vehicle::Crawler : Vehicle::Loader;
            Park(kind, lot.At(lot.length * 0.5f, s, lot.depth * 0.65f), lot.At(1.0f, 0.0f, 0.0f) - lot.At(0.0f, 0.0f, 0.0f), VehicleColour());
            return;
        }
        // A few crates, stacked here and there, each stack in a place of its own.
        const int across = std::max(1, static_cast<int>((lot.length - 2.0f) / 3.0f));
        const int back = std::max(1, static_cast<int>((lot.depth - 2.0f) / 3.0f));
        std::vector<int> taken;
        const int stacks = m_random.Int(2, 6);
        for (int i = 0; i < stacks; ++i)
        {
            const int cell = m_random.Int(0, across * back - 1);
            if (std::find(taken.begin(), taken.end(), cell) != taken.end())
            {
                continue;
            }
            taken.push_back(cell);
            const float size = m_random.Range(1.0f, 1.6f);
            const float u = 1.0f + 3.0f * static_cast<float>(cell % across) + m_random.Range(0.0f, 2.9f - size * 1.5f);
            const float v = 1.0f + 3.0f * static_cast<float>(cell / across) + m_random.Range(0.0f, 2.9f - size);
            const int high = m_random.Int(1, 3);
            for (int level = 0; level < high; ++level)
            {
                const float y = s + (size + 0.02f) * static_cast<float>(level);
                Box(m_solid, lot, "crate", {u, y, v}, {u + size * 1.5f, y + size, v + size}, kBoxes[m_random.Int(0, 5)] * 0.8f, 0.8f, 0.1f);
            }
        }
    }

    // Glass and light, as its owner keeps them: nobody there, dark but for the odd one left on.
    glm::vec3 Glass(const glm::vec3& colour) const { return m_style.abandoned ? colour * 0.18f : colour; }
    float Glow(float glow) { return m_style.abandoned ? (m_random.Chance(0.06f) ? glow : 0.0f) : glow; }

    Random m_random;
    Layout& m_out;
    PartBuilder m_solid;
    PartBuilder m_dress;
    glm::vec3 m_ground;
    glm::vec3 m_rock;
    OutpostStyle m_style;
    uint32_t m_seed = 0;
    Scheme m_scheme{};
    glm::vec3 m_concrete = kConcrete;
    float m_streetNorth = -100.0f;
    float m_streetSouth = 80.0f;
    glm::vec2 m_lo{0.0f};
    glm::vec2 m_hi{0.0f};
    bool m_sternWall = false;
    bool m_comms = false;
    // How big an outpost it is: how many streets behind the first (0 to 2), and longer the bigger.
    int m_size = 0;
    std::vector<Placed> m_lots;
    std::vector<std::string> m_labels;
    std::vector<Street> m_streets;
    // The landing pads (x0, z0, x1, z1), the ship's first; the field round them all, the apron's edge; the first pad's number.
    std::vector<glm::vec4> m_pads;
    glm::vec4 m_field = kField;
    int m_padNumber = 1;
    // What is poured into the levelled ground so far (Pour), as (x0, z0, x1, z1).
    std::vector<glm::vec4> m_poured;
    // What stands out on the land so far (Stand), as its foot (x, z) and how far it reaches.
    std::vector<glm::vec3> m_standing;
    // The lanes in from the road to the buildings out along it (Clusters), as (x0, z0, x1, z1).
    std::vector<glm::vec4> m_lanes;
};

} // namespace

Layout Generate(uint32_t seed, const glm::vec3& ground, const glm::vec3& rock, const OutpostStyle& style)
{
    Layout layout;
    {
        Planner planner(seed, layout, ground, rock, style);
        planner.Run();
    }
    return layout;
}

} // namespace Outpost

} // namespace pred
