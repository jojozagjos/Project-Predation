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

// How thick each part is drawn, as fractions of standing height: width across the body, then depth
// front to back. Named here rather than written into the mesh sizes, because two things need them:
// what gets drawn, and how far a collapsed body has to keep off the floor. A ragdoll that used one
// radius for every joint sank its chest into the ground while its wrists floated above it.
constexpr float kHipsWide = 0.150f, kHipsDeep = 0.108f;
constexpr float kAbdomenWide = 0.160f, kAbdomenDeep = 0.112f;
constexpr float kChestWide = 0.215f, kChestDeep = 0.120f;
constexpr float kNeckWide = 0.050f, kNeckDeep = 0.050f;
constexpr float kHeadWide = 0.098f, kHeadTall = 0.132f, kHeadDeep = 0.118f;
constexpr float kUpperArmWide = 0.060f, kUpperArmDeep = 0.060f;
constexpr float kLowerArmWide = 0.050f, kLowerArmDeep = 0.050f;
constexpr float kHandWide = 0.050f, kHandTall = 0.082f, kHandDeep = 0.046f;
constexpr float kThighWide = 0.086f, kThighDeep = 0.086f;
constexpr float kShinWide = 0.068f, kShinDeep = 0.068f;
constexpr float kBootWide = 0.074f, kBootDeep = 0.160f;
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


