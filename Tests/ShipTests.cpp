#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/World/ShipMap.h"
#include "Game/World/Vehicles.h"
#include "Game/World/WorldObjects.h"

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

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
        ship.LayOutKit(items);
    }
};

} // namespace

TEST_CASE("The ship, and the lockers, crates and kit put aboard it, are inside nothing", "[ship]")
{
    Aboard aboard;
    REQUIRE(aboard.ship.Built());
    aboard.world.AddFacility(aboard.scene, aboard.meshes, aboard.physics, aboard.interactions, aboard.items, aboard.ship.Placements());
    const std::vector<PhysicsWorld::StaticOverlap> overlaps = aboard.physics.FindStaticOverlaps(0.01f);
    CHECK(overlaps.empty());
    CHECK(aboard.ship.Placements().lockers.size() == 3);
    CHECK(aboard.ship.Placements().ammoCrates.size() == 2);
    CHECK_FALSE(aboard.ship.Placements().items.empty());
}

TEST_CASE("Everybody arrives aboard standing on the briefing room's floor, and the kit is on the bench", "[ship]")
{
    Aboard aboard;
    for (uint8_t player = 0; player < 8; ++player)
    {
        INFO("player " << static_cast<int>(player));
        const glm::vec3 spawn = aboard.ship.Spawn(player);
        const RayHit floor = aboard.physics.RayCastStatic(spawn + glm::vec3(0.0f, 1.0f, 0.0f), {0.0f, -1.0f, 0.0f}, 3.0f);
        REQUIRE(floor.hit);
        CHECK(std::abs(floor.position.y - (ShipSpec::kOrigin.y + ShipSpec::kUpperDeck)) < 0.05f);
        // Nothing overhead but the ceiling, well above a head.
        const RayHit above = aboard.physics.RayCastStatic(spawn + glm::vec3(0.0f, 0.2f, 0.0f), {0.0f, 1.0f, 0.0f}, 10.0f);
        REQUIRE(above.hit);
        CHECK(above.distance > 2.5f);
        CHECK(aboard.ship.Contains(spawn));
    }
    for (const WorldObjects::PlacedItem& item : aboard.ship.Placements().items)
    {
        INFO(item.key);
        const RayHit bench = aboard.physics.RayCastStatic(item.position + glm::vec3(0.0f, 0.3f, 0.0f), {0.0f, -1.0f, 0.0f}, 1.0f);
        REQUIRE(bench.hit);
        CHECK(std::abs(bench.position.y - item.position.y) < 0.02f);
    }
    // And the ship is well away from everywhere else.
    CHECK_FALSE(aboard.ship.Contains({0.0f, 0.0f, 0.0f}));
    CHECK_FALSE(aboard.ship.Contains({250.0f, 0.0f, 40.0f}));
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
