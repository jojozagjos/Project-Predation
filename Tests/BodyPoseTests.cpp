#include "Engine/Animation/IK.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Player/PlayerBody.h"
#include "Game/Player/PlayerController.h"
#include "Game/Weapons/WeaponAppearance.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace pred;

// Poses are checked numerically rather than by looking at screenshots. Every bug in this area so
// far has been a sign error that produced a plausible-looking but anatomically backwards pose, and
// eyeballing a box figure is exactly how those survived.
namespace
{

constexpr float kTick = 1.0f / 60.0f;

float WrapAngle(float radians)
{
    while (radians > glm::pi<float>())
    {
        radians -= glm::two_pi<float>();
    }
    while (radians <= -glm::pi<float>())
    {
        radians += glm::two_pi<float>();
    }
    return radians;
}

// Runs the real controller and feeds its output to the body, with no renderer and no meshes.
//
// It used to fake the state instead: velocity set by hand, position and stride left at zero, the
// eye parked at standing height. That made every gait test meaningless, because the walk cycle
// never advanced, and it hid a geometric impossibility: the body anchors itself to the eye, and
// with the eye at full standing height a leg is exactly long enough to reach the ground straight
// down and no further. Only the controller's footfall dip gives the legs any room to step, so the
// controller has to be in the loop.
struct BodyHarness
{
    PhysicsWorld physics;
    Scene scene;
    PlayerBody body;
    PlayerController player;
    PlayerConfig config;
    PlayerInput input;

    BodyHarness()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        physics.CreateBox({60.0f, 0.5f, 60.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);
        physics.OptimizeBroadPhase();

        body.BuildForSimulation(config);
        REQUIRE(player.Init(physics, config, {0.0f, 0.05f, 0.0f}));
    }

    ~BodyHarness()
    {
        player.Shutdown();
        physics.Shutdown();
    }

    void Tick()
    {
        player.Step(input, kTick);
        physics.Step(kTick);
        player.UpdateView(kTick, 1.0f);
        body.Update(scene, player.State(), player.View(), config, physics, kTick);
    }

    // Long enough for a stance change or a change of direction to settle.
    void Settle(int ticks = 240)
    {
        for (int i = 0; i < ticks; ++i)
        {
            Tick();
        }
    }

    void SetStance(PlayerStance stance)
    {
        input.crouchHeld = stance == PlayerStance::Crouching;
        input.proneHeld = stance == PlayerStance::Prone;
    }

    // Moves in a world direction, whatever way the player happens to be facing.
    void SetTravel(const glm::vec3& direction)
    {
        if (glm::length(direction) < 1e-4f)
        {
            input.move = glm::vec2(0.0f);
            return;
        }
        const glm::vec3 forward{std::sin(input.yaw), 0.0f, -std::cos(input.yaw)};
        const glm::vec3 right{std::cos(input.yaw), 0.0f, std::sin(input.yaw)};
        const glm::vec3 unit = glm::normalize(direction);
        input.move = glm::vec2(glm::dot(unit, right), glm::dot(unit, forward));
    }

    const PlayerState& State() const { return player.State(); }
    const PlayerView& View() const { return player.View(); }

    glm::vec3 Bone(BoneIndex index) const { return body.GetPose().GlobalPosition(index); }
    const HumanoidRig& Rig() const { return body.Rig(); }

    // Where a bone sits relative to the player, so a moving character does not swamp the reading.
    glm::vec3 Local(BoneIndex index) const { return Bone(index) - State().position; }

    // Forward is -Z at yaw 0, so a larger forward offset means a more negative z.
    float ForwardOf(BoneIndex index) const { return -Local(index).z; }
};

} // namespace

