#include "Game/Player/PlayerController.h"

#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/DebugDraw.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

constexpr float kEarthGravity = 9.81f;

// Exponential smoothing that behaves the same at any frame rate. Naively doing
// `value += (target - value) * speed * dt` changes its response when dt changes, which makes tuning
// at 60 fps produce different feel at 144 fps.
float SmoothTowards(float value, float target, float speed, float dt)
{
    const float t = 1.0f - std::exp(-speed * dt);
    return value + (target - value) * t;
}

glm::vec2 ClampLength(const glm::vec2& v, float maxLength)
{
    const float length = glm::length(v);
    return (length > maxLength && length > 1e-6f) ? v * (maxLength / length) : v;
}

} // namespace

glm::vec3 PlayerView::Forward() const
{
    const float cp = std::cos(pitch);
    return glm::vec3(std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp);
}

glm::vec3 PlayerView::Right() const
{
    return glm::normalize(glm::cross(Forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::mat4 PlayerView::ViewMatrix() const
{
    const glm::vec3 forward = Forward();
    // Roll about the view axis so leaning tilts the horizon, which is most of what sells it.
    const glm::vec3 up = glm::angleAxis(leanRoll, forward) * glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::lookAtRH(eyePosition, eyePosition + forward, up);
}

// -----------------------------------------------------------------------------------------------

bool PlayerController::Init(PhysicsWorld& physics, const PlayerConfig& config, const glm::vec3& spawnPosition)
{
    m_physics = &physics;
    m_config = config;

    CharacterController::Settings settings;
    settings.height = config.standHeight;
    settings.radius = config.radius;
    settings.stepHeight = config.stepHeight;
    settings.maxSlopeAngleDegrees = config.maxSlopeAngle;

    if (!m_character.Create(physics, settings, spawnPosition))
    {
        return false;
    }

    m_state = PlayerState{};
    m_state.position = spawnPosition;
    m_prevPosition = spawnPosition;
    m_view = PlayerView{};
    m_view.eyeHeight = config.standEyeHeight;
    m_view.renderPosition = spawnPosition;
    m_view.eyePosition = spawnPosition + glm::vec3(0.0f, config.standEyeHeight, 0.0f);
    m_initialized = true;

    PRED_LOG_INFO(Gameplay, "Player spawned at {:.1f} {:.1f} {:.1f}", spawnPosition.x, spawnPosition.y,
                  spawnPosition.z);
    return true;
}

void PlayerController::Shutdown()
{
    m_character.Destroy();
    m_physics = nullptr;
    m_initialized = false;
}

void PlayerController::ApplyConfigToCharacter()
{
    if (!m_initialized)
    {
        return;
    }
    // Resizing can fail if the new capsule does not fit; that is fine, it will be retried as the
    // player moves.
    m_character.TryResize(m_config.HeightForStance(m_state.stance), m_config.radius);
}

glm::vec3 PlayerController::ComputeWishDirection(const PlayerInput& input) const
{
    glm::vec2 move = input.move;
    const float length = glm::length(move);
    if (length < 1e-4f)
    {
        return glm::vec3(0.0f);
    }
    if (length > 1.0f)
    {
        move /= length;
    }

    // Yaw only: looking up or down must not change where "forward" is on the ground.
    const float sinYaw = std::sin(m_state.yaw);
    const float cosYaw = std::cos(m_state.yaw);
    const glm::vec3 forward{sinYaw, 0.0f, -cosYaw};
    const glm::vec3 right{cosYaw, 0.0f, sinYaw};
    return forward * move.y + right * move.x;
}

void PlayerController::UpdateStance(const PlayerInput& input, float /*dt*/)
{
    PlayerStance desired = PlayerStance::Standing;
    if (input.proneHeld)
    {
        desired = PlayerStance::Prone;
    }
    else if (input.crouchHeld)
    {
        desired = PlayerStance::Crouching;
    }
    m_state.desiredStance = desired;

    if (desired == m_state.stance)
    {
        m_state.stanceBlocked = false;
        return;
    }

    // Shrinking always succeeds; growing can be blocked by whatever is overhead. That rejection is
    // what stops the player standing up inside a vent or under a low lintel.
    if (m_character.TryResize(m_config.HeightForStance(desired), m_config.radius))
    {
        m_state.stance = desired;
        m_state.stanceBlocked = false;
    }
    else
    {
        m_state.stanceBlocked = true;
    }
}

void PlayerController::Step(const PlayerInput& input, float dt)
{
    if (!m_initialized || dt <= 0.0f)
    {
        return;
    }

    m_state.yaw = input.yaw;
    m_state.pitch = input.pitch;
    m_state.jumpedThisTick = false;
    m_state.landedThisTick = false;
    m_debug.jumpConsumedBuffer = false;
    m_debug.jumpUsedCoyote = false;

    // --- Ground state, as resolved by the previous character update -----------------------------
    const GroundState ground = m_character.GetGroundState();
    const bool wasGrounded = m_state.grounded;
    m_state.grounded = ground == GroundState::Grounded;
    m_state.onSteepSlope = ground == GroundState::SteepGround;
    m_state.groundNormal = m_character.GetGroundNormal();
    m_debug.slopeAngleDegrees =
        glm::degrees(std::acos(std::clamp(m_state.groundNormal.y, -1.0f, 1.0f)));

    // --- Landing ---------------------------------------------------------------------------------
    if (m_state.grounded && !wasGrounded)
    {
        m_state.landedThisTick = true;
        m_state.landingImpactSpeed = m_state.fallPeakSpeed;
        m_pendingLandingImpact = std::max(m_pendingLandingImpact, m_state.fallPeakSpeed);

        if (m_config.fallDamageEnabled && m_state.fallPeakSpeed > m_config.fallDamageMinSpeed)
        {
            const float span = std::max(m_config.fallDamageLethalSpeed - m_config.fallDamageMinSpeed, 0.01f);
            const float severity = (m_state.fallPeakSpeed - m_config.fallDamageMinSpeed) / span;
            ApplyDamage(std::clamp(severity, 0.0f, 1.0f) * 100.0f, "fall");
        }
        m_state.fallPeakSpeed = 0.0f;
    }
    if (m_state.grounded)
    {
        m_state.fallPeakSpeed = 0.0f;
        m_state.timeSinceGrounded = 0.0f;
    }
    else
    {
        m_state.fallPeakSpeed = std::max(m_state.fallPeakSpeed, -m_state.velocity.y);
        m_state.timeSinceGrounded += dt;
    }

    // --- Jump buffering --------------------------------------------------------------------------
    m_state.jumpBufferTimer = std::max(0.0f, m_state.jumpBufferTimer - dt);
    if (input.jump)
    {
        m_state.jumpBufferTimer = m_config.jumpBufferTime;
    }

    UpdateStance(input, dt);

    // --- Horizontal velocity ---------------------------------------------------------------------
    const glm::vec3 wish = ComputeWishDirection(input);
    const glm::vec2 wishFlat{wish.x, wish.z};
    const float wishLength = glm::length(wishFlat);
    const glm::vec2 wishDir = wishLength > 1e-4f ? wishFlat / wishLength : glm::vec2(0.0f);

    // Sprinting only counts when actually heading forwards, so nobody sprints backwards.
    const bool sprinting = input.sprint && input.move.y > 0.4f && m_state.stance == PlayerStance::Standing;
    float targetSpeed = m_config.SpeedForStance(m_state.stance, sprinting, input.walk);

    // Slower sideways and slower still backwards, blended so there is no discontinuity.
    if (wishLength > 1e-4f)
    {
        const glm::vec2 normalized = input.move / std::max(glm::length(input.move), 1e-4f);
        float directionScale = 1.0f;
        if (normalized.y < 0.0f)
        {
            directionScale = glm::mix(1.0f, m_config.backwardScale, -normalized.y);
        }
        directionScale = glm::mix(directionScale, directionScale * m_config.strafeScale,
                                  std::abs(normalized.x));
        targetSpeed *= directionScale;
    }
    m_debug.targetSpeed = wishLength > 1e-4f ? targetSpeed : 0.0f;

    glm::vec2 horizontal{m_state.velocity.x, m_state.velocity.z};
    const glm::vec2 targetVelocity = wishDir * (targetSpeed * std::min(wishLength, 1.0f));

    if (m_state.grounded)
    {
        // Move towards the target velocity at a bounded rate. This gives crisp, predictable control
        // with no drift, which is what stops a first-person controller feeling floaty.
        const float rate = wishLength > 1e-4f ? m_config.groundAcceleration : m_config.groundDeceleration;
        horizontal += ClampLength(targetVelocity - horizontal, rate * dt);
    }
    else if (wishLength > 1e-4f)
    {
        // In the air momentum is preserved: input steers, it does not brake.
        horizontal += ClampLength(targetVelocity - horizontal, m_config.airAcceleration * dt);
    }

    // --- Vertical velocity -------------------------------------------------------------------------
    float verticalVelocity = m_state.velocity.y;
    const float risingGravity = kEarthGravity * m_config.gravityScale;

    if (m_state.grounded && !m_state.jumpedThisTick)
    {
        // Follow whatever the ground itself is doing, and never accumulate downward speed while
        // standing still.
        verticalVelocity = std::min(0.0f, m_character.GetGroundVelocity().y);
    }
    else
    {
        const float gravity =
            kEarthGravity * (verticalVelocity < 0.0f ? m_config.fallGravityScale : m_config.gravityScale);
        verticalVelocity -= gravity * dt;
    }

    // --- Jump ---------------------------------------------------------------------------------------
    const bool withinCoyoteWindow = m_state.timeSinceGrounded <= m_config.coyoteTime;
    const bool canJump = (m_state.grounded || withinCoyoteWindow) && !m_state.onSteepSlope &&
                         m_state.stance != PlayerStance::Prone && m_state.alive;
    if (m_state.jumpBufferTimer > 0.0f && canJump)
    {
        // Solve for the take-off speed that reaches exactly jumpHeight, so changing gravityScale
        // alters the arc's shape without changing how high the player can reach.
        float jumpSpeed = std::sqrt(2.0f * risingGravity * std::max(m_config.jumpHeight, 0.0f));
        if (m_state.stance == PlayerStance::Crouching)
        {
            jumpSpeed *= 0.8f;
        }
        verticalVelocity = jumpSpeed;
        m_state.jumpedThisTick = true;
        m_debug.jumpConsumedBuffer = true;
        m_debug.jumpUsedCoyote = !m_state.grounded;
        m_state.jumpBufferTimer = 0.0f;
        m_state.grounded = false;
        m_state.timeSinceGrounded = m_config.coyoteTime + 1.0f; // no double jump from the same window
    }

    m_state.velocity = glm::vec3(horizontal.x, verticalVelocity, horizontal.y);

    // --- Advance the character ----------------------------------------------------------------------
    m_prevPosition = m_character.GetPosition();
    const float expectedDeltaY = m_state.velocity.y * dt;

    m_character.SetLinearVelocity(m_state.velocity);
    m_character.Update(dt, glm::vec3(0.0f, -risingGravity, 0.0f));

    m_state.position = m_character.GetPosition();
    // Read the velocity back: walking into a wall must actually remove that component, otherwise
    // the controller keeps pretending it is moving.
    m_state.velocity = m_character.GetLinearVelocity();

    // --- Stair smoothing -----------------------------------------------------------------------------
    // Walking up a step teleports the capsule upwards within a single tick. Feeding that straight to
    // the camera reads as a jolt, so the difference between where physics put us and where our own
    // velocity would have put us is handed to the view as an offset that decays away.
    const float actualDeltaY = m_state.position.y - m_prevPosition.y;
    const float stepDelta = actualDeltaY - expectedDeltaY;
    m_debug.lastStepUp = stepDelta;
    if (m_character.IsGrounded() && std::abs(stepDelta) > 0.004f &&
        std::abs(stepDelta) < m_config.stepHeight * 2.5f)
    {
        m_pendingStepOffset -= stepDelta;
    }

    if (m_state.grounded)
    {
        m_state.strideDistance += glm::length(glm::vec2(m_state.velocity.x, m_state.velocity.z)) * dt;
    }

    // Leaning. Sprinting cancels it, since nobody peeks round a corner at a run.
    const float leanTarget = (m_config.leanEnabled && !sprinting && m_state.stance != PlayerStance::Prone)
                                 ? std::clamp(input.lean, -1.0f, 1.0f)
                                 : 0.0f;
    m_state.leanAmount = SmoothTowards(m_state.leanAmount, leanTarget, m_config.leanSpeed, dt);

    m_debug.horizontalSpeed = m_state.HorizontalSpeed();
}

void PlayerController::UpdateView(float dt, float alpha)
{
    if (!m_initialized || dt <= 0.0f)
    {
        return;
    }

    // Render between the two most recent simulation states, so a 144 Hz display does not show the
    // 60 Hz simulation stepping.
    const glm::vec3 renderPosition = glm::mix(m_prevPosition, m_state.position, std::clamp(alpha, 0.0f, 1.0f));
    m_view.renderPosition = renderPosition;

    m_view.eyeHeight = SmoothTowards(m_view.eyeHeight, m_config.EyeHeightForStance(m_state.stance),
                                     m_config.eyeTransitionSpeed, dt);

    // Absorb any stair step recorded by the simulation, then decay the offset away.
    m_view.stepOffset = std::clamp(m_view.stepOffset + m_pendingStepOffset, -m_config.stepSmoothMax,
                                   m_config.stepSmoothMax);
    m_pendingStepOffset = 0.0f;
    m_view.stepOffset *= std::exp(-m_config.stepSmoothSpeed * dt);
    if (std::abs(m_view.stepOffset) < 1e-4f)
    {
        m_view.stepOffset = 0.0f;
    }

    // Landing kick, modelled as a critically damped spring so it settles without oscillating.
    if (m_pendingLandingImpact > 0.0f)
    {
        const float dip = std::min(m_pendingLandingImpact * m_config.landingDipPerSpeed, m_config.landingDipMax);
        m_view.landingDip -= dip;
        m_view.landingDipVelocity = 0.0f;
        m_pendingLandingImpact = 0.0f;
    }
    const float omega = std::max(m_config.landingRecoverSpeed, 0.01f);
    const float springAcceleration =
        -omega * omega * m_view.landingDip - 2.0f * omega * m_view.landingDipVelocity;
    m_view.landingDipVelocity += springAcceleration * dt;
    m_view.landingDip += m_view.landingDipVelocity * dt;

    // Head bob, driven by distance travelled rather than time so it stays in step with the stride
    // regardless of speed.
    const float speed = m_state.HorizontalSpeed();
    if (m_state.grounded && speed > 0.15f && m_config.bobAmount > 0.0f)
    {
        const float phase =
            m_state.strideDistance / std::max(m_config.bobStrideLength, 0.05f) * glm::two_pi<float>();
        const float intensity =
            std::min(speed / std::max(m_config.moveSpeed, 0.1f), m_config.bobSprintScale);
        m_view.bobOffset = std::sin(phase) * m_config.bobAmount * intensity;
    }
    else
    {
        m_view.bobOffset = SmoothTowards(m_view.bobOffset, 0.0f, 10.0f, dt);
    }

    m_view.yaw = m_state.yaw;
    m_view.pitch = m_state.pitch;
    // Leaning right tips the head right, so the up vector tips right and the horizon rolls the other
    // way. Rotating about the view axis, that needs a positive angle; negating it rolled the camera
    // into the lean instead of with it.
    m_view.leanRoll = m_state.leanAmount * glm::radians(m_config.leanAngleDegrees);

    m_view.eyePosition =
        renderPosition +
        glm::vec3(0.0f, m_view.eyeHeight + m_view.stepOffset + m_view.landingDip + m_view.bobOffset, 0.0f);
    // Shift the eye sideways as well as rolling it, so leaning actually moves the viewpoint out
    // past cover rather than only tilting the picture.
    m_view.eyePosition += m_view.Right() * (m_state.leanAmount * m_config.leanSideOffset);
}

void PlayerController::Teleport(const glm::vec3& footPosition)
{
    if (!m_initialized)
    {
        return;
    }
    m_character.SetPosition(footPosition);
    m_character.SetLinearVelocity(glm::vec3(0.0f));
    m_state.position = footPosition;
    m_prevPosition = footPosition;
    m_state.velocity = glm::vec3(0.0f);
    m_state.fallPeakSpeed = 0.0f;
    m_view.stepOffset = 0.0f;
    m_pendingStepOffset = 0.0f;
    m_pendingLandingImpact = 0.0f;
}

void PlayerController::Respawn(const glm::vec3& footPosition)
{
    const float yaw = m_state.yaw;
    const float pitch = m_state.pitch;
    m_state = PlayerState{};
    m_state.yaw = yaw;
    m_state.pitch = pitch;
    Teleport(footPosition);
    m_character.TryResize(m_config.standHeight, m_config.radius);
    m_state.stance = PlayerStance::Standing;
    m_view.landingDip = 0.0f;
    m_view.landingDipVelocity = 0.0f;
    PRED_LOG_INFO(Gameplay, "Player respawned");
}

void PlayerController::ApplyDamage(float amount, const char* cause)
{
    if (!m_state.alive || amount <= 0.0f)
    {
        return;
    }
    m_state.health -= amount;
    PRED_LOG_INFO(Gameplay, "Player took {:.0f} damage from {} ({:.0f} health left)", amount,
                  cause != nullptr ? cause : "unknown", std::max(0.0f, m_state.health));
    if (m_state.health <= 0.0f)
    {
        m_state.health = 0.0f;
        m_state.alive = false;
        PRED_LOG_INFO(Gameplay, "Player died: {}", cause != nullptr ? cause : "unknown");
    }
}

void PlayerController::DebugDraw(class DebugDraw& draw) const
{
    if (!m_initialized)
    {
        return;
    }

    const float height = m_config.HeightForStance(m_state.stance);
    const float radius = m_config.radius;
    const glm::vec3 feet = m_state.position;
    const uint32_t color = m_state.grounded ? Color::kGreen : Color::kYellow;

    // Capsule as two rings and connecting lines: enough to read the shape without the cost of a
    // proper wireframe capsule.
    draw.Sphere(feet + glm::vec3(0.0f, radius, 0.0f), radius, color, 16);
    draw.Sphere(feet + glm::vec3(0.0f, height - radius, 0.0f), radius, color, 16);
    for (int i = 0; i < 4; ++i)
    {
        const float angle = glm::two_pi<float>() * static_cast<float>(i) / 4.0f;
        const glm::vec3 offset{std::sin(angle) * radius, 0.0f, std::cos(angle) * radius};
        draw.Line(feet + offset + glm::vec3(0.0f, radius, 0.0f),
                  feet + offset + glm::vec3(0.0f, height - radius, 0.0f), color);
    }

    // Ground normal and horizontal velocity.
    if (m_state.grounded)
    {
        draw.Line(feet, feet + m_state.groundNormal * 0.6f, Color::kCyan);
    }
    const glm::vec3 flatVelocity{m_state.velocity.x, 0.0f, m_state.velocity.z};
    if (glm::length(flatVelocity) > 0.05f)
    {
        const glm::vec3 origin = feet + glm::vec3(0.0f, 0.05f, 0.0f);
        draw.Line(origin, origin + flatVelocity * 0.25f, Color::kMagenta);
    }

    // Eye position, so the camera offsets are visible from outside.
    draw.Sphere(m_view.eyePosition, 0.045f, Color::kRed, 10);
}

} // namespace pred
