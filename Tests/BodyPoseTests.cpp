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
        // Below the eye and a little behind it, because a face is in front of a skull. The
        // horizontal part is measured along the body rather than the view, so looking around does
        // not swing the whole character; only turning the body moves it.
        const glm::vec3 eye = harness.View().eyePosition;

        INFO("stance " << PlayerStanceName(stance) << " eye " << harness.View().eyeHeight << " head "
                       << head.y << " pelvis " << pelvis.y);
        REQUIRE(std::abs(head.y - (eye.y - 0.085f)) < 0.01f);
        REQUIRE(glm::length(glm::vec2(head.x - eye.x, head.z - eye.z)) ==
                Catch::Approx(0.070f).margin(0.012));

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
        const glm::vec3 eye = harness.View().eyePosition;
        const float horizontal = glm::length(glm::vec2(head.x - eye.x, head.z - eye.z));
        INFO(testCase.name << ": head at " << head.x << ", " << head.y << ", " << head.z
                           << ", horizontally " << horizontal << " m from the eye");
        // The head sits a fixed distance behind the eye, because a face is in front of a skull.
        // What matters is that the distance is the same whichever way the player is travelling: it
        // used to slide off to one side while strafing, because the body was placed by a guessed
        // offset from the feet rather than by where the head actually ended up.
        REQUIRE(horizontal == Catch::Approx(0.070f).margin(0.012));
        REQUIRE(std::abs(head.y - (eye.y - 0.085f)) < 0.01f);
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
    //
    // Not exactly under it, though. The eye sits seven centimetres in front of the body, because a
    // chest is behind a face, and a body offset from the camera genuinely does move when it turns.
    // The bound is what that geometry allows over a turn in place, not zero.
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
    // Over a full turn the body traces a circle of radius eyeForwardOfHead about the eye, so the
    // furthest it can get from where it started is twice that. Seven centimetres of offset means
    // fourteen of travel, and anything much beyond that is the body sliding rather than turning.
    REQUIRE(worst < 0.16f);
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



TEST_CASE("The arms hang from the shoulders, not from the middle of the ribcage", "[body][pose]")
{
    // Standard anthropometry, as fractions of standing height: the shoulder joint is at 0.818, the
    // base of the neck around 0.825, and the fingertips of a hanging arm reach 0.377. The rig had
    // the shoulder at 0.732, which is inside the ribcage; the arms visibly grew out of the wrong
    // place and the hands hung past the knees.
    BodyHarness harness;
    harness.Settle(120);

    const float height = harness.config.standHeight;
    const float feet = harness.State().position.y;

    for (int side = 0; side < 2; ++side)
    {
        const float shoulder =
            (harness.Bone(harness.Rig().shoulder[static_cast<size_t>(side)]).y - feet) / height;
        INFO("side " << side << " shoulder at " << shoulder << " of standing height");
        CHECK(shoulder > 0.79f);
        CHECK(shoulder < 0.85f);
    }

    const float neck = (harness.Bone(harness.Rig().neck).y - feet) / height;
    INFO("neck base at " << neck);
    CHECK(neck > 0.80f);
    CHECK(neck < 0.87f);

    // The shoulder sits below the base of the neck, never above it.
    CHECK(harness.Bone(harness.Rig().shoulder[0]).y < harness.Bone(harness.Rig().neck).y);

    // And below the chin, which is where the drawn head starts.
    CHECK(harness.Bone(harness.Rig().neck).y < harness.Bone(harness.Rig().head).y);
}





