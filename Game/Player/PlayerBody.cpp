#include "Game/Player/PlayerBody.h"

#include "Engine/Animation/IK.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/Player/PlayerController.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

constexpr int kLeft = 0;
constexpr int kRight = 1;

// Anthropometric ratios, expressed as fractions of total standing height. Using real proportions
// costs nothing and means the body reads as human-sized without art-directed guesswork.
namespace Ratio
{
constexpr float kPelvisHeight = 0.530f;
constexpr float kHipHalfWidth = 0.055f;
constexpr float kUpperLeg = 0.245f;
constexpr float kLowerLeg = 0.246f;
constexpr float kAnkle = 0.039f;
constexpr float kSpine = 0.086f;
constexpr float kChest = 0.086f;
constexpr float kNeck = 0.100f;
constexpr float kHead = 0.070f;
constexpr float kShoulderHalfWidth = 0.115f;
constexpr float kShoulderRise = 0.030f;
constexpr float kUpperArm = 0.186f;
constexpr float kLowerArm = 0.146f;
constexpr float kHand = 0.090f;
} // namespace Ratio

// Wraps an angle into (-pi, pi]. Without this, turning past the wrap point makes the body spin the
// long way round.
float WrapAngle(float radians)
{
    while (radians > glm::pi<float>())
    {
        radians -= glm::two_pi<float>();
    }
    while (radians <= -glm::pi<float>())
    {
        radians += glm::two_pi<float>();
    }
    return radians;
}

Transform LocalOffset(float x, float y, float z)
{
    Transform transform;
    transform.position = {x, y, z};
    return transform;
}

// A matrix placing a segment that runs from `a` to `b`, with its local +Y along the segment.
glm::mat4 SegmentMatrix(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 delta = b - a;
    const float length = glm::length(delta);
    const glm::quat rotation =
        length > 1e-5f ? RotationBetween(glm::vec3(0.0f, 1.0f, 0.0f), delta / length)
                       : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::translate(glm::mat4(1.0f), a) * glm::mat4_cast(rotation);
}

// Neutral grey work kit. Light enough to read against dark interiors and to show the shading that
// sells the shape, which matters more than colour while the body is still placeholder geometry.
const Material kSuitMaterial = Material::Diffuse({0.215f, 0.230f, 0.265f}, 0.88f);
const Material kGearMaterial = Material::Diffuse({0.170f, 0.180f, 0.210f}, 0.84f);
const Material kGloveMaterial = Material::Diffuse({0.115f, 0.120f, 0.140f}, 0.72f);
const Material kHelmetMaterial = Material::Diffuse({0.190f, 0.200f, 0.230f}, 0.62f);

} // namespace

