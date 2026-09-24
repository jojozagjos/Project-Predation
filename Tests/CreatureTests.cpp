#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/Creature.h"
#include "Game/World/TestMap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

using namespace pred;
using namespace pred::TestMapSpec;

// The creature on the real test map, brain and body together, with players that are nothing but
// what its senses are told. Everything the exit criterion for this phase asks for is here: it
// navigates, sees, hears, pursues, investigates, attacks and retreats.
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

struct CreatureHarness
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;
    NavMesh nav;
    std::unique_ptr<Creature> creature;
    float time = 0.0f;

    explicit CreatureHarness(uint32_t seed = 7, glm::vec3 spawn = {0.0f, 0.0f, 0.0f}, float yaw = 0.0f)
        : CreatureHarness(Hunter(seed), spawn, yaw)
    {
    }
    explicit CreatureHarness(const CreatureTraits& traits, glm::vec3 spawn = {0.0f, 0.0f, 0.0f}, float yaw = 0.0f)
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        BuildTestMap(scene, meshes, &physics);
        REQUIRE(nav.Build(physics.StaticTriangles(), NavSettings{}));
        creature = std::make_unique<Creature>(scene, meshes, physics, &nav, traits, spawn);
        creature->SetShownState(creature->Position(), yaw, 0.0f, 0.0f, true);
    }

    // The same question the game answers: is anything solid between two points. Stopping a little
    // short of the far end, because the far end is usually a body with a shape of its own.
    //
    // And seeing through creatures, its own body and any other, as the game does: its eyes are inside
    // its own box, and a second creature standing in the same place -- the copy a client is shown --
    // would otherwise blind it.
    bool Clear(const glm::vec3& from, const glm::vec3& to) const
    {
        const glm::vec3 along = to - from;
        const float length = glm::length(along);
        if (length < 0.4f)
        {
            return true;
        }
        const glm::vec3 direction = along / length;
        glm::vec3 start = from;
        float left = length - 0.35f;
        for (int pass = 0; pass < 4 && left > 0.0f; ++pass)
        {
            const RayHit hit = physics.RayCast(start, direction, left, creature->Body());
            if (!hit)
            {
                return true;
            }
            const bool creatureBody = creature->Owns(hit.body) ||
                                      std::any_of(seeThrough.begin(), seeThrough.end(),
                                                  [&](const Creature* other) { return other->Owns(hit.body); });
            if (!creatureBody)
            {
                return false;
            }
            const float past = hit.distance + 0.05f;
            start += direction * past;
            left -= past;
        }
        return true;
    }
    // Other creatures, whose bodies sight passes through, as its own.
    std::vector<const Creature*> seeThrough;

    CreatureSenses Senses(const std::vector<SensedPlayer>& players, const std::vector<Noise>& noises = {})
    {
        CreatureSenses senses;
        senses.players = players;
        senses.noises = noises;
        senses.clearLine = [this](const glm::vec3& a, const glm::vec3& b) { return Clear(a, b); };
        senses.hidingPlaces = places;
        return senses;
    }

    // Lockers, as the game describes them. The test map has none of its own.
    std::vector<HidingPlace> places;

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

    // The same, with the players worked out afresh every tick from where the creature is -- for
    // somebody who turns to keep it in view, as anybody watching something move would.
    template <typename Players, typename Watch>
    void RunLive(float seconds, Players&& players, Watch&& watch)
    {
        constexpr float dt = 1.0f / 60.0f;
        for (float t = 0.0f; t < seconds; t += dt)
        {
            time += dt;
            creature->Update(Senses(players(*creature)), time, dt);
            watch(*creature);
        }
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

// Damage as a share of what this creature can take, after its plates. The tests were written against
// one body with 160 health; a body from a seed can take anything from 700 to 4000, and "a round" or
// "badly hurt" has to mean the same thing to every one of them.
float Share(const Creature& creature, float oldAmount)
{
    return oldAmount / 160.0f * creature.MaxHealth() / std::max(1.0f - creature.Capabilities().armour, 0.1f);
}

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
    // creature goes to see -- straight there if it is brazen, round the side if it has any sense.
    const uint32_t seed = GENERATE(1u, 5u, 7u, 23u, 42u, 99u);
    INFO("seed " << seed << ": " << Hunter(seed).Describe());
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
    const Behavior doing = harness.creature->Brain().Current();
    CHECK((doing == Behavior::Investigate || doing == Behavior::Flank));
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
    INFO("seed " << seed << ": " << Hunter(seed).Describe()
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
    harness.creature->TakeDamage(Share(*harness.creature, 21.0f), 1, muzzle, harness.time);

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
    INFO("seed " << seed << ": " << Hunter(seed).Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 7.0f, at, player));
    const std::vector<SensedPlayer> players{Somebody(1, player)};

    bool hunted = false;
    bool huntedFromAfar = false;
    float nearestEye = 1.0e9f;
    bool attacked = false;
    bool struck = false;
    float closest = 1.0e9f;
    harness.Run(10.0f, players, {},
                [&](const Creature& creature)
                {
                    const bool hunting = creature.Brain().Current() == Behavior::Hunt;
                    hunted = hunted || hunting;
                    nearestEye = std::min(nearestEye, glm::distance(creature.Eye(), player + glm::vec3(0.0f, 1.08f, 0.0f)));
                    huntedFromAfar = huntedFromAfar ||
                                     (hunting && nearestEye > CreatureBrain::CloseSense() + 0.3f);
                    attacked = attacked || creature.Brain().Current() == Behavior::Attack;
                    struck = struck || creature.Brain().Intent().strikeTarget == 1;
                    closest = std::min(closest, glm::distance(creature.Position(), player));
                });
    INFO("got within " << closest << " m");
    if (harness.creature->Anatomy().eyes == 0)
    {
        // A body with no eyes cannot see somebody standing still seven metres away, which is the whole
        // point of it: it has to hear them, or bump into them -- which, wandering, it may.
        CHECK_FALSE(huntedFromAfar);
        return;
    }
    CHECK(hunted);
    CHECK(attacked);
    CHECK(struck);
    // Within its own reach, which its neck and head decide.
    CHECK(closest < harness.creature->Capabilities().strikeReach + 0.2f);
}