TEST_CASE("Standing holds the body upright over the feet", "[body][pose]")
{
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle();

    const glm::vec3 pelvis = harness.Bone(harness.Rig().pelvis);
    const glm::vec3 chest = harness.Bone(harness.Rig().chest);
    const glm::vec3 head = harness.Bone(harness.Rig().head);

    REQUIRE(pelvis.y == Catch::Approx(0.53f * harness.config.standHeight).margin(0.06));
    REQUIRE(chest.y > pelvis.y);
    REQUIRE(head.y > chest.y);

    // Upright means the chest sits essentially over the pelvis, not in front of or behind it.
    REQUIRE(std::abs(harness.ForwardOf(harness.Rig().chest) - harness.ForwardOf(harness.Rig().pelvis)) <
            0.08f);
}

TEST_CASE("Crouching lowers the hips and folds the torso forwards, never backwards", "[body][pose]")
{
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle();
    const float standingPelvisY = harness.Bone(harness.Rig().pelvis).y;
    const float standingHeadY = harness.Bone(harness.Rig().head).y;

    harness.SetStance(PlayerStance::Crouching);
    harness.Settle();
    const glm::vec3 pelvis = harness.Bone(harness.Rig().pelvis);
    const glm::vec3 chest = harness.Bone(harness.Rig().chest);
    const glm::vec3 head = harness.Bone(harness.Rig().head);

    // Hips and head both come down.
    REQUIRE(pelvis.y < standingPelvisY - 0.15f);
    REQUIRE(head.y < standingHeadY - 0.2f);

    // The torso leans FORWARD over the hips. This is the assertion that catches the sign error that
    // made crouching arch the back and lean the chest behind the hips.
    REQUIRE(harness.ForwardOf(harness.Rig().chest) > harness.ForwardOf(harness.Rig().pelvis) + 0.04f);
    REQUIRE(head.y > chest.y); // still the right way up

    // Feet stay on the ground and roughly under the body, not flung out behind.
    for (int side = 0; side < 2; ++side)
    {
        const glm::vec3 foot = harness.Bone(harness.Rig().foot[side]);
        INFO("foot side " << side);
        REQUIRE(foot.y == Catch::Approx(harness.Rig().ankleHeight).margin(0.09));
        REQUIRE(std::abs(foot.z - pelvis.z) < 0.45f);
    }
}

TEST_CASE("Prone lays the body flat and face down with the legs trailing behind", "[body][pose]")
{
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle();
    const float standingChestY = harness.Bone(harness.Rig().chest).y;

    harness.SetStance(PlayerStance::Prone);
    harness.Settle();
    const glm::vec3 pelvis = harness.Bone(harness.Rig().pelvis);
    const glm::vec3 chest = harness.Bone(harness.Rig().chest);
    const glm::vec3 head = harness.Bone(harness.Rig().head);

    // Everything comes near the floor.
    REQUIRE(pelvis.y < 0.45f);
    REQUIRE(chest.y < 0.55f);
    REQUIRE(chest.y < standingChestY - 0.7f);

    // The spine runs forwards along the ground rather than standing up: the chest is well in front
    // of the pelvis, and barely above it.
    REQUIRE(harness.ForwardOf(harness.Rig().chest) > harness.ForwardOf(harness.Rig().pelvis) + 0.25f);
    REQUIRE(std::abs(chest.y - pelvis.y) < 0.22f);

    // Head ahead of the chest and still off the floor, which is what keeps it looking forward
    // instead of being driven into the ground.
    REQUIRE(harness.ForwardOf(harness.Rig().head) > harness.ForwardOf(harness.Rig().chest));
    REQUIRE(head.y > 0.05f);

    // Legs trail out behind, near the floor, not folded up into the air.
    for (int side = 0; side < 2; ++side)
    {
        const glm::vec3 knee = harness.Bone(harness.Rig().lowerLeg[side]);
        const glm::vec3 foot = harness.Bone(harness.Rig().foot[side]);
        INFO("leg side " << side);
        // Behind the hips: forward is -Z, so a trailing foot has a smaller forward offset.
        REQUIRE(-foot.z < harness.ForwardOf(harness.Rig().pelvis) - 0.3f);
        REQUIRE(foot.y < 0.35f);
        REQUIRE(knee.y < 0.5f);
    }
}

