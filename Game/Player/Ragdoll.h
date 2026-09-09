#pragma once

#include "Engine/Animation/Skeleton.h"

#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

class PhysicsWorld;

// A body that has stopped being animated.
//
// One particle per joint, distance constraints along the bones, gravity, and the floor. No
// constraint solver in the physics engine is needed for this and none is used: a skeleton is
// already a set of points with fixed distances between them, which is exactly what Verlet
// integration is for. It costs a few hundred additions per body per tick.
//
// The bones keep their lengths because that is what the constraints enforce, and the joints bend
// wherever they like because nothing stops them. That is not anatomically right, but a body
// collapsing reads correctly at the distance anyone will ever see one from, and the alternative is
// a joint limit solver for a thing that happens once per death.
class Ragdoll
{
public:
    struct Settings
    {
        float gravity = -19.0f;      // heavier than real, to match the player's own gravity
        float damping = 0.986f;      // air, and the fact that a body is not a bag of marbles
        float friction = 0.55f;      // how much sliding the floor takes out per contact
        float bounce = 0.05f;        // almost none: a person does not bounce
        int iterations = 8;          // constraint passes per step; more is stiffer
        float radius = 0.06f;        // how far a joint keeps off the floor
        float settleSpeed = 0.12f;   // below this for a while and it stops being simulated
        float settleSeconds = 1.2f;
    };

    // Takes the pose as it stands and starts falling from it. `impulse` is the direction and force
    // of whatever did it, applied to the chest so a body goes down the way it was hit.
    void Start(const Skeleton& skeleton, const Pose& pose, const glm::vec3& impulse);
    void Stop() { m_active = false; }
    bool Active() const { return m_active; }
    bool Settled() const { return m_settled; }
    float Age() const { return m_age; }

    void Step(PhysicsWorld& physics, float dt);
    // Writes the joint positions back into a pose, so the same drawn parts follow the ragdoll.
    void ApplyTo(const Skeleton& skeleton, Pose& pose) const;

    Settings& Tuning() { return m_settings; }
    const Settings& Tuning() const { return m_settings; }
    // Exposed for tests: bone lengths must survive, or the body stretches into spaghetti.
    const std::vector<glm::vec3>& Points() const { return m_positions; }

private:
    struct Constraint
    {
        int a = 0;
        int b = 0;
        float length = 0.0f;
    };

    void AddConstraint(int a, int b);

    Settings m_settings;
    std::vector<glm::vec3> m_positions;
    std::vector<glm::vec3> m_previous;
    std::vector<float> m_groundHeight;
    // Where each joint was when its floor was last traced, so the trace is only redone when it has
    // gone somewhere the answer might differ.
    std::vector<glm::vec3> m_groundSampledAt;
    std::vector<Constraint> m_constraints;
    float m_age = 0.0f;
    float m_stillFor = 0.0f;
    bool m_active = false;
    bool m_settled = false;
};

} // namespace pred
