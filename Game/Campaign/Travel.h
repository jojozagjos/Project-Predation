#pragma once

#include "Game/Campaign/Campaign.h"
#include "Game/Campaign/Universe.h"

#include <glm/vec3.hpp>

namespace pred
{

// The ship going from one place in a system to another: real distance, real time, flown.
//
// No jumps and no fuel. The ship accelerates for half the way and slows for the rest, so a trip twice as far takes
// about half as long again, not twice as long -- far places are further, not unreasonable. A better drive pushes
// harder, which is what makes travel quicker as the ship is upgraded, until a trip is little more than leaving and
// arriving. The ship steers for wherever the planet is now, and planets move, so it simply keeps steering; and
// because it only ever steers, a new destination part way is the same thing as the first one.
namespace Travel
{

// How hard the ship can push at a drive tier, in astronomical units a second, a second.
float Acceleration(int tier);
// About how long a trip of `distance` astronomical units takes, from rest to rest.
float Seconds(float distance, int tier);
// Within this of a body's middle is arriving at it.
inline constexpr float kArrival = 0.0015f;

// Where the ship is in its system now, in astronomical units from the star.
glm::vec3 ShipPosition(const CampaignState& campaign, const StarSystem& system);

// Sets a course from wherever the ship is for `body` (-1: stop where it is). False when there is nowhere to go (it is
// already there, or that is not a body).
bool SetCourse(CampaignState& campaign, const StarSystem& system, int body);

// One step of the ship under way. True on the step it arrives -- or, with no destination, comes to rest.
bool Step(CampaignState& campaign, const StarSystem& system, float dt, int tier);

} // namespace Travel

} // namespace pred