TEST_CASE("A creature with no eyes hears what it cannot see", "[creature][senses]")
{
    // Seed 99 is two-legged and eyeless. It does not see somebody standing in plain view, but a
    // footstep from them is enough.
    CreatureHarness harness(99);
    REQUIRE(harness.creature->Anatomy().eyes == 0);
    REQUIRE(harness.creature->Capabilities().hearing > 1.5f);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 7.0f, at, player));
    const std::vector<SensedPlayer> players{Somebody(1, player)};
    // Not seen from where it stands. (It may wander into them, and knows somebody is there when it does.)
    float closest = 1.0e9f;
    harness.Run(3.0f, players, {},
                [&](const Creature& creature)
                { closest = std::min(closest, glm::distance(creature.Eye(), player + glm::vec3(0.0f, 1.08f, 0.0f))); });
    const CreatureBrain::Track* track = harness.TrackOf(1);
    CHECK((track == nullptr || track->lastSeen < 0.0f || closest < CreatureBrain::CloseSense() + 0.5f));

    Noise step;
    step.kind = NoiseKind::Footstep;
    step.position = player;
    step.reach = 9.0f;
    step.player = 1;
    bool heard = false;
    harness.Run(2.0f, players, {step},
                [&](const Creature& creature)
                {
                    heard = heard || creature.Brain().Current() == Behavior::Investigate ||
                            creature.Brain().Current() == Behavior::Hunt;
                });
    CHECK(heard);
}

TEST_CASE("Badly hurt, it gets away from whoever hurt it", "[creature][retreat]")
{
    // A timid one, so a few hits are enough: seed chosen for high fear.
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 500; ++candidate)
    {
        if (Hunter(candidate).fear > 0.75f)
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
        harness.creature->TakeDamage(Share(*harness.creature, 30.0f), 1, player + glm::vec3(0.0f, 1.5f, 0.0f), harness.time);
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
    Creature shown(harness.scene, harness.meshes, harness.physics, &harness.nav, Hunter(5), at);
    harness.seeThrough.push_back(&shown);
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
    harness.creature->TakeDamage(Share(*harness.creature, 1000.0f), 1, player, harness.time);
    harness.Run(1.0f, players, {}, follow);
    CHECK_FALSE(shown.Alive());
    CHECK(shown.Health() == 0.0f);
}

namespace
{

// The first seed whose temperament passes a test, so a test can ask for "a stealthy one" rather than
// for a number that means stealthy only until somebody changes how traits are drawn.
// Only seeds whose bodies have eyes: these tests are about what a creature does with what it sees. An
// eyeless one hunts by sound and has a test of its own.
template <typename Want>
uint32_t SeedWhere(Want&& want)
{
    for (uint32_t seed = 1; seed < 5000; ++seed)
    {
        if (want(Hunter(seed)) && CreatureAnatomy::FromSeed(seed).eyes > 0)
        {
            return seed;
        }
    }
    return 0;
}

// Somebody standing at `feet` and looking at `at`.
SensedPlayer Watching(int id, const glm::vec3& feet, const glm::vec3& at)
{
    SensedPlayer player = Somebody(id, feet);
    const glm::vec3 eye = feet + glm::vec3(0.0f, 1.67f, 0.0f);
    player.forward = glm::normalize(at + glm::vec3(0.0f, 0.7f, 0.0f) - eye);
    return player;
}

std::string MindOf(const Creature& creature)
{
    std::string mind;
    for (const CreatureBrain::TimelineEntry& entry : creature.Brain().Timeline())
    {
        mind += "\n  " + std::to_string(entry.time).substr(0, 5) + "  " + entry.what;
    }
    for (const CreatureBrain::Option& option : creature.Brain().Options())
    {
        mind += "\n  option " + option.label + " " + std::to_string(option.score).substr(0, 5);
    }
    int hidden = 0;
    for (const CreatureBrain::CoverCandidate& candidate : creature.Brain().Cover())
    {
        hidden += candidate.hidden ? 1 : 0;
    }
    mind += "\n  cover weighed: " + std::to_string(creature.Brain().Cover().size()) + ", of which hidden " +
            std::to_string(hidden);
    mind += "\n  goal: " + creature.Brain().CurrentGoal();
    if (creature.Brain().HasCoverPoint())
    {
        const glm::vec3 point = creature.Brain().CoverPoint();
        mind += "\n  cover point " + std::to_string(point.x) + ", " + std::to_string(point.z) + ", " +
                std::to_string(glm::distance(point, creature.Position())) + " m away; at " +
                std::to_string(creature.Position().x) + ", " + std::to_string(creature.Position().z);
    }
    return mind;
}

// Cunning beating timid by a clear margin is what makes lying still win over running.
uint32_t CunningSeed()
{
    return SeedWhere(
        [](const CreatureTraits& t)
        {
            const float cunning = 0.3f + 0.45f * t.stealth + 0.45f * t.patience;
            const float timid = 0.6f + 0.6f * t.fear;
            return cunning > timid + 0.15f && t.aggression > 0.5f;
        });
}

} // namespace

TEST_CASE("Dead is dead: its mind stops, and a strike it was winding up never lands", "[creature][death]")
{
    CreatureHarness harness(5);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 1.8f, at, player));
    const std::vector<SensedPlayer> players{Somebody(1, player)};

    // Until it is part way through a wind-up: the moment a strike is on its way.
    bool windingUp = false;
    for (int i = 0; i < 600 && !windingUp; ++i)
    {
        harness.Run(1.0f / 60.0f, players);
        windingUp = harness.creature->Brain().Intent().windup > 0.3f;
    }
    REQUIRE(windingUp);

    harness.creature->TakeDamage(Share(*harness.creature, 1000.0f), 1, player, harness.time);
    REQUIRE_FALSE(harness.creature->Alive());
    CHECK(harness.creature->Brain().Dead());
    const size_t timeline = harness.creature->Brain().Timeline().size();
    const glm::vec3 where = harness.creature->Position();
    const Behavior doing = harness.creature->Brain().Current();

    bool struck = false;
    bool moved = false;
    Noise shot;
    shot.kind = NoiseKind::Gunshot;
    shot.reach = NoiseReach::kGunshot;
    shot.position = player;
    shot.player = 1;
    harness.Run(3.0f, players, {shot},
                [&](const Creature& creature)
                {
                    struck = struck || creature.Brain().Intent().strikeTarget >= 0;
                    moved = moved || creature.Brain().Intent().move;
                });
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK_FALSE(struck);
    CHECK_FALSE(moved);
    CHECK(harness.creature->Brain().Timeline().size() == timeline);
    CHECK(harness.creature->Brain().Current() == doing);
    CHECK(glm::distance(harness.creature->Position(), where) < 1e-4f);
    CHECK(harness.creature->Down());
}

