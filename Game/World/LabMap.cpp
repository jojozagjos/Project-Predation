#include "Game/World/LabMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/ParallelFor.h"
#include "Engine/Render/MeshSimplify.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Render/Sdf.h"
#include "Engine/Render/SurfaceNets.h"
#include "Game/World/MapBuilder.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <chrono>
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

// The hive: a mound of something grown rather than built, heaped round a gaping hollow, with egg sacs
// half sunk in its sides and cords rising from it into the dark of the roof. Sculpted the way a
// creature is, as blended shapes, and drawn as one surface.
MeshData BuildHive()
{
    using namespace Sdf;
    constexpr uint32_t kSeed = 0x1ee7u;
    struct Cord
    {
        glm::vec3 a, b, c;
        float r0, r1;
    };
    std::vector<glm::vec3> lumps;
    std::vector<float> lumpRadius;
    std::vector<glm::vec3> sacs;
    std::vector<Cord> cords;
    for (int i = 0; i < 14; ++i)
    {
        const float angle = static_cast<float>(i) * 2.39996f;
        const float r = 0.9f + 0.7f * Hash(i, 1, 0, kSeed);
        lumps.push_back({std::cos(angle) * r, 0.35f + 0.9f * Hash(i, 2, 0, kSeed) * (1.6f - r * 0.5f), std::sin(angle) * r});
        lumpRadius.push_back(0.35f + 0.35f * Hash(i, 3, 0, kSeed));
    }
    for (int i = 0; i < 7; ++i)
    {
        const float angle = static_cast<float>(i) * 0.897f + 0.3f;
        sacs.push_back({std::cos(angle) * 1.55f, 0.35f + 0.25f * Hash(i, 4, 0, kSeed), std::sin(angle) * 1.55f});
    }
    for (int i = 0; i < 8; ++i)
    {
        const float angle = static_cast<float>(i) * 0.785f + 0.2f;
        const float out = 0.6f + 1.8f * Hash(i, 5, 0, kSeed);
        const glm::vec3 foot{std::cos(angle) * 0.7f, 1.2f, std::sin(angle) * 0.7f};
        const glm::vec3 top{std::cos(angle) * out, kWallHeight + 0.2f, std::sin(angle) * out};
        const glm::vec3 bend = (foot + top) * 0.5f + glm::vec3(std::cos(angle + 1.3f), 0.0f, std::sin(angle + 1.3f)) * 0.5f;
        cords.push_back({foot, bend, top, 0.2f + 0.08f * Hash(i, 6, 0, kSeed), 0.07f});
    }

    // Which part of it a point is nearest: the mound, a sac, a cord, or the hollow.
    struct Parts
    {
        float mound;
        float sacs;
        float cords;
        float hollow;
    };
    const auto parts = [&](const glm::vec3& p)
    {
        Parts out{};
        out.mound = Ellipsoid(p - glm::vec3(0.0f, 0.45f, 0.0f), {kHiveRadius, 1.25f, kHiveRadius});
        for (size_t i = 0; i < lumps.size(); ++i)
        {
            out.mound = SmoothUnion(out.mound, glm::length(p - lumps[i]) - lumpRadius[i], 0.35f);
        }
        out.sacs = 1.0e9f;
        for (const glm::vec3& sac : sacs)
        {
            out.sacs = std::min(out.sacs, Ellipsoid(p - sac, {0.32f, 0.42f, 0.32f}));
        }
        out.cords = 1.0e9f;
        for (const Cord& cord : cords)
        {
            out.cords = std::min(out.cords, RoundCone(p, cord.a, cord.b, cord.r0, (cord.r0 + cord.r1) * 0.5f));
            out.cords = std::min(out.cords, RoundCone(p, cord.b, cord.c, (cord.r0 + cord.r1) * 0.5f, cord.r1));
        }
        out.hollow = glm::length(p - glm::vec3(0.0f, 1.65f, 0.0f)) - 0.65f;
        return out;
    };
    const auto distance = [&](const glm::vec3& p)
    {
        const Parts part = parts(p);
        float d = SmoothUnion(part.mound, part.sacs, 0.12f);
        d = SmoothUnion(d, part.cords, 0.25f);
        d = SmoothCut(d, part.hollow, 0.2f);
        // Lumpy, and the floor it grows from is the floor.
        d += (Fbm(p * 3.2f, kSeed, 3) - 0.5f) * 0.12f;
        return std::max(d, -p.y - 0.01f);
    };

    const auto started = std::chrono::steady_clock::now();
    const float reach = kHiveRadius + 2.6f;
    const DistanceGrid grid =
        SampleDistance({-reach, -0.05f, -reach}, {reach, kWallHeight + 0.35f, reach}, 0.06f, distance);
    MeshData surface = SurfaceNets(grid);
    ParallelFor(surface.vertices.size(), [&](size_t first, size_t last) {
        for (size_t v = first; v < last; ++v)
        {
            MeshVertex& vertex = surface.vertices[v];
            glm::vec3 p = vertex.position;
            const auto gradient = [&](const glm::vec3& at)
            {
                const float e = 0.02f;
                const glm::vec3 g{distance(at + glm::vec3(e, 0, 0)) - distance(at - glm::vec3(e, 0, 0)),
                                  distance(at + glm::vec3(0, e, 0)) - distance(at - glm::vec3(0, e, 0)),
                                  distance(at + glm::vec3(0, 0, e)) - distance(at - glm::vec3(0, 0, e))};
                const float length = glm::length(g);
                return length > 1e-8f ? g / length : glm::vec3(0.0f, 1.0f, 0.0f);
            };
            glm::vec3 normal = gradient(p);
            p -= normal * distance(p);
            normal = gradient(p);
            vertex.position = p;
            vertex.normal = normal;

            // Dark, wet flesh; pale, glossy sacs; black in the hollow; grey-brown cords drying out.
            const Parts part = parts(p);
            glm::vec3 colour{0.2f, 0.06f, 0.055f};
            float wet = 0.45f;
            const float nearest = std::min({part.mound, part.sacs, part.cords});
            if (part.sacs <= nearest + 0.02f)
            {
                colour = glm::vec3(0.52f, 0.46f, 0.3f);
                wet = 0.25f;
            }
            else if (part.cords <= nearest + 0.02f)
            {
                colour = glm::vec3(0.19f, 0.13f, 0.11f);
                wet = 0.6f;
            }
            if (part.hollow < 0.12f)
            {
                colour = glm::vec3(0.025f, 0.01f, 0.01f);
            }
            const float vein = 1.0f - std::abs(Fbm(p * 5.0f, kSeed + 9u, 3) * 2.0f - 1.0f);
            colour = glm::mix(colour, glm::vec3(0.38f, 0.08f, 0.07f), std::pow(vein, 10.0f) * 0.7f);
            colour *= 0.75f + 0.4f * Fbm(p * 2.0f, kSeed + 3u, 2);
            const auto channel = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
            vertex.color = channel(colour.r) | (channel(colour.g) << 8) | (channel(colour.b) << 16) | (channel(wet) << 24);
        }
    }, 256);
    std::vector<uint32_t> kept;
    MeshData hive = SimplifyMesh(surface, 16000, 0.004f, kept);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    PRED_LOG_INFO(AI, "Hive built: {} triangles in {:.0f} ms", hive.TriangleCount(), ms);
    return hive;
}

} // namespace

