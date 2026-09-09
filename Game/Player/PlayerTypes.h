#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>

namespace pred
{

enum class PlayerStance : uint8_t
{
    Standing,
    Crouching,
    Prone
};

const char* PlayerStanceName(PlayerStance stance);

// One tick's worth of intent. Deliberately plain data with no pointers: the same struct will be
// sent over the network and replayed during prediction, so it must be trivially copyable and must
// fully describe what the player asked for on that tick.
struct PlayerInput
{
    glm::vec2 move{0.0f}; // x = strafe right, y = forward. Clamped to the unit disc.
    float yaw = 0.0f;     // absolute look angles in radians, sampled per frame not per tick
    float pitch = 0.0f;
    bool jump = false;
    bool sprint = false;
    bool walk = false;         // slow, quiet movement
    bool crouchHeld = false;   // already resolved from hold-vs-toggle before it gets here
    bool proneHeld = false;
    // -1 left, +1 right. Part of the simulation rather than the camera, because peeking round a
    // corner changes what the player can see and be seen from.
    float lean = 0.0f;
    // Multiplies the stance's movement speed. Aiming down the sights is the first thing to use it;
    // injury and heavy cargo are the obvious next ones. Kept as a plain number so the controller
    // does not have to know why the player is slower.
    float speedScale = 1.0f;
};

// Everything the simulation needs to continue from this tick. Also the unit of network state:
// given a PlayerState and a sequence of PlayerInputs, the result must be reproducible.
struct PlayerState
{
    glm::vec3 position{0.0f}; // at the feet
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;

    PlayerStance stance = PlayerStance::Standing;
    PlayerStance desiredStance = PlayerStance::Standing;
    bool stanceBlocked = false; // could not stand up: something overhead

    bool grounded = false;
    bool onSteepSlope = false;
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};

    float timeSinceGrounded = 0.0f; // drives coyote time
    float jumpBufferTimer = 0.0f;
    bool jumpedThisTick = false;

    // Climbing a ledge. While this runs the character is moved along a fixed path rather than
    // simulated, which is why it lives in the state: a client replaying its inputs has to replay
    // the climb the same way, and it cannot do that from a timer kept somewhere else.
    bool mantling = false;
    float mantleTime = 0.0f;
    float mantleDuration = 0.0f;
    glm::vec3 mantleFrom{0.0f};
    glm::vec3 mantleTo{0.0f};
    // The lip itself: where the wall face meets the top. The hands go here, and it is nowhere near
    // the landing spot, which is a body's depth further on.
    glm::vec3 mantleEdge{0.0f};

    float fallPeakSpeed = 0.0f;      // fastest downward speed during the current fall
    bool landedThisTick = false;
    float landingImpactSpeed = 0.0f; // downward speed at the moment of landing

    float health = 100.0f;
    bool alive = true;

    // Distance travelled on foot, used to drive stride-phase effects such as head bob.
    float strideDistance = 0.0f;
    // Where in the two-step walk cycle the player is, wrapped to [0, 1). Both the camera dip and
    // the body's feet run off this one value, so the head drops exactly when a foot lands rather
    // than on a cycle of its own that slowly drifts out of step with the legs.
    float stridePhase = 0.0f;
    // Smoothed lean, -1 to +1. Simulated rather than presentation, so it can later be blocked by
    // geometry and read by the creature's line of sight.
    float leanAmount = 0.0f;

    float HorizontalSpeed() const;
};

// All movement tuning. Loaded from Assets/Data/player.json and hot-reloaded, so the feel can be
// dialled in without rebuilding. Defaults here are the fallback when the file is missing.
struct PlayerConfig
{
    // --- Speeds, metres per second ---
    float walkSpeed = 1.5f;   // slow, deliberate
    float moveSpeed = 3.4f;   // default pace
    float sprintSpeed = 5.6f;
    float crouchSpeed = 1.5f;
    float proneSpeed = 0.75f;
    float backwardScale = 0.78f; // moving backwards is slower
    float strafeScale = 0.92f;

    // --- Acceleration, metres per second squared ---
    float groundAcceleration = 60.0f;
    float groundDeceleration = 75.0f;
    float airAcceleration = 14.0f;

