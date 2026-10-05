#pragma once

#include "Engine/Assets/ModelAsset.h"
#include "Game/World/ScreenCanvas.h"

#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace pred
{

// Kestrel Station: a CIRRA frontier logistics and contract station on the settled world a campaign starts on, where the
// crew's ship stands in its bay on Hangar Row (and where the tutorial will end). Not a city: one working station that
// has grown by additions -- a street of buildings (Operations Hall, Crew Services, the Research Annex, the Navigation
// Relay), Shipworks beside the ship's bay, Salvage Intake out on the apron, a fence round it all.
//
// Everything is in the ship's frame (ShipSpec: the ship's middle at the origin, its bow towards -z, its deck at y = 0),
// the ground at ShipSpec::kFieldGround. Other worlds' hubs are built the same way without Kestrel's own name on them.
namespace KestrelStation
{

// What is walked on and bumped into: the ground, the hub's slab, walls, buildings, containers, the fence, and the ship's
// boarding stair. Solid, every part of it.
ModelAsset Solid(const glm::vec3& ground, const glm::vec3& rock);
// What is only looked at: lamps, conduits, gantries, markings, grime, the stair's treads. Not solid.
ModelAsset Dressing(const glm::vec3& ground);

// A sign: what it says, where it hangs (its middle, in the ship's frame), which way it faces (degrees about up; 0 faces
// +z), how big, and whether it is lit from behind or painted on.
struct Sign
{
    enum class Style
    {
        Lit,     // a lit panel: light letters on dark
        Painted, // painted on a wall: dark on the wall's colour
        Logo     // the CIRRA mark, lit
    };
    std::string id;
    std::vector<std::string> lines;
    glm::vec3 at{0.0f};
    float yaw = 0.0f;
    float width = 4.0f;
    float height = 1.0f;
    Style style = Style::Lit;
};
// Every sign; `named` puts Kestrel's own name up (only at Kestrel).
std::vector<Sign> Signs(bool named);
// A sign's picture, at the size its texture is made (wide by high, in pixels, from its proportions).
void Draw(ScreenCanvas& canvas, const Sign& sign);
// The CIRRA mark: the letters, and the orange bar in the A, filling the box given.
void DrawLogo(ScreenCanvas& canvas, float x, float y, float width, float height, Rgb letters, Rgb bar);

// Where people stand when the campaign begins: on the bay's floor by the ship's stair, looking at the ship.
glm::vec3 Spawn(int player);
float SpawnYaw();

// The ship's boarding door: to port, in the ops room, between these, up to the door's top.
inline constexpr float kAirlockFrom = -7.6f;
inline constexpr float kAirlockTo = -6.2f;

} // namespace KestrelStation

} // namespace pred
