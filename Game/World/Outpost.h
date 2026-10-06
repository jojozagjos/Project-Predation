#pragma once

#include "Engine/Assets/ModelAsset.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/OutpostStyle.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pred
{

// An outpost on any world but the home one, planned from its seed so every system's is its own and every machine builds the
// same one: the ship's pad and its stair as at Kestrel (the ship stands the same way everywhere, and so do its crew when they
// step off it), a street from the stair, and round them buildings, yards and masts picked and placed from the seed -- an
// operations block across the street from the stair with the outpost's name on it, sheds, habitats, tank farms, container
// yards, a comms mast, as many as it has -- as its owner has it (OutpostStyle): CIRRA's standard outpost with its mark, a
// neutral one, or one nobody keeps any more, dark. Kestrel Station (KestrelStation) is the one built by hand.
//
// Everything is in the ship's frame, as Kestrel's is, the ground at ShipSpec::kFieldGround. Nothing is put where the
// cinematics of setting down and taking off stand their cameras, or in the way the ship comes in.
namespace Outpost
{

// The most lamps an outpost has; ShipMap keeps this many ready for whichever one the ship stands at.
inline constexpr size_t kMaxLamps = 96;

struct Layout
{
    ModelAsset solid;    // walked on and bumped into
    ModelAsset dressing; // only looked at
    std::vector<KestrelStation::Lamp> lamps;
    // What the signs say. The outpost's own name goes on the one with the id "outpost_name", whose lines are left empty here
    // for whoever draws it: the plan knows only the seed.
    std::vector<KestrelStation::Sign> signs;
};

Layout Generate(uint32_t seed, const glm::vec3& ground, const glm::vec3& rock, const OutpostStyle& style = OutpostStyle{});

} // namespace Outpost

} // namespace pred
