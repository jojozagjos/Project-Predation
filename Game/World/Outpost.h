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
// same one: a landing field -- a pad far bigger than the ship, for the bigger ships to come, its apron round it, floodlight
// masts, service vehicles, hangars across it -- and beside it a town of one to three streets, an operations block across the
// first street from the stair with the outpost's name on it, and along the streets buildings, yards and masts picked and
// placed from the seed: blocks, sheds, habitats, tank farms, container yards, a comms mast, as many as it has -- as its owner
// has it (OutpostStyle): CIRRA's standard outpost with its mark, a neutral one, or one nobody keeps any more, dark. Concrete
// and asphalt where they are poured -- the field, the streets, the yards under the buildings -- and the world's own ground
// between, levelled. Kestrel Station (KestrelStation) is the one built by hand.
//
// Everything is in the ship's frame, as Kestrel's is, the ground at ShipSpec::kFieldGround. Nothing is put where the
// cinematics of setting down and taking off stand their cameras, or in the way the ship comes in.
namespace Outpost
{

// The most lamps an outpost has; ShipMap keeps this many ready for whichever one the ship stands at.
inline constexpr size_t kMaxLamps = 160;

struct Layout
{
    ModelAsset solid;    // walked on and bumped into
    ModelAsset dressing; // only looked at
    std::vector<KestrelStation::Lamp> lamps;
    // What the signs say. The outpost's own name goes on the one with the id "outpost_name", whose lines are left empty here
    // for whoever draws it: the plan knows only the seed.
    std::vector<KestrelStation::Sign> signs;
    // Red lights high on what is tall, which flash (ShipMap): where each light is. A beacon nobody keeps lit is not here, only
    // its housing.
    std::vector<glm::vec3> beacons;
    // How far the outpost reaches (x0, z0 to x1, z1, in the ship's frame): the world's own ground is made round it (SiteMap);
    // and what is poured into it, each (x0, z0 to x1, z1): the ground levelled under each, and blending into the land round.
    glm::vec4 slab{0.0f};
    std::vector<glm::vec4> levelled;
};

Layout Generate(uint32_t seed, const glm::vec3& ground, const glm::vec3& rock, const OutpostStyle& style = OutpostStyle{});

} // namespace Outpost

} // namespace pred
