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

TEST_CASE("Recoil kicks the view up and then gives it back", "[weapon]")
{
    const WeaponDefinition definition = TestWeapon(FireMode::Single);
    WeaponState state;
    WeaponSim::Equip(definition, state);

    WeaponInput input;
    input.trigger = true;
    Run(definition, state, input, 1);

    REQUIRE(state.recoilPitch > 0.5f);
    const float afterShot = state.recoilPitch;

    input.trigger = false;
    Run(definition, state, input, 120);
    // Recoil decays back towards where the player was aiming, so a burst does not permanently steal
    // their aim.
    REQUIRE(state.recoilPitch < afterShot * 0.05f);
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
