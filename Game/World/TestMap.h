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

// Where each zone sits, so everything that belongs together is together and nothing has to be
// hunted for. WorldObjects places the doors, lockers and loose items, so both files read these
// rather than each keeping a private copy of the layout that drifts from the other.
//
// The spawn looks south down the middle of the map with every zone in view.
inline constexpr float kSpawnZ = 16.0f;
inline constexpr float kBayZ = 8.6f;          // both bays share a row, either side of the middle
inline constexpr float kEquipmentBayX = -13.5f; // everything you can pick up
inline constexpr float kInteractionBayX = 13.5f; // everything you can open or get inside
inline constexpr float kBayHalfWidth = 2.6f;
inline constexpr float kBenchTop = 0.92f; // items sit on this, so they are at hand height

// A row of low slabs, one per footstep surface, to walk along and hear them against each other.
//
// A footstep cannot be judged on its own and cannot be judged against silence: the question is
// always whether this one sounds different from that one and whether either sounds like the floor
// it is on. Five pads in a line answers that in about ten seconds of walking.
//
// Which surface is underfoot is worked out from these rectangles rather than from the collider you
// are standing on. That is the right amount of machinery for a test lineup: when the game has real
// levels, the surface will come off the material on the geometry, and this row will not be needed.
struct SurfacePad
{
    const char* surface;
    float centreX;
    float centreZ;
    float sizeX;
    float sizeZ;
};
inline constexpr float kSurfaceRowZ = 5.0f;
inline constexpr float kSurfacePadDepth = 3.0f;
inline constexpr float kSurfacePadWidth = 1.8f;
inline constexpr float kSurfacePadHeight = 0.06f;

// The dark room: an enclosed box with one doorway, west of the spawn. Somewhere to stand with a
// torch on and find out whether the lighting works. It is dark because it has a roof, not because
// anything here says so -- see the comment where it is built.
inline constexpr float kDarkRoomX = -13.0f;
inline constexpr float kDarkRoomZ = 17.5f;
inline constexpr float kDarkRoomWidth = 9.0f; // along X
inline constexpr float kDarkRoomDepth = 8.0f; // along Z
inline constexpr float kDarkRoomHeight = 3.4f;
inline constexpr float kDarkRoomDoorWidth = 1.8f;
inline constexpr float kDarkRoomWallThickness = 0.3f;

inline constexpr SurfacePad kSurfacePads[] = {
    {"concrete", -3.8f, kSurfaceRowZ, kSurfacePadWidth, kSurfacePadDepth},
    {"stone", -1.9f, kSurfaceRowZ, kSurfacePadWidth, kSurfacePadDepth},
    {"metal", 0.0f, kSurfaceRowZ, kSurfacePadWidth, kSurfacePadDepth},
    {"gravel", 1.9f, kSurfaceRowZ, kSurfacePadWidth, kSurfacePadDepth},
    {"wood", 3.8f, kSurfaceRowZ, kSurfacePadWidth, kSurfacePadDepth},
};
} // namespace TestMapSpec

// Which surface pad, if any, a world position is over. Null off the row, which the caller reads as
// "the default surface": the rest of the map is one floor and has no opinion yet.
const char* SurfaceUnderfoot(float worldX, float worldZ);

} // namespace pred
