#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace pred
{

// Analytic two-bone inverse kinematics: the solver for arms and legs.
//
// Given a root (hip or shoulder), a joint (knee or elbow) and an end effector (foot or hand), it
// finds the two rotations that put the effector on the target. Because it is closed-form rather
// than iterative, it is exact, cheap, and produces no jitter, which matters when it runs twice per
// leg per creature every frame.
struct TwoBoneIKResult
{
    glm::vec3 rootPosition{0.0f};
    glm::vec3 jointPosition{0.0f};
    glm::vec3 endPosition{0.0f};
    bool reachedTarget = false; // false when the target was beyond the chain's reach
};

// `poleDirection` decides which way the joint bends: knees forward, elbows back. It does not need
// to be normalized or perpendicular to the chain.
TwoBoneIKResult SolveTwoBoneIK(const glm::vec3& root, const glm::vec3& target,
                               const glm::vec3& poleDirection, float upperLength, float lowerLength);

// Rotation that points `localForward` at `direction`, keeping roll under control by referencing
// `up`. Returns identity for degenerate inputs rather than producing a NaN quaternion.
glm::quat LookRotation(const glm::vec3& direction, const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

// Rotation taking `from` onto `to`, both assumed non-zero. Handles the exactly-opposite case, which
// is the one that produces NaNs in naive implementations.
glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to);

// Rotation whose local +Y runs along `along`, with local -Z turned as close to `forward` as it can.
//
// Use this, rather than RotationBetween, whenever a limb or body segment is drawn along an axis.
// The minimal rotation onto a direction leaves the spin about that direction undefined, so a
// segment that happens to point straight up gets the identity and silently ignores which way the
// character is facing. Supplying a forward reference pins that remaining degree of freedom.
glm::quat AlignYWithRoll(const glm::vec3& along, const glm::vec3& forward);

// Frame-rate independent smoothing, matching the convention used by the player camera.
float SmoothTowards(float value, float target, float speed, float dt);
glm::vec3 SmoothTowards(const glm::vec3& value, const glm::vec3& target, float speed, float dt);

} // namespace pred