TEST_CASE("Crawling backwards keeps the body facing forward", "[body][pose]")
{
    // Prone movement used to turn the body towards wherever it was travelling, so backing up
    // pivoted the whole character round to face its own heels and then crawled forwards while the
    // player slid the other way. It also read as being spun round every time you looked behind you
    // and backed off, because the pivot always went the same way.
    BodyHarness harness;
    harness.SetStance(PlayerStance::Prone);
    harness.Settle(300);

    const float facingBefore = harness.body.DebugBodyYaw();

    // Push straight backwards for two seconds.
    harness.input.move = {0.0f, -1.0f};
    harness.Settle(120);

    const float turned = std::abs(WrapAngle(harness.body.DebugBodyYaw() - facingBefore));
    INFO("body turned " << glm::degrees(turned) << " degrees while backing up");
    CHECK(turned < glm::radians(25.0f));

    // And it really did move backwards, rather than staying put.
    CHECK(harness.State().position.z > 0.15f);
}

TEST_CASE("The crawl cycle runs backwards when you back up", "[body][pose]")
{
    // The reach and pull has to reverse, or backing up plays a forward crawl while the body slides
    // the wrong way. The cycle is measured along the body, so its direction follows the movement.
    const auto handTravel = [](float forward)
    {
        BodyHarness harness;
        harness.SetStance(PlayerStance::Prone);
        harness.Settle(300);
        harness.input.move = {0.0f, forward};
        harness.Settle(40);

        // Where the left hand is relative to its shoulder, along the body, sampled over a stretch
        // of the cycle. Which way it sweeps is the direction the crawl is running.
        const glm::vec3 startHand = harness.Bone(harness.Rig().hand[0]);
        const glm::vec3 startShoulder = harness.Bone(harness.Rig().shoulder[0]);
        harness.Settle(20);
        const glm::vec3 endHand = harness.Bone(harness.Rig().hand[0]);
        const glm::vec3 endShoulder = harness.Bone(harness.Rig().shoulder[0]);
        return -((endHand.z - endShoulder.z) - (startHand.z - startShoulder.z));
    };

    const float forwards = handTravel(1.0f);
    const float backwards = handTravel(-1.0f);
    INFO("hand swept " << forwards << " crawling forwards and " << backwards << " backing up");
    CHECK(std::abs(forwards) > 1e-3f);
    CHECK(std::abs(backwards) > 1e-3f);
    CHECK(forwards * backwards < 0.0f);
}

TEST_CASE("The trigger hand stays on the weapon through a reload", "[body][pose][weapon]")
{
    // Writing a bone's global transform rebuilds every bone after it, and the left arm comes before
    // the right. Placing the support hand on the magazine after the trigger hand therefore threw
    // the right arm back onto its parent's pose and dropped it off the gun. The order is the fix,
    // so this measures the hand against the grip for the whole of a reload.
    BodyHarness harness;
    WeaponDefinition weapon;
    weapon.id = 1;
    weapon.key = "test_carbine";
    weapon.size = {0.06f, 0.16f, 0.62f};
    harness.body.SetWeaponForSimulation(&weapon);
    harness.Settle(120);

    float worst = 0.0f;
    float worstAt = 0.0f;
    for (int step = 0; step <= 20; ++step)
    {
        PlayerBody::WeaponPose pose;
        pose.reloading = true;
        pose.reload = static_cast<float>(step) / 20.0f;
        harness.body.SetWeaponPose(pose);
        harness.Tick();

        const glm::vec3 hand = harness.Bone(harness.Rig().hand[1]); // right, the trigger hand
        const float distance = glm::length(hand - harness.body.WeaponOrigin());
        if (distance > worst)
        {
            worst = distance;
            worstAt = pose.reload;
        }
    }

    INFO("trigger hand was " << worst << " m from the grip, worst at reload " << worstAt);
    CHECK(worst < 0.20f);
}

