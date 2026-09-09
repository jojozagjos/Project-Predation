#include "Engine/Animation/IK.h"

#include <glm/matrix.hpp>
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Player/PlayerBody.h"
#include "Game/Player/PlayerController.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace pred;

// Poses are checked numerically rather than by looking at screenshots. Every bug in this area so
// far has been a sign error that produced a plausible-looking but anatomically backwards pose, and
// eyeballing a box figure is exactly how those survived.
namespace
{

constexpr float kTick = 1.0f / 60.0f;

// Drives the body directly, with no renderer and no meshes.
struct BodyHarness
{
    PhysicsWorld physics;
    Scene scene;
    PlayerBody body;
    PlayerConfig config;
    PlayerState state;
    PlayerView view;

    BodyHarness()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        physics.CreateBox({60.0f, 0.5f, 60.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);
        physics.OptimizeBroadPhase();

        body.BuildForSimulation(config);

        state.grounded = true;
        state.position = glm::vec3(0.0f);
        view.renderPosition = glm::vec3(0.0f);
        view.eyeHeight = config.standEyeHeight;
        view.eyePosition = glm::vec3(0.0f, config.standEyeHeight, 0.0f);
    }

    ~BodyHarness() { physics.Shutdown(); }

    // One simulation tick. The eye moves with the stance, exactly as PlayerController drives it.
    // That coupling is not incidental: the body anchors its head to the eye, so feeding a standing
    // eye height while asking for a crouch describes a posture nobody can adopt, and the resulting
    // pose is meaningless.
    void Tick(PlayerStance stance)
    {
        state.stance = stance;
        view.eyeHeight = SmoothTowards(view.eyeHeight, config.EyeHeightForStance(stance),
                                       config.eyeTransitionSpeed, kTick);
        view.eyePosition = view.renderPosition + glm::vec3(0.0f, view.eyeHeight, 0.0f);
        body.Update(scene, state, view, config, physics, kTick);
    }

    // Long enough for the stance blend to settle.
    void Settle(PlayerStance stance, int ticks = 240)
    {
        for (int i = 0; i < ticks; ++i)
        {
            Tick(stance);
        }
    }

    glm::vec3 Bone(BoneIndex index) const { return body.GetPose().GlobalPosition(index); }
    const HumanoidRig& Rig() const { return body.Rig(); }

    // Forward is -Z at yaw 0, so a larger forward offset means a more negative z.
    float ForwardOf(BoneIndex index) const { return -Bone(index).z; }
};

} // namespace

