#include "Game/World/KestrelStation.h"

#include "Game/World/PartBuilder.h"
#include "Game/World/ShipMap.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <cmath>

namespace pred
{

namespace KestrelStation
{

namespace
{

constexpr float kG = ShipSpec::kFieldGround; // the ground
constexpr float kDegrees = 57.2957795f;

// Hangar Row's bay: the inside face of its port wall (a metre thick, out to -25), Shipworks' side as its starboard wall,
// open ahead onto the apron, its back wall's inside face; how high its blast walls stand; the gate through the port wall.
constexpr float kPortWall = -24.0f;
constexpr float kWorksWall = 24.0f;
constexpr float kBayOpen = -40.0f;
constexpr float kBackWall = 35.0f;
constexpr float kWallHigh = 9.0f;
constexpr float kGateFrom = -16.0f;
constexpr float kGateTo = 0.0f;
constexpr float kGateHigh = 7.0f;
// The street, along z: the pavement by the bay's wall (from here to the wall), the road, the far pavement (from here to the
// buildings' fronts), and how far it runs, its kerbs this high. The walkway from the stair crosses it to Operations Hall.
constexpr float kPavementEast = -28.0f;
constexpr float kPavementWest = -45.0f;
constexpr float kFacade = -49.0f;
constexpr float kStreetNorth = -150.0f;
constexpr float kStreetSouth = 100.0f;
constexpr float kKerb = 0.15f;
// Paint is laid this far over what it is on, and no two coats ever at the same height where they meet.
constexpr float kPaint = 0.05f;

// The station's colours: weathered concrete and painted steel, the old darker than the new; asphalt; amber sodium light and
// the cooler white of floodlights; hazard yellow; CIRRA's orange.
const glm::vec3 kConcrete{0.42f, 0.41f, 0.38f};
const glm::vec3 kConcreteOld{0.31f, 0.30f, 0.28f};
const glm::vec3 kPadColour{0.23f, 0.23f, 0.23f};
const glm::vec3 kAsphalt{0.12f, 0.12f, 0.13f};
const glm::vec3 kSteel{0.36f, 0.38f, 0.40f};
const glm::vec3 kSteelDark{0.17f, 0.18f, 0.19f};
const glm::vec3 kPanel{0.46f, 0.46f, 0.44f};
const glm::vec3 kRust{0.42f, 0.24f, 0.14f};
const glm::vec3 kHazard{0.80f, 0.62f, 0.12f};
const glm::vec3 kWhitePaint{0.80f, 0.80f, 0.76f};
const glm::vec3 kOrange{0.93f, 0.45f, 0.12f};
const glm::vec3 kGrime{0.16f, 0.15f, 0.14f};
const glm::vec3 kSodium{1.0f, 0.72f, 0.36f};
const glm::vec3 kFlood{0.92f, 0.95f, 1.0f};
const glm::vec3 kWindow{1.0f, 0.86f, 0.62f};
const glm::vec3 kGlass{0.12f, 0.16f, 0.18f};

// A light-ish hash for small variations: which panel of concrete is a shade darker.
float Shade(int a, int b)
{
    uint32_t h = static_cast<uint32_t>(a) * 73856093u ^ static_cast<uint32_t>(b) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

// The street's lamp posts: where each stands and which way its arm reaches out over the road.
struct Post
{
    glm::vec2 at;
    float reach;
};
std::vector<Post> StreetPosts()
{
    std::vector<Post> posts;
    for (float z = kStreetNorth + 12.0f; z < kStreetSouth - 6.0f; z += 24.0f)
    {
        posts.push_back({{kPavementEast + 1.4f, z}, -1.8f});
        // Not in front of Operations Hall's entrance, which has its canopy's light.
        if (std::abs(z + 12.0f + 2.0f) > 8.0f)
        {
            posts.push_back({{kPavementWest - 1.2f, z + 12.0f}, 1.8f});
        }
    }
    return posts;
}

// The floodlight masts at the bay's corners, and the apron's.
const glm::vec2 kBayMasts[] = {{-20.5f, -37.0f}, {20.5f, -37.0f}, {-12.0f, 33.6f}, {12.0f, 33.6f}};
const glm::vec2 kApronMasts[] = {{-10.0f, -62.0f}, {20.0f, -100.0f}, {-15.0f, -130.0f}, {60.0f, -140.0f}};
// Where pipes cross the street, carried on trestles.
const float kPipeBridges[] = {-22.0f, 14.0f};

// A building as a block: its walls and roof, and a parapet round the top.
void Block(PartBuilder& b, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour)
{
    b.Box("building", lo, hi, colour, 0.85f);
    b.Parapet("building_parapet", {lo.x - 0.15f, hi.y - 0.1f, lo.z - 0.15f}, {hi.x + 0.15f, hi.y + 0.6f, hi.z + 0.15f}, 0.35f, colour * 0.82f);
}

// Lit windows, a row of them proud of a wall facing +x (at `x`) between z0 and z1: so wide, so far apart, so high.
void WindowsX(PartBuilder& b, float x, float z0, float z1, float y0, float y1, float wide, float apart, float glow, float skipFrom = 1.0e9f,
              float skipTo = -1.0e9f)
{
    for (float z = z0; z + wide <= z1 + 0.01f; z += apart)
    {
        if (z + wide > skipFrom && z < skipTo)
        {
            continue;
        }
        b.Box("fx_window", {x - 0.05f, y0, z}, {x + 0.07f, y1, z + wide}, kWindow, 0.3f, 0.0f, glow);
    }
}

} // namespace

// --- What every pad has ----------------------------------------------------------------------------------------------

void Pad(PartBuilder& b)
{
    // Poured in four, its seams under the ship where nothing of the ship's own lies on them.
    for (const glm::vec2 x : {glm::vec2{kPad.x, 0.0f}, glm::vec2{0.0f, kPad.z}})
    {
        for (const glm::vec2 z : {glm::vec2{kPad.y, 2.0f}, glm::vec2{2.0f, kPad.w}})
        {
            b.Box("pad", {x.x, kG, z.x}, {x.y, kSlabTop, z.y}, kPadColour, 0.85f);
        }
    }
}

void Supply(PartBuilder& b)
{
    // The ground supply the ship is plugged into while it stands here.
    b.Box("bay_supply", {-12.6f, kSlabTop, 3.0f}, {-10.8f, kSlabTop + 1.1f, 4.6f}, kHazard * 0.85f, 0.6f, 0.3f);
}

void PadDressing(PartBuilder& b, float walkwayTo)
{
    const float s = kSlabTop;
    const float paint = s + kPaint;
    // A hazard border round the pad, its sides whole and its ends between them; lamps sunk outside the border; the guide line
    // out ahead; the walkway from the stair, broken where it crosses the border.
    b.Box("pad_edge", {-17.45f, s, -30.45f}, {-16.55f, paint, 34.45f}, kHazard, 0.7f);
    b.Box("pad_edge", {16.55f, s, -30.45f}, {17.45f, paint, 34.45f}, kHazard, 0.7f);
    b.Box("pad_edge", {-16.55f, s, -30.45f}, {16.55f, paint, -29.55f}, kHazard, 0.7f);
    b.Box("pad_edge", {-16.55f, s, 33.55f}, {16.55f, paint, 34.45f}, kHazard, 0.7f);
    for (const float x : {-17.9f, 17.9f})
    {
        for (float z = -28.0f; z <= 33.0f; z += 8.0f)
        {
            b.Box("fx_pad_lamp", {x - 0.2f, s, z - 0.2f}, {x + 0.2f, s + 0.1f, z + 0.2f}, kFlood, 0.3f, 0.0f, 3.0f);
        }
    }
    b.Box("pad_line", {-0.2f, s, kBayOpen}, {0.2f, paint, -31.0f}, kWhitePaint, 0.7f);
    const glm::vec3 walkway{0.22f, 0.36f, 0.42f};
    b.Box("walkway", {walkwayTo, s, kCrossFrom}, {-17.45f, paint, kCrossTo}, walkway, 0.8f);
    b.Box("walkway", {-16.55f, s, kCrossFrom}, {-9.4f, paint, kCrossTo}, walkway, 0.8f);
    // The supply's cable across to the ship.
    b.Box("bay_cable", {-10.8f, s + 0.02f, 3.6f}, {-5.6f, s + 0.12f, 3.75f}, kSteelDark, 0.7f, 0.2f);
}

void PadMasts(PartBuilder& b)
{
    for (const glm::vec2 mast : kBayMasts)
    {
        b.Box("bay_mast", {mast.x - 0.3f, kSlabTop, mast.y - 0.3f}, {mast.x + 0.3f, kSlabTop + 16.0f, mast.y + 0.3f}, kSteelDark, 0.5f, 0.7f);
    }
}

void PadMastBars(PartBuilder& b)
{
    for (const glm::vec2 mast : kBayMasts)
    {
        b.Box("mast_bar", {mast.x - 1.4f, kSlabTop + 16.0f, mast.y - 0.12f}, {mast.x + 1.4f, kSlabTop + 16.25f, mast.y + 0.12f}, kSteelDark, 0.5f, 0.7f);
    }
}

std::vector<Lamp> PadMastLamps()
{
    const float s = kSlabTop;
    std::vector<Lamp> lamps;
    const glm::vec3 padMiddle{0.0f, s, 0.0f};
    for (const glm::vec2 mast : kBayMasts)
    {
        for (const float side : {-1.0f, 1.0f})
        {
            const glm::vec3 at{mast.x + side * 1.0f, s + 15.8f, mast.y};
            lamps.push_back({at, glm::normalize(padMiddle + glm::vec3(side * 6.0f, 0.0f, 0.0f) - at), kFlood, 420.0f, 60.0f, 40.0f, 75.0f});
        }
    }
    return lamps;
}

void LampHeads(PartBuilder& b, const std::vector<Lamp>& lamps)
{
    for (const Lamp& lamp : lamps)
    {
        const glm::vec3 c = lamp.at;
        b.Box("fx_lamp_head", c - glm::vec3(0.45f, 0.18f, 0.35f), c + glm::vec3(0.45f, 0.02f, 0.35f), lamp.colour, 0.3f, 0.0f, 3.5f);
        b.Box("lamp_hood", c - glm::vec3(0.5f, -0.02f, 0.4f), c + glm::vec3(0.5f, 0.22f, 0.4f), kSteelDark, 0.5f, 0.7f);
    }
}

// --- What is solid -------------------------------------------------------------------------------------------------

ModelAsset Solid(const glm::vec3& ground, const glm::vec3& rock)
{
    ModelAsset model;
    model.name = "kestrel";
    PartBuilder b{model};
    const float s = kSlabTop;

    // The world's ground as far as the haze, rock through it, low hills at the edge of what can be seen.
    b.Box("ground", {-440.0f, kG - 1.0f, -440.0f}, {440.0f, kG, 440.0f}, ground, 0.95f);
    for (int i = 0; i < 22; ++i)
    {
        const float angle = 6.28318f * static_cast<float>(i) / 22.0f;
        const float reach = 330.0f + 40.0f * static_cast<float>((i * 7) % 3);
        const glm::vec3 at{std::cos(angle) * reach, kG, std::sin(angle) * reach};
        const float high = 14.0f + 9.0f * static_cast<float>((i * 5) % 4);
        b.Turned("hill", at, {150.0f + 30.0f * static_cast<float>(i % 3), high * 2.0f, 52.0f}, -angle * kDegrees + 90.0f, glm::mix(ground, rock, 0.45f), 18.0f,
                 0.95f);
    }
    for (int i = 0; i < 14; ++i)
    {
        const float angle = static_cast<float>(i) * 2.39996f;
        const float reach = 190.0f + 9.0f * static_cast<float>(i);
        const glm::vec3 at{std::cos(angle) * reach, kG, std::sin(angle) * reach};
        const float size = 6.0f + static_cast<float>((i * 7) % 5) * 3.0f;
        b.Box("rock", at - glm::vec3(size, 0.2f, size * 0.7f), at + glm::vec3(size, size * 0.35f, size * 0.7f), rock, 0.9f);
    }

    // The station's slab: everything inside the fence is concrete, poured in squares a shade apart -- and each square its
    // own piece, so the lamps over it light the part under them. The bay's pad and the street's road are poured into it, not
    // laid on it: a few centimetres of paint on concrete can be told apart close to, but a hundred metres off the picture's
    // depth cannot tell which is in front, and they flicker.
    const std::vector<glm::vec4> poured{kPad, {kPavementWest, kStreetNorth, kPavementEast, kStreetSouth}};
    for (int i = 0; i < 14; ++i)
    {
        for (int j = 0; j < 14; ++j)
        {
            const float x = -140.0f + 20.0f * static_cast<float>(i);
            const float z = -160.0f + 20.0f * static_cast<float>(j);
            for (const glm::vec4& piece : SubtractRects({x, z, x + 20.0f, z + 20.0f}, poured))
            {
                b.Box("slab", {piece.x, kG, piece.y}, {piece.z, s, piece.w}, kConcrete * (0.72f + 0.1f * Shade(i, j)), 0.9f);
            }
        }
    }
    Pad(b);
    for (float z = kStreetNorth; z < kStreetSouth - 0.01f; z += 25.0f)
    {
        b.Box("road", {kPavementWest, kG, z}, {kPavementEast, s, z + 25.0f}, kAsphalt, 0.95f);
    }

    // --- Hangar Row: the ship's bay -----------------------------------------------------------------------------------
    // The port wall, the gate through it and the lintel over the gate; the back wall between it and Shipworks.
    b.Box("bay_wall", {kPortWall - 1.0f, s, kBayOpen}, {kPortWall, s + kWallHigh, kGateFrom}, kConcreteOld, 0.9f);
    b.Box("bay_wall", {kPortWall - 1.0f, s, kGateTo}, {kPortWall, s + kWallHigh, kBackWall + 1.0f}, kConcreteOld, 0.9f);
    b.Box("bay_lintel", {kPortWall - 1.0f, s + kGateHigh, kGateFrom}, {kPortWall, s + kWallHigh, kGateTo}, kConcreteOld, 0.9f);
    b.Box("bay_wall", {kPortWall, s, kBackWall}, {kWorksWall, s + kWallHigh, kBackWall + 1.0f}, kConcrete * 0.9f, 0.9f);
    // Buttresses down the inside of the port wall and the back wall, the newer steel bolted onto the older concrete.
    for (float z = kBayOpen + 4.0f; z < 26.0f; z += 8.0f)
    {
        if (z > kGateFrom - 2.0f && z < kGateTo + 2.0f)
        {
            continue;
        }
        b.Box("bay_buttress", {kPortWall, s, z - 0.35f}, {kPortWall + 0.6f, s + kWallHigh - 0.4f, z + 0.35f}, kSteel, 0.6f, 0.5f);
    }
    for (const float x : {-6.0f, 2.0f, 10.0f, 18.0f})
    {
        b.Box("bay_buttress", {x - 0.35f, s, kBackWall - 0.6f}, {x + 0.35f, s + kWallHigh - 0.4f, kBackWall}, kSteel, 0.6f, 0.5f);
    }
    // The floodlight masts at its corners.
    PadMasts(b);
    // The control booth over the back corner to port, on legs, its windows on the pad; a ramp up to its door.
    for (const float x : {-23.2f, -17.0f})
    {
        for (const float z : {28.4f, 34.4f})
        {
            b.Box("booth_leg", {x - 0.2f, s, z - 0.2f}, {x + 0.2f, s + 5.0f, z + 0.2f}, kSteelDark, 0.5f, 0.7f);
        }
    }
    b.Box("booth", {-23.6f, s + 5.0f, 27.8f}, {-16.4f, s + 8.4f, kBackWall}, kPanel * 0.8f, 0.7f, 0.2f);
    b.Box("booth_roof", {-23.75f, s + 8.35f, 27.65f}, {-16.25f, s + 8.65f, kBackWall}, kSteelDark, 0.6f, 0.4f);
    b.Box("booth_landing", {-21.0f, s + 4.8f, 26.4f}, {-19.0f, s + 5.0f, 27.8f}, kSteel, 0.6f, 0.6f);
    {
        // From the floor ahead of it up to the landing: along z, its far end high.
        const float run = 12.0f;
        const float rise = 5.0f;
        const float length = std::sqrt(run * run + rise * rise);
        const glm::vec3 middle{-20.0f, s + rise * 0.5f - 0.1f, 26.4f - run * 0.5f};
        ModelPart& ramp = b.Box("booth_ramp", middle - glm::vec3(0.7f, 0.1f, length * 0.5f), middle + glm::vec3(0.7f, 0.1f, length * 0.5f), kSteel, 0.6f, 0.6f);
        // Turned about x, its far end (towards +z) up.
        ramp.rotation = {-std::atan2(rise, run) * kDegrees, 0.0f, 0.0f};
    }

    // --- The ground supply the ship is plugged into ------------------------------------------------------------------
    Supply(b);

    // --- Shipworks: the great shed to starboard, its side the bay's starboard wall ---------------------------------------
    b.Box("shipworks", {kWorksWall, s, kBayOpen}, {78.0f, s + 20.0f, kBackWall + 1.0f}, kPanel * 0.85f, 0.75f, 0.2f);

    // --- The street, through the gate --------------------------------------------------------------------------------
    // The pavements either side of the road, a kerb high, in lengths so each is lit by its own lamps.
    for (float z = kStreetNorth; z < kStreetSouth - 0.01f; z += 25.0f)
    {
        b.Box("pavement", {kPavementEast, s, z}, {kPortWall - 1.0f, s + kKerb, z + 25.0f}, kConcrete * 0.86f, 0.9f);
        b.Box("pavement", {kFacade, s, z}, {kPavementWest, s + kKerb, z + 25.0f}, kConcrete * 0.86f, 0.9f);
    }
    for (const Post& post : StreetPosts())
    {
        b.Box("street_post", {post.at.x - 0.13f, s, post.at.y - 0.13f}, {post.at.x + 0.13f, s + 8.8f, post.at.y + 0.13f}, kSteelDark, 0.5f, 0.7f);
    }
    // Bollards at the kerb either side of the gate.
    for (float z = kGateFrom - 9.0f; z <= kGateTo + 9.0f; z += 1.8f)
    {
        if (z > kGateFrom - 1.0f && z < kGateTo + 1.0f)
        {
            continue;
        }
        b.Box("bollard", {kPavementEast + 0.25f, s, z - 0.15f}, {kPavementEast + 0.55f, s + 1.0f, z + 0.15f}, kHazard, 0.6f);
    }
    // The trestles that carry pipes over the road.
    for (const float z : kPipeBridges)
    {
        for (const float x : {kPavementEast + 1.0f, kPavementWest - 1.6f})
        {
            b.Box("trestle_leg", {x - 0.2f, s, z - 0.6f}, {x + 0.2f, s + 8.0f, z - 0.2f}, kSteel, 0.5f, 0.7f);
            b.Box("trestle_leg", {x - 0.2f, s, z + 0.2f}, {x + 0.2f, s + 8.0f, z + 0.6f}, kSteel, 0.5f, 0.7f);
        }
    }

    // Operations Hall, facing the gate: ribbed concrete, its entrance set back under a canopy, silo towers along its roof.
    b.Box("ops", {-95.0f, s, -26.0f}, {kFacade - 2.0f, s + 16.0f, 22.0f}, kConcrete, 0.85f);
    b.Box("ops_front", {kFacade - 2.0f, s, -26.0f}, {kFacade, s + 16.0f, -9.0f}, kConcrete, 0.85f);
    b.Box("ops_front", {kFacade - 2.0f, s, 5.0f}, {kFacade, s + 16.0f, 22.0f}, kConcrete, 0.85f);
    b.Box("ops_front", {kFacade - 2.0f, s + 6.0f, -9.0f}, {kFacade, s + 16.0f, 5.0f}, kConcrete, 0.85f);
    b.Parapet("ops_parapet", {-95.15f, s + 15.9f, -26.15f}, {kFacade + 0.15f, s + 16.6f, 22.15f}, 0.35f, kConcrete * 0.82f);
    for (float z = -24.0f; z < 22.0f; z += 4.0f)
    {
        if (z > -10.0f && z < 6.0f)
        {
            continue;
        }
        b.Box("ops_rib", {kFacade - 0.05f, s, z - 0.3f}, {kFacade + 0.4f, s + 15.7f, z + 0.3f}, kConcreteOld * 1.15f, 0.85f);
    }
    for (const float z : {-8.5f, 4.5f})
    {
        b.Box("ops_canopy_post", {kPavementWest - 0.9f, s, z - 0.15f}, {kPavementWest - 0.6f, s + 5.6f, z + 0.15f}, kSteelDark, 0.5f, 0.7f);
    }
    for (int i = 0; i < 3; ++i)
    {
        b.Cylinder("ops_silo", {-82.0f, s + 20.4f, -18.0f + 14.0f * static_cast<float>(i)}, 6.5f, 9.0f, {0.0f, 0.0f, 0.0f}, kConcreteOld, 0.85f);
    }
    // The Research Annex, to the north: a low block with modules added on top, one reaching out over the pavement on legs.
    b.Box("annex", {-80.0f, s, -68.0f}, {kFacade, s + 7.0f, -38.0f}, kPanel * 0.9f, 0.8f, 0.1f);
    b.Box("annex_module", {-76.0f, s + 6.9f, -64.0f}, {-56.0f, s + 12.0f, -44.0f}, kPanel * 0.75f, 0.8f, 0.2f);
    b.Box("annex_module", {-58.0f, s + 7.4f, -62.0f}, {kFacade + 2.0f, s + 11.4f, -50.0f}, kSteel, 0.7f, 0.4f);
    for (const float z : {-61.4f, -50.6f})
    {
        b.Box("annex_leg", {kFacade + 1.4f, s, z - 0.2f}, {kFacade + 1.8f, s + 7.4f, z + 0.2f}, kSteelDark, 0.5f, 0.7f);
    }
    // Crew Services, to the south.
    Block(b, {-82.0f, s, 30.0f}, {kFacade, s + 10.0f, 58.0f}, kPanel * 0.8f);
    // The Navigation Relay closing the street's far end, its lattice mast beside it.
    Block(b, {-62.0f, s, kStreetSouth}, {-27.0f, s + 8.0f, kStreetSouth + 16.0f}, kConcreteOld);
    for (const glm::vec2 leg : {glm::vec2{-72.0f, 96.0f}, glm::vec2{-66.0f, 96.0f}, glm::vec2{-72.0f, 102.0f}, glm::vec2{-66.0f, 102.0f}})
    {
        b.Box("relay_leg", {leg.x - 0.3f, s, leg.y - 0.3f}, {leg.x + 0.3f, s + 42.0f, leg.y + 0.3f}, kSteel, 0.5f, 0.7f);
    }
    // The gatehouse where the street meets the fence, and its barrier's post.
    b.Box("gatehouse", {-53.0f, s, -147.0f}, {-49.5f, s + 3.0f, -143.0f}, kPanel * 0.8f, 0.7f, 0.2f);
    b.Box("gatehouse_roof", {-53.3f, s + 2.95f, -147.3f}, {-49.2f, s + 3.25f, -142.7f}, kSteelDark, 0.6f, 0.4f);
    b.Box("barrier_post", {kPavementWest - 0.6f, s, -145.3f}, {kPavementWest - 0.2f, s + 1.1f, -144.9f}, kHazard, 0.6f);

    // --- Salvage Intake, out on the apron: the shed, its docks, and the containers waiting to be gone through ---------
    Block(b, {30.0f, s, -112.0f}, {90.0f, s + 10.0f, -80.0f}, kPanel * 0.8f);
    b.Box("salvage_dock", {34.0f, s, -80.0f}, {86.0f, s + 1.3f, -74.0f}, kConcreteOld, 0.9f);
    const glm::vec3 boxes[] = {kRust, {0.18f, 0.32f, 0.36f}, kOrange * 0.8f, {0.32f, 0.33f, 0.30f}, {0.55f, 0.18f, 0.12f}};
    for (int i = 0; i < 18; ++i)
    {
        const int column = i % 6;
        const int row = (i / 6) % 3;
        const float x = 98.0f + static_cast<float>(column) * 6.5f;
        const float z = -110.0f + static_cast<float>(row) * 16.0f;
        const int high = 1 + (i * 7) % 3;
        for (int level = 0; level < high; ++level)
        {
            const float y = s + 2.62f * static_cast<float>(level);
            b.Box("container", {x, y, z}, {x + 2.44f, y + 2.59f, z + 12.2f}, boxes[(i + level * 2) % 5], 0.7f, 0.3f);
        }
    }
    for (const float x : {95.0f, 138.0f})
    {
        b.Box("crane_leg", {x - 0.5f, s, -114.0f}, {x + 0.5f, s + 14.0f, -113.0f}, kOrange * 0.85f, 0.6f, 0.4f);
        b.Box("crane_leg", {x - 0.5f, s, -62.0f}, {x + 0.5f, s + 14.0f, -61.0f}, kOrange * 0.85f, 0.6f, 0.4f);
    }
    for (const glm::vec2 at : kApronMasts)
    {
        b.Box("apron_mast", {at.x - 0.3f, s, at.y - 0.3f}, {at.x + 0.3f, s + 20.0f, at.y + 0.3f}, kSteelDark, 0.5f, 0.7f);
    }

    // --- The tanks behind Shipworks, and the fence round it all --------------------------------------------------------
    for (int i = 0; i < 3; ++i)
    {
        const glm::vec3 at{96.0f + 14.0f * static_cast<float>(i), s + 6.0f, 40.0f};
        b.Cylinder("tank", at, 10.0f, 12.0f, {0.0f, 0.0f, 0.0f}, kPanel, 0.5f, 0.2f);
    }
    for (int side = 0; side < 4; ++side)
    {
        const bool alongX = side < 2;
        const float fixed = side == 0 ? -160.0f : side == 1 ? 120.0f : side == 2 ? -140.0f : 140.0f;
        const float from2 = alongX ? -140.0f : -160.0f;
        const float to2 = alongX ? 140.0f : 120.0f;
        for (float a = from2; a < to2; a += 7.0f)
        {
            const float b2 = std::min(a + 7.0f, to2);
            const glm::vec3 lo = alongX ? glm::vec3(a, s, fixed - 0.05f) : glm::vec3(fixed - 0.05f, s, a);
            // The sides a hand lower than the ends, so where they meet at the corners their tops are not one on the other.
            const glm::vec3 hi = alongX ? glm::vec3(b2, s + 3.4f, fixed + 0.05f) : glm::vec3(fixed + 0.05f, s + 3.35f, b2);
            b.Box("fence", lo, hi, kSteelDark * 1.3f, 0.6f, 0.5f);
        }
    }
    return model;
}

// --- What is only looked at -----------------------------------------------------------------------------------------

ModelAsset Dressing(const glm::vec3& ground)
{
    ModelAsset model;
    model.name = "kestrel_dressing";
    PartBuilder b{model};
    const float s = kSlabTop;
    const float paint = s + kPaint;

    // --- The bay's floor ------------------------------------------------------------------------------------------------
    // The pad (poured in the slab: Solid) has a hazard border round it, its sides whole and its ends between them; lamps
    // sunk outside the border; the guide line out to the apron; the walkway from the stair to the gate, broken where it
    // crosses the border.
    PadDressing(b, kPortWall + 0.1f);

    // --- The bay's walls ------------------------------------------------------------------------------------------------
    // Copings along their tops, over their edges; grime along their feet and down from their tops.
    b.Box("coping", {kPortWall - 1.15f, s + kWallHigh - 0.05f, kBayOpen - 0.15f}, {kPortWall + 0.15f, s + kWallHigh + 0.3f, kBackWall + 1.15f}, kConcrete,
          0.85f);
    b.Box("coping", {kPortWall + 0.15f, s + kWallHigh - 0.05f, kBackWall - 0.15f}, {kWorksWall, s + kWallHigh + 0.3f, kBackWall + 1.15f}, kConcrete, 0.85f);
    for (const glm::vec2 run : {glm::vec2{kBayOpen, kGateFrom}, glm::vec2{kGateTo, kBackWall}})
    {
        // Short of the wall's ends, so no end of them lies in the end of the wall.
        b.Box("grime", {kPortWall - 0.05f, s, run.x + 0.05f}, {kPortWall + 0.05f, s + 1.4f, run.y - 0.05f}, kGrime, 0.95f);
        b.Box("grime", {kPortWall - 1.05f, s, run.x + 0.05f}, {kPortWall - 0.95f, s + 1.2f, run.y - 0.05f}, kGrime, 0.95f);
        for (float z = run.x + 3.0f; z < run.y - 1.0f; z += 7.0f)
        {
            b.Box("streak", {kPortWall - 0.05f, s + 4.0f + std::fmod(std::abs(z) * 0.37f, 2.5f), z}, {kPortWall + 0.03f, s + kWallHigh - 0.1f, z + 1.1f},
                  kGrime * 1.4f, 0.95f);
        }
    }
    b.Box("grime", {kPortWall + 0.05f, s, kBackWall - 0.05f}, {kWorksWall, s + 1.3f, kBackWall + 0.05f}, kGrime, 0.95f);
    // A gantry along the inside of the port wall, either side of the gate, on brackets; its rail; a ladder up to it.
    for (const glm::vec2 run : {glm::vec2{kBayOpen + 2.0f, kGateFrom - 1.0f}, glm::vec2{kGateTo + 1.0f, 26.0f}})
    {
        b.Box("gantry", {kPortWall + 0.6f, s + 5.6f, run.x}, {kPortWall + 2.0f, s + 5.8f, run.y}, kSteel, 0.6f, 0.6f);
        b.Box("gantry_rail", {kPortWall + 1.9f, s + 6.7f, run.x}, {kPortWall + 2.0f, s + 6.8f, run.y}, kHazard, 0.6f, 0.3f);
        for (float z = run.x; z <= run.y; z += 4.0f)
        {
            b.Box("gantry_post", {kPortWall + 1.9f, s + 5.8f, z}, {kPortWall + 2.0f, s + 6.7f, z + 0.1f}, kSteelDark, 0.6f, 0.6f);
            b.Box("gantry_bracket", {kPortWall + 0.6f, s + 5.0f, z}, {kPortWall + 1.8f, s + 5.6f, z + 0.1f}, kSteelDark, 0.6f, 0.6f);
        }
    }
    b.Box("ladder", {kPortWall + 0.7f, s, kBayOpen + 2.2f}, {kPortWall + 1.2f, s + 5.6f, kBayOpen + 2.35f}, kSteelDark, 0.6f, 0.6f);
    // Conduits along the port wall's inside, high, and along the back wall low.
    for (const float y : {7.4f, 7.9f})
    {
        b.Box("conduit", {kPortWall - 0.05f, s + y, kBayOpen + 0.05f}, {kPortWall + 0.75f, s + y + 0.3f, kGateFrom}, kSteel * 1.2f, 0.4f, 0.8f);
        b.Box("conduit", {kPortWall - 0.05f, s + y, kGateTo}, {kPortWall + 0.75f, s + y + 0.3f, 27.0f}, kSteel * 1.2f, 0.4f, 0.8f);
    }
    b.Box("conduit", {-16.0f, s + 2.0f, kBackWall - 0.95f}, {kWorksWall, s + 2.5f, kBackWall - 0.6f}, kRust, 0.6f, 0.5f);
    // The floodlights on the masts: a crossbar and two heads on each, and lamps over the gate and along the gantry.
    LampHeads(b, Lamps());
    PadMastBars(b);
    // The booth's windows on the pad, and its door.
    b.Box("fx_window", {-23.2f, s + 6.2f, 27.72f}, {-21.0f, s + 7.8f, 27.86f}, kWindow, 0.3f, 0.0f, 0.9f);
    b.Box("fx_window", {-19.0f, s + 6.2f, 27.72f}, {-16.8f, s + 7.8f, 27.86f}, kWindow, 0.3f, 0.0f, 0.9f);
    b.Box("booth_door", {-20.6f, s + 5.02f, 27.72f}, {-19.4f, s + 7.3f, 27.86f}, kSteelDark, 0.6f, 0.5f);
    for (const float x : {-20.75f, -19.25f})
    {
        // A metre over the ramp's middle, as long as it is and at its slope.
        b.Box("booth_rail", {x - 0.04f, s + 3.35f, 20.4f - 6.5f}, {x + 0.04f, s + 3.45f, 20.4f + 6.5f}, kHazard, 0.6f, 0.3f).rotation = {
            -std::atan2(5.0f, 12.0f) * kDegrees, 0.0f, 0.0f};
    }

    // --- Shipworks ------------------------------------------------------------------------------------------------------
    // Its roof, its great door onto the apron with stripes along the foot, its hangar door onto the pad, lit windows high on
    // its side, and the crane on its roof.
    b.Box("shipworks_roof", {kWorksWall - 0.3f, s + 19.95f, kBayOpen - 0.3f}, {78.3f, s + 20.8f, kBackWall + 1.3f}, kSteelDark, 0.6f, 0.4f);
    b.Box("shipworks_door", {32.0f, s, kBayOpen - 0.3f}, {71.0f, s + 17.0f, kBayOpen + 0.05f}, kSteel, 0.6f, 0.5f);
    for (float x = 33.0f; x < 70.0f; x += 4.0f)
    {
        b.Box("shipworks_stripe", {x, s + 0.2f, kBayOpen - 0.38f}, {x + 1.6f, s + 1.6f, kBayOpen - 0.3f}, kHazard, 0.6f);
    }
    b.Box("shipworks_door", {kWorksWall - 0.3f, s, -24.0f}, {kWorksWall + 0.05f, s + 15.0f, 14.0f}, kSteel * 0.9f, 0.6f, 0.5f);
    for (float z = -23.0f; z < 13.0f; z += 4.0f)
    {
        b.Box("shipworks_stripe", {kWorksWall - 0.38f, s + 0.2f, z}, {kWorksWall - 0.3f, s + 1.6f, z + 1.6f}, kHazard, 0.6f);
    }
    for (float z = -38.0f; z < 34.0f; z += 6.0f)
    {
        if (z > -26.0f && z < 15.0f)
        {
            continue;
        }
        b.Box("fx_window", {kWorksWall - 0.08f, s + 16.5f, z}, {kWorksWall + 0.05f, s + 18.0f, z + 3.6f}, kWindow, 0.3f, 0.0f, 0.8f);
    }
    b.Box("shipworks_crane", {30.0f, s + 20.8f, -4.0f}, {74.0f, s + 22.0f, -2.8f}, kOrange * 0.85f, 0.6f, 0.4f);
    b.Box("shipworks_crane_mast", {30.1f, s + 20.8f, -3.9f}, {31.1f, s + 27.8f, -2.9f}, kOrange * 0.85f, 0.6f, 0.4f);
    // Pipes from the tanks into its back.
    for (const float y : {3.0f, 4.0f})
    {
        b.Cylinder("pipe", {84.5f, s + y, 40.0f}, 0.5f, 13.0f, {0.0f, 0.0f, 90.0f}, kSteel * 1.1f, 0.4f, 0.8f);
    }

    // --- The street -----------------------------------------------------------------------------------------------------
    // The road (poured in the slab: Solid): its centre line dashed, its edges lined, both broken for the crossing from the gate to Operations
    // Hall, which is striped.
    const float middle = (kPavementWest + kPavementEast) * 0.5f;
    for (float z = kStreetNorth + 2.0f; z < kStreetSouth - 3.0f; z += 8.0f)
    {
        if (z + 3.0f > kCrossFrom - 1.0f && z < kCrossTo + 1.0f)
        {
            continue;
        }
        b.Box("road_line", {middle - 0.1f, s, z}, {middle + 0.1f, paint, z + 3.0f}, kWhitePaint, 0.7f);
    }
    for (const float x : {kPavementWest + 0.5f, kPavementEast - 0.5f})
    {
        b.Box("road_line", {x - 0.1f, s, kStreetNorth + 0.2f}, {x + 0.1f, paint, kCrossFrom - 0.6f}, kWhitePaint * 0.9f, 0.7f);
        b.Box("road_line", {x - 0.1f, s, kCrossTo + 0.6f}, {x + 0.1f, paint, kStreetSouth - 0.2f}, kWhitePaint * 0.9f, 0.7f);
    }
    for (float x = kPavementWest + 0.4f; x < kPavementEast - 0.5f; x += 1.1f)
    {
        b.Box("crossing", {x, s, kCrossFrom}, {x + 0.55f, paint, kCrossTo}, kWhitePaint, 0.7f);
    }
    // The lamp posts' arms, out over the road (their heads are among the lamps).
    for (const Post& post : StreetPosts())
    {
        const float x0 = std::min(post.at.x, post.at.x + post.reach);
        const float x1 = std::max(post.at.x, post.at.x + post.reach);
        b.Box("street_arm", {x0 - 0.06f, s + 8.6f, post.at.y - 0.06f}, {x1 + 0.06f, s + 8.75f, post.at.y + 0.06f}, kSteelDark, 0.5f, 0.7f);
    }
    // Pipes over the road on the trestles, into the bay's wall one side and the buildings the other.
    for (const float z : kPipeBridges)
    {
        b.Box("trestle_beam", {kPavementWest - 1.8f, s + 8.0f, z - 0.7f}, {kPavementEast + 1.2f, s + 8.35f, z + 0.7f}, kSteel, 0.5f, 0.7f);
        const float length = (kPortWall - 0.5f) - (kFacade - 0.5f);
        const float centre = ((kPortWall - 0.5f) + (kFacade - 0.5f)) * 0.5f;
        b.Cylinder("street_pipe", {centre, s + 8.75f, z - 0.35f}, 0.7f, length, {0.0f, 0.0f, 90.0f}, kSteel * 1.15f, 0.4f, 0.8f);
        b.Cylinder("street_pipe", {centre, s + 8.65f, z + 0.35f}, 0.5f, length, {0.0f, 0.0f, 90.0f}, kRust, 0.6f, 0.5f);
    }

    // --- Operations Hall --------------------------------------------------------------------------------------------------
    // Windows in bands across its front between the ribs; the lobby's glass doors at the back of the entrance, framed; the
    // canopy over it with its light; a darker plinth along its foot; plant on its roof.
    for (const float y : {9.4f, 13.4f})
    {
        WindowsX(b, kFacade, -23.4f, 21.0f, s + y, s + y + 1.2f, 2.8f, 4.0f, 0.75f, -9.5f, 5.5f);
        WindowsX(b, kFacade, -8.6f, 4.6f, s + y, s + y + 1.2f, 3.2f, 4.0f, 0.75f);
    }
    b.Box("fx_lobby_glass", {kFacade - 2.06f, s, -6.0f}, {kFacade - 1.94f, s + 3.2f, 2.0f}, kWindow * 0.75f, 0.2f, 0.0f, 0.55f);
    for (float z = -6.0f; z <= 2.01f; z += 2.0f)
    {
        b.Box("lobby_mullion", {kFacade - 1.97f, s, z - 0.06f}, {kFacade - 1.85f, s + 3.32f, z + 0.06f}, kSteelDark, 0.5f, 0.7f);
    }
    b.Box("lobby_transom", {kFacade - 1.96f, s + 3.2f, -6.04f}, {kFacade - 1.86f, s + 3.35f, 2.04f}, kSteelDark, 0.5f, 0.7f);
    b.Box("ops_canopy", {kFacade - 0.05f, s + 5.6f, -9.5f}, {kPavementWest - 0.5f, s + 6.0f, 5.5f}, kSteel, 0.6f, 0.5f);
    b.Box("ops_plinth", {kFacade - 0.05f, s, -25.95f}, {kFacade + 0.12f, s + 0.9f, -9.05f}, kConcreteOld * 0.8f, 0.9f);
    b.Box("ops_plinth", {kFacade - 0.05f, s, 5.05f}, {kFacade + 0.12f, s + 0.9f, 21.95f}, kConcreteOld * 0.8f, 0.9f);
    for (int i = 0; i < 5; ++i)
    {
        b.Box("roof_unit", {-93.0f + 7.0f * static_cast<float>(i), s + 15.95f, 13.0f}, {-89.0f + 7.0f * static_cast<float>(i), s + 17.9f, 18.0f}, kSteel, 0.6f,
              0.5f);
    }
    b.Cylinder("stack", {-90.0f, s + 20.4f, -22.0f}, 1.4f, 9.0f, {0.0f, 0.0f, 0.0f}, kRust, 0.7f, 0.4f);
    b.Cylinder("stack", {-86.0f, s + 19.4f, -22.0f}, 1.0f, 7.0f, {0.0f, 0.0f, 0.0f}, kSteelDark, 0.7f, 0.4f);
    for (const float z : {-25.0f, 21.0f})
    {
        b.Box("ops_pipe", {kFacade - 0.05f, s, z - 0.25f}, {kFacade + 0.45f, s + 15.8f, z + 0.25f}, kSteel * 1.1f, 0.4f, 0.8f);
    }

    // --- The Annex, Crew Services and the Relay ------------------------------------------------------------------------
    WindowsX(b, kFacade, -67.0f, -39.0f, s + 3.0f, s + 4.2f, 2.4f, 3.2f, 0.85f, -56.0f, -51.0f);
    b.Box("fx_door", {kFacade - 0.05f, s, -55.0f}, {kFacade + 0.08f, s + 2.6f, -52.0f}, kWindow * 0.6f, 0.3f, 0.0f, 0.8f);
    WindowsX(b, kFacade + 2.0f, -61.0f, -51.0f, s + 8.6f, s + 10.2f, 2.0f, 2.6f, 0.9f);
    for (const float y : {3.0f, 6.4f})
    {
        WindowsX(b, kFacade, 31.0f, 57.0f, s + y, s + y + 1.4f, 2.6f, 4.0f, 0.8f, 39.0f, 45.0f);
    }
    b.Box("fx_door", {kFacade - 0.05f, s, 40.5f}, {kFacade + 0.08f, s + 2.8f, 43.5f}, kWindow * 0.6f, 0.3f, 0.0f, 0.8f);
    b.Box("crew_canopy", {kFacade - 0.05f, s + 3.1f, 40.0f}, {kFacade + 1.4f, s + 3.3f, 44.0f}, kSteel, 0.6f, 0.5f);
    for (float x = -56.0f; x < -30.0f; x += 4.0f)
    {
        if (x > -48.0f && x < -42.0f)
        {
            continue;
        }
        b.Box("fx_window", {x, s + 3.0f, kStreetSouth - 0.07f}, {x + 2.6f, s + 4.4f, kStreetSouth + 0.05f}, kWindow, 0.3f, 0.0f, 0.85f);
    }
    b.Box("fx_door", {-47.0f, s, kStreetSouth - 0.08f}, {-44.0f, s + 2.8f, kStreetSouth + 0.05f}, kWindow * 0.6f, 0.3f, 0.0f, 0.8f);
    for (int i = 1; i < 8; ++i)
    {
        const float y = s + 5.0f * static_cast<float>(i);
        b.Box("relay_brace", {-72.0f, y, 95.8f}, {-66.0f, y + 0.25f, 96.2f}, kSteel, 0.5f, 0.7f);
        b.Box("relay_brace", {-72.0f, y, 101.8f}, {-66.0f, y + 0.25f, 102.2f}, kSteel, 0.5f, 0.7f);
        b.Box("relay_brace", {-72.2f, y + 0.3f, 96.0f}, {-71.8f, y + 0.55f, 102.0f}, kSteel, 0.5f, 0.7f);
        b.Box("relay_brace", {-66.2f, y + 0.3f, 96.0f}, {-65.8f, y + 0.55f, 102.0f}, kSteel, 0.5f, 0.7f);
    }
    b.Cylinder("relay_dish", {-69.0f, s + 39.0f, 94.0f}, 9.0f, 0.4f, {60.0f, 0.0f, 0.0f}, kPanel, 0.4f, 0.3f);
    b.Box("fx_relay_light", {-69.3f, s + 42.0f, 98.7f}, {-68.7f, s + 42.6f, 99.3f}, {1.0f, 0.12f, 0.08f}, 0.3f, 0.0f, 6.0f);
    // The barrier arm at the gatehouse.
    b.Box("barrier_arm", {kPavementWest - 0.4f, s + 0.9f, -145.2f}, {kPavementEast, s + 1.05f, -145.0f}, kHazard, 0.6f);

    // --- Salvage Intake: its doors over the dock, the gantry crane over the containers ---------------------------------
    for (int i = 0; i < 4; ++i)
    {
        const float x = 36.0f + 13.0f * static_cast<float>(i);
        b.Box("salvage_door", {x, s + 1.3f, -80.05f}, {x + 9.0f, s + 7.5f, -79.9f}, kSteel * 1.1f, 0.6f, 0.5f);
        b.Box("salvage_door_stripe", {x, s + 7.5f, -80.05f}, {x + 9.0f, s + 7.8f, -79.9f}, kHazard, 0.6f);
    }
    for (const float x : {95.0f, 138.0f})
    {
        b.Box("crane_beam", {x - 0.5f, s + 14.0f, -114.0f}, {x + 0.5f, s + 15.0f, -61.0f}, kOrange * 0.85f, 0.6f, 0.4f);
    }
    b.Box("crane_bridge", {95.5f, s + 15.0f, -90.0f}, {137.5f, s + 16.0f, -88.5f}, kOrange * 0.9f, 0.6f, 0.4f);
    b.Box("crane_hoist", {114.0f, s + 9.0f, -89.6f}, {115.4f, s + 15.0f, -88.9f}, kSteelDark, 0.5f, 0.6f);
    // The apron's taxi line from the bay out past Salvage Intake.
    b.Box("apron_line", {-0.2f, s, -150.0f}, {0.2f, paint, kBayOpen - 0.5f}, kHazard * 0.9f, 0.7f);

    // Some of the ground scuffed and patched round the slab, so it is not one colour to the hills -- never on the slab.
    for (int i = 0; i < 24; ++i)
    {
        const float angle = static_cast<float>(i) * 2.39996f + 0.7f;
        const float reach = 190.0f + 6.0f * static_cast<float>(i);
        const glm::vec3 at{std::cos(angle) * reach, kG, std::sin(angle) * reach};
        const float wide = 16.0f + static_cast<float>((i * 11) % 7) * 6.0f;
        if (std::abs(at.x) < 140.0f + wide * 1.2f && at.z > -160.0f - wide * 1.2f && at.z < 120.0f + wide * 1.2f)
        {
            continue;
        }
        // Raised a little each, and each by a different amount, so where they overlap their tops never lie together, nor on
        // the ground's -- this far off the picture's depth needs a hand's breadth between surfaces, not centimetres.
        ModelPart& patch = b.Box("patch", at - glm::vec3(wide, 0.1f, wide * 0.6f), at + glm::vec3(wide, 0.2f + 0.04f * static_cast<float>(i), wide * 0.6f),
                                 ground * (0.82f + 0.06f * static_cast<float>((i * 5) % 7)), 0.95f);
        patch.rotation = {0.0f, angle * kDegrees, 0.0f};
    }
    return model;
}

// --- Its lamps -------------------------------------------------------------------------------------------------------

std::vector<Lamp> Lamps()
{
    const float s = kSlabTop;
    // The bay's masts, two heads each, on the pad.
    std::vector<Lamp> lamps = PadMastLamps();
    // Along the gantry on the port wall, down onto the floor by it.
    for (const float z : {-30.0f, 10.0f, 22.0f})
    {
        const glm::vec3 at{kPortWall + 1.2f, s + 7.0f, z};
        lamps.push_back({at, glm::normalize(glm::vec3(1.0f, -1.6f, 0.0f)), kFlood, 80.0f, 26.0f, 50.0f, 90.0f});
    }
    // Over the gate, inside and out.
    for (const float x : {kPortWall + 0.4f, kPortWall - 1.4f})
    {
        lamps.push_back({{x, s + 6.6f, (kGateFrom + kGateTo) * 0.5f}, {0.0f, -1.0f, 0.0f}, kSodium, 30.0f, 16.0f, 70.0f, 120.0f});
    }
    // Over Shipworks' hangar door.
    for (const float z : {-18.0f, 8.0f})
    {
        const glm::vec3 at{kWorksWall - 0.8f, s + 15.6f, z};
        lamps.push_back({at, glm::normalize(glm::vec3(-1.0f, -1.4f, 0.0f)), kFlood, 60.0f, 30.0f, 45.0f, 85.0f});
    }
    // The street's lamps, at the ends of their arms.
    for (const Post& post : StreetPosts())
    {
        lamps.push_back({{post.at.x + post.reach, s + 8.45f, post.at.y}, {0.0f, -1.0f, 0.0f}, kSodium, 42.0f, 20.0f, 70.0f, 120.0f});
    }
    // Operations Hall's canopy; the other doors' lamps.
    lamps.push_back({{kPavementWest - 1.4f, s + 5.4f, -2.0f}, {0.0f, -1.0f, 0.0f}, kSodium, 26.0f, 13.0f, 90.0f, 150.0f});
    lamps.push_back({{kFacade + 0.6f, s + 3.0f, 42.0f}, glm::normalize(glm::vec3(1.0f, -1.2f, 0.0f)), kSodium, 14.0f, 10.0f, 80.0f, 140.0f});
    lamps.push_back({{kFacade + 0.6f, s + 3.2f, -53.5f}, glm::normalize(glm::vec3(1.0f, -1.2f, 0.0f)), kSodium, 14.0f, 10.0f, 80.0f, 140.0f});
    lamps.push_back({{-45.5f, s + 3.4f, kStreetSouth - 0.6f}, glm::normalize(glm::vec3(0.0f, -1.2f, -1.0f)), kSodium, 14.0f, 10.0f, 80.0f, 140.0f});
    // Over Salvage Intake's docks.
    for (int i = 0; i < 4; ++i)
    {
        const float x = 40.5f + 13.0f * static_cast<float>(i);
        lamps.push_back({{x, s + 7.9f, -79.4f}, glm::normalize(glm::vec3(0.0f, -1.0f, 0.6f)), kSodium, 34.0f, 18.0f, 70.0f, 120.0f});
    }
    // The apron's masts.
    for (const glm::vec2 at : kApronMasts)
    {
        lamps.push_back({{at.x, s + 19.8f, at.y}, {0.0f, -1.0f, 0.0f}, kFlood, 320.0f, 55.0f, 80.0f, 130.0f});
    }
    return lamps;
}

// --- Signs -----------------------------------------------------------------------------------------------------------

std::vector<Sign> Signs(bool named)
{
    const float s = kSlabTop;
    std::vector<Sign> signs;
    // Over Operations Hall's entrance, facing the street (+x): the mark over the canopy, the station's name above it.
    signs.push_back({"logo_ops", {}, {kFacade + 0.12f, s + 7.7f, -2.0f}, 90.0f, 11.0f, 2.8f, Sign::Style::Logo});
    if (named)
    {
        signs.push_back({"station_name", {"KESTREL STATION"}, {kFacade + 0.12f, s + 11.6f, -2.0f}, 90.0f, 13.0f, 1.6f, Sign::Style::Painted});
    }
    signs.push_back({"ops_hall", {"OPERATIONS HALL"}, {kPavementWest - 0.42f, s + 5.25f, -2.0f}, 90.0f, 6.0f, 0.7f, Sign::Style::Lit});
    signs.push_back({"crew", {"CREW SERVICES"}, {kFacade + 0.12f, s + 4.3f, 42.0f}, 90.0f, 6.0f, 0.8f, Sign::Style::Lit});
    signs.push_back({"annex", {"RESEARCH ANNEX"}, {kFacade + 0.12f, s + 5.4f, -53.5f}, 90.0f, 6.5f, 0.8f, Sign::Style::Lit});
    signs.push_back({"relay", {"NAVIGATION RELAY"}, {-45.5f, s + 5.6f, kStreetSouth - 0.1f}, 180.0f, 9.0f, 1.0f, Sign::Style::Lit});
    signs.push_back({"salvage", {"SALVAGE INTAKE"}, {60.0f, s + 9.0f, -79.85f}, 0.0f, 14.0f, 1.4f, Sign::Style::Lit});
    signs.push_back({"shipworks", {"SHIPWORKS"}, {51.5f, s + 18.4f, kBayOpen - 0.35f}, 180.0f, 16.0f, 2.2f, Sign::Style::Painted});
    signs.push_back({"logo_shipworks", {}, {kWorksWall - 0.35f, s + 17.3f, -5.0f}, -90.0f, 8.0f, 2.0f, Sign::Style::Logo});
    signs.push_back({"shipworks_side", {"SHIPWORKS"}, {kWorksWall - 0.35f, s + 15.9f, -5.0f}, -90.0f, 9.0f, 1.1f, Sign::Style::Lit});
    // Hangar Row: on the bay's walls, inside by the gate and outside on the street, and the bay's number on its pad.
    signs.push_back({"row_inside", {"HANGAR ROW", "BAY 02"}, {kPortWall + 0.05f, s + 3.6f, 8.0f}, 90.0f, 4.4f, 1.8f, Sign::Style::Painted});
    signs.push_back({"row_street", {"HANGAR ROW", "BAY 02"}, {kPortWall - 1.05f, s + 4.2f, 8.0f}, -90.0f, 5.2f, 2.1f, Sign::Style::Painted});
    signs.push_back({"bay_number", {"02"}, {0.0f, s + kPaint + 0.01f, -24.0f}, 180.0f, 8.0f, 5.0f, Sign::Style::Floor});
    signs.push_back({"bay_logo", {}, {0.0f, s + 6.2f, kBackWall - 0.62f}, 180.0f, 9.0f, 2.2f, Sign::Style::Logo});
    return signs;
}

void DrawLogo(ScreenCanvas& canvas, float x, float y, float width, float height, Rgb letters, Rgb bar)
{
    // Five squared letters, as the mark has them: C, I, R, R and an A with no crossbar, the orange bar low inside it.
    const float h = height;
    const float t = h * 0.17f;            // a stroke
    const float w = h * 0.86f;            // a letter's width
    const float gap = h * 0.16f;
    const float total = w * 3.0f + t + w * 1.25f + gap * 4.0f;
    const float scale = std::min(1.0f, width / total);
    const float hh = h * scale;
    const float tt = t * scale;
    const float ww = w * scale;
    const float gg = gap * scale;
    float cx = x + (width - (ww * 3.0f + tt + ww * 1.25f + gg * 4.0f)) * 0.5f;
    const float top = y + (height - hh) * 0.5f;
    const float bottom = top + hh;
    const float r = tt * 0.9f; // the corners' rounding
    const auto rounded = [&](float x0, float y0, float x1, float y1)
    {
        canvas.Fill(x0 + r, y0, x1 - r, y1, letters);
        canvas.Fill(x0, y0 + r, x1, y1 - r, letters);
        canvas.Dot(x0 + r, y0 + r, r, letters);
        canvas.Dot(x1 - r, y0 + r, r, letters);
        canvas.Dot(x0 + r, y1 - r, r, letters);
        canvas.Dot(x1 - r, y1 - r, r, letters);
    };
    // C: a rounded bar down the left, bars across the top and bottom.
    rounded(cx, top, cx + tt * 1.6f, bottom);
    canvas.Fill(cx + tt, top, cx + ww, top + tt, letters);
    canvas.Fill(cx + tt, bottom - tt, cx + ww, bottom, letters);
    cx += ww + gg;
    // I.
    canvas.Fill(cx, top, cx + tt, bottom, letters);
    cx += tt + gg;
    // R, twice: the stem, the bowl (top, right side, middle), and the leg down to the right.
    for (int i = 0; i < 2; ++i)
    {
        const float mid = top + hh * 0.56f;
        canvas.Fill(cx, top, cx + tt, bottom, letters);
        canvas.Fill(cx, top, cx + ww - r, top + tt, letters);
        rounded(cx + ww - tt * 1.6f, top, cx + ww, mid);
        canvas.Fill(cx, mid - tt, cx + ww - r, mid, letters);
        canvas.FillQuad({cx + ww * 0.32f, mid - tt}, {cx + ww * 0.32f + tt * 1.3f, mid - tt}, {cx + ww + tt * 0.2f, bottom}, {cx + ww - tt * 1.1f, bottom},
                        letters);
        cx += ww + gg;
    }
    // A: two legs meeting at the top; the orange bar between them, low, cut to their slope.
    const float aw = ww * 1.25f;
    const glm::vec2 apex{cx + aw * 0.5f, top};
    canvas.FillQuad({apex.x - tt * 0.6f, apex.y}, {apex.x + tt * 0.6f, apex.y}, {cx + tt * 1.3f, bottom}, {cx, bottom}, letters);
    canvas.FillQuad({apex.x - tt * 0.6f, apex.y}, {apex.x + tt * 0.6f, apex.y}, {cx + aw, bottom}, {cx + aw - tt * 1.3f, bottom}, letters);
    const float barTop = top + hh * 0.72f;
    const float barBottom = top + hh * 0.86f;
    const auto inner = [&](float yy, bool left)
    {
        const float along = (yy - top) / hh;
        return left ? apex.x - (apex.x - cx - tt * 1.3f) * along + tt * 0.2f : apex.x + (cx + aw - tt * 1.3f - apex.x) * along - tt * 0.2f;
    };
    canvas.FillQuad({inner(barTop, true), barTop}, {inner(barTop, false), barTop}, {inner(barBottom, false), barBottom}, {inner(barBottom, true), barBottom},
                    bar);
}

void Draw(ScreenCanvas& canvas, const Sign& sign)
{
    const Rgb light{226, 228, 224};
    const Rgb orange{238, 116, 30};
    if (sign.style == Sign::Style::Logo)
    {
        canvas.Clear({22, 24, 26});
        canvas.Box(2.0f, 2.0f, static_cast<float>(canvas.width - 3), static_cast<float>(canvas.height - 3), {60, 62, 64});
        DrawLogo(canvas, canvas.width * 0.06f, canvas.height * 0.18f, canvas.width * 0.88f, canvas.height * 0.64f, light, orange);
        return;
    }
    const bool painted = sign.style == Sign::Style::Painted;
    const bool floor = sign.style == Sign::Style::Floor;
    // Painted on the floor: worn white on the pad's dark, nothing round it.
    canvas.Clear(painted ? Rgb{148, 146, 140} : floor ? Rgb{132, 132, 132} : Rgb{18, 20, 22});
    const Rgb ink = painted ? Rgb{34, 34, 34} : floor ? Rgb{236, 236, 226} : light;
    if (!painted && !floor)
    {
        canvas.Box(3.0f, 3.0f, static_cast<float>(canvas.width - 4), static_cast<float>(canvas.height - 4), {80, 78, 70});
        canvas.Fill(6.0f, static_cast<float>(canvas.height) - 12.0f, 46.0f, static_cast<float>(canvas.height) - 8.0f, orange);
    }
    // As big as fits: the widest line across, the lines down.
    const int lines = static_cast<int>(sign.lines.size());
    if (lines == 0)
    {
        return;
    }
    size_t longest = 1;
    for (const std::string& line : sign.lines)
    {
        longest = std::max(longest, line.size());
    }
    const int byWidth = static_cast<int>((canvas.width * 0.86f) / (static_cast<float>(longest) * 6.0f));
    const int byHeight = static_cast<int>((canvas.height * 0.74f) / (static_cast<float>(lines) * 9.0f));
    const int scale = std::max(1, std::min(byWidth, byHeight));
    const int lineHeight = 9 * scale;
    int y = (canvas.height - lineHeight * lines) / 2 + scale;
    for (const std::string& line : sign.lines)
    {
        const int width = canvas.TextWidth(line.c_str(), scale);
        canvas.Text((canvas.width - width) / 2, y, line.c_str(), ink, scale);
        y += lineHeight;
    }
}

glm::vec3 Spawn(int player)
{
    // On the walkway beside the stair's foot, two by two, looking at the ship.
    const float x = -11.0f - static_cast<float>((player / 2) % 4) * 1.2f;
    const float z = (kCrossFrom + kCrossTo) * 0.5f + (static_cast<float>(player % 2) - 0.5f) * 1.1f;
    return ShipMap::ToWorld({x, kSlabTop + 0.1f, z});
}

float SpawnYaw()
{
    // Facing +x, at the ship.
    return 1.5707963f;
}

} // namespace KestrelStation

} // namespace pred
