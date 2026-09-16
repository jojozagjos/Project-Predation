#include "Engine/Core/Paths.h"
#include "Game/Weapons/ShotResolver.h"
#include "Game/Weapons/WeaponDatabase.h"
#include "Game/Weapons/WeaponSystem.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <vector>

using namespace pred;

namespace
{

constexpr float kTick = 1.0f / 60.0f;
const glm::vec3 kMuzzle{0.0f, 1.6f, 0.0f};
const glm::vec3 kForward{0.0f, 0.0f, -1.0f};

// A weapon with the awkward numbers taken out, so a test says what it means.
WeaponDefinition TestWeapon(FireMode mode)
{
    WeaponDefinition definition;
    definition.id = 1;
    definition.key = "test";
    definition.mode = mode;
    definition.roundsPerMinute = 300.0f; // one round every five ticks
    definition.magazineSize = 5;
    definition.reserveOnPickup = 10;
    definition.reloadSeconds = 1.0f;
    definition.spreadHip = 0.0f;
    definition.spreadAim = 0.0f;
    definition.spreadPerShot = 0.0f;
    definition.burstCount = 3;
    return definition;
}

// Runs the simulation for a while and returns everything that came out of the barrel.
std::vector<FireEvent> Run(const WeaponDefinition& definition, WeaponState& state, WeaponInput input,
                           int ticks)
{
    std::vector<FireEvent> events;
    for (int i = 0; i < ticks; ++i)
    {
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, events);
    }
    return events;
}

} // namespace

TEST_CASE("An unloaded weapon does not fire", "[weapon]")
{
    const WeaponDefinition definition = TestWeapon(FireMode::Auto);
    WeaponState state;
    WeaponSim::Equip(definition, state);
    state.rounds = 0;
    state.reserve = 0;

    WeaponInput input;
    input.trigger = true;
    REQUIRE(Run(definition, state, input, 60).empty());
}

TEST_CASE("Single fire needs the trigger released between rounds", "[weapon]")
{
    const WeaponDefinition definition = TestWeapon(FireMode::Single);
    WeaponState state;
    WeaponSim::Equip(definition, state);

    WeaponInput input;
    input.trigger = true;
    // Held down for a second: exactly one round, however long the magazine is.
    REQUIRE(Run(definition, state, input, 60).size() == 1);
    REQUIRE(state.rounds == definition.magazineSize - 1);

    input.trigger = false;
    Run(definition, state, input, 5);
    input.trigger = true;
    REQUIRE(Run(definition, state, input, 5).size() == 1);
}

TEST_CASE("Automatic fire respects the rate of fire and the magazine", "[weapon]")
{
    const WeaponDefinition definition = TestWeapon(FireMode::Auto);
    WeaponState state;
    WeaponSim::Equip(definition, state);

    WeaponInput input;
    input.trigger = true;
    // 300 rounds a minute is one every 0.2 s, so half a second holds three at most; the magazine
    // holds five, so a full second empties it and no more.
    const std::vector<FireEvent> events = Run(definition, state, input, 60);
    REQUIRE(events.size() == static_cast<size_t>(definition.magazineSize));
    REQUIRE(state.rounds == 0);

    // Sequence numbers count up without gaps, which is what a host uses to spot a client claiming
    // shots it never took.
    for (size_t i = 0; i < events.size(); ++i)
    {
        REQUIRE(events[i].sequence == static_cast<uint32_t>(i));
    }
}

TEST_CASE("A burst fires its count and then stops until the trigger is pulled again", "[weapon]")
{
    const WeaponDefinition definition = TestWeapon(FireMode::Burst);
    WeaponState state;
    WeaponSim::Equip(definition, state);

    WeaponInput input;
    input.trigger = true;
    REQUIRE(Run(definition, state, input, 60).size() == static_cast<size_t>(definition.burstCount));

    input.trigger = false;
    Run(definition, state, input, 5);
    input.trigger = true;
    // Only two rounds left in the magazine, so the second burst is cut short rather than borrowing
    // from the reserve.
    REQUIRE(Run(definition, state, input, 60).size() == 2);
}

