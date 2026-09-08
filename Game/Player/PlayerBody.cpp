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

// ACRD field kit: near-black tactical clothing over dark load-bearing gear, with only the visor
// catching any light. Values are deliberately low; against the bright test map the operator should
// read as a silhouette.
const Material kSuitMaterial = Material::Diffuse({0.055f, 0.056f, 0.062f}, 0.93f);
const Material kGearMaterial = Material::Diffuse({0.032f, 0.033f, 0.038f}, 0.86f);
const Material kPadMaterial = Material::Diffuse({0.046f, 0.047f, 0.051f}, 0.80f);
const Material kGloveMaterial = Material::Diffuse({0.028f, 0.028f, 0.032f}, 0.72f);
const Material kHelmetMaterial = Material::Diffuse({0.041f, 0.042f, 0.047f}, 0.55f);
const Material kVisorMaterial = Material::Metal({0.10f, 0.11f, 0.13f}, 0.20f);

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

    // --- Torso. A real torso is much wider than it is deep; modelling it as a square column pushes
    // the chest far enough forward to hide the legs from the wearer's own eyes.
    limb("hips", m_rig.pelvis, m_rig.spine, 0.150f * h, 0.108f * h, kSuitMaterial);
    limb("abdomen", m_rig.spine, m_rig.chest, 0.158f * h, 0.110f * h, kSuitMaterial);
    limb("upper_chest", m_rig.chest, m_rig.neck, 0.088f * h, 0.086f * h, kSuitMaterial);
    limb("neck", m_rig.neck, m_rig.head, 0.052f * h, 0.052f * h, kSuitMaterial, true);

    gear("plate_carrier", m_rig.chest, {0.186f * h, 0.200f * h, 0.140f * h}, {0.0f, -0.075f * h, 0.0f},
         kGearMaterial);
    gear("backpack", m_rig.chest, {0.150f * h, 0.190f * h, 0.080f * h}, {0.0f, -0.070f * h, 0.104f * h},
         kGearMaterial);
    gear("belt", m_rig.pelvis, {0.162f * h, 0.038f * h, 0.118f * h}, {0.0f, 0.012f * h, 0.0f},
         kGearMaterial);

    // Chest rig pouches, mirrored either side of the sternum.
    for (int side = 0; side < 2; ++side)
    {
        const float sign = side == kLeft ? -1.0f : 1.0f;
        gear(side == kLeft ? "pouch_left" : "pouch_right", m_rig.chest,
             {0.056f * h, 0.062f * h, 0.042f * h},
             {sign * 0.048f * h, -0.100f * h, -0.086f * h}, kGearMaterial);
    }

    // --- Head. All of it is suppressed in first person, because the camera sits inside it.
    gear("helmet", m_rig.head, {0.118f * h, 0.104f * h, 0.128f * h}, {0.0f, 0.020f * h, 0.006f * h},
         kHelmetMaterial, PartFrame::BoneFrame, true);
    gear("visor", m_rig.head, {0.100f * h, 0.042f * h, 0.030f * h}, {0.0f, 0.008f * h, -0.062f * h},
         kVisorMaterial, PartFrame::BoneFrame, true);
    gear("respirator", m_rig.head, {0.074f * h, 0.052f * h, 0.044f * h},
         {0.0f, -0.034f * h, -0.056f * h}, kGearMaterial, PartFrame::BoneFrame, true);

    // --- Arms.
    for (int side = 0; side < 2; ++side)
    {
        const float sign = side == kLeft ? -1.0f : 1.0f;
        const char* pad = side == kLeft ? "shoulder_pad_left" : "shoulder_pad_right";
        const char* upper = side == kLeft ? "upper_arm_left" : "upper_arm_right";
        const char* elbow = side == kLeft ? "elbow_pad_left" : "elbow_pad_right";
        const char* fore = side == kLeft ? "forearm_left" : "forearm_right";
        const char* glove = side == kLeft ? "glove_left" : "glove_right";

        gear(pad, m_rig.shoulder[side], {0.080f * h, 0.064f * h, 0.078f * h},
             {sign * 0.006f * h, -0.008f * h, 0.0f}, kGearMaterial);
        limb(upper, m_rig.shoulder[side], m_rig.lowerArm[side], 0.058f * h, 0.058f * h, kSuitMaterial);
        gear(elbow, m_rig.lowerArm[side], {0.056f * h, 0.052f * h, 0.056f * h}, glm::vec3(0.0f),
             kPadMaterial);
        limb(fore, m_rig.lowerArm[side], m_rig.hand[side], 0.050f * h, 0.050f * h, kSuitMaterial);
        gear(glove, m_rig.hand[side], {0.050f * h, 0.086f * h, 0.046f * h}, {0.0f, -0.030f * h, 0.0f},
             kGloveMaterial);
    }

    // --- Legs.
    for (int side = 0; side < 2; ++side)
    {
        const char* thigh = side == kLeft ? "thigh_left" : "thigh_right";
        const char* knee = side == kLeft ? "knee_pad_left" : "knee_pad_right";
        const char* shin = side == kLeft ? "shin_left" : "shin_right";
        const char* boot = side == kLeft ? "boot_left" : "boot_right";

        limb(thigh, m_rig.upperLeg[side], m_rig.lowerLeg[side], 0.084f * h, 0.084f * h, kSuitMaterial);
        gear(knee, m_rig.lowerLeg[side], {0.078f * h, 0.072f * h, 0.048f * h},
             {0.0f, 0.010f * h, -0.032f * h}, kPadMaterial);
        limb(shin, m_rig.lowerLeg[side], m_rig.foot[side], 0.068f * h, 0.068f * h, kSuitMaterial);
        gear(boot, m_rig.foot[side], {0.074f * h, m_rig.ankleHeight * 1.35f, 0.160f * h},
             {0.0f, 0.004f * h, -0.024f * h}, kGloveMaterial);
    }

    // Sidearm on the right thigh, as in the reference kit.
    gear("holster", m_rig.upperLeg[kRight], {0.048f * h, 0.115f * h, 0.055f * h},
         {0.052f * h, -0.115f * h, 0.010f * h}, kGearMaterial);

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
    // The body stands under the camera. Placing the root from the simulation rather than from the
    // smoothed view would make the body slide against the camera on stairs.
    const float stanceHeight = playerConfig.HeightForStance(state.stance);
    const float stanceScale = stanceHeight / std::max(playerConfig.standHeight, 0.01f);

    // Yaw follows the camera. Turn-in-place and a decoupled upper body come with the animation
    // milestone; for now the whole body faces where the player looks.
    m_bodyYaw = view.yaw;

    // Push the body back along the facing so the camera sits at the eyes rather than on the body's
    // own centre line.
    const glm::vec3 facing{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
    m_rootPosition = state.position +
                     glm::vec3(0.0f, Ratio::kPelvisHeight * m_rig.height * stanceScale, 0.0f) -
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
    pelvis.rotation = glm::angleAxis(glm::radians(sway * 60.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    // Lean forward from the spine, and counter-rotate the chest slightly so the torso does not fold.
    m_pose.Local(m_rig.spine).rotation =
        glm::angleAxis(glm::radians(m_lean * 0.6f), glm::vec3(1.0f, 0.0f, 0.0f));
    m_pose.Local(m_rig.chest).rotation =
        glm::angleAxis(glm::radians(-m_lean * 0.2f), glm::vec3(1.0f, 0.0f, 0.0f));

    // The head carries the camera's pitch, minus what the spine already contributed.
    m_pose.Local(m_rig.neck).rotation =
        glm::angleAxis(view.pitch * 0.35f - glm::radians(m_lean * 0.2f), glm::vec3(1.0f, 0.0f, 0.0f));
    m_pose.Local(m_rig.head).rotation = glm::angleAxis(view.pitch * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));

    // Arms swing opposite the legs.
    for (int side = 0; side < 2; ++side)
    {
        const float sideSign = side == kLeft ? 1.0f : -1.0f;
        const float swing = std::sin(phase + (side == kLeft ? 0.0f : glm::pi<float>())) *
                            glm::radians(m_config.armSwingDegrees) * m_gaitWeight;
        const float rest = glm::radians(m_config.armRestDegrees);

        m_pose.Local(m_rig.upperArm[side]).rotation =
            glm::angleAxis(swing, glm::vec3(1.0f, 0.0f, 0.0f)) *
            glm::angleAxis(sideSign * rest, glm::vec3(0.0f, 0.0f, 1.0f));
        // A permanently straight elbow reads as a mannequin, so keep a little bend at all times.
        m_pose.Local(m_rig.lowerArm[side]).rotation =
            glm::angleAxis(glm::radians(12.0f) + std::abs(swing) * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
    }

    const glm::mat4 root = glm::translate(glm::mat4(1.0f), m_rootPosition) *
                           glm::mat4_cast(glm::angleAxis(m_bodyYaw, glm::vec3(0.0f, 1.0f, 0.0f)));
    m_pose.ComputeGlobals(m_skeleton, root);
}

void PlayerBody::UpdateLegs(const PlayerState& state, const PlayerConfig& playerConfig,
                            PhysicsWorld& physics, float dt)
{
    const glm::vec3 flatVelocity{state.velocity.x, 0.0f, state.velocity.z};
    const float speed = glm::length(flatVelocity);
    const glm::vec3 moveDirection = speed > 0.05f ? flatVelocity / speed : glm::vec3(0.0f);

    const float phase = m_stridePhase * glm::two_pi<float>();
    const float stanceScale =
        playerConfig.HeightForStance(state.stance) / std::max(playerConfig.standHeight, 0.01f);

    for (int side = 0; side < 2; ++side)
    {
        const glm::vec3 hip = m_pose.GlobalPosition(m_rig.upperLeg[side]);
        const float footPhase = phase + (side == kLeft ? 0.0f : glm::pi<float>());

        // Swing the foot forward and back along the direction of travel, lifting it on the forward
        // half of the cycle.
        const float reach = std::cos(footPhase) * m_config.strideLength * 0.5f * m_gaitWeight;
        const float lift = std::max(0.0f, std::sin(footPhase)) * m_config.stepHeight * m_gaitWeight;

        glm::vec3 target = hip + moveDirection * reach;
        target.y = state.position.y + m_rig.ankleHeight + lift;

        // Trace for the real ground under the foot so it lands on stairs and slopes instead of
        // hovering at the character's own base height.
        const glm::vec3 traceStart = target + glm::vec3(0.0f, 0.6f, 0.0f);
        const RayHit hit = physics.RayCast(traceStart, glm::vec3(0.0f, -1.0f, 0.0f), 1.4f);
        if (hit)
        {
            target.y = std::max(hit.position.y + m_rig.ankleHeight, hit.position.y) + lift;
        }

        FootState& foot = m_feet[static_cast<size_t>(side)];
        // Smoothing hides the discontinuity when the trace steps from one surface to another.
        foot.position = SmoothTowards(foot.position, target, m_config.footPlantSmoothing, dt);
        foot.planted = lift < 0.01f;

        // Knees bend forwards, so the pole points along the body's facing.
        const glm::vec3 forward{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
        const TwoBoneIKResult ik =
            SolveTwoBoneIK(hip, foot.position, forward, m_rig.upperLegLength * stanceScale,
                           m_rig.lowerLegLength * stanceScale);

        // Write the solved chain straight into the pose's globals. Parent before child, because
        // setting a bone rebuilds everything after it from local transforms.
        m_pose.SetGlobal(m_skeleton, m_rig.upperLeg[side], SegmentMatrix(hip, ik.jointPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerLeg[side],
                         SegmentMatrix(ik.jointPosition, ik.endPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.foot[side],
                         glm::translate(glm::mat4(1.0f), ik.endPosition) *
                             glm::mat4_cast(glm::angleAxis(m_bodyYaw, glm::vec3(0.0f, 1.0f, 0.0f))));
    }
}

void PlayerBody::PushToScene(Scene& scene)
{
    const glm::quat bodyRotation = glm::angleAxis(m_bodyYaw, glm::vec3(0.0f, 1.0f, 0.0f));

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
    UpdateLegs(state, playerConfig, physics, dt);
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
