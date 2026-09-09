#include "Game/Player/Ragdoll.h"

#include "Engine/Physics/PhysicsWorld.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

// Places a segment running from `a` to `b` with its local +Y along it, which is how every drawn
// part is built. `up` resolves the spin about that axis, which a segment alone cannot.
glm::mat4 SegmentFrame(const glm::vec3& a, const glm::vec3& b, const glm::vec3& reference)
{
    const glm::vec3 delta = b - a;
    const float length = glm::length(delta);
    if (length < 1e-5f)
    {
        return glm::translate(glm::mat4(1.0f), a);
    }
    const glm::vec3 up = delta / length;
    glm::vec3 right = glm::cross(reference, up);
    if (glm::dot(right, right) < 1e-6f)
    {
        right = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), up);
    }
    right = glm::normalize(right);
    const glm::vec3 forward = glm::cross(up, right);

    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(right, 0.0f);
    frame[1] = glm::vec4(up, 0.0f);
    frame[2] = glm::vec4(forward, 0.0f);
    frame[3] = glm::vec4(a, 1.0f);
    return frame;
}

} // namespace

void Ragdoll::AddConstraint(int a, int b)
{
    if (a < 0 || b < 0 || a >= static_cast<int>(m_positions.size()) ||
        b >= static_cast<int>(m_positions.size()) || a == b)
    {
        return;
    }
    Constraint constraint;
    constraint.a = a;
    constraint.b = b;
    constraint.length = glm::distance(m_positions[static_cast<size_t>(a)],
                                      m_positions[static_cast<size_t>(b)]);
    m_constraints.push_back(constraint);
}

void Ragdoll::Start(const Skeleton& skeleton, const Pose& pose, const glm::vec3& impulse)
{
    const int bones = skeleton.BoneCount();
    m_positions.resize(static_cast<size_t>(bones));
    m_previous.resize(static_cast<size_t>(bones));
    m_groundHeight.assign(static_cast<size_t>(bones), -1000.0f);
    m_groundSampledAt.assign(static_cast<size_t>(bones), glm::vec3(1e6f));
    m_constraints.clear();

    for (int i = 0; i < bones; ++i)
    {
        m_positions[static_cast<size_t>(i)] = pose.GlobalPosition(static_cast<BoneIndex>(i));
        m_previous[static_cast<size_t>(i)] = m_positions[static_cast<size_t>(i)];
    }

    // Along the bones. This is what keeps limbs the length they were.
    for (int i = 0; i < bones; ++i)
    {
        const BoneIndex parent = skeleton.GetBone(static_cast<BoneIndex>(i)).parent;
        if (parent != kInvalidBone)
        {
            AddConstraint(static_cast<int>(parent), i);
        }
    }

    // And across the torso. Without these the chest folds flat the moment it lands, because a chain
    // of points in a line has nothing holding its shape.
    const auto boneNamed = [&](const char* name)
    {
        for (int i = 0; i < bones; ++i)
        {
            if (skeleton.GetBone(static_cast<BoneIndex>(i)).name == name)
            {
                return i;
            }
        }
        return -1;
    };

    const int pelvis = boneNamed("pelvis");
    const int chest = boneNamed("chest");
    const int neck = boneNamed("neck");
    const int shoulderLeft = boneNamed("shoulder_left");
    const int shoulderRight = boneNamed("shoulder_right");
    const int hipLeft = boneNamed("upper_leg_left");
    const int hipRight = boneNamed("upper_leg_right");

    AddConstraint(shoulderLeft, shoulderRight);
    AddConstraint(hipLeft, hipRight);
    AddConstraint(shoulderLeft, hipRight);
    AddConstraint(shoulderRight, hipLeft);
    AddConstraint(pelvis, chest);
    AddConstraint(pelvis, neck);
    AddConstraint(shoulderLeft, pelvis);
    AddConstraint(shoulderRight, pelvis);

    // The blow lands on the chest and the rest follows through the constraints, which is what makes
    // a body go down the way it was hit rather than dropping straight through itself.
    if (chest >= 0)
    {
        m_previous[static_cast<size_t>(chest)] -= impulse * (1.0f / 60.0f);
    }

    m_age = 0.0f;
    m_stillFor = 0.0f;
    m_active = true;
    m_settled = false;
}

