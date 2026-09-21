#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/Creature.h"
#include "Game/World/TestMap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <memory>

using namespace pred;
using namespace pred::TestMapSpec;

// The creature on the real test map, brain and body together, with players that are nothing but
// what its senses are told. Everything the exit criterion for this phase asks for is here: it
// navigates, sees, hears, pursues, investigates, attacks and retreats.
namespace
{

struct CreatureHarness
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;
    NavMesh nav;
    std::unique_ptr<Creature> creature;
    float time = 0.0f;

    explicit CreatureHarness(uint32_t seed = 7, glm::vec3 spawn = {0.0f, 0.0f, 0.0f}, float yaw = 0.0f)
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        BuildTestMap(scene, meshes, &physics);
        REQUIRE(nav.Build(physics.StaticTriangles(), NavSettings{}));
        creature = std::make_unique<Creature>(scene, meshes, physics, &nav, CreatureTraits::FromSeed(seed),
                                              spawn);
        creature->SetShownState(creature->Position(), yaw, 0.0f, 0.0f, true);
    }

    // The same question the game answers: is anything solid between two points. Stopping a little
    // short of the far end, because the far end is usually a body with a shape of its own.
    bool Clear(const glm::vec3& from, const glm::vec3& to) const
    {
        const glm::vec3 along = to - from;
        const float length = glm::length(along);
        if (length < 0.4f)
        {
            return true;
        }
        return !physics.RayCast(from, along / length, length - 0.35f, creature->Body());
    }

    CreatureSenses Senses(const std::vector<SensedPlayer>& players, const std::vector<Noise>& noises = {})
    {
        CreatureSenses senses;
        senses.players = players;
        senses.noises = noises;
        senses.clearLine = [this](const glm::vec3& a, const glm::vec3& b) { return Clear(a, b); };
        return senses;
    }

    // Runs for a while at the game's tick. The noises are made on the first tick only; `watch` sees
    // every tick, so a test can notice a moment that does not last -- a strike -- as it happens.
    template <typename Watch>
    void Run(float seconds, const std::vector<SensedPlayer>& players, std::vector<Noise> noises,
             Watch&& watch)
    {
        constexpr float dt = 1.0f / 60.0f;
        for (float t = 0.0f; t < seconds; t += dt)
        {
            time += dt;
            creature->Update(Senses(players, noises), time, dt);
            noises.clear();
            watch(*creature);
        }
    }
    void Run(float seconds, const std::vector<SensedPlayer>& players, std::vector<Noise> noises = {})
    {
        Run(seconds, players, std::move(noises), [](const Creature&) {});
    }

    const CreatureBrain::Track* TrackOf(int id) const
    {
        for (const CreatureBrain::Track& track : creature->Brain().Tracks())
        {
            if (track.id == id)
            {
                return &track;
            }
        }
        return nullptr;
    }
};

SensedPlayer Somebody(int id, const glm::vec3& feet, float height = 1.8f, float light = 1.0f)
{
    SensedPlayer player;
    player.id = id;
    player.name = "player" + std::to_string(id);
    player.feet = feet;
    player.height = height;
    player.light = light;
    return player;
}