// A matrix placing a segment that runs from `a` to `b`, with its local +Y along the segment and its
// local +X turned towards `side`.
//
// The roll about the segment is not decoration. This used to take the minimal rotation onto the
// direction, which leaves the spin undefined, and every drawn limb then had to recover a roll from
// somewhere else. The body's facing was the obvious somewhere and it is the worst possible choice:
// a crouched thigh runs almost exactly along a folded torso's facing, so what survived projecting
// the one out of the other was three centimetres of rounding, and the drawn leg spun a full turn
// about its own axis every stride. Limb sections are square, so that reads as the leg twisting into
// a diamond and back rather than as a spin, which is how it was reported: the legs rotating
// sideways.
//
// There is no fixed reference that works. Sideways is the best single choice for a leg, which never
// points out along the hips, and a poor one for an arm, which spends half a crawl reaching out to
// the side. The knee pole is worse still: it lies in the plane the leg swings through and crosses
// the thigh once a stride. So the reference is the joint's own hinge, which is square to both of
// its segments by construction and only fails when the limb straightens, and the caller carries the
// answer forward from the last frame to cover that. See PlayerBody::RollFront.
glm::mat4 SegmentMatrix(const glm::vec3& a, const glm::vec3& b, const glm::vec3& front)
{
    return glm::translate(glm::mat4(1.0f), a) * glm::mat4_cast(AlignYWithRoll(b - a, front));
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
        // Turned end over end relative to the forearm, so the hand's own +Y runs from the wrist
        // towards the fingers. Every arm bone in this rig hangs down its parent's -Y, so without
        // this the hand would point back up the arm and the glove drawn in its frame would sit
        // above the wrist rather than beyond it.
        Transform handRest = LocalOffset(0.0f, -m_rig.lowerArmLength, 0.0f);
        handRest.rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        m_rig.hand[side] = m_skeleton.AddBone("hand" + suffix, m_rig.lowerArm[side], handRest);
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

    // How far each joint has to keep off the floor once the body is a ragdoll. A single radius for
    // every joint is wrong by a factor of three across a body: it either sinks the chest into the
    // ground or floats the wrists above it. Half the mean of the part's width and depth is the
    // radius of the cylinder it would roll on, which is what a joint resting on a floor is doing.
    //
    // Done here rather than with the drawn parts, because a body posed for a test has no meshes and
    // still has to fall over correctly.
    const auto radius = [](float wide, float deep) { return (wide + deep) * 0.25f; };
    m_boneRadius.assign(static_cast<size_t>(m_skeleton.BoneCount()), Ratio::kNeckWide * 0.5f * h);
    const auto setRadius = [&](BoneIndex bone, float value)
    {
        if (bone != kInvalidBone && static_cast<size_t>(bone) < m_boneRadius.size())
        {
            m_boneRadius[static_cast<size_t>(bone)] = value;
        }
    };
    setRadius(m_rig.pelvis, radius(Ratio::kHipsWide, Ratio::kHipsDeep) * h);
    setRadius(m_rig.spine, radius(Ratio::kAbdomenWide, Ratio::kAbdomenDeep) * h);
    setRadius(m_rig.chest, radius(Ratio::kChestWide, Ratio::kChestDeep) * h);
    setRadius(m_rig.neck, radius(Ratio::kNeckWide, Ratio::kNeckDeep) * h);
    setRadius(m_rig.head, radius(Ratio::kHeadWide, Ratio::kHeadDeep) * h);
    for (int side = 0; side < 2; ++side)
    {
        setRadius(m_rig.shoulder[side], radius(Ratio::kUpperArmWide, Ratio::kUpperArmDeep) * h);
        setRadius(m_rig.upperArm[side], radius(Ratio::kUpperArmWide, Ratio::kUpperArmDeep) * h);
        setRadius(m_rig.lowerArm[side], radius(Ratio::kLowerArmWide, Ratio::kLowerArmDeep) * h);
        setRadius(m_rig.hand[side], radius(Ratio::kHandWide, Ratio::kHandDeep) * h);
        setRadius(m_rig.upperLeg[side], radius(Ratio::kThighWide, Ratio::kThighDeep) * h);
        setRadius(m_rig.lowerLeg[side], radius(Ratio::kShinWide, Ratio::kShinDeep) * h);
        // A boot rests on its sole, so what keeps the ankle up is the ankle height, not the width.
        setRadius(m_rig.foot[side], m_rig.ankleHeight);
    }
    m_ragdoll.SetJointRadii(m_boneRadius);
    m_boneFront.assign(static_cast<size_t>(m_skeleton.BoneCount()), glm::vec3(0.0f, 0.0f, -1.0f));
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
    limb("hips", m_rig.pelvis, m_rig.spine, Ratio::kHipsWide * h, Ratio::kHipsDeep * h, kSuitMaterial);
    limb("abdomen", m_rig.spine, m_rig.chest, Ratio::kAbdomenWide * h, Ratio::kAbdomenDeep * h,
         kSuitMaterial);
    // The shoulder yoke: broad and shallow, spanning between the shoulder joints and closing the
    // top of the torso. With the chest joint at its proper height this is the whole top of the
    // body, so there is nothing left to cap and no separate collar piece between it and the neck.
    limb("chest", m_rig.chest, m_rig.neck, Ratio::kChestWide * h, Ratio::kChestDeep * h, kGearMaterial);
    // Hidden in first person, because nobody can see their own neck.
    limb("neck", m_rig.neck, m_rig.head, Ratio::kNeckWide * h, Ratio::kNeckDeep * h, kSuitMaterial, true);

    // A human head is about 0.13 of standing height tall and noticeably narrower than it is tall.
    // Sized from the crown down, so the top of the head lands at full standing height.
    gear("head", m_rig.head, {Ratio::kHeadWide * h, Ratio::kHeadTall * h, Ratio::kHeadDeep * h},
         {0.0f, 0.063f * h, 0.004f * h + m_config.skullBehindEye},
         kHelmetMaterial, PartFrame::BoneFrame, true);

    // --- Arms.
    for (int side = 0; side < 2; ++side)
    {
        const char* upper = side == kLeft ? "upper_arm_left" : "upper_arm_right";
        const char* fore = side == kLeft ? "forearm_left" : "forearm_right";
        const char* glove = side == kLeft ? "hand_left" : "hand_right";

        // Hung off the upper arm bone rather than the shoulder, though the two sit at the same
        // point. The shoulder belongs to the torso and carries the chest's front; the upper arm
        // carries the arm's own, which is what the drawn segment needs to take its roll from.
        limb(upper, m_rig.upperArm[side], m_rig.lowerArm[side], Ratio::kUpperArmWide * h,
             Ratio::kUpperArmDeep * h, kSuitMaterial);
        limb(fore, m_rig.lowerArm[side], m_rig.hand[side], Ratio::kLowerArmWide * h,
             Ratio::kLowerArmDeep * h, kSuitMaterial);
        // In the hand bone's own frame, whose +Y runs from the wrist towards the fingers. Drawn in
        // the body's frame instead, as it was, a glove is a box hanging below the wrist however the
        // arm is held: reach out to carry something and the hand still points at the floor.
        gear(glove, m_rig.hand[side], {Ratio::kHandWide * h, Ratio::kHandTall * h, Ratio::kHandDeep * h},
             {0.0f, Ratio::kHandTall * 0.34f * h, 0.0f}, kGloveMaterial, PartFrame::BoneFrame);
    }

    // --- Legs.
    for (int side = 0; side < 2; ++side)
    {
        const char* thigh = side == kLeft ? "thigh_left" : "thigh_right";
        const char* shin = side == kLeft ? "shin_left" : "shin_right";
        const char* boot = side == kLeft ? "boot_left" : "boot_right";

        limb(thigh, m_rig.upperLeg[side], m_rig.lowerLeg[side], Ratio::kThighWide * h,
             Ratio::kThighDeep * h, kSuitMaterial);
        limb(shin, m_rig.lowerLeg[side], m_rig.foot[side], Ratio::kShinWide * h,
             Ratio::kShinDeep * h, kSuitMaterial);
        gear(boot, m_rig.foot[side], {Ratio::kBootWide * h, m_rig.ankleHeight * 1.35f, Ratio::kBootDeep * h},
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
    // The hands go with the body. Only the body's own parts used to, so a player who left took
    // their skeleton away and left the rifle they were holding hanging in the air where they had
    // been standing, with nothing to move it and nobody able to pick it up. The same leak filled a
    // new game with the last one's guns.
    DestroyWeapon(scene);
    scene.Destroy(m_heldItemEntity);
    m_heldItemEntity = Entity{};
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
    // And how far through the crouch. The spine lean is only ever non-zero for a crouch, so it
    // doubles as the blend and follows the same smoothing as the rest of the posture.
    m_crouchness = m_config.crouch.spineLeanDeg > 0.01f
                       ? std::clamp(m_pose_blend.spineLeanDeg / m_config.crouch.spineLeanDeg, 0.0f, 1.0f)
                       : 0.0f;

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
    // How badly hurt, as far as the pose is concerned. Smoothed, so a hit is a wobble that grows
    // rather than an instant change of how the weapon is held.
    {
        const float healthFraction = std::clamp(state.health / 100.0f, 0.0f, 1.0f);
        const float threshold = std::max(playerConfig.injuryThreshold, 0.01f);
        const float hurt = healthFraction < threshold ? (1.0f - healthFraction / threshold) : 0.0f;
        m_injurySway = SmoothTowards(m_injurySway, hurt * (playerConfig.injuredSwayScale - 1.0f),
                                     2.5f, dt);
    }

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
    // Climbing folds the body forward over the ledge as the hands pull, then straightens as the
    // legs come under it. Without it the torso rides up the wall bolt upright, which is what made
    // the climb read as being lifted rather than as pulling yourself up.
    float climbFold = 0.0f;
    if (state.mantling || m_mantleFade > 0.001f)
    {
        const float duration = std::max(state.mantleDuration, 0.05f);
        const float t = std::clamp(state.mantleTime / duration, 0.0f, 1.0f);
        // Most of the fold in the middle of the pull, gone by the time you stand up on top.
        climbFold = glm::radians(m_config.mantleFoldDegrees) *
                    std::sin(t * glm::pi<float>()) * std::max(m_mantleFade, state.mantling ? 1.0f : 0.0f);
    }

    const float runLean = glm::radians(m_lean) + climbFold;
    // Peek lean: the torso tips sideways so the body follows the camera out past cover.
    const float peek = state.leanAmount * glm::radians(m_config.leanAngleDegrees);

    // Crawling, the shoulders roll from side to side as each arm reaches and pulls. Without it the
    // torso slides along the floor as one rigid plank while the limbs work, which is what made the
    // crawl read as a body being dragged rather than one pulling itself.
    const float crawlPhase =
        m_crawlDistance / std::max(m_config.crawlCycleLength, 0.05f) * glm::two_pi<float>();
    const float crawlRoll = std::sin(crawlPhase) * glm::radians(m_config.crawlShoulderRollDegrees) *
                            m_gaitWeight * m_flatness;

    // A bladed stance while a weapon is up. Negative about +Y turns the body towards its own right,
    // which is the side the weapon is carried on, and that brings the support shoulder forward and
    // in towards the centre line where the handguard actually is. It fades out lying down, where
    // the body has its own heading and both arms are doing something else.
    const float carryBlade =
        m_hasWeapon ? glm::radians(m_config.weaponCarryTurnDegrees) * (1.0f - m_flatness) : 0.0f;

    m_pose.Local(m_rig.spine).rotation =
        glm::angleAxis(-torsoTwist * 0.45f - carryBlade * 0.4f, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(-(spineLean * 0.65f + runLean * 0.6f), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::angleAxis(-peek * 0.55f + crawlRoll * 0.4f, glm::vec3(0.0f, 0.0f, 1.0f));
    m_pose.Local(m_rig.chest).rotation =
        glm::angleAxis(-torsoTwist * 0.55f - carryBlade * 0.6f, glm::vec3(0.0f, 1.0f, 0.0f)) *
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
    // And further forward the further down you look, so looking at your own feet shows you the
    // length of your body rather than the top of your chest. Tucking your chin does move your eyes
    // out over your torso, and the offset can afford to be bigger here because nobody spins on the
    // spot while staring at the floor, which is the case a large fixed offset would spoil.
    const float lookingDown =
        glm::smoothstep(0.0f, 1.0f, std::clamp(-view.pitch / glm::half_pi<float>(), 0.0f, 1.0f));

    // The standing offset follows the body, so looking around does not swing the character. The
    // part that only exists while looking down follows the *view*, because that is the case where
    // you are looking at your own body and it has to stay in front of you: measured along the body
    // it slid off to one side as soon as you turned your head while looking at your boots.
    const glm::vec3 bodyFacing{std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw)};
    const glm::vec3 viewFacing{std::sin(view.yaw), 0.0f, -std::cos(view.yaw)};
    // Negative, because this offset is subtracted: pushing the desired head position forward is
    // what carries the hips forward with it.
    const glm::vec3 offset = bodyFacing * m_config.eyeForwardOfHead +
                             viewFacing * (m_config.eyeForwardLookingDown * lookingDown) -
                             bodyFacing * (m_config.crouchBodyForward * m_crouchness);

    const glm::vec3 desiredHead =
        view.eyePosition - offset - glm::vec3(0.0f, m_config.eyeAboveHead, 0.0f);
    m_rootPosition += desiredHead - m_pose.GlobalPosition(m_rig.head);

    root = glm::translate(glm::mat4(1.0f), m_rootPosition) * glm::mat4_cast(BodyRotation());
    m_pose.ComputeGlobals(m_skeleton, root);
}

// The direction a bone's front faces, carried forward from the last frame so it can never flip.
//
// Whatever fixed direction is used to resolve a limb's spin, some pose lines the limb up with it and
// the answer collapses into rounding. That is what turned a crouched thigh a full circle about its
// own axis every stride. The joint's hinge is square to both its segments and so cannot line up with
// either, but it vanishes when the limb straightens, which a standing leg nearly does.
//
// So the roll is carried: last frame's answer, projected onto this frame's direction, which is
// continuous by construction. The hinge only pulls it back into line, and only as far as the hinge
// is square to this bone and therefore has an opinion worth listening to. A straight limb keeps the
// roll it had, which is exactly right, because a straight limb has no bend to take one from.
glm::vec3 PlayerBody::RollFront(BoneIndex bone, const glm::vec3& axis, const glm::vec3& hinge)
{
    const size_t index = static_cast<size_t>(bone);
    const glm::vec3 preferred = glm::cross(hinge, axis);
    const float trust = glm::length(preferred);

    glm::vec3 carried = index < m_boneFront.size() ? m_boneFront[index] : glm::vec3(0.0f);
    carried -= axis * glm::dot(carried, axis);
    if (glm::length(carried) < 1e-3f)
    {
        carried = trust > 1e-3f ? preferred : glm::vec3(0.0f, 0.0f, -1.0f) - axis * -axis.z;
    }
    if (glm::length(carried) < 1e-3f)
    {
        carried = glm::vec3(1.0f, 0.0f, 0.0f) - axis * axis.x;
    }
    carried = glm::normalize(carried);

    if (trust > 1e-3f)
    {
        // A quarter of the way each frame, scaled by how square the hinge is. Fast enough that the
        // roll never drifts anywhere, slow enough that a hinge passing through nothing as the limb
        // straightens cannot drag the roll round with it.
        const glm::vec3 blended =
            glm::mix(carried, preferred / trust, std::clamp(trust, 0.0f, 1.0f) * 0.25f);
        if (glm::length(blended) > 1e-4f)
        {
            carried = glm::normalize(blended);
        }
    }

    if (index < m_boneFront.size())
    {
        m_boneFront[index] = carried;
    }
    return carried;
}

// Places a limb bone from `a` to `b`, rolled so its front faces the way the joint bends.
glm::mat4 PlayerBody::SegmentFrame(BoneIndex bone, const glm::vec3& a, const glm::vec3& b,
                                   const glm::vec3& hinge)
{
    const glm::vec3 delta = b - a;
    const float length = glm::length(delta);
    if (length < 1e-5f)
    {
        return glm::translate(glm::mat4(1.0f), a);
    }
    return SegmentMatrix(a, b, RollFront(bone, delta / length, hinge));
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

// Puts a model in the hands directly, without going through the weapon database or the disk. The
// editor needs this: what it is holding is whatever is open in front of it, which has usually not
// been saved yet and belongs to no weapon at all.
void PlayerBody::SetWeaponFromModel(Scene& scene, MeshLibrary& meshes,
                                    const WeaponDefinition& definition, const ModelAsset& model)
{
    DestroyWeapon(scene);
    m_weaponId = definition.id;
    m_weaponVisual = BuildWeaponVisualFrom(model, definition, m_textures);
    BuildWeaponEntities(scene, meshes, definition);
}

void PlayerBody::RefreshWeaponSockets(const WeaponDefinition& definition, const ModelAsset& model)
{
    if (!m_hasWeapon)
    {
        return;
    }
    ApplyWeaponSockets(m_weaponVisual, model, definition);
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
    m_weaponVisual = BuildWeaponVisual(*definition, m_textures);
    BuildWeaponEntities(scene, meshes, *definition);
}

// One entity per part, however the visual was arrived at.
void PlayerBody::BuildWeaponEntities(Scene& scene, MeshLibrary& meshes,
                                     const WeaponDefinition& definition)
{
    // One entity per part, which is what lets a reload take the magazine out on its own.
    const std::string key = "weapon_" + definition.key;
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
    const float flashRadius = std::max(definition.size.y, 0.06f) * 0.9f;
    m_muzzleFlashEntity =
        scene.CreateMeshEntity(key + "_flash", Transform{},
                               meshes.Upload(Primitives::Sphere(flashRadius, 10, 6), key + "_flash"),
                               Material::Emissive({1.0f, 0.78f, 0.36f}, 3.4f));
}

// Moves the whole drawn weapon, parts and all. The parts carry their own world transforms, worked
// out when the hold was solved, so moving the weapon afterwards has to move them with it.
void PlayerBody::ShiftWeapon(const glm::vec3& delta)
{
    m_weaponTransform.position += delta;
    for (Transform& transform : m_weaponPartTransforms)
    {
        transform.position += delta;
    }
    m_muzzleFlashTransform.position += delta;
}

glm::vec3 PlayerBody::MuzzlePoint() const
{
    return m_weaponTransform.position + m_weaponTransform.rotation * m_weaponVisual.muzzle;
}

// Two failures with one cause. Walking up to a wall put the barrel through it, because the capsule
// stops a third of a metre from a surface and a rifle is most of a metre long; the soft pull-back
// that was meant to handle it traced along the view, so looking sideways at the wall aimed the trace
// somewhere the barrel was not. And lying down put the hold under the floor, because the prone carry
// hangs the weapon below an eye that is itself only a few centimetres up.
//
// Both are the same question, asked of the world rather than of a rule: is anything between the
// player and where they want to hold this, and is there floor under it.
glm::vec3 PlayerBody::ClearOfWorld(PhysicsWorld& physics, const glm::vec3& eye, glm::vec3 wanted,
                                   float clearance) const
{
    const glm::vec3 toHold = wanted - eye;
    const float distance = glm::length(toHold);
    if (distance > 1e-4f)
    {
        const glm::vec3 direction = toHold / distance;
        const RayHit blocked = physics.RayCast(eye, direction, distance + clearance);
        if (blocked)
        {
            wanted = eye + direction * std::max(blocked.distance - clearance, 0.0f);
        }
    }

    // Traced rather than compared against the player's own feet, so lying on a crate keeps the hold
    // on top of the crate instead of at the height of the floor beside it.
    //
    // Only just below, deliberately. A long probe finds the top of anything the player happens to be
    // standing against and lifts the weapon onto it, which is how looking up beside a crate threw
    // the gun out of the player's hands. This is meant to answer "is this resting in the floor",
    // which is a question about the few centimetres underneath it.
    constexpr float kProbeAbove = 0.12f;
    const float probe = kProbeAbove + clearance + 0.24f;
    const RayHit ground = physics.RayCast(wanted + glm::vec3(0.0f, kProbeAbove, 0.0f),
                                          glm::vec3(0.0f, -1.0f, 0.0f), probe);
    if (ground)
    {
        wanted.y = std::max(wanted.y, ground.position.y + clearance);
    }
    return wanted;
}

// Where the trigger hand will be during a climb, and how much of the way there it is.
//
// Worked out here as well as in the arm solve, because whatever is being carried has to move before
// the weapon's parts are placed and the arms are solved after that. Both read the same lip and the
// same release curve, so the gun and the hand holding it arrive together.
bool PlayerBody::MantleCarry(const PlayerState& state, glm::vec3& outPoint, glm::quat& outRotation,
                             float& outWeight) const
{
    if (m_mantleFade <= 0.001f)
    {
        return false;
    }

    const float duration = std::max(state.mantleDuration, 0.05f);
    const float t = std::clamp(state.mantleTime / duration, 0.0f, 1.0f);
    const glm::vec3 travel = state.mantleTo - state.mantleFrom;
    const glm::vec3 flat{travel.x, 0.0f, travel.z};
    const glm::vec3 forward = glm::length(flat) > 1e-4f
                                  ? glm::normalize(flat)
                                  : glm::vec3(std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw));
    const glm::vec3 right{-forward.z, 0.0f, forward.x};

    const float release = glm::smoothstep(m_config.mantleReleaseAt, 1.0f, t);
    outWeight = (1.0f - release) * m_mantleFade;
    outPoint = state.mantleEdge + right * (m_config.mantleGripSpread * m_rig.height);
    // Muzzle along the way the climb is going, which is where a hand over a lip points it.
    outRotation = LookRotation(forward, glm::vec3(0.0f, 1.0f, 0.0f));
    return outWeight > 0.001f;
}

bool PlayerBody::UpdateWeaponHold(const PlayerState& state, const PlayerView& view,
                                  PhysicsWorld& physics, float dt)
{
    if (!m_hasWeapon)
    {
        return false;
    }

    const bool flat = m_flatness > 0.5f;

    // How boxed in the weapon is, from the trace taken once a frame in Update.
    const float crowded = 1.0f - m_wallClearance;

    // You cannot aim into a wall. Holding the sights up against one puts them on the view axis with
    // the barrel inside the bricks, which is a lie in the shape of a feature; the sights come down
    // instead, and the player can see perfectly well why.
    //
    // And you cannot aim while hauling yourself over a ledge, for the same reason: both hands are on
    // the wall. The sights come down as the climb starts and come back as it releases, on the same
    // fade the hands use, so it is one movement rather than the sights switching off.
    const float wantedAim = glm::clamp(m_weaponPose.aim, 0.0f, 1.0f) * (1.0f - m_mantleFade);
    const float aim = wantedAim * glm::mix(1.0f, m_config.weaponWallAim, crowded);

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
    // A hurt player cannot hold a weapon still. This is most of what makes being shot something
    // you feel rather than a number you read.
    const float breathe =
        (1.0f - aim * 0.85f) * m_config.weaponBreatheAmount * (1.0f + m_injurySway);
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
    // Pulled in against a wall, which is what anyone does with a long weapon in a corridor. Walking
    // into one otherwise put the barrel straight through it.
    //
    // Pulled straight back, and only back. It used to be lifted as well, on the idea that a weapon
    // comes up in a corridor, and what that did was raise the receiver into the camera: the eye
    // sits above the hold, so lifting the hold closes the last of the gap between them.
    // A long weapon and a near wall cannot both be satisfied. Something has to give, and pulling
    // straight back gives the barrel to the wall and the receiver to the camera. Dropping the muzzle
    // gives up only where the weapon is pointing, which nobody is using in a corridor anyway, and it
    // is what anyone does with a rifle indoors.
    //
    // Not while aiming. Sighted, the whole weapon lies on the view axis and pulling it in runs it
    // straight at the eye: what a player sees is the gun reversing into their face the moment they
    // brush a doorframe, which is worse than a barrel in the bricks and is the one thing they are
    // looking at while it happens. The pull-back fades out with the sights coming up, on the aim
    // asked for rather than the one the wall left behind.
    const float forwardScale =
        glm::mix(glm::mix(1.0f, m_config.weaponWallForward, crowded), 1.0f, wantedAim);

    // How short the weapon is, from the weapon itself rather than from anything anyone had to
    // write down: a metre-long carbine reads zero, a pistol reads one, and a submachine gun lands
    // somewhere sensible in between without being a third case in the data.
    const float weaponLength =
        std::max(m_weaponVisual.muzzle.z - m_weaponVisual.rearPoint.z, 0.05f);
    const float shortness = glm::clamp((0.52f - weaponLength) / 0.30f, 0.0f, 1.0f);

    const glm::vec3 readyOffset =
        carryRight * (m_config.weaponReadyRight + m_config.weaponShortRight * shortness) +
        carryUp * (m_config.weaponReadyDown + m_config.weaponShortRise * shortness) +
        carryForward *
            ((m_config.weaponReadyForward + m_config.weaponShortForward * shortness) * forwardScale);
    // Never closer than the minimum, however crowded it is: at full aim the pull-back runs straight
    // down the view axis, so an unclamped one puts the receiver through the near plane.
    const float aimForwardDistance =
        std::max(m_config.weaponAimForward * forwardScale, m_config.weaponAimMinForward);
    const glm::vec3 sightedOffset = aimForward * aimForwardDistance;

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
        // Shortened only, and only along the aim axis. Moving along that axis leaves the sight on
        // it; moving across it does not, and the lift that used to be added here is why the sights
        // stopped lining up as soon as the player aimed downwards. Whatever it was keeping the
        // weapon clear of, a sight that does not sit on the axis is not aiming at all.
        const float shorten = std::min(m_config.weaponAimForward * 0.55f * steep * aim,
                                       std::max(aimForwardDistance - m_config.weaponAimMinForward, 0.0f));
        offset -= aimForward * shorten;
    }

    // Bringing a weapon up: it starts low and out of the way and rises into the hold. Presentation
    // only, so it never delays a shot.
    //
    // Bringing a weapon up and reloading it are authored on the model now, not written in here.
    //
    // There was a built-in version of each, and it was in the way. It could only ever move the
    // weapon as one lump, because it does not know what parts a weapon has; a real reload is a
    // magazine coming out of a well and a bolt going home, and those are the model's own parts.
    // Worse, a model that carried its own clip had to fight it: the built-in movement stepped aside
    // for a clip of the same name, so what an author saw depended on what they had named things.
    // Fire is the exception and it is deliberate, below: the recoil stays here, because it is the
    // whole weapon moving in a hold and every weapon does it, and a "fire" clip plays on top of it
    // so a bolt can cycle without anybody re-authoring the kick.
    const float draw = glm::clamp(m_weaponPose.draw, 0.0f, 1.0f);

    // --- Which way it points --------------------------------------------------------------------
    const glm::quat sighted = LookRotation(aimForward, aimUp);
    // Held ready the weapon cants inwards, the way a carried rifle does, and the muzzle comes down
    // as the space in front runs out. Lowering is what makes room for a long weapon indoors: pulling
    // it back only moves the problem from the wall to the camera.
    //
    // Note the sign. The barrel runs down the weapon's +Z, and a positive turn about +X sends +Z
    // downwards, so lowering the muzzle is a positive angle here. The small standing cant is
    // negative and therefore lifts it slightly, which is what a rifle held ready actually does.
    const glm::quat ready =
        LookRotation(carryForward, carryUp) *
        glm::angleAxis(glm::radians(-9.0f + m_config.weaponWallLower * crowded),
                       glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::angleAxis(glm::radians(-4.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
        // Turned in across the body. A positive turn about +Y sends the barrel to the right, so
        // inwards is negative.
        glm::angleAxis(glm::radians(-m_config.weaponReadyInward), glm::vec3(0.0f, 1.0f, 0.0f));
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
    // --- Reload ---------------------------------------------------------------------------------
    // How far through a reload the hands are. The weapon's own movement through one is the model's
    // "reload" clip; this is what the support hand follows, which no clip can do because a clip
    // moves parts of a weapon and a hand is not one of them.
    //
    // The movement runs to its end even when the reload itself finishes first. The weapon is
    // usable again the moment the simulation says so; what is left is a pair of hands finishing
    // what they were doing, and cutting that off partway is what snapped.
    if (m_weaponPose.reloading)
    {
        m_reloadPlay = glm::clamp(m_weaponPose.reload, 0.0f, 1.0f);
        m_reloadRunning = true;
    }
    else if (m_reloadRunning)
    {
        m_reloadPlay += dt / std::max(m_config.reloadFollowThrough, 0.05f);
        if (m_reloadPlay >= 1.0f)
        {
            m_reloadPlay = 1.0f;
            m_reloadRunning = false;
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

    // The offset says where the *hold* goes, not where the model's origin goes, and the two are
    // only the same thing for a weapon built procedurally with its origin on the grip. An imported
    // model has its origin wherever the person who made it left it, which for both guns that
    // arrived is the middle of the receiver: carrying by the origin put the whole carbine a
    // hand-span too far forward, and the support arm was left pointing at a handguard 21 cm out of
    // its reach.
    //
    // Held ready, the point carried is the trigger grip, so the tuning below reads as where the
    // firing hand is. Sighted, it is the sight itself, which is what puts the sight block on the
    // view axis in all three axes rather than only in height.
    //
    // Which grip the weapon is *carried* by can be pinned to an older value, and that is the whole
    // of what the editor's "hold it still" does. Placing the weapon by the live grip socket means
    // the trigger hand is at the carry point by construction and can never move: dragging the grip
    // moves the gun instead, which is the right answer in the game and the wrong one while placing
    // a grip, where the question is where the hand ends up on the weapon. Pinning the carried grip
    // leaves the gun where it is and walks the hand along it.
    const glm::vec3 carriedGrip =
        m_weaponGripPinned ? m_weaponPinnedGrip : m_weaponVisual.triggerGrip;
    const glm::vec3 holdPoint = glm::mix(carriedGrip, m_weaponVisual.sightPoint, aim);
    // And the socket's own turn, so a model exported lying on its side can be righted by rotating
    // the grip rather than by rotating the geometry, which would take the sockets, the clips and
    // everything else along with it. The model spins about the hold: the grip stays in the hand.
    const glm::quat holdTurn =
        glm::slerp(m_weaponVisual.gripRotation, m_weaponVisual.sightRotation, aim);
    rotation = rotation * holdTurn;
    m_weaponTransform.position = view.eyePosition + offset - rotation * holdPoint;
    m_weaponTransform.rotation = rotation;
    m_weaponTransform.scale = glm::vec3(1.0f);


    // The barrel out of the scenery. The muzzle is what goes through a wall, not the grip, so that
    // is what gets traced for, and the whole weapon moves by whatever correction it needs.
    //
    // Skipped during a climb: both hands are on a ledge, the carry offset this started from
    // describes nowhere the body is, and the correction was dragging the weapon down onto whatever
    // the player was climbing.
    if (m_mantleFade <= 0.001f)
    {
        const glm::vec3 muzzle =
            m_weaponTransform.position + m_weaponTransform.rotation * m_weaponVisual.muzzle;
        const glm::vec3 clear =
            ClearOfWorld(physics, view.eyePosition, muzzle, m_config.muzzleClearance);
        // Faded out with the sights, like the pull-back above and for the same reason: sighted, the
        // weapon lies along the view, so lifting the muzzle out of a wall drags the receiver back
        // through the camera. Aiming, the barrel is given to the wall.
        m_weaponTransform.position += (clear - muzzle) * (1.0f - wantedAim);
    }

    // Never so close to the eye that the camera is inside it. The trace above keeps the barrel out
    // of a wall, and it has to give way to this: a barrel is half a metre long and a player can
    // stand a third of a metre from a wall, so there are places where the two cannot both be had.
    // Given that choice the camera wins, because a barrel tip in the bricks is something nobody
    // looks at and a receiver through the near plane fills the screen. Lowering the muzzle is what
    // makes the choice rare.
    //
    // Measured at the hold rather than at the model's origin, which is the only reading that means
    // anything across models: these two numbers were tuned when a weapon's origin was its grip, and
    // an imported one has its origin in the middle of the receiver a hand-span further forward.
    {
        const float floorDistance =
            glm::mix(m_config.weaponMinForward, m_config.weaponAimMinForward, aim);
        const glm::vec3 along = aim > 0.5f ? aimForward : carryForward;
        const glm::vec3 held = m_weaponTransform.position + m_weaponTransform.rotation * holdPoint;
        const glm::vec3 rear =
            m_weaponTransform.position + m_weaponTransform.rotation * m_weaponVisual.rearPoint;
        // Two floors, whichever needs the bigger push: the hold stays out at arm's length, and the
        // back of the stock stays in front of the near plane.
        //
        // The second one only matters while aiming, and it is faded in with the sights rather than
        // applied throughout. Carried, the stock hangs low and to the right, so a stock level with
        // the eye is behind the camera and off to one side of a frustum that has no width there at
        // all: nothing is drawn and nothing is cut. Sighted, the whole weapon lies along the view
        // axis, and then the near plane really does slice the receiver open. Enforcing it while
        // carried buys nothing and pushes the muzzle further into every wall.
        //
        // On the aim the player asked for, not the one the wall left them with. Jammed against
        // something, the sights are broken down towards the carry, and using that here switched the
        // floor off exactly where it was needed: aiming into a corner is the one place a receiver
        // ends up through the near plane.
        const float rearFloor = glm::mix(-0.30f, m_config.weaponRearMinForward, wantedAim);
        const float push = std::max(floorDistance - glm::dot(held - view.eyePosition, along),
                                    rearFloor - glm::dot(rear - view.eyePosition, along));
        if (push > 0.0f)
        {
            m_weaponTransform.position += along * push;
        }
    }

    // The trigger hand never lets go, so the weapon is pulled in until the grip is somewhere the
    // arm can actually reach. Without this the hold asked for a point further away than the arm is
    // long, the IK stretched to its limit, and the weapon appeared to float free of the hand that
    // was supposed to be holding it: looking straight up, or turning while prone, put the grip well
    // out of range. It happens here, before the parts are placed, so the model moves with it.
    //
    // And after the clearance above rather than before it, which is the way round it has to be. A
    // weapon standing against a wall and looked up at could have its muzzle lifted onto whatever
    // was above, well out of reach, and the arm then drew as a straight bar pointing at a gun that
    // had visibly left the hand. Half a centimetre of barrel back in the wall is the cheaper fault.
    {
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[kRight]);
        const float reach = (m_rig.upperArmLength + m_rig.lowerArmLength) * 0.94f;
        // The grip, not the origin. Measuring the origin let an imported weapon sit with its grip
        // well past the arm's reach while the point being checked was still inside it.
        const glm::vec3 gripOffset = m_weaponTransform.rotation * carriedGrip;

        // Aiming, the hold comes back along the sight line rather than in towards the shoulder.
        // Sliding along that line leaves the sight on it; pulling across it is what took the sights
        // off the middle of the screen when the player aimed steeply upwards, where the hold is
        // furthest from the shoulder and the clamp bites hardest.
        //
        // Solved rather than stepped. This walked back along the sight line two centimetres at a
        // time until the grip came within reach, which means the answer moves in two-centimetre
        // jumps: with the hold sitting near the limit, one frame takes a step and the next does not,
        // and what that looks like from inside is the hands shaking while you aim and turn. The
        // distance to move is the smaller root of where the line first enters the arm's sphere.
        if (aim > 0.5f)
        {
            const glm::vec3 fromShoulder = m_weaponTransform.position + gripOffset - shoulder;
            const float along = glm::dot(fromShoulder, aimForward);
            const float outside = glm::dot(fromShoulder, fromShoulder) - reach * reach;
            const float inside = along * along - outside;
            if (outside > 0.0f && inside >= 0.0f)
            {
                const float back = along - std::sqrt(inside);
                if (back > 0.0f)
                {
                    m_weaponTransform.position -= aimForward * back;
                }
            }
        }

        const glm::vec3 toGrip = m_weaponTransform.position + gripOffset - shoulder;
        const float distance = glm::length(toGrip);
        if (distance > reach && distance > 1e-4f)
        {
            m_weaponTransform.position = shoulder + toGrip * (reach / distance) - gripOffset;
        }
    }

    // Recorded after every correction, so what is measured is where the hold ended up rather than
    // where it was asked to go.
    m_weaponHold = m_weaponTransform.position + m_weaponTransform.rotation * holdPoint;

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
        // The editor asks for a clip by name and says how far through it is. Nothing in the game
        // does: what plays there is decided by what the weapon is doing. But an animation nobody
        // can watch on demand is one nobody can make, and a clip that only plays when it happens to
        // be named after a movement the weapon already has is most of a clip system nobody can use.
        if (!m_weaponPose.clip.empty())
        {
            clip = m_weaponVisual.asset->FindClip(m_weaponPose.clip);
            clipProgress = glm::clamp(m_weaponPose.clipProgress, 0.0f, 1.0f);
        }
        else if (m_reloadRunning)
        {
            clip = m_weaponVisual.asset->FindClip("reload");
            clipProgress = glm::clamp(m_reloadPlay, 0.0f, 1.0f);
        }
        else if (m_weaponPose.holster < 1.0f)
        {
            // Going away, not coming out. Put back to front rather than a clip of its own when
            // there is none, because a weapon leaving the hands the way it arrived is far closer to
            // right than a weapon that simply vanishes.
            clip = m_weaponVisual.asset->FindClip("unequip");
            clipProgress = 1.0f - glm::clamp(m_weaponPose.holster, 0.0f, 1.0f);
            if (clip == nullptr)
            {
                clip = m_weaponVisual.asset->FindClip("equip");
                clipProgress = glm::clamp(m_weaponPose.holster, 0.0f, 1.0f);
            }
        }
        else if (draw < 1.0f)
        {
            clip = m_weaponVisual.asset->FindClip("equip");
            clipProgress = draw;
        }
        // Firing is the one that layers. The kick below is the whole weapon moving in a hold, which
        // every weapon does and nobody should have to author; a "fire" clip is the bolt cycling,
        // which only this weapon does and nothing here could guess at. So the clip runs on top of a
        // movement rather than instead of one, and it wins any part it names.
        if (clip == nullptr && m_weaponPose.kick > 0.001f)
        {
            clip = m_weaponVisual.asset->FindClip("fire");
            clipProgress = 1.0f - glm::clamp(m_weaponPose.kick, 0.0f, 1.0f);
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

    // Climbing takes both hands, whatever is in them. The carry offset above is measured from the
    // eye, and during a climb the body is nowhere near where that offset assumes it is, so the
    // weapon was left hanging in the air in front of a player who had both hands on a ledge. Done
    // here rather than with the arms, because the parts below are placed from this frame and the
    // arms are solved after they are.
    {
        glm::vec3 climbPoint{0.0f};
        glm::quat climbRotation{1.0f, 0.0f, 0.0f, 0.0f};
        float climbWeight = 0.0f;
        if (MantleCarry(state, climbPoint, climbRotation, climbWeight))
        {
            m_weaponTransform.position = glm::mix(m_weaponTransform.position, climbPoint, climbWeight);
            rotation = glm::slerp(rotation, climbRotation, climbWeight);
            m_weaponTransform.rotation = rotation;
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
    // A socket is where the palm closes, and the arm chain ends at the wrist, which is a hand's
    // length short of that. Driving the wrist onto the socket put the joint inside the weapon and
    // the fingers out the far side of it, and it charged the arm for a hand it does not have: the
    // support hand was called out of reach while the fingers would have closed comfortably.
    constexpr float kWristBack = 0.075f;
    const auto wristFor = [](const glm::vec3& socket, const glm::vec3& shoulder)
    {
        const glm::vec3 out = socket - shoulder;
        const float distance = glm::length(out);
        return distance > 1e-4f ? socket - out * (kWristBack / distance) : socket;
    };
    const glm::vec3 barrel = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 weaponUp = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 weaponRight = glm::cross(barrel, weaponUp);

    // Both from the model, rather than the support hand from the model and the trigger hand from
    // the origin. A model's origin is wherever the person who made it left it, and for anything
    // imported that is usually the middle of the weapon: the trigger hand ended up floating in the
    // air beside the receiver with nothing under it.
    glm::vec3 gripPoints[2] = {
        m_weaponTransform.position + rotation * m_weaponVisual.supportGrip, // left, the support hand
        m_weaponTransform.position + rotation * m_weaponVisual.triggerGrip  // right, the trigger hand
    };

    // A long weapon puts the handguard further out than the arm can reach, and an over-extended IK
    // chain draws the arm as a straight bar pointing at the target. Slide the support hand back
    // along the barrel until it is within reach instead: a shorter hold looks like a hold, a
    // straight arm looks broken.
    //
    // How far back it may slide is measured from the trigger grip, not from the model's origin. An
    // imported model's origin is the middle of its receiver, which on the carbine is a hand-span in
    // front of the grip: the slide stopped there with the hand still 21 cm short of anything, and
    // the arm drew as a straight bar. The floor is one hand's width ahead of the trigger hand,
    // which is as close as two hands can get without sharing a knuckle.
    {
        // Solved rather than stepped, for the same reason the aim clamp above is: two centimetres at
        // a time means the answer moves in two-centimetre jumps, and near the limit it takes a step
        // on one frame and not the next, which reads as the support hand shaking.
        const glm::vec3 leftShoulder = m_pose.GlobalPosition(m_rig.shoulder[kLeft]);
        constexpr float kHandsApart = 0.11f;
        const float nearest = glm::dot(m_weaponVisual.triggerGrip, glm::vec3(0.0f, 0.0f, 1.0f)) +
                              kHandsApart;
        const float span = armSpan + kWristBack;
        const glm::vec3 fromShoulder = gripPoints[kLeft] - leftShoulder;
        if (glm::dot(fromShoulder, fromShoulder) > span * span)
        {
            const float along = glm::dot(fromShoulder, barrel);
            const float inside =
                along * along - (glm::dot(fromShoulder, fromShoulder) - span * span);
            // How far back down the barrel the socket has to come, and how far back it may come:
            // one hand's width in front of the trigger hand, which is as close as two hands get.
            const float travel = glm::dot(gripPoints[kLeft] - m_weaponTransform.position, barrel) - nearest;
            const float wanted = inside >= 0.0f ? along - std::sqrt(inside) : travel;
            gripPoints[kLeft] -= barrel * glm::clamp(wanted, 0.0f, std::max(travel, 0.0f));
        }
    }

    // Crawling, the support hand lets go and reaches for the ground; only the trigger hand stays on
    // the weapon, which is how anyone actually low-crawls with a rifle.
    const int firstSide = flat ? kRight : kLeft;
    // The support hand also comes off the gun while the magazine is being changed.
    // Driven by the movement rather than by the reload, so the hand comes back to the handguard as
    // part of finishing rather than the instant the weapon becomes usable again.
    const bool supportHandFree =
        flat || (m_reloadRunning && m_reloadPlay > 0.10f && m_reloadPlay < 0.94f);

    // How far back onto the weapon the support hand has got since it let go.
    //
    // A hand on a weapon is placed rather than smoothed, because it is rigidly attached to it and
    // any smoothing shows up as the grip sliding off the gun whenever the player turns. That is
    // right while it is holding, and wrong on the frame it starts: the hand had been down at the
    // magazine well and was put on the handguard in one step, which is the snap at the end of every
    // reload. It goes back over a fifth of a second and is rigid after that.
    constexpr float kRejoinSeconds = 0.22f;
    if (supportHandFree)
    {
        m_supportRejoin = 0.0f;
    }
    else if (m_supportRejoin < 1.0f)
    {
        m_supportRejoin = std::min(m_supportRejoin + dt / kRejoinSeconds, 1.0f);
    }

    // The magazine change needs a hand to do it with, so the support hand goes to the magazine well
    // rather than hanging in mid air.
    //
    // This runs BEFORE the trigger hand, and the order is load-bearing. Writing a bone's global
    // transform rebuilds every bone after it in the skeleton, and the left arm comes before the
    // right, so placing the left hand last threw the right arm back onto its parent's pose: the
    // trigger hand dropped off the gun the moment a reload started.
    if (supportHandFree && !flat && m_reloadRunning)
    {
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[kLeft]);
        const glm::vec3 magazineWorld =
            m_weaponTransform.position + rotation * m_weaponVisual.magazineSeated;
        glm::vec3 target = magazineWorld + weaponRight * -0.03f;

        // Between letting the old magazine go and bringing the new one up, the hand has somewhere
        // to be, and it is not hovering under the weapon: it is down at the belt fetching one.
        // Without this the reload reads as a magazine teleporting into a hand that never moved.
        const float fetch = glm::smoothstep(0.26f, 0.42f, m_reloadPlay) *
                            (1.0f - glm::smoothstep(0.52f, 0.66f, m_reloadPlay));
        if (fetch > 0.001f)
        {
            const glm::vec3 belt = view.eyePosition - carryUp * (m_rig.height * 0.34f) -
                                   carryRight * (m_rig.height * 0.11f) +
                                   carryForward * (m_rig.height * 0.05f);
            target = glm::mix(target, belt, fetch);
        }

        FootState& hand = m_hands[static_cast<size_t>(kLeft)];
        hand.position = SmoothTowards(hand.position, target, m_config.weaponHandSmoothing, dt);

        const glm::vec3 elbowPole =
            glm::normalize(-carryUp * 1.0f - carryRight * 0.9f - carryForward * 0.25f);
        const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                                  m_rig.upperArmLength, m_rig.lowerArmLength);
        const glm::vec3 hinge =
            glm::cross(ik.jointPosition - shoulder, ik.endPosition - ik.jointPosition);
        m_pose.SetGlobal(m_skeleton, m_rig.upperArm[kLeft],
                         SegmentFrame(m_rig.upperArm[kLeft], shoulder, ik.jointPosition, hinge));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[kLeft],
                         SegmentFrame(m_rig.lowerArm[kLeft], ik.jointPosition, ik.endPosition, hinge));
        // The hand continues the forearm: its +Y runs from the wrist towards the fingers, which is
        // the convention the glove is drawn in.
        m_pose.SetGlobal(m_skeleton, m_rig.hand[kLeft],
                         SegmentFrame(m_rig.hand[kLeft], ik.endPosition,
                                      ik.endPosition + (ik.endPosition - ik.jointPosition), hinge));
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
        //
        // Except on the way back. The support hand spends a reload at the magazine well and a crawl
        // on the floor, and putting it back on the handguard in one step is what snapped.
        glm::vec3 wanted = wristFor(gripPoints[side], shoulder);
        if (side == kLeft && m_supportRejoin < 1.0f)
        {
            wanted = glm::mix(hand.position, wanted, glm::smoothstep(0.0f, 1.0f, m_supportRejoin));
        }
        hand.position = wanted;
        // And never further out than the arm goes. The slide above works from the pose as it was
        // before the spine and shoulders were written this frame, so the shoulder has usually moved
        // a couple of centimetres by the time the arm is solved, and a couple of centimetres is the
        // difference between a bent elbow and a locked one. This is the last word on it.
        {
            const glm::vec3 toWrist = hand.position - shoulder;
            const float outTo = glm::length(toWrist);
            if (outTo > armSpan && outTo > 1e-4f)
            {
                hand.position = shoulder + toWrist * (armSpan / outTo);
            }
        }
        hand.planted = true;

        // Elbows drop and swing outwards, away from the ribs. Aiming tucks them in, which is what
        // brings the silhouette down behind the sights.
        const glm::vec3 elbowPole =
            glm::normalize(-carryUp * 1.0f + carryRight * (sideSign * (0.85f - 0.45f * aim)) -
                           carryForward * 0.35f);
        const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                                  m_rig.upperArmLength, m_rig.lowerArmLength);

        const glm::vec3 hinge =
            glm::cross(ik.jointPosition - shoulder, ik.endPosition - ik.jointPosition);
        m_pose.SetGlobal(m_skeleton, m_rig.upperArm[side],
                         SegmentFrame(m_rig.upperArm[side], shoulder, ik.jointPosition, hinge));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[side],
                         SegmentFrame(m_rig.lowerArm[side], ik.jointPosition, ik.endPosition, hinge));
        m_pose.SetGlobal(m_skeleton, m_rig.hand[side],
                         SegmentFrame(m_rig.hand[side], ik.endPosition,
                                      ik.endPosition + (ik.endPosition - ik.jointPosition), hinge));
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
    //
    // The fade carries on past the end of the climb, because cutting straight back to the normal
    // arms put the hands somewhere else in a single frame, which is what snapped.
    const float mantleTarget = state.mantling ? 1.0f : 0.0f;
    const float fadeStep = dt / std::max(m_config.mantleArmFadeSeconds, 0.01f);
    m_mantleFade = mantleTarget > m_mantleFade ? std::min(m_mantleFade + fadeStep * 3.0f, 1.0f)
                                               : std::max(m_mantleFade - fadeStep, 0.0f);

    const bool holding = m_hasWeapon;
    if (m_flatness >= 0.02f)
    {
        UpdateCrawlArms(state, view, physics, dt, holding);
    }
    if (holding)
    {
        UpdateWeaponHold(state, view, physics, dt);
    }
    else if (m_hasHeldItem)
    {
        UpdateHeldItem(state, view, physics, dt);
    }

    // Last, and blended over whatever the arms were already doing. Solving the climb first and
    // returning meant the transition out of it was a cut; blending onto the pose the arms would
    // otherwise be in means there is nothing to cut between.
    if (m_mantleFade > 0.001f)
    {
        UpdateMantleArms(state, m_mantleFade);
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

void PlayerBody::UpdateHeldItem(const PlayerState& state, const PlayerView& view,
                                PhysicsWorld& physics, float dt)
{
    // Carried in the trigger hand, out in front and a little to the side, where you would hold
    // something you were about to use. The other arm is left alone: one hand is what carrying a
    // medical kit takes, and posing both would read as presenting it rather than holding it.
    const glm::vec3 forward = view.Forward();
    const glm::vec3 yawRight{std::cos(view.yaw), 0.0f, std::sin(view.yaw)};
    const glm::vec3 up = glm::cross(yawRight, forward);

    // Brought in against a wall, the same as a weapon is.
    const float crowded = 1.0f - m_wallClearance;
    glm::vec3 target = view.eyePosition + forward * glm::mix(0.52f, 0.26f, crowded) +
                       yawRight * 0.26f + up * -0.34f;

    // Out of the wall in front and off the floor below, the same as a weapon is. The soft pull-back
    // above only knows what is straight ahead; this knows where the item actually is, which is what
    // matters when the body is lying down and the hold hangs below an eye a few centimetres up.
    //
    // Not during a climb. The hand this is in has gone to the ledge, so an offset measured from the
    // eye describes nowhere, and correcting it only dragged the item down onto the block being
    // climbed.
    if (m_mantleFade <= 0.001f)
    {
        target = ClearOfWorld(physics, view.eyePosition, target, m_config.heldItemClearance);
    }

    const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[kRight]);

    // Pulled into reach before the arm is solved. An over-extended chain draws the arm as a
    // straight bar pointing at the target, and the hand ends up short of what it is meant to be
    // holding.
    const float reach = (m_rig.upperArmLength + m_rig.lowerArmLength) * 0.94f;
    const glm::vec3 toTarget = target - shoulder;
    const float distance = glm::length(toTarget);
    if (distance > reach && distance > 1e-4f)
    {
        target = shoulder + toTarget * (reach / distance);
    }

    // Placed, not smoothed. Something in your hand moves with you, so smoothing here is not weight,
    // it is lag: running dragged the item out of shot and left it trailing behind the camera.
    FootState& hand = m_hands[static_cast<size_t>(kRight)];
    hand.position = target;
    (void)dt;

    const glm::vec3 elbowPole = glm::normalize(-up * 1.0f + yawRight * 0.8f - forward * 0.3f);
    const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                              m_rig.upperArmLength, m_rig.lowerArmLength);

    const glm::quat rotation = glm::quat_cast(glm::mat3(glm::vec3(yawRight), up, -forward));
    const glm::vec3 hinge = glm::cross(ik.jointPosition - shoulder, ik.endPosition - ik.jointPosition);
    m_pose.SetGlobal(m_skeleton, m_rig.upperArm[kRight],
                     SegmentFrame(m_rig.upperArm[kRight], shoulder, ik.jointPosition, hinge));
    m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[kRight],
                     SegmentFrame(m_rig.lowerArm[kRight], ik.jointPosition, ik.endPosition, hinge));
    m_pose.SetGlobal(m_skeleton, m_rig.hand[kRight],
                     SegmentFrame(m_rig.hand[kRight], ik.endPosition,
                                  ik.endPosition + (ik.endPosition - ik.jointPosition), hinge));

    // Drawn in the hand rather than at the point the hand was aimed at. The two differ whenever the
    // arm cannot quite get there, and the difference is exactly the gap between the glove and the
    // thing it is supposed to be holding. Carried a little beyond the wrist, where the fingers are.
    const glm::vec3 palm = glm::normalize(ik.endPosition - ik.jointPosition + glm::vec3(1e-5f));
    // And then wherever the item itself says it sits. No rule about a bounding box can work out how
    // a keycard is held or which way up a flare goes, so each item carries its own offset and turn,
    // placed by eye in the editor and written into items.json.
    const glm::quat itemTurn = glm::quat(glm::radians(m_heldItemRotation));
    m_heldItemTransform.rotation = rotation * itemTurn;
    m_heldItemTransform.position = ik.endPosition + palm * (Ratio::kHand * m_rig.height * 0.45f) +
                                   m_heldItemTransform.rotation * m_heldItemOffset;

    // Except while climbing, when the hand it is in has gone to the ledge and the carry offset,
    // which is measured from the eye, no longer describes anywhere the body is.
    {
        glm::vec3 climbPoint{0.0f};
        glm::quat climbRotation{1.0f, 0.0f, 0.0f, 0.0f};
        float climbWeight = 0.0f;
        if (MantleCarry(state, climbPoint, climbRotation, climbWeight))
        {
            m_heldItemTransform.position =
                glm::mix(m_heldItemTransform.position, climbPoint, climbWeight);
            m_heldItemTransform.rotation =
                glm::slerp(m_heldItemTransform.rotation, climbRotation, climbWeight);
        }
    }
}

void PlayerBody::UpdateMantleArms(const PlayerState& state, float weight)
{
    // Both hands go to the lip of the ledge, take the weight while the body rises, and let go as it
    // comes over the top. The lip is where the wall face meets the top, which the climb recorded:
    // aiming at the landing spot instead put the hands most of a metre past the edge, out over
    // thin air, which is why nothing appeared to be grabbed.
    const float duration = std::max(state.mantleDuration, 0.05f);
    const float t = std::clamp(state.mantleTime / duration, 0.0f, 1.0f);

    const glm::vec3 travel = state.mantleTo - state.mantleFrom;
    const glm::vec3 flat{travel.x, 0.0f, travel.z};
    const glm::vec3 forward =
        glm::length(flat) > 1e-4f ? glm::normalize(flat) : glm::vec3(std::sin(m_bodyYaw), 0.0f, -std::cos(m_bodyYaw));
    const glm::vec3 right{-forward.z, 0.0f, forward.x};
    const glm::vec3 lip = state.mantleEdge;

    // Held for the pull, then released as the body comes over.
    const float release = glm::smoothstep(m_config.mantleReleaseAt, 1.0f, t);
    const float grip = (1.0f - release) * weight;

    // Where the trigger hand actually finished, as opposed to where it was aimed.
    glm::vec3 triggerWrist{0.0f};
    glm::vec3 triggerPalm{0.0f, 1.0f, 0.0f};
    bool solvedTrigger = false;

    for (int side = 0; side < 2; ++side)
    {
        const float sideSign = side == kLeft ? -1.0f : 1.0f;
        const glm::vec3 shoulder = m_pose.GlobalPosition(m_rig.shoulder[side]);

        const glm::vec3 held = lip + right * (sideSign * m_config.mantleGripSpread * m_rig.height);
        // Blended onto wherever the arms already are, which for a released hand is exactly where it
        // is going anyway. That is what makes the end of a climb a fade rather than a cut.
        const glm::vec3 natural = m_pose.GlobalPosition(m_rig.hand[side]);
        const glm::vec3 target = glm::mix(natural, held, grip);

        FootState& hand = m_hands[static_cast<size_t>(side)];
        hand.position = target;
        hand.planted = grip > 0.5f;

        // Elbows out and down while pulling, which is what taking your own weight looks like.
        const glm::vec3 elbowPole = glm::normalize(right * (sideSign * 1.0f) -
                                                   glm::vec3(0.0f, 0.7f, 0.0f) - forward * 0.3f);
        const TwoBoneIKResult ik = SolveTwoBoneIK(shoulder, hand.position, elbowPole,
                                                  m_rig.upperArmLength, m_rig.lowerArmLength);

        const glm::vec3 hinge =
            glm::cross(ik.jointPosition - shoulder, ik.endPosition - ik.jointPosition);
        m_pose.SetGlobal(m_skeleton, m_rig.upperArm[side],
                         SegmentFrame(m_rig.upperArm[side], shoulder, ik.jointPosition, hinge));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[side],
                         SegmentFrame(m_rig.lowerArm[side], ik.jointPosition, ik.endPosition, hinge));
        m_pose.SetGlobal(m_skeleton, m_rig.hand[side],
                         SegmentFrame(m_rig.hand[side], ik.endPosition,
                                      ik.endPosition + (ik.endPosition - ik.jointPosition), hinge));

        if (side == kRight)
        {
            triggerWrist = ik.endPosition;
            triggerPalm = glm::normalize(ik.endPosition - ik.jointPosition + glm::vec3(1e-5f));
            solvedTrigger = true;
        }
    }

    // Whatever is in the hands goes where the hands actually got to. The carry that ran earlier
    // predicts where the trigger hand is heading, because a weapon's parts are placed before the
    // arms are solved and they have to be placed somewhere. A prediction is not an arm that ran out
    // of reach on the way to a ledge, and the difference is the gap between the glove and the thing
    // it is supposed to be holding.
    // Blended by the same weight the arms are, not applied outright. Applied outright it held the
    // weapon exactly in the hand right up to the frame the fade ran out, and then let go of it in
    // one step: the correction stopping is what snapped, not the climb ending.
    if (solvedTrigger && weight > 0.001f)
    {
        if (m_hasHeldItem)
        {
            const glm::vec3 inHand =
                triggerWrist + triggerPalm * (Ratio::kHand * m_rig.height * 0.45f);
            m_heldItemTransform.position = glm::mix(m_heldItemTransform.position, inHand, weight);
        }
        if (m_hasWeapon)
        {
            ShiftWeapon((triggerWrist - m_weaponTransform.position) * weight);
        }
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

        // And out of the walls. A crawl reaches forward and out to the side, which in a vent is
        // straight into the sheet metal either side of you: the hands went through it and the arms
        // with them. Swept from the shoulder, so what is found is between the body and the reach
        // rather than something the hand is already past.
        {
            const glm::vec3 toTarget = target - shoulder;
            const float span = glm::length(toTarget);
            if (span > 0.02f)
            {
                const glm::vec3 direction = toTarget / span;
                const RayHit blocked =
                    physics.RayCast(shoulder, direction, span + m_config.crawlHandClearance);
                if (blocked)
                {
                    target = shoulder +
                             direction * std::max(blocked.distance - m_config.crawlHandClearance, 0.0f);
                }
            }
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

        const glm::vec3 hinge =
            glm::cross(ik.jointPosition - shoulder, ik.endPosition - ik.jointPosition);
        m_pose.SetGlobal(m_skeleton, m_rig.upperArm[side],
                         SegmentFrame(m_rig.upperArm[side], shoulder, ik.jointPosition, hinge));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerArm[side],
                         SegmentFrame(m_rig.lowerArm[side], ik.jointPosition, ik.endPosition, hinge));
        m_pose.SetGlobal(m_skeleton, m_rig.hand[side],
                         SegmentFrame(m_rig.hand[side], ik.endPosition,
                                      ik.endPosition + (ik.endPosition - ik.jointPosition), hinge));
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
    const float stride = playerConfig.StrideLength(speed, state.stance);
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
        //
        // Measured from where the character is standing, not from the hip. The hip moves: folding
        // the torso to crouch swings the pelvis back behind the camera, and feet anchored to it went
        // back with it, which left the legs folded double with the knees out in front and the shins
        // running backwards. Your feet are under where you are standing whatever your back is doing.
        const glm::vec3 stance = view.renderPosition +
                                 right * (sideSign * Ratio::kHipHalfWidth * m_rig.height);
        const glm::vec3 rest = stance - facing * (m_pose_blend.footBackRatio * legSpan) + spread;

        FootState& foot = m_feet[static_cast<size_t>(side)];
        const float cycle = glm::fract(m_stridePhase + (side == kLeft ? 0.0f : 0.5f));
        const bool inSwing = walking && cycle >= stanceShare;

        glm::vec3 target = rest;
        // Standing still, a foot stays where it was put.
        //
        // The stance above is worked out from where the character is and which way the body faces,
        // and both of those move while the player is standing perfectly still: leaning shifts the
        // body sideways and turning swings the shoulders, and the feet slid across the floor with
        // them. A foot on the ground is on the ground; the leg above it takes up the difference.
        // The plant is given up when the foot has to reach too far for it, which is what turning
        // far enough on the spot does, and a step re-establishes it anyway.
        if (!walking && m_flatness < 0.5f)
        {
            const glm::vec2 fromRest{foot.plant.x - rest.x, foot.plant.z - rest.z};
            if (!foot.holding || glm::length(fromRest) > legSpan * 0.42f)
            {
                foot.plant = rest;
                foot.holding = true;
            }
            target = glm::vec3(foot.plant.x, rest.y, foot.plant.z);
        }
        else
        {
            foot.holding = false;
        }
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
                // Lower the more the body is crouched. A folded leg has nowhere to pick the foot up
                // to, and lifting it a standing step's worth put the thigh through the horizontal
                // at the top of every stride.
                const float height =
                    m_config.stepHeight * glm::mix(1.0f, m_config.crouchStepScale, m_crouchness);
                lift = std::sin(t * glm::pi<float>()) * height;
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
        //
        // And a body lying down cannot step onto anything at all. Nearly half a metre of rise is a
        // kerb to somebody standing; to somebody on their belly beside a low wall it is the top of
        // the wall, and the trace found it and put the foot up there. Crawling along a wall had the
        // legs climbing it.
        const float base = view.renderPosition.y;
        const float rise = glm::mix(m_config.maxFootRise, 0.10f, m_flatness);
        const float groundY =
            hit ? std::clamp(hit.position.y, base - m_config.maxFootDrop, base + rise) : base;
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
        // A knee bends forward and down, and the more the leg is folded the more of it is down. A
        // purely forward pole sends the knee straight out in front of a crouching hip and ends up
        // above it, which is a leg bending the wrong way.
        const float fold =
            1.0f - std::clamp(glm::distance(hip, foot.position) / legSpan, 0.0f, 1.0f);
        const glm::vec3 uprightPole = glm::normalize(
            pelvisForward - glm::vec3(0.0f, m_config.kneeDropWhenFolded * fold, 0.0f) + glm::vec3(1e-4f));
        const glm::vec3 kneePole =
            glm::normalize(glm::mix(uprightPole, pronePole, m_flatness) + glm::vec3(1e-4f));

        // Limb lengths are constant in every stance; the knee bend is what absorbs a lowered hip.
        const TwoBoneIKResult ik = SolveTwoBoneIK(hip, foot.position, kneePole, m_rig.upperLegLength,
                                                  m_rig.lowerLegLength);

        // Write the solved chain straight into the pose's globals. Parent before child, because
        // setting a bone rebuilds everything after it from local transforms.
        const glm::vec3 hinge = glm::cross(ik.jointPosition - hip, ik.endPosition - ik.jointPosition);
        m_pose.SetGlobal(m_skeleton, m_rig.upperLeg[side],
                         SegmentFrame(m_rig.upperLeg[side], hip, ik.jointPosition, hinge));
        m_pose.SetGlobal(m_skeleton, m_rig.lowerLeg[side],
                         SegmentFrame(m_rig.lowerLeg[side], ik.jointPosition, ik.endPosition, hinge));
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
        // midpoint and aligns its local +Y with the bone.
        //
        // The spin about that axis comes from the bone the part hangs off, not from the body. Every
        // bone in this rig has its own front square to its own length, by construction: the torso
        // chain from the FK pose, every limb from the pole its IK was solved against. The body's
        // facing has no such guarantee, and a crouched thigh runs so nearly along a folded torso's
        // facing that what survived projecting the one out of the other was rounding: the drawn leg
        // spun a full turn about its own axis every stride.
        const glm::vec3 boneFront = -glm::vec3(m_pose.Global(part.from)[2]);
        transform->position = (a + b) * 0.5f;
        transform->rotation = AlignYWithRoll(delta, boneFront);
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

    // How close a wall is in front of the eye, as a fraction of how far a weapon reaches. A rifle is
    // most of a metre long and the capsule stops a third of a metre from a wall, so walking up to
    // one put the barrel through it. Measured here because it is one trace for everything the hands
    // might be holding.
    {
        const glm::vec3 forward = view.Forward();
        const float reach = m_config.wallCheckDistance;
        const RayHit hit = physics.RayCast(view.eyePosition, forward, reach);
        const float free = hit ? std::clamp(hit.distance / std::max(reach, 0.01f), 0.0f, 1.0f) : 1.0f;
        // Eased, so brushing past a doorframe does not jerk the weapon in and out.
        m_wallClearance = SmoothTowards(m_wallClearance, free, m_config.wallCheckSpeed, dt);
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
