#pragma once

namespace pred
{

class Scene;
class MeshLibrary;
class PhysicsWorld;

// Builds the developer test map: a laboratory for gameplay systems rather than a real level.
//
// Zones are laid out around the origin so each movement case can be reached quickly:
//   origin        reference cube and calibration markers
//   +X            staircases with three different step rises
//   -X            ramps at 15, 25, 35 and 45 degrees
//   +Z            ledges at mantle-relevant heights, 0.3 m to 1.8 m
//   -Z            a corridor with progressively narrower gaps and a doorway
//   corners       pillars for occlusion, cover and creature line-of-sight tests
//
// Heights and widths here are the numbers the player controller will be tuned against, so they are
// named constants rather than literals scattered through the builder.
// Passing `physics` also creates static collision geometry that matches the visuals exactly.
// Pass nullptr for a render-only build.
void BuildTestMap(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics);

namespace TestMapSpec
{
// Movement calibration values, all in metres.
inline constexpr float kGroundSize = 90.0f;
inline constexpr float kPlayerHeight = 1.8f;
inline constexpr float kPlayerRadius = 0.35f;
inline constexpr float kDoorHeight = 2.1f;

inline constexpr float kStepRises[] = {0.12f, 0.18f, 0.25f};   // shallow, normal, steep stairs
inline constexpr float kRampAngles[] = {15.0f, 25.0f, 35.0f, 45.0f};
inline constexpr float kLedgeHeights[] = {0.3f, 0.6f, 0.9f, 1.2f, 1.5f, 1.8f};
inline constexpr float kGapWidths[] = {1.2f, 0.9f, 0.7f, 0.55f, 0.45f};
} // namespace TestMapSpec

} // namespace pred