void PlayerBody::BuildSkeleton(const PlayerConfig& playerConfig)
{
    const float h = playerConfig.standHeight;
    m_rig = HumanoidRig{};
    m_rig.height = h;
    m_rig.upperLegLength = Ratio::kUpperLeg * h;
    m_rig.lowerLegLength = Ratio::kLowerLeg * h;
    m_rig.upperArmLength = Ratio::kUpperArm * h;
    m_rig.lowerArmLength = Ratio::kLowerArm * h;
    m_rig.ankleHeight = Ratio::kAnkle * h;

    m_skeleton = Skeleton{};

    // Torso chain. The pelvis is the root, positioned relative to the feet by the caller.
    m_rig.pelvis = m_skeleton.AddBone("pelvis", kInvalidBone, LocalOffset(0.0f, 0.0f, 0.0f));
    m_rig.spine = m_skeleton.AddBone("spine", m_rig.pelvis, LocalOffset(0.0f, Ratio::kSpine * h, 0.0f));
    m_rig.chest = m_skeleton.AddBone("chest", m_rig.spine, LocalOffset(0.0f, Ratio::kChest * h, 0.0f));
    m_rig.neck = m_skeleton.AddBone("neck", m_rig.chest, LocalOffset(0.0f, Ratio::kNeck * h, 0.0f));
    m_rig.head = m_skeleton.AddBone("head", m_rig.neck, LocalOffset(0.0f, Ratio::kHead * h, 0.0f));

    const char* sideName[2] = {"left", "right"};
    const float sideSign[2] = {-1.0f, 1.0f};

    for (int side = 0; side < 2; ++side)
    {
        const std::string suffix = std::string("_") + sideName[side];
        const float sign = sideSign[side];

        m_rig.shoulder[side] = m_skeleton.AddBone(
            "shoulder" + suffix, m_rig.chest,
            LocalOffset(sign * Ratio::kShoulderHalfWidth * h, Ratio::kShoulderRise * h, 0.0f));
        m_rig.upperArm[side] = m_skeleton.AddBone("upper_arm" + suffix, m_rig.shoulder[side],
                                                  LocalOffset(0.0f, 0.0f, 0.0f));
        m_rig.lowerArm[side] = m_skeleton.AddBone("lower_arm" + suffix, m_rig.upperArm[side],
                                                  LocalOffset(0.0f, -m_rig.upperArmLength, 0.0f));
        m_rig.hand[side] = m_skeleton.AddBone("hand" + suffix, m_rig.lowerArm[side],
                                              LocalOffset(0.0f, -m_rig.lowerArmLength, 0.0f));
    }

    for (int side = 0; side < 2; ++side)
    {
        const std::string suffix = std::string("_") + sideName[side];
        const float sign = sideSign[side];

        m_rig.upperLeg[side] = m_skeleton.AddBone("upper_leg" + suffix, m_rig.pelvis,
                                                  LocalOffset(sign * Ratio::kHipHalfWidth * h, 0.0f, 0.0f));
        m_rig.lowerLeg[side] = m_skeleton.AddBone("lower_leg" + suffix, m_rig.upperLeg[side],
                                                  LocalOffset(0.0f, -m_rig.upperLegLength, 0.0f));
        m_rig.foot[side] = m_skeleton.AddBone("foot" + suffix, m_rig.lowerLeg[side],
                                              LocalOffset(0.0f, -m_rig.lowerLegLength, 0.0f));
    }

    if (!m_skeleton.ValidateOrdering())
    {
        PRED_LOG_ERROR(Animation, "Humanoid skeleton is not in parent-before-child order");
    }
    m_pose.ResetToBind(m_skeleton);
}

