#include "Game/Creature/GiantRagdoll.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

glm::mat4 Placed(const Transform& transform)
{
    return glm::translate(glm::mat4(1.0f), transform.position) * glm::mat4_cast(transform.rotation);
}

} // namespace

void GiantRagdoll::Start(PhysicsWorld& physics, const std::vector<Piece>& pieces, const std::vector<glm::mat4>& skinning, const glm::vec3& velocity,
                         const std::vector<std::pair<int, int>>& carried)
{
    Stop(physics);
    std::vector<int> partOfPiece(pieces.size(), -1);
    for (size_t p = 0; p < pieces.size(); ++p)
    {
        const Piece& piece = pieces[p];
        if (piece.bone < 0 || static_cast<size_t>(piece.bone) >= skinning.size())
        {
            continue;
        }
        const glm::vec3 along = piece.end - piece.start;
        const float length = glm::length(along);
        const float radius = std::max(std::min(piece.radius, length * 0.5f), 0.05f);
        Transform transform;
        transform.position = (piece.start + piece.end) * 0.5f;
        transform.rotation = length > 1e-4f ? glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), along / length) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const BodyHandle body = physics.CreateCapsule(std::max(length * 0.5f - radius, 0.05f), radius, transform, BodyMotion::Dynamic, piece.density,
                                                      PhysicsLayer::Ragdoll);
        if (!body.IsValid())
        {
            continue;
        }
        physics.SetLinearVelocity(body, velocity);
        Part part;
        part.bone = piece.bone;
        part.body = body;
        part.offset = glm::inverse(Placed(transform)) * skinning[static_cast<size_t>(piece.bone)];
        partOfPiece[p] = static_cast<int>(m_parts.size());
        m_parts.push_back(part);
    }
    // Joined where they meet: about the start of each, within its cone.
    for (size_t p = 0; p < pieces.size(); ++p)
    {
        const Piece& piece = pieces[p];
        if (piece.parent < 0 || partOfPiece[p] < 0 || partOfPiece[static_cast<size_t>(piece.parent)] < 0)
        {
            continue;
        }
        const glm::vec3 along = glm::normalize(piece.end - piece.start + glm::vec3(0.0f, 1e-5f, 0.0f));
        glm::vec3 across = glm::cross(along, glm::vec3(0.0f, 1.0f, 0.0f));
        across = glm::length(across) > 1e-3f ? glm::normalize(across) : glm::vec3(1.0f, 0.0f, 0.0f);
        const uint32_t joint = physics.AddSwingTwistJoint(m_parts[static_cast<size_t>(partOfPiece[static_cast<size_t>(piece.parent)])].body,
                                                          m_parts[static_cast<size_t>(partOfPiece[p])].body, piece.start, along, across, piece.cone, piece.cone,
                                                          -piece.twist, piece.twist);
        if (joint != 0)
        {
            m_joints.push_back(joint);
        }
    }
    // What rides on what: kept where it was on it.
    for (const auto& [bone, on] : carried)
    {
        if (bone < 0 || on < 0 || static_cast<size_t>(bone) >= skinning.size() || static_cast<size_t>(on) >= skinning.size())
        {
            continue;
        }
        m_riders.push_back({bone, on, glm::inverse(skinning[static_cast<size_t>(on)]) * skinning[static_cast<size_t>(bone)]});
    }
}

void GiantRagdoll::Stop(PhysicsWorld& physics)
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
    m_riders.clear();
}

void GiantRagdoll::Update(const PhysicsWorld& physics, std::vector<glm::mat4>& skinning) const
{
    for (const Part& part : m_parts)
    {
        skinning[static_cast<size_t>(part.bone)] = Placed(physics.GetTransform(part.body)) * part.offset;
    }
    // (In order: a rider on a rider -- a jaw on a head on a hull -- after the one it rides on.)
    for (const Rider& rider : m_riders)
    {
        skinning[static_cast<size_t>(rider.bone)] = skinning[static_cast<size_t>(rider.on)] * rider.offset;
    }
}

glm::vec3 GiantRagdoll::Centre(const PhysicsWorld& physics) const
{
    return m_parts.empty() ? glm::vec3(0.0f) : physics.GetTransform(m_parts.front().body).position;
}

glm::vec3 GiantRagdoll::Velocity(const PhysicsWorld& physics) const
{
    return m_parts.empty() ? glm::vec3(0.0f) : physics.GetLinearVelocity(m_parts.front().body);
}

void GiantRagdoll::Float(PhysicsWorld& physics, const std::function<float(const glm::vec3&, float&)>& water, bool sinks, float dt)
{
    if (!water)
    {
        return;
    }
    for (const Part& part : m_parts)
    {
        const glm::vec3 at = physics.GetTransform(part.body).position;
        float surface = 0.0f;
        if (water(at, surface) <= 0.0f || at.y > surface)
        {
            continue;
        }
        // Fully buoyed a metre down, less towards the surface: it comes up and lies along it, its back out -- or, plated, only slowed as
        // it goes down.
        const float under = std::clamp((surface - at.y) / 1.0f, 0.0f, 1.0f);
        glm::vec3 velocity = physics.GetLinearVelocity(part.body);
        velocity.y += (sinks ? 9.81f * 0.7f : 9.81f * 1.25f) * under * dt;
        velocity *= std::max(1.0f - 1.2f * dt, 0.0f);
        physics.SetLinearVelocity(part.body, velocity);
        physics.SetAngularVelocity(part.body, physics.GetAngularVelocity(part.body) * std::max(1.0f - 1.5f * dt, 0.0f));
    }
}

} // namespace pred
