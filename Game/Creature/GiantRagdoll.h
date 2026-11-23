#pragma once

#include "Engine/Physics/PhysicsWorld.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <functional>
#include <vector>

namespace pred
{

// A giant gone limp (you, 2026-11-23: dead, they should pretty much just ragdoll): a capsule for each of its pieces -- a strider's
// hull and each leg's two lengths, a leviathan's spine link by link -- joined where they meet, loosely, and handed to the physics to
// come down however they come down: onto the ground and onto and against what is built, never through it, its body bending freely.
// Each piece carries the bone it stands for, read back every frame; bones with no piece of their own (a head, a jaw, a tail's last
// joint) ride on the one they hang from, as they were when it fell.
//
// Every machine runs its own, as a creature's: where each leg lands is not something anybody acts on.
class GiantRagdoll
{
public:
    GiantRagdoll() = default;
    GiantRagdoll(const GiantRagdoll&) = delete;
    GiantRagdoll& operator=(const GiantRagdoll&) = delete;

    struct Piece
    {
        int bone = -1;            // the skinning matrix it carries
        glm::vec3 start{0.0f};    // its ends, in the world as it falls (where it joins its parent: `start`)
        glm::vec3 end{0.0f};
        float radius = 1.0f;
        float density = 400.0f;
        int parent = -1;          // which piece it hangs from (-1: none)
        float cone = 1.0f;        // how far it swings about where it joins its parent, either way (radians)
        float twist = 0.3f;       // and how far it twists about itself
    };
    // From the bones as they are drawn now (`skinning`), moving at `velocity`; `carried` the bones that ride on others (bone, the bone
    // it rides on).
    void Start(PhysicsWorld& physics, const std::vector<Piece>& pieces, const std::vector<glm::mat4>& skinning, const glm::vec3& velocity,
               const std::vector<std::pair<int, int>>& carried);
    void Stop(PhysicsWorld& physics);
    bool Active() const { return !m_parts.empty(); }
    // Every piece's bone, and every carried one, read back into `skinning`.
    void Update(const PhysicsWorld& physics, std::vector<glm::mat4>& skinning) const;
    // Where its first piece is (its hull, its head), and how fast it is going.
    glm::vec3 Centre(const PhysicsWorld& physics) const;
    glm::vec3 Velocity(const PhysicsWorld& physics) const;
    // In water (how deep at a point, and its surface): each piece under it buoyed up -- a little more than it weighs, or less where it
    // `sinks` -- and slowed by it.
    void Float(PhysicsWorld& physics, const std::function<float(const glm::vec3&, float&)>& water, bool sinks, float dt);

private:
    struct Part
    {
        int bone = -1;
        BodyHandle body;
        glm::mat4 offset{1.0f}; // the bone in the capsule's frame
    };
    struct Rider
    {
        int bone = -1;
        int on = -1;
        glm::mat4 offset{1.0f}; // the bone in the frame of the one it rides on
    };
    std::vector<Part> m_parts;
    std::vector<Rider> m_riders;
    std::vector<uint32_t> m_joints;
};

} // namespace pred