TEST_CASE("A stealthy creature being watched stalks from cover, and comes when they look away",
          "[creature][stalk]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t)
                                    { return t.stealth > 0.85f && t.aggression > 0.5f && t.fear < 0.6f; });
    REQUIRE(seed != 0);
    INFO("seed " << seed << ": " << Hunter(seed).Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 14.0f, at, player));

    // Somebody standing in the open looking straight at it.
    bool charged = false;
    bool stalked = false;
    harness.RunLive(8.0f, [&](const Creature& c) { return std::vector<SensedPlayer>{Watching(1, player, c.Position())}; },
                [&](const Creature& creature)
                {
                    const Behavior now = creature.Brain().Current();
                    charged = charged || now == Behavior::Hunt || now == Behavior::Attack;
                    stalked = stalked || now == Behavior::Stalk;
                });
    const glm::vec3 eye = player + glm::vec3(0.0f, 1.67f, 0.0f);
    const bool inTheirSight =
        harness.Clear(eye, harness.creature->Position() + glm::vec3(0.0f, harness.creature->Brain().Traits().bodyMiddle, 0.0f));
    INFO("its mind:" << MindOf(*harness.creature));
    INFO("ended " << glm::distance(harness.creature->Position(), player) << " m from them, "
                  << (inTheirSight ? "in their sight" : "out of their sight"));
    CHECK(stalked);
    CHECK_FALSE(charged);
    CHECK_FALSE(inTheirSight);
    CHECK(harness.creature->Brain().Intent().crouch > 0.5f);

    // They turn their back.
    SensedPlayer away = Somebody(1, player);
    away.forward = glm::normalize(player - at);
    bool came = false;
    harness.Run(6.0f, {away}, {},
                [&](const Creature& creature)
                {
                    const Behavior now = creature.Brain().Current();
                    came = came || now == Behavior::Hunt || now == Behavior::Attack;
                });
    INFO("after they looked away:" << MindOf(*harness.creature));
    CHECK(came);
}

TEST_CASE("A brazen creature comes whether it is watched or not", "[creature][stalk]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t)
                                    { return t.stealth < 0.2f && t.aggression > 0.5f && t.fear < 0.6f; });
    REQUIRE(seed != 0);
    INFO("seed " << seed << ": " << Hunter(seed).Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 12.0f, at, player));
    bool came = false;
    harness.RunLive(3.0f, [&](const Creature& c) { return std::vector<SensedPlayer>{Watching(1, player, c.Position())}; },
                [&](const Creature& creature)
                {
                    const Behavior now = creature.Brain().Current();
                    came = came || now == Behavior::Hunt || now == Behavior::Attack;
                });
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK(came);
}

TEST_CASE("A stalker's patience runs out, and then any moment will do", "[creature][stalk]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t)
                                    { return t.stealth > 0.8f && t.patience < 0.3f && t.aggression > 0.5f &&
                                             t.fear < 0.6f; });
    REQUIRE(seed != 0);
    const CreatureTraits traits = Hunter(seed);
    INFO("seed " << seed << ": " << traits.Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 14.0f, at, player));

    // Watched the whole time: it never gets the opening it wants.
    float firstCame = -1.0f;
    harness.RunLive(traits.StalkPatienceSeconds() + 8.0f, [&](const Creature& c) { return std::vector<SensedPlayer>{Watching(1, player, c.Position())}; },
                [&](const Creature& creature)
                {
                    const Behavior now = creature.Brain().Current();
                    if (firstCame < 0.0f && (now == Behavior::Hunt || now == Behavior::Attack))
                    {
                        firstCame = harness.time;
                    }
                });
    INFO("its mind:" << MindOf(*harness.creature));
    INFO("patience " << traits.StalkPatienceSeconds() << " s; first came at " << firstCame << " s");
    REQUIRE(firstCame > 0.0f);
    // Not straight away -- it waited -- and not long after its patience was spent.
    CHECK(firstCame > 3.0f);
    CHECK(firstCame < traits.StalkPatienceSeconds() + 8.0f);
}

TEST_CASE("Badly hurt, a cunning creature plays dead -- alive underneath -- and springs when they come close",
          "[creature][playdead]")
{
    const uint32_t seed = CunningSeed();
    REQUIRE(seed != 0);
    INFO("seed " << seed << ": " << Hunter(seed).Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 6.0f, at, player));
    harness.Run(1.0f, {Somebody(1, player)});

    harness.creature->TakeDamage(Share(*harness.creature, 85.0f), 1, player + glm::vec3(0.0f, 1.5f, 0.0f), harness.time);
    harness.Run(0.5f, {Somebody(1, player)});
    INFO("its mind:" << MindOf(*harness.creature));
    REQUIRE(harness.creature->Brain().Current() == Behavior::PlayDead);
    CHECK(harness.creature->Alive());
    CHECK(harness.creature->Down());
    CHECK_FALSE(harness.creature->Brain().Dead());

    // Lying there while they stand off watching: it stays down.
    harness.Run(3.0f, {Watching(1, player, at)});
    CHECK(harness.creature->Brain().Current() == Behavior::PlayDead);

    // They walk up to it.
    const glm::vec3 close =
        harness.creature->Position() + glm::normalize(player - harness.creature->Position()) * 1.6f;
    bool sprang = false;
    bool struck = false;
    harness.Run(3.0f, {Watching(1, close, harness.creature->Position())}, {},
                [&](const Creature& creature)
                {
                    sprang = sprang || creature.Brain().Current() == Behavior::Attack;
                    struck = struck || creature.Brain().Intent().strikeTarget == 1;
                });
    INFO("after they came close:" << MindOf(*harness.creature));
    CHECK(sprang);
    CHECK(struck);
    CHECK_FALSE(harness.creature->Down());
}

TEST_CASE("Shot while playing dead, it gives up the act and runs", "[creature][playdead]")
{
    const uint32_t seed = CunningSeed();
    REQUIRE(seed != 0);
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 7.0f, at, player));
    harness.Run(1.0f, {Somebody(1, player)});
    harness.creature->TakeDamage(Share(*harness.creature, 85.0f), 1, player + glm::vec3(0.0f, 1.5f, 0.0f), harness.time);
    harness.Run(0.5f, {Somebody(1, player)});
    REQUIRE(harness.creature->Brain().Current() == Behavior::PlayDead);

    harness.creature->TakeDamage(Share(*harness.creature, 10.0f), 1, player + glm::vec3(0.0f, 1.5f, 0.0f), harness.time);
    harness.Run(0.5f, {Watching(1, player, at)});
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK(harness.creature->Brain().Current() == Behavior::Retreat);
    CHECK(harness.creature->Alive());
}

