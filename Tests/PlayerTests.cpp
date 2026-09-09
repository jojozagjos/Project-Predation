#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Primitives.h"
#include "Game/Player/PlayerController.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace pred;

namespace
{

constexpr float kTick = 1.0f / 60.0f;

Transform At(float x, float y, float z)
{
    Transform transform;
    transform.position = {x, y, z};
    return transform;
}

// A minimal world with flat ground, driven exactly the way Application drives the real one:
// the player decides first, then physics resolves. Everything here runs headlessly, with no
// window, renderer or input device.
struct PlayerHarness
{
    PhysicsWorld physics;
    PlayerController player;
    PlayerConfig config;

    PlayerHarness()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1; // deterministic and cheap for tests
        REQUIRE(physics.Init(settings));
        // Ground slab whose top face is exactly y = 0.
        physics.CreateBox({60.0f, 0.5f, 60.0f}, At(0.0f, -0.5f, 0.0f), BodyMotion::Static);
    }

    ~PlayerHarness()
    {
        player.Shutdown();
        physics.Shutdown();
    }

    void AddStaticBox(const glm::vec3& halfExtents, const glm::vec3& centre)
    {
        physics.CreateBox(halfExtents, At(centre.x, centre.y, centre.z), BodyMotion::Static);
    }

    void Spawn(const glm::vec3& position = {0.0f, 0.05f, 0.0f})
    {
        physics.OptimizeBroadPhase();
        REQUIRE(player.Init(physics, config, position));
    }

    void Simulate(const PlayerInput& input, int ticks)
    {
        for (int i = 0; i < ticks; ++i)
        {
            player.Step(input, kTick);
            physics.Step(kTick);
        }
    }

    // Runs until the player is standing on something, so tests start from a settled state.
    void Settle(int ticks = 30)
    {
        Simulate(PlayerInput{}, ticks);
    }

    const PlayerState& State() const { return player.State(); }
};

PlayerInput ForwardInput()
{
    PlayerInput input;
    input.move = {0.0f, 1.0f};
    return input;
}

} // namespace

TEST_CASE("Player settles on the ground and stays there", "[player]")
{
    PlayerHarness harness;
    harness.Spawn({0.0f, 2.0f, 0.0f});

    harness.Simulate(PlayerInput{}, 120);

    REQUIRE(harness.State().grounded);
    REQUIRE(harness.State().position.y == Catch::Approx(0.0f).margin(0.05));
    REQUIRE(std::abs(harness.State().velocity.y) < 0.5f);
}

TEST_CASE("Player accelerates to the configured speed and stops when input stops", "[player]")
{
    PlayerHarness harness;
    harness.Spawn();
    harness.Settle();

    const float startZ = harness.State().position.z;
    harness.Simulate(ForwardInput(), 90);

    REQUIRE(harness.State().grounded);
    REQUIRE(harness.State().HorizontalSpeed() == Catch::Approx(harness.config.moveSpeed).epsilon(0.06));
    // Yaw 0 faces -Z, so moving forward decreases z.
    REQUIRE(harness.State().position.z < startZ - 2.0f);

    // Releasing input must bring the player to a stop, not let them drift.
    harness.Simulate(PlayerInput{}, 40);
    REQUIRE(harness.State().HorizontalSpeed() < 0.05f);
}

TEST_CASE("Sprinting is faster than walking, and only forwards", "[player]")
{
    PlayerHarness harness;
    harness.Spawn();
    harness.Settle();

    PlayerInput sprintForward = ForwardInput();
    sprintForward.sprint = true;
    harness.Simulate(sprintForward, 120);
    const float forwardSprintSpeed = harness.State().HorizontalSpeed();
    REQUIRE(forwardSprintSpeed == Catch::Approx(harness.config.sprintSpeed).epsilon(0.06));

    // Holding sprint while walking backwards must not grant sprint speed.
    PlayerInput sprintBackward;
    sprintBackward.move = {0.0f, -1.0f};
    sprintBackward.sprint = true;
    harness.Simulate(sprintBackward, 120);
    const float backwardSpeed = harness.State().HorizontalSpeed();
    REQUIRE(backwardSpeed < harness.config.moveSpeed);
    REQUIRE(backwardSpeed == Catch::Approx(harness.config.moveSpeed * harness.config.backwardScale)
                                 .epsilon(0.08));
}

