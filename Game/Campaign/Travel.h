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
// About how long a trip of `distance` astronomical units takes, from orbit to orbit.
float Seconds(float distance, int tier);

// --- In orbit, setting out, arriving -------------------------------------------------------------------------------------
// At a body the ship is in orbit of it: so far out from its middle, once round in kOrbitSeconds, round from where it came into
// orbit (CampaignState::Travel::orbitOut, orbitSince) -- the same on every machine.
inline constexpr double kOrbitSeconds = 960.0;
float OrbitRadius(const Body& body);
// Up off the ground into orbit of the body it stood on, its orbit begun now over the day side.
void Lift(CampaignState& campaign);
// Out from the body's middle to the ship, and the way it is going round, at a moment.
void OrbitFrame(const CampaignState& campaign, const StarSystem& system, int body, double clock, glm::vec3& out, glm::vec3& along);
// Setting out from orbit the ship first turns, swinging round to the side of the world that faces where it is going and a
// little higher, and only then burns: this long (less with a better drive), by how far round it has to go.
float AlignSeconds(const CampaignState& campaign, const StarSystem& system);
// How far through that it is, 0 to 1 (1 once it is burning, or not setting out at all); and where it is in it at a moment, out
// from the middle of the body it is leaving.
float AlignDone(const CampaignState& campaign, const StarSystem& system);
glm::vec3 AlignOffset(const CampaignState& campaign, const StarSystem& system, double clock);
// Near a body it pushes gently -- harder the further out it is, up to all the drive has -- so the world it leaves falls away
// and the one it comes to grows over long enough to see: its acceleration at most this many times its distance from the body
// (per second squared). Settling into orbit, it closes the last of the way at the square root of this times what is left.
float Gentleness(int tier);
// How fast it is going against what it is leaving or coming to (astronomical units a second), and whether it is burning --
// for the dust past the windows and the engines.
float RelativeSpeed(const CampaignState& campaign, const StarSystem& system);
bool Burning(const CampaignState& campaign, const StarSystem& system);

// Where the ship is in its system now, in astronomical units from the star.
glm::vec3 ShipPosition(const CampaignState& campaign, const StarSystem& system);

// Sets a course from wherever the ship is for `body` (-1: stop where it is). False when there is nowhere to go (it is
// already there, or that is not a body). From orbit it sets out (AlignSeconds).
bool SetCourse(CampaignState& campaign, const StarSystem& system, int body);

// One step of the ship under way, the campaign's clock already moved on by `dt`. True on the step it comes into orbit of where
// it is going -- or, with no destination, comes to rest.
//
// It never flies through the star: a straight line that would pass too close to it is steered round, by a point beside the
// star, until the way on is clear. Nor through a world: it leaves one straight out from the side facing where it is going, and
// comes to the next on the near side, into orbit at its height.
bool Step(CampaignState& campaign, const StarSystem& system, float dt, int tier);
// How close to the star a course may go, in astronomical units, for a trip between these two places.
float StarClearance(const StarSystem& system, const glm::vec3& from, const glm::vec3& to);
// The way the ship will go from where it is to where it is heading, flown ahead: up to so many points along it, in
// astronomical units from the star. Empty when it is not under way to anywhere.
std::vector<glm::vec3> Preview(const CampaignState& campaign, const StarSystem& system, int tier, int points);
// How long until it is in orbit where it is going, flown ahead the same way: as it is moving now, not as if it set out from
// rest. Nought when it is not under way to anywhere.
float TimeLeft(const CampaignState& campaign, const StarSystem& system, int tier);

// --- Between the stars ---------------------------------------------------------------------------------------------
// A crossing to another system takes a while however good the drive -- a minute and a bit at the least -- and longer
// the further it is, less for a better drive. Not flown step by step: set out, and arrived when the time is up, at the
// edge of the system on the side it came from. A new system can be chosen part way: from wherever it has got to.
float InterstellarSeconds(float lightYears, int tier);
// The drive it takes to cross between the stars at all: the ship's first drive is for its own system.
inline constexpr int kCrossingTier = 1;
// How far one crossing can go at a drive tier, in light years (0: none), and how far round the systems been to the charts
// reach at a sensor tier -- the galaxy is not all there to see or reach at once; upgrades open it up.
float CrossingRange(int tier);
float ChartRange(int sensorTier);
// The drive that makes a trip short enough that leaving is shown as a cinematic and the trip is all but skipped.
inline constexpr int kInstantTier = 4;
bool SetSystemCourse(CampaignState& campaign, Universe& universe, uint64_t toSystem, int tier);
// Where the ship is in the galaxy, in light years: its system's place, or part way between two.
glm::vec3 GalaxyPosition(const CampaignState& campaign, Universe& universe);
// How far through a crossing it is, 0 to 1.
float CrossingDone(const CampaignState& campaign);
// True on the step it arrives in the new system.
bool StepInterstellar(CampaignState& campaign, Universe& universe);

} // namespace Travel

} // namespace pred