TEST_CASE("The same seed in the same situation makes the same creature, decision for decision",
          "[creature][seed]")
{
    // The exit criterion for Milestone 9: reproducible from seed. Two creatures from one seed, given
    // the same world and the same things happening, think the same thoughts at the same moments and
    // end up in the same place.
    const uint32_t seed = GENERATE(3u, 17u, 256u);
    std::vector<std::string> minds;
    std::vector<glm::vec3> ends;
    for (int run = 0; run < 2; ++run)
    {
        CreatureHarness harness(seed);
        glm::vec3 at;
        glm::vec3 player;
        REQUIRE(OpenView(harness, 12.0f, at, player));
        Noise shot;
        shot.kind = NoiseKind::Gunshot;
        shot.reach = NoiseReach::kGunshot;
        shot.position = player + glm::vec3(4.0f, 0.5f, -3.0f);
        harness.Run(3.0f, {}, {shot});
        harness.Run(6.0f, {Watching(1, player, at)});
        harness.creature->TakeDamage(Share(*harness.creature, 40.0f), 1, player, harness.time);
        harness.Run(4.0f, {Somebody(1, player)});
        std::string mind;
        for (const CreatureBrain::TimelineEntry& entry : harness.creature->Brain().Timeline())
        {
            mind += std::to_string(entry.time) + " " + entry.what + "\n";
        }
        minds.push_back(mind);
        ends.push_back(harness.creature->Position());
    }
    INFO("seed " << seed << "\nfirst run:\n" << minds[0] << "\nsecond run:\n" << minds[1]);
    CHECK(minds[0] == minds[1]);
    CHECK(glm::distance(ends[0], ends[1]) < 1e-4f);
    CHECK(minds[0].size() > 20); // it did something worth comparing
}

namespace
{

// Distance across the ground, ignoring height.
float Horizontal(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(glm::vec2(b.x - a.x, b.z - a.z));
}

// A locker standing on open ground at `front`, its back to the direction `away` points. What the game
// hands the brain for a real one.
HidingPlace LockerAt(const glm::vec3& front, const glm::vec3& away, bool shut)
{
    HidingPlace place;
    place.front = front;
    place.inside = front + glm::normalize(glm::vec3(away.x, 0.0f, away.z)) * 1.4f;
    place.shut = shut;
    return place;
}

// Somebody inside locker `index`: out of sight, where the locker is.
SensedPlayer InLocker(int id, const HidingPlace& place, int index)
{
    SensedPlayer player = Somebody(id, place.inside);
    player.hidden = true;
    player.hidingPlace = index;
    return player;
}

} // namespace

TEST_CASE("It loses somebody, searches where they could have gone, and in the end gives up", "[creature][search]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t)
                                    { return t.stealth < 0.4f && t.aggression > 0.5f && t.fear < 0.6f; });
    REQUIRE(seed != 0);
    INFO("seed " << seed << ": " << Hunter(seed).Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 12.0f, at, player));

    // Seen, then gone -- somewhere far out of sight, nowhere it could know about.
    harness.Run(1.5f, {Somebody(1, player)});
    REQUIRE(harness.TrackOf(1)->lastSeen >= 0.0f);
    const std::vector<SensedPlayer> gone{Somebody(1, glm::vec3(40.0f, 0.0f, 40.0f))};

    bool searched = false;
    size_t furthestStep = 0;
    bool gaveUp = false;
    harness.Run(60.0f, gone, {},
                [&](const Creature& creature)
                {
                    if (creature.Brain().Current() == Behavior::Search)
                    {
                        searched = true;
                        furthestStep = std::max(furthestStep, creature.Brain().SearchStep());
                    }
                    else if (searched && creature.Brain().Current() == Behavior::Roam)
                    {
                        gaveUp = true;
                    }
                });
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK(searched);
    CHECK(furthestStep >= 2); // went through more than one place, not only where they were last
    CHECK(gaveUp);
}

TEST_CASE("It hears a locker door, goes to it, and pulls out whoever is inside", "[creature][search][locker]")
{
    CreatureHarness harness(5);
    glm::vec3 at;
    glm::vec3 near;
    REQUIRE(OpenView(harness, 7.0f, at, near));
    harness.places = {LockerAt(near, near - at, true)};

    bool pulledOut = false;
    bool opened = false;
    bool wentForThem = false;
    Noise slam;
    slam.kind = NoiseKind::Door;
    slam.reach = NoiseReach::kDoor * 0.7f;
    slam.position = harness.places[0].inside;
    slam.player = 1;
    // Somebody it never saw, getting into a locker it can hear. Once the door is pulled open they are
    // out, standing in front of it, as the game would leave them.
    harness.Run(0.1f, {InLocker(1, harness.places[0], 0)}, {slam});
    harness.RunLive(
        12.0f,
        [&](const Creature&)
        {
            return pulledOut ? std::vector<SensedPlayer>{Somebody(1, harness.places[0].front)}
                             : std::vector<SensedPlayer>{InLocker(1, harness.places[0], 0)};
        },
        [&](const Creature& creature)
        {
            if (creature.Brain().Intent().openHidingPlace == 0)
            {
                opened = true;
                pulledOut = true;
            }
            wentForThem = wentForThem || (pulledOut && creature.Brain().Current() == Behavior::Attack &&
                                          creature.Brain().CurrentTarget() == 1);
        });
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK(opened);
    CHECK(wentForThem);
    CHECK(harness.creature->Brain().FoundHiding() == 1);
}

TEST_CASE("Seen stepping up to a locker and vanishing, it checks that locker first", "[creature][search][locker]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t)
                                    { return t.stealth < 0.4f && t.aggression > 0.5f && t.fear < 0.6f; });
    REQUIRE(seed != 0);
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 12.0f, at, player));
    harness.places = {LockerAt(player, player - at, false)};

    // In plain sight, standing at the locker; then inside it, and the door shut.
    harness.Run(1.2f, {Somebody(1, player)});
    harness.places[0].shut = true;
    int firstOpened = -1;
    bool pulledOut = false;
    harness.RunLive(
        20.0f,
        [&](const Creature&)
        {
            return pulledOut ? std::vector<SensedPlayer>{Somebody(1, harness.places[0].front)}
                             : std::vector<SensedPlayer>{InLocker(1, harness.places[0], 0)};
        },
        [&](const Creature& creature)
        {
            if (firstOpened < 0 && creature.Brain().Intent().openHidingPlace >= 0)
            {
                firstOpened = creature.Brain().Intent().openHidingPlace;
                pulledOut = true;
            }
        });
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK(firstOpened == 0);
    CHECK(harness.creature->Brain().FoundHiding() == 1);
}