TEST_CASE("Arms hang clear of the legs", "[body][pose]")
{
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle();

    for (int side = 0; side < 2; ++side)
    {
        const glm::vec3 hand = harness.Bone(harness.Rig().hand[side]);
        const glm::vec3 knee = harness.Bone(harness.Rig().lowerLeg[side]);
        INFO("side " << side);
        // Hands must sit further out than the legs, or the arms pass through the thighs.
        REQUIRE(std::abs(hand.x) > std::abs(knee.x) + 0.04f);
        // And on the correct side of the body.
        REQUIRE((hand.x < 0.0f) == (knee.x < 0.0f));
    }
}

TEST_CASE("Strafing turns the hips while the torso stays aimed", "[body][pose]")
{
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle();

    // Looking straight down -Z while travelling to the right. A real person's hips follow where
    // they are going; their chest stays pointed at what they are looking at.
    harness.SetTravel(glm::vec3(1.0f, 0.0f, 0.0f));
    harness.Settle(240);

    // The local -Z axis of each bone is where that part of the body faces.
    const auto facingOf = [&](BoneIndex bone)
    { return glm::normalize(-glm::vec3(harness.body.GetPose().Global(bone)[2])); };

    const glm::vec3 hips = facingOf(harness.Rig().pelvis);
    const glm::vec3 chest = facingOf(harness.Rig().chest);

    // Hips swing towards the direction of travel, +X.
    REQUIRE(hips.x > 0.45f);
    // The chest stays much closer to the aim direction, -Z, than the hips do.
    REQUIRE(-chest.z > -hips.z + 0.25f);
    REQUIRE(chest.x < hips.x - 0.25f);
}

TEST_CASE("Crawling reaches the hands forward and cycles them", "[body][pose]")
{
    BodyHarness harness;
    harness.SetStance(PlayerStance::Prone);
    harness.Settle();

    harness.SetTravel(glm::vec3(0.0f, 0.0f, -1.0f));

    // Measured against the shoulder, not the world. The body is travelling, so a world-space range
    // would mostly be reporting how far the character moved.
    float minReach = 1e9f;
    float maxReach = -1e9f;
    float highestHand = -1e9f;
    float minKneeOut = 1e9f;
    float maxKneeOut = -1e9f;

    for (int i = 0; i < 400; ++i)
    {
        harness.Tick();

        const glm::vec3 shoulder = harness.Bone(harness.Rig().shoulder[0]);
        const glm::vec3 hand = harness.Bone(harness.Rig().hand[0]);
        const float reach = shoulder.z - hand.z; // forward is -Z, so positive means out in front
        minReach = std::min(minReach, reach);
        maxReach = std::max(maxReach, reach);
        highestHand = std::max(highestHand, hand.y - harness.State().position.y);

        // The knee has to swing out to the side and back as the leg is drawn up and pushed. A knee
        // that only moves fore and aft is the leg sliding, not crawling.
        const glm::vec3 knee = harness.Bone(harness.Rig().lowerLeg[0]);
        const float out = knee.x - harness.State().position.x;
        minKneeOut = std::min(minKneeOut, out);
        maxKneeOut = std::max(maxKneeOut, out);
    }

    // The hand must actually travel fore and aft, which is what pulls the body along. A static
    // hand would mean the crawl is not animating at all.
    REQUIRE(maxReach - minReach > 0.15f);
    // And stay near the ground rather than waving in the air.
    REQUIRE(highestHand < 0.6f);
    // Hands reach out in front of the shoulders, not behind them.
    REQUIRE(maxReach > 0.2f);
    // The knees work sideways as well as backwards.
    REQUIRE(maxKneeOut - minKneeOut > 0.08f);
}

