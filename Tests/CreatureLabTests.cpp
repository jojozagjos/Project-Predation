#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/Creature.h"
#include "Game/World/LabMap.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

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

float Horizontal2(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(glm::vec2(a.x - b.x, a.z - b.z));
}

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

TEST_CASE("Holding somebody, a creature takes them off: to its nest, or away from the others to throw them down", "[creature][lab][hive]")
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
            // Well away from the nest, let go and turned on them: it never kills anybody it is holding.
            fed = creature.Brain().Holding() < 0 && creature.Brain().Current() == Behavior::Attack &&
                  glm::distance(creature.Position(), stand) > 6.0f;
            CHECK((creature.Brain().Intent().holding == victim.id || cocooned || fed));
            CHECK((creature.Brain().Intent().strikeTarget != victim.id || fed));
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

TEST_CASE("A crawlspace is in the mesh, marked, and only a body that fits is routed along it", "[creature][lab][navigation][crawl]")
{
    Lab lab;
    // The crawlspace runs north to south down the middle of the lab, 1.3 m high, 2.4 m wide.
    const glm::vec3 inside{LabSpec::kX, 0.0f, LabSpec::kZ + 11.0f};
    const glm::vec3 onRoof{LabSpec::kX, 1.9f, LabSpec::kZ + 11.0f};
    CHECK(lab.nav.InCrawlspace(inside));
    CHECK_FALSE(lab.nav.InCrawlspace(onRoof));
    CHECK_FALSE(lab.nav.InCrawlspace(lab.At(6.0f, 11.0f)));

    // Its two ends are openings onto standing floor.
    int ends = 0;
    for (const glm::vec3& mouth : lab.nav.CrawlMouths())
    {
        if (std::abs(mouth.x - LabSpec::kX) < 1.5f &&
            (std::abs(mouth.z - (LabSpec::kZ + 6.0f)) < 1.2f || std::abs(mouth.z - (LabSpec::kZ + 16.0f)) < 1.2f))
        {
            ++ends;
        }
    }
    INFO(lab.nav.CrawlMouths().size() << " openings in all");
    CHECK(ends >= 2);

    // Something that cannot crawl gets no nearer than the mouth -- or the roof -- and knows it.
    const glm::vec3 outside = lab.At(0.0f, 2.0f);
    std::vector<glm::vec3> route;
    bool reached = true;
    lab.nav.FindPath(outside, inside, route, &reached, nullptr, NavMesh::kAllJumps);
    CHECK_FALSE(reached);
    // Something that can goes all the way in.
    lab.nav.FindPath(outside, inside, route, &reached, nullptr, NavMesh::kAllJumps | NavMesh::kCrawl);
    CHECK(reached);
    REQUIRE_FALSE(route.empty());
    CHECK(glm::distance(route.back(), inside) < 0.6f);
    // And moving along the floor in there works for it.
    glm::vec3 moved;
    glm::vec3 crawlStart;
    REQUIRE(lab.nav.NearestPoint(inside, 1.0f, crawlStart, NavMesh::kCrawl));
    CHECK(lab.nav.MoveAlongSurface(crawlStart, crawlStart + glm::vec3(0.0f, 0.0f, 0.5f), moved, NavMesh::kCrawl));
}

namespace
{

// Somebody lying in the middle of the crawlspace, and a creature from the seed that `fits` picks,
// hunting them from outside for a while. Where it got to, and whether it was ever up on the roof.
struct CrawlChase
{
    uint32_t seed = 0;
    glm::vec3 end{0.0f};
    bool onRoof = false;
    Behavior doing = Behavior::Roam;
    std::string mind;
};

CrawlChase ChaseIntoCrawlspace(bool fits)
{
    Lab lab;
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 600 && seed == 0; ++candidate)
    {
        const CreatureAnatomy anatomy = CreatureAnatomy::FromSeed(candidate);
        const CreatureCapabilities caps = CreatureCapabilities::From(anatomy);
        const CreatureTraits traits = Hunter(candidate);
        if (caps.fitsVents == fits && anatomy.eyes > 0 && traits.aggression > 0.55f && traits.fear < 0.5f)
        {
            seed = candidate;
        }
    }
    REQUIRE(seed != 0);
    // South of it, facing north up the tunnel at them.
    const glm::vec3 start = lab.At(-3.0f, 21.0f);
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(seed), start);
    SensedPlayer player;
    player.id = 1;
    player.name = "Crawling";
    player.feet = {LabSpec::kX, 0.0f, LabSpec::kZ + 11.0f};
    player.height = 0.45f;

    CrawlChase result;
    result.seed = seed;
    constexpr float dt = 1.0f / 60.0f;
    float time = 0.0f;
    for (int tick = 0; tick < 60 * 20; ++tick)
    {
        time += dt;
        CreatureSenses senses;
        senses.players = {player};
        creature.Update(senses, time, dt);
        creature.UpdateVisual(dt);
        const glm::vec3 at = creature.Position();
        const bool overTunnel = std::abs(at.x - LabSpec::kX) < 1.5f && at.z > LabSpec::kZ + 6.2f && at.z < LabSpec::kZ + 15.8f;
        result.onRoof = result.onRoof || (overTunnel && at.y > 1.0f);
    }
    result.end = creature.Position();
    result.doing = creature.Brain().Current();
    result.mind = MindOf(creature);
    return result;
}

} // namespace

