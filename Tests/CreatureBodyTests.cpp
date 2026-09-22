#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/Creature.h"
#include "Game/Creature/CreatureRig.h"
#include "Game/Creature/CreatureSkin.h"
#include "Game/World/TestMap.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <memory>

using namespace pred;

// The creature's body: one skin over a skeleton, posed by procedural animation, falling as a ragdoll.

TEST_CASE("A creature's skin is one mesh over its skeleton, the same for the same seed", "[creature][skin]")
{
    for (const uint32_t seed : {1u, 4u, 13u, 21u})
    {
        const CreatureAnatomy anatomy = CreatureAnatomy::FromSeed(seed);
        const CreatureSkin skin = CreatureSkin::Build(anatomy);
        INFO("seed " << seed << ": " << anatomy.Describe());
        REQUIRE_FALSE(skin.mesh.vertices.empty());
        REQUIRE(skin.weights.size() == skin.mesh.vertices.size());
        // Detailed enough to be a body, light enough to bend on the processor every frame.
        CHECK(skin.mesh.TriangleCount() > 4000);
        CHECK(skin.mesh.TriangleCount() < 30000);
        CHECK(skin.head >= 0);
        CHECK(skin.jaw >= 0);
        CHECK(skin.limbs.size() == anatomy.Rest().legs.size());

        // Every vertex follows bones that exist, by weights that add up.
        for (size_t v = 0; v < skin.weights.size(); v += 17)
        {
            float total = 0.0f;
            for (size_t k = 0; k < 4; ++k)
            {
                CHECK(skin.weights[v].bone[k] < skin.bones.size());
                total += skin.weights[v].weight[k];
            }
            CHECK(total == Catch::Approx(1.0f).margin(1e-3));
            CHECK(glm::length(skin.mesh.vertices[v].normal) == Catch::Approx(1.0f).margin(1e-2));
        }

        // Standing on the ground under it, no lower.
        AABB bounds = skin.mesh.ComputeBounds();
        CHECK(bounds.min.y > -0.06f);
        CHECK(bounds.max.y > anatomy.hipHeight);

        // The same seed, the same skin.
        const CreatureSkin again = CreatureSkin::Build(anatomy);
        CHECK(again.mesh.vertices.size() == skin.mesh.vertices.size());
        CHECK(again.mesh.indices == skin.mesh.indices);
    }
}

TEST_CASE("At rest the skeleton leaves the skin where it was built", "[creature][skin]")
{
    const CreatureAnatomy anatomy = CreatureAnatomy::FromSeed(13);
    const CreatureSkin skin = CreatureSkin::Build(anatomy);
    CreatureRig rig;
    rig.Init(anatomy, skin, glm::vec3(0.0f), 0.0f);
    RigInput input;
    input.dt = 0.0f;
    rig.Update(input);
    std::vector<MeshVertex> posed;
    AABB bounds;
    SkinVertices(skin, rig.Skinning(), posed, bounds);
    // Breathing and a small idle sway move it a little; nothing moves far.
    float furthest = 0.0f;
    for (size_t v = 0; v < posed.size(); ++v)
    {
        furthest = std::max(furthest, glm::distance(posed[v].position, skin.mesh.vertices[v].position));
    }
    CHECK(furthest < 0.12f);
}

TEST_CASE("Turning on the spot, a creature steps round rather than spinning on planted feet", "[creature][rig]")
{
    const CreatureAnatomy anatomy = CreatureAnatomy::FromSeed(13);
    const CreatureSkin skin = CreatureSkin::Build(anatomy);
    CreatureRig rig;
    rig.Init(anatomy, skin, glm::vec3(0.0f), 0.0f);
    const std::vector<CreatureRig::Foot> start = rig.Feet();

    RigInput input;
    input.dt = 1.0f / 60.0f;
    // A small turn: the feet stay where they are.
    for (int i = 0; i < 10; ++i)
    {
        input.yaw += 0.01f;
        input.time += input.dt;
        rig.Update(input);
    }
    for (size_t f = 0; f < start.size(); ++f)
    {
        CHECK(glm::distance(rig.Feet()[f].planted, start[f].planted) < 1e-4f);
    }
    // A half turn: every foot has been picked up and put down again, and points the new way.
    for (int i = 0; i < 180; ++i)
    {
        input.yaw = std::min(input.yaw + 0.03f, glm::pi<float>());
        input.time += input.dt;
        rig.Update(input);
    }
    for (int i = 0; i < 60; ++i)
    {
        input.time += input.dt;
        rig.Update(input);
    }
    for (size_t f = 0; f < start.size(); ++f)
    {
        INFO("foot " << f);
        CHECK(glm::distance(rig.Feet()[f].planted, start[f].planted) > 0.05f);
        CHECK(std::abs(std::remainder(rig.Feet()[f].yaw - glm::pi<float>(), glm::two_pi<float>())) < 0.3f);
        CHECK_FALSE(rig.Feet()[f].stepping);
    }
}

