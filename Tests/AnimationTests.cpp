#include "Engine/Animation/IK.h"
#include "Engine/Animation/Skeleton.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace pred;

namespace
{

Transform Offset(float x, float y, float z)
{
    Transform transform;
    transform.position = {x, y, z};
    return transform;
}

// A three-bone chain running up the Y axis: root at the origin, then two 1 m segments.
Skeleton MakeChain()
{
    Skeleton skeleton;
    const BoneIndex root = skeleton.AddBone("root", kInvalidBone, Offset(0.0f, 0.0f, 0.0f));
    const BoneIndex middle = skeleton.AddBone("middle", root, Offset(0.0f, 1.0f, 0.0f));
    skeleton.AddBone("tip", middle, Offset(0.0f, 1.0f, 0.0f));
    return skeleton;
}

} // namespace

TEST_CASE("Skeleton keeps parents before children", "[animation][skeleton]")
{
    const Skeleton skeleton = MakeChain();
    REQUIRE(skeleton.BoneCount() == 3);
    REQUIRE(skeleton.ValidateOrdering());

    REQUIRE(skeleton.Find("middle") == 1);
    REQUIRE(skeleton.Find("nonexistent") == kInvalidBone);
    REQUIRE(skeleton.IsValid(0));
    REQUIRE_FALSE(skeleton.IsValid(3));
    REQUIRE_FALSE(skeleton.IsValid(kInvalidBone));
}

TEST_CASE("Pose accumulates global transforms down the hierarchy", "[animation][skeleton]")
{
    const Skeleton skeleton = MakeChain();
    Pose pose;
    pose.ResetToBind(skeleton);
    pose.ComputeGlobals(skeleton);

    REQUIRE(pose.GlobalPosition(0).y == Catch::Approx(0.0f));
    REQUIRE(pose.GlobalPosition(1).y == Catch::Approx(1.0f));
    REQUIRE(pose.GlobalPosition(2).y == Catch::Approx(2.0f));

    // The root transform moves the whole skeleton.
    pose.ComputeGlobals(skeleton, glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f)));
    REQUIRE(pose.GlobalPosition(2).x == Catch::Approx(5.0f));
    REQUIRE(pose.GlobalPosition(2).y == Catch::Approx(2.0f));
}

TEST_CASE("Rotating a parent carries its children with it", "[animation][skeleton]")
{
    const Skeleton skeleton = MakeChain();
    Pose pose;
    pose.ResetToBind(skeleton);

    // A quarter turn about Z maps the chain's +Y onto -X.
    pose.Local(0).rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    pose.ComputeGlobals(skeleton);

    REQUIRE(pose.GlobalPosition(2).x == Catch::Approx(-2.0f).margin(1e-4));
    REQUIRE(pose.GlobalPosition(2).y == Catch::Approx(0.0f).margin(1e-4));
}

TEST_CASE("Writing a global transform rebuilds the bones below it", "[animation][skeleton]")
{
    const Skeleton skeleton = MakeChain();
    Pose pose;
    pose.ResetToBind(skeleton);
    pose.ComputeGlobals(skeleton);

    // This is what the IK solver does: it produces global transforms, not local rotations.
    pose.SetGlobal(skeleton, 1, glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 4.0f, 0.0f)));

    REQUIRE(pose.GlobalPosition(1).x == Catch::Approx(10.0f));
    // The tip keeps its 1 m local offset, now measured from the relocated middle bone.
    REQUIRE(pose.GlobalPosition(2).x == Catch::Approx(10.0f));
    REQUIRE(pose.GlobalPosition(2).y == Catch::Approx(5.0f));
    // The root is above the edited bone in the hierarchy, so it must not have moved.
    REQUIRE(pose.GlobalPosition(0).x == Catch::Approx(0.0f));
}

TEST_CASE("Two-bone IK places the effector on reachable targets", "[animation][ik]")
{
    const glm::vec3 root{0.0f, 0.0f, 0.0f};
    const glm::vec3 pole{0.0f, 0.0f, 1.0f};
    constexpr float upper = 1.0f;
    constexpr float lower = 1.0f;

    const glm::vec3 targets[] = {{1.5f, 0.0f, 0.0f},  {0.0f, -1.2f, 0.0f}, {0.8f, -0.8f, 0.3f},
                                 {-1.0f, 0.5f, 0.5f}, {0.2f, 1.7f, 0.0f}};

    for (const glm::vec3& target : targets)
    {
        const TwoBoneIKResult result = SolveTwoBoneIK(root, target, pole, upper, lower);
        INFO("target " << target.x << ", " << target.y << ", " << target.z);

        REQUIRE(result.reachedTarget);
        REQUIRE(glm::length(result.endPosition - target) == Catch::Approx(0.0f).margin(2e-3));

        // Both bones must keep their length, which is the property that makes a limb look solid.
        REQUIRE(glm::length(result.jointPosition - result.rootPosition) == Catch::Approx(upper).margin(2e-3));
        REQUIRE(glm::length(result.endPosition - result.jointPosition) == Catch::Approx(lower).margin(2e-3));
    }
}