// A place in open ground with a clear view a given distance ahead of a creature facing -Z. Found
// rather than assumed, because the plaza has things standing in it.
bool OpenView(CreatureHarness& harness, float distance, glm::vec3& creatureAt, glm::vec3& playerAt)
{
    for (float x = -6.0f; x <= 6.0f; x += 1.5f)
    {
        for (float z = 6.0f; z >= -2.0f; z -= 1.0f)
        {
            glm::vec3 start;
            glm::vec3 end;
            if (!harness.nav.NearestPoint({x, 0.0f, z}, 0.5f, start) ||
                !harness.nav.NearestPoint({x, 0.0f, z - distance}, 0.5f, end))
            {
                continue;
            }
            if (std::abs(end.z - (z - distance)) > 0.3f || std::abs(start.y - end.y) > 0.1f)
            {
                continue;
            }
            harness.creature->SetShownState(start, 0.0f, 0.0f, 0.0f, true);
            bool clear = true;
            for (const float at : {0.3f, 0.6f, 0.92f})
            {
                clear = clear && harness.Clear(harness.creature->Eye(), end + glm::vec3(0.0f, 1.8f * at, 0.0f));
            }
            if (clear && harness.nav.StraightWalk(start, end))
            {
                creatureAt = start;
                playerAt = end;
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("A creature's temperament comes from its seed", "[creature]")
{
    const CreatureTraits a = CreatureTraits::FromSeed(42);
    const CreatureTraits b = CreatureTraits::FromSeed(42);
    CHECK(a.aggression == b.aggression);
    CHECK(a.fear == b.fear);
    CHECK(a.curiosity == b.curiosity);
    CHECK(a.persistence == b.persistence);
    CHECK(a.perception == b.perception);
    CHECK(a.runSpeed == b.runSpeed);

    // And different seeds are different animals.
    const CreatureTraits c = CreatureTraits::FromSeed(43);
    CHECK((c.aggression != a.aggression || c.fear != a.fear || c.curiosity != a.curiosity));

    // Every trait inside its range, across a spread of seeds.
    for (uint32_t seed = 0; seed < 200; ++seed)
    {
        const CreatureTraits t = CreatureTraits::FromSeed(seed);
        CHECK(t.aggression >= 0.25f);
        CHECK(t.aggression <= 0.95f);
        CHECK(t.runSpeed >= 4.4f);
        CHECK(t.runSpeed <= 6.0f);
    }
}

TEST_CASE("A glimpse is not a sighting; a steady look is", "[creature][senses]")
{
    CreatureHarness harness;
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 8.0f, at, player));
    const std::vector<SensedPlayer> players{Somebody(1, player)};

    harness.Run(0.15f, players);
    const CreatureBrain::Track* track = harness.TrackOf(1);
    REQUIRE(track != nullptr);
    INFO("exposure after a glimpse " << track->exposure);
    CHECK_FALSE(track->visible);
    CHECK(track->exposure > 0.0f);

    harness.Run(1.5f, players);
    track = harness.TrackOf(1);
    INFO("exposure after a steady look " << track->exposure);
    CHECK(track->visible);
    // Hunting them, or already on them: 8 m is a second and a half at a run.
    const Behavior now = harness.creature->Brain().Current();
    CHECK((now == Behavior::Hunt || now == Behavior::Attack));
}

TEST_CASE("It does not see through walls", "[creature][senses]")
{
    // Inside the dark room facing its back wall, with somebody standing in the open behind it.
    const glm::vec3 inside{kDarkRoomX, 0.06f, kDarkRoomZ};
    CreatureHarness harness(7, inside, glm::radians(-90.0f)); // yaw -90: facing -X, the back wall
    const glm::vec3 behindWall{kDarkRoomX - kDarkRoomWidth * 0.5f - 3.0f, 0.0f, kDarkRoomZ};
    REQUIRE_FALSE(harness.Clear(harness.creature->Eye(), behindWall + glm::vec3(0.0f, 1.1f, 0.0f)));

    harness.Run(3.0f, {Somebody(1, behindWall)});
    const CreatureBrain::Track* track = harness.TrackOf(1);
    REQUIRE(track != nullptr);
    CHECK_FALSE(track->visible);
    CHECK(track->exposure == 0.0f);
}

TEST_CASE("Crouched in the dark is harder to see than standing in the light", "[creature][senses]")
{
    float lit = 0.0f;
    float dim = 0.0f;
    for (const bool dark : {false, true})
    {
        CreatureHarness harness;
        glm::vec3 at;
        glm::vec3 player;
        REQUIRE(OpenView(harness, 10.0f, at, player));
        harness.Run(0.2f, {Somebody(1, player, dark ? 1.1f : 1.8f, dark ? 0.15f : 1.0f)});
        (dark ? dim : lit) = harness.TrackOf(1)->exposure;
    }
    INFO("exposure after 0.2 s: standing in the light " << lit << ", crouched in the dark " << dim);
    CHECK(dim < lit * 0.25f);
}

TEST_CASE("A gunshot draws it to look", "[creature][hearing]")
{
    // Across temperaments: a gunshot is the loudest thing that happens, and even an incurious
    // creature goes to see.
    const uint32_t seed = GENERATE(1u, 5u, 7u, 23u, 42u, 99u);
    INFO("seed " << seed << ": " << CreatureTraits::FromSeed(seed).Describe());
    CreatureHarness harness(seed);
    const glm::vec3 start = harness.creature->Position();
    // Somewhere it cannot see, far enough that it has to walk.
    Noise shot;
    shot.kind = NoiseKind::Gunshot;
    shot.reach = NoiseReach::kGunshot;
    shot.position = {kDarkRoomX, 0.5f, kDarkRoomZ};
    const float before = glm::distance(start, shot.position);

    harness.Run(0.3f, {}, {shot});
    std::string mind;
    for (const CreatureBrain::TimelineEntry& entry : harness.creature->Brain().Timeline())
    {
        mind += "\n  " + std::to_string(entry.time).substr(0, 5) + "  " + entry.what;
    }
    for (const CreatureBrain::Option& option : harness.creature->Brain().Options())
    {
        mind += "\n  option " + option.label + " " + std::to_string(option.score).substr(0, 5);
    }
    INFO("its mind:" << mind);
    CHECK(harness.creature->Brain().Current() == Behavior::Investigate);
    CHECK_FALSE(harness.creature->Brain().Interest().resolved);

    harness.Run(5.0f, {});
    const float after = glm::distance(harness.creature->Position(), shot.position);
    INFO("from " << before << " m to " << after << " m away from the shot");
    CHECK(after < before - 4.0f);
}

TEST_CASE("Shot in the back, it goes for the shooter rather than the sound", "[creature][hearing]")
{
    // What happened the first time it was shot in the game: hurt by somebody behind it, it heard the
    // gunshot as well, and went to "investigate the gunshot" at a walk -- the sound of the person
    // it already knew had shot it, and was already scoring a hunt for.
    //
    // And the same when the first round missed, which is what the second time in the game was: the
    // miss is a gunshot from nobody in particular and rightly starts it looking, and the hit that
    // follows says who that was -- so the looking is over.
    const uint32_t seed = GENERATE(1u, 5u, 7u, 23u, 42u, 99u);
    const bool missedFirst = GENERATE(false, true);
    INFO("seed " << seed << ": " << CreatureTraits::FromSeed(seed).Describe()
                 << (missedFirst ? ", first round missed" : ""));
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 8.0f, at, player));
    // Turned round, so the shooter is behind it and it has not seen them.
    harness.creature->SetShownState(at, glm::pi<float>(), 0.0f, 0.0f, true);
    const std::vector<SensedPlayer> players{Somebody(1, player)};
    harness.Run(0.3f, players);
    REQUIRE(harness.TrackOf(1)->exposure == 0.0f);

    const glm::vec3 muzzle = player + glm::vec3(0.0f, 1.5f, 0.0f);
    Noise shot;
    shot.kind = NoiseKind::Gunshot;
    shot.reach = NoiseReach::kGunshot;
    shot.position = muzzle;
    shot.player = 1;
    if (missedFirst)
    {
        harness.Run(0.25f, players, {shot});
    }
    harness.creature->TakeDamage(21.0f, 1, muzzle, harness.time);

    // Given a decision's worth of time to change its mind, since after a miss it was, rightly,
    // already on its way to look.
    const float hitAt = harness.time;
    bool investigated = false;
    harness.Run(1.0f, players, {shot},
                [&](const Creature& creature)
                {
                    investigated = investigated || (harness.time > hitAt + 0.25f &&
                                                    creature.Brain().Current() == Behavior::Investigate);
                });
    std::string mind;
    for (const CreatureBrain::TimelineEntry& entry : harness.creature->Brain().Timeline())
    {
        mind += "\n  " + std::to_string(entry.time).substr(0, 5) + "  " + entry.what;
    }
    INFO("its mind:" << mind);
    CHECK_FALSE(investigated);
    const Behavior now = harness.creature->Brain().Current();
    CHECK((now == Behavior::Hunt || now == Behavior::Attack || now == Behavior::Retreat));
}

