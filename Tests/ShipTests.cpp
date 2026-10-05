#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/ShipMap.h"
#include "Game/World/Vehicles.h"
#include "Game/World/WorldObjects.h"

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <filesystem>

using namespace pred;

namespace
{

struct Aboard
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;
    InteractionSystem interactions;
    ItemDatabase items;
    ShipMap ship;
    WorldObjects world;

    Aboard()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 2;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        REQUIRE(items.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "items.json"));
        ship.Build(scene, meshes, physics, nullptr);
    }
};

} // namespace

TEST_CASE("The ship, and the lockers and crates put aboard it, are inside nothing", "[ship]")
{
    Aboard aboard;
    REQUIRE(aboard.ship.Built());
    aboard.world.AddFacility(aboard.scene, aboard.meshes, aboard.physics, aboard.interactions, aboard.items, aboard.ship.Placements());
    const std::vector<PhysicsWorld::StaticOverlap> overlaps = aboard.physics.FindStaticOverlaps(0.01f);
    CHECK(overlaps.empty());
    CHECK(aboard.ship.Placements().lockers.size() == 3);
    CHECK(aboard.ship.Placements().ammoCrates.size() == 2);
    // Nothing is laid out on the benches: the kit is drawn at the loadout locker.
    CHECK(aboard.ship.Placements().items.empty());
}

TEST_CASE("Everybody arrives aboard standing on the ops room's floor", "[ship]")
{
    Aboard aboard;
    for (uint8_t player = 0; player < 8; ++player)
    {
        INFO("player " << static_cast<int>(player));
        const glm::vec3 spawn = aboard.ship.Spawn(player);
        const RayHit floor = aboard.physics.RayCastStatic(spawn + glm::vec3(0.0f, 1.0f, 0.0f), {0.0f, -1.0f, 0.0f}, 3.0f);
        REQUIRE(floor.hit);
        CHECK(std::abs(floor.position.y - (ShipSpec::kOrigin.y + ShipSpec::kDeck)) < 0.05f);
        // Nothing overhead but the ceiling, well above a head.
        const RayHit above = aboard.physics.RayCastStatic(spawn + glm::vec3(0.0f, 0.2f, 0.0f), {0.0f, 1.0f, 0.0f}, 10.0f);
        REQUIRE(above.hit);
        CHECK(above.distance > 2.3f);
        CHECK(aboard.ship.Contains(spawn));
    }
    // And the ship is well away from everywhere else.
    CHECK_FALSE(aboard.ship.Contains({0.0f, 0.0f, 0.0f}));
    CHECK_FALSE(aboard.ship.Contains({250.0f, 0.0f, 40.0f}));
}

TEST_CASE("The loadout locker is the first thing seen coming into the gear room, its screen on its face", "[ship]")
{
    Aboard aboard;
    const CinePose locker = aboard.ship.LoadoutLocker();
    // In the gear room, to starboard of the corridor, at the height of a face.
    const glm::vec3 local = locker.position - ShipSpec::kOrigin;
    CHECK(local.x > 1.0f);
    CHECK(local.z > -4.0f);
    CHECK(local.z < 3.5f);
    CHECK(local.y > 1.2f);
    CHECK(local.y < 1.8f);
    // Facing into the room, towards the door.
    const glm::vec3 facing = locker.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    CHECK(facing.x < -0.99f);
    // And nothing between the doorway and it: a look straight in from the corridor lands on its face.
    const glm::vec3 doorway = ShipSpec::kOrigin + glm::vec3(0.5f, local.y, local.z);
    const RayHit look = aboard.physics.RayCastStatic(doorway, {1.0f, 0.0f, 0.0f}, 20.0f);
    REQUIRE(look.hit);
    CHECK(std::abs(look.position.x - locker.position.x) < 0.05f);
}

TEST_CASE("The navigation console stands in the ops room, the screens beyond it, facing where people come in", "[ship]")
{
    Aboard aboard;
    const CinePose console = aboard.ship.BriefingConsole();
    const glm::vec3 local = console.position - ShipSpec::kOrigin;
    // Aft of the screens, so they are seen over it.
    float width = 0.0f;
    CHECK(aboard.ship.BriefingScreen(0, width).position.z < console.position.z);
    CHECK(width > 1.5f);
    CHECK(std::abs(local.x) < 1.0f);
    // Its front (-z of its own) looks into the room, and there is floor to stand on there with nothing in the way.
    const glm::vec3 front = console.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    CHECK(front.z > 0.99f);
    const glm::vec3 standing = console.position + front * 1.2f + glm::vec3(0.0f, 1.0f, 0.0f);
    const RayHit floor = aboard.physics.RayCastStatic(standing, {0.0f, -1.0f, 0.0f}, 2.0f);
    REQUIRE(floor.hit);
    CHECK(std::abs(floor.position.y - console.position.y) < 0.05f);
    const RayHit up = aboard.physics.RayCastStatic(standing, {0.0f, 1.0f, 0.0f}, 3.0f);
    CHECK((!up.hit || up.distance > 1.5f));
}