TEST_CASE("Two-bone IK stretches towards targets it cannot reach", "[animation][ik]")
{
    const glm::vec3 root{0.0f, 0.0f, 0.0f};
    const glm::vec3 target{10.0f, 0.0f, 0.0f}; // far beyond a 2 m chain
    const TwoBoneIKResult result = SolveTwoBoneIK(root, target, glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, 1.0f);

    REQUIRE_FALSE(result.reachedTarget);
    // The chain should point at the target and be nearly straight, not fold or produce NaNs.
    REQUIRE(glm::length(result.endPosition - root) == Catch::Approx(2.0f).margin(0.01));
    REQUIRE(result.endPosition.x > 1.9f);
    REQUIRE(std::isfinite(result.jointPosition.x));
    REQUIRE(std::isfinite(result.jointPosition.y));
    REQUIRE(std::isfinite(result.jointPosition.z));
}

TEST_CASE("The pole vector decides which way the joint bends", "[animation][ik]")
{
    const glm::vec3 root{0.0f, 0.0f, 0.0f};
    const glm::vec3 target{0.0f, -1.5f, 0.0f};

    const TwoBoneIKResult forward =
        SolveTwoBoneIK(root, target, glm::vec3(0.0f, 0.0f, -1.0f), 1.0f, 1.0f);
    const TwoBoneIKResult backward =
        SolveTwoBoneIK(root, target, glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, 1.0f);

    // Knees forward, elbows back: the same chain and target must bend opposite ways.
    REQUIRE(forward.jointPosition.z < -0.1f);
    REQUIRE(backward.jointPosition.z > 0.1f);
    REQUIRE(glm::length(forward.endPosition - target) == Catch::Approx(0.0f).margin(2e-3));
    REQUIRE(glm::length(backward.endPosition - target) == Catch::Approx(0.0f).margin(2e-3));
}

TEST_CASE("Two-bone IK survives degenerate input", "[animation][ik]")
{
    SECTION("target sitting exactly on the root")
    {
        const TwoBoneIKResult result =
            SolveTwoBoneIK(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, 1.0f);
        REQUIRE(std::isfinite(result.jointPosition.y));
        REQUIRE(std::isfinite(result.endPosition.y));
    }

    SECTION("pole parallel to the chain")
    {
        // A pole along the chain leaves the bend plane undefined; the solver must pick one rather
        // than divide by zero.
        const TwoBoneIKResult result = SolveTwoBoneIK(glm::vec3(0.0f), glm::vec3(0.0f, -1.5f, 0.0f),
                                                      glm::vec3(0.0f, -1.0f, 0.0f), 1.0f, 1.0f);
        REQUIRE(std::isfinite(result.jointPosition.x));
        REQUIRE(glm::length(result.jointPosition) == Catch::Approx(1.0f).margin(2e-3));
    }

    SECTION("zero-length bones")
    {
        const TwoBoneIKResult result =
            SolveTwoBoneIK(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, 0.0f);
        REQUIRE(std::isfinite(result.endPosition.x));
    }
}

TEST_CASE("RotationBetween handles the exactly-opposite case", "[animation][ik]")
{
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const glm::vec3 down{0.0f, -1.0f, 0.0f};

    // The naive cross-product form yields a zero axis here and produces NaNs.
    const glm::quat rotation = RotationBetween(up, down);
    const glm::vec3 rotated = rotation * up;

    REQUIRE(std::isfinite(rotated.x));
    REQUIRE(std::isfinite(rotated.y));
    REQUIRE(std::isfinite(rotated.z));
    REQUIRE(rotated.y == Catch::Approx(-1.0f).margin(1e-3));
}

TEST_CASE("RotationBetween maps one direction onto another", "[animation][ik]")
{
    const glm::vec3 pairs[][2] = {
        {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {{1.0f, 0.0f, 0.0f}, {0.3f, 0.6f, -0.7f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, // identity
    };

    for (const auto& pair : pairs)
    {
        const glm::vec3 from = glm::normalize(pair[0]);
        const glm::vec3 to = glm::normalize(pair[1]);
        const glm::vec3 rotated = RotationBetween(from, to) * from;

        INFO("to " << to.x << ", " << to.y << ", " << to.z);
        REQUIRE(rotated.x == Catch::Approx(to.x).margin(1e-3));
        REQUIRE(rotated.y == Catch::Approx(to.y).margin(1e-3));
        REQUIRE(rotated.z == Catch::Approx(to.z).margin(1e-3));
    }
}

TEST_CASE("LookRotation stays finite when looking along its own up axis", "[animation][ik]")
{
    const glm::quat straightUp = LookRotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(std::isfinite(straightUp.w));
    REQUIRE(std::isfinite(straightUp.x));

    const glm::quat zero = LookRotation(glm::vec3(0.0f));
    REQUIRE(zero.w == Catch::Approx(1.0f));
}

TEST_CASE("Exponential smoothing is frame-rate independent", "[animation][ik]")
{
    constexpr float speed = 8.0f;
    constexpr float total = 1.0f;

    float coarse = 0.0f;
    for (int i = 0; i < 30; ++i)
    {
        coarse = SmoothTowards(coarse, 1.0f, speed, total / 30.0f);
    }

    float fine = 0.0f;
    for (int i = 0; i < 480; ++i)
    {
        fine = SmoothTowards(fine, 1.0f, speed, total / 480.0f);
    }

    // Sixteen times the step count must reach the same place, or tuning at one frame rate would
    // produce different behaviour at another.
    REQUIRE(coarse == Catch::Approx(fine).epsilon(0.01));
    REQUIRE(coarse > 0.99f);
}