TEST_CASE("It hunts somebody it sees and strikes when it reaches them", "[creature][attack]")
{
    const uint32_t seed = GENERATE(1u, 5u, 11u, 23u, 42u, 99u);
    INFO("seed " << seed << ": " << CreatureTraits::FromSeed(seed).Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 7.0f, at, player));
    const std::vector<SensedPlayer> players{Somebody(1, player)};

    bool hunted = false;
    bool attacked = false;
    bool struck = false;
    float closest = 1.0e9f;
    harness.Run(10.0f, players, {},
                [&](const Creature& creature)
                {
                    hunted = hunted || creature.Brain().Current() == Behavior::Hunt;
                    attacked = attacked || creature.Brain().Current() == Behavior::Attack;
                    struck = struck || creature.Brain().Intent().strikeTarget == 1;
                    closest = std::min(closest, glm::distance(creature.Position(), player));
                });
    INFO("got within " << closest << " m");
    CHECK(hunted);
    CHECK(attacked);
    CHECK(struck);
    CHECK(closest < 2.4f);
}

TEST_CASE("Badly hurt, it gets away from whoever hurt it", "[creature][retreat]")
{
    // A timid one, so a few hits are enough: seed chosen for high fear.
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 500; ++candidate)
    {
        if (CreatureTraits::FromSeed(candidate).fear > 0.75f)
        {
            seed = candidate;
            break;
        }
    }
    REQUIRE(seed != 0);
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 9.0f, at, player));
    const std::vector<SensedPlayer> players{Somebody(1, player)};

    // It sees them, then they shoot it three times.
    harness.Run(1.0f, players);
    for (int i = 0; i < 3; ++i)
    {
        harness.creature->TakeDamage(30.0f, 1, player + glm::vec3(0.0f, 1.5f, 0.0f), harness.time);
        harness.Run(0.2f, players);
    }
    const float before = glm::distance(harness.creature->Position(), player);

    bool retreated = false;
    harness.Run(6.0f, players, {},
                [&](const Creature& creature)
                { retreated = retreated || creature.Brain().Current() == Behavior::Retreat; });
    const float after = glm::distance(harness.creature->Position(), player);
    INFO("fear " << harness.creature->Brain().Feelings().fear << ", from " << before << " m to " << after
                 << " m from the shooter");
    CHECK(retreated);
    CHECK(after > before + 5.0f);

    // And every change of mind is on the timeline with its reason.
    bool logged = false;
    for (const CreatureBrain::TimelineEntry& entry : harness.creature->Brain().Timeline())
    {
        logged = logged || entry.what.find("-> Retreat") != std::string::npos;
    }
    CHECK(logged);
}