TEST_CASE("Leaning rolls and shifts the view without moving the feet", "[player][lean]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    physics.CreateBox({60.0f, 0.5f, 60.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);
    physics.OptimizeBroadPhase();

    PlayerController player;
    PlayerConfig config;
    REQUIRE(player.Init(physics, config, {0.0f, 0.05f, 0.0f}));

    PlayerInput input;
    for (int i = 0; i < 30; ++i)
    {
        player.Step(input, kTick);
        physics.Step(kTick);
    }
    player.UpdateView(kTick, 1.0f);
    const glm::vec3 uprightEye = player.View().eyePosition;
    const glm::vec3 uprightFeet = player.State().position;

    input.lean = 1.0f;
    for (int i = 0; i < 90; ++i)
    {
        player.Step(input, kTick);
        physics.Step(kTick);
        player.UpdateView(kTick, 1.0f);
    }

    REQUIRE(player.State().leanAmount == Catch::Approx(1.0f).margin(0.05));

    // Direction matters, and testing the magnitude alone is how the camera came to roll the wrong
    // way unnoticed. At yaw 0 the player faces -Z, so their right hand is +X.
    REQUIRE(player.View().eyePosition.x - uprightEye.x > 0.2f);
    REQUIRE(player.View().leanRoll > glm::radians(10.0f));

    // Leaning right tips the head right, so the camera's own up axis tips towards +X with it.
    // Rolling the other way is what made the horizon fight the lean.
    const glm::vec3 up = glm::vec3(glm::inverse(player.View().ViewMatrix())[1]);
    INFO("camera up " << up.x << ", " << up.y << ", " << up.z);
    REQUIRE(up.x > 0.15f);
    REQUIRE(up.y > 0.8f);
    // The feet stay put; leaning is not a step.
    REQUIRE(glm::length(player.State().position - uprightFeet) < 0.05f);

    // Releasing returns to upright.
    input.lean = 0.0f;
    for (int i = 0; i < 120; ++i)
    {
        player.Step(input, kTick);
        physics.Step(kTick);
    }
    REQUIRE(player.State().leanAmount == Catch::Approx(0.0f).margin(0.05));

    player.Shutdown();
    physics.Shutdown();
}

TEST_CASE("Stance changes blend rather than snapping", "[body][pose]")
{
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle();
    const float standingPelvisY = harness.Bone(harness.Rig().pelvis).y;

    // A single tick must move only part of the way, or the transition would pop.
    harness.SetStance(PlayerStance::Prone);
    harness.Tick();
    const float afterOneTick = harness.Bone(harness.Rig().pelvis).y;
    REQUIRE(afterOneTick < standingPelvisY);
    REQUIRE(afterOneTick > standingPelvisY - 0.25f);

    // And it must actually arrive.
    harness.SetStance(PlayerStance::Prone);
    harness.Settle();
    REQUIRE(harness.Bone(harness.Rig().pelvis).y < 0.45f);
}

TEST_CASE("Every stance puts the head on the camera", "[body][pose]")
{
    // The whole first-person body rests on this: if the head bone is not where the eye is, the
    // camera is somewhere inside the model and the player sees the back of their own skull. It is
    // also the assertion that keeps the stance eye heights in player.json honest, because the body
    // now follows them rather than guessing its own.
    const PlayerStance stances[] = {PlayerStance::Standing, PlayerStance::Crouching, PlayerStance::Prone};
    for (const PlayerStance stance : stances)
    {
        BodyHarness harness;
        harness.SetStance(stance);
        harness.Settle();

        const glm::vec3 head = harness.Bone(harness.Rig().head);
        const glm::vec3 pelvis = harness.Bone(harness.Rig().pelvis);
        // Straight below the eye. The offset is deliberately vertical only: anything horizontal is
        // measured along the view, and swings the whole body when the player turns round.
        const glm::vec3 eye = head + glm::vec3(0.0f, 0.085f, 0.0f);

        INFO("stance " << PlayerStanceName(stance) << " eye " << harness.View().eyeHeight << " head "
                       << head.y << " pelvis " << pelvis.y);
        REQUIRE(glm::distance(eye, harness.View().eyePosition) < 0.01f);

        // And the legs must still be able to reach the ground from wherever that leaves the hips.
        for (int side = 0; side < 2; ++side)
        {
            const glm::vec3 foot = harness.Bone(harness.Rig().foot[side]);
            INFO("foot side " << side << " at " << foot.y);
            REQUIRE(foot.y < harness.Rig().ankleHeight + 0.12f);
        }
    }
}

