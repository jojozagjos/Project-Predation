#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/Weapons/BulletHole.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

using namespace pred;

// Where a bullet hole's vertices end up relative to the thing it landed on.
//
// Reported as "the bullet holes aren't wrapping the object -- on something round it stays flat".
// Every hole was one flat disc on the tangent plane at the point of impact, which is exact on a wall
// and a plate stuck to anything curved; on the level's pillars and sphere, which are triangle meshes,
// it was also a plate lying on the single facet the round struck. These measure the gap between the
// mark and the surface under each of its vertices, which is the thing that was wrong.
namespace
{

struct World
{
    PhysicsWorld physics;
    World()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
    }
};

// How far the flat disc's outer ring would stand off a curve, to compare against.
float FlatGap(float curveRadius, float footprint)
{
    return curveRadius - std::sqrt(curveRadius * curveRadius - footprint * footprint);
}

// The largest footprint radius in the shape, which is the lip.
float LipRadius(const MeshData& shape)
{
    float widest = 0.0f;
    for (const MeshVertex& vertex : shape.vertices)
    {
        widest = std::max(widest, std::hypot(vertex.position.x, vertex.position.z));
    }
    return widest;
}

} // namespace

TEST_CASE("A bullet hole wraps a pillar instead of lying on it like a plate", "[weapons][holes]")
{
    World world;
    // The level's own pillar, built the way the level builds it: a triangle mesh from the drawn
    // cylinder, so the physics surface is exactly the faceted surface on screen.
    constexpr float kRadius = 0.28f;
    const MeshData pillar = Primitives::Cylinder(kRadius, 2.6f, 16);
    world.physics.CreateMeshBody(pillar, Transform{{0.0f, 1.3f, 0.0f}});
    world.physics.OptimizeBroadPhase();

    // A round from the side, level with the middle of the pillar.
    const RayHit hit = world.physics.RayCast({-3.0f, 1.3f, 0.07f}, {1.0f, 0.0f, 0.0f}, 5.0f);
    REQUIRE(hit);

    const MeshData shape = BuildBulletHoleShape();
    const BulletHolePlacement placed =
        ConformBulletHole(shape, world.physics, hit.position, hit.normal, 0.3f, 1.15f);

    // Every vertex found the surface under it.
    CHECK(placed.conformed == static_cast<int>(shape.vertices.size()));

    // And sits the same small distance off it all the way round: measured from the pillar's axis,
    // the mark is a thin shell a few millimetres outside the surface rather than a flat plate
    // touching it in the middle and standing off at the edges.
    //
    // Measured against the circle through the facet corners, so the facets themselves (a sixteen-
    // sided pillar is up to 5 mm inside that circle between corners) count as surface.
    float nearest = 1e9f;
    float furthest = -1e9f;
    for (const MeshVertex& vertex : placed.mesh.vertices)
    {
        const glm::vec3 world = placed.position + placed.rotation * vertex.position;
        const float fromAxis = std::hypot(world.x, world.z);
        nearest = std::min(nearest, fromAxis - kRadius);
        furthest = std::max(furthest, fromAxis - kRadius);
    }
    const float lip = LipRadius(shape) * 1.15f;
    INFO("off the pillar between " << nearest * 1000.0f << " and " << furthest * 1000.0f
                                   << " mm; a flat disc would stand " << FlatGap(kRadius, lip) * 1000.0f
                                   << " mm further off at its edge");
    // Nothing inside the pillar, or the depth test hides it.
    CHECK(nearest > -0.006f);
    // And nothing further out than the stand-off, the lip's height and a facet's worth of chord.
    CHECK(furthest < kBulletHoleStandOff + 0.0015f + 0.006f);
}

