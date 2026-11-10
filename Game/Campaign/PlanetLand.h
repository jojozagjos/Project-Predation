#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace pred::PlanetLand
{

// Where a planet's land and seas are, worked out on the processor exactly as its surface is drawn from space
// (Engine/Shaders/planet/planet_surface.sh: PlanetContinents and its shore line) -- so a place to land can be put where the
// globe shows ground under it, and the shuttle going down to it sees that ground coming up, not open sea.
//
// `n` is the point on the unit sphere in the planet's own frame (as the map's Globe makes it from latitude and longitude);
// `seed` is the look's (PredationGame::LookOf: the body's seed, modulo 997, times 0.37); `oceanAmount` the sea's share.

// How high the continents are there: 0 to 1, the shore at Shore(oceanAmount).
float Continents(const glm::vec3& n, float seed);
float Shore(float oceanAmount);
// Ground there, by `margin` clear of the shore (a little, so a coast drawn a hair differently still has the place inland).
bool Land(const glm::vec3& n, float seed, float oceanAmount, float margin = 0.02f);

// The look's seed for a body's.
float LookSeed(unsigned long long bodySeed);

// From latitude and longitude in degrees to the unit sphere, the pole up (the map's Globe).
glm::vec3 Globe(const glm::vec2& latLon);

// The nearest place to `latLon` (spiralling out from it, no further than `reach` degrees) that is ground -- or sea, when
// `wantSea` -- and between the latitudes given; `latLon` itself when nowhere near is.
glm::vec2 Nearest(const glm::vec2& latLon, float seed, float oceanAmount, bool wantSea, float minLat = -80.0f, float maxLat = 80.0f,
                  float reach = 60.0f);

} // namespace pred::PlanetLand