void Ragdoll::Step(PhysicsWorld& physics, float dt)
{
    if (!m_active || m_settled || dt <= 0.0f)
    {
        return;
    }
    m_age += dt;

    // Verlet: the velocity is wherever the point was last step, so a constraint that moves a point
    // also changes its velocity for free. That is the whole reason to integrate this way here.
    float fastest = 0.0f;
    for (size_t i = 0; i < m_positions.size(); ++i)
    {
        const glm::vec3 velocity = (m_positions[i] - m_previous[i]) * m_settings.damping;
        fastest = std::max(fastest, glm::length(velocity) / dt);
        m_previous[i] = m_positions[i];
        m_positions[i] += velocity + glm::vec3(0.0f, m_settings.gravity, 0.0f) * dt * dt;
    }

    for (int pass = 0; pass < m_settings.iterations; ++pass)
    {
        for (const Constraint& constraint : m_constraints)
        {
            glm::vec3& a = m_positions[static_cast<size_t>(constraint.a)];
            glm::vec3& b = m_positions[static_cast<size_t>(constraint.b)];
            const glm::vec3 delta = b - a;
            const float distance = glm::length(delta);
            if (distance < 1e-5f)
            {
                continue;
            }
            const glm::vec3 correction = delta * ((distance - constraint.length) / distance) * 0.5f;
            a += correction;
            b -= correction;
        }
    }

    // The floor. Traced once per point per step from above it, so a body lands on a crate or a
    // stair rather than on whatever height the level happens to start at.
    for (size_t i = 0; i < m_positions.size(); ++i)
    {
        // The floor under a joint is re-traced only when that joint has moved somewhere new. A body
        // has about twenty of them and it takes a second or two to come to rest, so tracing every
        // one every step was a couple of thousand raycasts per corpse for an answer that barely
        // changes. Fifteen centimetres is well inside the width of anything you can lie on.
        const glm::vec3 flat{m_positions[i].x, 0.0f, m_positions[i].z};
        if (glm::distance(flat, m_groundSampledAt[i]) > 0.15f || m_groundHeight[i] < -900.0f)
        {
            const glm::vec3 from = m_positions[i] + glm::vec3(0.0f, 1.0f, 0.0f);
            const RayHit hit = physics.RayCast(from, glm::vec3(0.0f, -1.0f, 0.0f), 3.0f);
            if (hit)
            {
                m_groundHeight[i] = hit.position.y;
            }
            m_groundSampledAt[i] = flat;
        }

        const float floor = m_groundHeight[i] + m_settings.radius;
        if (m_positions[i].y >= floor)
        {
            continue;
        }
        m_positions[i].y = floor;

        // Friction, applied by dragging the previous position towards the current one: with Verlet
        // that is the same as taking speed out, and it is what stops a body skating.
        const glm::vec3 velocity = m_positions[i] - m_previous[i];
        m_previous[i].x = m_positions[i].x - velocity.x * (1.0f - m_settings.friction);
        m_previous[i].z = m_positions[i].z - velocity.z * (1.0f - m_settings.friction);
        m_previous[i].y = m_positions[i].y + velocity.y * m_settings.bounce;
    }

    if (fastest < m_settings.settleSpeed)
    {
        m_stillFor += dt;
        if (m_stillFor >= m_settings.settleSeconds)
        {
            // Stopped moving. Left where it lies rather than integrated for the rest of the round.
            m_settled = true;
        }
    }
    else
    {
        m_stillFor = 0.0f;
    }
}

void Ragdoll::ApplyTo(const Skeleton& skeleton, Pose& pose) const
{
    if (m_positions.empty())
    {
        return;
    }

    // Written parent first, because writing a bone's global transform rebuilds everything after it.
    const int bones = skeleton.BoneCount();
    for (int i = 0; i < bones; ++i)
    {
        const auto index = static_cast<BoneIndex>(i);
        const glm::vec3 position = m_positions[static_cast<size_t>(i)];

        // A bone points at its first child. A bone with no children keeps its parent's direction,
        // because there is nothing to point at.
        int child = -1;
        for (int j = i + 1; j < bones; ++j)
        {
            if (skeleton.GetBone(static_cast<BoneIndex>(j)).parent == index)
            {
                child = j;
                break;
            }
        }

        glm::vec3 towards = position + glm::vec3(0.0f, 0.1f, 0.0f);
        if (child >= 0)
        {
            towards = m_positions[static_cast<size_t>(child)];
        }
        else if (skeleton.GetBone(index).parent != kInvalidBone)
        {
            const glm::vec3 parent =
                m_positions[static_cast<size_t>(skeleton.GetBone(index).parent)];
            towards = position + (position - parent);
        }

        pose.SetGlobal(skeleton, index, SegmentFrame(position, towards, glm::vec3(0.0f, 0.0f, -1.0f)));
    }
}

} // namespace pred