TEST_CASE("A creature shown from the host's state keeps up smoothly, and dies there too", "[creature][net]")
{
    // The host's creature hunting somebody, and a second one standing in for a client's copy of it,
    // told where the first is thirty times a second as the wire does.
    CreatureHarness harness(5);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 12.0f, at, player));
    Creature shown(harness.scene, harness.meshes, harness.physics, &harness.nav, CreatureTraits::FromSeed(5), at);
    const std::vector<SensedPlayer> players{Somebody(1, player)};

    constexpr float dt = 1.0f / 60.0f;
    int tick = 0;
    float furthestBehind = 0.0f;
    float biggestStep = 0.0f;
    float fastest = 0.0f;
    glm::vec3 last = shown.Position();
    const auto follow = [&](const Creature& host)
    {
        if (tick++ % 2 == 0)
        {
            shown.Receive(host.Position(), host.Yaw(), host.Speed(), host.Brain().Intent().windup,
                          host.Health() / host.MaxHealth(), host.Alive());
        }
        shown.FollowReceived(dt);
        shown.UpdateVisual(dt);
        fastest = std::max(fastest, host.Speed());
        if (tick > 30)
        {
            furthestBehind = std::max(furthestBehind, glm::distance(shown.Position(), host.Position()));
            biggestStep = std::max(biggestStep, glm::distance(shown.Position(), last));
        }
        last = shown.Position();
    };
    harness.Run(3.0f, players, {}, follow);

    INFO("host creature up to " << fastest << " m/s; the copy was at most " << furthestBehind
                                << " m off and moved at most " << biggestStep << " m in a frame");
    REQUIRE(fastest > 3.0f); // it did run, or this proves nothing
    // Close enough that a round aimed at the copy hits the host's creature: its body is a metre and
    // a half long.
    CHECK(furthestBehind < 0.4f);
    // And no frame jumps further than running for a frame and a bit would carry it.
    CHECK(biggestStep < fastest * dt * 1.6f);

    // Killed on the host, and the copy goes down with it.
    harness.creature->TakeDamage(1000.0f, 1, player, harness.time);
    harness.Run(1.0f, players, {}, follow);
    CHECK_FALSE(shown.Alive());
    CHECK(shown.Health() == 0.0f);
}
