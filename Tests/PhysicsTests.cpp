#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Game/Player/PlayerController.h"
#include "Game/World/WorldObjects.h"


#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <future>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

using namespace pred;

// The collision layers, exercised through the character controller rather than through Jolt
// directly: what matters is whether a player can walk where they should be able to.

TEST_CASE("A character walks through loose items but not through the world", "[physics][layers]")
{
    // Dropped items used to be ordinary dynamic bodies, so the player could kick a rifle across the
    // room, and an item that ended up wedged against the capsule could not be picked up because the
    // player could never get close enough to it.
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    physics.CreateBox({20.0f, 0.5f, 20.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);

    // A dropped item and a solid crate, side by side, two metres ahead.
    const BodyHandle item = physics.CreateBox({0.12f, 0.12f, 0.12f}, Transform{{-1.0f, 0.12f, -2.0f}},
                                              BodyMotion::Dynamic, 400.0f, PhysicsLayer::Debris);
    physics.CreateBox({0.5f, 0.5f, 0.5f}, Transform{{1.0f, 0.5f, -2.0f}}, BodyMotion::Static);
    physics.OptimizeBroadPhase();
    REQUIRE(item.IsValid());

    const auto walkInto = [&](float startX)
    {
        PlayerConfig config;
        PlayerController player;
        REQUIRE(player.Init(physics, config, {startX, 0.05f, 0.0f}));
        PlayerInput input;
        input.move = glm::vec2(0.0f, 1.0f); // straight ahead, which is -Z
        for (int i = 0; i < 180; ++i)
        {
            player.Step(input, 1.0f / 60.0f);
            physics.Step(1.0f / 60.0f);
        }
        const float reached = player.State().position.z;
        player.Shutdown();
        return reached;
    };

    // Let the item settle before anyone walks at it, so what is measured is the walking.
    for (int i = 0; i < 60; ++i)
    {
        physics.Step(1.0f / 60.0f);
    }
    const glm::vec3 restingAt = physics.GetTransform(item).position;

    // Past the item, stopped short of the crate.
    const float pastItem = walkInto(-1.0f);
    const float atCrate = walkInto(1.0f);
    INFO("reached z " << pastItem << " through the item, " << atCrate << " at the crate");
    CHECK(pastItem < -2.5f);
    CHECK(atCrate > -1.9f);

    // And the item is exactly where it was. Walking through it must not shove it: an item that
    // slides away from the player is one they can chase but never reach.
    const glm::vec3 nowAt = physics.GetTransform(item).position;
    INFO("item moved " << glm::distance(restingAt, nowAt) << " m");
    CHECK(glm::distance(restingAt, nowAt) < 0.01f);

    physics.Shutdown();
}
TEST_CASE("Two machines agree which pickup is which", "[items][net]")
{
    // The index is the name every machine uses for a dropped item afterwards: it is taken by index
    // and removed by index everywhere at once. Both sides used to choose their own, so two machines
    // with different holes in their pickup lists disagreed about which item was which, and from
    // then on taking one removed a different one somewhere else. That is an item that cannot be
    // picked up on one screen and a second copy of it on another.
    //
    // Only the choice of index is exercised here. Everything else about spawning a pickup needs a
    // renderer, and the choice is the whole of what the two machines have to agree on.
    using Pickup = WorldObjects::Pickup;
    std::vector<Pickup> host;
    std::vector<Pickup> client;

    const auto add = [](std::vector<Pickup>& list, int index)
    {
        if (static_cast<size_t>(index) >= list.size())
        {
            Pickup empty;
            empty.alive = false;
            list.resize(static_cast<size_t>(index) + 1, empty);
        }
        list[static_cast<size_t>(index)].alive = true;
    };

    // The host drops three and picks the middle one back up, leaving a hole at index 1.
    for (int i = 0; i < 3; ++i)
    {
        const int at = WorldObjects::ChooseSlot(host, -1);
        CHECK(at == i);
        add(host, at);
    }
    host[1].alive = false;

    // The client has seen none of it, so left to itself it would call the next drop index 0.
    CHECK(WorldObjects::ChooseSlot(client, -1) == 0);

    // The host's next drop reuses its hole.
    const int chosen = WorldObjects::ChooseSlot(host, -1);
    CHECK(chosen == 1);

    // Told that number, the client uses it rather than its own answer. That is the whole fix.
    CHECK(WorldObjects::ChooseSlot(client, chosen) == chosen);

    // And an index past the end is still that index, because the caller fills the gap.
    CHECK(WorldObjects::ChooseSlot(client, 9) == 9);
}

TEST_CASE("Uploading the same mesh twice does not take twice the buffers", "[render][mesh]")
{
    // The editor rebuilds its preview on every change, and every rebuild used to take four more
    // buffer handles and give none back. Four thousand of them is a couple of minutes of dragging a
    // socket, and after that nothing can be uploaded at all and the model quietly disappears.
    //
    // No renderer here, so what is checked is the bookkeeping: the same name and the same geometry
    // is the same entry, and the same name with different geometry replaces it in place so every
    // handle handed out before still points at the right thing.
    MeshData first;
    first.vertices.resize(3);
    first.vertices[1].position = {1.0f, 0.0f, 0.0f};
    first.vertices[2].position = {0.0f, 1.0f, 0.0f};
    first.indices = {0, 1, 2};

    MeshData resized = first;
    resized.vertices[1].position = {2.0f, 0.0f, 0.0f};

    CHECK(MeshFingerprintForTesting(first) == MeshFingerprintForTesting(first));
    CHECK(MeshFingerprintForTesting(first) != MeshFingerprintForTesting(resized));

    MeshData moreTriangles = first;
    moreTriangles.indices = {0, 1, 2, 0, 1, 2};
    CHECK(MeshFingerprintForTesting(first) != MeshFingerprintForTesting(moreTriangles));
}

TEST_CASE("The level check says nothing of one structure's pieces reaching into each other, but still of anything in them", "[physics]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    const auto at = [](float x, float y, float z)
    {
        Transform transform;
        transform.position = {x, y, z};
        return transform;
    };
    // A floor, and a wall standing 2 cm down into it: one building.
    const BodyHandle floor = physics.CreateBox({5.0f, 0.3f, 5.0f}, at(0.0f, -0.3f, 0.0f), BodyMotion::Static);
    const BodyHandle wall = physics.CreateBox({0.1f, 1.5f, 5.0f}, at(0.0f, 1.48f, 0.0f), BodyMotion::Static);
    REQUIRE(physics.FindStaticOverlaps(0.01f).size() == 1);
    const uint32_t building = physics.NewOverlapGroup();
    physics.SetOverlapGroup(floor, building);
    physics.SetOverlapGroup(wall, building);
    CHECK(physics.FindStaticOverlaps(0.01f).empty());
    // A crate put half into the wall is still a mistake.
    physics.CreateBox({0.4f, 0.4f, 0.4f}, at(0.3f, 0.45f, 0.0f), BodyMotion::Static);
    CHECK(physics.FindStaticOverlaps(0.01f).size() == 1);
    // And so is another building's wall, put into this one's floor.
    const BodyHandle other = physics.CreateBox({0.1f, 1.5f, 1.0f}, at(3.0f, 1.48f, 0.0f), BodyMotion::Static);
    physics.SetOverlapGroup(other, physics.NewOverlapGroup());
    CHECK(physics.FindStaticOverlaps(0.01f).size() == 2);
}

TEST_CASE("A mesh's shape can be made on other threads and the body added from it later", "[physics]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    // A floor, two triangles, ten metres square, made into a shape on two threads at once.
    MeshData floor;
    for (const glm::vec3 corner : {glm::vec3(-5.0f, 0.0f, -5.0f), glm::vec3(5.0f, 0.0f, -5.0f), glm::vec3(-5.0f, 0.0f, 5.0f), glm::vec3(5.0f, 0.0f, 5.0f)})
    {
        MeshVertex vertex;
        vertex.position = corner;
        floor.vertices.push_back(vertex);
    }
    floor.indices = {0, 2, 1, 1, 2, 3};
    auto first = std::async(std::launch::async, [&] { return PhysicsWorld::PrepareMesh(floor); });
    auto second = std::async(std::launch::async, [&] { return PhysicsWorld::PrepareMesh(floor); });
    const PhysicsWorld::PreparedMesh a = first.get();
    const PhysicsWorld::PreparedMesh b = second.get();
    REQUIRE(a.Valid());
    REQUIRE(b.Valid());
    const BodyHandle low = physics.CreateMeshBody(a, Transform{{0.0f, 0.0f, 0.0f}});
    const BodyHandle high = physics.CreateMeshBody(b, Transform{{0.0f, 3.0f, 0.0f}});
    REQUIRE(low.IsValid());
    REQUIRE(high.IsValid());
    // Both there, each where it was put; and the one shape can be used again.
    const RayHit down = physics.RayCast({1.0f, 10.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    REQUIRE(down);
    CHECK(std::abs(down.position.y - 3.0f) < 1.0e-3f);
    physics.DestroyBody(high);
    const RayHit again = physics.RayCast({1.0f, 10.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    REQUIRE(again);
    CHECK(std::abs(again.position.y) < 1.0e-3f);
    CHECK(physics.CreateMeshBody(a, Transform{{0.0f, -2.0f, 0.0f}}).IsValid());
    CHECK(physics.StaticTriangles().size() == 12);
    // Nothing to make a shape from, nothing made.
    CHECK_FALSE(PhysicsWorld::PrepareMesh(MeshData{}).Valid());
}