TEST_CASE("Jump reaches roughly the configured height and lands again", "[player]")
{
    PlayerHarness harness;
    harness.Spawn();
    harness.Settle();

    const float groundY = harness.State().position.y;

    PlayerInput jump;
    jump.jump = true;
    harness.Simulate(jump, 1);
    REQUIRE(harness.State().jumpedThisTick);

    float peak = groundY;
    PlayerInput idle;
    for (int i = 0; i < 180; ++i)
    {
        harness.Simulate(idle, 1);
        peak = std::max(peak, harness.State().position.y);
        if (harness.State().grounded && i > 10)
        {
            break;
        }
    }

    REQUIRE(peak - groundY == Catch::Approx(harness.config.jumpHeight).epsilon(0.18));
    REQUIRE(harness.State().grounded);
}

TEST_CASE("Jump input is buffered so a slightly early press still fires on landing", "[player]")
{
    PlayerHarness harness;
    harness.Spawn({0.0f, 1.5f, 0.0f});

    // Hold jump the whole way down. The buffer should make the player leave the ground again
    // immediately on contact rather than swallowing the press.
    PlayerInput jump;
    jump.jump = true;

    bool jumpedAfterLanding = false;
    for (int i = 0; i < 200; ++i)
    {
        harness.Simulate(jump, 1);
        if (harness.State().jumpedThisTick)
        {
            jumpedAfterLanding = true;
            break;
        }
    }
    REQUIRE(jumpedAfterLanding);
}

TEST_CASE("Crouching shrinks the player and slows them down", "[player]")
{
    PlayerHarness harness;
    harness.Spawn();
    harness.Settle();

    PlayerInput crouchForward = ForwardInput();
    crouchForward.crouchHeld = true;
    harness.Simulate(crouchForward, 90);

    REQUIRE(harness.State().stance == PlayerStance::Crouching);
    REQUIRE_FALSE(harness.State().stanceBlocked);
    REQUIRE(harness.State().HorizontalSpeed() == Catch::Approx(harness.config.crouchSpeed).epsilon(0.08));
    REQUIRE(harness.State().HorizontalSpeed() < harness.config.moveSpeed);

    // Standing up again in the open must succeed.
    harness.Simulate(ForwardInput(), 60);
    REQUIRE(harness.State().stance == PlayerStance::Standing);
}

// Prone is shorter than the standing capsule is wide, which makes the capsule geometry degenerate
// unless the radius is narrowed to suit. Getting that wrong made Jolt assert, and the assert
// handler terminated the process: pressing the prone key closed the game.
TEST_CASE("Going prone produces a valid shape and does not abort", "[player][prone]")
{
    PlayerHarness harness;
    harness.Spawn();
    harness.Settle();

    REQUIRE(harness.config.proneHeight < 2.0f * harness.config.radius); // the case that used to crash

    PlayerInput prone;
    prone.proneHeld = true;
    harness.Simulate(prone, 30);

    REQUIRE(harness.State().stance == PlayerStance::Prone);
    REQUIRE_FALSE(harness.State().stanceBlocked);

    // Still simulating normally afterwards, rather than wedged or fallen through the floor.
    REQUIRE(harness.State().grounded);
    REQUIRE(harness.State().position.y == Catch::Approx(0.0f).margin(0.1));

    PlayerInput proneForward = prone;
    proneForward.move = {0.0f, 1.0f};
    harness.Simulate(proneForward, 90);
    REQUIRE(harness.State().HorizontalSpeed() == Catch::Approx(harness.config.proneSpeed).epsilon(0.15));

    // And back up again in the open.
    harness.Simulate(PlayerInput{}, 40);
    REQUIRE(harness.State().stance == PlayerStance::Standing);
}