    // --- Jump and gravity ---
    float jumpHeight = 0.95f;      // apex above the takeoff point
    float gravityScale = 1.7f;     // heavier than reality; real gravity feels floaty
    float fallGravityScale = 2.1f; // extra pull while descending, for a snappier arc
    float coyoteTime = 0.12f;      // grace period to still jump after leaving a ledge
    float jumpBufferTime = 0.15f;  // pressing jump slightly early still works on landing

    // --- Capsule, metres ---
    float standHeight = 1.80f;
    float crouchHeight = 1.15f;
    float proneHeight = 0.60f;
    float radius = 0.32f;
    float stepHeight = 0.35f;
    float maxSlopeAngle = 46.0f;

    // --- Camera ---
    float standEyeHeight = 1.66f;
    float crouchEyeHeight = 1.02f;
    float proneEyeHeight = 0.36f;
    float eyeTransitionSpeed = 9.0f;
    // Stair steps teleport the capsule upwards. Without smoothing, the camera snaps and stairs feel
    // like a series of small shocks.
    float stepSmoothSpeed = 11.0f;
    float stepSmoothMax = 0.5f;
    float landingDipPerSpeed = 0.012f;
    float landingDipMax = 0.20f;
    float landingRecoverSpeed = 8.0f;
    // The head drops as each foot lands, twice per cycle, and never rises above standing height.
    // That is what walking really does to your eyeline, and it is also what gives the legs room:
    // with the hips at standing height a leg is almost straight, so without a dip a planted foot
    // cannot be more than a few centimetres from the hip before the leg runs out of length.
    float bobAmount = 0.055f;      // how far the eye dips at each footfall
    // A constant lowering while moving, on top of the footfall dip. Standing still the legs are
    // straight and the character stands tall; walking, the knees soften and the hips come down.
    // That is both what people do and what gives the legs the room to reach out and plant a step.
    float bobWalkLower = 0.055f;
    float bobSprintScale = 1.8f;   // deeper at speed, as a real gait is

    // --- Stride ---
    // Metres of travel per full two-step cycle, growing with speed: a walk takes short steps, a run
    // takes long ones. Cadence would otherwise go up with speed alone and the legs would windmill.
    float strideLengthBase = 0.75f;
    float strideLengthPerSpeed = 0.22f;
    float strideLengthMax = 2.00f;
    // Share of the cycle each foot spends on the ground, from a walk to a run. Below 0.5 the two
    // stances no longer overlap, which is what makes a run a run.
    float stanceFractionWalk = 0.56f;
    float stanceFractionRun = 0.44f;

    // --- Mantling ---
    // Pulling yourself up onto something. Below the step height the character walks up it without
    // noticing; above head height there is nothing to pull against. In between, pressing jump at a
    // ledge climbs it.
    bool mantleEnabled = true;
    float mantleMinHeight = 0.40f;  // anything lower is a step, not a climb
    float mantleMaxHeight = 1.60f;  // about chest height on a 1.8 m body
    float mantleReach = 0.55f;      // how far in front of the capsule a ledge can be
    float mantleClearance = 0.30f;  // how much flat top a ledge needs before it can be stood on
    // A low vault is quick and a full pull-up is not, so the time scales with the height. Being
    // stuck in a climb is the cost of using one, and it is what stops mantling being strictly
    // better than walking round.
    float mantleSecondsLow = 0.32f;
    float mantleSecondsHigh = 0.85f;

    // --- Leaning ---
    float leanAngleDegrees = 16.0f; // camera roll at full lean
    float leanSideOffset = 0.32f;   // how far the eye shifts sideways
    float leanSpeed = 9.0f;
    bool leanEnabled = true;

    // --- Look ---
    float maxPitchDegrees = 89.0f;

    // --- Fall damage ---
    float fallDamageMinSpeed = 9.5f;    // below this, landing is free
    float fallDamageLethalSpeed = 24.0f; // at or above this, landing kills
    bool fallDamageEnabled = true;

    bool LoadFromFile(const std::filesystem::path& file);
    bool SaveToFile(const std::filesystem::path& file) const;

    float HeightForStance(PlayerStance stance) const;
    float EyeHeightForStance(PlayerStance stance) const;
    // Metres per full two-step cycle at a given speed. Longer steps at speed rather than a faster
    // cadence, which is what people actually do.
    float StrideLength(float speed) const;
    // Share of the cycle a foot is on the ground, blending from a walk towards a run.
    float StanceFraction(float speed) const;
    float SpeedForStance(PlayerStance stance, bool sprint, bool walk) const;
};

} // namespace pred
