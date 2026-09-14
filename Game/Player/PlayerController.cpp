#include "Game/Player/PlayerController.h"

#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/DebugDraw.h"

#include <glm/common.hpp>
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
    // Not in mid air. Lying down is a thing you do to a floor, and there is nothing to lie on
    // halfway through a jump: what it did instead was shrink the capsule while the body carried on
    // falling, so a player could dive through a gap they could not walk through and land already
    // flat. Crouching in the air is left alone, because tucking your legs up is a real thing a
    // jumping body does and it changes nothing about where the body can fit.
    //
    // Measured on how long the feet have been off the ground rather than on whether they are off it
    // this instant. Walking over a stair nosing or a doorway lip reports airborne for a tick or two
    // at a time, and a rule written on the instant would sit a crawling player up every time they
    // crossed one.
    const bool airborne = !m_state.grounded && m_state.timeSinceGrounded > m_config.coyoteTime;
    if (input.proneHeld && !airborne)
    {
        desired = PlayerStance::Prone;
    }
    else if (input.crouchHeld || input.proneHeld)
    {
        // Already down and now off the ground, which is walking off a ledge while prone. Crouching
        // is the nearest thing to it that is not lying on nothing, and it is what the body will be
        // in when it lands.
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

bool PlayerController::FindMantle(const PlayerInput& input, glm::vec3& outTarget, glm::vec3& outEdge) const
{
    if (!m_config.mantleEnabled || m_physics == nullptr || !m_state.alive ||
        m_state.stance == PlayerStance::Prone)
    {
        return false;
    }
    // You climb what you are walking at. Without this you would haul yourself over every crate you
    // happened to be standing next to whenever you pressed jump.
    if (input.move.y < 0.5f)
    {
        return false;
    }

    const glm::vec3 forward{std::sin(m_state.yaw), 0.0f, -std::cos(m_state.yaw)};
    const glm::vec3 feet = m_state.position;
    const float ahead = m_config.radius + m_config.mantleReach;

    // Look down from above and in front. Where that lands is the top of whatever is there, which is
    // the only thing a climb actually needs to know.
    const glm::vec3 probe = feet + forward * ahead;
    const float ceiling = m_config.mantleMaxHeight + 0.25f;
    const RayHit top = m_physics->RayCast(probe + glm::vec3(0.0f, ceiling, 0.0f),
                                          glm::vec3(0.0f, -1.0f, 0.0f), ceiling);
    if (!top)
    {
        return false;
    }

    const float height = top.position.y - feet.y;
    if (height < m_config.mantleMinHeight || height > m_config.mantleMaxHeight)
    {
        return false;
    }
    // Facing a wall, not a slope. A surface you could walk up is not something you climb.
    if (top.normal.y < 0.7f)
    {
        return false;
    }

    // There has to be something in the way, or this is flat ground and walking is the answer.
    const RayHit wall =
        m_physics->RayCast(feet + glm::vec3(0.0f, height * 0.5f, 0.0f), forward, ahead + 0.1f);
    if (!wall)
    {
        return false;
    }

    // And what is in front has to be the thing whose top that is.
    //
    // The probe above looks down from in front of the player, which finds the top of whatever is
    // under that point. Standing at a wall too tall to climb, that point is on the far side of it,
    // and if anything stands over there at a climbable height, its top is what comes back. The
    // checks that follow all agreed, because they are asking about a ledge that really is there.
    // What none of them asked was whether the wall in between goes on up past it, and the climb
    // itself moves the capsule by setting its position, so nothing stopped the player passing
    // straight through. Tracing forward again at just above the lip is the whole of the question:
    // if something is still in the way up there, this is not the top of it.
    const RayHit beyond = m_physics->RayCast(
        glm::vec3(feet.x, top.position.y + 0.08f, feet.z), forward, ahead + 0.1f);
    if (beyond)
    {
        return false;
    }

    // Enough flat top to stand on. A ledge one centimetre deep is a lip, and climbing onto it drops
    // you straight back off the far side.
    const glm::vec3 landing = probe + forward * m_config.mantleClearance;
    const RayHit far = m_physics->RayCast(landing + glm::vec3(0.0f, ceiling, 0.0f),
                                          glm::vec3(0.0f, -1.0f, 0.0f), ceiling);
    if (!far || std::abs(far.position.y - top.position.y) > m_config.stepHeight)
    {
        return false;
    }

    // And headroom, or the climb ends with the capsule inside a ceiling.
    const float standing = m_config.HeightForStance(PlayerStance::Standing);
    const RayHit above = m_physics->RayCast(top.position + glm::vec3(0.0f, 0.05f, 0.0f),
                                            glm::vec3(0.0f, 1.0f, 0.0f), standing);
    if (above)
    {
        return false;
    }

    outTarget = glm::vec3(landing.x, top.position.y + 0.02f, landing.z);
    outEdge = glm::vec3(wall.position.x, top.position.y, wall.position.z);
    return true;
}

void PlayerController::StepMantle(float dt)
{
    m_state.mantleTime += dt;
    const float duration = std::max(m_state.mantleDuration, 0.05f);
    const float t = std::clamp(m_state.mantleTime / duration, 0.0f, 1.0f);

    // Up first, then over. Doing both at once slides the body through the corner of the ledge,
    // which is what a straight line between two points looks like when there is a solid between
    // them. Rising early and stepping across late is also what the movement actually is.
    const float rise = glm::smoothstep(0.0f, 0.65f, t);
    const float across = glm::smoothstep(0.35f, 1.0f, t);

    glm::vec3 position = m_state.mantleFrom;
    position.y = glm::mix(m_state.mantleFrom.y, m_state.mantleTo.y, rise);
    position.x = glm::mix(m_state.mantleFrom.x, m_state.mantleTo.x, across);
    position.z = glm::mix(m_state.mantleFrom.z, m_state.mantleTo.z, across);

    m_prevPosition = m_state.position;
    m_character.SetPosition(position);
    m_character.SetLinearVelocity(glm::vec3(0.0f));
    m_state.position = position;
    // The velocity reported is what the climb is actually doing, so the body leans and the camera
    // bobs as if it were moving, which it is.
    m_state.velocity = dt > 1e-5f ? (position - m_prevPosition) / dt : glm::vec3(0.0f);
    m_state.grounded = false;
    m_state.fallPeakSpeed = 0.0f;
    m_state.timeSinceGrounded = 0.0f;

    if (t >= 1.0f)
    {
        m_state.mantling = false;
        m_state.velocity = glm::vec3(0.0f);
        m_state.grounded = true;
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

    // The dead do not walk, climb or stand up. Gravity still applies, so a body killed in mid-air
    // comes down rather than hanging there, and the look angles still track so a corpse can be
    // looked around from while somebody is spectating it.
    PlayerInput living = input;
    if (!m_state.alive)
    {
        living = PlayerInput{};
        living.yaw = input.yaw;
        living.pitch = input.pitch;
        m_state.mantling = false;
    }
    const PlayerInput& effective = living;

    if (m_attached)
    {
        // Held where something else put them: inside a locker now, carried by a creature later.
        // Movement is not simulated at all rather than being fed zero input, because a capsule
        // that overlaps geometry gets pushed out by depenetration however still it is being asked
        // to stand. That is what drifted the player out through the side of a locker.
        m_character.SetPosition(m_attachPosition);
        m_character.SetLinearVelocity(glm::vec3(0.0f));
        m_prevPosition = m_attachPosition;
        m_state.position = m_attachPosition;
        m_state.velocity = glm::vec3(0.0f);
        m_state.grounded = true;
        m_state.fallPeakSpeed = 0.0f;
        m_state.timeSinceGrounded = 0.0f;
        UpdateStance(effective, dt);
        return;
    }

    // A climb is a fixed path, not a simulation. Nothing below runs while one is in progress: the
    // player has committed, which is the cost of the shortcut.
    if (m_state.mantling)
    {
        StepMantle(dt);
        UpdateStance(effective, dt);
        return;
    }

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
    if (effective.jump)
    {
        m_state.jumpBufferTimer = m_config.jumpBufferTime;
    }

    UpdateStance(effective, dt);

    // --- Horizontal velocity ---------------------------------------------------------------------
    const glm::vec3 wish = ComputeWishDirection(effective);
    const glm::vec2 wishFlat{wish.x, wish.z};
    const float wishLength = glm::length(wishFlat);
    const glm::vec2 wishDir = wishLength > 1e-4f ? wishFlat / wishLength : glm::vec2(0.0f);

    // --- Stamina ---------------------------------------------------------------------------------
    // Spent by sprinting and recovered by not, after a pause. Once it is gone you have to let a
    // little back before you can sprint again, or sprint flickers on and off at zero and the player
    // runs at a permanent limp.
    const bool wantsSprint =
        effective.sprint && effective.move.y > 0.4f && m_state.stance == PlayerStance::Standing;
    const bool canSprint = m_state.stamina > (m_state.winded ? m_config.staminaSprintAgain : 0.0f);
    const bool sprinting = wantsSprint && canSprint && m_state.alive;

    if (sprinting)
    {
        m_state.stamina =
            std::max(m_state.stamina - dt / std::max(m_config.sprintSeconds, 0.1f), 0.0f);
        m_state.staminaRecoveryDelay = m_config.staminaRecoverDelay;
        if (m_state.stamina <= 0.0f)
        {
            m_state.winded = true;
        }
    }
    else
    {
        m_state.staminaRecoveryDelay = std::max(m_state.staminaRecoveryDelay - dt, 0.0f);
        if (m_state.staminaRecoveryDelay <= 0.0f)
        {
            m_state.stamina =
                std::min(m_state.stamina + dt / std::max(m_config.staminaRecoverSeconds, 0.1f), 1.0f);
        }
        if (m_state.stamina >= m_config.staminaSprintAgain)
        {
            m_state.winded = false;
        }
    }
    float targetSpeed = m_config.SpeedForStance(m_state.stance, sprinting, effective.walk) *
                        std::clamp(effective.speedScale, 0.05f, 1.0f);

    // Injury. Below the threshold a player slows down, all the way to a limp at the point of death.
    // Health that does nothing until it reaches zero is an accounting entry, not a wound.
    const float healthFraction = std::clamp(m_state.health / 100.0f, 0.0f, 1.0f);
    if (healthFraction < m_config.injuryThreshold)
    {
        const float hurt = 1.0f - healthFraction / std::max(m_config.injuryThreshold, 0.01f);
        targetSpeed *= glm::mix(1.0f, m_config.injuredSpeedScale, hurt);
    }

    // Slower sideways and slower still backwards, blended so there is no discontinuity.
    if (wishLength > 1e-4f)
    {
        const glm::vec2 normalized = effective.move / std::max(glm::length(effective.move), 1e-4f);
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

    // --- Mantle -------------------------------------------------------------------------------------
    // Checked before the jump and on the same button. Pressing jump at a ledge you could climb
    // should climb it rather than bouncing you off the front of it, and a separate key for climbing
    // is a key nobody presses.
    //
    // It works in the air too, so running at a wall and jumping catches the top, which is both what
    // people try and what makes the movement feel like it belongs to a body.
    if (m_state.jumpBufferTimer > 0.0f && m_state.alive)
    {
        glm::vec3 target{0.0f};
        glm::vec3 edge{0.0f};
        if (FindMantle(effective, target, edge))
        {
            m_state.mantling = true;
            m_state.mantleTime = 0.0f;
            m_state.mantleFrom = m_state.position;
            m_state.mantleTo = target;
            m_state.mantleEdge = edge;
            const float height = std::max(target.y - m_state.position.y, 0.0f);
            const float reachedFraction =
                std::clamp(height / std::max(m_config.mantleMaxHeight, 0.01f), 0.0f, 1.0f);
            m_state.mantleDuration =
                glm::mix(m_config.mantleSecondsLow, m_config.mantleSecondsHigh, reachedFraction);
            m_state.jumpBufferTimer = 0.0f;
            StepMantle(dt);
            return;
        }
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

    // What walking along the ground we are already on would have raised us by.
    //
    // Without this, a ramp is indistinguishable from a staircase and was treated as one. Walking on
    // a slope, our own velocity is very nearly horizontal and the physics lifts us as we go, so the
    // difference between the two was the whole of the climb — about two centimetres a tick at
    // walking pace on a twenty-five degree ramp, which is well over the threshold below. Every
    // single tick was therefore recorded as a step up, the view pulled the camera down by it and
    // then let it recover, and what that looks like is the whole body juddering on every slope in
    // the game. On a staircase the ground under the foot is flat and this term is zero, so a real
    // step is still the whole jump and is still smoothed.
    float slopeRise = 0.0f;
    if (m_character.IsGrounded() && m_state.groundNormal.y > 0.10f)
    {
        const glm::vec3 moved = m_state.position - m_prevPosition;
        slopeRise = -(moved.x * m_state.groundNormal.x + moved.z * m_state.groundNormal.z) /
                    m_state.groundNormal.y;
    }
    const float stepDelta = actualDeltaY - expectedDeltaY - slopeRise;
    m_debug.lastStepUp = stepDelta;
    if (m_character.IsGrounded() && std::abs(stepDelta) > 0.004f &&
        std::abs(stepDelta) < m_config.stepHeight * 2.5f)
    {
        m_pendingStepOffset -= stepDelta;
    }

    if (m_state.grounded)
    {
        const float distance = glm::length(glm::vec2(m_state.velocity.x, m_state.velocity.z)) * dt;
        m_state.strideDistance += distance;
        // Integrated rather than derived from the total distance, because the stride length changes
        // with speed; dividing a running total by a moving divisor would jump the phase, and the
        // legs with it, every time the player sped up or slowed down.
        m_state.stridePhase =
            glm::fract(m_state.stridePhase +
                       distance / m_config.StrideLength(m_state.HorizontalSpeed(), m_state.stance));
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

    // Lower on a slope, which is what gives the legs anywhere to go.
    //
    // The proportions leave almost nothing spare: the hip sits at 0.530 of standing height and the
    // leg plus ankle comes to 0.542, about two per cent in hand. On the flat that is enough. Walking
    // up a ramp the trailing foot is behind the body and below it and the two add together into a
    // reach the leg does not have — measured at 0.998 of its own length, dead straight — so the foot
    // is pulled in towards the hip and the stride collapses into a shuffle.
    //
    // Shifting the stance up the slope and shortening the stride were both tried and both measured:
    // the leg stays at 0.998 through every value of either, because the shortfall is vertical and
    // neither of them is. Bending the knees is the only thing that makes any, and bending your knees
    // lowers your head: the body is anchored to the eye, so dropping the pelvis alone nets out to
    // nothing once the head is put back on the camera.
    //
    // Which is also what a person does on a hill, and it is small: nine centimetres at twenty-five
    // degrees, eased in like any other change of stance.
    const float slope = m_state.grounded && m_state.groundNormal.y > 0.10f
                            ? glm::length(glm::vec2(m_state.groundNormal.x, m_state.groundNormal.z)) /
                                  m_state.groundNormal.y
                            : 0.0f;
    const float slopeCrouch = std::min(slope * m_config.eyeSlopeCrouch, m_config.eyeSlopeCrouchMax);
    m_view.eyeHeight =
        SmoothTowards(m_view.eyeHeight, m_config.EyeHeightForStance(m_state.stance) - slopeCrouch,
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
        // Deepest at each footfall and never above standing height, which is what a real gait does
        // to your eyeline. It is not decoration: the body anchors itself to the eye, so this dip is
        // also what lowers the hips far enough for a leg to reach forward and plant a step.
        const float phase = m_state.stridePhase * glm::two_pi<float>();
        const float dip = (1.0f + std::cos(phase * 2.0f)) * 0.5f;
        const float intensity =
            std::min(speed / std::max(m_config.moveSpeed, 0.1f), m_config.bobSprintScale);
        const float target = -(m_config.bobWalkLower + dip * m_config.bobAmount) * intensity;
        // Eased in, so setting off does not drop the camera in one frame.
        m_view.bobOffset = SmoothTowards(m_view.bobOffset, target, 12.0f, dt);
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

    // And then make sure it is not inside anything.
    //
    // The eye is a stack of offsets and several of them move it away from the middle of the
    // capsule: leaning pushes it sideways out past cover, a step pulls it down, a landing dips it,
    // the walk bobs it. Each is reasonable alone, and together they put the camera through a wall
    // or into the floor. That is what "the camera is in the map" is, and no amount of tuning the
    // individual offsets fixes it, because the fault is in the total.
    //
    // Traced from a point that is always well inside the player, so whatever it finds is genuinely
    // between the body and the eye rather than something the eye is standing on.
    if (m_physics != nullptr)
    {
        const float inside = std::min(m_view.eyeHeight, m_config.HeightForStance(m_state.stance) * 0.5f);
        const glm::vec3 from = renderPosition + glm::vec3(0.0f, inside, 0.0f);
        const glm::vec3 toEye = m_view.eyePosition - from;
        const float distance = glm::length(toEye);
        if (distance > 1e-4f)
        {
            // The near plane needs room in front of it, or stopping exactly at the surface still
            // shows what is on the other side.
            constexpr float kNearPlaneRoom = 0.10f;
            const glm::vec3 direction = toEye / distance;
            const RayHit blocked = m_physics->RayCast(from, direction, distance + kNearPlaneRoom);
            if (blocked)
            {
                m_view.eyePosition =
                    from + direction * std::max(blocked.distance - kNearPlaneRoom, 0.0f);
            }
        }
    }
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

void PlayerController::RestoreState(const PlayerState& state)
{
    if (!m_initialized)
    {
        return;
    }
    m_state = state;
    m_character.SetPosition(state.position);
    m_character.SetLinearVelocity(state.velocity);
    ApplyConfigToCharacter();
    m_prevPosition = state.position;
}

void PlayerController::Attach(const glm::vec3& footPosition, float yaw)
{
    Teleport(footPosition);
    m_attachPosition = footPosition;
    m_attached = true;
    m_state.yaw = yaw;
    m_state.velocity = glm::vec3(0.0f);
}

void PlayerController::Detach(const glm::vec3& footPosition)
{
    m_attached = false;
    Teleport(footPosition);
}

void PlayerController::Respawn(const glm::vec3& footPosition)
{
    const float yaw = m_state.yaw;
    const float pitch = m_state.pitch;
    m_state = PlayerState{};
    m_state.yaw = yaw;
    m_state.pitch = pitch;
    // Respawning has to break any attachment, or a player killed inside a locker comes back still
    // pinned to it.
    m_attached = false;
    Teleport(footPosition);
    m_character.TryResize(m_config.standHeight, m_config.radius);
    m_state.stance = PlayerStance::Standing;
    m_view.landingDip = 0.0f;
    m_view.landingDipVelocity = 0.0f;
    PRED_LOG_INFO(Gameplay, "Player respawned");
}

void PlayerController::ApplyDamage(float amount, const char* cause)
{
    // A client predicts where it will be, not whether it is alive. Its own copy of this controller
    // applied fall damage, so a landing the host had run a little differently killed the player on
    // their own screen with nobody else told, and they then revived on their own clock as well.
    if (!m_decidesDamage || !m_state.alive || amount <= 0.0f)
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