TEST_CASE("Reloading takes time, refills from the reserve, and blocks firing", "[weapon]")
{
    const WeaponDefinition definition = TestWeapon(FireMode::Auto);
    WeaponState state;
    WeaponSim::Equip(definition, state);
    state.rounds = 1;

    WeaponInput input;
    input.reload = true;
    Run(definition, state, input, 1);
    REQUIRE(state.IsReloading());

    // Nothing comes out while the magazine is out.
    input.reload = false;
    input.trigger = true;
    REQUIRE(Run(definition, state, input, 30).empty());
    REQUIRE(state.rounds == 1);

    Run(definition, state, input, 31);
    REQUIRE_FALSE(state.IsReloading());
    REQUIRE(state.rounds == definition.magazineSize);
    // Four rounds were taken to top up a magazine that had one in it.
    REQUIRE(state.reserve == definition.reserveOnPickup - 4);
}

TEST_CASE("An empty magazine reloads itself rather than clicking", "[weapon]")
{
    const WeaponDefinition definition = TestWeapon(FireMode::Auto);
    WeaponState state;
    WeaponSim::Equip(definition, state);

    WeaponInput input;
    input.trigger = true;
    Run(definition, state, input, 60); // empties it
    REQUIRE(state.rounds == 0);

    Run(definition, state, input, 1);
    REQUIRE(state.IsReloading());
}



TEST_CASE("Aiming tightens the spread and takes time to come up", "[weapon]")
{
    WeaponDefinition definition = TestWeapon(FireMode::Single);
    definition.spreadHip = 4.0f;
    definition.spreadAim = 0.5f;
    definition.aimSeconds = 0.2f;

    WeaponState state;
    WeaponSim::Equip(definition, state);
    REQUIRE(WeaponSim::CurrentSpread(definition, state) == Catch::Approx(4.0f));

    WeaponInput input;
    input.aim = true;
    Run(definition, state, input, 6); // 0.1 s, half way up
    REQUIRE(state.aim > 0.3f);
    REQUIRE(state.aim < 0.7f);

    Run(definition, state, input, 20);
    REQUIRE(state.aim == Catch::Approx(1.0f));
    REQUIRE(WeaponSim::CurrentSpread(definition, state) == Catch::Approx(0.5f));
}

TEST_CASE("Spread is the same everywhere for the same shot", "[weapon]")
{
    // Two machines simulating the same shot must send the round the same way, or a client's
    // prediction and the host's hit test disagree about what was in front of the barrel. The
    // deviation therefore comes from the shot number, not from a random number generator whose
    // state would have to be replicated.
    const glm::vec3 first = WeaponSim::DeviateShot(kForward, 5.0f, 41u);
    const glm::vec3 again = WeaponSim::DeviateShot(kForward, 5.0f, 41u);
    REQUIRE(glm::length(first - again) < 1e-6f);

    const glm::vec3 next = WeaponSim::DeviateShot(kForward, 5.0f, 42u);
    REQUIRE(glm::length(first - next) > 1e-4f);

    // And every round must stay inside the cone it was given.
    for (uint32_t shot = 0; shot < 400; ++shot)
    {
        const glm::vec3 direction = WeaponSim::DeviateShot(kForward, 5.0f, shot);
        REQUIRE(glm::length(direction) == Catch::Approx(1.0f).margin(1e-4));
        REQUIRE(glm::dot(direction, kForward) > std::cos(glm::radians(5.0f)) - 1e-4f);
    }
}

TEST_CASE("A shot hits what is in front of it and stops at its range", "[weapon]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));

    Transform wall;
    wall.position = {0.0f, 1.6f, -10.0f};
    const BodyHandle body = physics.CreateBox({2.0f, 2.0f, 0.2f}, wall, BodyMotion::Static);
    physics.OptimizeBroadPhase();

    FireEvent event;
    event.origin = kMuzzle;
    event.direction = kForward;
    event.damage = 26.0f;
    event.range = 55.0f;

    const ShotResult hit = ResolveShot(physics, event);
    REQUIRE(hit.hit);
    REQUIRE(hit.body == body);
    REQUIRE(hit.distance == Catch::Approx(9.8f).margin(0.05));
    REQUIRE(hit.damage == Catch::Approx(26.0f));

    // Short of the wall, the round finds nothing. Damage is a result of the trace, so a miss can
    // never carry any.
    event.range = 5.0f;
    const ShotResult miss = ResolveShot(physics, event);
    REQUIRE_FALSE(miss.hit);
    REQUIRE(miss.damage == Catch::Approx(0.0f));

    physics.Shutdown();
}

