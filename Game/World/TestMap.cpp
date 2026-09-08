#include "Game/World/TestMap.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <string>

namespace pred
{
namespace
{

using namespace TestMapSpec;

// A restrained, cold palette. Zones differ enough to be told apart without looking like a toy.
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

} // namespace

void BuildTestMap(Scene& scene, MeshLibrary& meshes)
{
    // ---------------------------------------------------------------------
    // Shared meshes. Uploaded once, referenced by many entities.
    // ---------------------------------------------------------------------
    const MeshHandle groundMesh =
        meshes.Upload(Primitives::Plane({kGroundSize, kGroundSize}, 45), "ground");
    const MeshHandle markerMesh = meshes.Upload(Primitives::Sphere(0.15f, 20, 14), "marker");
    const MeshHandle referenceCube = meshes.Upload(Primitives::Box({1.0f, 1.0f, 1.0f}), "reference_cube");
    const MeshHandle pillarMesh = meshes.Upload(Primitives::Cylinder(0.35f, 4.5f, 20), "pillar");

    scene.CreateMeshEntity("ground", AtPosition(0.0f, 0.0f, 0.0f), groundMesh, kFloorMaterial);

    // A 1 m cube at the origin is the scale reference for everything else.
    scene.CreateMeshEntity("reference_cube", AtPosition(0.0f, 0.5f, 0.0f), referenceCube, kMarkerMaterial);

    // ---------------------------------------------------------------------
    // +X: staircases. Same total climb, different step rises.
    // ---------------------------------------------------------------------
    float stairX = 8.0f;
    for (const float rise : kStepRises)
    {
        const int steps = static_cast<int>(std::round(2.4f / rise));
        const MeshHandle mesh = meshes.Upload(Primitives::Stairs(steps, 3.0f, rise, 0.30f),
                                              "stairs_" + std::to_string(static_cast<int>(rise * 100.0f)));
        scene.CreateMeshEntity("stairs", AtPosition(stairX, 0.0f, -4.0f), mesh, kStairMaterial);

        // A landing at the top, so there is somewhere to arrive at and step off.
        const float topHeight = rise * static_cast<float>(steps);
        const MeshHandle landing = meshes.Upload(Primitives::Box({3.0f, topHeight, 3.0f}), "stair_landing");
        scene.CreateMeshEntity("stair_landing",
                               AtPosition(stairX, topHeight * 0.5f, -4.0f + 0.30f * steps + 1.5f), landing,
                               kStairMaterial);
        stairX += 5.0f;
    }

    // ---------------------------------------------------------------------
    // -X: ramps at increasing angles. The steepest should be the walk/slide boundary.
    // ---------------------------------------------------------------------
    float rampX = -8.0f;
    for (const float angleDegrees : kRampAngles)
    {
        constexpr float rampLength = 6.0f;
        const float height = rampLength * std::tan(glm::radians(angleDegrees));
        const MeshHandle mesh = meshes.Upload(Primitives::Ramp(3.0f, rampLength, height),
                                              "ramp_" + std::to_string(static_cast<int>(angleDegrees)));
        scene.CreateMeshEntity("ramp", AtPosition(rampX, 0.0f, -4.0f), mesh, kRampMaterial);

        // Marker floating at the top of each ramp, to read the angle at a glance.
        scene.CreateMeshEntity("ramp_marker", AtPosition(rampX, height + 0.6f, -4.0f + rampLength), markerMesh,
                               kMarkerMaterial);
        rampX -= 5.0f;
    }

    // ---------------------------------------------------------------------
    // +Z: ledges at mantle-relevant heights.
    // ---------------------------------------------------------------------
    float ledgeX = -7.5f;
    for (const float height : kLedgeHeights)
    {
        const MeshHandle mesh = meshes.Upload(Primitives::Box({2.4f, height, 2.4f}),
                                              "ledge_" + std::to_string(static_cast<int>(height * 100.0f)));
        scene.CreateMeshEntity("ledge", AtPosition(ledgeX, height * 0.5f, 10.0f), mesh, kLedgeMaterial);
        ledgeX += 3.0f;
    }

    // ---------------------------------------------------------------------
    // -Z: a corridor whose gaps narrow, for collision and squeeze tuning.
    // ---------------------------------------------------------------------
    constexpr float corridorZ = -14.0f;
    constexpr float wallThickness = 0.4f;
    constexpr float wallHeight = 3.2f;
    constexpr float segmentLength = 4.0f;

    float gapX = -10.0f;
    for (const float gap : kGapWidths)
    {
        const float halfGap = gap * 0.5f;
        constexpr float sideWidth = 3.0f;
        const MeshHandle wall =
            meshes.Upload(Primitives::Box({sideWidth, wallHeight, wallThickness}), "corridor_wall");

        // Two blocks with a gap between them, rotated to face down the corridor.
        scene.CreateMeshEntity("gap_wall_left",
                               AtPosition(gapX - halfGap - sideWidth * 0.5f, wallHeight * 0.5f, corridorZ), wall,
                               kWallMaterial);
        scene.CreateMeshEntity("gap_wall_right",
                               AtPosition(gapX + halfGap + sideWidth * 0.5f, wallHeight * 0.5f, corridorZ), wall,
                               kWallMaterial);

        // Lintel above, forming a doorway rather than an open slot.
        const MeshHandle lintel =
            meshes.Upload(Primitives::Box({gap, wallHeight - kDoorHeight, wallThickness}), "gap_lintel");
        scene.CreateMeshEntity("gap_lintel",
                               AtPosition(gapX, kDoorHeight + (wallHeight - kDoorHeight) * 0.5f, corridorZ),
                               lintel, kWallMaterial);
        gapX += segmentLength;
    }

    // ---------------------------------------------------------------------
    // Pillars: occlusion, cover, and future line-of-sight tests.
    // ---------------------------------------------------------------------
    const float pillarPositions[][2] = {{-14.0f, 14.0f}, {14.0f, 14.0f}, {-14.0f, -22.0f}, {14.0f, -22.0f},
                                        {0.0f, 20.0f},   {-20.0f, 4.0f}, {20.0f, 4.0f}};
    for (const auto& position : pillarPositions)
    {
        scene.CreateMeshEntity("pillar", AtPosition(position[0], 2.25f, position[1]), pillarMesh,
                               kPillarMaterial);
    }

    // A rotated block, to prove non-axis-aligned transforms survive the whole pipeline.
    const MeshHandle angledBlock = meshes.Upload(Primitives::Box({2.0f, 1.0f, 4.0f}), "angled_block");
    scene.CreateMeshEntity("angled_block", AtPositionYaw(6.0f, 0.5f, 12.0f, 35.0f), angledBlock,
                           kLedgeMaterial);

    PRED_LOG_INFO(Gameplay, "Test map built: {} entities, {} meshes", scene.EntityCount(), meshes.Count());
}

} // namespace pred
