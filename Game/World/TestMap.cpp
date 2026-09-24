#include "Game/World/TestMap.h"

#include "Game/World/MapBuilder.h"

#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <iterator>
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


    // Each zone gets a painted floor, so from the spawn the map reads as a set of rooms rather than a
    // field of scattered props. Decoration: no collider.
    //
    // There were emissive posts at the corners and floating spheres over the ramps as well. They
    // were a legend for a map nobody needs a legend for any more, and being emissive they were the
    // brightest things in every dark scene -- six glowing sticks in the middle of the horror game.
    int zoneIndex = 0;
    auto zone = [&](const char* name, float centreX, float centreZ, float sizeX, float sizeZ,
                    const glm::vec3& tint)
    {
        const MeshHandle pad =
            meshes.Upload(Primitives::Plane({sizeX, sizeZ}, 1), std::string("zone_pad_") + name);
        // Each pad sits a couple of millimetres above the last. The zones do not overlap any more,
        // but two coplanar surfaces flicker horribly where they do, and a two millimetre step is
        // invisible; this makes the layout impossible to get wrong in that particular way.
        const float height = 0.012f + 0.002f * static_cast<float>(zoneIndex++);
        builder.AddDecoration("zone_pad", AtPosition(centreX, height, centreZ), pad,
                              Material::Diffuse(tint, 0.95f));
    };

    // ---------------------------------------------------------------------
    // Origin: the spawn plaza. The spawn looks south, with every zone in view.
    // ---------------------------------------------------------------------
    // Kept clear of the ledge row in front of it. Two pads sharing ground looked like a patch, and
    // before the heights were staggered they flickered against each other as well.
    zone("spawn", 0.0f, 15.6f, 8.0f, 8.0f, {0.30f, 0.32f, 0.36f});

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

        rampX -= 5.0f;
    }

    // ---------------------------------------------------------------------
    // +Z: ledges at mantle-relevant heights, in one rising row.
    // ---------------------------------------------------------------------
    zone("ledges", 0.0f, 10.0f, 19.0f, 3.0f, {0.34f, 0.26f, 0.26f});
    float ledgeX = -7.5f;
    for (const float height : kLedgeHeights)
    {
        builder.AddBox("ledge_" + std::to_string(static_cast<int>(height * 100.0f)),
                       AtPosition(ledgeX, height * 0.5f, 10.0f), {2.4f, height, 2.4f}, kLedgeMaterial);
        ledgeX += 3.0f;
    }

    // ---------------------------------------------------------------------
    // Between the ledges and the stairs: the footstep surfaces, in a row.
    //
    // Low enough to walk straight onto without a step up, wide enough to get two or three paces on
    // one before reaching the next. Each is tinted differently so it is obvious which is underfoot
    // while listening, and the colours are roughly what the material looks like rather than an
    // arbitrary palette: what you hear and what you see should agree.
    // ---------------------------------------------------------------------
    zone("surfaces", 0.0f, kSurfaceRowZ, 10.4f, kSurfacePadDepth + 0.6f, {0.28f, 0.28f, 0.30f});
    {
        const Material padMaterials[] = {
            Material::Diffuse({0.46f, 0.46f, 0.45f}, 0.94f), // concrete: flat grey
            Material::Diffuse({0.55f, 0.54f, 0.50f}, 0.78f), // stone: paler, harder
            Material::Diffuse({0.40f, 0.44f, 0.48f}, 0.35f), // metal: blue-grey and shiny
            Material::Diffuse({0.40f, 0.36f, 0.30f}, 0.98f), // gravel: brown and rough
            Material::Diffuse({0.44f, 0.31f, 0.19f}, 0.85f), // wood: warm
        };
        static_assert(std::size(padMaterials) == std::size(kSurfacePads),
                      "every surface pad needs a material");
        for (size_t i = 0; i < std::size(kSurfacePads); ++i)
        {
            const SurfacePad& pad = kSurfacePads[i];
            builder.AddBox(std::string("surface_") + pad.surface,
                           AtPosition(pad.centreX, kSurfacePadHeight * 0.5f, pad.centreZ),
                           {pad.sizeX, kSurfacePadHeight, pad.sizeZ}, padMaterials[i]);
        }
    }

    // ---------------------------------------------------------------------
    // Three panels of the same metal at three roughnesses, to look at what the
    // reflections actually do.
    //
    // A polished surface is the only thing that shows whether reflection is working: everything else
    // in the map is rough enough that its reflection is a wash. Standing in front of these and
    // walking sideways, the sky slides across the mirror and stays put on the matte one, which is
    // the difference between reflecting the world and being painted a lighter colour.
    //
    // They do show the room, now. All three lie in one plane, which is what lets them share a single
    // reflection pass: a planar reflection is the world drawn again from a camera mirrored across
    // one flat surface, so mirrors in different planes need a pass each and these deliberately do
    // not. Left to right they are polished, brushed and matte, and the reflection is mixed in less
    // as the surface roughens, because a rough mirror is not a dimmer mirror but a blurrier one and
    // this renderer cannot blur it yet. See kMirrorPlane in TestMap.h.
    // ---------------------------------------------------------------------
    {
        constexpr float kMirrorHeight = 2.0f;
        const float roughnesses[] = {0.03f, 0.18f, 0.45f};
        const float reflectivity[] = {0.90f, 0.45f, 0.0f};
        const char* names[] = {"mirror_polished", "mirror_brushed", "mirror_matte"};
        for (int i = 0; i < 3; ++i)
        {
            Material panel = Material::Metal({0.92f, 0.93f, 0.95f}, roughnesses[i]);
            panel.reflectivity = reflectivity[i];
            builder.AddBox(names[i], AtPosition(-2.2f + 2.2f * static_cast<float>(i),
                                                kMirrorHeight * 0.5f, kMirrorZ),
                           {1.9f, kMirrorHeight, kMirrorThickness}, panel);
        }
    }

    // ---------------------------------------------------------------------
    // A room with no light in it.
    //
    // Four walls, a roof and one doorway, west of the spawn. It exists because a flashlight cannot
    // be judged anywhere else: outdoors under a sun and a sky there is nothing for a beam to be
    // brighter than.
    //
    // Nothing here declares the room dark and nothing in the game looks up whether a position is
    // inside it. It is dark because the roof is between it and the sky, which the renderer works out
    // for itself from this geometry: see Engine/Render/ShadowMap.h. Build a roof anywhere else and
    // it will be dark underneath too.
    // ---------------------------------------------------------------------
    {
        const Material kDarkWall = Material::Diffuse({0.20f, 0.20f, 0.22f}, 0.92f);
        const float halfX = kDarkRoomWidth * 0.5f;
        const float halfZ = kDarkRoomDepth * 0.5f;
        const float t = kDarkRoomWallThickness;
        const float h = kDarkRoomHeight;

        // Floor, a little proud of the ground so the two do not fight for the same pixels.
        //
        // Every piece here runs from the inside face of one wall to the inside face of the next, so
        // they butt rather than overlap or leave a slot. That is fiddlier than it sounds and it is
        // not cosmetic: the first version left 15 cm gaps at both back corners, and with real
        // occlusion they read as two blades of daylight down the inside of a supposedly sealed room.
        // A wall that does not meet its neighbour is a window.
        builder.AddBox("dark_floor", AtPosition(kDarkRoomX, 0.03f, kDarkRoomZ),
                       {kDarkRoomWidth - t, 0.06f, kDarkRoomDepth - t}, kDarkWall);
        // Thick, so the top of the walls inside is not read as under the open sky: a shadow map counts a
        // surface as lit within a few centimetres of whatever is over it, and a thin roof let daylight
        // along the top of every wall.
        constexpr float kRoof = 0.6f;
        builder.AddBox("dark_roof", AtPosition(kDarkRoomX, h + kRoof * 0.5f, kDarkRoomZ),
                       {kDarkRoomWidth + t * 2.0f, kRoof, kDarkRoomDepth + t * 2.0f}, kDarkWall);

        // Back and sides. The west wall runs the full outer depth and the other two stop against
        // its inside face, so the corner is closed by one of them rather than by neither.
        builder.AddBox("dark_wall_w", AtPosition(kDarkRoomX - halfX, h * 0.5f, kDarkRoomZ),
                       {t, h, kDarkRoomDepth + t * 2.0f}, kDarkWall);
        builder.AddBox("dark_wall_n", AtPosition(kDarkRoomX, h * 0.5f, kDarkRoomZ - halfZ),
                       {kDarkRoomWidth - t, h, t}, kDarkWall);
        builder.AddBox("dark_wall_s", AtPosition(kDarkRoomX, h * 0.5f, kDarkRoomZ + halfZ),
                       {kDarkRoomWidth - t, h, t}, kDarkWall);

        // The east wall, with a doorway in the middle of it facing the spawn. Two posts and a
        // lintel rather than a hole, because the geometry is boxes. The posts reach the outer
        // corners for the same reason the west wall does.
        const float postWidth = (kDarkRoomDepth + t * 2.0f - kDarkRoomDoorWidth) * 0.5f;
        const float postCentre = (kDarkRoomDoorWidth + postWidth) * 0.5f;
        builder.AddBox("dark_door_a", AtPosition(kDarkRoomX + halfX, h * 0.5f, kDarkRoomZ - postCentre),
                       {t, h, postWidth}, kDarkWall);
        builder.AddBox("dark_door_b", AtPosition(kDarkRoomX + halfX, h * 0.5f, kDarkRoomZ + postCentre),
                       {t, h, postWidth}, kDarkWall);
        const float lintelHeight = h - kDoorHeight;
        builder.AddBox("dark_lintel",
                       AtPosition(kDarkRoomX + halfX, kDoorHeight + lintelHeight * 0.5f, kDarkRoomZ),
                       {t, lintelHeight, kDarkRoomDoorWidth}, kDarkWall);

        // Something inside to point a torch at. A beam on an empty floor says very little; a beam
        // across a box, a cylinder and a sphere says whether the falloff and the cone are right.
        builder.AddBox("dark_crate", AtPosition(kDarkRoomX - 1.6f, 0.51f, kDarkRoomZ - 1.4f),
                       {0.9f, 0.9f, 0.9f}, Material::Diffuse({0.38f, 0.34f, 0.28f}, 0.85f));
        builder.AddMesh("dark_pillar",
                        AtPosition(kDarkRoomX + 1.8f, 1.36f, kDarkRoomZ + 1.2f),
                        Primitives::Cylinder(0.28f, 2.6f, 16), kPillarMaterial);
        builder.AddMesh("dark_sphere", AtPosition(kDarkRoomX, 0.68f, kDarkRoomZ + 2.4f),
                        Primitives::Sphere(0.55f, 24, 16),
                        Material::Metal({0.62f, 0.63f, 0.66f}, 0.30f));
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

const char* SurfaceUnderfoot(float worldX, float worldZ)
{
    for (const TestMapSpec::SurfacePad& pad : TestMapSpec::kSurfacePads)
    {
        if (std::abs(worldX - pad.centreX) <= pad.sizeX * 0.5f &&
            std::abs(worldZ - pad.centreZ) <= pad.sizeZ * 0.5f)
        {
            return pad.surface;
        }
    }
    return nullptr;
}
} // namespace pred
