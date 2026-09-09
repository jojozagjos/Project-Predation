#include "Game/World/TestMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace pred
{
namespace
{

using namespace TestMapSpec;

// A restrained, cold palette. Zones differ enough to tell apart without looking like a toy.
const Material kFloorMaterial = Material::Diffuse({0.30f, 0.31f, 0.34f}, 0.92f);
const Material kStairMaterial = Material::Diffuse({0.42f, 0.38f, 0.30f}, 0.85f);
const Material kRampMaterial = Material::Diffuse({0.30f, 0.38f, 0.42f}, 0.85f);
const Material kLedgeMaterial = Material::Diffuse({0.44f, 0.32f, 0.32f}, 0.80f);
const Material kWallMaterial = Material::Diffuse({0.26f, 0.27f, 0.30f}, 0.95f);
const Material kPillarMaterial = Material::Metal({0.55f, 0.56f, 0.60f}, 0.42f);
const Material kMarkerMaterial = Material::Emissive({0.9f, 0.45f, 0.2f}, 0.6f);

Transform AtPosition(float x, float y, float z)
{
    Transform transform;
    transform.position = {x, y, z};
    return transform;
}

Transform AtPositionYaw(float x, float y, float z, float yawDegrees)
{
    Transform transform = AtPosition(x, y, z);
    transform.rotation = glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
    return transform;
}

// Builds visual and collision geometry together so the two can never drift apart.
class MapBuilder
{
public:
    MapBuilder(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics)
        : m_scene(scene), m_meshes(meshes), m_physics(physics)
    {
    }

    // Box: rendered as a box mesh, collided as a box shape. Cheaper and more robust than a
    // triangle mesh, and exact for this shape.
    void AddBox(const std::string& name, const Transform& transform, const glm::vec3& size,
                const Material& material)
    {
        const MeshHandle mesh = m_meshes.Upload(Primitives::Box(size), name);
        m_scene.CreateMeshEntity(name, transform, mesh, material);
        if (m_physics != nullptr)
        {
            m_physics->CreateBox(size * 0.5f, transform, BodyMotion::Static);
        }
    }

    // Reuses an already-uploaded mesh, for shapes placed many times.
    void AddBoxInstance(const std::string& name, const Transform& transform, MeshHandle mesh,
                        const glm::vec3& size, const Material& material)
    {
        m_scene.CreateMeshEntity(name, transform, mesh, material);
        if (m_physics != nullptr)
        {
            m_physics->CreateBox(size * 0.5f, transform, BodyMotion::Static);
        }
    }

    // Arbitrary geometry: collided as a static triangle mesh so stairs and ramps are exact.
    void AddMesh(const std::string& name, const Transform& transform, const MeshData& data,
                 const Material& material, bool collide = true)
    {
        const MeshHandle mesh = m_meshes.Upload(data, name);
        m_scene.CreateMeshEntity(name, transform, mesh, material);
        if (collide && m_physics != nullptr)
        {
            m_physics->CreateMeshBody(data, transform);
        }
    }

    void AddMeshInstance(const std::string& name, const Transform& transform, MeshHandle mesh,
                         const MeshData& data, const Material& material, bool collide = true)
    {
        m_scene.CreateMeshEntity(name, transform, mesh, material);
        if (collide && m_physics != nullptr)
        {
            m_physics->CreateMeshBody(data, transform);
        }
    }

    // Visual only: markers and other things the player should pass through.
    void AddDecoration(const std::string& name, const Transform& transform, MeshHandle mesh,
                       const Material& material)
    {
        m_scene.CreateMeshEntity(name, transform, mesh, material);
    }

private:
    Scene& m_scene;
    MeshLibrary& m_meshes;
    PhysicsWorld* m_physics;
};

} // namespace

void BuildTestMap(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics)
{
    MapBuilder builder(scene, meshes, physics);

    // ---------------------------------------------------------------------
    // Ground. Rendered as a subdivided plane, collided as a slab whose top
    // face sits exactly at y = 0, so visual and collision surfaces agree.
    // ---------------------------------------------------------------------
    const MeshHandle groundMesh =
        meshes.Upload(Primitives::Plane({kGroundSize, kGroundSize}, 45), "ground");
    scene.CreateMeshEntity("ground", AtPosition(0.0f, 0.0f, 0.0f), groundMesh, kFloorMaterial);
    if (physics != nullptr)
    {
        constexpr float slabHalfThickness = 0.5f;
        physics->CreateBox({kGroundSize * 0.5f, slabHalfThickness, kGroundSize * 0.5f},
                           AtPosition(0.0f, -slabHalfThickness, 0.0f), BodyMotion::Static);
    }

    const MeshHandle markerMesh = meshes.Upload(Primitives::Sphere(0.15f, 20, 14), "marker");

    // Each zone gets a painted floor and a coloured post at one corner, so from the spawn the map
    // reads as a set of rooms rather than a field of scattered props. Both are decoration: no
    // collider, and the post is emissive so it can be found in the dark.
    const MeshData postData = Primitives::Cylinder(0.09f, 2.4f, 12);
    const MeshHandle postMesh = meshes.Upload(postData, "zone_post");
    auto zone = [&](const char* name, float centreX, float centreZ, float sizeX, float sizeZ,
                    const glm::vec3& tint)
    {
        const MeshHandle pad =
            meshes.Upload(Primitives::Plane({sizeX, sizeZ}, 1), std::string("zone_pad_") + name);
        // A hair above the ground, so the two surfaces do not fight over the same depth.
        builder.AddDecoration("zone_pad", AtPosition(centreX, 0.012f, centreZ), pad,
                              Material::Diffuse(tint, 0.95f));
        builder.AddDecoration("zone_post",
                              AtPosition(centreX - sizeX * 0.5f + 0.3f, 1.2f, centreZ + sizeZ * 0.5f - 0.3f),
                              postMesh, Material::Emissive(tint * 2.2f, 0.55f));
    };

    // ---------------------------------------------------------------------
    // Origin: the spawn plaza, with a 1 m cube as the scale reference for
    // everything else. The spawn looks south, with every zone in view.
    // ---------------------------------------------------------------------
    zone("spawn", 0.0f, 13.0f, 9.0f, 9.0f, {0.30f, 0.32f, 0.36f});
    builder.AddBox("reference_cube", AtPosition(0.0f, 0.5f, 13.5f), {1.0f, 1.0f, 1.0f}, kMarkerMaterial);

    // ---------------------------------------------------------------------
    // +X: staircases. Same total climb, different step rises, side by side so
    // one can be tried straight after another.
    // ---------------------------------------------------------------------
    zone("stairs", 13.0f, -1.0f, 16.0f, 12.0f, {0.34f, 0.31f, 0.25f});
    float stairX = 8.0f;
    for (const float rise : kStepRises)
    {
        const int steps = static_cast<int>(std::round(2.4f / rise));
        const MeshData stairData = Primitives::Stairs(steps, 3.0f, rise, 0.30f);
        builder.AddMesh("stairs_" + std::to_string(static_cast<int>(rise * 100.0f)),
                        AtPosition(stairX, 0.0f, -4.0f), stairData, kStairMaterial);

        // A landing at the top, so there is somewhere to arrive and step off.
        const float topHeight = rise * static_cast<float>(steps);
        builder.AddBox("stair_landing",
                       AtPosition(stairX, topHeight * 0.5f, -4.0f + 0.30f * static_cast<float>(steps) + 1.5f),
                       {3.0f, topHeight, 3.0f}, kStairMaterial);
        stairX += 5.0f;
    }

    // ---------------------------------------------------------------------
    // -X: ramps at increasing angles. The steepest is the walk/slide boundary.
    // ---------------------------------------------------------------------
    zone("ramps", -15.5f, -1.0f, 21.0f, 12.0f, {0.24f, 0.30f, 0.34f});
    float rampX = -8.0f;
    for (const float angleDegrees : kRampAngles)
    {
        constexpr float rampLength = 6.0f;
        const float height = rampLength * std::tan(glm::radians(angleDegrees));
        const MeshData rampData = Primitives::Ramp(3.0f, rampLength, height);
        builder.AddMesh("ramp_" + std::to_string(static_cast<int>(angleDegrees)),
                        AtPosition(rampX, 0.0f, -4.0f), rampData, kRampMaterial);

        // Marker floating above each ramp, to read the angle at a glance.
        builder.AddDecoration("ramp_marker", AtPosition(rampX, height + 0.6f, -4.0f + rampLength), markerMesh,
                              kMarkerMaterial);
        rampX -= 5.0f;
    }

    // ---------------------------------------------------------------------
    // +Z: ledges at mantle-relevant heights, in one rising row.
    // ---------------------------------------------------------------------
    zone("ledges", 0.0f, 10.0f, 19.0f, 3.4f, {0.34f, 0.26f, 0.26f});
    float ledgeX = -7.5f;
    for (const float height : kLedgeHeights)
    {
        builder.AddBox("ledge_" + std::to_string(static_cast<int>(height * 100.0f)),
                       AtPosition(ledgeX, height * 0.5f, 10.0f), {2.4f, height, 2.4f}, kLedgeMaterial);
        ledgeX += 3.0f;
    }

    // ---------------------------------------------------------------------
    // -Z: a corridor whose doorways narrow, for collision and squeeze tuning.
    // ---------------------------------------------------------------------
    constexpr float corridorZ = -14.0f;
    constexpr float wallThickness = 0.4f;
    constexpr float wallHeight = 3.2f;
    // Doorways must be spaced further apart than the walls flanking them are wide, or consecutive
    // segments build walls on top of each other. At the previous 4 m spacing with 3 m walls they
    // overlapped almost entirely.
    constexpr float sideWidth = 2.4f;
    constexpr float segmentLength = 7.5f;

    zone("corridor", 0.0f, corridorZ, 42.0f, 4.0f, {0.26f, 0.27f, 0.31f});

    const glm::vec3 sideWallSize{sideWidth, wallHeight, wallThickness};
    const MeshHandle sideWallMesh = meshes.Upload(Primitives::Box(sideWallSize), "corridor_wall");

    float gapX = -15.0f;
    for (const float gap : kGapWidths)
    {
        const float halfGap = gap * 0.5f;

        builder.AddBoxInstance("gap_wall_left",
                               AtPosition(gapX - halfGap - sideWidth * 0.5f, wallHeight * 0.5f, corridorZ),
                               sideWallMesh, sideWallSize, kWallMaterial);
        builder.AddBoxInstance("gap_wall_right",
                               AtPosition(gapX + halfGap + sideWidth * 0.5f, wallHeight * 0.5f, corridorZ),
                               sideWallMesh, sideWallSize, kWallMaterial);

        // Lintel above, so it reads as a doorway rather than an open slot.
        const float lintelHeight = wallHeight - kDoorHeight;
        builder.AddBox("gap_lintel", AtPosition(gapX, kDoorHeight + lintelHeight * 0.5f, corridorZ),
                       {gap, lintelHeight, wallThickness}, kWallMaterial);
        gapX += segmentLength;
    }

    // ---------------------------------------------------------------------
    // Equipment bay: every loose item in the level, in one row on one bench,
    // weapons included. Items used to be scattered across four zones, which
    // taught nobody anything and made the guns impossible to find.
    // WorldObjects lays the items themselves out on top of this.
    // ---------------------------------------------------------------------
    zone("equipment", kEquipmentBayX, kBayZ, kBayHalfWidth * 2.0f + 1.4f, 5.6f, {0.24f, 0.32f, 0.26f});
    builder.AddBox("equipment_bench", AtPosition(kEquipmentBayX, kBenchTop * 0.5f, kBayZ + 1.3f),
                   {kBayHalfWidth * 2.0f, kBenchTop, 0.75f}, kStairMaterial);
    builder.AddBox("equipment_backboard", AtPosition(kEquipmentBayX, 1.35f, kBayZ + 1.95f),
                   {kBayHalfWidth * 2.0f + 0.6f, 2.7f, 0.2f}, kWallMaterial);

    // ---------------------------------------------------------------------
    // Interaction bay: both doors and both lockers together, so everything
    // you operate is one place you can walk between.
    // WorldObjects hangs the doors and stands the lockers here.
    // ---------------------------------------------------------------------
    zone("interaction", kInteractionBayX, kBayZ, kBayHalfWidth * 2.0f + 1.4f, 5.6f, {0.30f, 0.26f, 0.34f});
    builder.AddBox("locker_backboard", AtPosition(kInteractionBayX, 1.35f, kBayZ + 1.95f),
                   {kBayHalfWidth * 2.0f + 0.6f, 2.7f, 0.2f}, kWallMaterial);

    // Two door frames facing the plaza, wide enough to walk through and tall enough not to duck.
    for (int side = 0; side < 2; ++side)
    {
        const float frameX = kInteractionBayX + (side == 0 ? -1.55f : 1.55f);
        constexpr float openingWidth = 1.15f;
        constexpr float frameHeight = 2.6f;
        constexpr float postWidth = 0.35f;
        const float frameZ = kBayZ - 2.1f;
        for (int post = 0; post < 2; ++post)
        {
            const float postX = frameX + (post == 0 ? -1.0f : 1.0f) * (openingWidth + postWidth) * 0.5f;
            builder.AddBox("door_post", AtPosition(postX, frameHeight * 0.5f, frameZ),
                           {postWidth, frameHeight, 0.28f}, kWallMaterial);
        }
        builder.AddBox("door_lintel", AtPosition(frameX, (frameHeight + kDoorHeight) * 0.5f, frameZ),
                       {openingWidth, frameHeight - kDoorHeight, 0.28f}, kWallMaterial);
    }

    // ---------------------------------------------------------------------
    // Pillars: occlusion, cover, and future line-of-sight tests.
    // ---------------------------------------------------------------------
    const MeshData pillarData = Primitives::Cylinder(0.35f, 4.5f, 20);
    const MeshHandle pillarMesh = meshes.Upload(pillarData, "pillar");
    const float pillarPositions[][2] = {{-6.0f, 17.5f},  {6.0f, 17.5f},   {-14.0f, -22.0f},
                                        {14.0f, -22.0f}, {-22.0f, 12.0f}, {22.0f, 12.0f}};
    for (const auto& position : pillarPositions)
    {
        builder.AddMeshInstance("pillar", AtPosition(position[0], 2.25f, position[1]), pillarMesh, pillarData,
                                kPillarMaterial);
    }

    // A rotated block, to prove non-axis-aligned transforms survive the whole pipeline.
    builder.AddBox("angled_block", AtPositionYaw(0.0f, 0.5f, 20.5f, 35.0f), {2.0f, 1.0f, 4.0f},
                   kLedgeMaterial);

    if (physics != nullptr)
    {
        // The level is static and fully built at this point, so rebuild the broad-phase once rather
        // than leaving it in creation order.
        physics->OptimizeBroadPhase();
    }

    PRED_LOG_INFO(Gameplay, "Test map built: {} entities, {} meshes, {} physics bodies", scene.EntityCount(),
                  meshes.Count(), physics != nullptr ? physics->GetStats().bodyCount : 0u);
}

} // namespace pred
