#include "Game/World/LabMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"
#include "Game/World/LevelLights.h"
#include "Game/World/MapBuilder.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace pred
{
namespace
{

using namespace LabSpec;

const Material kLabFloor = Material::Diffuse({0.27f, 0.28f, 0.30f}, 0.93f);
const Material kLabWall = Material::Diffuse({0.23f, 0.24f, 0.26f}, 0.95f);
const Material kLabRoof = Material::Diffuse({0.17f, 0.17f, 0.18f}, 0.95f);
const Material kLabBlock = Material::Diffuse({0.40f, 0.31f, 0.27f}, 0.82f);
const Material kLabTower = Material::Diffuse({0.33f, 0.35f, 0.30f}, 0.85f);
const Material kLabStair = Material::Diffuse({0.42f, 0.38f, 0.30f}, 0.85f);
const Material kLabConcrete = Material::Diffuse({0.35f, 0.34f, 0.32f}, 0.9f);
const Material kLabPillar = Material::Metal({0.45f, 0.45f, 0.48f}, 0.5f);
const Material kLabNestWall = Material::Diffuse({0.16f, 0.12f, 0.11f}, 0.8f);

// How thick a roof is: see the crawlspace.
constexpr float kRoof = 0.6f;

Transform At(float lx, float y, float lz)
{
    Transform transform;
    transform.position = {kX + lx, y, kZ + lz};
    return transform;
}

Transform AtYaw(float lx, float y, float lz, float yawDegrees)
{
    Transform transform = At(lx, y, lz);
    transform.rotation = glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
    return transform;
}

// A box between two corners, in the lab's own coordinates. Walls are easier to get right as the space
// they fill than as a centre and a size.
void Block(MapBuilder& builder, const char* name, float x0, float y0, float z0, float x1, float y1, float z1,
           const Material& material)
{
    const glm::vec3 lo{std::min(x0, x1), std::min(y0, y1), std::min(z0, z1)};
    const glm::vec3 hi{std::max(x0, x1), std::max(y0, y1), std::max(z0, z1)};
    const glm::vec3 centre = (lo + hi) * 0.5f;
    builder.AddBox(name, At(centre.x, centre.y, centre.z), hi - lo, material);
}

} // namespace

void BuildLabMap(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics, LevelLights* lights)
{
    MapBuilder builder(scene, meshes, physics, "lab_");
    const float h = kWallHeight;

    // The floor, drawn as a grid of eight-metre tiles and collided as one slab with its top at nought.
    // In tiles so each is lit by the lamps above it: see MapBuilder::kTile.
    const int tiles = static_cast<int>(std::ceil(kHalf * 2.0f / MapBuilder::kTile));
    const float tileSize = kHalf * 2.0f / static_cast<float>(tiles);
    const MeshHandle floor = meshes.Upload(Primitives::Plane({tileSize, tileSize}, 4), "lab_floor_tile");
    for (int i = 0; i < tiles; ++i)
    {
        for (int k = 0; k < tiles; ++k)
        {
            scene.CreateMeshEntity("lab_floor",
                                   At(-kHalf + (static_cast<float>(i) + 0.5f) * tileSize, 0.0f,
                                      -kHalf + (static_cast<float>(k) + 0.5f) * tileSize),
                                   floor, kLabFloor);
        }
    }
    if (physics != nullptr)
    {
        physics->CreateBox({kHalf, 0.5f, kHalf}, At(0.0f, -0.5f, 0.0f), BodyMotion::Static);
    }

    // The outer walls, meeting edge to edge at the corners.
    Block(builder, "lab_wall_n", -kHalf - 0.5f, 0.0f, -kHalf - 0.5f, kHalf + 0.5f, h, -kHalf, kLabWall);
    Block(builder, "lab_wall_s", -kHalf - 0.5f, 0.0f, kHalf, kHalf + 0.5f, h, kHalf + 0.5f, kLabWall);
    Block(builder, "lab_wall_w", -kHalf - 0.5f, 0.0f, -kHalf, -kHalf, h, kHalf, kLabWall);
    Block(builder, "lab_wall_e", kHalf, 0.0f, -kHalf, kHalf + 0.5f, h, kHalf, kLabWall);

    // --- The spawn's bench, for the weapons WorldObjects lays on it.
    Block(builder, "lab_bench", kBench.x - kX - 2.4f, 0.0f, kBench.z - kZ - 0.32f, kBench.x - kX + 2.4f, kBenchTop,
          kBench.z - kZ + 0.32f, kLabConcrete);

    // --- The crate yard: a row of blocks, each higher than the last, and a watchtower up a stair too
    // narrow for anything but a person.
    for (size_t i = 0; i < std::size(kBlockHeights); ++i)
    {
        const float x = kBlockRowX - kX + kBlockSpacing * static_cast<float>(i);
        const float z = kBlockRowZ - kZ;
        const float half = kBlockSize * 0.5f;
        Block(builder, "lab_block", x - half, 0.0f, z - half, x + half, kBlockHeights[i], z + half, kLabBlock);
    }
    {
        constexpr float kTowerHeight = 3.4f;
        Block(builder, "lab_tower", -17.5f, 0.0f, 3.5f, -14.5f, kTowerHeight, 6.5f, kLabTower);
        // Seventeen steps of twenty centimetres, sixty centimetres wide, climbing west to the top.
        const MeshData stairs = Primitives::Stairs(17, 0.6f, 0.2f, 0.25f);
        builder.AddMesh("lab_tower_stairs", AtYaw(-14.5f + 17.0f * 0.25f, 0.0f, 5.0f, -90.0f), stairs, kLabStair);
    }

    // --- The crawlspace: a tunnel a person can crawl into and nothing bigger can follow them down.
    Block(builder, "lab_crawl_w", -1.5f, 0.0f, 6.0f, -1.2f, 1.3f, 16.0f, kLabConcrete);
    Block(builder, "lab_crawl_e", 1.2f, 0.0f, 6.0f, 1.5f, 1.3f, 16.0f, kLabConcrete);
    // Roofs are thick throughout -- sixty centimetres -- and not for the look of it. A shadow map reads a
    // surface as lit if it is within a few centimetres of the nearest thing over it, and a roof thinner
    // than that let the top of every inside wall read as under the open sky: the band of daylight along the
    // top of each room. See kRoof.
    Block(builder, "lab_crawl_roof", -1.5f, 1.3f, 6.0f, 1.5f, 1.3f + kRoof, 16.0f, kLabConcrete);

    // --- The corridor of doors, roofed, with a room of lockers and a locked store off it.
    {
        const float west = kCorridorWest - kX;
        const float east = kCorridorEast - kX;
        const float south = kCorridorSouth - kZ;
        const float north = kCorridorNorth - kZ;
        const float room = kRoomEast - kX;
        const float split = kRoomSplit - kZ;
        const float top = 3.0f;
        const float t = 0.3f;
        const float half = kDoorWidth * 0.5f;
        const float doorX = kCorridorDoorX - kX;
        const float lockerZ = kLockerDoorZ - kZ;
        const float storeZ = kStoreDoorZ - kZ;
        Block(builder, "lab_wing_n", west - t, 0.0f, north - t, room + t, top, north, kLabWall);
        Block(builder, "lab_wing_s_a", west - t, 0.0f, south, doorX - half, top, south + t, kLabWall);
        Block(builder, "lab_wing_s_b", doorX + half, 0.0f, south, room + t, top, south + t, kLabWall);
        Block(builder, "lab_wing_s_lintel", doorX - half, kDoorHeight, south, doorX + half, top, south + t, kLabWall);
        Block(builder, "lab_corridor_w", west - t, 0.0f, north, west, top, south, kLabWall);
        Block(builder, "lab_corridor_e_a", east, 0.0f, north, east + t, top, storeZ - half, kLabWall);
        Block(builder, "lab_corridor_e_b", east, 0.0f, storeZ + half, east + t, top, lockerZ - half, kLabWall);
        Block(builder, "lab_corridor_e_c", east, 0.0f, lockerZ + half, east + t, top, south, kLabWall);
        Block(builder, "lab_corridor_e_store", east, kDoorHeight, storeZ - half, east + t, top, storeZ + half, kLabWall);
        Block(builder, "lab_corridor_e_lockers", east, kDoorHeight, lockerZ - half, east + t, top, lockerZ + half, kLabWall);
        Block(builder, "lab_room_split", east + t, 0.0f, split - 0.15f, room, top, split + 0.15f, kLabWall);
        Block(builder, "lab_room_e", room, 0.0f, north, room + t, top, south, kLabWall);
        Block(builder, "lab_wing_roof", west - t, top, north - t, room + t, top + kRoof, south + t, kLabRoof);
    }

    // --- The balcony, up a ramp from the south, with a drop off its east and north edges.
    {
        const float west = kBalconyWest - kX;
        const float east = kBalconyEast - kX;
        const float north = kBalconyNorth - kZ;
        const float south = kBalconySouth - kZ;
        Block(builder, "lab_balcony", west, kBalconyHeight - 0.3f, north, east, kBalconyHeight, south, kLabConcrete);
        for (const float x : {east - 0.8f, west + 3.8f})
        {
            for (const float z : {north + 0.8f, south - 0.8f})
            {
                Block(builder, "lab_balcony_post", x - 0.25f, 0.0f, z - 0.25f, x + 0.25f, kBalconyHeight - 0.3f, z + 0.25f,
                      kLabPillar);
            }
        }
        // Rising north from the floor to the balcony's south edge.
        const MeshData ramp = Primitives::Ramp(3.5f, 6.0f, kBalconyHeight);
        builder.AddMesh("lab_ramp", AtYaw(west + 2.25f, 0.0f, south + 6.0f, 180.0f), ramp, kLabStair);
    }

    // --- The pillar forest, roofed and dark.
    for (const float x : {18.0f, 21.0f, 24.0f, 27.0f})
    {
        for (const float z : {-9.0f, -6.0f, -3.0f})
        {
            Block(builder, "lab_pillar", x - 0.3f, 0.0f, z - 0.3f, x + 0.3f, 4.5f, z + 0.3f, kLabPillar);
        }
    }
    Block(builder, "lab_pillar_roof", 15.5f, 4.5f, -11.0f, 31.0f, 4.5f + kRoof, -1.0f, kLabRoof);

    // --- The nest: a dark chamber across the north end, two ways in, and the hive in the middle of it.
    {
        const float t = 0.4f;
        const float south = -12.0f;
        const float doorHigh = 3.0f;
        Block(builder, "lab_nest_w", -16.0f - t, 0.0f, -kHalf, -16.0f, h, south, kLabNestWall);
        Block(builder, "lab_nest_e", 16.0f, 0.0f, -kHalf, 16.0f + t, h, south, kLabNestWall);
        Block(builder, "lab_nest_s_a", -16.0f - t, 0.0f, south, -9.5f, h, south + t, kLabNestWall);
        Block(builder, "lab_nest_s_b", -6.5f, 0.0f, south, 6.5f, h, south + t, kLabNestWall);
        Block(builder, "lab_nest_s_c", 9.5f, 0.0f, south, 16.0f + t, h, south + t, kLabNestWall);
        Block(builder, "lab_nest_lintel_w", -9.5f, doorHigh, south, -6.5f, h, south + t, kLabNestWall);
        Block(builder, "lab_nest_lintel_e", 6.5f, doorHigh, south, 9.5f, h, south + t, kLabNestWall);
        Block(builder, "lab_nest_roof", -16.0f - t, h, -kHalf - 0.5f, 16.0f + t, h + kRoof, south + t, kLabRoof);

        // Nothing is built here: a nest exists only where a creature has made one, and this chamber is
        // somewhere dark and enclosed that one is likely to choose.
    }

    // --- The lamps. A building that has been looked after has its lights on; this one has not been,
    // entirely. Circuits: 1 the corridor and its rooms, 2 the pillar forest, 3 the nest chamber.
    if (lights != nullptr)
    {
        const glm::vec3 down{0.0f, -1.0f, 0.0f};
        // Each kept to the room it is in, given in the lab's own coordinates as two corners: lamps cast no
        // shadows, and the store's red lamp used to light the corridor through its wall.
        struct Room
        {
            glm::vec3 a;
            glm::vec3 b;
        };
        const auto keep = [&](int index, const Room& room)
        { lights->Bound(index, glm::vec3(kX, 0.0f, kZ) + room.a, glm::vec3(kX, 0.0f, kZ) + room.b); };
        const auto ceiling = [&](float lx, float roof, float lz, LightMood mood, int circuit, const Room& room)
        {
            const int index = lights->Add(scene, meshes, LightKind::Ceiling, mood, glm::vec3(kX + lx, roof - 0.04f, kZ + lz), down, circuit);
            keep(index, room);
            return index;
        };
        // A lamp's light through a doorway, into the box on the far side.
        const auto spill = [&](int source, float lx, float lz, const glm::vec3& facing, const Room& beyond)
        {
            lights->AddSpill(source, glm::vec3(kX + lx, 1.95f, kZ + lz), facing, glm::vec3(kX, 0.0f, kZ) + beyond.a,
                             glm::vec3(kX, 0.0f, kZ) + beyond.b);
        };
        const auto wall = [&](LightKind kind, float lx, float y, float lz, const glm::vec3& facing, LightMood mood, int circuit,
                              const Room* room)
        {
            const int index = lights->Add(scene, meshes, kind, mood, glm::vec3(kX + lx, y, kZ + lz), facing, circuit);
            if (room != nullptr)
            {
                keep(index, *room);
            }
        };
        const float corridorWest = kCorridorWest - kX;
        const float corridorEast = kCorridorEast - kX;
        const float roomEast = kRoomEast - kX;
        const float roomSplit = kRoomSplit - kZ;
        const Room corridorRoom{{corridorWest, -0.05f, kCorridorNorth - kZ}, {corridorEast, 3.05f, kCorridorSouth - kZ}};
        const Room lockerRoom{{corridorEast + 0.3f, -0.05f, roomSplit + 0.15f}, {roomEast, 3.05f, kCorridorSouth - kZ}};
        const Room storeRoom{{corridorEast + 0.3f, -0.05f, kCorridorNorth - kZ}, {roomEast, 3.05f, roomSplit - 0.15f}};
        const Room forest{{15.5f, -0.05f, -11.0f}, {31.0f, 4.55f, -1.0f}};
        const Room nestRoom{{-16.0f, -0.05f, -kHalf}, {16.0f, kWallHeight + 0.05f, -12.0f}};

        // The corridor: one good, one flickering, and the one by the locked store failing.
        const float corridor = (kCorridorWest + kCorridorEast) * 0.5f - kX;
        const int corridorSouthLamp = ceiling(corridor, 3.0f, 15.0f, LightMood::Steady, 1, corridorRoom);
        ceiling(corridor, 3.0f, 9.5f, LightMood::Flicker, 1, corridorRoom);
        const int corridorNorthLamp = ceiling(corridor, 3.0f, 3.5f, LightMood::Failing, 1, corridorRoom);
        // A caged lamp over the way in, outside, lighting the ground in front of it.
        wall(LightKind::Wall, kCorridorDoorX - kX, 2.6f, kCorridorSouth - kZ + 0.45f, {0.0f, -0.35f, 1.0f}, LightMood::Steady, 1,
             nullptr);
        // The locker room: lit, just about.
        const int lockerLamp = ceiling(18.0f, 3.0f, 14.0f, LightMood::Steady, 1, lockerRoom);
        ceiling(25.0f, 3.0f, 14.0f, LightMood::Flicker, 1, lockerRoom);
        // The store: its strip light long dead, and an emergency lamp breathing red on the far wall.
        ceiling(21.5f, 3.0f, 5.0f, LightMood::Dead, 1, storeRoom);
        wall(LightKind::Emergency, kRoomEast - kX - 0.25f, 2.3f, 5.0f, {-1.0f, -0.2f, 0.0f}, LightMood::Pulse, 1, &storeRoom);
        const int storeRed = static_cast<int>(lights->Count()) - 1;
        // Through the two doorways off the corridor, each way: the room's light a little way into the corridor,
        // the corridor's a little way into the room. Doorways into the dark otherwise looked like holes.
        const float lockerDoor = kLockerDoorZ - kZ;
        const float storeDoor = kStoreDoorZ - kZ;
        const Room corridorByLockers{{corridorWest, -0.05f, lockerDoor - 2.0f}, {corridorEast, 3.05f, lockerDoor + 2.0f}};
        const Room corridorByStore{{corridorWest, -0.05f, storeDoor - 2.0f}, {corridorEast, 3.05f, storeDoor + 2.0f}};
        const Room lockersByDoor{{corridorEast + 0.3f, -0.05f, lockerDoor - 2.0f}, {corridorEast + 3.5f, 3.05f, lockerDoor + 2.0f}};
        const Room storeByDoor{{corridorEast + 0.3f, -0.05f, storeDoor - 2.0f}, {corridorEast + 3.5f, 3.05f, storeDoor + 2.0f}};
        spill(lockerLamp, corridorEast, lockerDoor, {-1.0f, -0.3f, 0.0f}, corridorByLockers);
        spill(corridorSouthLamp, corridorEast, lockerDoor, {1.0f, -0.3f, 0.0f}, lockersByDoor);
        spill(storeRed, corridorEast, storeDoor, {-1.0f, -0.3f, 0.0f}, corridorByStore);
        spill(corridorNorthLamp, corridorEast, storeDoor, {1.0f, -0.3f, 0.0f}, storeByDoor);

        // The pillar forest: one failing, one dead. Dark on purpose: it is for being stalked in.
        ceiling(19.5f, 4.5f, -6.0f, LightMood::Failing, 2, forest);
        ceiling(27.0f, 4.5f, -4.0f, LightMood::Dead, 2, forest);

        // The nest chamber: the strip lights dead, and a red emergency lamp inside each way in.
        ceiling(-6.0f, kWallHeight, -22.0f, LightMood::Dead, 3, nestRoom);
        ceiling(6.0f, kWallHeight, -22.0f, LightMood::Dead, 3, nestRoom);
        wall(LightKind::Emergency, -8.0f, 3.4f, -12.6f, {0.0f, -0.3f, -1.0f}, LightMood::Pulse, 3, &nestRoom);
        wall(LightKind::Emergency, 8.0f, 3.4f, -12.6f, {0.0f, -0.3f, -1.0f}, LightMood::Pulse, 3, &nestRoom);
    }
}

} // namespace pred
