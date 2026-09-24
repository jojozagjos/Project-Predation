#pragma once

#include <glm/vec3.hpp>

namespace pred
{

class Scene;
class MeshLibrary;
class PhysicsWorld;
class LevelLights;

// The creature lab: a map for watching creatures do everything they can do, and for trying to survive
// them while they do it.
//
// Built into the same world as the test map, well to the east of it, rather than as a map of its own:
// there is only one world, built behind the menu, and going to the lab is being moved there. Everybody
// in a game goes together, and dies back to the lab until they leave it.
//
// From the spawn at the south edge, looking north:
//   south       the spawn, and a bench with a rifle, a pistol and ammunition
//   south-west  the crate yard: blocks from half a metre to three and a half high, for what can jump
//               onto what, and a watchtower up a stair too narrow for anything but a person
//   centre      a crawlspace a person can get into and nothing bigger can follow them down
//   east        a corridor of doors: an ordinary one, a room of lockers, and a locked door onto a dark
//               store room, for a creature to open, search and break down
//   north-west  a balcony up a ramp, with a drop off its edge, for jumping down and climbing up
//   north-east  a roofed forest of pillars, dark, for stalking
//   north       the nest: a dark chamber with the hive in it, where creatures take what they catch
namespace LabSpec
{
inline constexpr float kX = 170.0f;
inline constexpr float kZ = 0.0f;
inline constexpr float kHalf = 32.0f;
inline constexpr float kWallHeight = 5.0f;

// Where to stand when you arrive, looking north into it.
inline constexpr glm::vec3 kSpawn{kX, 0.5f, kZ + 27.0f};

// The nest, and how far its mound reaches from its middle.
inline constexpr glm::vec3 kHive{kX, 0.0f, kZ - 22.0f};
inline constexpr float kHiveRadius = 1.8f;

// The bench by the spawn, with the weapons on it, and the ammunition beside it.
inline constexpr glm::vec3 kBench{kX - 5.0f, 0.0f, kZ + 30.2f};
inline constexpr float kBenchTop = 0.92f;
inline constexpr glm::vec3 kAmmo{kX - 8.0f, 0.0f, kZ + 30.2f};

// The crate yard: how high each block is, in a row running east from its west end.
inline constexpr float kBlockHeights[] = {0.5f, 1.0f, 1.5f, 2.0f, 2.6f, 3.4f};
inline constexpr float kBlockRowZ = kZ + 13.0f;
inline constexpr float kBlockRowX = kX - 28.0f;
inline constexpr float kBlockSpacing = 3.6f;
inline constexpr float kBlockSize = 2.2f;

// The corridor of doors, and the rooms off it.
inline constexpr float kCorridorWest = kX + 10.0f;
inline constexpr float kCorridorEast = kX + 13.0f;
inline constexpr float kCorridorSouth = kZ + 18.0f;
inline constexpr float kCorridorNorth = kZ + 0.0f;
inline constexpr float kRoomEast = kX + 30.0f;
inline constexpr float kRoomSplit = kZ + 10.0f; // between the locker room (south) and the store (north)
inline constexpr float kDoorWidth = 1.2f;
inline constexpr float kDoorHeight = 2.2f;
// Where each doorway is: the corridor's own at its south end, the locker room's and the store's in its
// east wall.
inline constexpr float kCorridorDoorX = kX + 11.5f;
inline constexpr float kLockerDoorZ = kZ + 14.0f;
inline constexpr float kStoreDoorZ = kZ + 5.0f;

// Lockers along the south wall of the locker room, opening north into it.
inline constexpr glm::vec3 kLockers[] = {{kX + 17.0f, 0.0f, kZ + 17.2f},
                                         {kX + 19.2f, 0.0f, kZ + 17.2f},
                                         {kX + 21.4f, 0.0f, kZ + 17.2f},
                                         {kX + 23.6f, 0.0f, kZ + 17.2f}};

// The balcony and the drop off it.
inline constexpr float kBalconyHeight = 2.4f;
inline constexpr float kBalconyWest = kX - 31.75f;
inline constexpr float kBalconyEast = kX - 18.0f;
inline constexpr float kBalconyNorth = kZ - 10.0f;
inline constexpr float kBalconySouth = kZ + 2.0f;
} // namespace LabSpec

// Builds the lab's level: floors, walls, rooms, the crate yard and the nest. The doors, lockers and
// the things on the bench are WorldObjects', which reads the same numbers.
void BuildLabMap(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics, LevelLights* lights = nullptr);

// Whether a point is in the lab rather than the test map.
bool InLab(const glm::vec3& point);

} // namespace pred
