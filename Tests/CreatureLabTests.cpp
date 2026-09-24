#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/Creature.h"
#include "Game/World/LabMap.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <memory>
#include <string>

using namespace pred;

// Creatures in the creature lab: what they can jump onto, what they cannot reach, doors, grabbing
// somebody and taking them to the nest.
namespace
{

// A hunter from this seed, whatever temperament the seed itself would give it. Most of these tests are
// about how a creature hunts, and a timid one would pass them by keeping out of the way; the
// temperaments have tests of their own.
CreatureTraits Hunter(uint32_t seed)
{
    CreatureTraits traits = CreatureTraits::FromSeed(seed);
    traits.temperament = Temperament::Predator;
    return traits;
}

struct Lab
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;
    NavMesh nav;

    Lab()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        BuildLabMap(scene, meshes, &physics);
        REQUIRE(nav.Build(physics.StaticTriangles(), NavSettings{}));
    }

    glm::vec3 At(float lx, float lz) const
    {
        glm::vec3 out;
        REQUIRE(nav.NearestPoint({LabSpec::kX + lx, 0.0f, LabSpec::kZ + lz}, 1.5f, out));
        return out;
    }

    // Whether a body that can make `jumps` gets from `from` to the top of block `block` in the crate yard.
    bool ReachesBlock(const glm::vec3& from, size_t block, uint16_t jumps) const
    {
        const glm::vec3 top{LabSpec::kBlockRowX + LabSpec::kBlockSpacing * static_cast<float>(block),
                            LabSpec::kBlockHeights[block], LabSpec::kBlockRowZ};
        std::vector<glm::vec3> corners;
        bool reached = false;
        nav.FindPath(from, top, corners, &reached, nullptr, jumps);
        return reached && !corners.empty() && std::abs(corners.back().y - top.y) < 0.3f;
    }
};

std::string MindOf(const Creature& creature)
{
    std::string mind;
    for (const CreatureBrain::TimelineEntry& entry : creature.Brain().Timeline())
    {
        mind += "\n  " + std::to_string(entry.time).substr(0, 5) + "  " + entry.what;
    }
    return mind;
}

} // namespace

TEST_CASE("In the lab, what can jump onto what is decided by how high it jumps", "[creature][lab][navigation]")
{
    Lab lab;
    CHECK(lab.nav.JumpCount() > 20);
    const glm::vec3 yard = lab.At(-19.0f, 17.0f);

    // Walking alone reaches none of the blocks; each jump height reaches the blocks up to it.
    CHECK_FALSE(lab.ReachesBlock(yard, 1, 0));
    CHECK(lab.ReachesBlock(yard, 1, NavMesh::kJumpLow));      // a metre
    CHECK_FALSE(lab.ReachesBlock(yard, 3, NavMesh::kJumpLow)); // two metres is too high for a low jump
    CHECK(lab.ReachesBlock(yard, 3, NavMesh::kJumpLow | NavMesh::kJumpMid | NavMesh::kJumpHigh));
    // Three and a half metres is beyond anything.
    CHECK_FALSE(lab.ReachesBlock(yard, 5, NavMesh::kAllJumps));

    // The watchtower is up a stair too narrow for anything but a person, and too high to jump.
    glm::vec3 towerTop;
    REQUIRE(lab.nav.NearestPoint({LabSpec::kX - 16.0f, 3.4f, LabSpec::kZ + 5.0f}, 1.0f, towerTop));
    std::vector<glm::vec3> corners;
    bool reached = true;
    lab.nav.FindPath(yard, towerTop, corners, &reached, nullptr, NavMesh::kAllJumps);
    CHECK_FALSE(reached);
}

