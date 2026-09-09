#pragma once

#include "Game/Weapons/WeaponTypes.h"

#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

// The firing simulation: a pure function of state, input and time.
//
// It takes no renderer, no physics and no scene, and it never resolves a hit. That separation is
// the whole point. A client runs this immediately when the player pulls the trigger so the weapon
// feels instant, and sends the resulting events up; the host runs exactly the same code and decides
// what they hit. Damage is applied where authority lives, never where the trigger was pulled.
namespace WeaponSim
{

// Advances one tick. Any rounds that leave the barrel are appended to `events`.
void Step(const WeaponDefinition& definition, const WeaponInput& input, WeaponState& state,
          const glm::vec3& muzzle, const glm::vec3& aimDirection, float dt,
          std::vector<FireEvent>& events);

// Gives the player a weapon, filling the magazine from the reserve it comes with.
void Equip(const WeaponDefinition& definition, WeaponState& state);

// Where a round actually goes. Deterministic in the shot sequence, so the host and the client that
// predicted the shot agree on the direction without either sending it.
glm::vec3 DeviateShot(const glm::vec3& direction, float coneDegrees, uint32_t sequence);

// Total cone half-angle right now, in degrees: the stance's own spread plus whatever sustained fire
// has added.
float CurrentSpread(const WeaponDefinition& definition, const WeaponState& state);

} // namespace WeaponSim

} // namespace pred
