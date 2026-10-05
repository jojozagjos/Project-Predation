#include "Game/World/KestrelStation.h"

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
constexpr float kSlabTop = kG + 0.12f;       // the station's concrete, walked on
constexpr float kDegrees = 57.2957795f;

// The station's colours: weathered concrete and painted steel, the old darker than the new; amber sodium light and the
// cooler white of floodlights; hazard yellow; CIRRA's orange.
const glm::vec3 kConcrete{0.40f, 0.39f, 0.36f};
const glm::vec3 kConcreteOld{0.30f, 0.29f, 0.27f};
const glm::vec3 kSteel{0.36f, 0.38f, 0.40f};
const glm::vec3 kSteelDark{0.17f, 0.18f, 0.19f};
const glm::vec3 kPanel{0.46f, 0.46f, 0.44f};
const glm::vec3 kRust{0.42f, 0.24f, 0.14f};
const glm::vec3 kHazard{0.80f, 0.62f, 0.12f};
const glm::vec3 kOrange{0.93f, 0.45f, 0.12f};
const glm::vec3 kGrime{0.16f, 0.15f, 0.14f};
const glm::vec3 kSodium{1.0f, 0.72f, 0.36f};
const glm::vec3 kFlood{0.92f, 0.95f, 1.0f};
const glm::vec3 kWindow{1.0f, 0.86f, 0.62f};
const glm::vec3 kGlass{0.12f, 0.16f, 0.18f};

struct Builder
{
    ModelAsset& model;
    int count = 0;

    std::string Name(const char* stem) { return std::string(stem) + "_" + std::to_string(count++); }

    ModelPart& Box(const char* stem, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour, float rough = 0.8f, float metal = 0.0f,
                   float emissive = 0.0f)
    {
        ModelPart part;
        part.name = Name(stem);
        part.shape = PartShape::Box;
        part.position = (lo + hi) * 0.5f;
        part.size = glm::abs(hi - lo);
        part.color = colour;
        part.roughness = rough;
        part.metallic = metal;
        part.emissive = emissive;
        model.parts.push_back(part);
        return model.parts.back();
    }
    // A box about its middle, turned about up by `yaw` degrees and tipped about its own x by `tip`.
    ModelPart& Turned(const char* stem, const glm::vec3& centre, const glm::vec3& size, float yaw, const glm::vec3& colour, float tip = 0.0f,
                      float rough = 0.8f, float metal = 0.0f, float emissive = 0.0f)
    {
        ModelPart& part = Box(stem, centre - size * 0.5f, centre + size * 0.5f, colour, rough, metal, emissive);
        part.rotation = {tip, yaw, 0.0f};
        return part;
    }
    ModelPart& Cylinder(const char* stem, const glm::vec3& centre, float diameter, float length, const glm::vec3& rotation, const glm::vec3& colour,
                        float rough = 0.6f, float metal = 0.0f, float emissive = 0.0f)
    {
        ModelPart& part = Box(stem, centre - glm::vec3(diameter * 0.5f, length * 0.5f, diameter * 0.5f),
                              centre + glm::vec3(diameter * 0.5f, length * 0.5f, diameter * 0.5f), colour, rough, metal, emissive);
        part.shape = PartShape::Cylinder;
        part.rotation = rotation;
        return part;
    }
};

// A building as a block: walls and a roof edge, a plinth of older concrete, grime along its foot.
void Block(Builder& b, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour)
{
    b.Box("building", lo, hi, colour, 0.85f);
    b.Box("building_parapet", {lo.x - 0.2f, hi.y, lo.z - 0.2f}, {hi.x + 0.2f, hi.y + 0.7f, hi.z + 0.2f}, colour * 0.8f, 0.85f);
    b.Box("building_plinth", {lo.x - 0.15f, kSlabTop, lo.z - 0.15f}, {hi.x + 0.15f, kSlabTop + 1.0f, hi.z + 0.15f}, kConcreteOld, 0.9f);
}

} // namespace

// --- What is solid -------------------------------------------------------------------------------------------------

