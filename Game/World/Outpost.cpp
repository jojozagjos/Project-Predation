#include "Game/World/Outpost.h"

#include "Game/World/PartBuilder.h"
#include "Game/World/ShipMap.h"

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

// The street along the pad's port side, as at Kestrel: the pavement on the pad's side of it, the road, the far pavement and
// the fronts of the buildings along it. The walkway from the stair crosses it to the operations block's door.
constexpr float kPavementEast = -28.0f;
constexpr float kPavementEdge = -25.0f;
constexpr float kPavementWest = -45.0f;
constexpr float kFacade = -49.0f;
constexpr float kCross = (KestrelStation::kCrossFrom + KestrelStation::kCrossTo) * 0.5f;
// The yards round the pad, kept out of the way of the cinematics' cameras (on the street, on the pad's port side, and over its
// back to starboard) and of the ship coming in low over the pad's front: to starboard, between the pad's front and short of its
// back; ahead and off to starboard; behind.
constexpr float kStarboardFront = 24.0f;
constexpr float kStarboardFrom = -40.0f;
constexpr float kStarboardTo = 28.0f;
constexpr float kAheadFront = -46.0f;
constexpr float kAheadFrom = 26.0f;
constexpr float kBehindFront = 48.0f;
constexpr float kBehindFrom = -22.0f;
constexpr float kBehindTo = 20.0f;
// The points the slab always reaches: those cameras' places.
const glm::vec2 kKeepOnSlab[] = {{-40.0f, -22.0f}, {-19.0f, -18.0f}, {-44.0f, -78.0f}, {18.0f, 42.0f}};