void PlayerBody::BuildParts(Scene& scene, MeshLibrary& meshes)
{
    const float h = m_rig.height;
    m_parts.clear();
    m_pose.ComputeGlobals(m_skeleton, glm::mat4(1.0f));

    struct PartSpec
    {
        const char* name;
        BoneIndex from;
        BoneIndex to;      // kInvalidBone for a fixed piece of equipment
        glm::vec3 size;    // stretched parts read x and z as width and depth
        glm::vec3 offset;  // equipment only, in the part's own frame
        PartFrame frame;
        const Material* material;
        bool hiddenInFirstPerson;
    };

    std::vector<PartSpec> specs;

    auto limb = [&](const char* name, BoneIndex from, BoneIndex to, float width, float depth,
                    const Material& material, bool hidden = false)
    {
        specs.push_back({name, from, to, {width, 0.0f, depth}, glm::vec3(0.0f), PartFrame::BodyYaw,
                         &material, hidden});
    };
    auto gear = [&](const char* name, BoneIndex bone, const glm::vec3& size, const glm::vec3& offset,
                    const Material& material, PartFrame frame = PartFrame::BodyYaw, bool hidden = false)
    {
        specs.push_back({name, bone, kInvalidBone, size, offset, frame, &material, hidden});
    };

    // A deliberately plain figure: enough parts to read as a person, few enough to keep the
    // silhouette clean. The detailed load-out belongs on a real mesh, not on stacked boxes.
    //
    // A real torso is much wider than it is deep. Modelling it as a square column pushes the chest
    // far enough forward to hide the legs from the wearer's own eyes.
    limb("hips", m_rig.pelvis, m_rig.spine, 0.150f * h, 0.108f * h, kSuitMaterial);
    limb("abdomen", m_rig.spine, m_rig.chest, 0.160f * h, 0.112f * h, kSuitMaterial);
    limb("chest", m_rig.chest, m_rig.neck, 0.170f * h, 0.118f * h, kGearMaterial);
    limb("neck", m_rig.neck, m_rig.head, 0.050f * h, 0.050f * h, kSuitMaterial, true);

    // A human head is about 0.13 of standing height tall and noticeably narrower than it is tall.
    // Sized from the crown down, so the top of the head lands at full standing height.
    gear("head", m_rig.head, {0.098f * h, 0.132f * h, 0.118f * h}, {0.0f, 0.063f * h, 0.004f * h},
         kHelmetMaterial, PartFrame::BoneFrame, true);

    // --- Arms.
    for (int side = 0; side < 2; ++side)
    {
        const char* upper = side == kLeft ? "upper_arm_left" : "upper_arm_right";
        const char* fore = side == kLeft ? "forearm_left" : "forearm_right";
        const char* glove = side == kLeft ? "hand_left" : "hand_right";

        limb(upper, m_rig.shoulder[side], m_rig.lowerArm[side], 0.060f * h, 0.060f * h, kSuitMaterial);
        limb(fore, m_rig.lowerArm[side], m_rig.hand[side], 0.050f * h, 0.050f * h, kSuitMaterial);
        gear(glove, m_rig.hand[side], {0.050f * h, 0.082f * h, 0.046f * h}, {0.0f, -0.028f * h, 0.0f},
             kGloveMaterial);
    }

    // --- Legs.
    for (int side = 0; side < 2; ++side)
    {
        const char* thigh = side == kLeft ? "thigh_left" : "thigh_right";
        const char* shin = side == kLeft ? "shin_left" : "shin_right";
        const char* boot = side == kLeft ? "boot_left" : "boot_right";

        limb(thigh, m_rig.upperLeg[side], m_rig.lowerLeg[side], 0.086f * h, 0.086f * h, kSuitMaterial);
        limb(shin, m_rig.lowerLeg[side], m_rig.foot[side], 0.068f * h, 0.068f * h, kSuitMaterial);
        gear(boot, m_rig.foot[side], {0.074f * h, m_rig.ankleHeight * 1.35f, 0.160f * h},
             {0.0f, 0.004f * h, -0.026f * h}, kGloveMaterial);
    }

    for (const PartSpec& spec : specs)
    {
        Part part;
        part.from = spec.from;
        part.to = spec.to;
        part.size = spec.size;
        part.offset = spec.offset;
        part.frame = spec.frame;
        part.hiddenInFirstPerson = spec.hiddenInFirstPerson;

        glm::vec3 boxSize = spec.size;
        if (spec.to != kInvalidBone)
        {
            const float length =
                std::max(glm::length(m_pose.GlobalPosition(spec.to) - m_pose.GlobalPosition(spec.from)),
                         0.02f);
            part.bindLength = length;
            boxSize = {spec.size.x, length, spec.size.z};
        }

        // A mesh per part, sized to that part. Sharing one unit box and scaling it would skew the
        // normals, because the shader transforms them by the model matrix directly.
        part.mesh = meshes.Upload(Primitives::Box(boxSize), std::string("player_") + spec.name);
        part.entity = scene.CreateMeshEntity(std::string("player_") + spec.name, Transform{}, part.mesh,
                                             *spec.material);
        m_parts.push_back(part);
    }
}

void PlayerBody::Build(Scene& scene, MeshLibrary& meshes, const PlayerConfig& playerConfig)
{
    BuildSkeleton(playerConfig);
    BuildParts(scene, meshes);
    for (FootState& foot : m_feet)
    {
        foot = FootState{};
    }
    m_built = true;
    PRED_LOG_INFO(Animation, "Player body built: {} bones, {} drawn parts", m_skeleton.BoneCount(),
                  m_parts.size());
}

void PlayerBody::Destroy(Scene& scene)
{
    for (const Part& part : m_parts)
    {
        scene.Destroy(part.entity);
    }
    m_parts.clear();
    m_built = false;
}

void PlayerBody::SetVisible(Scene& scene, bool visible)
{
    m_config.visible = visible;
    for (const Part& part : m_parts)
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(part.entity))
        {
            renderer->visible = visible && !(part.hiddenInFirstPerson && m_config.hideHead);
        }
    }
}

