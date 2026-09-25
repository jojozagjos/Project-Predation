#include "Game/Creature/CreatureRagdoll.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

glm::mat4 Unscaled(const glm::mat4& m)
{
    glm::mat4 out = m;
    for (int c = 0; c < 3; ++c)
    {
        const float length = glm::length(glm::vec3(m[c]));
        if (length > 1e-6f)
        {
            out[c] = m[c] / length;
        }
    }
    return out;
}

// How far each kind of joint may bend and twist, in radians: a spine a little, a neck more, a shoulder or
// a hip a great deal, a knee a long way but only one way.
struct Limits
{
    float normal;
    float plane;
    float twistMin;
    float twistMax;
};

Limits LimitsFor(BoneKind kind)
{
    switch (kind)
    {
    case BoneKind::Spine:
    case BoneKind::Chest:
        return {0.35f, 0.35f, -0.2f, 0.2f};
    case BoneKind::Neck:
        return {0.6f, 0.6f, -0.3f, 0.3f};
    case BoneKind::Head:
        return {0.7f, 0.7f, -0.45f, 0.45f};
    case BoneKind::Upper:
        return {1.1f, 1.1f, -0.4f, 0.4f};
    case BoneKind::Lower:
        return {0.15f, 1.5f, -0.1f, 0.1f};
    case BoneKind::End:
        return {0.6f, 0.7f, -0.3f, 0.3f};
    case BoneKind::Tail:
        return {0.5f, 0.5f, -0.2f, 0.2f};
    default:
        return {0.3f, 0.3f, -0.1f, 0.1f};
    }
}

} // namespace

void CreatureRagdoll::Start(PhysicsWorld& physics, const CreatureSkin& skin, const std::vector<glm::mat4>& world,
                            const glm::vec3& velocity, int pushedBone, const glm::vec3& push)
{
    Stop(physics);
    const size_t count = skin.bones.size();
    m_world.resize(count);
    m_partOf.assign(count, -1);
    m_carrier.assign(count, -1);
    m_onCarrier.assign(count, glm::mat4(1.0f));
    for (size_t b = 0; b < count; ++b)
    {
        m_world[b] = Unscaled(world[b]);
    }

    for (size_t b = 0; b < count; ++b)
    {
        const SkinBone& bone = skin.bones[b];
        // The jaw hangs off the skull; everything else falls as a piece of its own.
        if (bone.kind == BoneKind::Jaw)
        {
            m_carrier[b] = bone.parent;
            m_onCarrier[b] = glm::inverse(m_world[static_cast<size_t>(bone.parent)]) * m_world[b];
            continue;
        }
        const float length = glm::distance(bone.head, bone.tail);
        const float radius = std::clamp(bone.radius, 0.012f, std::max(length * 0.6f, 0.015f));
        const float halfLength = length * 0.5f;
        const float halfHeight = std::max(halfLength - radius * 0.5f, 0.004f);
        const glm::mat4& frame = m_world[b];
        Transform transform;
        transform.position = glm::vec3(frame * glm::vec4(0.0f, halfLength, 0.0f, 1.0f));
        transform.rotation = glm::quat_cast(glm::mat3(frame));
        // Lighter than water: the capsules are fatter than the limbs they stand for, and a body that
        // weighs what its capsules would lands like a sack of cement.
        const BodyHandle body = physics.CreateCapsule(halfHeight, radius, transform, BodyMotion::Dynamic, 450.0f,
                                                      PhysicsLayer::Ragdoll);
        if (!body.IsValid())
        {
            continue;
        }
        physics.SetLinearVelocity(body, velocity);
        m_partOf[b] = static_cast<int>(m_parts.size());
        m_parts.push_back({static_cast<int>(b), body, halfLength});
    }

    // Joined to their parents where they meet.
    for (const Part& part : m_parts)
    {
        const SkinBone& bone = skin.bones[static_cast<size_t>(part.bone)];
        int parent = bone.parent;
        while (parent >= 0 && m_partOf[static_cast<size_t>(parent)] < 0)
        {
            parent = skin.bones[static_cast<size_t>(parent)].parent;
        }
        if (parent < 0)
        {
            continue;
        }
        const glm::mat4& frame = m_world[static_cast<size_t>(part.bone)];
        const Limits limits = LimitsFor(bone.kind);
        const uint32_t joint = physics.AddSwingTwistJoint(
            m_parts[static_cast<size_t>(m_partOf[static_cast<size_t>(parent)])].body, part.body, glm::vec3(frame[3]),
            glm::vec3(frame[1]), glm::vec3(frame[2]), limits.normal, limits.plane, limits.twistMin, limits.twistMax);
        if (joint != 0)
        {
            m_joints.push_back(joint);
        }
    }

    if (pushedBone >= 0 && static_cast<size_t>(pushedBone) < count && m_partOf[static_cast<size_t>(pushedBone)] >= 0)
    {
        physics.AddImpulse(m_parts[static_cast<size_t>(m_partOf[static_cast<size_t>(pushedBone)])].body, push);
    }
}

void CreatureRagdoll::Stop(PhysicsWorld& physics)
{
    for (const uint32_t joint : m_joints)
    {
        physics.RemoveJoint(joint);
    }
    m_joints.clear();
    for (const Part& part : m_parts)
    {
        physics.DestroyBody(part.body);
    }
    m_parts.clear();
}

void CreatureRagdoll::Update(const PhysicsWorld& physics)
{
    for (const Part& part : m_parts)
    {
        const Transform transform = physics.GetTransform(part.body);
        m_world[static_cast<size_t>(part.bone)] = glm::translate(glm::mat4(1.0f), transform.position) *
                                                  glm::mat4_cast(transform.rotation) *
                                                  glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -part.halfLength, 0.0f));
    }
    for (size_t b = 0; b < m_carrier.size(); ++b)
    {
        if (m_carrier[b] >= 0)
        {
            m_world[b] = m_world[static_cast<size_t>(m_carrier[b])] * m_onCarrier[b];
        }
    }
}

bool CreatureRagdoll::Settled(const PhysicsWorld& physics) const
{
    for (const Part& part : m_parts)
    {
        if (physics.IsActive(part.body))
        {
            return false;
        }
    }
    return true;
}

bool CreatureRagdoll::Owns(BodyHandle body) const
{
    return BoneOf(body) >= 0;
}

int CreatureRagdoll::BoneOf(BodyHandle body) const
{
    for (const Part& part : m_parts)
    {
        if (part.body == body)
        {
            return part.bone;
        }
    }
    return -1;
}

glm::vec3 CreatureRagdoll::Centre() const
{
    if (m_parts.empty())
    {
        return glm::vec3(0.0f);
    }
    return glm::vec3(m_world[static_cast<size_t>(m_parts.front().bone)][3]);
}

void CreatureRagdoll::Pull(PhysicsWorld& physics, int bone, const glm::vec3& velocity)
{
    if (bone < 0 || static_cast<size_t>(bone) >= m_partOf.size() || m_partOf[static_cast<size_t>(bone)] < 0)
    {
        return;
    }
    const BodyHandle body = m_parts[static_cast<size_t>(m_partOf[static_cast<size_t>(bone)])].body;
    physics.SetLinearVelocity(body, physics.GetLinearVelocity(body) + velocity);
}

void CreatureRagdoll::Push(PhysicsWorld& physics, BodyHandle body, const glm::vec3& impulse)
{
    if (Owns(body))
    {
        physics.AddImpulse(body, impulse);
    }
}

} // namespace pred