TEST_CASE("The head stays on the camera through every direction of travel", "[body][pose]")
{
    // Strafing turns the hips away from the view, which used to slide the head off to one side of
    // the camera because the body was positioned by a fixed offset from the feet rather than by
    // where the head actually ended up. Walking backwards is the same failure from the other end.
    struct Case
    {
        const char* name;
        glm::vec3 velocity;
    };
    const Case cases[] = {{"strafe right", {4.0f, 0.0f, 0.0f}},
                          {"strafe left", {-4.0f, 0.0f, 0.0f}},
                          {"backwards", {0.0f, 0.0f, 4.0f}},
                          {"forwards", {0.0f, 0.0f, -4.0f}}};

    for (const Case& testCase : cases)
    {
        BodyHarness harness;
        harness.SetStance(PlayerStance::Standing);
    harness.Settle();
        harness.SetTravel(testCase.velocity);
        harness.SetStance(PlayerStance::Standing);
    harness.Settle(120);

        const glm::vec3 head = harness.Bone(harness.Rig().head);
        INFO(testCase.name << ": head at " << head.x << ", " << head.y << ", " << head.z);
        REQUIRE(std::abs(head.x - harness.View().eyePosition.x) < 0.01f);
        REQUIRE(std::abs(head.y - (harness.View().eyePosition.y - 0.085f)) < 0.01f);
        REQUIRE(std::abs(head.z - harness.View().eyePosition.z) < 0.01f);
    }
}

TEST_CASE("Walking backwards keeps the body facing the way the player looks", "[body][pose]")
{
    // Reversing away from the aim used to spin the character round to face its own heels, because
    // the hips chased the direction of travel rather than the line of it.
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle();
    harness.SetTravel(glm::vec3(0.0f, 0.0f, 4.0f)); // straight backwards, since forward is -Z
    harness.SetStance(PlayerStance::Standing);
    harness.Settle(240);

    const auto facingOf = [&](BoneIndex bone)
    { return glm::normalize(-glm::vec3(harness.body.GetPose().Global(bone)[2])); };

    // The hips still point roughly where the player is looking, not 180 degrees away from it.
    REQUIRE(-facingOf(harness.Rig().pelvis).z > 0.5f);
    REQUIRE(-facingOf(harness.Rig().chest).z > 0.8f);
}

TEST_CASE("A foot on the ground stays where it is put", "[body][gait]")
{
    // The single thing that separates a walk from a skate. A foot in contact must be still in world
    // space while the body travels past it; the old cycle swung the foot around the hip on a sine
    // wave, which slid it backwards along the ground at several times walking pace in every
    // direction of travel.
    struct Case
    {
        const char* name;
        glm::vec3 velocity;
    };
    const Case cases[] = {{"forwards", {0.0f, 0.0f, -3.4f}},
                          {"backwards", {0.0f, 0.0f, 3.4f}},
                          {"strafe right", {3.4f, 0.0f, 0.0f}},
                          {"diagonal", {2.4f, 0.0f, -2.4f}}};

    for (const Case& testCase : cases)
    {
        BodyHarness harness;
        harness.SetStance(PlayerStance::Standing);
    harness.Settle();
        harness.SetTravel(testCase.velocity);
        harness.SetStance(PlayerStance::Standing);
    harness.Settle(90); // let the gait reach a steady state

        const float bodyStep = glm::length(glm::vec2(testCase.velocity.x, testCase.velocity.z)) * kTick;

        // Only the ticks where the foot is actually touching the ground count. A swinging foot is
        // supposed to move; what must not move is one bearing weight.
        std::vector<float> slips;
        glm::vec3 previous = harness.Bone(harness.Rig().foot[0]);
        for (int i = 0; i < 240; ++i)
        {
            harness.Tick();
            const glm::vec3 current = harness.Bone(harness.Rig().foot[0]);
            if (current.y - harness.State().position.y < harness.Rig().ankleHeight + 0.02f)
            {
                slips.push_back(glm::length(glm::vec2(current.x - previous.x, current.z - previous.z)));
            }
            previous = current;
        }

        REQUIRE(slips.size() > 60); // a foot that is never down is not walking
        std::sort(slips.begin(), slips.end());
        const float median = slips[slips.size() / 2];

        INFO(testCase.name << ": median movement of a planted foot " << median
                           << " m/tick against a body step of " << bodyStep);
        REQUIRE(median < bodyStep * 0.15f);
    }
}