TEST_CASE("A creature jumps up onto a block to get at somebody standing on it", "[creature][lab][jump]")
{
    Lab lab;
    // A crawler that jumps high enough for the two-metre block.
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 400 && seed == 0; ++candidate)
    {
        const CreatureAnatomy anatomy = CreatureAnatomy::FromSeed(candidate);
        const CreatureCapabilities caps = CreatureCapabilities::From(anatomy);
        const CreatureTraits traits = Hunter(candidate);
        if (caps.jump >= 2.6f && anatomy.eyes > 0 && traits.aggression > 0.6f && traits.fear < 0.5f)
        {
            seed = candidate;
        }
    }
    REQUIRE(seed != 0);
    const glm::vec3 blockTop{LabSpec::kBlockRowX + LabSpec::kBlockSpacing * 3.0f, LabSpec::kBlockHeights[3], LabSpec::kBlockRowZ};
    const glm::vec3 start = lab.At(-17.2f, 20.0f);
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(seed), start);
    SensedPlayer player;
    player.id = 1;
    player.name = "Up There";
    player.feet = blockTop;

    constexpr float dt = 1.0f / 60.0f;
    float time = 0.0f;
    bool jumped = false;
    float highest = start.y;
    for (int tick = 0; tick < 60 * 12; ++tick)
    {
        time += dt;
        CreatureSenses senses;
        senses.players = {player};
        creature.Update(senses, time, dt);
        creature.UpdateVisual(dt);
        lab.physics.Step(dt);
        jumped = jumped || creature.Airborne() > 0.0f;
        highest = std::max(highest, creature.Position().y);
    }
    INFO("seed " << seed << ", got as high as " << highest << "; its mind:" << MindOf(creature));
    CHECK(jumped);
    CHECK(highest > LabSpec::kBlockHeights[3] - 0.3f);
}

TEST_CASE("A creature opens a shut door in its way, and breaks down a locked one", "[creature][lab][door]")
{
    Lab lab;
    // The corridor's own door, across the way in from the south, shut.
    const float doorX = LabSpec::kCorridorDoorX;
    const float doorZ = LabSpec::kCorridorSouth + 0.15f;
    DoorSense door;
    door.index = 3;
    door.a = {doorX - 0.55f, 0.0f, doorZ};
    door.b = {doorX + 0.55f, 0.0f, doorZ};
    door.shut = true;

    for (const bool locked : {false, true})
    {
        INFO((locked ? "locked" : "not locked"));
        door.locked = locked;
        const glm::vec3 outside = lab.At(LabSpec::kCorridorDoorX - LabSpec::kX, LabSpec::kCorridorSouth - LabSpec::kZ + 2.5f);
        Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(5), outside);
        // Somebody heard inside: something to go and look at, through the door.
        Noise noise;
        noise.kind = NoiseKind::Gunshot;
        noise.position = {doorX, 0.0f, LabSpec::kCorridorSouth - 6.0f};
        noise.reach = 70.0f;
        constexpr float dt = 1.0f / 60.0f;
        float time = 0.0f;
        bool opened = false;
        int blows = 0;
        for (int tick = 0; tick < 60 * 10 && !opened && blows < 2; ++tick)
        {
            time += dt;
            CreatureSenses senses;
            senses.doors = {door};
            if (tick == 1)
            {
                senses.noises = {noise};
            }
            creature.Update(senses, time, dt);
            opened = opened || creature.Brain().Intent().openDoor == door.index;
            blows += creature.Brain().Intent().bashDoor == door.index ? 1 : 0;
            // It never walks through the shut door.
            CHECK(creature.Position().z > doorZ - 0.2f);
        }
        INFO("its mind:" << MindOf(creature));
        if (locked)
        {
            CHECK_FALSE(opened);
            CHECK(blows >= 2);
        }
        else
        {
            CHECK(opened);
        }
    }
}

TEST_CASE("Holding somebody, a creature takes them off: to its nest, or somewhere to kill them", "[creature][lab][hive]")
{
    Lab lab;
    glm::vec3 stand;
    REQUIRE(lab.nav.NearestPoint(LabSpec::kHive, 4.0f, stand));

    // Which of the two it does is its own choice each time it takes hold of somebody, so several goes:
    // both happen, and the nest is the rarer.
    int wrapped = 0;
    int killedElsewhere = 0;
    for (int attempt = 0; attempt < 14; ++attempt)
    {
        // A different creature each time: one creature always makes the same choice, because its mind is
        // its seed.
        const glm::vec3 start = lab.At(0.0f, -4.0f + static_cast<float>(attempt) * 0.2f);
        Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(13 + attempt * 7), start);
        SensedPlayer victim;
        victim.id = 2;
        victim.name = "Caught";
        constexpr float dt = 1.0f / 60.0f;
        float time = 0.0f;
        creature.Brain().OnGrabbed(victim.id, time);
        REQUIRE(creature.Brain().Current() == Behavior::Drag);

        bool cocooned = false;
        bool fed = false;
        for (int tick = 0; tick < 60 * 30 && !cocooned && !fed; ++tick)
        {
            time += dt;
            // Carried along in front of it, as the game carries them.
            victim.feet = creature.Position() + creature.Forward() * 1.2f;
            CreatureSenses senses;
            senses.players = {victim};
            senses.hasHive = true;
            senses.hive = stand;
            creature.Update(senses, time, dt);
            cocooned = creature.Brain().Intent().cocoonTarget == victim.id;
            // Biting into somebody it is holding, well away from the nest: killing them where it stopped.
            fed = creature.Brain().Intent().strikeTarget == victim.id &&
                  glm::distance(creature.Position(), stand) > 6.0f;
            CHECK((creature.Brain().Intent().holding == victim.id || cocooned));
        }
        INFO("attempt " << attempt << "; its mind:" << MindOf(creature));
        CHECK((cocooned || fed));
        wrapped += cocooned ? 1 : 0;
        killedElsewhere += fed ? 1 : 0;
        if (cocooned)
        {
            CHECK(creature.Brain().Holding() < 0);
        }
    }
    INFO(wrapped << " taken to the nest, " << killedElsewhere << " killed where they were dragged");
    CHECK(wrapped >= 1);
    CHECK(killedElsewhere >= 1);
    CHECK(killedElsewhere > wrapped);
}

