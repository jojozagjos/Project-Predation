#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Game/Weapons/WeaponTypes.h"

#include <glm/vec3.hpp>

namespace pred
{

// What a round found. Damage is a result, not an input: whoever holds authority decides it.
struct ShotResult
{
    bool hit = false;
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    BodyHandle body;
    float damage = 0.0f;
    // Whether what was struck is a fixed surface, so a mark left on it stays true.
    //
    // A round that stops in a person hits something that then walks away, and the hole it left
    // hangs in the air where they were standing. That is what "bullet holes stay in mid air" was:
    // not a placement bug, but a mark put on a thing that moves. The world trace sets this; anything
    // that intercepts the round on its way -- a player now, a creature later -- clears it.
    bool surface = false;

    explicit operator bool() const { return hit; }
};

// Traces one round against the world.
//
// Deliberately separate from the firing simulation. A client raises a FireEvent the instant the
// trigger goes down so the weapon feels immediate, and sends it up; the host runs this, decides
// what was struck, and applies the damage. Nothing on the client's side of that line ever changes
// another player's or a creature's health.
//
// `ignore` is the shooter's own body, so nobody shoots themselves at point blank range.
ShotResult ResolveShot(const PhysicsWorld& physics, const FireEvent& event, BodyHandle ignore = {});

} // namespace pred