TEST_CASE("A dead body falls over and keeps its bones the length they were", "[body][ragdoll]")
{
    // The whole point of solving this with distance constraints is that limbs cannot stretch. A
    // ragdoll that pulls itself into spaghetti is the classic failure, so it is what gets measured.
    BodyHarness harness;
    harness.Settle(120);

    // Bone lengths before, so they can be checked after.
    std::vector<float> before;
    for (int i = 0; i < harness.body.GetSkeleton().BoneCount(); ++i)
    {
        const BoneIndex bone = static_cast<BoneIndex>(i);
        const BoneIndex parent = harness.body.GetSkeleton().GetBone(bone).parent;
        before.push_back(parent == kInvalidBone
                             ? 0.0f
                             : glm::distance(harness.Bone(bone), harness.Bone(parent)));
    }

    const float headBefore = harness.Bone(harness.Rig().head).y;
    harness.body.Collapse(glm::vec3(0.0f, 1.0f, -6.0f));
    CHECK(harness.body.IsCollapsed());

    for (int i = 0; i < 240; ++i)
    {
        harness.Tick();
    }

    // It went down.
    const float headAfter = harness.Bone(harness.Rig().head).y;
    INFO("head fell from " << headBefore << " to " << headAfter);
    CHECK(headAfter < headBefore - 0.6f);

    // And it did not sink through the floor.
    CHECK(headAfter > harness.State().position.y - 0.2f);

    // And every bone is still the length it was.
    float worstStretch = 0.0f;
    for (int i = 0; i < harness.body.GetSkeleton().BoneCount(); ++i)
    {
        const BoneIndex bone = static_cast<BoneIndex>(i);
        const BoneIndex parent = harness.body.GetSkeleton().GetBone(bone).parent;
        if (parent == kInvalidBone || before[static_cast<size_t>(i)] < 1e-3f)
        {
            continue;
        }
        const float now = glm::distance(harness.Bone(bone), harness.Bone(parent));
        worstStretch = std::max(worstStretch, std::abs(now - before[static_cast<size_t>(i)]));
    }
    INFO("worst bone length change " << worstStretch << " m");
    CHECK(worstStretch < 0.02f);
}

TEST_CASE("A ragdoll settles instead of being simulated for ever", "[body][ragdoll]")
{
    BodyHarness harness;
    harness.Settle(120);
    harness.body.Collapse(glm::vec3(0.0f, 0.0f, -3.0f));

    for (int i = 0; i < 600 && !harness.body.GetRagdoll().Settled(); ++i)
    {
        harness.Tick();
    }
    INFO("settled after " << harness.body.GetRagdoll().Age() << " seconds");
    CHECK(harness.body.GetRagdoll().Settled());
    CHECK(harness.body.GetRagdoll().Age() < 8.0f);
}

TEST_CASE("A body goes down the way it was hit", "[body][ragdoll]")
{
    // Forward is -Z, so a round from in front drives the body backwards, towards +Z.
    const auto fallDirection = [](const glm::vec3& impulse)
    {
        BodyHarness harness;
        harness.Settle(120);
        const glm::vec3 start = harness.Bone(harness.Rig().chest);
        harness.body.Collapse(impulse);
        for (int i = 0; i < 120; ++i)
        {
            harness.Tick();
        }
        return harness.Bone(harness.Rig().chest) - start;
    };

    const glm::vec3 pushedBack = fallDirection(glm::vec3(0.0f, 1.0f, 8.0f));
    const glm::vec3 pushedForward = fallDirection(glm::vec3(0.0f, 1.0f, -8.0f));
    INFO("back " << pushedBack.z << ", forward " << pushedForward.z);
    CHECK(pushedBack.z > 0.15f);
    CHECK(pushedForward.z < -0.15f);
}

TEST_CASE("The drawn skull sits over the neck, not behind it", "[body][pose]")
{
    // The head bone is anchored to the camera, and the skull is drawn at an offset from it. Get
    // that offset wrong and the head reads as detached, floating behind the shoulders, which is
    // exactly what it looked like.
    BodyHarness harness;
    harness.Settle(180);

    const glm::vec3 skull = harness.body.DebugSkullCentre();
    const glm::vec3 neck = harness.Bone(harness.Rig().neck);
    const float behind = skull.z - neck.z; // forward is -Z, so positive is behind

    INFO("skull is " << behind << " m behind the neck, " << (skull.y - neck.y) << " m above it");
    CHECK(std::abs(behind) < 0.02f);
    // And above it, because that is where a head goes.
    CHECK(skull.y > neck.y);
}