TEST_CASE("Something too big for the crawlspace waits at its mouth rather than climbing on top of it",
          "[creature][lab][crawl]")
{
    const CrawlChase chase = ChaseIntoCrawlspace(false);
    INFO("seed " << chase.seed << " ended at " << chase.end.x << ", " << chase.end.y << ", " << chase.end.z << chase.mind);
    CHECK_FALSE(chase.onRoof);
    CHECK(chase.doing == Behavior::Ambush);
    // Beside one of the two ends.
    const float north = glm::length(glm::vec2(chase.end.x - LabSpec::kX, chase.end.z - (LabSpec::kZ + 6.0f)));
    const float south = glm::length(glm::vec2(chase.end.x - LabSpec::kX, chase.end.z - (LabSpec::kZ + 16.0f)));
    CHECK(std::min(north, south) < 3.5f);
}

TEST_CASE("Something small enough crawls into the crawlspace after them", "[creature][lab][crawl]")
{
    const CrawlChase chase = ChaseIntoCrawlspace(true);
    INFO("seed " << chase.seed << " ended at " << chase.end.x << ", " << chase.end.y << ", " << chase.end.z << chase.mind);
    CHECK_FALSE(chase.onRoof);
    const float gap = glm::length(glm::vec2(chase.end.x - LabSpec::kX, chase.end.z - (LabSpec::kZ + 11.0f)));
    CHECK(gap < 3.0f);
}

TEST_CASE("A patient creature that loses somebody through a door waits beside it, on the far side from them",
          "[creature][lab][ambush]")
{
    Lab lab;
    const float doorX = LabSpec::kCorridorDoorX;
    const float doorZ = LabSpec::kCorridorSouth + 0.15f;
    DoorSense door;
    door.index = 3;
    door.a = {doorX - 0.55f, 0.0f, doorZ};
    door.b = {doorX + 0.55f, 0.0f, doorZ};
    door.shut = false;

    CreatureTraits traits = Hunter(5);
    traits.stealth = 0.9f;
    traits.patience = 0.9f;
    traits.aggression = 0.5f;
    traits.fear = 0.2f;
    const glm::vec3 outside = lab.At(LabSpec::kCorridorDoorX - LabSpec::kX - 1.0f, LabSpec::kCorridorSouth - LabSpec::kZ + 7.0f);
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, traits, outside);

    SensedPlayer player;
    player.id = 1;
    player.name = "Through The Door";
    player.feet = {doorX, 0.0f, doorZ - 2.0f}; // just inside, seen through the doorway
    player.forward = {0.0f, 0.0f, 1.0f};        // backing in, watching the way it would come
    constexpr float dt = 1.0f / 60.0f;
    float time = 0.0f;
    int tick = 0;
    const auto clear = [&](const glm::vec3& from, const glm::vec3& to)
    {
        // Out of its sight, once they are in there, from wherever it is.
        if (tick >= 90 && glm::distance(glm::vec2(to.x, to.z), glm::vec2(player.feet.x, player.feet.z)) < 0.6f)
        {
            return false;
        }
        const glm::vec3 along = to - from;
        const float length = glm::length(along);
        return length < 0.4f || !lab.physics.RayCastStatic(from, along / length, length - 0.3f);
    };
    bool waited = false;
    for (tick = 0; tick < 60 * 20; ++tick)
    {
        time += dt;
        // Seen through the doorway for a second and a half, then gone further in, out of its sight.
        if (tick == 90)
        {
            player.feet = {doorX, 0.0f, LabSpec::kCorridorSouth - 6.0f};
        }
        CreatureSenses senses;
        senses.players = {player};
        senses.doors = {door};
        senses.clearLine = clear;
        creature.Update(senses, time, dt);
        creature.UpdateVisual(dt);
        const CreatureBrain::AmbushPlan& plan = creature.Brain().Ambush();
        if (creature.Brain().Current() == Behavior::Ambush && plan.valid && plan.kind == CreatureBrain::AmbushKind::Door &&
            Horizontal2(creature.Position(), plan.point) < 0.8f)
        {
            waited = true;
            // Beside the doorway, on its own side of it: outside the corridor.
            CHECK(plan.point.z > doorZ + 0.3f);
            CHECK(std::abs(plan.point.x - doorX) > 0.7f);
        }
    }
    std::string options;
    for (const CreatureBrain::Option& option : creature.Brain().Options())
    {
        options += "\n  " + option.label + " " + std::to_string(option.score);
    }
    INFO("it ended at " << creature.Position().x << ", " << creature.Position().z << "; the door is at " << doorX
                        << ", " << doorZ);
    INFO("options at the end:" << options << "\n ambush plan " << creature.Brain().Ambush().valid << " at "
                               << creature.Brain().Ambush().point.x << ", " << creature.Brain().Ambush().point.z);
    INFO("its mind:" << MindOf(creature));
    CHECK(waited);
}

