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

namespace
{

struct GiantLab
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;
    GiantWorld world;
    GiantLab(bool sea)
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        world.ground = [sea](float, float) { return sea ? -60.0f : 0.0f; };
        world.water = [sea](const glm::vec3& at, float& surface)
        {
            surface = 0.0f;
            return sea ? std::max(-at.y, 0.0f) : 0.0f;
        };
        world.roofed = [](const glm::vec3&) { return false; };
        world.clear = [](const glm::vec3&, const glm::vec3&) { return true; };
        world.middle = {0.0f, -60.0f};
        world.reach = 400.0f;
    }
};

} // namespace

TEST_CASE("A strider picks somebody up and lets go only for a reason: eaten, thrown, shot off, or killed", "[giant]")
{
    GiantLab lab(false);
    for (const uint32_t seed : {11u, 222u, 3333u})
    {
        INFO("seed " << seed);
        Giant giant(lab.scene, lab.meshes, lab.physics, GiantKind::Walker, seed, {0.0f, 0.0f, 0.0f}, 0.0f);
        GiantSensed person;
        person.id = 0;
        person.feet = {0.0f, 0.0f, -18.0f};
        giant.TakeDamage(1.0f, person.id, 0.0f);
        float time = 0.0f;
        const float dt = 1.0f / 30.0f;
        bool grabbed = false;
        bool released = false;
        float grabbedAt = -1.0f;
        float releasedAt = -1.0f;
        std::string why;
        for (int frame = 0; frame < 30 * 60 && !released; ++frame)
        {
            giant.Think(lab.world, {person}, time, dt);
            giant.Animate(lab.world, time, dt);
            for (const Giant::Blow& blow : giant.TakeBlows())
            {
                if (blow.grab && !grabbed)
                {
                    grabbed = true;
                    grabbedAt = time;
                }
                if (blow.release)
                {
                    released = true;
                    releasedAt = time;
                    why = giant.Goal();
                }
            }
            // Held: where it holds them.
            if (giant.Holding() == person.id)
            {
                person.feet = giant.HoldPoint() - glm::vec3(0.0f, 1.0f, 0.0f);
            }
            time += dt;
        }
        INFO("its goal: " << giant.Goal() << "; " << giant.Describe());
        REQUIRE(grabbed);
        // Looked at for a while first, never dropped for nothing in the meantime -- then thrown (or it is still eating them).
        if (released)
        {
            CHECK(releasedAt - grabbedAt > 5.5f);
            CHECK((why.find("threw") != std::string::npos || why.find("finished") != std::string::npos));
        }
        else
        {
            CHECK(giant.Holding() == person.id);
        }
        // Killed while it holds somebody: it lets go, and it is dead.
        if (giant.Holding() >= 0)
        {
            giant.TakeDamage(giant.MaxHealth() * 2.0f, person.id, time);
            bool letGo = false;
            for (const Giant::Blow& blow : giant.TakeBlows())
            {
                letGo = letGo || blow.release;
            }
            CHECK(letGo);
        }
        else
        {
            giant.TakeDamage(giant.MaxHealth() * 2.0f, person.id, time);
        }
        CHECK(giant.Dead());
        CHECK(giant.Holding() < 0);
    }
}

TEST_CASE("A leviathan takes somebody swimming in its jaws, drags them down, and lets go of them far under", "[giant]")
{
    GiantLab lab(true);
    int dragged = 0;
    for (const uint32_t seed : {5u, 55u, 555u, 5555u, 7u, 77u})
    {
        INFO("seed " << seed);
        Giant giant(lab.scene, lab.meshes, lab.physics, GiantKind::Swimmer, seed, {0.0f, -20.0f, 0.0f}, 0.0f);
        GiantSensed person;
        person.id = 0;
        person.feet = {0.0f, -6.0f, -25.0f};
        person.inWater = true;
        giant.TakeDamage(1.0f, person.id, 0.0f);
        float time = 0.0f;
        const float dt = 1.0f / 30.0f;
        float deepest = person.feet.y;
        bool released = false;
        for (int frame = 0; frame < 30 * 60 && !released; ++frame)
        {
            giant.Think(lab.world, {person}, time, dt);
            giant.Animate(lab.world, time, dt);
            for (const Giant::Blow& blow : giant.TakeBlows())
            {
                released = released || blow.release;
            }
            if (giant.Holding() == person.id)
            {
                person.feet = giant.HoldPoint() - glm::vec3(0.0f, 0.9f, 0.0f);
                deepest = std::min(deepest, person.feet.y);
            }
            time += dt;
        }
        INFO("seed " << seed << ": " << giant.Describe() << "; " << Giant::ModeName(giant.CurrentMode()) << " -- " << giant.Goal() << "; held? " << giant.Holding()
                               << "; it at " << giant.Position().y << ", them at " << person.feet.y);
        if (released)
        {
            ++dragged;
            CHECK(deepest < -10.0f);
        }
        CHECK_FALSE(giant.Dead());
    }
    CHECK(dragged >= 2);
}
