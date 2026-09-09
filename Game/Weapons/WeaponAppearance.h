#pragma once

#include "Engine/Render/Mesh.h"
#include "Game/Weapons/WeaponTypes.h"

#include <glm/vec3.hpp>

namespace pred
{

// The shape of a weapon, and the handful of points on it that other systems need.
//
// Built from the weapon's data entry rather than authored, in the same spirit as the item shapes:
// a new line in weapons.json produces something that reads as a firearm without waiting for art.
// The magazine is a separate mesh because a reload has to be able to take it out.
//
// The weapon's own frame has its origin at the pistol grip, +Z along the barrel, +Y up. Everything
// that positions a weapon works in that frame, so nothing has to know how the parts are laid out.
struct WeaponVisual
{
    MeshData body;     // receiver, barrel, handguard, grip, stock and sight
    MeshData magazine; // detachable, and animated on its own during a reload

    glm::vec3 magazineSeated{0.0f}; // where the magazine sits when it is in
    glm::vec3 supportGrip{0.0f};    // where the support hand goes
    glm::vec3 muzzle{0.0f};         // where a round appears to leave
    float sightHeight = 0.0f;       // sight line above the origin; aiming puts this on the view axis
};

WeaponVisual BuildWeaponVisual(const WeaponDefinition& definition);

} // namespace pred