TEST_CASE("A creature that climbs goes up a pillar onto the ceiling, upside down, and climbs back down",
          "[creature][lab][climb]")
{
    Lab lab;
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 600 && seed == 0; ++candidate)
    {
        if (CreatureCapabilities::From(CreatureAnatomy::FromSeed(candidate)).climbs)
        {
            seed = candidate;
        }
    }
    REQUIRE(seed != 0);
    // Among the pillars, under the pillar forest's roof, four and a half metres up.
    const glm::vec3 start = lab.At(19.5f, -6.0f);
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(seed), start);
    creature.SetClimbOverride(1);
    constexpr float dt = 1.0f / 60.0f;
    float time = 0.0f;
    bool wentUpAWall = false;
    bool hung = false;
    for (int tick = 0; tick < 60 * 6 && !hung; ++tick)
    {
        time += dt;
        creature.Update(CreatureSenses{}, time, dt);
        creature.UpdateVisual(dt);
        wentUpAWall = wentUpAWall || creature.Clinging() == Creature::Cling::Wall;
        // Settled on the ceiling: up there, upside down under it, planning from the floor, and seeing
        // from under the ceiling rather than from the floor.
        const glm::vec3 up = creature.Orientation() * glm::vec3(0.0f, 1.0f, 0.0f);
        hung = creature.Clinging() == Creature::Cling::Ceiling && creature.Position().y > start.y + 4.0f && up.y < -0.9f &&
               std::abs(creature.Anchor().y - start.y) < 0.3f && creature.Eye().y > start.y + 2.5f;
    }
    INFO("seed " << seed << ", at " << creature.Position().x << ", " << creature.Position().y << ", " << creature.Position().z);
    CHECK(wentUpAWall);
    REQUIRE(hung);

    // Down again: the wall it went up, head first, rather than letting go and falling four metres.
    creature.SetClimbOverride(0);
    bool climbedDown = false;
    for (int tick = 0; tick < 60 * 6; ++tick)
    {
        time += dt;
        creature.Update(CreatureSenses{}, time, dt);
        creature.UpdateVisual(dt);
        climbedDown = climbedDown || creature.Clinging() == Creature::Cling::Wall;
    }
    CHECK(climbedDown);
    CHECK(creature.Clinging() == Creature::Cling::Floor);
    CHECK(std::abs(creature.Position().y - start.y) < 0.3f);
    const glm::vec3 upright = creature.Orientation() * glm::vec3(0.0f, 1.0f, 0.0f);
    CHECK(upright.y > 0.9f);
}

