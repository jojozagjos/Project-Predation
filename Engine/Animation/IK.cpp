#include "Engine/Animation/IK.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

TwoBoneIKResult SolveTwoBoneIK(const glm::vec3& root, const glm::vec3& target,
                               const glm::vec3& poleDirection, float upperLength, float lowerLength)
{
    TwoBoneIKResult result;
    result.rootPosition = root;

    const float upper = std::max(upperLength, 1e-4f);
    const float lower = std::max(lowerLength, 1e-4f);
    const float reach = upper + lower;

    glm::vec3 toTarget = target - root;
    float distance = glm::length(toTarget);

    if (distance < 1e-5f)
    {
        // Target sits on the root: pick an arbitrary but stable direction so the chain stays defined.
        toTarget = glm::vec3(0.0f, -1.0f, 0.0f);
        distance = 1e-5f;
    }
    const glm::vec3 direction = toTarget / distance;

    // Clamp into the range the chain can actually span, staying slightly inside the limits because a
    // fully straight or fully folded chain has an undefined bend plane and would snap between
    // solutions.
    //
    // The margin has to be relative to the span, not a fixed epsilon. On a very short chain a fixed
    // margin pushes the lower bound above the upper one, and std::clamp with an inverted range is
    // undefined behaviour that aborts under a checked standard library. Procedurally generated
    // limbs will eventually produce exactly that.
    const float minSpan = std::abs(upper - lower);
    const float maxSpan = reach;
    const float margin = std::min(1e-3f, (maxSpan - minSpan) * 0.25f);
    const float lowerBound = minSpan + margin;
    const float upperBound = maxSpan - margin;
    const float clampedDistance = upperBound > lowerBound ? std::clamp(distance, lowerBound, upperBound)
                                                          : (minSpan + maxSpan) * 0.5f;
    result.reachedTarget = distance <= reach;

    // Law of cosines for the angle at the root between the chain direction and the upper bone.
    const float cosRootAngle =
        std::clamp((upper * upper + clampedDistance * clampedDistance - lower * lower) /
                       (2.0f * upper * clampedDistance),
                   -1.0f, 1.0f);
    const float rootAngle = std::acos(cosRootAngle);

    // Build the bend plane. The pole direction is projected perpendicular to the chain; if it is
    // parallel (a degenerate pole) any perpendicular will do.
    glm::vec3 bend = poleDirection - direction * glm::dot(poleDirection, direction);
    if (glm::length(bend) < 1e-5f)
    {
        const glm::vec3 fallback =
            std::abs(direction.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        bend = fallback - direction * glm::dot(fallback, direction);
    }
    bend = glm::normalize(bend);

    const glm::vec3 upperDirection = direction * std::cos(rootAngle) + bend * std::sin(rootAngle);
    result.jointPosition = root + upperDirection * upper;
    // The end lands on the clamped target, so an out-of-reach target gives a straight chain aimed
    // at it rather than a broken one.
    result.endPosition = root + direction * clampedDistance;
    return result;
}

glm::quat LookRotation(const glm::vec3& direction, const glm::vec3& up)
{
    const float length = glm::length(direction);
    if (length < 1e-6f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const glm::vec3 forward = direction / length;

    glm::vec3 reference = up;
    if (std::abs(glm::dot(forward, glm::normalize(reference))) > 0.999f)
    {
        // Looking straight along the up axis leaves roll undefined; pick a different reference.
        reference = std::abs(forward.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    }

    const glm::vec3 right = glm::normalize(glm::cross(reference, forward));
    const glm::vec3 realUp = glm::cross(forward, right);

    glm::mat3 basis;
    basis[0] = right;
    basis[1] = realUp;
    basis[2] = forward;
    return glm::normalize(glm::quat_cast(basis));
}

glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to)
{
    const float fromLength = glm::length(from);
    const float toLength = glm::length(to);
    if (fromLength < 1e-6f || toLength < 1e-6f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    const glm::vec3 a = from / fromLength;
    const glm::vec3 b = to / toLength;
    const float dot = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);

    if (dot > 0.99999f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    if (dot < -0.99999f)
    {
        // Exactly opposite: any perpendicular axis is a valid half turn, but one has to be chosen
        // deliberately or the cross product below is zero and the result is NaN.
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), a);
        if (glm::length(axis) < 1e-5f)
        {
            axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), a);
        }
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }

    const glm::vec3 axis = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + dot, axis.x, axis.y, axis.z));
}

glm::quat AlignYWithRoll(const glm::vec3& along, const glm::vec3& forward)
{
    const float length = glm::length(along);
    if (length < 1e-5f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const glm::vec3 y = along / length;

    glm::vec3 flattened = forward - y * glm::dot(forward, y);
    if (glm::length(flattened) < 1e-4f)
    {
        // The reference is parallel to the segment, so any perpendicular will do.
        const glm::vec3 fallback =
            std::abs(y.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        flattened = fallback - y * glm::dot(fallback, y);
    }

    const glm::vec3 z = -glm::normalize(flattened); // local -Z faces `forward`
    const glm::vec3 x = glm::normalize(glm::cross(y, z));

    glm::mat3 basis;
    basis[0] = x;
    basis[1] = y;
    basis[2] = z;
    return glm::normalize(glm::quat_cast(basis));
}

float SmoothTowards(float value, float target, float speed, float dt)
{
    return value + (target - value) * (1.0f - std::exp(-speed * dt));
}

glm::vec3 SmoothTowards(const glm::vec3& value, const glm::vec3& target, float speed, float dt)
{
    return value + (target - value) * (1.0f - std::exp(-speed * dt));
}

} // namespace pred