TEST_CASE("Weapon definitions load from the shipped data file", "[weapon][data]")
{
    // Without this the assets root is unset and the database quietly falls back to its built-ins,
    // which would let this test pass while reading nothing at all.
    Paths::Init(nullptr);

    WeaponDatabase database;
    REQUIRE(database.LoadFromFile(Paths::AssetsRoot() / "Data" / "weapons.json"));

    const WeaponDefinition* sidearm = database.Find("sidearm");
    REQUIRE(sidearm != nullptr);
    REQUIRE(sidearm->mode == FireMode::Single);
    REQUIRE(sidearm->magazineSize > 0);

    // The link from an inventory item to the weapon it carries is what lets a picked-up gun be
    // wielded without a second table mapping one to the other.
    REQUIRE(database.ForItem("sidearm") == sidearm->id);
    REQUIRE(database.ForItem("medkit") == kInvalidWeapon);

    // Only the real file defines the carbine, so this is what proves the file was read.
    const WeaponDefinition* carbine = database.Find("carbine");
    REQUIRE(carbine != nullptr);
    REQUIRE(carbine->mode == FireMode::Auto);
}
TEST_CASE("Recoil is given to the aim and stays there", "[weapon][recoil]")
{
    // The point of recoil in a game somebody plays is that it takes their aim off the target and
    // they have to put it back. An offset that decays returns the aim to exactly where it started,
    // so the weapon climbs on screen and then un-climbs and there is nothing to fight -- which is
    // what "it's not moving my camera up, when I'm done shooting my camera goes back to where it
    // was" is describing.
    //
    // So a round owes the view an angle, the debt is paid off over a few tens of milliseconds, and
    // what the view is given it keeps. This measures the total paid, which is what the player's aim
    // actually moved by.
    WeaponDefinition definition = TestWeapon(FireMode::Auto);
    definition.roundsPerMinute = 620.0f;
    definition.recoilPitch = 0.6f;
    definition.recoilYaw = 0.28f;
    definition.recoilRise = 26.0f;
    definition.magazineSize = 60; // enough to hold the trigger down for thirty rounds

    WeaponState state;
    WeaponSim::Equip(definition, state);

    WeaponInput input;
    input.trigger = true;

    // The tick the trigger goes down already moves the view: a shot has to answer immediately even
    // if the movement it starts takes a moment to finish.
    std::vector<FireEvent> shots;
    WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
    CHECK(state.kickPitch > 0.0f);

    // And it arrives over several ticks rather than in one, which is what stops a burst reading as
    // a stack of cuts.
    const float firstTick = state.kickPitch;
    CHECK(firstTick < definition.recoilPitch * 0.8f);

    // Thirty rounds, and the aim has moved by roughly thirty rounds' worth.
    float climbed = firstTick;
    int fired = 1;
    for (int i = 0; i < 400 && fired < 30; ++i)
    {
        const size_t before = shots.size();
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
        climbed += state.kickPitch;
        fired += static_cast<int>(shots.size() - before);
    }
    INFO("thirty rounds moved the aim " << climbed << " degrees");
    CHECK(climbed > 12.0f);
    CHECK(climbed < 25.0f);

    // Off the trigger, the rest of what is owed is paid and then nothing more happens. Crucially,
    // none of it is taken back: the aim stays where the weapon put it.
    input.trigger = false;
    float afterRelease = 0.0f;
    for (int i = 0; i < 240; ++i)
    {
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
        afterRelease += state.kickPitch;
    }
    INFO("after release the aim moved a further " << afterRelease << " degrees");
    // Whatever is left owed is small and positive. Nothing ever moves the aim back down: a negative
    // total here would be the weapon handing the player's aim back, which is the bug.
    CHECK(afterRelease >= 0.0f);
    CHECK(afterRelease < 1.0f);
}


TEST_CASE("The shove settles and never takes the aim with it", "[weapon][recoil]")
{
    // Two things happen to a camera when a weapon fires. The aim genuinely moves and stays moved,
    // which is what the player fights, and the view jolts and settles again within a fraction of a
    // second, which is what gives a shot weight. They are separate on purpose: the shove reaches the
    // camera and never the look angles, so it cannot steal anybody's aim.
    //
    // It is a spring, and a spring integrated in the wrong order gains energy at these stiffnesses.
    // A camera that slowly winds itself up is the one failure here nobody would think to look for,
    // so this fires once and then watches for a long time.
    WeaponDefinition definition = TestWeapon(FireMode::Single);
    definition.shakeAmount = 1.1f;
    definition.shakeStiffness = 220.0f;
    definition.shakeDamping = 22.0f;

    WeaponState state;
    WeaponSim::Equip(definition, state);

    std::vector<FireEvent> shots;
    WeaponInput input;
    input.trigger = true;
    WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
    REQUIRE(shots.size() == 1);

    input.trigger = false;
    float peak = 0.0f;
    for (int i = 0; i < 12; ++i)
    {
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
        peak = std::max(peak, std::abs(state.shakePitch));
    }
    INFO("the shove peaked at " << peak << " degrees");
    CHECK(peak > 0.15f); // it is felt
    CHECK(peak < 6.0f);  // and it is not a spasm

    // And it is gone in well under a second, with no sign of winding up.
    for (int i = 0; i < 120; ++i)
    {
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
        REQUIRE(std::abs(state.shakePitch) < peak * 1.05f);
    }
    INFO("two seconds later it sits at " << state.shakePitch);
    CHECK(std::abs(state.shakePitch) < 0.02f);
    CHECK(std::abs(state.shakeVelocityPitch) < 1.0f);
}