TEST_CASE("Feet stop at a wall instead of climbing it", "[body][gait]")
{
    // Walking into a wall used to put the foot target inside it, because a step reaches further
    // from the hip than the capsule the player is actually stopped by. The downward ground trace
    // then found the top of the wall, and the leg climbed three metres of it.
    BodyHarness harness;
    harness.physics.CreateBox({4.0f, 1.6f, 0.2f}, Transform{{0.0f, 1.6f, -1.0f}}, BodyMotion::Static);
    harness.physics.OptimizeBroadPhase();

    harness.SetStance(PlayerStance::Standing);
    harness.Settle(60);
    harness.SetTravel(glm::vec3(0.0f, 0.0f, -1.0f)); // straight at it
    harness.Settle(180);

    constexpr float kWallFace = -0.8f; // near face of a 0.4 m deep wall centred on z = -1
    for (int i = 0; i < 120; ++i)
    {
        harness.Tick();
        for (int side = 0; side < 2; ++side)
        {
            const glm::vec3 foot = harness.Bone(harness.Rig().foot[side]);
            INFO("foot " << side << " at " << foot.x << ", " << foot.y << ", " << foot.z);
            // Never standing part way up the wall.
            REQUIRE(foot.y < harness.State().position.y + 0.5f);
            // Never inside or beyond it.
            REQUIRE(foot.z > kWallFace);
        }
    }

    // And the player really did reach the wall, or the test proved nothing.
    REQUIRE(harness.State().position.z < 0.0f);
}

TEST_CASE("Looking behind while prone rolls the body onto its back", "[body][pose]")
{
    // A person on their belly cannot twist round to cover behind them, so they roll over. The body
    // keeps its own heading while prone and turns end over end when the view leaves what a neck can
    // reach, which is what makes lying down feel like lying down rather than standing up sideways.
    BodyHarness harness;
    harness.SetStance(PlayerStance::Prone);
    harness.Settle(300);

    const auto facingOf = [&](BoneIndex bone)
    { return glm::normalize(-glm::vec3(harness.body.GetPose().Global(bone)[2])); };

    INFO("face down chest facing y " << facingOf(harness.Rig().chest).y);
    REQUIRE(facingOf(harness.Rig().chest).y < -0.35f);

    // Look behind. The roll is a latched state, so it settles rather than flickering.
    harness.input.yaw = glm::pi<float>();
    harness.Settle(300);

    // The heading is unchanged: rolling over is a roll, not a turn. Turning the body end for end as
    // well snapped it round behind the player, and a half turn on top of a half roll leaves you
    // face down again pointing the other way.
    REQUIRE(std::abs(WrapAngle(harness.body.DebugBodyYaw())) < glm::radians(20.0f));

    const glm::vec3 chestFacing = facingOf(harness.Rig().chest);
    INFO("on back chest facing " << chestFacing.x << ", " << chestFacing.y << ", " << chestFacing.z);
    REQUIRE(chestFacing.y > 0.35f);

    // Still lying down, not sitting up or standing.
    const glm::vec3 chest = harness.Bone(harness.Rig().chest);
    const glm::vec3 pelvis = harness.Bone(harness.Rig().pelvis);
    INFO("chest y " << chest.y << " pelvis y " << pelvis.y);
    REQUIRE(std::abs(chest.y - pelvis.y) < 0.3f);
    REQUIRE(chest.y < 0.7f);

    // The head stayed where it was. Rolling over is a roll, not a turn: flipping the body end for
    // end as well is what left it face down again pointing the other way.
    const glm::vec3 head = harness.Bone(harness.Rig().head);
    const glm::vec3 toHead = head - pelvis;
    INFO("pelvis to head " << toHead.x << ", " << toHead.y << ", " << toHead.z);
    REQUIRE(toHead.z < -0.42f);

    // Legs trail behind, on the ground, rather than folding up under the body.
    for (int side = 0; side < 2; ++side)
    {
        const glm::vec3 foot = harness.Bone(harness.Rig().foot[side]);
        INFO("foot " << side << " at " << foot.x << ", " << foot.y << ", " << foot.z);
        REQUIRE(foot.y < harness.State().position.y + 0.35f);
        REQUIRE(foot.z > pelvis.z + 0.4f);
    }

    // And it settles: looking there does not make it roll back and forth for ever.
    const float settledY = chestFacing.y;
    harness.Settle(120);
    REQUIRE(facingOf(harness.Rig().chest).y == Catch::Approx(settledY).margin(0.1));
}

