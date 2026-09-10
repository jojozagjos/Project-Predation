#include "Engine/Physics/PhysicsWorld.h"
#include "Game/Player/PlayerController.h"

#include <catch2/catch_test_macros.hpp>

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