TEST_CASE("The shuttle stands on the closed bay doors, which swing down out of its way", "[ship]")
{
    Aboard aboard;
    // Standing in the hangar, on its floor.
    const glm::vec3 home = aboard.ship.Shuttle().Home().position;
    CHECK(aboard.ship.InHangar(home + glm::vec3(0.0f, 1.0f, 0.0f)));
    const RayHit under = aboard.physics.RayCastStatic(home + glm::vec3(1.3f, 0.2f, -2.0f), {0.0f, -1.0f, 0.0f}, 2.0f);
    REQUIRE(under.hit);
    CHECK(std::abs(under.position.y - home.y) < 0.05f);

    // Open, each leaf hangs down under the floor: its inner edge metres below where it lay.
    const ModelAsset doors = Vehicles::BayDoorsModel();
    const AnimationClip* opening = doors.FindClip("doors_opening");
    REQUIRE(opening != nullptr);
    for (const char* leaf : {"door_left", "door_right"})
    {
        INFO(leaf);
        const auto part = std::find_if(doors.parts.begin(), doors.parts.end(), [&](const ModelPart& p) { return p.name == leaf; });
        REQUIRE(part != doors.parts.end());
        const float inner = part->position.x < 0.0f ? part->size.x * 0.5f : -part->size.x * 0.5f;
        const glm::vec4 closed = doors.PartMatrixAt(*part, nullptr, 0.0f) * glm::vec4(inner, 0.0f, 0.0f, 1.0f);
        const glm::vec4 open = doors.PartMatrixAt(*part, opening, opening->duration) * glm::vec4(inner, 0.0f, 0.0f, 1.0f);
        CHECK(std::abs(closed.y + 0.15f) < 0.05f);
        CHECK(open.y < -3.5f);
        // And out of the middle, where the shuttle falls through.
        CHECK(std::abs(open.x) > 3.5f);
    }
}

TEST_CASE("No two parts of a vehicle share a surface facing the same way, which would flicker", "[ship][vehicle]")
{
    // Two faces in one plane, facing the same way and overlapping, are drawn in whichever order the depth test happens to
    // pick from pixel to pixel: the flickering seen on the back of the ship. Faces touching back to back are fine.
    ShipHullLook upgraded;
    upgraded.drive = 4;
    upgraded.sensors = 4;
    for (const ModelAsset& model : {ShipMap::HullModel(ShipHullLook{}), ShipMap::HullModel(upgraded), Vehicles::ShuttleModel(), Vehicles::BayDoorsModel()})
    {
        INFO(model.name);
        for (size_t i = 0; i < model.parts.size(); ++i)
        {
            const ModelPart& a = model.parts[i];
            if (a.shape != PartShape::Box || a.rotation != glm::vec3(0.0f) || !a.parent.empty())
            {
                continue;
            }
            for (size_t j = i + 1; j < model.parts.size(); ++j)
            {
                const ModelPart& b = model.parts[j];
                if (b.shape != PartShape::Box || b.rotation != glm::vec3(0.0f) || !b.parent.empty())
                {
                    continue;
                }
                for (int axis = 0; axis < 3; ++axis)
                {
                    const int u = (axis + 1) % 3;
                    const int v = (axis + 2) % 3;
                    const float overlapU = std::min(a.position[u] + a.size[u] * 0.5f, b.position[u] + b.size[u] * 0.5f) -
                                           std::max(a.position[u] - a.size[u] * 0.5f, b.position[u] - b.size[u] * 0.5f);
                    const float overlapV = std::min(a.position[v] + a.size[v] * 0.5f, b.position[v] + b.size[v] * 0.5f) -
                                           std::max(a.position[v] - a.size[v] * 0.5f, b.position[v] - b.size[v] * 0.5f);
                    if (overlapU < 0.01f || overlapV < 0.01f)
                    {
                        continue;
                    }
                    for (const float side : {-0.5f, 0.5f})
                    {
                        const float faceA = a.position[axis] + a.size[axis] * side;
                        const float faceB = b.position[axis] + b.size[axis] * side;
                        if (std::abs(faceA - faceB) > 0.001f)
                        {
                            continue;
                        }
                        // Buried: just outside the shared face is inside some other part, so neither is ever seen.
                        glm::vec3 outside;
                        outside[axis] = faceA + (side > 0.0f ? 0.002f : -0.002f);
                        outside[u] = (std::max(a.position[u] - a.size[u] * 0.5f, b.position[u] - b.size[u] * 0.5f) +
                                      std::min(a.position[u] + a.size[u] * 0.5f, b.position[u] + b.size[u] * 0.5f)) * 0.5f;
                        outside[v] = (std::max(a.position[v] - a.size[v] * 0.5f, b.position[v] - b.size[v] * 0.5f) +
                                      std::min(a.position[v] + a.size[v] * 0.5f, b.position[v] + b.size[v] * 0.5f)) * 0.5f;
                        const bool buried = std::any_of(model.parts.begin(), model.parts.end(), [&](const ModelPart& c)
                        {
                            return c.shape == PartShape::Box && c.rotation == glm::vec3(0.0f) && c.parent.empty() &&
                                   glm::all(glm::lessThan(glm::abs(outside - c.position), c.size * 0.5f));
                        });
                        INFO(a.name << " and " << b.name << " on axis " << axis);
                        CHECK(buried);
                    }
                }
            }
        }
    }
}