TEST_CASE("Diagnostic: holding the trigger through an empty magazine", "[.][weapon][diag]")
{
    WeaponDefinition definition = TestWeapon(FireMode::Auto);
    definition.roundsPerMinute = 620.0f;
    definition.magazineSize = 30;
    definition.recoilPitch = 0.6f;
    definition.recoilRise = 26.0f;

    WeaponState state;
    WeaponSim::Equip(definition, state);

    std::vector<FireEvent> shots;
    WeaponInput input;
    input.trigger = true;

    float total = 0.0f;
    int fired = 0;
    std::string table = "\n tick | rounds | fired | pending | kick    | total\n";
    for (int i = 0; i < 600; ++i)
    {
        const size_t before = shots.size();
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
        fired += static_cast<int>(shots.size() - before);
        total += state.kickPitch;
        if (i % 40 == 0 || (i > 170 && i < 200))
        {
            char row[160];
            std::snprintf(row, sizeof(row), "%5d | %6d | %5d | %7.4f | %7.4f | %7.3f\n", i,
                          state.rounds, fired, state.recoilPitch, state.kickPitch, total);
            table += row;
        }
    }
    WARN(table);
    WARN("rounds fired " + std::to_string(fired) + ", owed " +
         std::to_string(fired * definition.recoilPitch) + ", paid " + std::to_string(total));
}

TEST_CASE("The aim stops moving when the shooting stops", "[weapon][recoil]")
{
    // Holding the trigger until the magazine ran dry started an automatic reload, and the reload
    // path left the function before the recoil was settled -- so `kickPitch` kept whatever it had
    // been on the last tick that ran to the end, and the caller went on adding it to the player's
    // aim every tick. The camera climbed at a constant rate for the whole reload with nothing being
    // fired. Reported as "the camera recoil keeps going up even though I've used all my bullets and
    // I'm not shooting".
    //
    // The general shape of it is worth guarding rather than the one path: whatever the weapon is
    // doing, the total handed to the aim must be what the rounds actually owed and not a penny more.
    WeaponDefinition definition = TestWeapon(FireMode::Auto);
    definition.roundsPerMinute = 620.0f;
    definition.magazineSize = 30;
    definition.reserveOnPickup = 60;
    definition.reloadSeconds = 2.0f;
    definition.recoilPitch = 0.6f;
    definition.recoilRise = 26.0f;

    WeaponState state;
    WeaponSim::Equip(definition, state);

    std::vector<FireEvent> shots;
    WeaponInput input;
    input.trigger = true;

    // Ten seconds on the trigger: a magazine, a reload, and most of another magazine.
    float total = 0.0f;
    for (int i = 0; i < 600; ++i)
    {
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
        total += state.kickPitch;
    }
    const float owed = static_cast<float>(shots.size()) * definition.recoilPitch;
    INFO(shots.size() << " rounds owed " << owed << " degrees and the aim moved " << total);
    CHECK(total == Catch::Approx(owed).margin(0.05f));

    // And once the trigger is released and the last instalment paid, the aim stops dead. A tenth of
    // a degree over two seconds is the decay tailing off; anything more is movement from nowhere.
    input.trigger = false;
    for (int i = 0; i < 60; ++i)
    {
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
    }
    float afterwards = 0.0f;
    for (int i = 0; i < 120; ++i)
    {
        WeaponSim::Step(definition, input, state, kMuzzle, kForward, kTick, shots);
        afterwards += std::abs(state.kickPitch);
    }
    INFO("two seconds after release the aim moved a further " << afterwards);
    CHECK(afterwards < 0.01f);
}