TEST_CASE("Walking, feet stay on the ground while planted and keep up with the body", "[creature][rig]")
{
    const CreatureAnatomy anatomy = CreatureAnatomy::FromSeed(4);
    const CreatureSkin skin = CreatureSkin::Build(anatomy);
    CreatureRig rig;
    rig.Init(anatomy, skin, glm::vec3(0.0f), 0.0f);
    RigInput input;
    input.dt = 1.0f / 60.0f;
    input.velocity = {0.0f, 0.0f, -2.0f};
    float furthest = 0.0f;
    for (int i = 0; i < 240; ++i)
    {
        input.position += input.velocity * input.dt;
        input.time += input.dt;
        rig.Update(input);
        for (const CreatureRig::Foot& foot : rig.Feet())
        {
            if (!foot.stepping)
            {
                CHECK(foot.planted.y == Catch::Approx(0.0f).margin(1e-4));
            }
            furthest = std::max(furthest, glm::length(glm::vec2(foot.planted.x - input.position.x, foot.planted.z - input.position.z)));
        }
    }
    // Eight metres walked, and no foot left more than a body length behind.
    CHECK(furthest < anatomy.OverallLength() + 0.5f);
}

TEST_CASE("The head turns towards what it is looking at, within what a neck can do", "[creature][rig]")
{
    const CreatureAnatomy anatomy = CreatureAnatomy::FromSeed(13);
    const CreatureSkin skin = CreatureSkin::Build(anatomy);
    CreatureRig rig;
    rig.Init(anatomy, skin, glm::vec3(0.0f), 0.0f);
    RigInput input;
    input.dt = 1.0f / 60.0f;
    input.look = true;
    input.lookAt = {5.0f, 1.0f, -1.0f}; // off to its right
    for (int i = 0; i < 90; ++i)
    {
        input.time += input.dt;
        rig.Update(input);
    }
    CHECK(rig.HeadTurn() > 0.6f);
    CHECK(rig.HeadTurn() < 1.4f);
    input.lookAt = {-5.0f, 1.0f, -1.0f};
    for (int i = 0; i < 90; ++i)
    {
        input.time += input.dt;
        rig.Update(input);
    }
    CHECK(rig.HeadTurn() < -0.6f);
}

TEST_CASE("Killed, a creature falls as a ragdoll onto the floor and is still hit where it lies", "[creature][ragdoll]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    BuildTestMap(scene, meshes, &physics);
    NavMesh nav;
    REQUIRE(nav.Build(physics.StaticTriangles(), NavSettings{}));
    Creature creature(scene, meshes, physics, &nav, CreatureTraits::FromSeed(13), {0.0f, 0.0f, 0.0f});
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 20; ++i)
    {
        creature.UpdateVisual(dt);
        physics.Step(dt);
    }
    const float standing = glm::vec3(creature.BoneWorld(creature.Skin().chest)[3]).y;
    creature.TakeDamage(1.0e6f, 1, {0.0f, 1.0f, 5.0f}, 1.0f);
    REQUIRE_FALSE(creature.Alive());
    for (int i = 0; i < 240; ++i)
    {
        creature.UpdateVisual(dt);
        physics.Step(dt);
    }
    REQUIRE(creature.Ragdolled());
    const float lying = glm::vec3(creature.BoneWorld(creature.Skin().chest)[3]).y;
    INFO("chest at " << standing << " standing and " << lying << " lying");
    CHECK(lying < standing - 0.2f);
    CHECK(lying > -0.05f); // on the floor, not through it

    // A round aimed at where it lies finds it.
    const glm::vec3 chest = glm::vec3(creature.BoneWorld(creature.Skin().chest)[3]);
    const RayHit hit = physics.RayCast(chest + glm::vec3(0.0f, 2.0f, 0.0f), {0.0f, -1.0f, 0.0f}, 2.5f);
    REQUIRE(hit);
    CHECK(creature.Owns(hit.body));
}

TEST_CASE("A round in the head counts for more than one in a hand", "[creature][hitbox]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    Creature creature(scene, meshes, physics, nullptr, CreatureTraits::FromSeed(13), {0.0f, 0.0f, 0.0f});
    creature.UpdateVisual(1.0f / 60.0f);
    physics.Step(1.0f / 60.0f);

    // Straight down onto the skull from above it.
    const glm::vec3 head = glm::vec3(creature.BoneWorld(creature.Skin().head)[3]);
    const glm::vec3 skullTop = glm::vec3(creature.BoneWorld(creature.Skin().head) * glm::vec4(0.0f, 0.08f, 0.0f, 1.0f));
    const RayHit hit = physics.RayCast(skullTop + glm::vec3(0.0f, 1.5f, 0.0f), {0.0f, -1.0f, 0.0f}, 3.0f);
    REQUIRE(hit);
    INFO("head at " << head.x << " " << head.y << " " << head.z);
    REQUIRE(creature.Owns(hit.body));
    CHECK(creature.DamageScale(hit.body) > 1.5f);

    const int hand = creature.Skin().limbs.front().end;
    const glm::vec3 wrist = glm::vec3(creature.BoneWorld(hand) * glm::vec4(0.0f, 0.05f, 0.0f, 1.0f));
    const RayHit low = physics.RayCast(wrist + glm::vec3(0.0f, 0.0f, -1.5f), {0.0f, 0.0f, 1.0f}, 1.5f);
    REQUIRE(low);
    REQUIRE(creature.Owns(low.body));
    CHECK(creature.DamageScale(low.body) < 1.0f);
}