namespace
{

// Every pair of parts, across the models given, with a face within a few millimetres of one of the other's, facing the same way
// and overlapping, that nothing else covers: what flickers. Turned and non-box parts are left out (none of them lies on another).
std::vector<std::string> SharedFaces(const std::vector<const ModelAsset*>& models)
{
    std::vector<const ModelPart*> parts;
    for (const ModelAsset* model : models)
    {
        for (const ModelPart& part : model->parts)
        {
            if (part.shape == PartShape::Box && part.rotation == glm::vec3(0.0f) && part.parent.empty())
            {
                parts.push_back(&part);
            }
        }
    }
    std::vector<std::string> found;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const ModelPart& a = *parts[i];
        for (size_t j = i + 1; j < parts.size(); ++j)
        {
            const ModelPart& b = *parts[j];
            for (int axis = 0; axis < 3; ++axis)
            {
                const int u = (axis + 1) % 3;
                const int v = (axis + 2) % 3;
                const float lowU = std::max(a.position[u] - a.size[u] * 0.5f, b.position[u] - b.size[u] * 0.5f);
                const float highU = std::min(a.position[u] + a.size[u] * 0.5f, b.position[u] + b.size[u] * 0.5f);
                const float lowV = std::max(a.position[v] - a.size[v] * 0.5f, b.position[v] - b.size[v] * 0.5f);
                const float highV = std::min(a.position[v] + a.size[v] * 0.5f, b.position[v] + b.size[v] * 0.5f);
                if (highU - lowU < 0.01f || highV - lowV < 0.01f)
                {
                    continue;
                }
                for (const float side : {-0.5f, 0.5f})
                {
                    const float faceA = a.position[axis] + a.size[axis] * side;
                    const float faceB = b.position[axis] + b.size[axis] * side;
                    if (std::abs(faceA - faceB) > 0.005f)
                    {
                        continue;
                    }
                    // Buried: just outside the shared face is inside some other part, so neither is ever seen. Looked at in a
                    // few places over the overlap, not only its middle.
                    bool seen = false;
                    for (const glm::vec2 at : {glm::vec2{0.5f, 0.5f}, glm::vec2{0.1f, 0.1f}, glm::vec2{0.9f, 0.1f}, glm::vec2{0.1f, 0.9f}, glm::vec2{0.9f, 0.9f}})
                    {
                        glm::vec3 outside;
                        outside[axis] = std::max(faceA, faceB) * (side > 0.0f ? 1.0f : 0.0f) + std::min(faceA, faceB) * (side > 0.0f ? 0.0f : 1.0f) +
                                        (side > 0.0f ? 0.004f : -0.004f);
                        outside[u] = lowU + (highU - lowU) * at.x;
                        outside[v] = lowV + (highV - lowV) * at.y;
                        const bool buried = std::any_of(parts.begin(), parts.end(), [&](const ModelPart* c)
                                                        { return glm::all(glm::lessThanEqual(glm::abs(outside - c->position), c->size * 0.5f + glm::vec3(1.0e-4f))); });
                        seen = seen || !buried;
                    }
                    if (seen)
                    {
                        found.push_back(a.name + " and " + b.name + " on axis " + std::to_string(axis));
                    }
                }
            }
        }
    }
    return found;
}

} // namespace

TEST_CASE("Kestrel Station and the ship standing in it share no surface, which would flicker", "[ship][kestrel]")
{
    const ModelAsset hull = ShipMap::HullModel(ShipHullLook{});
    const ModelAsset solid = KestrelStation::Solid({0.4f, 0.4f, 0.4f}, {0.3f, 0.3f, 0.3f});
    const ModelAsset dressing = KestrelStation::Dressing({0.4f, 0.4f, 0.4f});
    const std::vector<std::string> found = SharedFaces({&hull, &solid, &dressing});
    for (const std::string& pair : found)
    {
        UNSCOPED_INFO(pair);
    }
    CHECK(found.empty());
}