ModelAsset Solid(const glm::vec3& ground, const glm::vec3& rock)
{
    ModelAsset model;
    model.name = "kestrel";
    Builder b{model};

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

    // The station's slab: everything inside the fence is concrete.
    b.Box("slab", {-140.0f, kG, -160.0f}, {140.0f, kSlabTop, 120.0f}, kConcrete * 0.78f, 0.9f);

    // --- Hangar Row: the ship's bay, open to the sky, a gate to port onto the street ---------------------------------
    const float wallHigh = kSlabTop + 15.0f;
    b.Box("bay_wall", {-25.0f, kSlabTop, -40.0f}, {-24.0f, wallHigh, -16.0f}, kConcreteOld, 0.9f);
    b.Box("bay_wall", {-25.0f, kSlabTop, 0.0f}, {-24.0f, wallHigh, 36.0f}, kConcreteOld, 0.9f);
    b.Box("bay_lintel", {-25.0f, kSlabTop + 9.0f, -16.0f}, {-24.0f, wallHigh, 0.0f}, kConcreteOld, 0.9f);
    b.Box("bay_wall", {24.0f, kSlabTop, -40.0f}, {25.0f, wallHigh, 36.0f}, kConcreteOld, 0.9f);
    b.Box("bay_wall", {-25.0f, kSlabTop, 35.0f}, {25.0f, wallHigh, 36.0f}, kConcrete, 0.9f);
    // Buttresses down the walls, the newer steel bolted onto the older concrete.
    for (float z = -36.0f; z < 34.0f; z += 10.0f)
    {
        if (z > -18.0f && z < 2.0f)
        {
            continue;
        }
        b.Box("bay_buttress", {-24.0f, kSlabTop, z - 0.4f}, {-23.4f, wallHigh - 0.5f, z + 0.4f}, kSteel, 0.6f, 0.5f);
        b.Box("bay_buttress", {23.4f, kSlabTop, z - 0.4f}, {24.0f, wallHigh - 0.5f, z + 0.4f}, kSteel, 0.6f, 0.5f);
    }

    // --- The ship's boarding stair: a landing at its door, a ramp down to the bay's floor, rails either side ----------
    const float from = kAirlockFrom;
    const float to = kAirlockTo;
    b.Box("stair_landing", {-5.3f, -0.2f, from - 0.2f}, {-3.6f, 0.0f, to + 0.2f}, kSteel, 0.6f, 0.6f);
    {
        // From the landing's edge (x -5.3, deck height) down to the slab, 3.8 metres out.
        const float run = 3.8f;
        const float drop = 0.0f - kSlabTop;
        const float length = std::sqrt(run * run + drop * drop);
        const float slope = std::atan2(drop, run) * kDegrees;
        const glm::vec3 middle{-5.3f - run * 0.5f, (0.0f + kSlabTop) * 0.5f - 0.1f, (from + to) * 0.5f};
        ModelPart& ramp = b.Box("stair_ramp", middle - glm::vec3(length * 0.5f, 0.1f, (to - from) * 0.5f + 0.2f),
                                middle + glm::vec3(length * 0.5f, 0.1f, (to - from) * 0.5f + 0.2f), kSteel, 0.6f, 0.6f);
        // Down towards -x: turned about z, its outer end low.
        ramp.rotation = {0.0f, 0.0f, slope};
        for (const float z : {from - 0.3f, to + 0.3f})
        {
            ModelPart& rail = b.Box("stair_rail", middle + glm::vec3(-length * 0.5f, 0.95f, -0.04f) + glm::vec3(0.0f, 0.0f, z - middle.z),
                                    middle + glm::vec3(length * 0.5f, 1.0f, 0.04f) + glm::vec3(0.0f, 0.0f, z - middle.z), kHazard, 0.6f, 0.3f);
            rail.rotation = {0.0f, 0.0f, slope};
            b.Box("stair_post", {-5.3f, 0.0f, z - 0.04f}, {-5.22f, 1.0f, z + 0.04f}, kSteelDark, 0.5f, 0.7f);
            b.Box("stair_post", {-9.1f, kSlabTop, z - 0.04f}, {-9.02f, kSlabTop + 1.0f, z + 0.04f}, kSteelDark, 0.5f, 0.7f);
        }
        b.Box("stair_leg", {-5.3f, kSlabTop, from - 0.1f}, {-5.1f, -0.2f, from + 0.1f}, kSteelDark, 0.5f, 0.7f);
        b.Box("stair_leg", {-5.3f, kSlabTop, to - 0.1f}, {-5.1f, -0.2f, to + 0.1f}, kSteelDark, 0.5f, 0.7f);
    }

    // --- Shipworks, beside the bay to starboard: a big shed, its great door to the apron, a crane on its roof ----------
    b.Box("shipworks", {25.0f, kSlabTop, -40.0f}, {78.0f, kSlabTop + 22.0f, 36.0f}, kPanel * 0.85f, 0.75f, 0.2f);
    b.Box("shipworks_roof", {24.6f, kSlabTop + 22.0f, -40.4f}, {78.4f, kSlabTop + 23.0f, 36.4f}, kSteelDark, 0.6f, 0.4f);
    b.Box("shipworks_door", {32.0f, kSlabTop, -40.4f}, {71.0f, kSlabTop + 17.0f, -40.0f}, kSteel, 0.6f, 0.5f);

    // --- The street, to port through the gate: Operations Hall, Crew Services, the Research Annex, the Navigation Relay
    // Operations Hall: ribbed concrete, the old block and the newer one bolted to it, silo towers along its top.
    Block(b, {-100.0f, kSlabTop, -30.0f}, {-56.0f, kSlabTop + 17.0f, 26.0f}, kConcrete);
    Block(b, {-56.0f, kSlabTop, -14.0f}, {-52.0f, kSlabTop + 6.5f, 10.0f}, kConcreteOld); // the entrance block, out into the street
    for (int i = 0; i < 4; ++i)
    {
        b.Cylinder("ops_silo", {-62.0f, kSlabTop + 21.5f, -20.0f + 13.0f * static_cast<float>(i)}, 6.5f, 9.0f, {0.0f, 0.0f, 0.0f}, kConcreteOld, 0.85f);
    }
    // Crew Services.
    Block(b, {-92.0f, kSlabTop, 34.0f}, {-56.0f, kSlabTop + 10.0f, 62.0f}, kPanel * 0.8f);
    // The Research Annex: modules stacked and added to, an upper one overhanging.
    Block(b, {-86.0f, kSlabTop, -76.0f}, {-56.0f, kSlabTop + 7.0f, -42.0f}, kPanel * 0.9f);
    b.Box("annex_module", {-82.0f, kSlabTop + 7.0f, -70.0f}, {-58.0f, kSlabTop + 12.5f, -50.0f}, kPanel * 0.75f, 0.8f, 0.2f);
    b.Box("annex_module", {-60.0f, kSlabTop + 7.0f, -66.0f}, {-53.0f, kSlabTop + 11.0f, -56.0f}, kSteel, 0.7f, 0.4f);
    // The Navigation Relay: a low block at the street's end, its lattice mast and dish beside it.
    Block(b, {-58.0f, kSlabTop, 92.0f}, {-30.0f, kSlabTop + 8.0f, 110.0f}, kConcreteOld);
    for (const glm::vec2 leg : {glm::vec2{-72.0f, 96.0f}, glm::vec2{-66.0f, 96.0f}, glm::vec2{-72.0f, 102.0f}, glm::vec2{-66.0f, 102.0f}})
    {
        b.Box("relay_leg", {leg.x - 0.3f, kSlabTop, leg.y - 0.3f}, {leg.x + 0.3f, kSlabTop + 42.0f, leg.y + 0.3f}, kSteel, 0.5f, 0.7f);
    }

    // --- Salvage Intake, out on the apron: the shed, its docks, and the containers waiting to be gone through ---------
    Block(b, {30.0f, kSlabTop, -112.0f}, {90.0f, kSlabTop + 10.0f, -78.0f}, kPanel * 0.8f);
    b.Box("salvage_dock", {34.0f, kSlabTop, -78.0f}, {86.0f, kSlabTop + 1.3f, -72.0f}, kConcreteOld, 0.9f);
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
            const float y = kSlabTop + 2.6f * static_cast<float>(level);
            b.Box("container", {x, y, z}, {x + 2.44f, y + 2.59f, z + 12.2f}, boxes[(i + level * 2) % 5], 0.7f, 0.3f);
        }
    }

    // --- The tanks behind the bays, and the fence round it all ------------------------------------------------------
    for (int i = 0; i < 3; ++i)
    {
        const glm::vec3 at{96.0f + 14.0f * static_cast<float>(i), kSlabTop + 6.0f, 40.0f};
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
            const glm::vec3 lo = alongX ? glm::vec3(a, kSlabTop, fixed - 0.05f) : glm::vec3(fixed - 0.05f, kSlabTop, a);
            const glm::vec3 hi = alongX ? glm::vec3(b2, kSlabTop + 3.4f, fixed + 0.05f) : glm::vec3(fixed + 0.05f, kSlabTop + 3.4f, b2);
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
    Builder b{model};
    const float s = kSlabTop;

    // The bay's floor: the pad's square, hazard edges, guide lines, lamps sunk round it.
    b.Box("pad", {-18.0f, s, -30.0f}, {18.0f, s + 0.012f, 30.0f}, kConcrete * 0.62f, 0.85f);
    for (const float x : {-18.0f, 18.0f})
    {
        b.Box("pad_edge", {x - 0.4f, s + 0.012f, -30.0f}, {x + 0.4f, s + 0.02f, 30.0f}, kHazard, 0.7f);
        for (float z = -28.0f; z <= 28.0f; z += 8.0f)
        {
            b.Box("fx_pad_lamp", {x - 0.25f, s, z - 0.25f}, {x + 0.25f, s + 0.12f, z + 0.25f}, kFlood, 0.3f, 0.0f, 3.0f);
        }
    }
    b.Box("pad_edge", {-18.0f, s + 0.012f, -30.4f}, {18.0f, s + 0.02f, -29.6f}, kHazard, 0.7f);
    b.Box("pad_line", {-0.2f, s + 0.012f, -40.0f}, {0.2f, s + 0.02f, -31.0f}, {0.85f, 0.85f, 0.82f}, 0.7f);
    // A walkway painted from the stair to the gate.
    b.Box("walkway", {-23.5f, s + 0.012f, -8.4f}, {-9.2f, s + 0.02f, -5.4f}, glm::vec3(0.22f, 0.36f, 0.42f), 0.8f);
    // Treads on the stair, for the look of steps on its ramp.
    for (int i = 0; i < 12; ++i)
    {
        const float t = (static_cast<float>(i) + 0.5f) / 12.0f;
        const float x = -5.3f - 3.8f * t;
        const float y = (1.0f - t) * 0.0f + t * s;
        b.Box("stair_tread", {x - 0.05f, y + 0.005f, kAirlockFrom - 0.15f}, {x + 0.05f, y + 0.045f, kAirlockTo + 0.15f}, kHazard * 0.7f, 0.7f);
    }

    // Floodlights high on the bay's walls, cool white, looking down at the ship.
    for (float z = -30.0f; z <= 30.0f; z += 15.0f)
    {
        for (const float x : {-23.2f, 23.2f})
        {
            b.Box("flood_arm", {std::min(x, x * 0.96f) - 0.1f, s + 13.5f, z - 0.1f}, {std::max(x, x * 0.96f) + 0.1f, s + 13.7f, z + 0.1f}, kSteelDark, 0.5f, 0.7f);
            b.Box("fx_flood", {x * 0.95f - 0.5f, s + 13.0f, z - 0.7f}, {x * 0.95f + 0.5f, s + 13.4f, z + 0.7f}, kFlood, 0.3f, 0.0f, 4.0f);
        }
    }
    // A gantry along the inside of each side wall, with its rail, and ladders up to it.
    for (const float x : {-23.0f, 21.5f})
    {
        b.Box("gantry", {x, s + 8.0f, -38.0f}, {x + 1.5f, s + 8.2f, 34.0f}, kSteel, 0.6f, 0.6f);
        b.Box("gantry_rail", {x < 0.0f ? x + 1.4f : x, s + 9.1f, -38.0f}, {x < 0.0f ? x + 1.5f : x + 0.1f, s + 9.2f, 34.0f}, kHazard, 0.6f, 0.3f);
        for (float z = -36.0f; z < 34.0f; z += 4.0f)
        {
            b.Box("gantry_post", {x < 0.0f ? x + 1.4f : x, s + 8.2f, z}, {x < 0.0f ? x + 1.5f : x + 0.1f, s + 9.1f, z + 0.1f}, kSteelDark, 0.6f, 0.6f);
        }
        b.Box("ladder", {x < 0.0f ? x + 0.2f : x + 0.8f, s, 32.0f}, {x < 0.0f ? x + 0.7f : x + 1.3f, s + 8.0f, 32.15f}, kSteelDark, 0.6f, 0.6f);
    }
    // Conduits along the walls: two runs high on each, a third low on the back wall.
    for (const float y : {11.5f, 12.3f})
    {
        b.Box("conduit", {-23.9f, s + y, -40.0f}, {-23.6f, s + y + 0.3f, -16.0f}, kSteel * 1.2f, 0.4f, 0.8f);
        b.Box("conduit", {-23.9f, s + y, 0.0f}, {-23.6f, s + y + 0.3f, 35.0f}, kSteel * 1.2f, 0.4f, 0.8f);
        b.Box("conduit", {23.6f, s + y, -40.0f}, {23.9f, s + y + 0.3f, 35.0f}, kSteel * 1.2f, 0.4f, 0.8f);
    }
    b.Box("conduit", {-24.0f, s + 2.0f, 34.6f}, {24.0f, s + 2.5f, 34.95f}, kRust, 0.6f, 0.5f);
    // Grime: the foot of every wall darker, streaks down from the tops.
    b.Box("grime", {-24.02f, s, -40.0f}, {-23.98f, s + 1.8f, 35.0f}, kGrime, 0.95f);
    b.Box("grime", {23.98f, s, -40.0f}, {24.02f, s + 1.8f, 35.0f}, kGrime, 0.95f);
    for (float z = -32.0f; z < 34.0f; z += 9.0f)
    {
        b.Box("streak", {-23.99f, s + 6.0f + std::fmod(std::abs(z) * 0.37f, 3.0f), z}, {-23.97f, s + 14.6f, z + 1.2f}, kGrime * 1.4f, 0.95f);
    }

    // --- The street: sodium lamps down both sides, bollards, the Operations Hall's face ------------------------------
    for (float z = -110.0f; z <= 90.0f; z += 20.0f)
    {
        for (const float x : {-27.0f, -54.0f})
        {
            const float lean = x < -40.0f ? 1.4f : -1.4f;
            b.Box("street_mast", {x - 0.15f, s, z - 0.15f}, {x + 0.15f, s + 9.0f, z + 0.15f}, kSteelDark, 0.5f, 0.7f);
            b.Box("street_arm", {std::min(x, x + lean) - 0.08f, s + 8.8f, z - 0.08f}, {std::max(x, x + lean) + 0.08f, s + 9.0f, z + 0.08f}, kSteelDark,
                  0.5f, 0.7f);
            b.Box("fx_street_lamp", {x + lean - 0.45f, s + 8.6f, z - 0.25f}, {x + lean + 0.45f, s + 8.8f, z + 0.25f}, kSodium, 0.3f, 0.0f, 4.5f);
        }
    }
    // The Operations Hall: louvres across its face, the entrance's canopy and glass, pipes up its side.
    for (int i = 0; i < 9; ++i)
    {
        const float y = s + 7.5f + 1.0f * static_cast<float>(i);
        b.Box("ops_louvre", {-56.05f, y, -28.0f}, {-55.6f, y + 0.45f, 24.0f}, kConcreteOld * 1.1f, 0.85f);
    }
    b.Box("ops_canopy", {-52.0f, s + 6.5f, -10.0f}, {-47.0f, s + 6.9f, 6.0f}, kSteel, 0.6f, 0.5f);
    b.Box("ops_canopy_lamp", {-48.0f, s + 6.45f, -8.0f}, {-47.6f, s + 6.5f, 4.0f}, kSodium, 0.3f, 0.0f, 3.0f);
    b.Box("ops_glass", {-52.06f, s + 0.4f, -7.0f}, {-52.0f, s + 5.0f, 3.0f}, kGlass, 0.1f, 0.6f, 0.0f);
    b.Box("fx_ops_glass_glow", {-52.07f, s + 0.4f, -6.8f}, {-52.06f, s + 5.0f, 2.8f}, kWindow * 0.5f, 0.2f, 0.0f, 0.9f);
    for (const float z : {-24.0f, -16.0f, 14.0f, 20.0f})
    {
        b.Box("ops_pipe", {-56.6f, s, z - 0.25f}, {-56.1f, s + 17.0f, z + 0.25f}, kSteel * 1.1f, 0.4f, 0.8f);
    }
    // Lit windows: Crew Services' rows, the Annex's strip, the Relay's.
    for (int i = 0; i < 6; ++i)
    {
        b.Box("fx_window", {-56.05f, s + 3.0f, 37.0f + 4.0f * static_cast<float>(i)}, {-55.98f, s + 4.4f, 39.6f + 4.0f * static_cast<float>(i)}, kWindow, 0.3f,
              0.0f, 1.4f);
        b.Box("fx_window", {-56.05f, s + 6.6f, 37.0f + 4.0f * static_cast<float>(i)}, {-55.98f, s + 8.0f, 39.6f + 4.0f * static_cast<float>(i)}, kWindow, 0.3f,
              0.0f, 1.2f);
    }
    b.Box("fx_window", {-56.05f, s + 3.0f, -72.0f}, {-55.98f, s + 4.2f, -46.0f}, kWindow * 0.9f, 0.3f, 0.0f, 1.2f);
    b.Box("fx_window", {-53.05f, s + 8.5f, -64.0f}, {-52.98f, s + 9.6f, -58.0f}, kFlood, 0.3f, 0.0f, 1.4f);
    b.Box("fx_window", {-45.0f, s + 3.0f, 91.95f}, {-34.0f, s + 4.4f, 92.0f}, kWindow, 0.3f, 0.0f, 1.3f);
    // The Relay's mast: braces up it, the dish and the red light at its top.
    for (int i = 1; i < 8; ++i)
    {
        const float y = s + 5.0f * static_cast<float>(i);
        b.Box("relay_brace", {-72.0f, y, 95.8f}, {-66.0f, y + 0.25f, 96.2f}, kSteel, 0.5f, 0.7f);
        b.Box("relay_brace", {-72.0f, y, 101.8f}, {-66.0f, y + 0.25f, 102.2f}, kSteel, 0.5f, 0.7f);
        b.Box("relay_brace", {-72.2f, y, 96.0f}, {-71.8f, y + 0.25f, 102.0f}, kSteel, 0.5f, 0.7f);
        b.Box("relay_brace", {-66.2f, y, 96.0f}, {-65.8f, y + 0.25f, 102.0f}, kSteel, 0.5f, 0.7f);
    }
    b.Cylinder("relay_dish", {-69.0f, s + 39.0f, 94.0f}, 9.0f, 0.4f, {60.0f, 0.0f, 0.0f}, kPanel, 0.4f, 0.3f);
    b.Box("fx_relay_light", {-69.3f, s + 42.0f, 98.7f}, {-68.7f, s + 42.6f, 99.3f}, {1.0f, 0.12f, 0.08f}, 0.3f, 0.0f, 6.0f);
    // Rooftop plant: units and stacks on the Hall and the Annex, a stack steaming.
    for (int i = 0; i < 5; ++i)
    {
        b.Box("roof_unit", {-95.0f + 7.0f * static_cast<float>(i), s + 17.7f, 12.0f}, {-91.0f + 7.0f * static_cast<float>(i), s + 19.7f, 17.0f}, kSteel, 0.6f, 0.5f);
    }
    b.Cylinder("stack", {-90.0f, s + 22.0f, -24.0f}, 1.4f, 9.0f, {0.0f, 0.0f, 0.0f}, kRust, 0.7f, 0.4f);
    b.Cylinder("stack", {-86.0f, s + 21.0f, -24.0f}, 1.0f, 7.0f, {0.0f, 0.0f, 0.0f}, kSteelDark, 0.7f, 0.4f);
    // Bollards and barriers along the street's edge by the gate.
    for (float z = -22.0f; z <= 6.0f; z += 3.0f)
    {
        if (z > -17.0f && z < 1.0f)
        {
            continue;
        }
        b.Box("bollard", {-26.5f, s, z - 0.2f}, {-26.1f, s + 1.0f, z + 0.2f}, kHazard, 0.6f);
    }

    // --- Salvage Intake: its doors, the gantry crane over the containers, a light over each dock -------------------------
    for (int i = 0; i < 4; ++i)
    {
        const float x = 36.0f + 13.0f * static_cast<float>(i);
        b.Box("salvage_door", {x, s + 1.3f, -78.05f}, {x + 9.0f, s + 7.5f, -77.95f}, kSteel * 1.1f, 0.6f, 0.5f);
        b.Box("salvage_door_stripe", {x, s + 7.5f, -78.06f}, {x + 9.0f, s + 7.8f, -77.94f}, kHazard, 0.6f);
        b.Box("fx_dock_lamp", {x + 4.0f, s + 8.4f, -77.9f}, {x + 5.0f, s + 8.6f, -77.5f}, kSodium, 0.3f, 0.0f, 3.5f);
    }
    for (const float x : {95.0f, 138.0f})
    {
        b.Box("crane_leg", {x - 0.5f, s, -114.0f}, {x + 0.5f, s + 14.0f, -113.0f}, kOrange * 0.85f, 0.6f, 0.4f);
        b.Box("crane_leg", {x - 0.5f, s, -62.0f}, {x + 0.5f, s + 14.0f, -61.0f}, kOrange * 0.85f, 0.6f, 0.4f);
        b.Box("crane_beam", {x - 0.5f, s + 14.0f, -114.0f}, {x + 0.5f, s + 15.0f, -61.0f}, kOrange * 0.85f, 0.6f, 0.4f);
    }
    b.Box("crane_bridge", {95.0f, s + 14.2f, -90.0f}, {138.0f, s + 15.2f, -88.5f}, kOrange * 0.9f, 0.6f, 0.4f);
    b.Box("crane_hoist", {114.0f, s + 9.0f, -89.6f}, {115.4f, s + 14.2f, -88.9f}, kSteelDark, 0.5f, 0.6f);
    // Apron lamp masts, tall, floodlit.
    for (const glm::vec2 at : {glm::vec2{-10.0f, -60.0f}, glm::vec2{20.0f, -100.0f}, glm::vec2{-15.0f, -130.0f}, glm::vec2{60.0f, -140.0f}})
    {
        b.Box("apron_mast", {at.x - 0.3f, s, at.y - 0.3f}, {at.x + 0.3f, s + 20.0f, at.y + 0.3f}, kSteelDark, 0.5f, 0.7f);
        b.Box("fx_apron_flood", {at.x - 1.4f, s + 20.0f, at.y - 0.6f}, {at.x + 1.4f, s + 20.6f, at.y + 0.6f}, kFlood, 0.3f, 0.0f, 4.0f);
    }
    // Shipworks: its door's stripes and the crane on its roof.
    for (float x = 33.0f; x < 70.0f; x += 4.0f)
    {
        b.Box("shipworks_stripe", {x, s + 0.2f, -40.45f}, {x + 1.6f, s + 1.6f, -40.4f}, kHazard, 0.6f);
    }
    b.Box("shipworks_crane", {30.0f, s + 23.0f, -4.0f}, {74.0f, s + 24.2f, -2.8f}, kOrange * 0.85f, 0.6f, 0.4f);
    b.Box("shipworks_crane_mast", {30.0f, s + 23.0f, -4.0f}, {31.2f, s + 30.0f, -2.8f}, kOrange * 0.85f, 0.6f, 0.4f);
    // Some of the ground scuffed and patched round the slab, so it is not one colour to the hills.
    for (int i = 0; i < 24; ++i)
    {
        const float angle = static_cast<float>(i) * 2.39996f + 0.7f;
        const float reach = 160.0f + 6.0f * static_cast<float>(i);
        const glm::vec3 at{std::cos(angle) * reach, kG, std::sin(angle) * reach};
        const float wide = 16.0f + static_cast<float>((i * 11) % 7) * 6.0f;
        ModelPart& patch = b.Box("patch", at - glm::vec3(wide, 0.02f, wide * 0.6f), at + glm::vec3(wide, 0.03f + 0.025f * static_cast<float>(i % 5), wide * 0.6f),
                                 ground * (0.82f + 0.06f * static_cast<float>((i * 5) % 7)), 0.95f);
        patch.rotation = {0.0f, angle * kDegrees, 0.0f};
    }
    return model;
}