TEST_CASE("A shut locker means little to it -- until it has found somebody in one", "[creature][locker][learn]")
{
    // Two lockers in view, both shut. Before it has ever found anybody hiding, a shut door is barely
    // worth a thought; after pulling somebody out of one, every shut door is a question.
    CreatureHarness harness(5);
    glm::vec3 at;
    glm::vec3 near;
    REQUIRE(OpenView(harness, 7.0f, at, near));
    const glm::vec3 across{near.z - at.z, 0.0f, at.x - near.x}; // sideways from the line between them
    glm::vec3 other;
    REQUIRE(harness.nav.NearestPoint(near + glm::normalize(across) * 3.0f, 1.0f, other));
    harness.places = {LockerAt(near, near - at, true), LockerAt(other, near - at, true)};

    harness.Run(1.0f, {});
    const float before = harness.creature->Brain().Places()[1].suspicion;
    CHECK(harness.creature->Brain().ShutMeansSomebody() == 0.0f);

    // A slam at the first; it finds somebody in it.
    Noise slam;
    slam.kind = NoiseKind::Door;
    slam.reach = NoiseReach::kDoor * 0.7f;
    slam.position = harness.places[0].inside;
    slam.player = 1;
    bool pulledOut = false;
    harness.Run(0.1f, {InLocker(1, harness.places[0], 0)}, {slam});
    harness.RunLive(
        12.0f,
        [&](const Creature&)
        {
            return pulledOut ? std::vector<SensedPlayer>{Somebody(1, harness.places[0].front)}
                             : std::vector<SensedPlayer>{InLocker(1, harness.places[0], 0)};
        },
        [&](const Creature& creature) { pulledOut = pulledOut || creature.Brain().Intent().openHidingPlace == 0; });
    REQUIRE(harness.creature->Brain().FoundHiding() == 1);
    harness.Run(0.5f, {});
    const float after = harness.creature->Brain().Places()[1].suspicion;
    INFO("the other shut locker: suspicion " << before << " before, " << after << " after; a shut door means "
                                             << harness.creature->Brain().ShutMeansSomebody());
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK(before < 0.2f);
    CHECK(after > 0.5f);
}

TEST_CASE("A curious, gentle creature watches from a distance, gives ground, and in time gets bored",
          "[creature][curiosity]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t)
                                    { return t.curiosity > 0.8f && t.aggression < 0.4f && t.fear < 0.6f; });
    REQUIRE(seed != 0);
    const CreatureTraits traits = Hunter(seed);
    INFO("seed " << seed << ": " << traits.Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 14.0f, at, player));

    // Somebody standing in the open. It comes to look, and it does not come for them.
    bool watched = false;
    bool attacked = false;
    harness.Run(10.0f, {Somebody(1, player)}, {},
                [&](const Creature& creature)
                {
                    watched = watched || creature.Brain().Current() == Behavior::Observe;
                    attacked = attacked || creature.Brain().Current() == Behavior::Attack ||
                               creature.Brain().Current() == Behavior::Hunt;
                });
    const float watching = Horizontal(harness.creature->Position(), player);
    INFO("its mind:" << MindOf(*harness.creature));
    INFO("watching from " << watching << " m");
    CHECK(watched);
    CHECK_FALSE(attacked);
    CHECK(watching > 4.0f);
    CHECK(watching < 12.0f);

    // They walk right up to it: it gives ground rather than meeting them.
    glm::vec3 toward = harness.creature->Position() - player;
    toward.y = 0.0f;
    glm::vec3 close;
    REQUIRE(harness.nav.NearestPoint(harness.creature->Position() - glm::normalize(toward) * 2.5f, 1.0f, close));
    harness.Run(2.5f, {Somebody(1, close)});
    const float afterApproach = Horizontal(harness.creature->Position(), close);
    INFO("walked up to within 2.5 m; two and a half seconds later it was " << afterApproach << " m off");
    INFO("after they walked up:" << MindOf(*harness.creature));
    CHECK(afterApproach > 3.5f);

    // And in the end it has seen enough.
    bool stoppedWatching = false;
    harness.Run(traits.BoredomSeconds() + 5.0f, {Somebody(1, close)}, {},
                [&](const Creature& creature)
                { stoppedWatching = stoppedWatching || creature.Brain().Current() != Behavior::Observe; });
    CHECK(stoppedWatching);
}

TEST_CASE("It remembers where it found people, prowls back there, and forgets in time", "[creature][memory]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t)
                                    { return t.stealth < 0.4f && t.aggression > 0.5f && t.fear < 0.6f; });
    REQUIRE(seed != 0);
    INFO("seed " << seed << ": " << Hunter(seed).Describe());
    CreatureHarness harness(seed);
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 14.0f, at, player));

    const auto heatAt = [&](const glm::vec3& where)
    {
        const int x = static_cast<int>(std::floor(where.x / CreatureBrain::kHeatCell));
        const int z = static_cast<int>(std::floor(where.z / CreatureBrain::kHeatCell));
        for (const CreatureBrain::HeatCell& cell : harness.creature->Brain().Heat())
        {
            if (cell.x == x && cell.z == z)
            {
                return cell.heat;
            }
        }
        return 0.0f;
    };

    // Somebody in view for a few seconds, then gone somewhere it cannot know about.
    harness.Run(3.0f, {Somebody(1, player)});
    const float warmth = heatAt(player);
    INFO("warmth where they stood: " << warmth);
    CHECK(warmth > 1.0f);

    // Searched for and given up on, it goes back to prowling -- and some of that is back where they were.
    const std::vector<SensedPlayer> gone{Somebody(1, glm::vec3(40.0f, 0.0f, 40.0f))};
    int prowls = 0;
    bool prowledThere = false;
    glm::vec3 lastDestination{1.0e9f};
    harness.Run(90.0f, gone, {},
                [&](const Creature& creature)
                {
                    const CreatureIntent& intent = creature.Brain().Intent();
                    if (creature.Brain().Current() == Behavior::Roam && intent.move &&
                        creature.Brain().CurrentGoal() == "prowling where people go" &&
                        glm::distance(intent.destination, lastDestination) > 0.5f)
                    {
                        lastDestination = intent.destination;
                        ++prowls;
                        prowledThere = prowledThere || Horizontal(intent.destination, player) < 6.0f;
                    }
                });
    INFO("its mind:" << MindOf(*harness.creature));
    INFO(prowls << " prowls");
    CHECK(prowledThere);

    // Left alone long enough, the memory fades.
    harness.Run(400.0f, gone);
    CHECK(heatAt(player) < warmth * 0.25f);
}