TEST_CASE("A prone body turned right round shuffles after you without flipping", "[body][pose]")
{
    // Rolling onto your back is gone. What is left has to be smooth: the body pivots on its front
    // to catch up, and nothing may jump. Parts flipping end for end part way through a turn is the
    // thing this measures, because that is what it looked like.
    BodyHarness harness;
    harness.SetStance(PlayerStance::Prone);
    harness.Settle(300);

    const auto chestFacing = [&]
    { return glm::normalize(-glm::vec3(harness.body.GetPose().Global(harness.Rig().chest)[2])); };
    const auto headFacing = [&]
    { return glm::normalize(-glm::vec3(harness.body.GetPose().Global(harness.Rig().head)[2])); };

    glm::vec3 previousChest = chestFacing();
    glm::vec3 previousHead = headFacing();
    float worstChestStep = 0.0f;
    float worstHeadStep = 0.0f;

    // Turn all the way round, the way a mouse does.
    for (int i = 0; i < 400; ++i)
    {
        harness.input.yaw = WrapAngle(harness.input.yaw + glm::radians(120.0f) * kTick);
        harness.Tick();
        worstChestStep = std::max(worstChestStep, glm::distance(chestFacing(), previousChest));
        worstHeadStep = std::max(worstHeadStep, glm::distance(headFacing(), previousHead));
        previousChest = chestFacing();
        previousHead = headFacing();
    }

    INFO("worst single-tick change: chest " << worstChestStep << ", head " << worstHeadStep);
    // A flip is a change of about two units in one tick. Turning at 120 degrees a second is two
    // degrees a tick, which is well under a tenth.
    CHECK(worstChestStep < 0.15f);
    CHECK(worstHeadStep < 0.15f);
}

TEST_CASE("A prone body ends up facing where you turned", "[body][pose]")
{
    BodyHarness harness;
    harness.SetStance(PlayerStance::Prone);
    harness.Settle(300);

    const float before = harness.body.DebugBodyYaw();
    harness.input.yaw = WrapAngle(before + glm::radians(150.0f));
    harness.Settle(400);

    const float turned = WrapAngle(harness.body.DebugBodyYaw() - before);
    INFO("body turned " << glm::degrees(turned) << " degrees");
    // It shuffles round to within the slack it is allowed to leave, and it goes the way you turned.
    CHECK(turned > glm::radians(60.0f));
    CHECK(turned < glm::radians(160.0f));
}

TEST_CASE("The torso sits behind the eye, the way a chest sits behind a face", "[body][pose]")
{
    // With the body directly under the camera, looking down showed the top of the shoulder yoke.
    // A person looking down sees the front of their chest and their boots, because their eyes are
    // in front of their torso.
    BodyHarness harness;
    harness.Settle(180);

    const glm::vec3 eye = harness.View().eyePosition;
    const glm::vec3 chest = harness.Bone(harness.Rig().chest);
    const glm::vec3 pelvis = harness.Bone(harness.Rig().pelvis);

    // Forward is -Z at yaw 0, so behind the eye is a larger z.
    INFO("chest is " << (chest.z - eye.z) << " m behind the eye, pelvis " << (pelvis.z - eye.z));
    CHECK(chest.z - eye.z > 0.04f);
    CHECK(chest.z - eye.z < 0.18f);
    CHECK(pelvis.z - eye.z > 0.04f);
}

TEST_CASE("Turning your head does not drag the body sideways", "[body][pose]")
{
    // The reason the eye offset is measured along the body and not the view. Measuring it along the
    // view swings the whole body round the camera whenever you look about, which is the drift this
    // anchoring exists to remove.
    BodyHarness harness;
    harness.Settle(180);
    const glm::vec3 before = harness.Bone(harness.Rig().pelvis);

    // Look ninety degrees to the side, slowly, without moving.
    for (int i = 0; i < 90; ++i)
    {
        harness.input.yaw = glm::radians(static_cast<float>(i));
        harness.Tick();
    }

    const glm::vec3 after = harness.Bone(harness.Rig().pelvis);
    const float drift = glm::length(glm::vec2(after.x - before.x, after.z - before.z));
    INFO("pelvis drifted " << drift << " m while looking around");
    CHECK(drift < 0.09f);
}