TEST_CASE("Beside a doorway, on the far side from somebody in the room, is where it would wait", "[creature][lab][ambush]")
{
    Lab lab;
    const float doorX = LabSpec::kCorridorDoorX;
    const float doorZ = LabSpec::kCorridorSouth + 0.15f;
    DoorSense door;
    door.index = 3;
    door.a = {doorX - 0.55f, 0.0f, doorZ};
    door.b = {doorX + 0.55f, 0.0f, doorZ};
    const glm::vec3 outside = lab.At(LabSpec::kCorridorDoorX - LabSpec::kX - 1.0f, LabSpec::kCorridorSouth - LabSpec::kZ + 7.0f);
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(5), outside);
    CreatureSenses senses;
    senses.position = outside;
    senses.nav = &lab.nav;
    senses.doors = {door};
    senses.clearLine = [&](const glm::vec3& from, const glm::vec3& to)
    {
        const glm::vec3 along = to - from;
        const float length = glm::length(along);
        return length < 0.4f || !lab.physics.RayCastStatic(from, along / length, length - 0.3f);
    };
    // Somebody two metres into the corridor.
    const glm::vec3 them{doorX, 0.0f, doorZ - 2.0f};
    CreatureBrain::AmbushPlan plan;
    REQUIRE(creature.Brain().PlanDoorAmbushNear(senses, them, plan));
    INFO("waits at " << plan.point.x << ", " << plan.point.z);
    CHECK(plan.point.z > doorZ + 0.3f);
    CHECK(std::abs(plan.point.x - doorX) > 0.7f);
    CHECK(glm::distance(plan.watch, (door.a + door.b) * 0.5f) < 0.1f);

    // From inside with them, it would have to go out past them: no.
    senses.position = glm::vec3(doorX, 0.0f, doorZ - 4.0f);
    const glm::vec3 nearDoor{doorX, 0.0f, doorZ - 1.0f};
    CreatureBrain::AmbushPlan inside;
    CHECK_FALSE(creature.Brain().PlanDoorAmbushNear(senses, nearDoor, inside));
}

TEST_CASE("Something small enough that loses somebody at a crawlspace goes down it after them", "[creature][lab][crawl]")
{
    Lab lab;
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 600 && seed == 0; ++candidate)
    {
        const CreatureAnatomy anatomy = CreatureAnatomy::FromSeed(candidate);
        const CreatureTraits traits = Hunter(candidate);
        if (CreatureCapabilities::From(anatomy).fitsVents && anatomy.eyes > 0 && traits.aggression > 0.55f &&
            traits.fear < 0.5f && traits.persistence > 14.0f)
        {
            seed = candidate;
        }
    }
    REQUIRE(seed != 0);
    Creature creature(lab.scene, lab.meshes, lab.physics, &lab.nav, Hunter(seed), lab.At(-2.0f, 0.0f));
    // They are seen at the north mouth, then crawl in out of its sight.
    const glm::vec3 mouth{LabSpec::kX, 0.0f, LabSpec::kZ + 5.2f};
    const glm::vec3 hideout{LabSpec::kX, 0.0f, LabSpec::kZ + 12.0f};
    SensedPlayer player;
    player.id = 1;
    player.name = "Crawling";
    player.forward = {0.0f, 0.0f, 1.0f};
    constexpr float dt = 1.0f / 60.0f;
    float time = 0.0f;
    float closest = 1.0e9f;
    for (int tick = 0; tick < 60 * 45; ++tick)
    {
        time += dt;
        const bool gone = time > 4.0f;
        player.feet = gone ? hideout : mouth;
        player.height = gone ? 0.45f : 1.8f;
        CreatureSenses senses;
        senses.players = {player};
        senses.mayBuildNest = false;
        // Heard at the mouth, so it turns and sees them there.
        if (tick % 30 == 0 && !gone)
        {
            Noise step;
            step.kind = NoiseKind::Footstep;
            step.reach = 20.0f;
            step.position = mouth;
            step.player = 1;
            senses.noises = {step};
        }
        // Nothing sees into the crawlspace from outside it, nor out of it from in.
        senses.clearLine = [&](const glm::vec3& from, const glm::vec3& to)
        { return lab.nav.InCrawlspace({from.x, 0.0f, from.z}) == lab.nav.InCrawlspace({to.x, 0.0f, to.z}); };
        creature.Update(senses, time, dt);
        creature.UpdateVisual(dt);
        // In there with them, not on the roof over them.
        if (gone && lab.nav.InCrawlspace(creature.Position()))
        {
            closest = std::min(closest, Horizontal2(creature.Position(), hideout));
        }
    }
    INFO("seed " << seed << ", closest " << closest << MindOf(creature));
    CHECK(closest < 3.0f);
}
