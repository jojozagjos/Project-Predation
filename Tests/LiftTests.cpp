#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Player/PlayerController.h"
#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/Lifts.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

using namespace pred;

// A building's lifts: how one is planned through a building, what it does when its buttons are pressed, and riding it.

namespace
{

FacilityMap::Lift ThreeStops()
{
    FacilityMap::Lift lift;
    lift.out = {0.0f, 0.0f, -1.0f};
    lift.stops = {0.0f, 3.6f, 7.2f};
    return lift;
}

// Runs it until something is true of it, or for so long; whether it came true.
template <typename Until>
bool RunUntil(LiftState& state, const FacilityMap::Lift& lift, Until until, float seconds = 30.0f, bool authority = true, bool powered = true,
              bool blocked = false)
{
    constexpr float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt)
    {
        LiftRules::Tick(state, lift, dt, authority, powered, blocked);
        if (until())
        {
            return true;
        }
    }
    return false;
}

// The first planned facility, from seed 1 on, with a lift in it.
uint16_t SeedWithLift()
{
    for (uint16_t seed = 1; seed < 300; ++seed)
    {
        const FacilityLayout layout = FacilityLayout::Generate(seed);
        if (std::any_of(layout.stairwells.begin(), layout.stairwells.end(),
                        [](const FacilityLayout::Stairwell& well) { return well.way == FacilityLayout::Stairwell::Way::Lift; }))
        {
            return seed;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("A lift opens its doors where it is, goes where it is asked once they are shut, and opens them there", "[lift]")
{
    const FacilityMap::Lift lift = ThreeStops();
    LiftState state;
    // Called where it stands: its doors open, stay open a while, and shut again.
    CHECK(LiftRules::Press(state, 0, true) == LiftRules::Pressed::Opened);
    REQUIRE(RunUntil(state, lift, [&] { return state.phase == LiftState::Phase::Open; }));
    CHECK(state.doors == 1.0f);
    REQUIRE(RunUntil(state, lift, [&] { return state.phase == LiftState::Phase::Shut; }));

    // Somebody in the doorway: the doors do not shut on them, and shutting, open again.
    LiftRules::Press(state, 0, true);
    REQUIRE(RunUntil(state, lift, [&] { return state.phase == LiftState::Phase::Open; }));
    CHECK_FALSE(RunUntil(state, lift, [&] { return state.phase == LiftState::Phase::Shut; }, 15.0f, true, true, true));
    CHECK(state.doors == 1.0f);

    // Asked to the top: the doors shut, it goes, and it opens them there -- having taken as long as a trip does.
    CHECK(LiftRules::Press(state, 2, true) == LiftRules::Pressed::Queued);
    REQUIRE(RunUntil(state, lift, [&] { return state.phase == LiftState::Phase::Moving; }));
    CHECK(state.at == 0);
    CHECK(state.to == 2);
    float travelled = 0.0f;
    float last = LiftRules::CarHeight(lift, state);
    bool rising = true;
    REQUIRE(RunUntil(state, lift,
                     [&]
                     {
                         travelled += 1.0f / 60.0f;
                         const float now = LiftRules::CarHeight(lift, state);
                         rising = rising && now >= last - 1.0e-4f;
                         last = now;
                         return state.phase != LiftState::Phase::Moving;
                     }));
    CHECK(rising);
    CHECK(std::abs(travelled - LiftRules::TripSeconds(lift, 0, 2)) < 0.05f);
    CHECK(state.at == 2);
    CHECK(LiftRules::CarHeight(lift, state) == 7.2f);
    CHECK(state.phase == LiftState::Phase::Opening);

    // Asked twice for the same stop, it is asked once.
    REQUIRE(RunUntil(state, lift, [&] { return state.phase == LiftState::Phase::Open; }));
    CHECK(LiftRules::Press(state, 1, true) == LiftRules::Pressed::Queued);
    CHECK(LiftRules::Press(state, 1, true) == LiftRules::Pressed::Nothing);
    CHECK(state.queue.size() == 1);
}

TEST_CASE("A lift with its power out goes nowhere, but its doors still open: nobody is ever shut in it", "[lift]")
{
    const FacilityMap::Lift lift = ThreeStops();
    LiftState state;
    LiftRules::RestAt(state, 1);
    // Called from another floor: nothing.
    CHECK(LiftRules::Press(state, 0, false) == LiftRules::Pressed::Nothing);
    CHECK(state.queue.empty());
    CHECK_FALSE(RunUntil(state, lift, [&] { return state.phase == LiftState::Phase::Moving; }, 10.0f, true, false));
    // From inside it, shut in: its doors open.
    CHECK(LiftRules::Press(state, 1, false) == LiftRules::Pressed::Opened);
    CHECK(RunUntil(state, lift, [&] { return state.doors >= 1.0f; }, 10.0f, true, false));
}

TEST_CASE("A lift goes only when the host sends it, and goes the same on every machine", "[lift][net]")
{
    const FacilityMap::Lift lift = ThreeStops();
    LiftState host;
    LiftState client;
    // Somebody presses for the top on the host; the client, not the authority, never sends it anywhere by itself.
    LiftRules::Press(host, 2, true);
    LiftRules::Press(client, 2, true);
    CHECK_FALSE(RunUntil(client, lift, [&] { return client.phase == LiftState::Phase::Moving; }, 5.0f, false));
    REQUIRE(RunUntil(host, lift, [&] { return host.phase == LiftState::Phase::Moving; }));
    // Told it has gone, the client's goes as the host's does, and gets there.
    LiftState told;
    LiftRules::Depart(told, host.at, host.to);
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i)
    {
        LiftRules::Tick(host, lift, dt, true, true, false);
        LiftRules::Tick(told, lift, dt, false, true, false);
        CHECK(std::abs(LiftRules::CarHeight(lift, host) - LiftRules::CarHeight(lift, told)) < 1.0e-4f);
    }
    REQUIRE(RunUntil(told, lift, [&] { return told.phase == LiftState::Phase::Open; }, 30.0f, false));
    CHECK(told.at == 2);
    // And somebody joining is told where it stands.
    LiftState joined;
    LiftRules::RestAt(joined, 2);
    CHECK(LiftRules::CarHeight(lift, joined) == 7.2f);
}

TEST_CASE("Lifts are planned through buildings of three floors and more, a door on every floor, and stairs besides", "[lift][facility]")
{
    int withLift = 0;
    for (uint16_t seed = 1; seed <= 80; ++seed)
    {
        INFO("seed " << seed);
        const FacilityLayout plan = FacilityLayout::Generate(seed);
        for (const FacilityLayout::Stairwell& well : plan.stairwells)
        {
            if (well.way != FacilityLayout::Stairwell::Way::Lift)
            {
                continue;
            }
            ++withLift;
            CHECK(plan.floors >= 3);
            CHECK(well.levels >= 2);
            CHECK(well.min == well.max);
            for (int f = well.floor; f <= well.Top(); ++f)
            {
                CHECK(plan.At(f, well.min.x, well.min.y) == FacilityLayout::Cell::Stair);
                // A doorway into it on every floor it stops at.
                const bool door = std::any_of(plan.doors.begin(), plan.doors.end(), [&](const FacilityLayout::Door& d)
                                              {
                                                  const glm::ivec2 next = d.cell + (d.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
                                                  return d.floor == f && (d.cell == well.min || next == well.min);
                                              });
                CHECK(door);
            }
        }
        // And between every two floors a flight or a ladder, whatever else.
        for (int f = 0; f + 1 < plan.floors; ++f)
        {
            CHECK(std::any_of(plan.stairwells.begin(), plan.stairwells.end(), [&](const FacilityLayout::Stairwell& well)
                              { return well.floor == f && well.way != FacilityLayout::Stairwell::Way::Lift; }));
        }
    }
    CHECK(withLift > 3);
}

TEST_CASE("A lift is ridden: called, walked into, sent up, and walked out of at the top", "[lift][facility][player]")
{
    const uint16_t seed = SeedWithLift();
    REQUIRE(seed != 0);
    INFO("seed " << seed);
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    InteractionSystem interactions;
    FacilityMap map;
    map.Build(seed, scene, meshes, physics, nullptr);
    REQUIRE_FALSE(map.Lifts().empty());
    Lifts lifts;
    lifts.Build(map.Lifts(), scene, meshes, physics, interactions);
    REQUIRE(lifts.Count() == map.Lifts().size());
    const FacilityMap::Lift& def = lifts.Def(0);
    const int top = static_cast<int>(def.stops.size()) - 1;
    // Its buttons: a call button at every stop, and one for each stop inside and one to open it.
    CHECK(interactions.FindByPayload(InteractionKind::Lift, Lifts::Payload(0, top)) != nullptr);
    CHECK(interactions.FindByPayload(InteractionKind::Lift, Lifts::Payload(0, Lifts::kOpenButton)) != nullptr);

    // Its doors shut at a stop: solid across the doorway.
    const glm::vec3 landing = lifts.Doorway(0, top) + def.out * 1.2f + glm::vec3(0.0f, 1.0f, 0.0f);
    CHECK(physics.RayCastStatic(landing, -def.out, 1.5f));

    PlayerController player;
    PlayerConfig config;
    physics.OptimizeBroadPhase();
    REQUIRE(player.Init(physics, config, lifts.Doorway(0, 0) + def.out * 1.3f + glm::vec3(0.0f, 0.05f, 0.0f)));
    constexpr float dt = 1.0f / 60.0f;
    LiftState& state = lifts.State(0);
    bool riding = false;
    const auto run = [&](const PlayerInput& input, int ticks)
    {
        for (int i = 0; i < ticks; ++i)
        {
            // As the game does (PredationGame::UpdateLifts): who is in the car when it goes is carried as far as it goes each
            // tick, walking about in it as they like, until it gets there.
            const float before = LiftRules::CarHeight(def, state);
            const LiftRules::Ticked ticked = LiftRules::Tick(state, def, dt, true, true, lifts.InDoorway(0, state.at, player.State().position));
            const float rise = LiftRules::CarHeight(def, state) - before;
            if (ticked.departed && lifts.InCar(0, player.State().position))
            {
                riding = true;
            }
            if (riding && (state.phase == LiftState::Phase::Moving || ticked.arrived))
            {
                player.Carry(glm::vec3(0.0f, rise, 0.0f));
            }
            if (ticked.arrived)
            {
                riding = false;
            }
            lifts.Show(scene, physics);
            player.Step(input, dt);
            physics.Step(dt);
        }
    };
    run(PlayerInput{}, 30);
    // Called: its doors open.
    REQUIRE(LiftRules::Press(state, 0, true) == LiftRules::Pressed::Opened);
    run(PlayerInput{}, 100);
    REQUIRE(state.phase == LiftState::Phase::Open);
    // In, facing into the shaft, over the sill and into the middle of the car.
    PlayerInput in;
    in.move = {0.0f, 1.0f};
    in.yaw = std::atan2(-def.out.x, def.out.z);
    for (int i = 0; i < 300 && glm::distance(glm::vec2(player.State().position.x, player.State().position.z), glm::vec2(def.centre.x, def.centre.z)) > 0.4f; ++i)
    {
        run(in, 1);
    }
    REQUIRE(lifts.InCar(0, player.State().position));
    // Sent to the top: carried there, and put down in it, standing.
    REQUIRE(LiftRules::Press(state, top, true) == LiftRules::Pressed::Queued);
    // Walking about in it, one side and back, on the way up: in it, on its floor, all the way.
    bool stayed = true;
    for (int i = 0; i < 60 * 40 && !(state.at == top && state.phase == LiftState::Phase::Open); ++i)
    {
        PlayerInput about;
        about.yaw = in.yaw;
        about.move = {i % 120 < 30 ? 1.0f : i % 120 < 60 ? -1.0f : 0.0f, 0.0f};
        run(state.phase == LiftState::Phase::Moving ? about : PlayerInput{}, 1);
        if (state.phase == LiftState::Phase::Moving)
        {
            stayed = stayed && lifts.InCar(0, player.State().position) && std::abs(player.State().position.y - lifts.CarFloor(0).y) < 0.2f;
        }
    }
    CHECK(stayed);
    REQUIRE(state.at == top);
    run(PlayerInput{}, 30);
    CHECK_FALSE(player.IsAttached());
    CHECK(player.State().grounded);
    CHECK(std::abs(player.State().position.y - def.stops.back()) < 0.1f);
    // And out onto the landing there.
    PlayerInput out;
    out.move = {0.0f, 1.0f};
    out.yaw = std::atan2(def.out.x, -def.out.z);
    run(out, 120);
    CHECK_FALSE(lifts.InCar(0, player.State().position));
    CHECK(std::abs(player.State().position.y - def.stops.back()) < 0.1f);
    CHECK(glm::dot(player.State().position - lifts.Doorway(0, top), def.out) > 0.5f);
    player.Shutdown();
    lifts.Clear(scene, physics, interactions);
    map.Clear(scene, physics, nullptr);
    physics.Shutdown();
}