void PlayerBody::UpdatePosture(const PlayerState& state, const PlayerView& view,
                               const PlayerConfig& playerConfig, float dt)
{
    // --- Hips versus aim -------------------------------------------------------------------------
    // Locking the whole body to the camera means nothing ever rotates relative to the view, so the
    // body reads as welded to it. Instead the hips follow where the player is actually travelling
    // and the torso twists back towards where they are looking.
    const glm::vec3 flat{state.velocity.x, 0.0f, state.velocity.z};
    const float planarSpeed = glm::length(flat);
    const bool moving = planarSpeed > 0.35f && state.grounded;

    float hipTarget = m_bodyYaw;
    float turnSpeed = m_config.hipTurnSpeedIdle;
    if (moving)
    {
        // atan2 inverted to match the engine's convention that yaw 0 faces -Z.
        hipTarget = std::atan2(flat.x, -flat.z);
        turnSpeed = m_config.hipTurnSpeedMoving;
    }
    else
    {
        // Standing still, the hips hold their ground until the twist gets uncomfortable, then swing
        // round to catch up. That is the turn-in-place you see in a real person.
        const float twist = WrapAngle(view.yaw - m_bodyYaw);
        if (std::abs(twist) > glm::radians(m_config.maxTorsoTwistDegrees))
        {
            const float sign = twist > 0.0f ? 1.0f : -1.0f;
            hipTarget = view.yaw - sign * glm::radians(m_config.turnInPlaceDegrees);
        }
    }
    m_bodyYaw = m_bodyYaw + WrapAngle(hipTarget - m_bodyYaw) * (1.0f - std::exp(-turnSpeed * dt));
    m_bodyYaw = WrapAngle(m_bodyYaw);

    // Whatever the hips did not cover, the spine makes up, so the chest stays aimed down the view.
    const float torsoTwist =
        std::clamp(WrapAngle(view.yaw - m_bodyYaw), -glm::radians(m_config.maxTorsoTwistDegrees * 1.4f),
                   glm::radians(m_config.maxTorsoTwistDegrees * 1.4f));

    // Blend the whole posture towards the target stance, so going prone plays out over a moment
    // instead of snapping between two poses.
    const Config::StancePose& target = state.stance == PlayerStance::Crouching ? m_config.crouch
                                       : state.stance == PlayerStance::Prone   ? m_config.prone
                                                                               : m_config.stand;
    const float blend = m_config.stanceBlendSpeed;
    m_pose_blend.pelvisRatio = SmoothTowards(m_pose_blend.pelvisRatio, target.pelvisRatio, blend, dt);
    m_pose_blend.pelvisPitchDeg =
        SmoothTowards(m_pose_blend.pelvisPitchDeg, target.pelvisPitchDeg, blend, dt);
    m_pose_blend.spineLeanDeg = SmoothTowards(m_pose_blend.spineLeanDeg, target.spineLeanDeg, blend, dt);
    m_pose_blend.footBackRatio = SmoothTowards(m_pose_blend.footBackRatio, target.footBackRatio, blend, dt);
    m_pose_blend.footSpread = SmoothTowards(m_pose_blend.footSpread, target.footSpread, blend, dt);
    m_pose_blend.armForwardDeg = SmoothTowards(m_pose_blend.armForwardDeg, target.armForwardDeg, blend, dt);

    const float pelvisPitch = glm::radians(m_pose_blend.pelvisPitchDeg);
    const float spineLean = glm::radians(m_pose_blend.spineLeanDeg);

    // Placed from the interpolated render position, not from the simulation state. The camera uses
    // the interpolated one, so using the raw state here made the body step at the tick rate while
    // the view moved at the frame rate, which reads as the body stuttering underneath you.
    const glm::vec3 facing{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
    m_rootPosition = view.renderPosition + glm::vec3(0.0f, m_pose_blend.pelvisRatio * m_rig.height, 0.0f) -
                     facing * m_config.eyeForwardOffset;

    const float speed = state.HorizontalSpeed();
    m_gaitWeight = SmoothTowards(m_gaitWeight,
                                 state.grounded ? std::clamp(speed / std::max(playerConfig.moveSpeed, 0.1f),
                                                             0.0f, 1.6f)
                                                : 0.0f,
                                 m_config.responsiveness, dt);

    const float targetLean =
        std::clamp(speed * m_config.leanPerSpeed, 0.0f, m_config.maxLean) * (state.grounded ? 1.0f : 0.3f);
    m_lean = SmoothTowards(m_lean, targetLean, m_config.responsiveness * 0.5f, dt);

    // Stride phase advances with distance travelled, so the legs stay in step with actual motion
    // rather than drifting against it as speed changes.
    m_stridePhase = state.strideDistance / std::max(m_config.strideLength, 0.05f);

    m_pose.ResetToBind(m_skeleton);

    // Hip sway and bob, the two things that stop a walk looking like a sliding statue.
    const float phase = m_stridePhase * glm::two_pi<float>();
    const float sway = std::sin(phase) * m_config.hipSwayAmount * m_gaitWeight;
    const float bob = -std::abs(std::cos(phase)) * m_config.hipBobAmount * m_gaitWeight;

    Transform& pelvis = m_pose.Local(m_rig.pelvis);
    pelvis.position = glm::vec3(sway, bob, 0.0f);
    // Pelvis pitch lays the body down for prone. It stays upright for crouching, where the fold
    // belongs at the hip and knee instead.
    pelvis.rotation = glm::angleAxis(pelvisPitch, glm::vec3(1.0f, 0.0f, 0.0f)) *
                      glm::angleAxis(glm::radians(sway * 60.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    // Lean forward from the spine, counter-rotate the chest slightly so the torso does not fold, and
    // spread the twist between the two so it does not all happen at one joint.
    // The twist is negated for the same reason the root rotation is: a model facing -Z turns the
    // opposite way to the yaw convention.
    m_pose.Local(m_rig.spine).rotation =
        glm::angleAxis(-torsoTwist * 0.45f, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(spineLean * 0.65f + glm::radians(m_lean * 0.6f), glm::vec3(1.0f, 0.0f, 0.0f));
    m_pose.Local(m_rig.chest).rotation =
        glm::angleAxis(-torsoTwist * 0.55f, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(spineLean * 0.35f - glm::radians(m_lean * 0.2f), glm::vec3(1.0f, 0.0f, 0.0f));

    // The neck and head undo whatever the pelvis and spine did, so the head stays level and keeps
    // looking where the player is aiming. Without this, laying the pelvis flat for prone drags the
    // head face-down into the floor and the spine reads as a backbend.
    const float torsoPitch = pelvisPitch + spineLean;
    m_pose.Local(m_rig.neck).rotation =
        glm::angleAxis(-torsoPitch * 0.55f + view.pitch * 0.35f - glm::radians(m_lean * 0.2f),
                       glm::vec3(1.0f, 0.0f, 0.0f));
    m_pose.Local(m_rig.head).rotation =
        glm::angleAxis(-torsoPitch * 0.45f + view.pitch * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));

    // Arms swing opposite the legs.
    for (int side = 0; side < 2; ++side)
    {
        const float sideSign = side == kLeft ? 1.0f : -1.0f;
        const float swing = std::sin(phase + (side == kLeft ? 0.0f : glm::pi<float>())) *
                            glm::radians(m_config.armSwingDegrees) * m_gaitWeight;
        const float rest = glm::radians(m_config.armRestDegrees);

        // Arms reach forward as the body goes down, so they are not left dangling through the floor
        // when prone.
        m_pose.Local(m_rig.upperArm[side]).rotation =
            glm::angleAxis(swing + glm::radians(m_pose_blend.armForwardDeg), glm::vec3(1.0f, 0.0f, 0.0f)) *
            glm::angleAxis(sideSign * rest, glm::vec3(0.0f, 0.0f, 1.0f));
        // A permanently straight elbow reads as a mannequin, so keep a little bend at all times.
        m_pose.Local(m_rig.lowerArm[side]).rotation =
            glm::angleAxis(glm::radians(12.0f) + std::abs(swing) * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
    }

    const glm::mat4 root =
        glm::translate(glm::mat4(1.0f), m_rootPosition) * glm::mat4_cast(BodyRotation());
    m_pose.ComputeGlobals(m_skeleton, root);
}

glm::quat PlayerBody::BodyRotation() const
{
    // Yaw zero looks down -Z, and yaw increases turning right. Rotating a model's local -Z by +yaw
    // about +Y sends it the other way, so the sign is negated here. Getting this wrong mirrors the
    // body: it turns left when the camera turns right.
    return glm::angleAxis(-m_bodyYaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

void PlayerBody::UpdateLegs(const PlayerState& state, const PlayerView& view,
                            const PlayerConfig& playerConfig, PhysicsWorld& physics, float dt)
{
    (void)playerConfig;
    const glm::vec3 flatVelocity{state.velocity.x, 0.0f, state.velocity.z};
    const float speed = glm::length(flatVelocity);
    const glm::vec3 moveDirection = speed > 0.05f ? flatVelocity / speed : glm::vec3(0.0f);

    const float phase = m_stridePhase * glm::two_pi<float>();
    const glm::vec3 facing{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
    const glm::quat bodyRotation = BodyRotation();

    // Prone trails the legs out behind; crouching keeps the feet under the hips, which is what
    // makes it read as a squat rather than a lunge.
    const float legSpan = m_rig.upperLegLength + m_rig.lowerLegLength;
    const glm::vec3 stanceFootOffset = -facing * (m_pose_blend.footBackRatio * legSpan);

    // Knees bend towards the body's front while upright. Once the pelvis is laid flat that axis
    // points at the sky, which folds the legs upwards, so the pole is blended back down towards the
    // ground as the body goes prone.
    const glm::vec3 bodyForward =
        glm::normalize(glm::vec3(m_pose.Global(m_rig.pelvis) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    const float flatness = std::clamp(m_pose_blend.pelvisPitchDeg / 90.0f, 0.0f, 1.0f);
    const glm::vec3 kneePole =
        glm::normalize(glm::mix(bodyForward, glm::vec3(0.0f, -1.0f, 0.0f), flatness) + glm::vec3(1e-4f));

    for (int side = 0; side < 2; ++side)
    {
        const glm::vec3 hip = m_pose.GlobalPosition(m_rig.upperLeg[side]);
        const float footPhase = phase + (side == kLeft ? 0.0f : glm::pi<float>());

        // Swing the foot forward and back along the direction of travel, lifting it on the forward
        // half of the cycle.
        const float reach = std::cos(footPhase) * m_config.strideLength * 0.5f * m_gaitWeight;
        const float lift = std::max(0.0f, std::sin(footPhase)) * m_config.stepHeight * m_gaitWeight;

        // Stance width: a crouch plants the feet wider, prone brings them together.
        const glm::vec3 right{std::cos(m_bodyYaw), 0.0f, std::sin(m_bodyYaw)};
        const float sideSign = side == kLeft ? -1.0f : 1.0f;
        const glm::vec3 spread =
            right * (sideSign * (m_pose_blend.footSpread - 1.0f) * Ratio::kHipHalfWidth * m_rig.height);

        glm::vec3 target = hip + moveDirection * reach + stanceFootOffset + spread;
        target.y = view.renderPosition.y + m_rig.ankleHeight + lift;

        // Trace for the real ground under the foot so it lands on stairs and slopes instead of
        // hovering at the character's own base height.
        const glm::vec3 traceStart = target + glm::vec3(0.0f, 0.6f, 0.0f);
        const RayHit hit = physics.RayCast(traceStart, glm::vec3(0.0f, -1.0f, 0.0f), 1.4f);
        if (hit)
        {
            target.y = hit.position.y + m_rig.ankleHeight + lift;
        }

        FootState& foot = m_feet[static_cast<size_t>(side)];
        // Smoothing hides the discontinuity when the trace steps from one surface to another.
        foot.position = SmoothTowards(foot.position, target, m_config.footPlantSmoothing, dt);
        foot.planted = lift < 0.01f;

        // Limb lengths are constant in every stance; the knee bend is what absorbs a lowered hip.
        const TwoBoneIKResult ik = SolveTwoBoneIK(hip, foot.position, kneePole, m_rig.upperLegLength,
                                                  m_rig.lowerLegLength);

        // Write the solved chain straight into the pose's globals. Parent before child, because
        // setting a bone rebuilds everything after it from local transforms.
        m_pose.SetGlobal(m_skeleton, m_rig.upperLeg[side], SegmentMatrix(hip, ik.jointPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerLeg[side],
                         SegmentMatrix(ik.jointPosition, ik.endPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.foot[side],
                         glm::translate(glm::mat4(1.0f), ik.endPosition) * glm::mat4_cast(bodyRotation));
    }
}

void PlayerBody::PushToScene(Scene& scene)
{
    const glm::quat bodyRotation = BodyRotation();

    for (const Part& part : m_parts)
    {
        Transform* transform = scene.GetTransform(part.entity);
        MeshRenderer* renderer = scene.GetMeshRenderer(part.entity);
        if (transform == nullptr || renderer == nullptr)
        {
            continue;
        }

        renderer->visible = m_config.visible && !(part.hiddenInFirstPerson && m_config.hideHead);
        if (!renderer->visible)
        {
            continue;
        }

        if (part.to == kInvalidBone)
        {
            // Equipment: a fixed box hanging at a joint. Limb bones are re-solved by IK and carry an
            // arbitrary roll about their own axis, so anything that must face forwards is aligned to
            // the body rather than to the bone it hangs from.
            const glm::quat frame =
                part.frame == PartFrame::BoneFrame
                    ? glm::normalize(glm::quat_cast(glm::mat3(m_pose.Global(part.from))))
                    : bodyRotation;
            transform->position = m_pose.GlobalPosition(part.from) + frame * part.offset;
            transform->rotation = frame;
            transform->scale = glm::vec3(1.0f);
            continue;
        }

        const glm::vec3 a = m_pose.GlobalPosition(part.from);
        const glm::vec3 b = m_pose.GlobalPosition(part.to);
        const glm::vec3 delta = b - a;
        const float length = glm::length(delta);

        // Box meshes are built centred on their own origin, so the transform sits at the segment's
        // midpoint and rotates the local +Y axis onto the bone.
        transform->position = (a + b) * 0.5f;
        transform->rotation = length > 1e-5f
                                  ? RotationBetween(glm::vec3(0.0f, 1.0f, 0.0f), delta / length)
                                  : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        // Parts are built at their bind length; stances change limb spans slightly, so stretch along
        // the bone only.
        transform->scale = glm::vec3(1.0f, part.bindLength > 1e-4f ? length / part.bindLength : 1.0f, 1.0f);
    }
}

void PlayerBody::Update(Scene& scene, const PlayerState& state, const PlayerView& view,
                        const PlayerConfig& playerConfig, PhysicsWorld& physics, float dt)
{
    if (!m_built || dt <= 0.0f)
    {
        return;
    }
    UpdatePosture(state, view, playerConfig, dt);
    UpdateLegs(state, view, playerConfig, physics, dt);
    PushToScene(scene);
}

void PlayerBody::DebugDraw(class DebugDraw& draw) const
{
    if (!m_built)
    {
        return;
    }

    for (int i = 0; i < m_skeleton.BoneCount(); ++i)
    {
        const BoneIndex index = static_cast<BoneIndex>(i);
        const BoneIndex parent = m_skeleton.GetBone(index).parent;
        const glm::vec3 position = m_pose.GlobalPosition(index);

        if (parent != kInvalidBone)
        {
            draw.Line(m_pose.GlobalPosition(parent), position, Color::kCyan);
        }
        draw.Sphere(position, 0.018f, Color::kWhite, 6);
    }

    // Foot targets, and whether each is planted.
    for (int side = 0; side < 2; ++side)
    {
        const FootState& foot = m_feet[static_cast<size_t>(side)];
        draw.Sphere(foot.position, 0.045f, foot.planted ? Color::kGreen : Color::kYellow, 10);
    }
}

} // namespace pred
