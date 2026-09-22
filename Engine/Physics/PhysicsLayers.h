#pragma once

#include <cstdint>

namespace pred::PhysicsLayers
{

// Narrow-phase object layers, shared between PhysicsWorld and CharacterController.
// Plain integers rather than Jolt types so this header stays free of Jolt includes.
inline constexpr uint16_t kNonMoving = 0;
inline constexpr uint16_t kMoving = 1;
// Things lying about: dropped weapons, spent kit, anything a player is meant to walk up to and take
// rather than trip over. They fall and settle on the world and on each other, and characters pass
// straight through them. Kicking a dropped rifle across the room is not a mechanic, and an item
// wedged against a capsule is one that cannot be picked up.
inline constexpr uint16_t kDebris = 2;
// The pieces of a body gone limp. They fall onto the level and loose things and pass through people.
inline constexpr uint16_t kRagdoll = 3;
// Where a creature can be hit, moved with its limbs. Collides with nothing; rays find it.
inline constexpr uint16_t kHitbox = 4;
inline constexpr uint16_t kCount = 5;

// Broad-phase layers, one per object layer.
inline constexpr uint8_t kBroadPhaseNonMoving = 0;
inline constexpr uint8_t kBroadPhaseMoving = 1;
inline constexpr uint8_t kBroadPhaseCount = 2;

} // namespace pred::PhysicsLayers