TEST_CASE("Eight creatures at once keep out of each other, all keep thinking, and cost little",
          "[creature][pack]")
{
    // The most there can be, all made on one spot, all given the same person to go for.
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
    glm::vec3 spot;
    glm::vec3 player;
    REQUIRE(nav.NearestPoint({-6.0f, 0.0f, 5.0f}, 1.0f, spot));
    REQUIRE(nav.NearestPoint({-6.0f, 0.0f, -7.0f}, 1.0f, player));

    std::vector<std::unique_ptr<Creature>> pack;
    for (uint8_t i = 0; i < 8; ++i)
    {
        pack.push_back(std::make_unique<Creature>(scene, meshes, physics, &nav, Hunter(100u + i), spot));
        pack.back()->SetNetId(i);
    }

    const std::vector<SensedPlayer> players{Somebody(1, player)};
    constexpr float dt = 1.0f / 60.0f;
    float time = 0.0f;
    double spent = 0.0;
    int ticks = 0;
    for (; time < 6.0f; time += dt, ++ticks)
    {
        const auto start = std::chrono::steady_clock::now();
        for (const std::unique_ptr<Creature>& creature : pack)
        {
            CreatureSenses senses;
            senses.players = players;
            senses.clearLine = [&physics, &pack](const glm::vec3& a, const glm::vec3& b)
            {
                // Through creatures, as the game sees: only the level blocks sight.
                const glm::vec3 along = b - a;
                const float length = glm::length(along);
                if (length < 0.4f)
                {
                    return true;
                }
                glm::vec3 start = a;
                float left = length - 0.35f;
                for (int pass = 0; pass < 8 && left > 0.0f; ++pass)
                {
                    const RayHit hit = physics.RayCast(start, along / length, left);
                    if (!hit)
                    {
                        return true;
                    }
                    const bool creatureBody = std::any_of(pack.begin(), pack.end(), [&](const std::unique_ptr<Creature>& c)
                                                          { return c->Owns(hit.body); });
                    if (!creatureBody)
                    {
                        return false;
                    }
                    start += along / length * (hit.distance + 0.05f);
                    left -= hit.distance + 0.05f;
                }
                return true;
            };
            for (const std::unique_ptr<Creature>& other : pack)
            {
                if (other.get() != creature.get())
                {
                    senses.others.push_back(other->Position());
                }
            }
            creature->Update(std::move(senses), time, dt);
        }
        spent += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    float closest = 1.0e9f;
    int moved = 0;
    int noticed = 0;
    for (size_t i = 0; i < pack.size(); ++i)
    {
        moved += Horizontal(pack[i]->Position(), spot) > 1.0f ? 1 : 0;
        noticed += pack[i]->Brain().Current() != Behavior::Roam ? 1 : 0;
        for (size_t j = i + 1; j < pack.size(); ++j)
        {
            closest = std::min(closest, Horizontal(pack[i]->Position(), pack[j]->Position()));
        }
    }
    const double perTick = spent / static_cast<double>(ticks);
    INFO("closest pair " << closest << " m apart; " << moved << " moved off the spot; " << noticed
                         << " doing something about the player; " << perTick << " ms a tick for all eight");
    CHECK(closest > 1.0f);
    CHECK(moved == 8);
    // Not all of them: packed together, some cannot see past the others' bodies, which is right.
    CHECK(noticed >= 4);
    // A frame at sixty is 16.7 ms. Eight creatures thinking should be a small part of it.
    CHECK(perTick < 3.0);
}

TEST_CASE("A creature that ran away comes back out rather than running for ever", "[creature][retreat]")
{
    // Reported as "they go retreat and get stuck retreating". Its fear was made partly of its wound,
    // and nothing mends a creature without a nest, so a timid one shot badly enough was afraid for
    // the rest of the match and chose running every time it was asked.
    uint32_t seed = 0;
    for (uint32_t candidate = 1; candidate < 500; ++candidate)
    {
        // Very timid, and not the sort to lie down and pretend instead.
        const CreatureTraits traits = Hunter(candidate);
        if (traits.fear > 0.85f && traits.stealth < 0.5f && traits.patience < 0.5f)
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
    harness.Run(1.0f, players);
    // Down to an eighth of its health: hurt enough that the wound alone used to keep it afraid.
    for (int i = 0; i < 4; ++i)
    {
        harness.creature->TakeDamage(Share(*harness.creature, 35.0f), 1, player + glm::vec3(0.0f, 1.5f, 0.0f),
                                     harness.time);
        harness.Run(0.2f, players);
    }
    harness.Run(4.0f, players);
    REQUIRE(harness.creature->Brain().Current() == Behavior::Retreat);

    // Then the shooter goes. Given the rest of a couple of minutes on its own it does something other
    // than hide -- still hurt: with no nest, nothing mends it.
    bool mended = false;
    float retreatingLate = 0.0f;
    float late = 0.0f;
    const float start = harness.time;
    harness.Run(150.0f, {}, {},
                [&](const Creature& creature)
                {
                    mended = mended || creature.Brain().Intent().recover > 0.0f;
                    if (harness.time - start > 90.0f)
                    {
                        late += 1.0f / 60.0f;
                        if (creature.Brain().Current() == Behavior::Retreat)
                        {
                            retreatingLate += 1.0f / 60.0f;
                        }
                    }
                });
    INFO("its mind:" << MindOf(*harness.creature));
    INFO("fear " << harness.creature->Brain().Feelings().fear << ", retreating for " << retreatingLate
                 << " s of the last " << late << " s");
    CHECK_FALSE(mended);
    CHECK(retreatingLate < late * 0.25f);
    CHECK(harness.creature->Brain().Current() != Behavior::Retreat);
}

// --- Temperament -------------------------------------------------------------------------------------
//
// Dangerous is not the same as malicious. Most of them hunt; the rest hold ground, keep away, or watch,
// and any of them turns on somebody who hurts it.

namespace
{

CreatureTraits WithTemperament(uint32_t seed, Temperament temperament)
{
    CreatureTraits traits = Hunter(seed);
    traits.temperament = temperament;
    return traits;
}

bool GoingFor(const Creature& creature)
{
    const Behavior now = creature.Brain().Current();
    return now == Behavior::Hunt || now == Behavior::Attack || now == Behavior::Stalk || now == Behavior::Search ||
           creature.Brain().Intent().strikeTarget >= 0;
}

} // namespace

TEST_CASE("Most creatures hunt, and the rest are a spread of the other temperaments", "[creature][temperament]")
{
    int counts[static_cast<int>(Temperament::Count)] = {};
    constexpr int kSeeds = 4000;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed)
    {
        ++counts[static_cast<int>(CreatureTraits::FromSeed(seed).temperament)];
    }
    INFO("predator " << counts[0] << ", territorial " << counts[1] << ", timid " << counts[2] << ", curious "
                     << counts[3]);
    CHECK(counts[0] > kSeeds * 0.45f);
    CHECK(counts[0] < kSeeds * 0.72f);
    for (int i = 1; i < 4; ++i)
    {
        CHECK(counts[i] > kSeeds * 0.05f);
    }
    // And the timid ones are the frightened ones.
    float timidFear = 0.0f;
    float otherFear = 0.0f;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed)
    {
        const CreatureTraits traits = CreatureTraits::FromSeed(seed);
        (traits.temperament == Temperament::Timid ? timidFear : otherFear) += traits.fear;
    }
    CHECK(timidFear / counts[2] > otherFear / (kSeeds - counts[2]) + 0.05f);
}

