#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/TestMap.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace pred;
using namespace pred::TestMapSpec;

// Navigation on the real test map, built exactly as the game builds it -- the same level, the same
// physics bodies, the navmesh made from those bodies -- with nothing drawn.
namespace
{

struct NavHarness
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;
    NavMesh nav;

    NavHarness()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        BuildTestMap(scene, meshes, &physics);
        std::string error;
        const bool built = nav.Build(physics.StaticTriangles(), NavSettings{}, &error);
        INFO("navmesh: " << error);
        REQUIRE(built);
    }
};

// Where a route crosses the plane x = `x`, if it does.
bool CrossesAt(const std::vector<glm::vec3>& route, float x, glm::vec3& at)
{
    for (size_t i = 1; i < route.size(); ++i)
    {
        const glm::vec3& a = route[i - 1];
        const glm::vec3& b = route[i];
        if ((a.x - x) * (b.x - x) <= 0.0f && std::abs(b.x - a.x) > 1e-5f)
        {
            const float t = (x - a.x) / (b.x - a.x);
            at = a + (b - a) * t;
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("The level becomes a navigation mesh", "[navigation]")
{
    NavHarness world;
    CHECK(world.nav.Valid());
    // The test map is a plaza, a corridor, stairs, ramps and a room: a few hundred polygons at most,
    // and certainly more than a handful. A mesh of three polygons would be one that lost most of the
    // level on the way.
    CHECK(world.nav.PolygonCount() > 50);

    // The spawn is standing room.
    glm::vec3 spawn;
    CHECK(world.nav.NearestPoint({0.0f, 0.0f, 0.0f}, 0.5f, spawn));
    CHECK(std::abs(spawn.y) < 0.2f);
}

TEST_CASE("A route into the dark room goes through its door", "[navigation]")
{
    // The room's only way in is a doorway in its east wall. A route that reaches the middle of the
    // room from the spawn has to cross that wall inside the doorway, or it went through a wall.
    NavHarness world;
    const glm::vec3 inside{kDarkRoomX, 0.06f, kDarkRoomZ};
    std::vector<glm::vec3> route;
    bool reached = false;
    REQUIRE(world.nav.FindPath({0.0f, 0.0f, 0.0f}, inside, route, &reached));
    CHECK(reached);
    REQUIRE(route.size() >= 2);
    CHECK(glm::distance(route.back(), inside) < 0.5f);

    const float eastWall = kDarkRoomX + kDarkRoomWidth * 0.5f;
    glm::vec3 crossing;
    REQUIRE(CrossesAt(route, eastWall, crossing));
    INFO("crossed the east wall at z " << crossing.z);
    CHECK(std::abs(crossing.z - kDarkRoomZ) < kDarkRoomDoorWidth * 0.5f);

    // And it is a real route rather than a line with corners: every leg of it is clear at knee
    // height, which is where a wall the mesh had missed would be.
    for (size_t i = 1; i < route.size(); ++i)
    {
        const glm::vec3 a = route[i - 1] + glm::vec3(0.0f, 0.4f, 0.0f);
        const glm::vec3 b = route[i] + glm::vec3(0.0f, 0.4f, 0.0f);
        const float length = glm::distance(a, b);
        if (length < 1e-3f)
        {
            continue;
        }
        const RayHit hit = world.physics.RayCast(a, (b - a) / length, length);
        INFO("leg " << i << " from " << a.x << "," << a.z << " to " << b.x << "," << b.z);
        CHECK_FALSE(hit);
    }
}

TEST_CASE("A route climbs the stairs to the landing", "[navigation]")
{
    // The first staircase: shallow enough steps to walk, with a landing 2.4 m up. Being able to go
    // up is what separates a navigation mesh from a map of the floor.
    NavHarness world;
    std::vector<glm::vec3> route;
    bool reached = false;
    // On top of the first landing: twenty steps of 12 cm, each 30 cm deep, starting at z -4, and a
    // 3 m landing after them -- so its middle is at z 3.5, 2.4 m up. Worked out from the level's own
    // numbers rather than guessed, because a guess landed on the stairs.
    const int steps = static_cast<int>(std::round(2.4f / kStepRises[0]));
    const float landingZ = -4.0f + 0.30f * static_cast<float>(steps) + 1.5f;
    glm::vec3 landingTop;
    REQUIRE(world.nav.NearestPoint({8.0f, 2.4f, landingZ}, 0.5f, landingTop));
    INFO("landing top found at " << landingTop.x << "," << landingTop.y << "," << landingTop.z);
    CHECK(landingTop.y > 2.0f);
    REQUIRE(world.nav.FindPath({0.0f, 0.0f, 0.0f}, landingTop, route, &reached));
    CHECK(reached);
    CHECK(route.back().y > 2.0f);
}

TEST_CASE("Walking in a straight line stops at a wall", "[navigation]")
{
    NavHarness world;
    // Where the route from the spawn is a single straight leg, the straight walk agrees that it is
    // clear. Found rather than assumed: the plaza has things standing in it, and a test that picks
    // a direction by eye is a test of the eye.
    bool foundOpen = false;
    for (int i = 0; i < 16 && !foundOpen; ++i)
    {
        const float angle = glm::two_pi<float>() * static_cast<float>(i) / 16.0f;
        const glm::vec3 end{std::cos(angle) * 3.0f, 0.0f, std::sin(angle) * 3.0f};
        std::vector<glm::vec3> route;
        bool reached = false;
        if (world.nav.FindPath({0.0f, 0.0f, 0.0f}, end, route, &reached) && reached &&
            route.size() == 2)
        {
            foundOpen = true;
            INFO("open towards " << end.x << "," << end.z);
            CHECK(world.nav.StraightWalk({0.0f, 0.0f, 0.0f}, end));
        }
    }
    CHECK(foundOpen);
    // From inside the dark room straight out through its back wall: not fine.
    const glm::vec3 inside{kDarkRoomX, 0.06f, kDarkRoomZ};
    const glm::vec3 outsideBehind{kDarkRoomX - kDarkRoomWidth, 0.0f, kDarkRoomZ};
    CHECK_FALSE(world.nav.StraightWalk(inside, outsideBehind));
}

TEST_CASE("Wandering is reproducible from a seed", "[navigation]")
{
    // The creature wanders, and a creature has to be reproducible from its seed: the same seed has
    // to send it to the same places.
    NavHarness world;
    uint32_t first = 1234;
    uint32_t second = 1234;
    glm::vec3 a;
    glm::vec3 b;
    REQUIRE(world.nav.RandomPointNear({0.0f, 0.0f, 0.0f}, 12.0f, first, a));
    REQUIRE(world.nav.RandomPointNear({0.0f, 0.0f, 0.0f}, 12.0f, second, b));
    CHECK(glm::distance(a, b) < 1e-4f);
    // And the seed moves on, so the next wander is somewhere else.
    glm::vec3 c;
    REQUIRE(world.nav.RandomPointNear({0.0f, 0.0f, 0.0f}, 12.0f, first, c));
    CHECK(glm::distance(a, c) > 1e-3f);
    // Every point it picks is somewhere it can actually get to.
    std::vector<glm::vec3> route;
    bool reached = false;
    CHECK(world.nav.FindPath({0.0f, 0.0f, 0.0f}, c, route, &reached));
    CHECK(reached);
}
