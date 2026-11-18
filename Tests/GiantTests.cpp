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

TEST_CASE("A strider picks somebody up with its strands and lets go only for a reason -- or, with none, stamps on them", "[giant]")
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
        bool stamped = false;
        bool released = false;
        float grabbedAt = -1.0f;
        float releasedAt = -1.0f;
        std::string why;
        for (int frame = 0; frame < 30 * 60 && !released && !stamped; ++frame)
        {
            giant.Think(lab.world, {person}, time, dt);
            giant.Animate(lab.world, time, dt);
            for (const Giant::Blow& blow : giant.TakeBlows())
            {
                if (!blow.grab && blow.damage > 0.0f)
                {
                    stamped = true;
                }
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
        // Taken only by its strands (you, 2026-11-16): one with none brings its foot down on them, and never has hold of anybody.
        if (!giant.HasStrands())
        {
            CHECK(stamped);
            CHECK(!grabbed);
            continue;
        }
        REQUIRE(grabbed);
        // Looked at for a while first, never dropped for nothing in the meantime -- then thrown (or it is still eating them).
        if (released)
        {
            CHECK(releasedAt - grabbedAt > 5.0f);
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

TEST_CASE("A leviathan is a serpent or of a serpent's kind, opens its jaws to bite somebody swimming in passing, and never takes them", "[giant]")
{
    // (You, 2026-11-16: serpents only, and they do not grab.)
    GiantLab lab(true);
    int bitten = 0;
    for (const uint32_t seed : {5u, 55u, 555u, 5555u, 7u, 77u})
    {
        INFO("seed " << seed);
        Giant giant(lab.scene, lab.meshes, lab.physics, GiantKind::Swimmer, seed, {0.0f, -20.0f, 0.0f}, 0.0f);
        const std::string what = giant.Describe();
        CHECK((what.find("serpent") != std::string::npos || what.find("eel") != std::string::npos || what.find("sea reptile") != std::string::npos ||
               what.find("sea dragon") != std::string::npos));
        GiantSensed person;
        person.id = 0;
        person.feet = {0.0f, -6.0f, -25.0f};
        person.inWater = true;
        giant.TakeDamage(1.0f, person.id, 0.0f);
        float time = 0.0f;
        const float dt = 1.0f / 30.0f;
        bool hurt = false;
        bool gaped = false;
        // (Two minutes: it goes off out of sight and round before it comes.)
        for (int frame = 0; frame < 30 * 120 && !hurt; ++frame)
        {
            giant.Think(lab.world, {person}, time, dt);
            giant.Animate(lab.world, time, dt);
            for (const Giant::Blow& blow : giant.TakeBlows())
            {
                CHECK_FALSE(blow.grab);
                hurt = hurt || blow.damage > 0.0f;
            }
            CHECK(giant.Holding() < 0);
            gaped = gaped || giant.Gape() > 0.6f;
            time += dt;
        }
        INFO(giant.Describe() << "; " << Giant::ModeName(giant.CurrentMode()) << " -- " << giant.Goal());
        bitten += hurt ? 1 : 0;
        // (Its jaws open wide as it comes at them: you, 2026-11-16.)
        CHECK((!hurt || gaped));
        CHECK_FALSE(giant.Dead());
    }
    CHECK(bitten >= 2);
}

TEST_CASE("A leviathan's body moves smoothly as it swims: nothing of it jerks from one frame to the next", "[giant][leviathan]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    GiantWorld world;
    world.ground = [](float, float) { return -60.0f; };
    world.water = [](const glm::vec3& at, float& surface)
    {
        surface = 0.0f;
        return std::max(-at.y, 0.0f);
    };
    world.roofed = [](const glm::vec3&) { return false; };
    world.clear = [](const glm::vec3&, const glm::vec3&) { return true; };
    world.middle = {0.0f, 0.0f};
    world.reach = 300.0f;
    for (const uint32_t seed : {1u, 2u, 6u})
    {
        Giant giant(scene, meshes, physics, GiantKind::Swimmer, seed, {0.0f, -30.0f, 0.0f}, 0.0f);
        const float dt = 1.0f / 60.0f;
        float time = 0.0f;
        std::vector<glm::vec3> before;
        std::vector<glm::vec3> last;
        float worst = 0.0f;
        int worstAt = -1;
        float worstTime = 0.0f;
        std::string worstLine;
        glm::vec3 p0{0.0f}, p1{0.0f};
        float headWorst = 0.0f;
        float headWorstAt = 0.0f;
        for (int frame = 0; frame < 60 * 30; ++frame)
        {
            giant.Think(world, {}, time, dt);
            giant.Animate(world, time, dt);
            time += dt;
            {
                const glm::vec3 p = giant.Position();
                const float j = glm::length(p - 2.0f * p1 + p0) / (dt * dt);
                if (time > 3.0f && j > headWorst)
                {
                    headWorst = j;
                    headWorstAt = time;
                }
                p0 = p1;
                p1 = p;
            }
            const std::vector<glm::vec3>& spine = giant.Spine();
            if (time > 1.0f && before.size() == spine.size() && last.size() == spine.size())
            {
                for (size_t k = 0; k < spine.size(); ++k)
                {
                    const float jerk = glm::length(spine[k] - 2.0f * last[k] + before[k]) / (dt * dt);
                    if (jerk > worst)
                    {
                        worst = jerk;
                        worstAt = static_cast<int>(k);
                        worstTime = time;
                        char line[400];
                        std::snprintf(line, sizeof(line), "b %.3f %.3f %.3f l %.3f %.3f %.3f n %.3f %.3f %.3f; k-1 %.3f %.3f %.3f -> %.3f %.3f %.3f", before[k].x, before[k].y, before[k].z, last[k].x, last[k].y, last[k].z,
                                      spine[k].x, spine[k].y, spine[k].z, last[k > 0 ? k - 1 : 0].x, last[k > 0 ? k - 1 : 0].y, last[k > 0 ? k - 1 : 0].z, spine[k > 0 ? k - 1 : 0].x, spine[k > 0 ? k - 1 : 0].y, spine[k > 0 ? k - 1 : 0].z);
                        worstLine = line;
                    }
                }
            }
            before = last;
            last = spine;
        }
        INFO("seed " << seed << ": " << giant.Describe() << "; worst " << worst << " m/s2 at joint " << worstAt << " of " << last.size() << " at " << worstTime << " s; its front " << headWorst << " at " << headWorstAt << "; " << worstLine);
        // (Anything over a few g, frame to frame, is a twitch the eye sees.)
        CHECK(worst < 40.0f);
    }
}

TEST_CASE("A strider's strands move smoothly, reaching, striking and hanging: none of them flings about from one frame to the next", "[giant][strands]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    GiantWorld world;
    world.ground = [](float, float) { return 0.0f; };
    world.water = [](const glm::vec3&, float& surface)
    {
        surface = -100.0f;
        return 0.0f;
    };
    world.roofed = [](const glm::vec3&) { return false; };
    world.clear = [](const glm::vec3&, const glm::vec3&) { return true; };
    world.middle = {0.0f, 0.0f};
    world.reach = 300.0f;
    for (const uint32_t seed : {7u, 8u, 14u})
    {
        Giant giant(scene, meshes, physics, GiantKind::Walker, seed, {0.0f, 0.0f, 0.0f}, 0.0f);
        if (!giant.HasStrands())
        {
            continue;
        }
        GiantSensed person;
        person.id = 0;
        person.feet = {0.0f, 0.0f, -40.0f};
        person.light = 1.0f;
        const float dt = 1.0f / 60.0f;
        float time = 0.0f;
        std::vector<glm::vec3> before;
        std::vector<glm::vec3> last;
        float worst = 0.0f;
        float worstAt = 0.0f;
        std::string worstMode;
        giant.Force(Giant::Mode::Hunt, 0, person.feet, time);
        for (int frame = 0; frame < 60 * 25; ++frame)
        {
            // (Somebody walking slowly round it.)
            person.feet = glm::vec3(std::sin(time * 0.1f) * 40.0f, 0.0f, -std::cos(time * 0.1f) * 40.0f);
            giant.Think(world, {person}, time, dt);
            (void)giant.TakeBlows();
            giant.Animate(world, time, dt);
            time += dt;
            std::vector<glm::vec3> points = giant.StrandPoints();
            points.push_back(giant.Position());
            if (time > 2.0f && before.size() == points.size() && last.size() == points.size())
            {
                for (size_t k = 0; k < points.size(); ++k)
                {
                    const float jerk = glm::length(points[k] - 2.0f * last[k] + before[k]) / (dt * dt);
                    if (jerk > worst)
                    {
                        worst = jerk;
                        worstAt = time;
                        worstMode = giant.Goal() + " link " + std::to_string(k) + " moved " + std::to_string(glm::length(points[k] - last[k])) + " then " + std::to_string(glm::length(last[k] - before[k]));
                    }
                }
            }
            before = last;
            last = points;
        }
        INFO("seed " << seed << ": " << giant.Describe() << "; worst " << worst << " m/s2 at " << worstAt << " s, " << worstMode);
        // (Nothing shakes; a strike, though, whips out from rest at once.)
        CHECK(worst < (worstMode.find("reaching down") != std::string::npos ? 1500.0f : 400.0f));
    }
}

TEST_CASE("A strider waiting where somebody went in never stamps at the building, and never reaches in through a shut door", "[giant]")
{
    // You, 2026-11-17: a giant grabbed me through the airlock; and its stamping by where I went in made no sense, its leg through the
    // building.
    GiantLab lab(false);
    // A building, its doorway in the middle of its south wall; whatever is between the doorway and somebody in it, shut (the door).
    lab.world.roofed = [](const glm::vec3& at) { return at.x > -10.0f && at.x < 10.0f && at.z > -40.0f && at.z < -20.0f; };
    lab.world.builtUp = [](float x, float z) { return x > -16.0f && x < 16.0f && z > -46.0f && z < -14.0f ? 8.0f : 0.0f; };
    lab.world.roof = [](float x, float z) { return x > -10.0f && x < 10.0f && z > -40.0f && z < -20.0f ? 8.0f : -1.0e9f; };
    lab.world.clear = [](const glm::vec3& from, const glm::vec3& to) { return (from.z > -20.0f) == (to.z > -20.0f); };
    int strandless = 0;
    for (const uint32_t seed : {11u, 222u, 3333u, 44u, 505u, 6006u})
    {
        INFO("seed " << seed);
        Giant giant(lab.scene, lab.meshes, lab.physics, GiantKind::Walker, seed, {0.0f, 0.0f, 30.0f}, 0.0f);
        strandless += giant.HasStrands() ? 0 : 1;
        GiantSensed person;
        person.id = 0;
        person.feet = {0.0f, 0.0f, -12.0f};
        giant.TakeDamage(1.0f, person.id, 0.0f);
        float time = 0.0f;
        const float dt = 1.0f / 30.0f;
        bool harmed = false;
        bool grabbed = false;
        bool waited = false;
        for (int frame = 0; frame < 30 * 70; ++frame)
        {
            // In, three metres past the doorway, a moment after it knows of them.
            if (time > 1.0f)
            {
                person.feet = {0.0f, 0.0f, -23.0f};
                person.roofed = true;
            }
            giant.Think(lab.world, {person}, time, dt);
            giant.Animate(lab.world, time, dt);
            waited = waited || giant.CurrentMode() == Giant::Mode::Linger;
            for (const Giant::Blow& blow : giant.TakeBlows())
            {
                grabbed = grabbed || blow.grab;
                harmed = harmed || (!blow.grab && blow.damage > 0.0f && person.roofed);
            }
            time += dt;
        }
        INFO(giant.Goal() << "; " << giant.Describe());
        CHECK(waited);
        CHECK_FALSE(grabbed);
        CHECK_FALSE(harmed);
    }
    CHECK(strandless > 0);
}