TEST_CASE("Climbing puts both hands on the ledge", "[body][pose][mantle]")
{
    // The only part of a climb you can see from inside it. Without this the arms carried on doing
    // their walking swing while the body rose past a wall, which reads as being levitated.
    BodyHarness harness;
    harness.Settle(120);

    // Drive a climb directly: the harness has no ledge, and what is being measured is the pose.
    PlayerState& state = const_cast<PlayerState&>(harness.State());
    const glm::vec3 from = state.position;
    const glm::vec3 to = from + glm::vec3(0.0f, 1.0f, -0.9f);
    state.mantling = true;
    state.mantleTime = 0.0f;
    state.mantleDuration = 0.7f;
    state.mantleFrom = from;
    state.mantleTo = to;
    // The lip: where the wall face meets the top, well short of where the feet will land.
    const glm::vec3 edge = from + glm::vec3(0.0f, 1.0f, -0.35f);
    state.mantleEdge = edge;

    float closest = 100.0f;
    for (int i = 0; i < 20; ++i)
    {
        state.mantleTime = 0.7f * 0.3f; // early, while the hands are taking the weight
        harness.body.Update(harness.scene, state, harness.View(), harness.config, harness.physics,
                            kTick);
        for (int side = 0; side < 2; ++side)
        {
            const glm::vec3 hand = harness.Bone(harness.Rig().hand[side]);
            closest = std::min(closest, glm::length(glm::vec2(hand.y - edge.y, hand.z - edge.z)));
        }
    }

    INFO("nearest hand came within " << closest << " m of the ledge edge");
    CHECK(closest < 0.35f);
}

TEST_CASE("A ragdoll keeps its shape instead of folding into a knot", "[body][ragdoll]")
{
    // Distance constraints alone let a body fold through itself: elbows close to nothing, knees
    // invert, the chest packs into the hips, and a corpse becomes a heap. Joint limits are what
    // stop it, so what gets measured is that a settled body still occupies a body's worth of space.
    BodyHarness harness;
    harness.Settle(120);

    const float standingSpan =
        glm::distance(harness.Bone(harness.Rig().head), harness.Bone(harness.Rig().foot[0]));

    harness.body.Collapse(glm::vec3(0.0f, 1.0f, -5.0f));
    for (int i = 0; i < 420; ++i)
    {
        harness.Tick();
    }

    // Head to foot: a body lying down is about as long as it was standing, whatever shape it landed
    // in. Half that means it has folded up.
    const float span =
        glm::distance(harness.Bone(harness.Rig().head), harness.Bone(harness.Rig().foot[0]));
    INFO("standing span " << standingSpan << ", settled span " << span);
    CHECK(span > standingSpan * 0.62f);

    // Joints still open. A knee that has folded flat reads as zero here.
    for (int side = 0; side < 2; ++side)
    {
        const float knee = glm::distance(harness.Bone(harness.Rig().upperLeg[side]),
                                         harness.Bone(harness.Rig().foot[side]));
        const float elbow = glm::distance(harness.Bone(harness.Rig().shoulder[side]),
                                          harness.Bone(harness.Rig().hand[side]));
        INFO("side " << side << " hip to ankle " << knee << ", shoulder to hand " << elbow);
        CHECK(knee > 0.45f);
        CHECK(elbow > 0.28f);
    }

    // And the torso has not concertinaed.
    const float torso =
        glm::distance(harness.Bone(harness.Rig().pelvis), harness.Bone(harness.Rig().neck));
    INFO("pelvis to neck " << torso);
    CHECK(torso > 0.42f);
}