TEST_CASE("Character shapes stay valid at awkward dimensions", "[player][physics]")
{
    // Every one of these has a height at or below twice the radius, so a naive capsule would have a
    // zero or negative cylinder section.
    const float heights[] = {0.60f, 0.50f, 0.40f, 0.30f, 0.10f, 0.02f};

    for (const float height : heights)
    {
        PlayerHarness harness;
        harness.config.proneHeight = height;
        harness.Spawn();
        harness.Settle(10);

        PlayerInput prone;
        prone.proneHeld = true;
        harness.Simulate(prone, 20);

        INFO("prone height " << height);
        REQUIRE(harness.State().stance == PlayerStance::Prone);
        REQUIRE(harness.State().position.y == Catch::Approx(0.0f).margin(0.2));
    }
}

TEST_CASE("Prone is lower and slower than crouching", "[player][prone]")
{
    PlayerHarness harness;
    harness.Spawn();
    harness.Settle();

    PlayerInput crouch;
    crouch.crouchHeld = true;
    crouch.move = {0.0f, 1.0f};
    harness.Simulate(crouch, 90);
    const float crouchSpeed = harness.State().HorizontalSpeed();

    PlayerInput prone;
    prone.proneHeld = true;
    prone.move = {0.0f, 1.0f};
    harness.Simulate(prone, 90);
    const float proneSpeed = harness.State().HorizontalSpeed();

    REQUIRE(proneSpeed < crouchSpeed);
    REQUIRE(harness.config.proneHeight < harness.config.crouchHeight);
}

TEST_CASE("Player cannot stand up under a low ceiling", "[player]")
{
    PlayerHarness harness;
    // A slab spanning y = 1.30 to 1.75, ten metres along +X. Crouch height fits under it; standing
    // height does not.
    harness.AddStaticBox({3.0f, 0.225f, 3.0f}, {10.0f, 1.525f, 0.0f});
    harness.Spawn();
    harness.Settle();

    PlayerInput crouch;
    crouch.crouchHeld = true;
    harness.Simulate(crouch, 20);
    REQUIRE(harness.State().stance == PlayerStance::Crouching);

    // Move under the slab while still crouched.
    harness.player.Teleport({10.0f, 0.05f, 0.0f});
    harness.Simulate(crouch, 20);
    REQUIRE(harness.State().stance == PlayerStance::Crouching);

    // Releasing crouch must be refused, and reported as blocked rather than silently ignored.
    harness.Simulate(PlayerInput{}, 20);
    REQUIRE(harness.State().stance == PlayerStance::Crouching);
    REQUIRE(harness.State().stanceBlocked);

    // Stepping back into the open, the player stands up on their own.
    harness.player.Teleport({0.0f, 0.05f, 0.0f});
    harness.Simulate(PlayerInput{}, 20);
    REQUIRE(harness.State().stance == PlayerStance::Standing);
    REQUIRE_FALSE(harness.State().stanceBlocked);
}