TEST_CASE("Standing holds the body upright over the feet", "[body][pose]")
{
    BodyHarness harness;
    harness.Settle(PlayerStance::Standing);

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
    harness.Settle(PlayerStance::Standing);
    const float standingPelvisY = harness.Bone(harness.Rig().pelvis).y;
    const float standingHeadY = harness.Bone(harness.Rig().head).y;

    harness.Settle(PlayerStance::Crouching);
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
    harness.Settle(PlayerStance::Standing);
    const float standingChestY = harness.Bone(harness.Rig().chest).y;

    harness.Settle(PlayerStance::Prone);
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
    harness.Settle(PlayerStance::Standing);

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
    harness.Settle(PlayerStance::Standing);

    // Looking straight down -Z while travelling to the right. A real person's hips follow where
    // they are going; their chest stays pointed at what they are looking at.
    harness.view.yaw = 0.0f;
    harness.state.velocity = glm::vec3(4.0f, 0.0f, 0.0f);
    harness.state.grounded = true;
    for (int i = 0; i < 240; ++i)
    {
        harness.body.Update(harness.scene, harness.state, harness.view, harness.config, harness.physics,
                            kTick);
    }

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
    harness.Settle(PlayerStance::Prone);

    // Crawl forward. Stride distance is what drives the cycle, so it has to advance.
    harness.state.velocity = glm::vec3(0.0f, 0.0f, -0.7f);
    harness.state.grounded = true;

    float minHandForward = 1e9f;
    float maxHandForward = -1e9f;
    float highestHand = -1e9f;

    for (int i = 0; i < 400; ++i)
    {
        harness.state.strideDistance += 0.7f * kTick;
        harness.body.Update(harness.scene, harness.state, harness.view, harness.config, harness.physics,
                            kTick);

        const glm::vec3 hand = harness.Bone(harness.Rig().hand[0]);
        const float forward = -hand.z;
        minHandForward = std::min(minHandForward, forward);
        maxHandForward = std::max(maxHandForward, forward);
        highestHand = std::max(highestHand, hand.y);
    }

    // The hand must actually travel fore and aft, which is what pulls the body along. A static
    // hand would mean the crawl is not animating at all.
    REQUIRE(maxHandForward - minHandForward > 0.15f);
    // And stay near the ground rather than waving in the air.
    REQUIRE(highestHand < 0.6f);

    // Hands reach out in front of the chest.
    REQUIRE(maxHandForward > harness.ForwardOf(harness.Rig().chest));
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
    harness.Settle(PlayerStance::Standing);
    const float standingPelvisY = harness.Bone(harness.Rig().pelvis).y;

    // A single tick must move only part of the way, or the transition would pop.
    harness.Tick(PlayerStance::Prone);
    const float afterOneTick = harness.Bone(harness.Rig().pelvis).y;
    REQUIRE(afterOneTick < standingPelvisY);
    REQUIRE(afterOneTick > standingPelvisY - 0.25f);

    // And it must actually arrive.
    harness.Settle(PlayerStance::Prone);
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
        harness.Settle(stance);

        const glm::vec3 head = harness.Bone(harness.Rig().head);
        const glm::vec3 pelvis = harness.Bone(harness.Rig().pelvis);
        const glm::vec3 facing{std::sin(harness.view.yaw), 0.0f, -std::cos(harness.view.yaw)};
        const glm::vec3 eye = head + facing * 0.085f + glm::vec3(0.0f, 0.085f, 0.0f);

        INFO("stance " << PlayerStanceName(stance) << " eye " << harness.view.eyeHeight << " head "
                       << head.y << " pelvis " << pelvis.y);
        REQUIRE(glm::distance(eye, harness.view.eyePosition) < 0.01f);

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
        harness.Settle(PlayerStance::Standing);
        harness.view.yaw = 0.0f;
        harness.state.velocity = testCase.velocity;
        harness.Settle(PlayerStance::Standing, 120);

        const glm::vec3 head = harness.Bone(harness.Rig().head);
        INFO(testCase.name << ": head at " << head.x << ", " << head.y << ", " << head.z);
        REQUIRE(std::abs(head.x - harness.view.eyePosition.x) < 0.01f);
        REQUIRE(std::abs(head.y - (harness.view.eyePosition.y - 0.085f)) < 0.01f);
    }
}

TEST_CASE("Walking backwards keeps the body facing the way the player looks", "[body][pose]")
{
    // Reversing away from the aim used to spin the character round to face its own heels, because
    // the hips chased the direction of travel rather than the line of it.
    BodyHarness harness;
    harness.Settle(PlayerStance::Standing);
    harness.view.yaw = 0.0f;
    harness.state.velocity = glm::vec3(0.0f, 0.0f, 4.0f); // straight backwards, since forward is -Z
    harness.Settle(PlayerStance::Standing, 240);

    const auto facingOf = [&](BoneIndex bone)
    { return glm::normalize(-glm::vec3(harness.body.GetPose().Global(bone)[2])); };

    // The hips still point roughly where the player is looking, not 180 degrees away from it.
    REQUIRE(-facingOf(harness.Rig().pelvis).z > 0.5f);
    REQUIRE(-facingOf(harness.Rig().chest).z > 0.8f);
}
