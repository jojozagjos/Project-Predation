#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/Giant.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

using namespace pred;

// The giants on their own, in a world of nothing but functions: the sea's floor, the sea, and what is built in it.
TEST_CASE("A leviathan swims round the floating pad between it and its prey, never through the pad or its moorings", "[giant]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);

    // Sixty metres of sea; a pad twenty metres across moored on the line from it to somebody swimming on the far side.
    const glm::vec2 pad{0.0f, -120.0f};
    const float padRadius = 20.0f;
    GiantWorld world;
    world.ground = [](float, float) { return -60.0f; };
    world.water = [](const glm::vec3& at, float& surface)
    {
        surface = 0.0f;
        return std::max(-at.y, 0.0f);
    };
    world.roofed = [](const glm::vec3&) { return false; };
    world.clear = [](const glm::vec3&, const glm::vec3&) { return true; };
    world.builtUp = [&](float x, float z) { return glm::distance(glm::vec2(x, z), pad) < padRadius ? 62.0f : 0.0f; };
    world.middle = {0.0f, -120.0f};
    world.reach = 400.0f;

    Giant giant(scene, meshes, physics, GiantKind::Swimmer, 0x9E3779B9u, {0.0f, -30.0f, 0.0f}, 0.0f);
    GiantSensed swimmer;
    swimmer.id = 0;
    swimmer.feet = {0.0f, -1.5f, -240.0f};
    swimmer.inWater = true;
    swimmer.light = 1.0f;

    // (Shot by them: it knows where they are, whatever it can see that far through the water -- eighty metres.)
    giant.TakeDamage(1.0f, swimmer.id, 0.0f);
    float nearest = 1.0e9f;
    float closestToPrey = 1.0e9f;
    float time = 0.0f;
    const float dt = 1.0f / 30.0f;
    for (int frame = 0; frame < 30 * 90; ++frame)
    {
        giant.Think(world, {swimmer}, time, dt);
        (void)giant.TakeBlows();
        time += dt;
        const glm::vec3 at = giant.Position();
        nearest = std::min(nearest, glm::distance(glm::vec2(at.x, at.z), pad));
        closestToPrey = std::min(closestToPrey, glm::distance(glm::vec2(at.x, at.z), glm::vec2(swimmer.feet.x, swimmer.feet.z)));
    }
    // It went for them -- past the pad -- and never into it.
    CHECK(closestToPrey < 80.0f);
    CHECK(nearest > padRadius);
}
