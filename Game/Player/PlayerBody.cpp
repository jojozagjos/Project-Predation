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

void PlayerBody::BuildForSimulation(const PlayerConfig& playerConfig)
{
    BuildSkeleton(playerConfig);
    m_parts.clear();
    for (FootState& foot : m_feet)
    {
        foot = FootState{};
    }
    m_built = true;
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
        float moveYaw = std::atan2(flat.x, -flat.z);

        // Hips align to the *line* of travel, not its direction, taking whichever end is nearer to
        // where the player is aiming. Walking backwards then keeps the body facing forward and the
        // feet stepping back, instead of spinning the whole character round to face its own heels.
        float offset = WrapAngle(moveYaw - view.yaw);
        if (std::abs(offset) > glm::radians(135.0f))
        {
            moveYaw = WrapAngle(moveYaw + glm::pi<float>());
            offset = WrapAngle(moveYaw - view.yaw);
        }

        // Never let the hips stray further from the aim than the torso can twist back, or the upper
        // body ends up wrenched round to compensate.
        const float maxOffset = glm::radians(m_config.maxTorsoTwistDegrees * 0.9f);
        hipTarget = view.yaw + std::clamp(offset, -maxOffset, maxOffset);
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
    m_pose_blend.pelvisPitchDeg =
        SmoothTowards(m_pose_blend.pelvisPitchDeg, target.pelvisPitchDeg, blend, dt);
    m_pose_blend.spineLeanDeg = SmoothTowards(m_pose_blend.spineLeanDeg, target.spineLeanDeg, blend, dt);
    m_pose_blend.footBackRatio = SmoothTowards(m_pose_blend.footBackRatio, target.footBackRatio, blend, dt);
    m_pose_blend.footSpread = SmoothTowards(m_pose_blend.footSpread, target.footSpread, blend, dt);
    m_pose_blend.armForwardDeg = SmoothTowards(m_pose_blend.armForwardDeg, target.armForwardDeg, blend, dt);

    const float pelvisPitch = glm::radians(m_pose_blend.pelvisPitchDeg);
    const float spineLean = glm::radians(m_pose_blend.spineLeanDeg);
    // How far through the transition to lying flat we are. Drives the crawl and the knee pole.
    m_flatness = std::clamp(m_pose_blend.pelvisPitchDeg / 90.0f, 0.0f, 1.0f);

    // Provisional placement only. The height here cancels out: the pose is built relative to this
    // root and then the whole root is shifted so the head lands on the eye, so any starting height
    // gives the same answer. What matters is the horizontal position, which comes from the
    // interpolated render position rather than the simulation state. Using the raw state made the
    // body step at the tick rate while the view moved at the frame rate, which reads as the body
    // stuttering underneath you.
    m_rootPosition = view.renderPosition + glm::vec3(0.0f, m_rig.height * 0.53f, 0.0f);

    // Measured against the speed this stance normally travels at, not against a standing walk.
    // Otherwise crawling at 0.75 m/s reads as a fifth of a stride and the limbs barely move.
    const float speed = state.HorizontalSpeed();
    const float referenceSpeed = std::max(playerConfig.SpeedForStance(state.stance, false, false), 0.1f);
    m_gaitWeight =
        SmoothTowards(m_gaitWeight,
                      state.grounded ? std::clamp(speed / referenceSpeed, 0.0f, 1.6f) : 0.0f,
                      m_config.responsiveness, dt);

    const float targetLean =
        std::clamp(speed * m_config.leanPerSpeed, 0.0f, m_config.maxLean) * (state.grounded ? 1.0f : 0.3f);
    m_lean = SmoothTowards(m_lean, targetLean, m_config.responsiveness * 0.5f, dt);

    // The walk cycle comes from the simulation, not from a second clock kept here. The camera dip
    // runs off the same value, so the head drops exactly as a foot lands.
    m_stridePhase = state.stridePhase;

    m_pose.ResetToBind(m_skeleton);

    // Hip sway and bob, the two things that stop a walk looking like a sliding statue.
    const float phase = m_stridePhase * glm::two_pi<float>();
    const float sway = std::sin(phase) * m_config.hipSwayAmount * m_gaitWeight;
    const float bob = -std::abs(std::cos(phase)) * m_config.hipBobAmount * m_gaitWeight;

    Transform& pelvis = m_pose.Local(m_rig.pelvis);
    pelvis.position = glm::vec3(sway, bob, 0.0f);
    // Pelvis pitch lays the body down for prone. It stays upright for crouching, where the fold
    // belongs at the hip and knee instead.
    //
    // Note the sign. Rotating by a positive angle about +X tips the body's up axis towards +Z,
    // which is *backwards* here, because forward is -Z. Every forward pitch below is therefore
    // negative. Getting this wrong arches the body backwards: it is what made crouch look like a
    // limbo and prone like a backbend.
    pelvis.rotation = glm::angleAxis(-pelvisPitch, glm::vec3(1.0f, 0.0f, 0.0f)) *
                      glm::angleAxis(glm::radians(sway * 60.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    // Lean forward from the spine, counter-rotate the chest slightly so the torso does not fold, and
    // spread the twist between the two so it does not all happen at one joint.
    // The twist is negated for the same reason the root rotation is: a model facing -Z turns the
    // opposite way to the yaw convention.
    const float runLean = glm::radians(m_lean);
    // Peek lean: the torso tips sideways so the body follows the camera out past cover.
    const float peek = state.leanAmount * glm::radians(m_config.leanAngleDegrees);
    m_pose.Local(m_rig.spine).rotation =
        glm::angleAxis(-torsoTwist * 0.45f, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(-(spineLean * 0.65f + runLean * 0.6f), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::angleAxis(-peek * 0.55f, glm::vec3(0.0f, 0.0f, 1.0f));
    m_pose.Local(m_rig.chest).rotation =
        glm::angleAxis(-torsoTwist * 0.55f, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(-(spineLean * 0.35f + runLean * 0.2f), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::angleAxis(-peek * 0.45f, glm::vec3(0.0f, 0.0f, 1.0f));

    // The neck and head undo whatever the pelvis and spine did, so the head stays level and keeps
    // looking where the player is aiming. Positive here, because it is cancelling a forward pitch.
    // Without it, laying the pelvis flat for prone drives the head face-down into the floor.
    const float torsoPitch = pelvisPitch + spineLean + runLean * 0.8f;
    m_pose.Local(m_rig.neck).rotation =
        glm::angleAxis(torsoPitch * 0.55f + view.pitch * 0.35f, glm::vec3(1.0f, 0.0f, 0.0f));
    m_pose.Local(m_rig.head).rotation =
        glm::angleAxis(torsoPitch * 0.45f + view.pitch * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));

    // Arms swing opposite the legs.
    for (int side = 0; side < 2; ++side)
    {
        // Must match the skeleton's own convention, where the left side sits at -X. Having this
        // backwards swung both arms inwards, straight through the thighs.
        const float sideSign = side == kLeft ? -1.0f : 1.0f;
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

    glm::mat4 root = glm::translate(glm::mat4(1.0f), m_rootPosition) * glm::mat4_cast(BodyRotation());
    m_pose.ComputeGlobals(m_skeleton, root);

    // Anchor the head to the camera, rather than placing the body by a guessed offset and hoping the
    // head lands near the eye. Solving it exactly fixes several problems at once: the head no longer
    // drifts off-centre when the hips turn away from the view while strafing, the torso stops
    // sitting too far forward, and the camera stops clipping through the body on stairs and while
    // crouching, because the head now carries the same step smoothing and landing dip the camera
    // does. It costs one extra pass over nineteen bones.
    const glm::vec3 viewFacing{std::sin(view.yaw), 0.0f, -std::cos(view.yaw)};
    const glm::vec3 desiredHead = view.eyePosition - viewFacing * m_config.eyeForwardOfHead -
                                  glm::vec3(0.0f, m_config.eyeAboveHead, 0.0f);
    m_rootPosition += desiredHead - m_pose.GlobalPosition(m_rig.head);

    root = glm::translate(glm::mat4(1.0f), m_rootPosition) * glm::mat4_cast(BodyRotation());
    m_pose.ComputeGlobals(m_skeleton, root);
}

glm::quat PlayerBody::BodyRotation() const
{
    // Yaw zero looks down -Z, and yaw increases turning right. Rotating a model's local -Z by +yaw
    // about +Y sends it the other way, so the sign is negated here. Getting this wrong mirrors the
    // body: it turns left when the camera turns right.
    return glm::angleAxis(-m_bodyYaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

void PlayerBody::SetWeapon(Scene& scene, MeshLibrary& meshes, const WeaponDefinition* definition)
{
    if (definition == nullptr || definition->id == kInvalidWeapon)
    {
        if (m_weaponEntity.IsValid())
        {
            scene.Destroy(m_weaponEntity);
            scene.Destroy(m_magazineEntity);
            scene.Destroy(m_muzzleFlashEntity);
            m_weaponEntity = Entity{};
            m_magazineEntity = Entity{};
            m_muzzleFlashEntity = Entity{};
        }
        m_weaponId = kInvalidWeapon;
        return;
    }
    if (definition->id == m_weaponId && m_weaponEntity.IsValid())
    {
        return;
    }

    if (m_weaponEntity.IsValid())
    {
        scene.Destroy(m_weaponEntity);
        scene.Destroy(m_magazineEntity);
        scene.Destroy(m_muzzleFlashEntity);
    }

    m_weaponId = definition->id;
    m_weaponVisual = BuildWeaponVisual(*definition);

    const std::string key = "weapon_" + definition->key;
    const Material metal = Material::Metal(definition->color, 0.42f);
    // The magazine reads slightly darker, so it can be told apart from the receiver while it is
    // moving during a reload.
    const Material magazineMetal = Material::Metal(definition->color * 0.75f, 0.55f);

    m_weaponEntity = scene.CreateMeshEntity(key, Transform{}, meshes.Upload(m_weaponVisual.body, key),
                                            metal);
    m_magazineEntity = scene.CreateMeshEntity(key + "_mag", Transform{},
                                              meshes.Upload(m_weaponVisual.magazine, key + "_mag"),
                                              magazineMetal);

    // A flash is a scaled-to-nothing sphere most of the time. Giving it its own entity means firing
    // costs a transform write rather than creating and destroying geometry.
    const float flashRadius = std::max(definition->size.y, 0.06f) * 0.9f;
    m_muzzleFlashEntity =
        scene.CreateMeshEntity(key + "_flash", Transform{},
                               meshes.Upload(Primitives::Sphere(flashRadius, 10, 6), key + "_flash"),
                               Material::Emissive({1.0f, 0.78f, 0.36f}, 3.4f));
}

glm::vec3 PlayerBody::MuzzlePoint() const
{
    return m_weaponTransform.position + m_weaponTransform.rotation * m_weaponVisual.muzzle;
}

bool PlayerBody::UpdateWeaponHold(const PlayerView& view, float dt)
{
    if (!m_weaponEntity.IsValid())
    {
        return false;
    }

    const float aim = glm::clamp(m_weaponPose.aim, 0.0f, 1.0f);
    const bool flat = m_flatness > 0.5f;

    // --- Sway -----------------------------------------------------------------------------------
    // The weapon lags the view a little when the player turns, then catches up. Deliberately held
    // on the weapon and not on the hands: smoothing the hands towards a moving grip point pulled
    // them off the gun every time the player looked around quickly.
    const float yawDelta = WrapAngle(view.yaw - m_lastViewYaw);
    const float pitchDelta = view.pitch - m_lastViewPitch;
    m_lastViewYaw = view.yaw;
    m_lastViewPitch = view.pitch;
    const float swayScale = (1.0f - aim * 0.7f) * m_config.weaponSwayAmount;
    m_weaponSway += glm::vec2(-yawDelta, -pitchDelta) * swayScale;
    m_weaponSway = glm::vec2(SmoothTowards(m_weaponSway.x, 0.0f, m_config.weaponSwayRecover, dt),
                             SmoothTowards(m_weaponSway.y, 0.0f, m_config.weaponSwayRecover, dt));
    m_weaponSway = glm::clamp(m_weaponSway, glm::vec2(-0.09f), glm::vec2(0.09f));

    // --- The frame the weapon is carried in -----------------------------------------------------
    // Aiming follows the view exactly, because the sights have to line up with it. Carrying it does
    // not: a weapon held ready stays in front of the chest, so it follows yaw fully but pitch only
    // part way. Following pitch fully meant looking at your feet swung the gun round behind you.
    const float carryPitch = view.pitch * glm::mix(m_config.weaponCarryPitchFollow, 1.0f, aim);
    const float cp = std::cos(carryPitch);
    const glm::vec3 carryForward{std::sin(view.yaw) * cp, std::sin(carryPitch), -std::cos(view.yaw) * cp};
    const glm::vec3 carryRight = glm::normalize(glm::cross(carryForward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 carryUp = glm::cross(carryRight, carryForward);

    // The aim frame, which the barrel is pointed down.
    const float ap = std::cos(view.pitch);
    const glm::vec3 aimForward{std::sin(view.yaw) * ap, std::sin(view.pitch), -std::cos(view.yaw) * ap};
    const glm::vec3 aimRight = glm::normalize(glm::cross(aimForward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 aimUp = glm::cross(aimRight, aimForward);

    // --- Where the weapon sits ------------------------------------------------------------------
    // Held ready it sits low and to the right, close enough to the view axis to be on screen: at a
    // 90 degree horizontal field of view anything at arm's length below the chin is outside the
    // frame entirely, which is why the weapon used to be invisible until the sights came up.
    //
    // Sighted, the origin drops by exactly the sight height, which puts the sight block on the view
    // axis rather than near it.
    const glm::vec3 readyOffset = carryRight * m_config.weaponReadyRight +
                                  carryUp * m_config.weaponReadyDown + carryForward * m_config.weaponReadyForward;
    const glm::vec3 sightedOffset = aimForward * m_config.weaponAimForward - aimUp * m_weaponVisual.sightHeight;

    // Prone puts it down beside the body, muzzle forward, out of the way of the arm that is doing
    // the crawling.
    const glm::vec3 proneOffset = carryRight * 0.14f + carryUp * -0.30f + carryForward * 0.34f;

    glm::vec3 offset = glm::mix(readyOffset, sightedOffset, aim);
    if (flat)
    {
        offset = glm::mix(offset, proneOffset, glm::clamp((m_flatness - 0.5f) * 2.0f, 0.0f, 1.0f));
    }

    // --- Which way it points --------------------------------------------------------------------
    const glm::quat sighted = LookRotation(aimForward, aimUp);
    // Held ready the muzzle drops and the weapon cants inwards, the way a carried rifle does.
    const glm::quat ready = LookRotation(carryForward, carryUp) *
                            glm::angleAxis(glm::radians(-9.0f), glm::vec3(1.0f, 0.0f, 0.0f)) *
                            glm::angleAxis(glm::radians(-4.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::quat rotation = glm::slerp(ready, sighted, aim);

    // --- Reload ---------------------------------------------------------------------------------
    // The weapon rolls towards the player so the magazine well is where the hand can reach it, dips
    // as the magazine comes out, and comes back up as the new one seats. `reload` runs 0 to 1.
    glm::vec3 magazineOffset{0.0f};
    float magazineVisible = 1.0f;
    if (m_weaponPose.reloading)
    {
        const float t = glm::clamp(m_weaponPose.reload, 0.0f, 1.0f);
        // A raised-cosine envelope: nothing at either end, most of the movement in the middle.
        const float envelope = 0.5f - 0.5f * std::cos(t * glm::two_pi<float>());
        rotation = rotation * glm::angleAxis(glm::radians(38.0f * envelope), glm::vec3(0.0f, 0.0f, 1.0f)) *
                   glm::angleAxis(glm::radians(-16.0f * envelope), glm::vec3(1.0f, 0.0f, 0.0f));
        offset += carryUp * (-0.09f * envelope) + carryRight * (-0.05f * envelope);

        // The old magazine drops away over the first third, then the new one rises into place.
        constexpr float kOut = 0.34f;
        constexpr float kIn = 0.78f;
        if (t < kOut)
        {
            const float drop = t / kOut;
            magazineOffset.y = -0.32f * drop * drop;
            magazineVisible = 1.0f - drop;
        }
        else if (t < kIn)
        {
            magazineVisible = 0.0f;
        }
        else
        {
            const float rise = (t - kIn) / (1.0f - kIn);
            magazineOffset.y = -0.30f * (1.0f - rise) * (1.0f - rise);
            magazineVisible = 1.0f;
        }
    }

    // --- Fire kick ------------------------------------------------------------------------------
    const float kick = glm::clamp(m_weaponPose.kick, 0.0f, 1.0f);
    if (kick > 0.0f)
    {
        offset -= aimForward * (0.055f * kick);
        rotation = rotation * glm::angleAxis(glm::radians(-7.5f * kick), glm::vec3(1.0f, 0.0f, 0.0f));
    }

    // Sway is applied last, as a rotation about the carry frame, so it moves the whole hold: weapon,
    // magazine and both hands together.
    rotation = glm::angleAxis(m_weaponSway.x, glm::vec3(0.0f, 1.0f, 0.0f)) *
               glm::angleAxis(m_weaponSway.y, carryRight) * rotation;
    offset += carryRight * (m_weaponSway.x * 0.16f) + carryUp * (m_weaponSway.y * 0.16f);

    m_weaponTransform.position = view.eyePosition + offset;
    m_weaponTransform.rotation = rotation;
    m_weaponTransform.scale = glm::vec3(1.0f);

    m_magazineTransform.position =
        m_weaponTransform.position + rotation * (m_weaponVisual.magazineSeated + magazineOffset);
    m_magazineTransform.rotation = rotation;
    m_magazineTransform.scale = glm::vec3(magazineVisible > 0.01f ? 1.0f : 0.0f);

    // The flash lives at the muzzle and is scaled to nothing except on the frames just after a shot.
    m_muzzleFlashTransform.position = MuzzlePoint();
    m_muzzleFlashTransform.rotation = rotation;
    const float flash = glm::clamp((kick - 0.62f) / 0.38f, 0.0f, 1.0f);
    m_muzzleFlashTransform.scale = glm::vec3(flash);

    // --- Hands ----------------------------------------------------------------------------------
    const glm::vec3 barrel = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 weaponUp = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 weaponRight = glm::cross(barrel, weaponUp);

    glm::vec3 gripPoints[2] = {
        m_weaponTransform.position + rotation * m_weaponVisual.supportGrip, // left, the support hand
        m_weaponTransform.position - weaponUp * 0.02f                       // right, the trigger hand
    };

    // A long weapon puts the handguard further out than the arm can reach, and an over-extended IK
    // chain draws the arm as a straight bar pointing at the target. Slide the support hand back
    // along the barrel until it is within reach instead: a shorter hold looks like a hold, a
    // straight arm looks broken.
    {
        const glm::vec3 leftShoulder = m_pose.GlobalPosition(m_rig.shoulder[kLeft]);
        const float armSpan = (m_rig.upperArmLength + m_rig.lowerArmLength) * 0.94f;
        while (glm::length(gripPoints[kLeft] - leftShoulder) > armSpan &&
               glm::dot(gripPoints[kLeft] - m_weaponTransform.position, barrel) > 0.02f)
        {
            gripPoints[kLeft] -= barrel * 0.02f;
        }
    }

    // Crawling, the support hand lets go and reaches for the ground; only the trigger hand stays on
    // the weapon, which is how anyone actually low-crawls with a rifle.
    const int firstSide = flat ? kRight : kLeft;
    // The support hand also comes off the gun while the magazine is being changed.
    const bool supportHandFree = flat || (m_weaponPose.reloading && m_weaponPose.reload > 0.12f &&
                                          m_weaponPose.reload < 0.92f);

    for (int side = firstSide; side < 2; ++side)
    {
        if (side == kLeft && supportHandFree)
        {
            continue;
        }
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[side]);
        const float sideSign = side == kLeft ? -1.0f : 1.0f;

        FootState& hand = m_hands[static_cast<size_t>(side)];
        // Placed, not smoothed. A hand holding a weapon is rigidly attached to it, so any smoothing
        // here shows up as the grip sliding off the gun whenever the player turns.
        hand.position = gripPoints[side];
        hand.planted = true;

        // Elbows drop and swing outwards, away from the ribs. Aiming tucks them in, which is what
        // brings the silhouette down behind the sights.
        const glm::vec3 elbowPole =
            glm::normalize(-carryUp * 1.0f + carryRight * (sideSign * (0.85f - 0.45f * aim)) -
                           carryForward * 0.35f);
        const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                                  m_rig.upperArmLength, m_rig.lowerArmLength);

        m_pose.SetGlobal(m_skeleton, m_rig.upperArm[side], SegmentMatrix(shoulder, ik.jointPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[side], SegmentMatrix(ik.jointPosition, ik.endPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.hand[side],
                         glm::translate(glm::mat4(1.0f), ik.endPosition) * glm::mat4_cast(rotation));
    }

    // The magazine change needs a hand to do it with, so the support hand goes to the magazine well
    // rather than hanging in mid air.
    if (supportHandFree && !flat && m_weaponPose.reloading)
    {
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[kLeft]);
        const glm::vec3 target = m_magazineTransform.position + weaponRight * -0.03f;
        FootState& hand = m_hands[static_cast<size_t>(kLeft)];
        hand.position = SmoothTowards(hand.position, target, m_config.weaponHandSmoothing, dt);

        const glm::vec3 elbowPole =
            glm::normalize(-carryUp * 1.0f - carryRight * 0.9f - carryForward * 0.25f);
        const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                                  m_rig.upperArmLength, m_rig.lowerArmLength);
        m_pose.SetGlobal(m_skeleton, m_rig.upperArm[kLeft], SegmentMatrix(shoulder, ik.jointPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[kLeft], SegmentMatrix(ik.jointPosition, ik.endPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.hand[kLeft],
                         glm::translate(glm::mat4(1.0f), ik.endPosition) * glm::mat4_cast(rotation));
    }
    return true;
}

void PlayerBody::UpdateArms(const PlayerState& state, const PlayerView& view, PhysicsWorld& physics,
                            float dt)
{
    // A weapon takes the arms over: both hands go on it, and the elbows follow. It keeps hold of it
    // when the body goes flat too, because dropping a rifle to crawl is not what anybody does.
    const bool holding = UpdateWeaponHold(view, dt);

    // Upright and empty-handed, the procedural rotations set in UpdatePosture are enough.
    if (m_flatness < 0.02f)
    {
        return;
    }

    // Flat on the ground: the arms crawl. With a weapon in the trigger hand, only the support arm
    // reaches and pulls; the weapon hand stays on the gun and comes along.
    UpdateCrawlArms(state, view, physics, dt, holding);
}

void PlayerBody::UpdateCrawlArms(const PlayerState& state, const PlayerView& view,
                                 PhysicsWorld& physics, float dt, bool supportArmOnly)
{
    const glm::vec3 facing{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
    const glm::vec3 right{std::cos(m_bodyYaw), 0.0f, std::sin(m_bodyYaw)};

    // Crawling runs on its own cycle, opposite the legs, so the body reads as pulling itself along
    // rather than sliding. Holding a weapon halves the cycle: one arm is doing all the work, so it
    // reaches, plants and pulls on every stride rather than every other one.
    const float cycleLength =
        m_config.crawlCycleLength * (supportArmOnly ? 0.55f : 1.0f);
    const float crawlPhase = state.strideDistance / std::max(cycleLength, 0.05f) * glm::two_pi<float>();

    const int firstSide = supportArmOnly ? kLeft : 0;
    const int lastSide = supportArmOnly ? kLeft : 1;

    for (int side = firstSide; side <= lastSide; ++side)
    {
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[side]);
        const float sideSign = side == kLeft ? -1.0f : 1.0f;
        const float phase = crawlPhase + (side == kLeft ? 0.0f : glm::pi<float>());

        const float reach = std::cos(phase) * m_config.crawlReach * m_gaitWeight;
        const float lift = std::max(0.0f, std::sin(phase)) * m_config.crawlLift * m_gaitWeight;

        glm::vec3 target = shoulder + facing * (m_config.crawlHandForward + reach) +
                           right * (sideSign * 0.16f * m_rig.height);
        target.y = view.renderPosition.y + 0.05f + lift;

        // Plant the hand on whatever is actually underneath it.
        const RayHit hit = physics.RayCast(target + glm::vec3(0.0f, 0.5f, 0.0f),
                                           glm::vec3(0.0f, -1.0f, 0.0f), 1.2f);
        if (hit)
        {
            target.y = hit.position.y + 0.05f + lift;
        }

        FootState& hand = m_hands[static_cast<size_t>(side)];
        // Blend in from the upright pose so going prone does not snap the arms into place.
        const glm::vec3 restHand = m_pose.GlobalPosition(m_rig.hand[side]);
        const glm::vec3 blended = glm::mix(restHand, target, m_flatness);
        hand.position = SmoothTowards(hand.position, blended, m_config.footPlantSmoothing, dt);
        hand.planted = lift < 0.01f;

        // Elbows bend backwards and outwards, away from the body's front.
        const glm::vec3 elbowPole =
            -facing * 0.6f + right * (sideSign * 0.8f) + glm::vec3(0.0f, 0.4f, 0.0f);
        const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                                  m_rig.upperArmLength, m_rig.lowerArmLength);

        m_pose.SetGlobal(m_skeleton, m_rig.upperArm[side], SegmentMatrix(shoulder, ik.jointPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[side],
                         SegmentMatrix(ik.jointPosition, ik.endPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.hand[side],
                         glm::translate(glm::mat4(1.0f), ik.endPosition) * glm::mat4_cast(BodyRotation()));
    }
}

void PlayerBody::UpdateLegs(const PlayerState& state, const PlayerView& view,
                            const PlayerConfig& playerConfig, PhysicsWorld& physics, float dt)
{
    const glm::vec3 flatVelocity{state.velocity.x, 0.0f, state.velocity.z};
    const float speed = glm::length(flatVelocity);
    const bool travelling = speed > 0.05f;
    const glm::vec3 travel = travelling ? flatVelocity / speed : glm::vec3(0.0f);

    const glm::vec3 facing{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
    const glm::vec3 right{std::cos(m_bodyYaw), 0.0f, std::sin(m_bodyYaw)};
    const glm::quat bodyRotation = BodyRotation();
    const float legSpan = m_rig.upperLegLength + m_rig.lowerLegLength;

    const glm::vec3 pelvisForward =
        glm::normalize(glm::vec3(m_pose.Global(m_rig.pelvis) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

    // A foot on the ground stays where it is, in world space, while the body walks past it. That is
    // the whole trick: swinging the foot around the hip on a sine wave, as this used to, slides it
    // backwards under the character at several times walking pace, which is what made every
    // direction of travel look like skating. Strafing was worst, because the hips can only turn so
    // far towards the way you are going, so the sliding ran diagonally across the stride.
    const float stanceShare = std::clamp(playerConfig.StanceFraction(speed), 0.2f, 0.9f);
    const float stride = playerConfig.StrideLength(speed);
    // Below a slow walk there is no cycle worth playing, so the feet simply stand under the hips.
    const float gait = std::clamp((m_gaitWeight - 0.10f) / 0.30f, 0.0f, 1.0f);
    const bool walking = gait > 0.0f && travelling;

    // Crawling has its own cycle: a knee draws up and out to the side, then extends to push. It
    // runs opposite the hands, so the body reads as pulling with one arm and pushing with the
    // opposite leg.
    const float crawlPhase =
        state.strideDistance / std::max(m_config.crawlCycleLength, 0.05f) * glm::two_pi<float>();

    for (int side = 0; side < 2; ++side)
    {
        const glm::vec3 hip = m_pose.GlobalPosition(m_rig.upperLeg[side]);
        const float sideSign = side == kLeft ? -1.0f : 1.0f;
        const glm::vec3 spread =
            right * (sideSign * (m_pose_blend.footSpread - 1.0f) * Ratio::kHipHalfWidth * m_rig.height);
        // Where this foot would stand if the player were not going anywhere.
        const glm::vec3 rest = hip - facing * (m_pose_blend.footBackRatio * legSpan) + spread;

        FootState& foot = m_feet[static_cast<size_t>(side)];
        const float cycle = glm::fract(m_stridePhase + (side == kLeft ? 0.0f : 0.5f));
        const bool inSwing = walking && cycle >= stanceShare;

        glm::vec3 target = rest;
        float lift = 0.0f;
        if (walking)
        {
            // Half a stance ahead of the hip, so the foot lands as far in front as it will end up
            // behind. The step is then symmetric about the moment the hip passes over it, which is
            // also the moment the leg has the least reaching to do.
            const float lead = stride * stanceShare * 0.5f;

            if (inSwing && !foot.inSwing)
            {
                foot.swingFrom = foot.plant;
            }
            else if (!inSwing && foot.inSwing)
            {
                // Planted where the step was aimed, not where the foot had got to. Using the
                // current position instead landed it short, because the smoothing lags a target
                // moving at twice walking pace, and the foot then spent the whole stance trailing
                // further behind the hip than the leg could reach.
                foot.plant = rest + travel * lead;
            }

            if (inSwing)
            {
                const float t = (cycle - stanceShare) / (1.0f - stanceShare);
                // The landing spot is worked out afresh every frame rather than fixed at lift-off,
                // so changing direction mid-step puts the foot down where you are going now.
                const float remaining = stride * (1.0f - stanceShare) * (1.0f - t);
                const glm::vec3 landing = rest + travel * (remaining + lead);
                target = glm::mix(foot.swingFrom, landing, glm::smoothstep(0.0f, 1.0f, t));
                lift = std::sin(t * glm::pi<float>()) * m_config.stepHeight;
            }
            else
            {
                target = foot.plant;
            }
            target = glm::mix(rest, target, gait);
        }
        else
        {
            foot.plant = rest;
        }
        foot.inSwing = inSwing;

        // Crawling replaces the step entirely as the body goes flat: both feet stay down and the
        // legs work like a frog kick, alternately drawing up and pushing back.
        if (m_flatness > 0.001f)
        {
            const float drawn =
                (std::cos(crawlPhase + (side == kLeft ? glm::pi<float>() : 0.0f)) * 0.5f + 0.5f) *
                m_gaitWeight;
            const float back = glm::mix(0.96f, 0.56f, drawn) * legSpan;
            const glm::vec3 crawlTarget =
                hip - facing * back +
                right * (sideSign * drawn * m_config.crawlLegDraw * legSpan) + spread;
            target = glm::mix(target, crawlTarget, m_flatness);
            lift *= 1.0f - m_flatness;
        }

        // Keep the foot out of walls. A step aims at a point up to a third of a metre from the hip,
        // which reaches past the capsule the player is actually stopped by, so pressing into a wall
        // put the foot target inside it; the ground trace below then found the top of the wall and
        // the leg climbed it. Sweeping from beside the ankle to the target and stopping short of
        // whatever is in the way keeps the step on the near side of the surface.
        {
            // Cast at knee height, deliberately. Anything the player can step onto is below this,
            // so stair risers and kerbs pass underneath and a step still reaches the tread above;
            // anything at or above it is a wall, and the foot stops short of it.
            const glm::vec3 knee{hip.x, view.renderPosition.y + m_config.maxFootRise, hip.z};
            const glm::vec3 toTarget{target.x - knee.x, 0.0f, target.z - knee.z};
            const float span = glm::length(toTarget);
            if (span > 0.02f)
            {
                const RayHit blocked = physics.RayCast(knee, toTarget / span, span + 0.10f);
                if (blocked && blocked.distance < span)
                {
                    const float allowed = std::max(blocked.distance - 0.10f, 0.0f);
                    target.x = knee.x + toTarget.x / span * allowed;
                    target.z = knee.z + toTarget.z / span * allowed;
                    if (!inSwing)
                    {
                        foot.plant = target;
                    }
                }
            }
        }

        // Trace for the real ground under the foot so it lands on stairs and slopes instead of
        // hovering at the character's own base height.
        const glm::vec3 traceStart{target.x, view.renderPosition.y + 0.9f, target.z};
        const RayHit hit = physics.RayCast(traceStart, glm::vec3(0.0f, -1.0f, 0.0f), 2.0f);
        // Clamped to what the character could actually have stepped onto. Without this a trace that
        // lands on top of something tall puts the foot up there while the player stands beside it.
        const float groundY = hit ? std::min(hit.position.y, view.renderPosition.y + m_config.maxFootRise)
                                  : view.renderPosition.y;
        target.y = groundY + m_rig.ankleHeight + lift;

        // Never ask for a foot the leg cannot reach. With the hips at standing height a leg is
        // almost straight, so there is very little room to reach forward or back; asking for more
        // makes the IK stretch to its limit and the foot gets dragged along the ground for the rest
        // of the stance. Pulling the target in instead turns the overrun into a short slip as the
        // stance ends, which is what happens to a real foot that has run out of leg.
        // The clamp is horizontal only: the foot has to stay on the ground it is standing on, so
        // pulling it in towards the hip in three dimensions would lift it into the air instead.
        const float drop = hip.y - target.y;
        const float maxReach = legSpan * m_config.stepReachMargin;
        const float maxHorizontal = std::sqrt(std::max(maxReach * maxReach - drop * drop, 0.0f));
        const glm::vec2 offset{target.x - hip.x, target.z - hip.z};
        const float horizontal = glm::length(offset);
        if (horizontal > maxHorizontal)
        {
            const float scale = maxHorizontal / std::max(horizontal, 1e-4f);
            target.x = hip.x + offset.x * scale;
            target.z = hip.z + offset.y * scale;
            // The foot has slipped, so this is where it now stands.
            if (!inSwing)
            {
                foot.plant = target;
            }
        }

        // Smoothing hides the discontinuity when the trace steps from one surface to another. A
        // planted foot converges to a fixed point, so this costs it nothing.
        foot.position = SmoothTowards(
            foot.position, target, inSwing ? m_config.footSwingSmoothing : m_config.footPlantSmoothing, dt);
        foot.planted = !inSwing;

        // Knees bend towards the body's front while upright. Once the pelvis is laid flat that axis
        // points at the sky, which folds the legs upwards, so the pole swings out to the side and
        // down: that is the direction a knee actually goes when you draw it up to crawl.
        const glm::vec3 pronePole =
            glm::normalize(right * (sideSign * 0.78f) + glm::vec3(0.0f, -0.62f, 0.0f));
        const glm::vec3 kneePole =
            glm::normalize(glm::mix(pelvisForward, pronePole, m_flatness) + glm::vec3(1e-4f));

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
    // The weapon is three separate entities so a reload can move the magazine on its own and firing
    // can flash the muzzle without either disturbing the other.
    if (m_weaponEntity.IsValid())
    {
        if (Transform* transform = scene.GetTransform(m_weaponEntity))
        {
            *transform = m_weaponTransform;
        }
        if (Transform* transform = scene.GetTransform(m_magazineEntity))
        {
            *transform = m_magazineTransform;
        }
        if (Transform* transform = scene.GetTransform(m_muzzleFlashEntity))
        {
            *transform = m_muzzleFlashTransform;
        }
    }

    const glm::quat bodyRotation = BodyRotation();
    // Torso segments must inherit the chest's twist, not just the hips', or the upper body reads as
    // rigidly welded to the legs.
    const glm::vec3 bodyForward =
        glm::normalize(-glm::vec3(m_pose.Global(m_rig.chest)[2]) + glm::vec3(1e-5f));

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
        // midpoint and aligns its local +Y with the bone. The body's facing resolves the spin about
        // that axis, without which a vertical segment such as the torso would never turn with the
        // character.
        transform->position = (a + b) * 0.5f;
        transform->rotation = AlignYWithRoll(delta, bodyForward);
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
    UpdateArms(state, view, physics, dt);
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