// --- Signs -----------------------------------------------------------------------------------------------------------

std::vector<Sign> Signs(bool named)
{
    const float s = kSlabTop;
    std::vector<Sign> signs;
    // Over the Operations Hall's entrance, facing the street (+x): the mark, and the station's name.
    signs.push_back({"logo_ops", {}, {-51.95f, s + 9.6f, -2.0f}, 90.0f, 12.0f, 3.0f, Sign::Style::Logo});
    if (named)
    {
        signs.push_back({"station_name", {"KESTREL STATION"}, {-55.55f, s + 15.2f, -2.0f}, 90.0f, 22.0f, 2.6f, Sign::Style::Painted});
    }
    signs.push_back({"ops_hall", {"OPERATIONS HALL"}, {-51.95f, s + 5.6f, -2.0f}, 90.0f, 7.0f, 0.9f, Sign::Style::Lit});
    signs.push_back({"crew", {"CREW SERVICES"}, {-55.9f, s + 9.0f, 48.0f}, 90.0f, 9.0f, 1.2f, Sign::Style::Lit});
    signs.push_back({"annex", {"RESEARCH ANNEX"}, {-55.9f, s + 5.6f, -59.0f}, 90.0f, 9.0f, 1.2f, Sign::Style::Lit});
    signs.push_back({"relay", {"NAVIGATION RELAY"}, {-44.0f, s + 6.4f, 91.9f}, 180.0f, 10.0f, 1.2f, Sign::Style::Lit});
    signs.push_back({"salvage", {"SALVAGE INTAKE"}, {60.0f, s + 9.2f, -78.06f}, 180.0f, 14.0f, 1.6f, Sign::Style::Lit});
    signs.push_back({"shipworks", {"SHIPWORKS"}, {51.5f, s + 19.0f, -40.5f}, 180.0f, 18.0f, 2.6f, Sign::Style::Painted});
    signs.push_back({"logo_shipworks", {}, {51.5f, s + 21.0f, -40.5f}, 180.0f, 6.0f, 1.4f, Sign::Style::Logo});
    // Hangar Row: on the bay's walls, inside by the gate and outside on the street, and the bay's number on its front.
    signs.push_back({"row_inside", {"HANGAR ROW", "BAY 02"}, {-23.95f, s + 5.0f, 8.0f}, 90.0f, 6.0f, 2.2f, Sign::Style::Painted});
    signs.push_back({"row_street", {"HANGAR ROW", "BAY 02"}, {-25.05f, s + 6.0f, 8.0f}, -90.0f, 7.0f, 2.6f, Sign::Style::Painted});
    signs.push_back({"bay_front", {"02"}, {-24.5f, s + 11.0f, -40.05f}, 180.0f, 3.0f, 2.4f, Sign::Style::Painted});
    signs.push_back({"bay_logo", {}, {0.0f, s + 11.5f, 34.95f}, 180.0f, 10.0f, 2.5f, Sign::Style::Logo});
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
    canvas.Clear(painted ? Rgb{148, 146, 140} : Rgb{18, 20, 22});
    const Rgb ink = painted ? Rgb{34, 34, 34} : light;
    if (!painted)
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
    // On the bay's floor beside the stair's foot, two rows of four, looking at the ship.
    const float x = -11.0f - static_cast<float>((player / 4) % 2) * 1.2f;
    const float z = -8.6f + static_cast<float>(player % 4) * 1.1f - (player % 4 >= 2 ? 3.2f : 0.0f);
    return ShipMap::ToWorld({x, kSlabTop + 0.1f, z});
}

float SpawnYaw()
{
    // Facing +x, at the ship.
    return 1.5707963f;
}

} // namespace KestrelStation

} // namespace pred
