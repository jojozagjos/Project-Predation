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
// Heights up the torso, each measured from the joint below it, as fractions of standing height.
// They add up to put the chin at 0.873 and the shoulder joint at 0.818, which is where a real
// shoulder is. The chest joint used to sit at 0.702, which put the shoulders fifteen centimetres
// too low: the arms grew out of the middle of the ribcage, the hands hung past the knees, and the
// gap left above the chest needed a separate block to plug it.
constexpr float kSpine = 0.090f; // pelvis to the waist
constexpr float kChest = 0.170f; // waist to the top of the ribcage, where the shoulders hang
constexpr float kNeck = 0.035f;  // ribcage to the base of the neck
constexpr float kHead = 0.048f;  // neck to the jaw
constexpr float kShoulderHalfWidth = 0.115f;
constexpr float kShoulderRise = 0.028f;
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
    // The shoulder yoke: broad and shallow, spanning between the shoulder joints and closing the
    // top of the torso. With the chest joint at its proper height this is the whole top of the
    // body, so there is nothing left to cap and no separate collar piece between it and the neck.
    limb("chest", m_rig.chest, m_rig.neck, 0.215f * h, 0.120f * h, kGearMaterial);
    // Hidden in first person, because nobody can see their own neck.
    limb("neck", m_rig.neck, m_rig.head, 0.050f * h, 0.050f * h, kSuitMaterial, true);

    // A human head is about 0.13 of standing height tall and noticeably narrower than it is tall.
    // Sized from the crown down, so the top of the head lands at full standing height.
    gear("head", m_rig.head, {0.098f * h, 0.132f * h, 0.118f * h},
         {0.0f, 0.063f * h, 0.004f * h + m_config.skullBehindEye},
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

    // Prone is a different animal, so it is settled first and the upright cases below are skipped.
    //
    // Lying down, the body has a heading of its own. It turns towards where you are crawling, and
    // looking around does not turn it: a person on their belly pivots slowly and deliberately, and
    // that is most of what makes prone feel like prone rather than like standing up sideways.
    //
    // Turn far enough that the body cannot follow and you roll onto your back instead, coming to
    // rest facing the new direction. That is what a person does rather than twisting their neck
    // past what a neck does, and it is what lets you cover behind you without getting up.
    const bool prone = m_flatness > 0.5f;
    if (prone)
    {
        // The heading only changes when you crawl. Rolling over used to turn the body end for end
        // as well, which snapped it round behind the player in a single frame; worse, a half turn
        // in yaw on top of a half roll about the body's own length leaves you face down again
        // pointing the other way, which is why lying on your back never actually happened.
        //
        // Nothing about the heading changes when you roll now. Looking behind puts you on your
        // back, looking forward again puts you back on your front, and the two thresholds are
        // apart so it settles instead of chattering at the boundary.
        const float signedTwist = WrapAngle(view.yaw - m_bodyYaw);
        const float twist = std::abs(signedTwist);

        // How far over you are follows how far round you are looking, rather than flipping between
        // lying on your front and lying on your back. Past the point where the neck runs out you
        // start coming up onto your side, and you are only fully supine looking straight behind.
        //
        // The difference matters because halfway is a real position and not just a frame of a
        // transition: up on one shoulder is where you can cover behind you without giving up the
        // ground, and two fixed states cannot express it. It also means the roll cannot snap,
        // because there is no state to snap between.
        // Turned further than a neck allows, a prone body shuffles round on its front. It is slow,
        // because pivoting on your elbows is, and it is the only answer now that rolling onto your
        // back is gone: without it the body is left wrenched at its twist limit.
        //
        // Which way it goes is decided once and held until the body has caught up. Deciding it
        // every tick makes the body jitter at exactly half a turn, where which way is shorter
        // flips back and forth between one frame and the next.
        if (twist > glm::radians(m_config.pronePivotDegrees))
        {
            if (!m_pronePivoting)
            {
                m_pronePivoting = true;
                m_pronePivotSign = signedTwist > 0.0f ? 1.0f : -1.0f;
            }
            const float excess = twist - glm::radians(m_config.pronePivotDegrees);
            m_bodyYaw = WrapAngle(m_bodyYaw + m_pronePivotSign *
                                                  std::min(excess, m_config.pronePivotSpeed * dt));
        }
        else
        {
            m_pronePivoting = false;
        }

        if (moving)
        {
            // The heading follows the *line* of travel, not its direction, taking whichever end is
            // nearer to the way the body already lies. Crawling backwards then keeps you facing
            // where you were and pushes you back on your elbows, instead of pivoting the whole
            // body round to face its own heels. That pivot is what made backing up look wrong, and
            // it also read as being spun round every time, because turning to look behind you and
            // then backing away always swung the body the same way.
            float target = std::atan2(flat.x, -flat.z);
            if (std::abs(WrapAngle(target - m_bodyYaw)) > glm::radians(115.0f))
            {
                target = WrapAngle(target + glm::pi<float>());
            }
            m_bodyYaw = WrapAngle(m_bodyYaw + WrapAngle(target - m_bodyYaw) *
                                                  (1.0f - std::exp(-m_config.proneTurnSpeed * dt)));
        }
    }
    else
    {
        m_pronePivoting = false;
    }

    float hipTarget = m_bodyYaw;
    float turnSpeed = m_config.hipTurnSpeedIdle;
    if (prone)
    {
        // Already settled above.
        turnSpeed = 0.0f;
    }
    else if (moving)
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
    //
    // Flat on the ground there is far less of it available: a chest pressed into the floor cannot
    // wind round the way a standing one can, and on your back there is none at all, because you are
    // looking back over your own body rather than turning towards anything. Letting the full
    // standing twist through while prone wound the chest most of the way round and left the body
    // lying on its side instead of its back.
    const float twistLimit = glm::radians(m_config.maxTorsoTwistDegrees) *
                             glm::mix(1.4f, 0.55f, m_flatness);
    const float torsoTwist = std::clamp(WrapAngle(view.yaw - m_bodyYaw), -twistLimit, twistLimit);

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

    // Airborne. Whether the player is going up or down changes the whole shape of the pose: legs
    // come up on the way up and reach down on the way down, and the arms come out either way. Held
    // as two smoothed numbers rather than read straight from the state so a hop over a kerb does
    // not snap the body into a jump pose and out again.
    m_airborne = SmoothTowards(m_airborne, state.grounded ? 0.0f : 1.0f, m_config.airBlendSpeed, dt);
    m_airRise = SmoothTowards(m_airRise, std::clamp(state.velocity.y / m_config.airRiseReference, -1.0f, 1.0f),
                              m_config.airBlendSpeed, dt);

    // Running tips the torso forward. In the air it tips back on the way up and forward on the way
    // down, which is what a body does when its legs are no longer under it.
    const float targetLean =
        std::clamp(speed * m_config.leanPerSpeed, 0.0f, m_config.maxLean) * (state.grounded ? 1.0f : 0.3f) -
        m_config.airLeanDegrees * m_airborne * m_airRise;
    m_lean = SmoothTowards(m_lean, targetLean, m_config.responsiveness * 0.5f, dt);

    // The walk cycle comes from the simulation, not from a second clock kept here. The camera dip
    // runs off the same value, so the head drops exactly as a foot lands.
    m_stridePhase = state.stridePhase;

    // The crawl cycle is measured along the body rather than as a distance travelled, so it runs
    // backwards when you back up. The simulation's stride distance only ever grows, which ran the
    // reach-and-pull the same way whichever way you were going: backing up looked like crawling
    // forwards while sliding the wrong way. This is presentation only, and nothing reads it but the
    // arms and legs below.
    {
        const glm::vec3 facing{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
        m_crawlDistance += glm::dot(flat, facing) * dt;
    }

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
    //
    // The roll is applied before the pitch, about the spine. Standing that would be a turn on the
    // spot; laid down it becomes a roll about the body's own length, which is the difference
    // between lying on your front and lying on your back. Between the two it reads as rolling over,
    // which is exactly the movement being animated.
    pelvis.rotation = glm::angleAxis(-pelvisPitch, glm::vec3(1.0f, 0.0f, 0.0f)) *

                      glm::angleAxis(glm::radians(sway * 60.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    // Lean forward from the spine, counter-rotate the chest slightly so the torso does not fold, and
    // spread the twist between the two so it does not all happen at one joint.
    // The twist is negated for the same reason the root rotation is: a model facing -Z turns the
    // opposite way to the yaw convention.
    const float runLean = glm::radians(m_lean);
    // Peek lean: the torso tips sideways so the body follows the camera out past cover.
    const float peek = state.leanAmount * glm::radians(m_config.leanAngleDegrees);

    // Crawling, the shoulders roll from side to side as each arm reaches and pulls. Without it the
    // torso slides along the floor as one rigid plank while the limbs work, which is what made the
    // crawl read as a body being dragged rather than one pulling itself.
    const float crawlPhase =
        m_crawlDistance / std::max(m_config.crawlCycleLength, 0.05f) * glm::two_pi<float>();
    const float crawlRoll = std::sin(crawlPhase) * glm::radians(m_config.crawlShoulderRollDegrees) *
                            m_gaitWeight * m_flatness;

    m_pose.Local(m_rig.spine).rotation =
        glm::angleAxis(-torsoTwist * 0.45f, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(-(spineLean * 0.65f + runLean * 0.6f), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::angleAxis(-peek * 0.55f + crawlRoll * 0.4f, glm::vec3(0.0f, 0.0f, 1.0f));
    m_pose.Local(m_rig.chest).rotation =
        glm::angleAxis(-torsoTwist * 0.55f, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(-(spineLean * 0.35f + runLean * 0.2f), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::angleAxis(-peek * 0.45f + crawlRoll * 0.6f, glm::vec3(0.0f, 0.0f, 1.0f));

    // The neck and head undo whatever the pelvis and spine did, so the head stays level and keeps
    // looking where the player is aiming. Positive here, because it is cancelling a forward pitch.
    // Without it, laying the pelvis flat for prone drives the head face-down into the floor.
    //
    // Rolled onto the back, the body is turned end over end about its own length, so a rotation
    // that used to lift the chin now drops it. The compensation flips sign with the roll; without
    // that, rolling over drives the face into the floor it just came off.
    const float torsoPitch = pelvisPitch + spineLean + runLean * 0.8f;
    constexpr float pitchSign = 1.0f;
    m_pose.Local(m_rig.neck).rotation = glm::angleAxis(
        pitchSign * torsoPitch * 0.55f + view.pitch * 0.35f, glm::vec3(1.0f, 0.0f, 0.0f));
    m_pose.Local(m_rig.head).rotation =
        glm::angleAxis(pitchSign * torsoPitch * 0.45f + view.pitch * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));

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
        // In the air the arms come out from the sides for balance, and lift as the jump rises.
        const float airSpread = glm::radians(m_config.airArmDegrees) * m_airborne;
        const float airLift = glm::radians(m_config.airArmDegrees * 0.45f) * m_airborne * m_airRise;

        m_pose.Local(m_rig.upperArm[side]).rotation =
            glm::angleAxis(swing + glm::radians(m_pose_blend.armForwardDeg) - airLift,
                           glm::vec3(1.0f, 0.0f, 0.0f)) *
            glm::angleAxis(sideSign * (rest + airSpread), glm::vec3(0.0f, 0.0f, 1.0f));
        // A permanently straight elbow reads as a mannequin, so keep a little bend at all times.
        m_pose.Local(m_rig.lowerArm[side]).rotation =
            glm::angleAxis(glm::radians(12.0f) + std::abs(swing) * 0.5f + airSpread * 0.45f,
                           glm::vec3(1.0f, 0.0f, 0.0f));
    }

    glm::mat4 root = glm::translate(glm::mat4(1.0f), m_rootPosition) * glm::mat4_cast(BodyRotation());
    m_pose.ComputeGlobals(m_skeleton, root);

    // Anchor the head to the camera, rather than placing the body by a guessed offset and hoping the
    // head lands near the eye. Solving it exactly fixes several problems at once: the head no longer
    // drifts off-centre when the hips turn away from the view while strafing, the torso stops
    // sitting too far forward, and the camera stops clipping through the body on stairs and while
    // crouching, because the head now carries the same step smoothing and landing dip the camera
    // does. It costs one extra pass over nineteen bones.
    // The eye sits forward of the body, not on top of it. Your chest is behind your face, which is
    // why looking down shows you the front of your torso and your boots rather than the tops of
    // your own shoulders.
    //
    // Measured along the *body's* facing rather than the view's. Using the view swings the whole
    // body sideways whenever you turn your head, which is the drift this anchoring was written to
    // remove in the first place; using the body only moves it when the body itself turns.
    const glm::vec3 bodyFacing{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
    const glm::vec3 desiredHead = view.eyePosition - bodyFacing * m_config.eyeForwardOfHead -
                                  glm::vec3(0.0f, m_config.eyeAboveHead, 0.0f);
    m_rootPosition += desiredHead - m_pose.GlobalPosition(m_rig.head);

    root = glm::translate(glm::mat4(1.0f), m_rootPosition) * glm::mat4_cast(BodyRotation());
    m_pose.ComputeGlobals(m_skeleton, root);
}

glm::vec3 PlayerBody::DebugSkullCentre() const
{
    // The same arithmetic PushToScene does for a piece of equipment in its bone frame.
    const glm::mat4 head = m_pose.Global(m_rig.head);
    const glm::quat frame = glm::normalize(glm::quat_cast(glm::mat3(head)));
    const float h = m_rig.height;
    return glm::vec3(head[3]) +
           frame * glm::vec3(0.0f, 0.063f * h, 0.004f * h + m_config.skullBehindEye);
}

glm::quat PlayerBody::BodyRotation() const
{
    // Yaw zero looks down -Z, and yaw increases turning right. Rotating a model's local -Z by +yaw
    // about +Y sends it the other way, so the sign is negated here. Getting this wrong mirrors the
    // body: it turns left when the camera turns right.
    return glm::angleAxis(-m_bodyYaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

void PlayerBody::DestroyWeapon(Scene& scene)
{
    for (Entity entity : m_weaponParts)
    {
        scene.Destroy(entity);
    }
    m_weaponParts.clear();
    m_weaponPartTransforms.clear();
    scene.Destroy(m_muzzleFlashEntity);
    m_weaponEntity = Entity{};
    m_muzzleFlashEntity = Entity{};
    m_weaponId = kInvalidWeapon;
    m_hasWeapon = false;
}

void PlayerBody::SetWeaponForSimulation(const WeaponDefinition* definition)
{
    if (definition == nullptr || definition->id == kInvalidWeapon)
    {
        m_weaponVisual = WeaponVisual{};
        m_weaponId = kInvalidWeapon;
        m_hasWeapon = false;
        return;
    }
    if (definition->id == m_weaponId && m_hasWeapon)
    {
        return;
    }
    m_weaponId = definition->id;
    m_weaponVisual = BuildWeaponVisual(*definition);
    m_hasWeapon = true;
}

void PlayerBody::SetWeapon(Scene& scene, MeshLibrary& meshes, const WeaponDefinition* definition)
{
    if (definition == nullptr || definition->id == kInvalidWeapon)
    {
        if (m_weaponEntity.IsValid())
        {
            DestroyWeapon(scene);
        }
        return;
    }
    if (definition->id == m_weaponId && m_weaponEntity.IsValid())
    {
        return;
    }
    DestroyWeapon(scene);

    m_weaponId = definition->id;
    m_weaponVisual = BuildWeaponVisual(*definition);

    // One entity per part, whether the model came from the editor or was built from the weapon's
    // numbers. That is what lets a reload take the magazine out and an authored clip move anything.
    const std::string key = "weapon_" + definition->key;
    for (const WeaponVisual::Part& part : m_weaponVisual.parts)
    {
        const MeshHandle mesh = meshes.Upload(part.mesh, key + "_" + part.name);
        m_weaponParts.push_back(scene.CreateMeshEntity(key + "_" + part.name, Transform{}, mesh,
                                                       part.material));
        m_weaponPartTransforms.emplace_back();
    }
    m_weaponEntity = m_weaponParts.empty() ? Entity{} : m_weaponParts.front();
    m_hasWeapon = true;

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
    if (!m_hasWeapon)
    {
        return false;
    }

    const float aim = glm::clamp(m_weaponPose.aim, 0.0f, 1.0f);
    const bool flat = m_flatness > 0.5f;

    // --- Sway -----------------------------------------------------------------------------------
    // Three things move a held weapon, and they are separate on purpose.
    //
    // Turning lags it and it catches up. That is driven by how fast the view is turning rather than
    // by how far it turned this frame: accumulating per-frame deltas and decaying them per frame
    // made the amount of sway depend on the frame rate, which is why it felt different on every
    // machine. Angular velocity is the same number at any frame rate.
    if (dt > 1e-5f)
    {
        m_viewRate = glm::vec2(WrapAngle(view.yaw - m_lastViewYaw) / dt, (view.pitch - m_lastViewPitch) / dt);
    }
    m_lastViewYaw = view.yaw;
    m_lastViewPitch = view.pitch;

    const float swayScale = (1.0f - aim * 0.75f) * m_config.weaponSwayAmount;
    const glm::vec2 lagTarget = glm::clamp(-m_viewRate * swayScale * 0.12f, glm::vec2(-0.10f),
                                           glm::vec2(0.10f));

    // Breathing. A weapon in someone's hands is never still, and this is most of what stops a held
    // gun looking welded to the camera. It all but stops when the sights are up, because that is
    // what holding your breath is for.
    m_swayClock += dt;
    const float breathe = (1.0f - aim * 0.85f) * m_config.weaponBreatheAmount;
    const glm::vec2 idle{std::sin(m_swayClock * 0.9f) * breathe,
                         std::sin(m_swayClock * 1.7f + 1.1f) * breathe * 0.6f};

    // Walking swings it with the stride, on the same phase the legs use, so the weapon moves with
    // the steps rather than on a rhythm of its own.
    const float walkPhase = m_stridePhase * glm::two_pi<float>();
    const float walk = m_gaitWeight * (1.0f - aim * 0.6f) * m_config.weaponWalkAmount;
    const glm::vec2 stride{std::sin(walkPhase) * walk, -std::abs(std::cos(walkPhase)) * walk * 0.8f};

    m_weaponSway = glm::vec2(SmoothTowards(m_weaponSway.x, lagTarget.x, m_config.weaponSwayRecover, dt),
                             SmoothTowards(m_weaponSway.y, lagTarget.y, m_config.weaponSwayRecover, dt));
    const glm::vec2 sway = m_weaponSway + idle + stride;

    // --- The frame the weapon is carried in -----------------------------------------------------
    // Aiming follows the view exactly, because the sights have to line up with it. Carrying it does
    // not: a weapon held ready stays in front of the chest, so it follows yaw fully but pitch only
    // part way. Following pitch fully meant looking at your feet swung the gun round behind you.
    // Right comes from the yaw alone. Crossing forward with world up looks equivalent and is not:
    // at ninety degrees of pitch, forward *is* world up and the cross product collapses to nothing,
    // so the whole frame flips and the weapon ends up somewhere behind the player. A yaw-only right
    // is well defined at every pitch, and up follows from it.
    const glm::vec3 yawRight{std::cos(view.yaw), 0.0f, std::sin(view.yaw)};

    const float carryPitch = view.pitch * glm::mix(m_config.weaponCarryPitchFollow, 1.0f, aim);
    const float cp = std::cos(carryPitch);
    const glm::vec3 carryForward{std::sin(view.yaw) * cp, std::sin(carryPitch), -std::cos(view.yaw) * cp};
    const glm::vec3 carryRight = yawRight;
    const glm::vec3 carryUp = glm::cross(carryRight, carryForward);

    // The aim frame, which the barrel is pointed down. Leaning rolls it, so the sights stay lined up
    // with a view that has itself rolled: without that, peeking round a corner left the sight block
    // off the axis and aiming stopped meaning anything.
    const float ap = std::cos(view.pitch);
    const glm::vec3 aimForward{std::sin(view.yaw) * ap, std::sin(view.pitch), -std::cos(view.yaw) * ap};
    const glm::quat leanRoll = glm::angleAxis(view.leanRoll, aimForward);
    const glm::vec3 aimRight = leanRoll * yawRight;
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
    const glm::vec3 proneOffset = carryRight * 0.11f + carryUp * -0.26f + carryForward * 0.36f;

    glm::vec3 offset = glm::mix(readyOffset, sightedOffset, aim);
    if (flat)
    {
        // Only what is left over after aiming. Blending the prone carry in on top of the sighted
        // offset overrode it completely, which is why the sights did nothing while lying down:
        // prone is where a rifle is steadiest and where aiming matters most.
        const float prone = glm::clamp((m_flatness - 0.5f) * 2.0f, 0.0f, 1.0f) * (1.0f - aim);
        // On the back the weapon comes up over the chest rather than down beside the body: there is
        // no ground on that side to lay it on.
        offset = glm::mix(offset, proneOffset, prone);

    }

    // Looking steeply down, the sighted hold puts the weapon inside the player's own chest and
    // legs, because the offset runs straight down the view. Shortening it as the angle steepens
    // keeps the model out of the body while the sights stay on the axis, which is what matters.
    if (view.pitch < 0.0f)
    {
        const float steep = glm::clamp(-view.pitch / glm::radians(75.0f), 0.0f, 1.0f);
        offset -= aimForward * (m_config.weaponAimForward * 0.55f * steep * aim);
        offset += carryUp * (0.10f * steep * aim);
    }

    // Bringing a weapon up: it starts low and out of the way and rises into the hold. Presentation
    // only, so it never delays a shot.
    //
    // These are the fallbacks. A model that carries its own "equip" or "reload" clip animates
    // through that instead, and the built-in movement steps out of the way rather than fighting it.
    const auto hasClip = [this](const char* clipName)
    { return m_weaponVisual.asset != nullptr && m_weaponVisual.asset->FindClip(clipName) != nullptr; };
    const bool authoredDraw = hasClip("equip");
    const bool authoredReload = hasClip("reload");

    const float draw = glm::clamp(m_weaponPose.draw, 0.0f, 1.0f);
    if (draw < 1.0f && !authoredDraw)
    {
        const float lift = (1.0f - draw) * (1.0f - draw);
        offset += carryUp * (-0.40f * lift) + carryRight * (0.10f * lift) - carryForward * (0.12f * lift);
    }

    // --- Which way it points --------------------------------------------------------------------
    const glm::quat sighted = LookRotation(aimForward, aimUp);
    // Held ready the muzzle drops and the weapon cants inwards, the way a carried rifle does.
    const glm::quat ready = LookRotation(carryForward, carryUp) *
                            glm::angleAxis(glm::radians(-9.0f), glm::vec3(1.0f, 0.0f, 0.0f)) *
                            glm::angleAxis(glm::radians(-4.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::quat rotation = glm::slerp(ready, sighted, aim);

    // Peeking rolls the weapon with the head. The roll is already in the aim frame above, so the
    // sighted offset rolls with it and the sight block stays on the view axis; applying the roll
    // only to the rotation, as this used to, tipped the weapon without moving where it sat, and the
    // sights came off the middle of the screen exactly when the player needed them.
    if (std::abs(view.leanRoll) > 1e-4f && aim < 0.999f)
    {
        rotation = glm::slerp(glm::angleAxis(view.leanRoll, carryForward), glm::quat(1, 0, 0, 0), aim) *
                   rotation;
    }
    if (draw < 1.0f && !authoredDraw)
    {
        const float lift = (1.0f - draw) * (1.0f - draw);
        rotation = rotation * glm::angleAxis(glm::radians(-34.0f * lift), glm::vec3(1.0f, 0.0f, 0.0f)) *
                   glm::angleAxis(glm::radians(18.0f * lift), glm::vec3(0.0f, 0.0f, 1.0f));
    }

    // --- Reload ---------------------------------------------------------------------------------
    // The weapon rolls towards the player so the magazine well is where the hand can reach it, dips
    // as the magazine comes out, and comes back up as the new one seats. `reload` runs 0 to 1.
    glm::vec3 magazineOffset{0.0f};
    float magazineVisible = 1.0f;
    if (m_weaponPose.reloading && !authoredReload)
    {
        const float t = glm::clamp(m_weaponPose.reload, 0.0f, 1.0f);
        // A raised-cosine envelope: nothing at either end, most of the movement in the middle.
        const float envelope = 0.5f - 0.5f * std::cos(t * glm::two_pi<float>());
        rotation = rotation * glm::angleAxis(glm::radians(48.0f * envelope), glm::vec3(0.0f, 0.0f, 1.0f)) *
                   glm::angleAxis(glm::radians(-24.0f * envelope), glm::vec3(1.0f, 0.0f, 0.0f));
        offset += carryUp * (-0.13f * envelope) + carryRight * (-0.07f * envelope) -
                  carryForward * (0.06f * envelope);

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

    // Sway is applied last, entirely in the carry frame, so it moves the whole hold together:
    // weapon, magazine and both hands. Turning about the world up axis instead, as this used to,
    // stopped lining up with the screen as soon as the player looked up or down.
    rotation = glm::angleAxis(sway.x, carryUp) * glm::angleAxis(sway.y, carryRight) * rotation;
    offset += carryRight * (sway.x * 0.16f) + carryUp * (sway.y * 0.16f);

    m_weaponTransform.position = view.eyePosition + offset;
    m_weaponTransform.rotation = rotation;
    m_weaponTransform.scale = glm::vec3(1.0f);

    // The trigger hand never lets go, so the weapon is pulled in until the grip is somewhere the
    // arm can actually reach. Without this the hold asked for a point further away than the arm is
    // long, the IK stretched to its limit, and the weapon appeared to float free of the hand that
    // was supposed to be holding it: looking straight up, or turning while prone, put the grip well
    // out of range. It happens here, before the parts are placed, so the model moves with it.
    {
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[kRight]);
        const float reach = (m_rig.upperArmLength + m_rig.lowerArmLength) * 0.94f;
        const glm::vec3 toGrip = m_weaponTransform.position - shoulder;
        const float distance = glm::length(toGrip);
        if (distance > reach && distance > 1e-4f)
        {
            m_weaponTransform.position = shoulder + toGrip * (reach / distance);
        }
    }

    // Every part rides the weapon's frame. A model authored in the editor can carry its own clip
    // for a reload, in which case that is what moves its parts; otherwise the built-in magazine
    // swap below is what a reload looks like.
    //
    // Which clip plays is decided by what the weapon is doing, and its progress comes from the
    // simulation's own timers, so a clip works at whatever reload or draw time the weapon has.
    const AnimationClip* clip = nullptr;
    float clipTime = 0.0f;
    float clipProgress = 0.0f;
    if (m_weaponVisual.asset != nullptr)
    {
        if (m_weaponPose.reloading)
        {
            clip = m_weaponVisual.asset->FindClip("reload");
            clipProgress = glm::clamp(m_weaponPose.reload, 0.0f, 1.0f);
        }
        else if (draw < 1.0f)
        {
            clip = m_weaponVisual.asset->FindClip("equip");
            clipProgress = draw;
        }
        if (clip != nullptr)
        {
            clipTime = clipProgress * clip->duration;
        }
    }

    // A track named "root" moves the whole weapon rather than one part, in the frame it is carried
    // in. That is what lets an equip, a holster or a fire kick be authored in the editor instead of
    // written in here.
    if (clip != nullptr)
    {
        const auto root = std::find_if(clip->tracks.begin(), clip->tracks.end(),
                                       [](const AnimationTrack& track) { return track.part == "root"; });
        if (root != clip->tracks.end() && !root->keys.empty())
        {
            ModelPart origin;
            origin.name = "root";
            const glm::mat4 local = m_weaponVisual.asset->PartMatrixAt(origin, clip, clipTime, nullptr);
            const glm::vec3 shift = glm::vec3(local[3]);
            m_weaponTransform.position +=
                carryRight * shift.x + carryUp * shift.y + carryForward * shift.z;
            rotation = rotation * glm::quat_cast(glm::mat3(local));
        }
    }

    const glm::mat4 weaponMatrix =
        glm::translate(glm::mat4(1.0f), m_weaponTransform.position) * glm::mat4_cast(rotation);
    for (size_t i = 0; i < m_weaponVisual.parts.size() && i < m_weaponPartTransforms.size(); ++i)
    {
        const WeaponVisual::Part& part = m_weaponVisual.parts[i];
        glm::mat4 local = part.rest;
        float visible = 1.0f;

        if (clip != nullptr && m_weaponVisual.asset != nullptr)
        {
            const auto& modelParts = m_weaponVisual.asset->parts;
            const auto found = std::find_if(modelParts.begin(), modelParts.end(),
                                            [&](const ModelPart& candidate)
                                            { return candidate.name == part.name; });
            if (found != modelParts.end())
            {
                local = m_weaponVisual.asset->PartMatrixAt(*found, clip, clipTime, &visible);
            }
        }
        else if (static_cast<int>(i) == m_weaponVisual.magazinePart && !authoredReload)
        {
            // The built-in magazine swap, for a model that has no reload clip of its own.
            local = glm::translate(glm::mat4(1.0f), magazineOffset) * local;
            visible = magazineVisible;
        }

        const glm::mat4 world = weaponMatrix * local;
        Transform& transform = m_weaponPartTransforms[i];
        transform.position = glm::vec3(world[3]);
        transform.rotation = glm::quat_cast(glm::mat3(world));
        transform.scale = glm::vec3(visible > 0.01f ? 1.0f : 0.0f);
    }

    // The flash lives at the muzzle and is scaled to nothing except on the frames just after a shot.
    m_muzzleFlashTransform.position = MuzzlePoint();
    m_muzzleFlashTransform.rotation = rotation;
    const float flash = glm::clamp((kick - 0.62f) / 0.38f, 0.0f, 1.0f);
    m_muzzleFlashTransform.scale = glm::vec3(flash);

    // --- Hands ----------------------------------------------------------------------------------
    //
    const float armSpan = (m_rig.upperArmLength + m_rig.lowerArmLength) * 0.94f;
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

    // The magazine change needs a hand to do it with, so the support hand goes to the magazine well
    // rather than hanging in mid air.
    //
    // This runs BEFORE the trigger hand, and the order is load-bearing. Writing a bone's global
    // transform rebuilds every bone after it in the skeleton, and the left arm comes before the
    // right, so placing the left hand last threw the right arm back onto its parent's pose: the
    // trigger hand dropped off the gun the moment a reload started.
    if (supportHandFree && !flat && m_weaponPose.reloading)
    {
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[kLeft]);
        const glm::vec3 magazineWorld =
            m_weaponTransform.position + rotation * (m_weaponVisual.magazineSeated + magazineOffset);
        const glm::vec3 target = magazineWorld + weaponRight * -0.03f;
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

    return true;
}

void PlayerBody::UpdateArms(const PlayerState& state, const PlayerView& view, PhysicsWorld& physics,
                            float dt)
{
    // A weapon takes the arms over: both hands go on it, and the elbows follow. It keeps hold of it
    // when the body goes flat too, because dropping a rifle to crawl is not what anybody does: only
    // the support arm reaches and pulls, and the trigger hand stays on the gun.
    //
    // The order is load-bearing. Writing a bone's global transform rebuilds every bone after it,
    // and the right arm comes after the left in the skeleton, so the crawl has to be solved first
    // or it wipes the grip it was meant to leave alone. Holding the weapon first is what made the
    // hand let go of it the moment the player lay down.
    // Climbing takes both hands, whatever is in them. Nobody hauls themselves over a wall one
    // handed with a rifle up, and it is the only part of a climb anyone can see from inside it.
    if (state.mantling)
    {
        UpdateMantleArms(state, dt);
        return;
    }

    const bool holding = m_hasWeapon;
    if (m_flatness >= 0.02f)
    {
        UpdateCrawlArms(state, view, physics, dt, holding);
    }
    if (holding)
    {
        UpdateWeaponHold(view, dt);
    }
    else if (m_hasHeldItem)
    {
        UpdateHeldItem(view, dt);
    }
}

void PlayerBody::SetHeldItem(Scene& scene, MeshLibrary& meshes, const std::string& name,
                             const MeshData& mesh, const Material& material)
{
    ClearHeldItem(scene);
    m_heldItemMesh = meshes.Upload(mesh, "held_" + name);
    m_heldItemEntity = scene.CreateMeshEntity("held_" + name, Transform{}, m_heldItemMesh, material);
    m_hasHeldItem = true;
}

void PlayerBody::ClearHeldItem(Scene& scene)
{
    if (m_hasHeldItem)
    {
        scene.Destroy(m_heldItemEntity);
    }
    m_heldItemEntity = Entity{};
    m_hasHeldItem = false;
}

void PlayerBody::UpdateHeldItem(const PlayerView& view, float dt)
{
    // Carried in the trigger hand, out in front and a little to the side, where you would hold
    // something you were about to use. The other arm is left alone: one hand is what carrying a
    // medical kit takes, and posing both would read as presenting it rather than holding it.
    const glm::vec3 forward = view.Forward();
    const glm::vec3 yawRight{std::cos(view.yaw), 0.0f, std::sin(view.yaw)};
    const glm::vec3 up = glm::cross(yawRight, forward);

    const glm::vec3 target = view.eyePosition + forward * 0.52f + yawRight * 0.26f + up * -0.34f;

    const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[kRight]);
    FootState& hand = m_hands[static_cast<size_t>(kRight)];
    hand.position = SmoothTowards(hand.position, target, m_config.weaponHandSmoothing, dt);

    const glm::vec3 elbowPole = glm::normalize(-up * 1.0f + yawRight * 0.8f - forward * 0.3f);
    const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                              m_rig.upperArmLength, m_rig.lowerArmLength);

    const glm::quat rotation = glm::quat_cast(glm::mat3(glm::vec3(yawRight), up, -forward));
    m_pose.SetGlobal(m_skeleton, m_rig.upperArm[kRight], SegmentMatrix(shoulder, ik.jointPosition));
    m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[kRight], SegmentMatrix(ik.jointPosition, ik.endPosition));
    m_pose.SetGlobal(m_skeleton, m_rig.hand[kRight],
                     glm::translate(glm::mat4(1.0f), ik.endPosition) * glm::mat4_cast(rotation));

    m_heldItemTransform.position = ik.endPosition;
    m_heldItemTransform.rotation = rotation;
}

void PlayerBody::UpdateMantleArms(const PlayerState& state, float dt)
{
    // Both hands go to the lip of the ledge, take the weight while the body rises, and let go as it
    // comes over the top. Where the lip is is known exactly: the climb was aimed at it.
    const float duration = std::max(state.mantleDuration, 0.05f);
    const float t = std::clamp(state.mantleTime / duration, 0.0f, 1.0f);

    const glm::vec3 travel = state.mantleTo - state.mantleFrom;
    const glm::vec3 flat{travel.x, 0.0f, travel.z};
    const glm::vec3 forward =
        glm::length(flat) > 1e-4f ? glm::normalize(flat) : glm::vec3(std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw));
    const glm::vec3 right{-forward.z, 0.0f, forward.x};

    // The edge itself: at the height being climbed to, back from the landing spot by roughly the
    // depth the body has to clear.
    const glm::vec3 lip = glm::vec3(state.mantleTo.x, state.mantleTo.y, state.mantleTo.z) -
                          forward * m_config.mantleGripBack;

    // Held for the pull, then released to the sides as the body comes over.
    const float release = glm::smoothstep(m_config.mantleReleaseAt, 1.0f, t);
    const float grip = 1.0f - release;

    for (int side = 0; side < 2; ++side)
    {
        const float sideSign = side == kLeft ? -1.0f : 1.0f;
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[side]);

        const glm::vec3 held = lip + right * (sideSign * m_config.mantleGripSpread * m_rig.height);
        // Once the hands let go they swing down and forward, which is where they would be as you
        // step off the top.
        const glm::vec3 freed = shoulder + forward * 0.28f - glm::vec3(0.0f, 0.42f, 0.0f) +
                                right * (sideSign * 0.16f * m_rig.height);
        const glm::vec3 target = glm::mix(held, freed, release);

        FootState& hand = m_hands[static_cast<size_t>(side)];
        hand.position = SmoothTowards(hand.position, target, m_config.weaponHandSmoothing, dt);
        hand.planted = grip > 0.5f;

        // Elbows out and down while pulling, which is what taking your own weight looks like.
        const glm::vec3 elbowPole = glm::normalize(right * (sideSign * 1.0f) -
                                                   glm::vec3(0.0f, 0.7f, 0.0f) - forward * 0.3f);
        const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                                  m_rig.upperArmLength, m_rig.lowerArmLength);

        m_pose.SetGlobal(m_skeleton, m_rig.upperArm[side], SegmentMatrix(shoulder, ik.jointPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[side],
                         SegmentMatrix(ik.jointPosition, ik.endPosition));
        m_pose.SetGlobal(m_skeleton, m_rig.hand[side],
                         glm::translate(glm::mat4(1.0f), ik.endPosition) *
                             glm::mat4_cast(BodyRotation()));
    }
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
    const float crawlPhase = m_crawlDistance / std::max(cycleLength, 0.05f) * glm::two_pi<float>();

    const int firstSide = supportArmOnly ? kLeft : 0;
    const int lastSide = supportArmOnly ? kLeft : 1;

    for (int side = firstSide; side <= lastSide; ++side)
    {
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[side]);
        const float sideSign = side == kLeft ? -1.0f : 1.0f;
        const float phase = crawlPhase + (side == kLeft ? 0.0f : glm::pi<float>());

        const float reach = std::cos(phase) * m_config.crawlReach * m_gaitWeight;
        const float lift = std::max(0.0f, std::sin(phase)) * m_config.crawlLift * m_gaitWeight;

        // On the back you do not reach ahead and pull; you dig your elbows in beside you and push.
        // The arms come back to the hips and the reach shortens as the roll completes.
        constexpr float onBack = 0.0f;
        const float forwardReach = glm::mix(m_config.crawlHandForward + reach, -0.06f, onBack);
        const float outward = glm::mix(0.16f, 0.30f, onBack) * m_rig.height;

        // The hand's offset out to the side swings up and over as the body rolls, the same way the
        // feet do. On your side the upper arm is genuinely in the air, not still pressed flat
        // against a floor the shoulder has left.
        const float lateral = sideSign * outward;
        constexpr float rollRise = 0.0f;

        glm::vec3 target = shoulder + facing * forwardReach +
                           right * lateral;
        target.y = view.renderPosition.y + 0.05f + lift * (1.0f - onBack) + rollRise;

        // Plant the hand on whatever is actually underneath it, unless the roll has lifted it clear.
        const RayHit hit = physics.RayCast(target + glm::vec3(0.0f, 0.5f, 0.0f),
                                           glm::vec3(0.0f, -1.0f, 0.0f), 1.2f);
        if (hit)
        {
            target.y = std::max(target.y, hit.position.y + 0.05f + lift);
        }

        FootState& hand = m_hands[static_cast<size_t>(side)];
        // Blend in from the upright pose so going prone does not snap the arms into place.
        const glm::vec3 restHand = m_pose.GlobalPosition(m_rig.hand[side]);
        const glm::vec3 blended = glm::mix(restHand, target, m_flatness);
        hand.position = SmoothTowards(hand.position, blended, m_config.footPlantSmoothing, dt);
        hand.planted = lift < 0.01f;

        // Elbows bend backwards and outwards, away from the body's front. Rolled onto the back the
        // arm is the other way up, so the elbow drops instead of lifting.
        constexpr float elbowSign = 1.0f;
        const glm::vec3 elbowPole = -facing * 0.6f + right * (sideSign * elbowSign * 0.8f) +
                                    glm::vec3(0.0f, 0.4f * elbowSign, 0.0f);
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
        m_crawlDistance / std::max(m_config.crawlCycleLength, 0.05f) * glm::two_pi<float>();

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
        // Where this foot goes while the body is up on its side, following its own hip instead of
        // the floor. Kept apart from the ground trace below, which answers where the floor is, not
        // how far above it a rolling body has taken the foot.
        float rollRise = 0.0f;
        float proneFootY = hip.y;
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

            // How far this foot sits out to the side of the body, in the body's own frame.
            const float lateral = sideSign * drawn * m_config.crawlLegDraw * legSpan +
                                  sideSign * (m_pose_blend.footSpread - 1.0f) * Ratio::kHipHalfWidth *
                                      m_rig.height;

            // Rolling over turns the whole body about its own length, so that sideways offset
            // swings up and over with it: halfway through, the upper leg is stacked above the lower
            // one and the body is genuinely on its side. Leaving the feet flat on the floor while
            // the torso turned is what made rolling look like the body being switched onto its back
            // rather than rolling through the side to get there.
            //
            // Rotating the offset about the body's forward axis sends the right side down for a
            // positive roll, which is the same way the chest goes.
            const glm::vec3 crawlTarget = hip - facing * back +
                                          right * lateral;
            rollRise = 0.0f;
            // The hips themselves are already stacked by the pelvis roll, so following them is
            // most of what puts one leg above the other.
            proneFootY = hip.y + rollRise;

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
            //
            // Knee height is measured from the hip rather than fixed, because a crouched or prone
            // hip is well below a standing one and a fixed height passed clean over the crates the
            // legs were sinking into.
            const float kneeHeight =
                std::min(m_config.maxFootRise, std::max((hip.y - view.renderPosition.y) * 0.5f, 0.12f));
            const glm::vec3 knee{hip.x, view.renderPosition.y + kneeHeight, hip.z};
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
        // Clamped both ways to what the character could actually have stepped onto or off. Without
        // the upper bound a trace that lands on something tall puts the foot up there while the
        // player stands beside it; without the lower one, a foot whose target hangs over the edge
        // of whatever they are standing on drops all the way to the floor below, and the leg
        // stretches down through the side of the crate. That is the one that looks worst.
        const float base = view.renderPosition.y;
        const float groundY =
            hit ? std::clamp(hit.position.y, base - m_config.maxFootDrop, base + m_config.maxFootRise)
                : base;
        const float groundedY = groundY + m_rig.ankleHeight + lift + rollRise;
        // Up on its side a body has its legs stacked, and only the lower one is on the floor.
        // Pinning both feet to the ground there is what left the legs lying flat while the torso
        // turned over above them. It peaks halfway through and returns to nothing at both ends,
        // because on your front and on your back your heels really are on the ground.
        constexpr float sideness = 0.0f;
        target.y = glm::mix(groundedY, proneFootY, sideness);

        // In the air the ground is no use: it can be metres below, and reaching for it stretches the
        // legs into two straight poles, which is what a jump used to look like. The feet are placed
        // relative to the hips instead, tucked up on the way up and reaching down on the way down.
        // The blend fades back in as the ground comes within a step, so a landing is still planted.
        if (m_airborne > 0.001f)
        {
            const float clearance = view.renderPosition.y - groundY;
            const float airborne = m_airborne * std::clamp((clearance - 0.12f) / 0.35f, 0.0f, 1.0f);
            if (airborne > 0.001f)
            {
                const float tuck = std::max(m_airRise, 0.0f);
                const float reach = std::max(-m_airRise, 0.0f);
                glm::vec3 airTarget = hip + spread + facing * (m_config.airTuck * tuck * legSpan * 0.5f);
                airTarget.y =
                    hip.y - legSpan * (0.62f - m_config.airTuck * tuck + m_config.airReach * reach);
                target = glm::mix(target, airTarget, airborne);
                foot.plant = target;
            }
        }

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
        // On the back the knee goes the other way, because the leg is now the other way up. Left and
        // right swap with it, for the same reason.
        constexpr float rollSign = 1.0f;
        const glm::vec3 pronePole = glm::normalize(right * (sideSign * rollSign * 0.78f) +
                                                   glm::vec3(0.0f, -0.62f * rollSign, 0.0f));
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
    if (m_hasHeldItem)
    {
        if (Transform* transform = scene.GetTransform(m_heldItemEntity))
        {
            *transform = m_heldItemTransform;
        }
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_heldItemEntity))
        {
            renderer->visible = m_config.visible && !m_ragdoll.Active();
        }
    }

    // Each weapon part is its own entity, so a reload can move the magazine on its own and firing
    // can flash the muzzle without either disturbing the rest of the model.
    for (size_t i = 0; i < m_weaponParts.size() && i < m_weaponPartTransforms.size(); ++i)
    {
        if (Transform* transform = scene.GetTransform(m_weaponParts[i]))
        {
            *transform = m_weaponPartTransforms[i];
        }
    }
    if (m_muzzleFlashEntity.IsValid())
    {
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
    // A dead body is not posed, it falls. Everything below this is animation, and animation is what
    // stops when somebody dies.
    if (m_ragdoll.Active())
    {
        m_ragdoll.Step(physics, dt);
        m_ragdoll.ApplyTo(m_skeleton, m_pose);
        PushToScene(scene);
        return;
    }

    UpdatePosture(state, view, playerConfig, dt);
    UpdateArms(state, view, physics, dt);
    UpdateLegs(state, view, playerConfig, physics, dt);
    PushToScene(scene);
}

void PlayerBody::Collapse(const glm::vec3& impulse)
{
    if (!m_built || m_ragdoll.Active())
    {
        return;
    }
    // Started from the pose as it stands, so a body falls from wherever it was standing rather than
    // snapping to a death pose first.
    m_ragdoll.Start(m_skeleton, m_pose, impulse);
}

void PlayerBody::Revive()
{
    m_ragdoll.Stop();
    m_pose.ResetToBind(m_skeleton);
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