TEST_CASE("A timid creature keeps its distance and never goes for somebody who leaves it be",
          "[creature][temperament]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t) { return t.fear > 0.5f; });
    REQUIRE(seed != 0);
    CreatureHarness harness(WithTemperament(seed, Temperament::Timid));
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 8.0f, at, player));

    bool kept = false;
    bool went = false;
    harness.Run(20.0f, {Somebody(1, player)}, {},
                [&](const Creature& creature)
                {
                    kept = kept || creature.Brain().Current() == Behavior::Avoid;
                    went = went || GoingFor(creature);
                });
    const float after = glm::distance(harness.creature->Position(), player);
    INFO("its mind:" << MindOf(*harness.creature));
    INFO("ended " << after << " m away, having started 8 m away");
    CHECK(kept);
    CHECK_FALSE(went);
    CHECK(after > 10.0f);
}

TEST_CASE("A timid creature hurt and followed closely enough turns and fights", "[creature][temperament]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t) { return t.fear > 0.5f; });
    REQUIRE(seed != 0);
    CreatureHarness harness(WithTemperament(seed, Temperament::Timid));
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 4.0f, at, player));
    harness.Run(0.6f, {Somebody(1, player)});
    harness.creature->TakeDamage(Share(*harness.creature, 12.0f), 1, player + glm::vec3(0.0f, 1.5f, 0.0f),
                                 harness.time);

    // Whoever shot it stays on top of it, wherever it goes.
    bool struck = false;
    harness.RunLive(
        6.0f,
        [&](const Creature& creature)
        {
            const glm::vec3 close = creature.Position() + creature.Forward() * 1.3f;
            return std::vector<SensedPlayer>{Watching(1, close, creature.Position())};
        },
        [&](const Creature& creature) { struck = struck || creature.Brain().Intent().strikeTarget == 1; });
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK(struck);
}

TEST_CASE("A curious creature watches, and turns only on somebody who hurts it", "[creature][temperament]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t) { return t.curiosity > 0.7f && t.fear < 0.6f; });
    REQUIRE(seed != 0);
    CreatureHarness harness(WithTemperament(seed, Temperament::Curious));
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 8.0f, at, player));

    bool watched = false;
    bool went = false;
    harness.Run(20.0f, {Somebody(1, player)}, {},
                [&](const Creature& creature)
                {
                    watched = watched || creature.Brain().Current() == Behavior::Observe;
                    went = went || GoingFor(creature);
                });
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK(watched);
    CHECK_FALSE(went);

    // Shot, it is somebody else's animal.
    harness.creature->TakeDamage(Share(*harness.creature, 15.0f), 1, player + glm::vec3(0.0f, 1.5f, 0.0f),
                                 harness.time);
    bool turned = false;
    harness.Run(6.0f, {Somebody(1, player)}, {},
                [&](const Creature& creature) { turned = turned || GoingFor(creature); });
    CHECK(turned);
}

TEST_CASE("A territorial creature warns somebody off its ground before it fights them", "[creature][temperament]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t) { return t.fear < 0.6f; });
    REQUIRE(seed != 0);
    CreatureHarness harness(WithTemperament(seed, Temperament::Territorial));
    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 9.0f, at, player));

    float warnedAt = -1.0f;
    float wentAt = -1.0f;
    bool displayed = false;
    harness.Run(14.0f, {Somebody(1, player)}, {},
                [&](const Creature& creature)
                {
                    if (warnedAt < 0.0f && creature.Brain().Current() == Behavior::Warn)
                    {
                        warnedAt = harness.time;
                    }
                    displayed = displayed || creature.Brain().Intent().display;
                    if (wentAt < 0.0f && GoingFor(creature))
                    {
                        wentAt = harness.time;
                    }
                });
    INFO("its mind:" << MindOf(*harness.creature));
    INFO("warned at " << warnedAt << ", went for them at " << wentAt);
    REQUIRE(warnedAt >= 0.0f);
    CHECK(displayed);
    // It fought, in the end, for somebody who would not leave -- but not before it had said so.
    REQUIRE(wentAt >= 0.0f);
    CHECK(wentAt > warnedAt + 2.0f);
}

TEST_CASE("A territorial creature leaves alone somebody who is not on its ground", "[creature][temperament]")
{
    const uint32_t seed = SeedWhere([](const CreatureTraits& t) { return t.fear < 0.6f; });
    REQUIRE(seed != 0);
    CreatureHarness harness(WithTemperament(seed, Temperament::Territorial));

    // Its ground is well away from where the two of them are about to be.
    glm::vec3 home;
    bool found = false;
    uint32_t pick = 99u;
    for (int i = 0; i < 200 && !found; ++i)
    {
        found = harness.nav.RandomPointNear({0.0f, 0.0f, 0.0f}, 60.0f, pick, home) &&
                glm::length(glm::vec2(home.x, home.z)) > CreatureBrain::kTerritory + 14.0f;
    }
    REQUIRE(found);
    harness.creature->SetShownState(home, 0.0f, 0.0f, 0.0f, true);
    harness.Run(0.2f, {});
    REQUIRE(glm::distance(harness.creature->Brain().Home(), home) < 1.0f);

    glm::vec3 at;
    glm::vec3 player;
    REQUIRE(OpenView(harness, 7.0f, at, player));
    bool went = false;
    bool warned = false;
    harness.Run(12.0f, {Somebody(1, player)}, {},
                [&](const Creature& creature)
                {
                    went = went || GoingFor(creature);
                    warned = warned || creature.Brain().Current() == Behavior::Warn;
                });
    INFO("its mind:" << MindOf(*harness.creature));
    CHECK_FALSE(went);
    CHECK_FALSE(warned);
}

