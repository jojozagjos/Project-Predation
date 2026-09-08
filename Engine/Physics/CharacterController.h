#pragma once

#include <glm/vec3.hpp>

#include <memory>

namespace pred
{

class PhysicsWorld;

enum class GroundState : uint8_t
{
    Grounded,    // standing on a surface shallow enough to walk on
    SteepGround, // touching ground, but too steep to stand on: slide off it
    InAir
};

// A moving capsule that collides against the world without being simulated by it.
//
// This wraps Jolt's CharacterVirtual, which solves the parts of movement that are tedious and
// error-prone to write from scratch: collide-and-slide against arbitrary geometry, stepping up
// stairs, sliding off steep slopes, and staying attached to the floor when walking downhill.
// How the character *feels* (acceleration curves, air control, stances, jumping) is not Jolt's job
// and lives in the player controller on top of this.
class CharacterController
{
public:
    struct Settings
    {
        float height = 1.8f;          // total capsule height, feet to crown
        float radius = 0.32f;
        float maxSlopeAngleDegrees = 46.0f;
        float stepHeight = 0.35f;     // tallest ledge that can be walked up without jumping
        float stickToFloorDistance = 0.5f; // how far to search downwards to stay on the ground
        float mass = 80.0f;
        // A small skin around the shape. Too small and the character catches on geometry; too large
        // and it visibly floats away from walls.
        float padding = 0.02f;
        float penetrationRecoverySpeed = 1.2f;
        float predictiveContactDistance = 0.1f;
    };

    CharacterController();
    ~CharacterController();
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    bool Create(PhysicsWorld& world, const Settings& settings, const glm::vec3& footPosition);
    void Destroy();
    bool IsValid() const;

    // Advances the character by its current velocity, resolving collisions.
    void Update(float deltaSeconds, const glm::vec3& gravity);

    // Position is at the character's feet, not the capsule centre.
    glm::vec3 GetPosition() const;
    void SetPosition(const glm::vec3& footPosition);

    glm::vec3 GetLinearVelocity() const;
    void SetLinearVelocity(const glm::vec3& velocity);

    GroundState GetGroundState() const;
    bool IsGrounded() const { return GetGroundState() == GroundState::Grounded; }
    glm::vec3 GetGroundNormal() const;
    glm::vec3 GetGroundVelocity() const;

    // Swaps the capsule, for crouching and going prone. Returns false and changes nothing when the
    // new shape would not fit, which is what stops the player standing up inside a vent.
    bool TryResize(float height, float radius);
    float GetHeight() const;
    float GetRadius() const;

    const Settings& GetSettings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace pred
