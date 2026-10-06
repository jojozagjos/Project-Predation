#pragma once

#include "Engine/Assets/ModelAsset.h"
#include "Game/World/ScreenCanvas.h"
#include "Game/World/ShipMap.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <string>
#include <vector>

namespace pred
{

struct PartBuilder;

// Kestrel Station: a CIRRA frontier logistics and contract station on the settled world a campaign starts on, where the
// crew's ship stands in its bay on Hangar Row (and where the tutorial will end). Not a city: one working station that
// has grown by additions --
//   Hangar Row   the ship's bay, open to the sky, its blast walls round three sides, floodlight masts at its corners, a
//                control booth over its back corner, and a gate in its port wall onto the street
//   Shipworks    the great shed to starboard, its side the bay's starboard wall, its hangar door onto the pad
//   the street   through the gate: a road between pavements, sodium lamps, pipes carried over it on trestles, and the
//                buildings along its far side -- Operations Hall (the CIRRA mark and the station's name over its entrance),
//                the Research Annex, Crew Services, and the Navigation Relay closing its far end with its mast
//   the apron    ahead of the bay: Salvage Intake, its docks, containers and gantry crane, lamp masts; a fence round it all
//
// Everything is in the ship's frame (ShipSpec: the ship's middle at the origin, its bow towards -z, its deck at y = 0),
// the ground at ShipSpec::kFieldGround. Other worlds' outposts are planned from their seeds (Outpost), on the same pad.
namespace KestrelStation
{

// What is walked on and bumped into: the ground, the slab, walls, buildings, masts and posts, containers, the fence, and
// the ship's boarding stair. Solid, every part of it.
ModelAsset Solid(const glm::vec3& ground, const glm::vec3& rock);
// What is only looked at: paint, lamp heads, conduits, gantries out of reach, windows, grime. Not solid.
ModelAsset Dressing(const glm::vec3& ground);

// A lamp that lights the station: where it is (the ship's frame), which way it shines, its colour, how bright and how far,
// and its cone (degrees across; 180 for all round). Its fitting is part of the dressing.
struct Lamp
{
    glm::vec3 at{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 colour{1.0f};
    float intensity = 20.0f;
    float range = 15.0f;
    float inner = 60.0f;
    float outer = 90.0f;
};
std::vector<Lamp> Lamps();

// A sign: what it says, where it hangs (its middle, in the ship's frame), which way it faces (degrees about up; 0 faces
// +z), how big, and whether it is lit from behind or painted on. A sign on the floor lies flat, its top towards where
// `yaw` faces.
struct Sign
{
    enum class Style
    {
        Lit,     // a lit panel: light letters on dark
        Painted, // painted on a wall: dark on the wall's colour
        Logo,    // the CIRRA mark, lit
        Floor    // painted on the ground: light on the dark of the pad
    };
    std::string id;
    std::vector<std::string> lines;
    glm::vec3 at{0.0f};
    float yaw = 0.0f;
    float width = 4.0f;
    float height = 1.0f;
    Style style = Style::Lit;
    // Lit no longer: a place nobody keeps the power on in.
    bool dark = false;
};
// Every sign; `named` puts Kestrel's own name up (only at Kestrel).
std::vector<Sign> Signs(bool named);
// A sign's picture, at the size its texture is made (wide by high, in pixels, from its proportions).
void Draw(ScreenCanvas& canvas, const Sign& sign);
// The CIRRA mark: the letters, and the orange bar in the A, filling the box given.
void DrawLogo(ScreenCanvas& canvas, float x, float y, float width, float height, Rgb letters, Rgb bar);

// --- What every outpost's pad has, Kestrel's and the generated ones' (Outpost) alike -----------------------------------
// The pad the ship stands on (x0, z0, x1, z1), poured into the slab round it rather than laid on it.
inline constexpr glm::vec4 kPad{-17.0f, -30.0f, 17.0f, 34.0f};
// The concrete's top, walked on.
inline constexpr float kSlabTop = ShipSpec::kFieldGround + 0.12f;
// The walkway from the stair to the street, across the pad's port side, between these (z).
inline constexpr float kCrossFrom = -8.6f;
inline constexpr float kCrossTo = -5.2f;
void Pad(PartBuilder& solid);
// The ground supply the ship is plugged into, beside where its stair comes down (the stair is the ship's: ShipMap).
void Supply(PartBuilder& solid);
// The pad's paint, the lamps sunk round it and the supply's cable; the walkway from the stair out as far as `walkwayTo` (x).
void PadDressing(PartBuilder& dressing, float walkwayTo);
// The floodlight masts at the pad's corners, their crossbars (dressing) and their lamps.
void PadMasts(PartBuilder& solid);
void PadMastBars(PartBuilder& dressing);
std::vector<Lamp> PadMastLamps();
// A head and a hood on each lamp; dark heads, for lamps that no longer work.
void LampHeads(PartBuilder& dressing, const std::vector<Lamp>& lamps, bool lit = true);

// Where people stand when the campaign begins: on the bay's floor by the ship's stair, looking at the ship.
glm::vec3 Spawn(int player);
float SpawnYaw();

// The ship's boarding door: to port, in the ops room, between these, up to the door's top.
inline constexpr float kAirlockFrom = -7.6f;
inline constexpr float kAirlockTo = -6.2f;

} // namespace KestrelStation

} // namespace pred