TEST_CASE("Only some creatures take people; the rest only fight", "[creature][temperament][grab]")
{
    const auto grabsIn = [](uint32_t seed)
    {
        CreatureHarness harness(seed);
        glm::vec3 at;
        glm::vec3 player;
        REQUIRE(OpenView(harness, 3.0f, at, player));
        int grabs = 0;
        AttackKind last = AttackKind::None;
        harness.RunLive(
            25.0f,
            [&](const Creature& creature)
            {
                const glm::vec3 close = creature.Position() + creature.Forward() * 1.2f;
                return std::vector<SensedPlayer>{Somebody(1, close)};
            },
            [&](const Creature& creature)
            {
                const AttackKind now = creature.Brain().CurrentAttack();
                if (now == AttackKind::Grab && last != AttackKind::Grab)
                {
                    ++grabs;
                }
                last = now;
            });
        return grabs;
    };
    const uint32_t taker = SeedWhere([](const CreatureTraits& t) { return t.Captures() && t.aggression > 0.6f; });
    const uint32_t fighter = SeedWhere([](const CreatureTraits& t) { return !t.Captures() && t.aggression > 0.6f; });
    REQUIRE(taker != 0);
    REQUIRE(fighter != 0);
    CHECK(grabsIn(taker) > 0);
    CHECK(grabsIn(fighter) == 0);
    // And nests are only ever built by ones that take people, which is what a nest is for.
    for (uint32_t seed = 1; seed < 2000; ++seed)
    {
        const CreatureTraits traits = CreatureTraits::FromSeed(seed);
        if (traits.Nests())
        {
            REQUIRE(traits.Captures());
        }
    }
}

TEST_CASE("Its nest attacked, it leaves whatever it was doing and goes back to it", "[creature][nest]")
{
    // Whatever its temperament: even a timid one does not leave its brood to whoever is shooting it.
    CreatureTraits traits = CreatureTraits::FromSeed(11);
    traits.temperament = GENERATE(Temperament::Timid, Temperament::Predator, Temperament::Curious);
    CreatureHarness harness(traits);
    harness.Run(1.0f, {});
    const glm::vec3 nest{kDarkRoomX, 0.0f, kDarkRoomZ};
    const float before = glm::distance(harness.creature->Position(), nest);
    harness.creature->Brain().OnNestAttacked(1, nest + glm::vec3(0.0f, 1.2f, 0.0f), harness.time, false);
    harness.Run(0.5f, {});
    INFO("temperament " << TemperamentName(traits.temperament) << ", " << MindOf(*harness.creature));
    CHECK_FALSE(harness.creature->Brain().Interest().resolved);
    CHECK(glm::distance(harness.creature->Brain().Interest().position, nest) < 1.5f);
    harness.Run(5.0f, {});
    CHECK(glm::distance(harness.creature->Position(), nest) < before - 3.0f);
}


TEST_CASE("A careful creature goes round to shooting, out of sight of it, rather than straight at it", "[creature][hearing][flank]")
{
    CreatureTraits traits = Hunter(23);
    traits.stealth = 0.8f;
    traits.aggression = 0.45f;
    traits.patience = 0.3f; // goes in after listening, rather than waiting by a door
    CreatureHarness harness(traits);
    const glm::vec3 start = harness.creature->Position();
    Noise shot;
    shot.kind = NoiseKind::Gunshot;
    shot.reach = NoiseReach::kGunshot;
    shot.position = {kDarkRoomX, 0.5f, kDarkRoomZ};
    harness.Run(0.5f, {}, {shot});
    INFO(MindOf(*harness.creature));
    REQUIRE(harness.creature->Brain().Current() == Behavior::Flank);
    REQUIRE(harness.creature->Brain().HasFlankPoint());
    const glm::vec3 flank = harness.creature->Brain().FlankPoint();
    // Off to the side of the way straight in: not on the line from where it was to the shot.
    const glm::vec2 straight = glm::normalize(glm::vec2(start.x - shot.position.x, start.z - shot.position.z));
    const glm::vec2 round = glm::normalize(glm::vec2(flank.x - shot.position.x, flank.z - shot.position.z));
    INFO("flank point " << flank.x << ", " << flank.z << "; " << glm::dot(straight, round) << " along the way in");
    CHECK(glm::dot(straight, round) < 0.85f);
    const float away = glm::length(glm::vec2(flank.x - shot.position.x, flank.z - shot.position.z));
    CHECK(away > 5.5f);
    CHECK(away < 14.5f);

    // In the end it does go in, having listened first.
    harness.Run(30.0f, {});
    INFO("after: " << MindOf(*harness.creature) << " at " << harness.creature->Position().x << ", " << harness.creature->Position().z);
    bool listened = false;
    for (const CreatureBrain::TimelineEntry& entry : harness.creature->Brain().Timeline())
    {
        listened = listened || entry.what.find("listen") != std::string::npos;
    }
    CHECK(listened);
}

TEST_CASE("It does not go straight back to what it has only just given up", "[creature][decide]")
{
    // A timid one near somebody: it keeps away, and a noise they make right after is not a reason to walk
    // straight back to them. It used to flip between the two every few seconds.
    CreatureTraits traits = CreatureTraits::FromSeed(11);
    traits.temperament = Temperament::Timid;
    traits.fear = 0.8f;
    traits.curiosity = 0.9f;
    CreatureHarness harness(traits);
    const glm::vec3 at = harness.creature->Position();
    SensedPlayer player;
    player.id = 1;
    player.name = "Near";
    player.feet = at + glm::vec3(0.0f, 0.0f, -7.0f);
    player.light = 1.0f;
    player.forward = {0.0f, 0.0f, 1.0f};
    std::vector<Behavior> seen;
    Noise step;
    step.kind = NoiseKind::Footstep;
    step.reach = NoiseReach::kFootstepSprint;
    step.player = 1;
    for (int second = 0; second < 30; ++second)
    {
        step.position = player.feet;
        // Visible for the first few seconds, then only heard.
        std::vector<SensedPlayer> players{player};
        if (second > 4)
        {
            players.front().feet = player.feet;
            players.front().hidden = false;
        }
        harness.Run(1.0f, second <= 4 ? players : std::vector<SensedPlayer>{player}, {step});
        seen.push_back(harness.creature->Brain().Current());
    }
    int changes = 0;
    for (size_t i = 1; i < seen.size(); ++i)
    {
        changes += seen[i] != seen[i - 1] ? 1 : 0;
    }
    INFO(MindOf(*harness.creature));
    CHECK(changes <= 6);
}
