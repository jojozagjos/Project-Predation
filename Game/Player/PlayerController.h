#pragma once

#include "Engine/Physics/CharacterController.h"
#include "Game/Player/PlayerTypes.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace pred
{

class PhysicsWorld;
class DebugDraw;

// View state derived from the simulation. Kept separate from PlayerState because it is presentation
// only: it must never feed back into the simulation, or network prediction would diverge.
struct PlayerView
{
    glm::vec3 eyePosition{0.0f};
    // Feet, interpolated between the two most recent simulation states. Anything drawn as part of
    // the player must be placed from this rather than from PlayerState::position, or it will step
    // at the simulation rate while the camera moves at the frame rate, and visibly stutter.
    glm::vec3 renderPosition{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;

    float eyeHeight = 1.66f;    // smoothed towards the current stance
    float stepOffset = 0.0f;    // lags the camera behind stair steps
    float landingDip = 0.0f;    // downward kick on impact, springs back
    float landingDipVelocity = 0.0f;
    float bobOffset = 0.0f;
    float leanRoll = 0.0f; // radians of camera roll from leaning

    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::mat4 ViewMatrix() const;
};

// First-person movement.
//
// The simulation half (Step) is a pure function of state, input, config and the physics world. It
// takes no rendering or timing dependency and reads no globals, so it can be replayed for network
// reconciliation later without re-tuning how it feels. The presentation half (UpdateView) runs once
// per frame and only produces camera offsets.
class PlayerController
{
public:
    bool Init(PhysicsWorld& physics, const PlayerConfig& config, const glm::vec3& spawnPosition);
    void Shutdown();

    // Fixed-rate simulation. Everything that affects gameplay happens here.
    void Step(const PlayerInput& input, float fixedDeltaSeconds);

    // Per-frame presentation. Camera smoothing only; never changes PlayerState.
    // `alpha` is the fraction between the previous and current simulation states, so rendering
    // above the tick rate stays smooth.
    void UpdateView(float frameDeltaSeconds, float alpha);

    void Teleport(const glm::vec3& footPosition);

    // Puts the simulation back to a state the host sent, so the client can replay the inputs the
    // host had not yet seen. Unlike Teleport this keeps velocity, stance and the gait, because a
    // correction that reset those would turn a two-centimetre disagreement into a visible stumble.
    void RestoreState(const PlayerState& state);

    // Pins the player to a fixed spot: hiding in a locker now, being carried by a creature later.
    // Movement stops being simulated entirely rather than being fed zero input, because a capsule
    // wedged inside geometry gets pushed out by depenetration no matter what the input says.
    void Attach(const glm::vec3& footPosition, float yaw);
    void Detach(const glm::vec3& footPosition);
    bool IsAttached() const { return m_attached; }
    void Respawn(const glm::vec3& footPosition);
    void ApplyDamage(float amount, const char* cause);

    PlayerState& State() { return m_state; }
    const PlayerState& State() const { return m_state; }
    const PlayerView& View() const { return m_view; }
    PlayerConfig& Config() { return m_config; }
    const PlayerConfig& Config() const { return m_config; }

    // Re-applies capsule dimensions after the config changes at runtime.
    void ApplyConfigToCharacter();

    void DebugDraw(class DebugDraw& draw) const;

    // Diagnostics for the tuning panel.
    struct Debug
    {
        float targetSpeed = 0.0f;
        float horizontalSpeed = 0.0f;
        float lastStepUp = 0.0f;
        float slopeAngleDegrees = 0.0f;
        bool jumpConsumedBuffer = false;
        bool jumpUsedCoyote = false;
    };
    const Debug& Diagnostics() const { return m_debug; }

private:
    void UpdateStance(const PlayerInput& input, float dt);
    // Looks for a ledge in front worth climbing. Returns false when there is nothing there, the
    // ledge is the wrong height, its top is too narrow to stand on, or there is no headroom above
    // it. All of it is raycasts against static geometry, so a client replaying an input finds the
    // same ledge the host did.
    bool FindMantle(const PlayerInput& input, glm::vec3& outTarget) const;
    void StepMantle(float dt);
    glm::vec3 ComputeWishDirection(const PlayerInput& input) const;

    CharacterController m_character;
    PhysicsWorld* m_physics = nullptr;
    PlayerConfig m_config;
    PlayerState m_state;
    PlayerView m_view;
    Debug m_debug;
    bool m_attached = false;
    glm::vec3 m_attachPosition{0.0f};

    // Position at the start of the current tick, for render interpolation.
    glm::vec3 m_prevPosition{0.0f};
    // Written by Step, consumed by UpdateView. Keeping them separate means the simulation never
    // depends on how often the view updates.
    float m_pendingStepOffset = 0.0f;
    float m_pendingLandingImpact = 0.0f;

    bool m_initialized = false;
};

} // namespace pred
