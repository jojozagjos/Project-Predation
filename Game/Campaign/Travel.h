#pragma once

#include "Game/Campaign/Campaign.h"
#include "Game/Campaign/Universe.h"

#include <glm/vec3.hpp>

#include <vector>

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
//
// It never flies through the star: a straight line that would pass too close to it is steered round, by a point
// beside the star, until the way on is clear.
bool Step(CampaignState& campaign, const StarSystem& system, float dt, int tier);
// How close to the star a course may go, in astronomical units, for a trip between these two places.
float StarClearance(const StarSystem& system, const glm::vec3& from, const glm::vec3& to);
// The way the ship will go from where it is to where it is heading, flown ahead: up to so many points along it, in
// astronomical units from the star. Empty when it is not under way to anywhere.
std::vector<glm::vec3> Preview(const CampaignState& campaign, const StarSystem& system, int tier, int points);

// --- Between the stars ---------------------------------------------------------------------------------------------
// A crossing to another system takes a while however good the drive -- a minute and a bit at the least -- and longer
// the further it is, less for a better drive. Not flown step by step: set out, and arrived when the time is up, at the
// edge of the system on the side it came from. A new system can be chosen part way: from wherever it has got to.
float InterstellarSeconds(float lightYears, int tier);
bool SetSystemCourse(CampaignState& campaign, Universe& universe, uint64_t toSystem, int tier);
// Where the ship is in the galaxy, in light years: its system's place, or part way between two.
glm::vec3 GalaxyPosition(const CampaignState& campaign, Universe& universe);
// How far through a crossing it is, 0 to 1.
float CrossingDone(const CampaignState& campaign);
// True on the step it arrives in the new system.
bool StepInterstellar(CampaignState& campaign, Universe& universe);

} // namespace Travel

} // namespace pred