TEST_CASE("Shot enough while it holds somebody, a creature lets go", "[creature][hive]")
{
    Lab lab;
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(13), lab.At(0.0f, 0.0f));
    creature.Brain().OnGrabbed(2, 0.0f);
    REQUIRE(creature.Brain().Holding() == 2);
    // A few rounds is not enough; a burst from the others is.
    creature.TakeDamage(creature.MaxHealth() * 0.02f, 1, creature.Position() + glm::vec3(0.0f, 1.0f, 5.0f), 0.1f);
    CHECK(creature.Brain().Holding() == 2);
    creature.TakeDamage(creature.MaxHealth() * 0.06f, 1, creature.Position() + glm::vec3(0.0f, 1.0f, 5.0f), 0.2f);
    CHECK(creature.Brain().Holding() < 0);
    CHECK(creature.Brain().Current() != Behavior::Drag);
}

TEST_CASE("A creature that nests builds one, somewhere dark and out of the way", "[creature][lab][hive]")
{
    Lab lab;
    // One that builds. Most do not.
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 400 && seed == 0; ++candidate)
    {
        const CreatureTraits traits = Hunter(candidate);
        if (traits.Nests() && traits.fear < 0.6f)
        {
            seed = candidate;
        }
    }
    REQUIRE(seed != 0);
    REQUIRE(Hunter(seed).Nests());
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(seed), lab.At(0.0f, 20.0f));

    constexpr float dt = 1.0f / 60.0f;
    float time = 0.0f;
    bool built = false;
    glm::vec3 where{0.0f};
    for (int tick = 0; tick < 60 * 90 && !built; ++tick)
    {
        time += dt;
        CreatureSenses senses;
        // The nest chamber across the north end is dark; the rest of the lab is under the sky.
        senses.lightAt = [](const glm::vec3& at) { return at.z < LabSpec::kZ - 12.0f ? 0.05f : 1.0f; };
        creature.Update(senses, time, dt);
        if (creature.Brain().Intent().buildHive)
        {
            built = true;
            where = creature.Brain().Intent().hiveAt;
        }
    }
    INFO("its mind:" << MindOf(creature));
    REQUIRE(built);
    // In the dark, which is what it was looking for.
    CHECK(where.z < LabSpec::kZ - 12.0f);
    // And nothing builds a second one on top of the first.
    CHECK(creature.Brain().Current() != Behavior::Nest);
}

TEST_CASE("A creature that does not nest never builds one", "[creature][hive]")
{
    Lab lab;
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 200 && seed == 0; ++candidate)
    {
        if (!Hunter(candidate).Nests())
        {
            seed = candidate;
        }
    }
    REQUIRE(seed != 0);
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(seed), lab.At(0.0f, 20.0f));
    constexpr float dt = 1.0f / 60.0f;
    float time = 0.0f;
    for (int tick = 0; tick < 60 * 60; ++tick)
    {
        time += dt;
        CreatureSenses senses;
        creature.Update(senses, time, dt);
        REQUIRE_FALSE(creature.Brain().Intent().buildHive);
        REQUIRE(creature.Brain().Current() != Behavior::Nest);
    }
}
