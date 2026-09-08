#pragma once

#include <cstdint>

namespace pred::PhysicsLayers
{

// Narrow-phase object layers, shared between PhysicsWorld and CharacterController.
// Plain integers rather than Jolt types so this header stays free of Jolt includes.
inline constexpr uint16_t kNonMoving = 0;
inline constexpr uint16_t kMoving = 1;
inline constexpr uint16_t kCount = 2;

// Broad-phase layers, one per object layer.
inline constexpr uint8_t kBroadPhaseNonMoving = 0;
inline constexpr uint8_t kBroadPhaseMoving = 1;
inline constexpr uint8_t kBroadPhaseCount = 2;

} // namespace pred::PhysicsLayers