TEST_CASE("Airborne legs stop reaching for a floor that is not there", "[body][gait]")
{
    // Off the ground, the ground trace can be metres below, and reaching for it stretched both legs
    // into straight poles pointing at the floor. That is what a jump looked like. The feet are
    // placed relative to the hips instead once the ground is further away than a step.
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle(60);

    const float legSpan = harness.Rig().upperLegLength + harness.Rig().lowerLegLength;

    // Straight up, well clear of the ground.
    harness.player.Teleport({0.0f, 3.0f, 0.0f});
    harness.input.jump = true;
    harness.Settle(20);
    harness.input.jump = false;
    harness.Settle(20);

    REQUIRE_FALSE(harness.State().grounded);

    const glm::vec3 hip = harness.Bone(harness.Rig().upperLeg[0]);
    const glm::vec3 foot = harness.Bone(harness.Rig().foot[0]);
    const float drop = hip.y - foot.y;
    INFO("hip " << hip.y << " foot " << foot.y << " drop " << drop << " of a " << legSpan << " leg");

    // The leg is bent, not locked out reaching for the ground three metres down.
    REQUIRE(drop < legSpan * 0.97f);
    // And the foot is nowhere near the floor.
    REQUIRE(foot.y > 1.5f);
}

TEST_CASE("Turning on the spot leaves the body where it is", "[body][pose]")
{
    // In first person you look down and see your own body. Turning round must not slide it out from
    // under you: the hips swing to catch up, but the body stays under the head.
    BodyHarness harness;
    harness.SetStance(PlayerStance::Standing);
    harness.Settle(120);

    const glm::vec3 start = harness.Local(harness.Rig().pelvis);
    float worst = 0.0f;

    // A full turn, at a speed a mouse can easily produce.
    for (int i = 0; i < 240; ++i)
    {
        harness.input.yaw = WrapAngle(harness.input.yaw + glm::radians(1.5f));
        harness.Tick();
        const glm::vec3 pelvis = harness.Local(harness.Rig().pelvis);
        worst = std::max(worst, glm::length(glm::vec2(pelvis.x - start.x, pelvis.z - start.z)));
    }

    INFO("worst horizontal drift of the hips " << worst << " m");
    REQUIRE(worst < 0.02f);
}

