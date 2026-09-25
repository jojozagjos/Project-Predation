#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Game/Creature/CreatureSkin.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

// A creature's body gone limp: a capsule for every bone that has any weight to it, joined at the joints
// by swing-and-twist limits a spine, a neck, a shoulder and a knee each have, and handed to the physics
// to fall however it falls. Its bones are read back from the capsules every frame, so the same skin that
// walked is the skin lying in a heap.
//
// Every machine runs its own. A body falling is not something the players act on -- rounds still find
// it, and that is all -- so there is no need for the host to say where each finger landed.
class CreatureRagdoll
{
public:
    CreatureRagdoll() = default;
    CreatureRagdoll(const CreatureRagdoll&) = delete;
    CreatureRagdoll& operator=(const CreatureRagdoll&) = delete;

    // Starts from `world` -- every bone's frame in the world right now -- moving at `velocity`, with a
    // push at one bone from whatever brought it down.
    void Start(PhysicsWorld& physics, const CreatureSkin& skin, const std::vector<glm::mat4>& world,
               const glm::vec3& velocity, int pushedBone, const glm::vec3& push);
    void Stop(PhysicsWorld& physics);
    bool Active() const { return !m_parts.empty(); }

    // Reads every bone back from the physics. Bones with no capsule of their own -- the jaw -- keep the
    // place on their parent they had when it fell.
    void Update(const PhysicsWorld& physics);
    const std::vector<glm::mat4>& World() const { return m_world; }
    // Whether it has come to rest, when there is nothing left to read back.
    bool Settled(const PhysicsWorld& physics) const;

    bool Owns(BodyHandle body) const;
    int BoneOf(BodyHandle body) const;
    glm::vec3 Centre() const;
    void Push(PhysicsWorld& physics, BodyHandle body, const glm::vec3& impulse);
    // Adds to how fast the part carrying `bone` is moving: a creature lying there pulling itself along.
    void Pull(PhysicsWorld& physics, int bone, const glm::vec3& velocity);

private:
    struct Part
    {
        int bone = -1;
        BodyHandle body;
        float halfLength = 0.0f;
    };
    std::vector<Part> m_parts;
    std::vector<uint32_t> m_joints;
    std::vector<glm::mat4> m_world;
    // For a bone without a capsule: which bone carries it and where it sits on that bone.
    std::vector<int> m_carrier;
    std::vector<glm::mat4> m_onCarrier;
    std::vector<int> m_partOf; // bone to part, -1 for none
};

} // namespace pred
