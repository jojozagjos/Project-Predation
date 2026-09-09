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

    // A 1 m cube at the origin is the scale reference for everything else.
    builder.AddBox("reference_cube", AtPosition(0.0f, 0.5f, 0.0f), {1.0f, 1.0f, 1.0f}, kMarkerMaterial);

    // ---------------------------------------------------------------------
    // +X: staircases. Same total climb, different step rises.
    // ---------------------------------------------------------------------
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
    // +Z: ledges at mantle-relevant heights.
    // ---------------------------------------------------------------------
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
    // Pillars: occlusion, cover, and future line-of-sight tests.
    // ---------------------------------------------------------------------
    const MeshData pillarData = Primitives::Cylinder(0.35f, 4.5f, 20);
    const MeshHandle pillarMesh = meshes.Upload(pillarData, "pillar");
    const float pillarPositions[][2] = {{-14.0f, 14.0f}, {14.0f, 14.0f}, {-14.0f, -22.0f}, {14.0f, -22.0f},
                                        {0.0f, 20.0f},   {-20.0f, 4.0f}, {20.0f, 4.0f}};
    for (const auto& position : pillarPositions)
    {
        builder.AddMeshInstance("pillar", AtPosition(position[0], 2.25f, position[1]), pillarMesh, pillarData,
                                kPillarMaterial);
    }

    // A rotated block, to prove non-axis-aligned transforms survive the whole pipeline. Placed
    // clear of the ledge row, which it used to intersect.
    builder.AddBox("angled_block", AtPositionYaw(10.0f, 0.5f, 13.5f, 35.0f), {2.0f, 1.0f, 4.0f},
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