TEST_CASE("The trigger hand keeps hold of the weapon while crawling", "[body][pose]")
{
    // Solving the arms in the wrong order let go of the gun the moment the player lay down. Writing
    // a bone's global transform rebuilds every bone after it, and the right arm comes after the
    // left, so the crawling arm has to be solved before the one holding the weapon.
    BodyHarness harness;
    WeaponDefinition weapon;
    weapon.id = 1;
    weapon.key = "test_rifle";
    weapon.size = {0.06f, 0.16f, 0.62f};

    harness.SetStance(PlayerStance::Prone);
    harness.Settle(240);
    harness.body.SetWeaponForSimulation(&weapon);
    harness.SetTravel(glm::vec3(0.0f, 0.0f, -1.0f));
    harness.Settle(180);

    // The trigger hand is the right one, and it must be on the weapon rather than out on the floor
    // with the other arm.
    const glm::vec3 hand = harness.Bone(harness.Rig().hand[1]);
    const glm::vec3 muzzle = harness.body.MuzzlePoint();
    const glm::vec3 grip = harness.body.WeaponOrigin();
    INFO("right hand " << hand.x << ", " << hand.y << ", " << hand.z << "  grip " << grip.x << ", "
                       << grip.y << ", " << grip.z);
    REQUIRE(glm::distance(hand, grip) < 0.12f);

    // And the weapon is out in front, not dropped on the ground behind.
    INFO("muzzle " << muzzle.x << ", " << muzzle.y << ", " << muzzle.z);
    REQUIRE(muzzle.z < grip.z);
}

TEST_CASE("Rolling over goes the way you turned", "[body][pose]")
{
    // Turning right and turning left have to put you over opposite shoulders. Rolling the same way
    // every time meant looking right threw the body over to the left.
    // The body's own right hand side is level at both ends of the roll and swings through the
    // vertical on the way, so the extreme it reaches part way through is which way it went.
    const auto rollDirection = [](float lookYaw)
    {
        BodyHarness harness;
        harness.SetStance(PlayerStance::Prone);
        harness.Settle(300);
        harness.input.yaw = lookYaw;
        float extreme = 0.0f;
        for (int i = 0; i < 200; ++i)
        {
            harness.Tick();
            const float side = glm::vec3(harness.body.GetPose().Global(harness.Rig().chest)[0]).y;
            if (std::abs(side) > std::abs(extreme))
            {
                extreme = side;
            }
        }
        return extreme;
    };

    const float right = rollDirection(glm::radians(170.0f));
    const float left = rollDirection(glm::radians(-170.0f));
    INFO("turning right rolled " << right << ", turning left rolled " << left);
    REQUIRE(std::abs(right) > 0.3f);
    REQUIRE(std::abs(left) > 0.3f);
    REQUIRE(right * left < 0.0f); // opposite shoulders
}

TEST_CASE("Rolling over is a movement, not a jump", "[body][pose]")
{
    // It has to take a moment and ease at both ends. Smoothing exponentially towards the target is
    // fastest at the start, which reads as a switch being thrown rather than a body turning over.
    BodyHarness harness;
    harness.SetStance(PlayerStance::Prone);
    harness.Settle(300);

    // Which way the chest faces: straight down on your front, straight up on your back.
    const auto chestFacing = [&]
    { return glm::normalize(-glm::vec3(harness.body.GetPose().Global(harness.Rig().chest)[2])).y; };
    const float before = chestFacing();

    harness.input.yaw = glm::pi<float>();
    float worstStep = 0.0f;
    float previous = before;
    int ticksMoving = 0;
    for (int i = 0; i < 120; ++i)
    {
        harness.Tick();
        const float now = chestFacing();
        const float step = std::abs(now - previous);
        worstStep = std::max(worstStep, step);
        if (step > 0.002f)
        {
            ++ticksMoving;
        }
        previous = now;
    }

    INFO("worst single-tick change " << worstStep << " over " << ticksMoving << " moving ticks");
    REQUIRE(ticksMoving > 20); // a third of a second at the very least
    // The facing swings through two units in total, so a single tick doing more than an eighth of
    // that is a jump rather than a roll. An instant flip would put the whole two in one tick.
    REQUIRE(worstStep < 0.25f);
}