// Concrete, asphalt, steel, lamps and windows, as Kestrel's.
const glm::vec3 kConcrete{0.42f, 0.41f, 0.38f};
const glm::vec3 kAsphalt{0.12f, 0.12f, 0.13f};
const glm::vec3 kSteel{0.36f, 0.38f, 0.40f};
const glm::vec3 kSteelDark{0.17f, 0.18f, 0.19f};
const glm::vec3 kRust{0.42f, 0.24f, 0.14f};
const glm::vec3 kWhitePaint{0.80f, 0.80f, 0.76f};
const glm::vec3 kGrime{0.16f, 0.15f, 0.14f};
const glm::vec3 kSodium{1.0f, 0.72f, 0.36f};
const glm::vec3 kFlood{0.92f, 0.95f, 1.0f};
const glm::vec3 kWindow{1.0f, 0.86f, 0.62f};

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
};

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
        m_streetNorth = -std::round(m_random.Range(80.0f, 130.0f));
        m_streetSouth = std::round(m_random.Range(50.0f, 105.0f));
        PlanLots();
        Ground();
        Pad();
        for (const Placed& placed : m_lots)
        {
            Fill(placed);
        }
        // After the buildings: its lamp posts stand clear of their doors.
        Street();
        Fence();
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

    void PlanLots()
    {
        // The operations block across the street from the stair, the walkway's crossing at its door.
        const float mainLength = std::round(m_random.Range(24.0f, 34.0f));
        const float before = std::round(m_random.Range(9.0f, mainLength - 9.0f));
        const float mainFrom = kCross - before;
        Placed main{Lot{{kFacade, mainFrom}, 0, mainLength, std::round(m_random.Range(18.0f, 28.0f))}, Use::Operations, m_style.operations, before};
        m_lots.push_back(main);
        // Along the street both ways from it, with alleys between, as far as the street runs.
        float cursor = mainFrom - m_random.Range(4.0f, 9.0f);
        while (cursor - (m_streetNorth + 4.0f) >= 12.0f)
        {
            const float length = std::min(std::round(m_random.Range(14.0f, 30.0f)), cursor - (m_streetNorth + 4.0f));
            AlongStreet({{kFacade, cursor - length}, 0, length, std::round(m_random.Range(14.0f, 26.0f))});
            cursor -= length + m_random.Range(4.0f, 9.0f);
        }
        cursor = mainFrom + mainLength + m_random.Range(4.0f, 9.0f);
        while ((m_streetSouth - 4.0f) - cursor >= 12.0f)
        {
            const float length = std::min(std::round(m_random.Range(14.0f, 30.0f)), (m_streetSouth - 4.0f) - cursor);
            AlongStreet({{kFacade, cursor}, 0, length, std::round(m_random.Range(14.0f, 26.0f))});
            cursor += length + m_random.Range(4.0f, 9.0f);
        }
        // To starboard: a shed whose side is the pad's wall that way, or a yard behind a blast wall.
        {
            const float roll = m_random.Unit();
            const Use use = roll < 0.4f ? Use::Shed : roll < 0.6f ? Use::Tanks : roll < 0.85f ? Use::Containers : Use::Open;
            // CIRRA's standard outpost has its pad walled behind and to starboard, as Kestrel's is.
            m_starboardWall = use != Use::Shed && (m_style.standard || m_random.Chance(0.6f));
            // Behind a wall, room between it and the yard for a lamp pole.
            const float front = m_starboardWall ? kStarboardFront + 3.5f : kStarboardFront;
            const float length = std::round(m_random.Range(40.0f, kStarboardTo - kStarboardFrom));
            Add({{front, kStarboardFrom}, 1, length, std::round(m_random.Range(26.0f, 44.0f))}, use);
        }
        // Ahead, off to starboard, and behind.
        {
            const float roll = m_random.Unit();
            const Use use = roll < 0.35f ? Use::Containers : roll < 0.55f ? Use::Tanks : roll < 0.7f ? Use::Block : roll < 0.85f ? Use::Shed : Use::Open;
            const float length = std::round(m_random.Range(24.0f, 50.0f));
            Add({{kAheadFrom, kAheadFront}, 2, length, std::round(m_random.Range(20.0f, 38.0f))}, use);
        }
        {
            const float roll = m_random.Unit();
            const Use use = roll < 0.25f ? Use::Block : roll < 0.45f ? Use::Habitat : roll < 0.6f ? Use::Tanks : roll < 0.8f ? (m_comms ? Use::Containers : Use::Comms)
                                                                                                                              : Use::Open;
            Add({{kBehindFrom, kBehindFront}, 3, kBehindTo - kBehindFrom, std::round(m_random.Range(20.0f, 34.0f))}, use);
        }
        m_sternWall = m_starboardWall || m_style.standard || m_random.Chance(0.45f);
    }

    void AlongStreet(const Lot& lot)
    {
        // As often as its owner has each kind (OutpostStyle::uses), or a mix of everything.
        static const std::pair<const char*, Use> kinds[] = {{"block", Use::Block},           {"shed", Use::Shed},   {"habitat", Use::Habitat},
                                                            {"tanks", Use::Tanks},           {"containers", Use::Containers},
                                                            {"comms", Use::Comms},           {"open", Use::Open}};
        static const float fallback[] = {30.0f, 15.0f, 15.0f, 10.0f, 15.0f, 8.0f, 7.0f};
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
        Add(lot, use);
    }

    void Add(const Lot& lot, Use use)
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
        Placed placed{lot, use, Label(use)};
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
        case Use::Shed: return pick(m_style.sheds);
        case Use::Habitat: return pick(m_style.habitats);
        case Use::Tanks: return m_style.tanks.empty() ? std::string() : m_style.tanks[static_cast<size_t>(m_random.Int(0, static_cast<int>(m_style.tanks.size()) - 1))];
        case Use::Comms: return m_style.comms;
        default: return {};
        }
    }

    // --- The ground, the slab, the pad, the street --------------------------------------------------------------------------

    void Ground()
    {
        // How far the outpost reaches: everything in it, the street, and the cinematics' cameras' places, in twenty-metre squares.
        glm::vec2 lo{kFacade - 6.0f, m_streetNorth - 6.0f};
        glm::vec2 hi{40.0f, m_streetSouth + 6.0f};
        for (const Placed& placed : m_lots)
        {
            const glm::vec4 rect = placed.lot.Rect(6.0f);
            lo = glm::min(lo, glm::vec2(rect.x, rect.y));
            hi = glm::max(hi, glm::vec2(rect.z, rect.w));
        }
        for (const glm::vec2 keep : kKeepOnSlab)
        {
            lo = glm::min(lo, keep - glm::vec2(6.0f));
            hi = glm::max(hi, keep + glm::vec2(6.0f));
        }
        m_lo = glm::floor(lo / 20.0f) * 20.0f;
        m_hi = glm::ceil(hi / 20.0f) * 20.0f;

        // The world's ground as far as the haze, low hills at the edge of what can be seen, rock about beyond the slab.
        m_solid.Box("ground", {-440.0f, kG - 1.0f, -440.0f}, {440.0f, kG, 440.0f}, m_ground, 0.95f);
        const float turn = m_random.Range(0.0f, 6.28318f);
        const int hills = m_random.Int(16, 26);
        for (int i = 0; i < hills; ++i)
        {
            const float angle = turn + 6.28318f * static_cast<float>(i) / static_cast<float>(hills) + m_random.Range(-0.1f, 0.1f);
            const float reach = m_random.Range(320.0f, 400.0f);
            const glm::vec3 at{std::cos(angle) * reach, kG, std::sin(angle) * reach};
            const float high = m_random.Range(10.0f, 42.0f);
            m_solid.Turned("hill", at, {m_random.Range(130.0f, 200.0f), high * 2.0f, m_random.Range(40.0f, 64.0f)}, -angle * kDegrees + 90.0f,
                           glm::mix(m_ground, m_rock, m_random.Range(0.3f, 0.6f)), m_random.Range(12.0f, 22.0f), 0.95f);
        }
        const float clear = std::max(glm::length(m_lo), glm::length(m_hi)) + 20.0f;
        const int rocks = m_random.Int(10, 20);
        for (int i = 0; i < rocks; ++i)
        {
            const float angle = m_random.Range(0.0f, 6.28318f);
            const glm::vec3 at{std::cos(angle) * m_random.Range(clear, 300.0f), kG, std::sin(angle) * m_random.Range(clear, 300.0f)};
            const float size = m_random.Range(5.0f, 18.0f);
            // None in the way the ship comes in.
            if (std::abs(at.x) < 30.0f + size && at.z < 0.0f)
            {
                continue;
            }
            m_solid.Box("rock", at - glm::vec3(size, 0.2f, size * 0.7f), at + glm::vec3(size, size * m_random.Range(0.25f, 0.45f), size * 0.7f), m_rock, 0.9f);
        }
        // Some of the ground scuffed and patched round the slab, never on it, each raised a different hand's breadth so where they
        // overlap their tops never lie together.
        for (int i = 0; i < 20; ++i)
        {
            const float angle = m_random.Range(0.0f, 6.28318f);
            const float wide = m_random.Range(14.0f, 50.0f);
            const glm::vec3 at{std::cos(angle) * m_random.Range(clear, 280.0f), kG, std::sin(angle) * m_random.Range(clear, 280.0f)};
            if (at.x + wide * 1.2f > m_lo.x && at.x - wide * 1.2f < m_hi.x && at.z + wide * 1.2f > m_lo.y && at.z - wide * 1.2f < m_hi.y)
            {
                continue;
            }
            ModelPart& patch = m_dress.Box("patch", at - glm::vec3(wide, 0.1f, wide * 0.6f), at + glm::vec3(wide, 0.2f + 0.04f * static_cast<float>(i), wide * 0.6f),
                                           m_ground * m_random.Range(0.8f, 1.1f), 0.95f);
            patch.rotation = {0.0f, angle * kDegrees, 0.0f};
        }

        // The slab: everything inside is concrete, poured in squares a shade apart, the pad and the road poured into it.
        const std::vector<glm::vec4> poured{KestrelStation::kPad, {kPavementWest, m_streetNorth, kPavementEast, m_streetSouth}};
        for (float x = m_lo.x; x < m_hi.x - 0.01f; x += 20.0f)
        {
            for (float z = m_lo.y; z < m_hi.y - 0.01f; z += 20.0f)
            {
                const glm::vec3 shade = m_concrete * m_random.Range(0.72f, 0.82f);
                for (const glm::vec4& piece : SubtractRects({x, z, x + 20.0f, z + 20.0f}, poured))
                {
                    m_solid.Box("slab", {piece.x, kG, piece.y}, {piece.z, kS, piece.w}, shade, 0.9f);
                }
            }
        }
    }

    void Pad()
    {
        KestrelStation::Pad(m_solid);
        KestrelStation::Supply(m_solid);
        KestrelStation::PadDressing(m_dress, kPavementEdge + 0.1f);
        KestrelStation::PadMasts(m_solid);
        KestrelStation::PadMastBars(m_dress);
        for (const Lamp& lamp : KestrelStation::PadMastLamps())
        {
            m_out.lamps.push_back(lamp);
        }
        // The pad's number, on it ahead of the ship.
        const int number = m_random.Int(1, 6);
        m_out.signs.push_back({"bay_number", {std::string(number < 10 ? "0" : "") + std::to_string(number)}, {0.0f, kS + kPaint + 0.01f, -24.0f}, 180.0f,
                               8.0f, 5.0f, Sign::Style::Floor});
        // The way the ship comes in, painted on ahead of the pad out to the slab's edge.
        m_dress.Box("apron_line", {-0.2f, kS, m_lo.y + 2.0f}, {0.2f, kS + kPaint, -40.5f}, m_scheme.warning * 0.9f, 0.7f);

        // Blast walls behind the pad and to starboard, as the outpost has them, with a gantry's worth of grime and buttresses.
        const float high = std::round(m_random.Range(6.0f, 9.0f));
        if (m_sternWall)
        {
            const float to = kStarboardFront;
            m_solid.Box("bay_wall", {-24.0f, kS, 35.0f}, {to, kS + high, 36.0f}, m_concrete * 0.78f, 0.9f);
            m_dress.Box("coping", {-24.15f, kS + high - 0.05f, 34.85f}, {m_starboardWall ? to + 1.15f : to + 0.15f, kS + high + 0.3f, 36.15f}, m_concrete,
                        0.85f);
            m_dress.Box("grime", {-23.95f, kS, 34.95f}, {to - 0.05f, kS + 1.3f, 35.05f}, kGrime, 0.95f);
            for (float x = -18.0f; x < to - 2.0f; x += 8.0f)
            {
                m_solid.Box("bay_buttress", {x - 0.35f, kS, 34.4f}, {x + 0.35f, kS + high - 0.4f, 35.0f}, kSteel, 0.6f, 0.5f);
            }
            if (m_style.mark)
            {
                // In the gap between the two buttresses nearest the middle.
                m_out.signs.push_back({"bay_logo", {}, {2.0f, kS + high - 2.6f, 34.3f}, 180.0f, 6.0f, 1.5f, Sign::Style::Logo});
            }
        }
        // (With a wall to starboard there is always one behind: they meet at the corner, under the stern wall's coping.)
        if (m_starboardWall)
        {
            m_solid.Box("bay_wall", {kStarboardFront, kS, -40.0f}, {kStarboardFront + 1.0f, kS + high, 36.0f}, m_concrete * 0.78f, 0.9f);
            m_dress.Box("coping", {kStarboardFront - 0.15f, kS + high - 0.05f, -40.15f}, {kStarboardFront + 1.15f, kS + high + 0.3f, 34.85f}, m_concrete, 0.85f);
            m_dress.Box("grime", {kStarboardFront - 0.05f, kS, -39.95f}, {kStarboardFront + 0.05f, kS + 1.3f, 34.95f}, kGrime, 0.95f);
            for (float z = -36.0f; z < 30.0f; z += 8.0f)
            {
                m_solid.Box("bay_buttress", {kStarboardFront - 0.6f, kS, z - 0.35f}, {kStarboardFront, kS + high - 0.4f, z + 0.35f}, kSteel, 0.6f, 0.5f);
            }
        }
    }

    void Street()
    {
        const float s = kS;
        const float paint = s + kPaint;
        // The road, poured into the slab in lengths; the pavements either side, a kerb high.
        for (float z = m_streetNorth; z < m_streetSouth - 0.01f; z += 25.0f)
        {
            const float end = std::min(z + 25.0f, m_streetSouth);
            m_solid.Box("road", {kPavementWest, kG, z}, {kPavementEast, s, end}, kAsphalt, 0.95f);
            m_solid.Box("pavement", {kPavementEast, s, z}, {kPavementEdge, s + kKerb, end}, m_concrete * 0.86f, 0.9f);
            m_solid.Box("pavement", {kFacade, s, z}, {kPavementWest, s + kKerb, end}, m_concrete * 0.86f, 0.9f);
        }
        // Its centre line dashed and its edges lined, both broken for the crossing from the stair, which is striped.
        const float middle = (kPavementWest + kPavementEast) * 0.5f;
        for (float z = m_streetNorth + 2.0f; z < m_streetSouth - 3.0f; z += 8.0f)
        {
            if (z + 3.0f > KestrelStation::kCrossFrom - 1.0f && z < KestrelStation::kCrossTo + 1.0f)
            {
                continue;
            }
            m_dress.Box("road_line", {middle - 0.1f, s, z}, {middle + 0.1f, paint, z + 3.0f}, kWhitePaint, 0.7f);
        }
        for (const float x : {kPavementWest + 0.5f, kPavementEast - 0.5f})
        {
            m_dress.Box("road_line", {x - 0.1f, s, m_streetNorth + 0.2f}, {x + 0.1f, paint, KestrelStation::kCrossFrom - 0.6f}, kWhitePaint * 0.9f, 0.7f);
            m_dress.Box("road_line", {x - 0.1f, s, KestrelStation::kCrossTo + 0.6f}, {x + 0.1f, paint, m_streetSouth - 0.2f}, kWhitePaint * 0.9f, 0.7f);
        }
        for (float x = kPavementWest + 0.4f; x < kPavementEast - 0.5f; x += 1.1f)
        {
            m_dress.Box("crossing", {x, s, KestrelStation::kCrossFrom}, {x + 0.55f, paint, KestrelStation::kCrossTo}, kWhitePaint, 0.7f);
        }
        // Lamp posts along both pavements, their arms out over the road -- not on the crossing, nor in front of a door.
        const auto byDoor = [this](float z)
        {
            return std::any_of(m_doors.begin(), m_doors.end(), [z](float door) { return std::abs(door - z) < 5.0f; }) || std::abs(z - kCross) < 4.0f;
        };
        for (float z = m_streetNorth + 10.0f; z < m_streetSouth - 6.0f; z += 26.0f)
        {
            for (const float side : {1.0f, -1.0f})
            {
                const float at = side > 0.0f ? z : z + 13.0f;
                if (at > m_streetSouth - 4.0f || byDoor(at))
                {
                    continue;
                }
                const float x = side > 0.0f ? kPavementEast + 1.4f : kPavementWest - 1.2f;
                const float reach = side > 0.0f ? -1.8f : 1.8f;
                m_solid.Box("street_post", {x - 0.13f, s, at - 0.13f}, {x + 0.13f, s + 8.8f, at + 0.13f}, kSteelDark, 0.5f, 0.7f);
                m_dress.Box("street_arm", {std::min(x, x + reach) - 0.06f, s + 8.6f, at - 0.06f}, {std::max(x, x + reach) + 0.06f, s + 8.75f, at + 0.06f},
                            kSteelDark, 0.5f, 0.7f);
                m_out.lamps.push_back({{x + reach, s + 8.45f, at}, {0.0f, -1.0f, 0.0f}, kSodium, 42.0f, 20.0f, 70.0f, 120.0f});
            }
        }
    }

    void Fence()
    {
        if (!m_style.standard && !m_random.Chance(0.6f))
        {
            return;
        }
        // Round the slab's edge, the sides a hand lower than the ends so where they meet their tops are not one on the other.
        const glm::vec2 lo = m_lo + glm::vec2(1.0f);
        const glm::vec2 hi = m_hi - glm::vec2(1.0f);
        for (int side = 0; side < 4; ++side)
        {
            const bool alongX = side < 2;
            const float fixed = side == 0 ? lo.y : side == 1 ? hi.y : side == 2 ? lo.x : hi.x;
            const float from = alongX ? lo.x : lo.y;
            const float to = alongX ? hi.x : hi.y;
            for (float a = from; a < to; a += 7.0f)
            {
                const float b = std::min(a + 7.0f, to);
                if (m_style.abandoned && m_random.Chance(0.3f))
                {
                    continue;
                }
                const glm::vec3 low = alongX ? glm::vec3(a, kS, fixed - 0.05f) : glm::vec3(fixed - 0.05f, kS, a);
                const glm::vec3 high = alongX ? glm::vec3(b, kS + 3.4f, fixed + 0.05f) : glm::vec3(fixed + 0.05f, kS + 3.35f, b);
                m_solid.Box("fence", low, high, kSteelDark * 1.3f, 0.6f, 0.5f);
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
        if (lot.facing == 0)
        {
            m_doors.push_back(lot.At(u, 0.0f, 0.0f).z);
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
        }
    }

    void Operations(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        const float H = std::round(m_random.Range(10.0f, 15.0f));
        const float door = placed.door;
        StreetDoor(lot, door);
        Box(m_solid, lot, "ops", {0.0f, s, 0.0f}, {L, s + H, D}, m_scheme.panel * 0.9f, 0.85f);
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
        // Plant on its roof; a stack, perhaps a dish.
        for (int i = 0; i < m_random.Int(2, 5); ++i)
        {
            const float u = m_random.Range(2.0f, L - 6.0f);
            const float v = m_random.Range(2.0f, D - 6.0f);
            Box(m_solid, lot, "roof_unit", {u, s + H - 0.05f, v}, {u + 4.0f, s + H + m_random.Range(1.4f, 2.4f), v + 3.2f}, kSteel, 0.6f, 0.5f);
        }
        m_solid.Cylinder("stack", lot.At(L - 2.5f, s + H + 3.5f, D - 2.5f), 1.2f, 7.0f, glm::vec3(0.0f), kRust, 0.7f, 0.4f);
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
        const float H = std::round(m_random.Range(6.0f, 11.0f) * 2.0f) * 0.5f;
        const glm::vec3 colour = m_scheme.panel * m_random.Range(0.72f, 0.95f);
        Box(m_solid, lot, "building", {0.0f, s, 0.0f}, {L, s + H, D}, colour, 0.85f);
        Parapet(lot, 0.0f, L, 0.0f, D, s + H, colour);
        const float door = std::round(m_random.Range(3.0f, L - 3.0f));
        StreetDoor(lot, door);
        Box(m_dress, lot, "fx_door", {door - 1.5f, s, -0.08f}, {door + 1.5f, s + 2.8f, 0.05f}, Glass(kWindow * 0.6f), 0.3f, 0.0f, Glow(0.8f));
        Box(m_solid, lot, "door_canopy", {door - 2.0f, s + 3.1f, -1.4f}, {door + 2.0f, s + 3.3f, 0.05f}, kSteel, 0.6f, 0.5f);
        DoorLamp(lot, door, s + 3.0f, -0.6f);
        LitSign(lot, placed.label, door, s + 4.2f, -0.12f);
        Windows(lot, 1.0f, L - 1.0f, s + 1.1f, s + H - 0.8f, 2.4f, 3.6f, door - 4.0f, door + 4.0f, s + 6.0f);
        // Something added on top, now and then.
        if (H < 10.0f && L > 14.0f && D > 12.0f && m_random.Chance(0.5f))
        {
            const float u = m_random.Range(1.5f, L * 0.4f);
            const float v = m_random.Range(1.5f, D * 0.3f);
            const glm::vec3 module = m_scheme.panel * 0.75f;
            Box(m_solid, lot, "module", {u, s + H - 0.05f, v}, {u + L * 0.45f, s + H + 3.4f, v + D * 0.5f}, module, 0.8f, 0.2f);
        }
        for (int i = 0; i < m_random.Int(0, 3); ++i)
        {
            const float u = m_random.Range(L * 0.55f, L - 4.0f);
            const float v = m_random.Range(D * 0.6f, D - 3.0f);
            Box(m_solid, lot, "roof_unit", {u, s + H - 0.05f, v}, {u + 2.6f, s + H + m_random.Range(1.2f, 2.2f), v + 2.0f}, kSteel, 0.6f, 0.5f);
        }
    }

    void Shed(const Placed& placed)
    {
        const Lot& lot = placed.lot;
        const float s = kS;
        const float L = lot.length;
        const float D = lot.depth;
        const float H = std::round(m_random.Range(11.0f, 18.0f));
        Box(m_solid, lot, "shed", {0.0f, s, 0.0f}, {L, s + H, D}, m_scheme.panel * 0.85f, 0.75f, 0.2f);
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
        const glm::vec2 box = deep ? glm::vec2(2.44f, 12.2f) : glm::vec2(12.2f, 2.44f); // along u, along v
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
                    Box(m_solid, lot, "container", {u, y, v}, {u + box.x, y + 2.59f, v + box.y}, kBoxes[m_random.Int(0, static_cast<int>(std::size(kBoxes)) - 1)],
                        0.7f, 0.3f);
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
        Box(m_solid, lot, "yard_mast", {-1.85f, s, 0.75f}, {-1.35f, s + 14.0f, 1.25f}, kSteelDark, 0.5f, 0.7f);
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
        const float high = std::round(m_random.Range(28.0f, 42.0f));
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
    bool m_starboardWall = false;
    bool m_sternWall = false;
    bool m_comms = false;
    std::vector<Placed> m_lots;
    std::vector<std::string> m_labels;
    std::vector<float> m_doors; // along the street, where doors open onto it
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