TEST_CASE("Player walks up stairs without jumping", "[player]")
{
    PlayerHarness harness;

    // Ten 0.18 m steps climbing along -Z, which is the direction "forward" faces at yaw 0.
    const MeshData stairs = Primitives::Stairs(10, 4.0f, 0.18f, 0.30f);
    Transform stairTransform;
    stairTransform.position = {0.0f, 0.0f, -2.0f};
    stairTransform.rotation = glm::angleAxis(glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    harness.physics.CreateMeshBody(stairs, stairTransform);

    harness.Spawn();
    harness.Settle();
    const float startY = harness.State().position.y;

    // Track the highest point reached *while still standing on something*. Walking off the top of
    // the staircase and dropping back to the floor would otherwise look like a failure to climb.
    float highestGroundedY = startY;
    for (int i = 0; i < 180; ++i)
    {
        harness.Simulate(ForwardInput(), 1);
        if (harness.State().grounded)
        {
            highestGroundedY = std::max(highestGroundedY, harness.State().position.y);
        }
    }

    // Ten steps of 0.18 m is 1.8 m of climb; reaching most of it proves stepping works.
    REQUIRE(highestGroundedY > startY + 1.0f);
}

TEST_CASE("Steep slopes are not walkable", "[player]")
{
    PlayerHarness harness;
    harness.config.maxSlopeAngle = 46.0f;

    // A 60 degree ramp, well beyond the walkable limit.
    const MeshData ramp = Primitives::Ramp(6.0f, 4.0f, 4.0f * std::tan(glm::radians(60.0f)));
    Transform rampTransform;
    rampTransform.position = {0.0f, 0.0f, -2.0f};
    rampTransform.rotation = glm::angleAxis(glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    harness.physics.CreateMeshBody(ramp, rampTransform);

    harness.Spawn();
    harness.Settle();
    const float startY = harness.State().position.y;

    harness.Simulate(ForwardInput(), 180);

    // The player should be stopped by it, not stroll up a 60 degree face.
    REQUIRE(harness.State().position.y < startY + 0.6f);
}

TEST_CASE("Falling far enough causes damage, and a short drop does not", "[player]")
{
    SECTION("short drop is harmless")
    {
        PlayerHarness harness;
        harness.Spawn({0.0f, 1.5f, 0.0f});
        harness.Simulate(PlayerInput{}, 180);
        REQUIRE(harness.State().grounded);
        REQUIRE(harness.State().health == Catch::Approx(100.0f));
        REQUIRE(harness.State().alive);
    }

    SECTION("long drop hurts")
    {
        PlayerHarness harness;
        harness.Spawn({0.0f, 14.0f, 0.0f});
        harness.Simulate(PlayerInput{}, 300);
        REQUIRE(harness.State().grounded);
        REQUIRE(harness.State().health < 100.0f);
        REQUIRE(harness.State().landingImpactSpeed > harness.config.fallDamageMinSpeed);
    }

    SECTION("fall damage can be turned off")
    {
        PlayerHarness harness;
        harness.config.fallDamageEnabled = false;
        harness.Spawn({0.0f, 14.0f, 0.0f});
        harness.Simulate(PlayerInput{}, 300);
        REQUIRE(harness.State().health == Catch::Approx(100.0f));
    }
}

TEST_CASE("Respawn restores health, position and stance", "[player]")
{
    PlayerHarness harness;
    harness.Spawn({0.0f, 20.0f, 0.0f});
    harness.Simulate(PlayerInput{}, 300);
    REQUIRE(harness.State().health < 100.0f);

    harness.player.Respawn({3.0f, 0.05f, 4.0f});
    REQUIRE(harness.State().health == Catch::Approx(100.0f));
    REQUIRE(harness.State().alive);
    REQUIRE(harness.State().stance == PlayerStance::Standing);
    REQUIRE(harness.State().position.x == Catch::Approx(3.0f));
    REQUIRE(harness.State().position.z == Catch::Approx(4.0f));
}

TEST_CASE("Player config selects the right speed for each stance", "[player][config]")
{
    PlayerConfig config;
    REQUIRE(config.SpeedForStance(PlayerStance::Standing, false, false) == config.moveSpeed);
    REQUIRE(config.SpeedForStance(PlayerStance::Standing, true, false) == config.sprintSpeed);
    REQUIRE(config.SpeedForStance(PlayerStance::Standing, false, true) == config.walkSpeed);
    // Slow walk beats sprint when both are asked for.
    REQUIRE(config.SpeedForStance(PlayerStance::Standing, true, true) == config.walkSpeed);
    // Sprint and walk are meaningless off the feet.
    REQUIRE(config.SpeedForStance(PlayerStance::Crouching, true, false) == config.crouchSpeed);
    REQUIRE(config.SpeedForStance(PlayerStance::Prone, true, false) == config.proneSpeed);

    REQUIRE(config.HeightForStance(PlayerStance::Standing) == config.standHeight);
    REQUIRE(config.HeightForStance(PlayerStance::Crouching) == config.crouchHeight);
    REQUIRE(config.HeightForStance(PlayerStance::Prone) == config.proneHeight);
}

TEST_CASE("Player config round-trips through JSON", "[player][config]")
{
    PlayerConfig original;
    original.moveSpeed = 4.25f;
    original.sprintSpeed = 7.5f;
    original.jumpHeight = 1.33f;
    original.crouchHeight = 1.05f;
    original.bobAmount = 0.055f;
    original.fallDamageEnabled = false;

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "predation_player_config_test.json";
    REQUIRE(original.SaveToFile(file));

    PlayerConfig loaded;
    REQUIRE(loaded.LoadFromFile(file));
    REQUIRE(loaded.moveSpeed == Catch::Approx(original.moveSpeed));
    REQUIRE(loaded.sprintSpeed == Catch::Approx(original.sprintSpeed));
    REQUIRE(loaded.jumpHeight == Catch::Approx(original.jumpHeight));
    REQUIRE(loaded.crouchHeight == Catch::Approx(original.crouchHeight));
    REQUIRE(loaded.bobAmount == Catch::Approx(original.bobAmount));
    REQUIRE(loaded.fallDamageEnabled == false);

    std::filesystem::remove(file);
}

TEST_CASE("A partial player config overrides only the fields it names", "[player][config]")
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "predation_player_partial_test.json";
    {
        std::ofstream stream(file);
        stream << R"({"speeds": {"sprint": 9.5}})";
    }

    PlayerConfig config;
    const float defaultMove = config.moveSpeed;
    REQUIRE(config.LoadFromFile(file));
    REQUIRE(config.sprintSpeed == Catch::Approx(9.5f));
    REQUIRE(config.moveSpeed == Catch::Approx(defaultMove));

    std::filesystem::remove(file);
}

TEST_CASE("An attached player holds still inside geometry that would push them out", "[player][attach]")
{
    // Hiding in a locker. The shell is barely wider than the capsule, so a player left to simulate
    // normally gets pushed out sideways by depenetration however still they are asked to stand, and
    // slowly drifts through the wall. Attaching suspends movement instead of feeding it zero input,
    // which is the same mechanism a creature will use when it carries someone off.
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    physics.CreateBox({20.0f, 0.5f, 20.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);

    PlayerConfig config;
    // A box narrower than the capsule is wide, which is the situation that caused the drift.
    const float clearance = config.radius * 0.8f;
    physics.CreateBox({0.06f, 1.0f, 0.5f}, Transform{{-clearance, 1.0f, 0.0f}}, BodyMotion::Static);
    physics.CreateBox({0.06f, 1.0f, 0.5f}, Transform{{clearance, 1.0f, 0.0f}}, BodyMotion::Static);
    physics.OptimizeBroadPhase();

    PlayerController player;
    REQUIRE(player.Init(physics, config, {0.0f, 4.0f, 0.0f}));

    const glm::vec3 inside{0.0f, 0.0f, 0.0f};
    player.Attach(inside, 0.0f);
    REQUIRE(player.IsAttached());

    PlayerInput input;
    for (int i = 0; i < 300; ++i)
    {
        player.Step(input, 1.0f / 60.0f);
        physics.Step(1.0f / 60.0f);
    }

    INFO("ended at " << player.State().position.x << ", " << player.State().position.y << ", "
                     << player.State().position.z);
    REQUIRE(glm::length(player.State().position - inside) < 0.001f);
    REQUIRE(glm::length(player.State().velocity) < 0.001f);

    // Letting go hands control back, and the player falls out of the gap under gravity.
    player.Detach({0.0f, 0.0f, -2.0f});
    REQUIRE_FALSE(player.IsAttached());
    for (int i = 0; i < 60; ++i)
    {
        player.Step(input, 1.0f / 60.0f);
        physics.Step(1.0f / 60.0f);
    }
    REQUIRE(player.State().position.z == Catch::Approx(-2.0f).margin(0.1));

    player.Shutdown();
    physics.Shutdown();
}

TEST_CASE("Mantling climbs the ledges it should and refuses the rest", "[player][mantle]")
{
    // The test map has ledges from 0.3 to 1.8 m for exactly this. Below the step height the
    // character walks up without noticing; above chest height there is nothing to pull against.
    // What matters is that the boundaries are where the config says, not where the code drifts to.
    const auto climb = [](float ledgeHeight)
    {
        PhysicsWorld physics;
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        physics.CreateBox({20.0f, 0.5f, 20.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);
        // A block in front, long enough that walking on after getting up never reaches the far
        // edge. A short one measures where the test ends rather than where the climb does.
        physics.CreateBox({2.0f, ledgeHeight * 0.5f, 9.0f},
                          Transform{{0.0f, ledgeHeight * 0.5f, -10.0f}}, BodyMotion::Static);
        physics.OptimizeBroadPhase();

        PlayerConfig config;
        PlayerController player;
        REQUIRE(player.Init(physics, config, {0.0f, 0.05f, -0.6f}));

        PlayerInput input;
        input.move = {0.0f, 1.0f}; // forward is -Z at yaw 0, straight at the block

        // Walk into it, then press jump.
        for (int i = 0; i < 120; ++i)
        {
            physics.Step(1.0f / 60.0f);
            player.Step(input, 1.0f / 60.0f);
        }
        input.jump = true;
        physics.Step(1.0f / 60.0f);
        player.Step(input, 1.0f / 60.0f);
        input.jump = false;

        const bool started = player.State().mantling;
        for (int i = 0; i < 180; ++i)
        {
            physics.Step(1.0f / 60.0f);
            player.Step(input, 1.0f / 60.0f);
        }

        const float ended = player.State().position.y;
        player.Shutdown();
        physics.Shutdown();
        return std::make_pair(started, ended);
    };

    SECTION("a knee-high step is walked up, not climbed")
    {
        const auto [started, height] = climb(0.30f);
        INFO("ended at " << height);
        CHECK_FALSE(started);
        CHECK(height > 0.25f); // it still got up there, by stepping
    }

    SECTION("a waist-high ledge is climbed")
    {
        const auto [started, height] = climb(1.00f);
        INFO("ended at " << height);
        CHECK(started);
        CHECK(height > 0.95f);
    }

    SECTION("a chest-high ledge is climbed")
    {
        const auto [started, height] = climb(1.50f);
        INFO("ended at " << height);
        CHECK(started);
        CHECK(height > 1.45f);
    }

    SECTION("a wall above head height is not")
    {
        const auto [started, height] = climb(2.40f);
        INFO("ended at " << height);
        CHECK_FALSE(started);
        CHECK(height < 0.5f);
    }
}

TEST_CASE("A climb takes time and cannot be steered out of", "[player][mantle]")
{
    // The cost of the shortcut is that you are committed. If a climb could be cancelled or steered
    // it would be strictly better than walking round, and nobody would ever walk round.
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    physics.CreateBox({20.0f, 0.5f, 20.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);
    physics.CreateBox({2.0f, 0.6f, 2.0f}, Transform{{0.0f, 0.6f, -3.0f}}, BodyMotion::Static);
    physics.OptimizeBroadPhase();

    PlayerConfig config;
    PlayerController player;
    REQUIRE(player.Init(physics, config, {0.0f, 0.05f, -0.6f}));

    PlayerInput input;
    input.move = {0.0f, 1.0f};
    for (int i = 0; i < 120; ++i)
    {
        physics.Step(1.0f / 60.0f);
        player.Step(input, 1.0f / 60.0f);
    }
    input.jump = true;
    physics.Step(1.0f / 60.0f);
    player.Step(input, 1.0f / 60.0f);
    REQUIRE(player.State().mantling);

    // Let go of everything and try to walk backwards out of it.
    input.jump = false;
    input.move = {0.0f, -1.0f};

    int ticks = 0;
    float highest = player.State().position.y;
    while (player.State().mantling && ticks < 240)
    {
        physics.Step(1.0f / 60.0f);
        player.Step(input, 1.0f / 60.0f);
        highest = std::max(highest, player.State().position.y);
        ++ticks;
    }

    const float seconds = static_cast<float>(ticks) / 60.0f;
    INFO("climb took " << seconds << " s and reached " << highest);
    CHECK(seconds > 0.2f);
    CHECK(seconds < 1.2f);
    CHECK(highest > 1.15f); // it finished the climb despite being told to go the other way

    player.Shutdown();
    physics.Shutdown();
}

TEST_CASE("Stances can be changed in the air", "[player][stance]")
{
    // Crouching and going prone mid-jump is something people try, and there is no reason to refuse
    // it: the capsule is shrinking, and shrinking cannot be blocked.
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    physics.CreateBox({20.0f, 0.5f, 20.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);
    physics.OptimizeBroadPhase();

    PlayerConfig config;
    PlayerController player;
    REQUIRE(player.Init(physics, config, {0.0f, 0.05f, 0.0f}));

    PlayerInput input;
    for (int i = 0; i < 30; ++i)
    {
        physics.Step(1.0f / 60.0f);
        player.Step(input, 1.0f / 60.0f);
    }

    input.jump = true;
    physics.Step(1.0f / 60.0f);
    player.Step(input, 1.0f / 60.0f);
    input.jump = false;

    // A few ticks into the jump, well clear of the floor.
    for (int i = 0; i < 8; ++i)
    {
        physics.Step(1.0f / 60.0f);
        player.Step(input, 1.0f / 60.0f);
    }
    REQUIRE_FALSE(player.State().grounded);

    input.crouchHeld = true;
    physics.Step(1.0f / 60.0f);
    player.Step(input, 1.0f / 60.0f);
    CHECK(player.State().stance == PlayerStance::Crouching);

    input.crouchHeld = false;
    input.proneHeld = true;
    physics.Step(1.0f / 60.0f);
    player.Step(input, 1.0f / 60.0f);
    CHECK(player.State().stance == PlayerStance::Prone);

    player.Shutdown();
    physics.Shutdown();
}

TEST_CASE("A refused stance change says so", "[player][stance]")
{
    // The game keeps its toggle in step with the body by watching this. Without it a refused stand
    // still flipped the toggle, the button and the body disagreed, and the next press asked for the
    // stance you were already in.
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    physics.CreateBox({20.0f, 0.5f, 20.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);
    // A lintel low enough to crouch under and not to stand under.
    physics.CreateBox({2.0f, 0.2f, 2.0f}, Transform{{0.0f, 1.4f, 0.0f}}, BodyMotion::Static);
    physics.OptimizeBroadPhase();

    PlayerConfig config;
    PlayerController player;
    REQUIRE(player.Init(physics, config, {0.0f, 0.05f, 0.0f}));

    PlayerInput input;
    input.crouchHeld = true;
    for (int i = 0; i < 30; ++i)
    {
        physics.Step(1.0f / 60.0f);
        player.Step(input, 1.0f / 60.0f);
    }
    REQUIRE(player.State().stance == PlayerStance::Crouching);
    CHECK_FALSE(player.State().stanceBlocked);

    input.crouchHeld = false;
    for (int i = 0; i < 10; ++i)
    {
        physics.Step(1.0f / 60.0f);
        player.Step(input, 1.0f / 60.0f);
    }
    CHECK(player.State().stance == PlayerStance::Crouching);
    CHECK(player.State().stanceBlocked);

    player.Shutdown();
    physics.Shutdown();
}