bool InLab(const glm::vec3& point)
{
    return std::abs(point.x - kX) < kHalf + 2.0f && std::abs(point.z - kZ) < kHalf + 2.0f;
}

void BuildLabMap(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics)
{
    MapBuilder builder(scene, meshes, physics, "lab_");
    const float h = kWallHeight;

    // The floor, drawn as a plane and collided as a slab with its top at nought.
    const MeshHandle floor = meshes.Upload(Primitives::Plane({kHalf * 2.0f, kHalf * 2.0f}, 32), "lab_floor");
    scene.CreateMeshEntity("lab_floor", At(0.0f, 0.0f, 0.0f), floor, kLabFloor);
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
    Block(builder, "lab_bench", kBench.x - kX - 1.3f, 0.0f, kBench.z - kZ - 0.32f, kBench.x - kX + 1.3f, kBenchTop,
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
    Block(builder, "lab_crawl_roof", -1.5f, 1.3f, 6.0f, 1.5f, 1.6f, 16.0f, kLabConcrete);

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
        Block(builder, "lab_wing_roof", west - t, top, north - t, room + t, top + 0.2f, south + t, kLabRoof);
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
    Block(builder, "lab_pillar_roof", 15.5f, 4.5f, -11.0f, 31.0f, 4.8f, -1.0f, kLabRoof);

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
        Block(builder, "lab_nest_roof", -16.0f - t, h, -kHalf - 0.5f, 16.0f + t, h + 0.3f, south + t, kLabRoof);

        const MeshData hive = BuildHive();
        if (!hive.vertices.empty())
        {
            const MeshHandle mesh = meshes.Upload(hive, "lab_hive");
            scene.CreateMeshEntity("lab_hive", At(kHive.x - kX, 0.0f, kHive.z - kZ), mesh, Material::Diffuse(glm::vec3(1.0f), 0.5f));
        }
        // Solid where the mound is: nothing walks into it, and the navigation mesh goes round it.
        if (physics != nullptr)
        {
            physics->CreateBox({kHiveRadius * 0.8f, 1.0f, kHiveRadius * 0.8f}, At(kHive.x - kX, 1.0f, kHive.z - kZ), BodyMotion::Static);
        }
    }
}

} // namespace pred