TEST_CASE("A bullet hole on a ball follows the ball's curve", "[weapons][holes]")
{
    World world;
    // A smooth sphere, as the dropped physics balls are, and small enough that a flat mark is
    // obviously wrong on it.
    constexpr float kRadius = 0.15f;
    world.physics.CreateSphere(kRadius, Transform{{0.0f, 1.0f, 0.0f}}, BodyMotion::Static);
    world.physics.OptimizeBroadPhase();

    // Struck high on the side, which is where a flat mark stood furthest off.
    const glm::vec3 direction = glm::normalize(glm::vec3(1.0f, -0.6f, 0.0f));
    const RayHit hit = world.physics.RayCast(glm::vec3(0.0f, 1.0f, 0.0f) - direction * 2.0f +
                                                 glm::vec3(0.0f, 0.05f, 0.0f),
                                             direction, 5.0f);
    REQUIRE(hit);

    const MeshData shape = BuildBulletHoleShape();
    const BulletHolePlacement placed =
        ConformBulletHole(shape, world.physics, hit.position, hit.normal, 1.1f, 1.15f);
    CHECK(placed.conformed == static_cast<int>(shape.vertices.size()));

    // Distance from the centre of the ball, minus its radius, for every vertex. On a true wrap every
    // one of them is the stand-off plus that vertex's own height in the crater.
    float worstError = 0.0f;
    for (size_t i = 0; i < shape.vertices.size(); ++i)
    {
        const glm::vec3 world =
            placed.position + placed.rotation * placed.mesh.vertices[i].position;
        const float above = glm::length(world - glm::vec3(0.0f, 1.0f, 0.0f)) - kRadius;
        const float wanted = kBulletHoleStandOff + shape.vertices[i].position.y * 1.15f;
        worstError = std::max(worstError, std::abs(above - wanted));
    }
    const float flat = FlatGap(kRadius, LipRadius(shape) * 1.15f);
    INFO("worst vertex is " << worstError * 1000.0f << " mm from where a wrapped mark puts it; a flat "
                            << "disc's edge was " << flat * 1000.0f << " mm off");
    CHECK(worstError < 0.0005f);
    // Which is a real improvement, not a rounding difference.
    CHECK(worstError < flat * 0.25f);

    // And the normals turned with it, so the lip shades as though it were lying on the ball: the
    // outermost vertices point away from the centre, not all in the direction of the impact.
    for (size_t i = 0; i < shape.vertices.size(); ++i)
    {
        const glm::vec3 world =
            placed.position + placed.rotation * placed.mesh.vertices[i].position;
        const glm::vec3 radial = glm::normalize(world - glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 normal = placed.rotation * placed.mesh.vertices[i].normal;
        CHECK(glm::dot(normal, radial) > 0.5f);
    }
}

TEST_CASE("A bullet hole near an edge shrinks rather than hanging off it", "[weapons][holes]")
{
    World world;
    // A crate, and a round landing a centimetre from its top edge.
    world.physics.CreateBox({0.5f, 0.5f, 0.5f}, Transform{{0.0f, 0.5f, 0.0f}}, BodyMotion::Static);
    world.physics.OptimizeBroadPhase();
    const glm::vec3 at{-0.5f, 0.99f, 0.0f};
    const glm::vec3 normal{-1.0f, 0.0f, 0.0f};

    const MeshData shape = BuildBulletHoleShape();
    const BulletHolePlacement placed = ConformBulletHole(shape, world.physics, at, normal, 0.0f, 1.0f);

    // Some of the mark had nothing under it and was pulled back onto the face.
    CHECK(placed.pulledIn > 0);
    // None of it is above the top of the crate, where it would stand in the air.
    for (const MeshVertex& vertex : placed.mesh.vertices)
    {
        const glm::vec3 world = placed.position + placed.rotation * vertex.position;
        CHECK(world.y <= 1.0f + 1e-4f);
    }
}

TEST_CASE("A bullet hole on a flat wall is the crater it always was", "[weapons][holes]")
{
    // The flat case must not have been disturbed by any of this: on a wall every vertex is the
    // shape's own vertex, stood off by the same amount.
    World world;
    world.physics.CreateBox({2.0f, 2.0f, 0.1f}, Transform{{0.0f, 2.0f, 0.0f}}, BodyMotion::Static);
    world.physics.OptimizeBroadPhase();
    const glm::vec3 at{0.2f, 1.5f, 0.1f};

    const MeshData shape = BuildBulletHoleShape();
    const BulletHolePlacement placed =
        ConformBulletHole(shape, world.physics, at, {0.0f, 0.0f, 1.0f}, 0.7f, 1.0f);
    REQUIRE(placed.mesh.vertices.size() == shape.vertices.size());
    CHECK(placed.conformed == static_cast<int>(shape.vertices.size()));
    for (size_t i = 0; i < shape.vertices.size(); ++i)
    {
        const glm::vec3 expected = shape.vertices[i].position + glm::vec3(0.0f, kBulletHoleStandOff, 0.0f);
        CHECK(glm::length(placed.mesh.vertices[i].position - expected) < 1e-4f);
    }
}
