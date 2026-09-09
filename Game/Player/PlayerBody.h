#pragma once

#include "Engine/Animation/Skeleton.h"
#include "Engine/Scene/Scene.h"
#include "Game/Player/PlayerTypes.h"
#include "Game/Weapons/WeaponAppearance.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <vector>

namespace pred
{

class MeshLibrary;
class PhysicsWorld;
class DebugDraw;
struct PlayerView;

// Named joints of the humanoid rig, so gameplay code never looks bones up by string at runtime.
struct HumanoidRig
{
    BoneIndex pelvis = kInvalidBone;
    BoneIndex spine = kInvalidBone;
    BoneIndex chest = kInvalidBone;
    BoneIndex neck = kInvalidBone;
    BoneIndex head = kInvalidBone;

    std::array<BoneIndex, 2> shoulder{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> upperArm{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> lowerArm{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> hand{kInvalidBone, kInvalidBone};

    std::array<BoneIndex, 2> upperLeg{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> lowerLeg{kInvalidBone, kInvalidBone};
    std::array<BoneIndex, 2> foot{kInvalidBone, kInvalidBone};

    float upperLegLength = 0.44f;
    float lowerLegLength = 0.44f;
    float upperArmLength = 0.33f;
    float lowerArmLength = 0.26f;
    float ankleHeight = 0.07f;
    float height = 1.8f;
};

// The player's own body, visible when looking down.
//
// There is no authored animation and no rigged mesh yet, so the body is a skeleton driven entirely
// procedurally and drawn as one box per segment. That is enough to answer the question this
// milestone exists to answer, which is whether the camera feels attached to a physical body, and it
// builds the skeleton, pose and IK machinery the creatures will need later.
//
// The camera stays authoritative: the body is positioned to agree with the view rather than the
// view being driven by the head bone. Driving a first-person camera from an animated head is what
// produces motion sickness.
class PlayerBody
{
public:
    struct Config
    {
        // Gait
        float stepHeight = 0.14f; // how far a swinging foot lifts
        float footPlantSmoothing = 22.0f;
        float weaponHandSmoothing = 26.0f;
        // Where a weapon sits when it is carried rather than aimed, in the view frame. Close enough
        // to the axis to be on screen: at a 90 degree horizontal field of view anything at arm's
        // length below the chin is outside the frame entirely.
        float weaponReadyRight = 0.185f;
        float weaponReadyDown = -0.205f;
        float weaponReadyForward = 0.56f;
        float weaponAimForward = 0.42f;
        // How much of the view pitch a carried weapon follows. Following it fully swung the gun
        // round behind the player whenever they looked straight down. Aiming raises this to one,
        // because the sights have to line up with the view exactly.
        float weaponCarryPitchFollow = 0.90f;
        // The weapon lags a turn and then catches up, which is what gives it weight.
        float weaponSwayAmount = 0.55f;   // how far a turn drags the weapon behind the view
        float weaponSwayRecover = 11.0f;  // how fast it catches up again
        float weaponBreatheAmount = 0.018f; // the small movement of a weapon in someone's hands
        float weaponWalkAmount = 0.030f;  // how much walking swings it, on the stride's own phase
        // A swinging foot follows an already-smooth arc, so it tracks its target almost exactly. It
        // has to: any lag here lands the foot short of where the step was aimed, and it spends the
        // stance catching up, which is a visible skid at every touchdown.
        float footSwingSmoothing = 70.0f;
        // How much of the reachable leg length a step is allowed to use. Going right to the limit
        // leaves the knee locked straight at the end of every stance, which reads as stiff.
        float stepReachMargin = 0.99f;
        // How far above the player a foot may be planted. A stair step or a kerb, not the top of a
        // wall the player is standing next to.
        float maxFootRise = 0.45f;
        // And how far below. A foot whose target hangs over an edge would otherwise reach the floor
        // underneath, which puts the leg through whatever the player is standing on.
        float maxFootDrop = 0.40f;
        float hipSwayAmount = 0.035f;
        float hipBobAmount = 0.030f;

        // Posture
        float leanPerSpeed = 1.4f;    // degrees of forward lean per m/s
        float maxLean = 12.0f;
        float armSwingDegrees = 22.0f;
        // Splays the arms clear of the thighs. Anything under about 10 degrees and the upper arms
        // overlap the legs at this build's proportions.
        float armRestDegrees = 13.0f;

        // One posture per stance, blended between. Limb lengths never change: lowering the hips and
        // letting the knees bend is what actually happens when someone crouches.
        struct StancePose
        {
            float pelvisPitchDeg = 0.0f;  // tips the whole body; this is what lays it down for prone
            float spineLeanDeg = 0.0f;    // torso folds forward over the hips; this is the crouch
            float footBackRatio = 0.0f;   // feet behind the hips, as a fraction of total leg length
            float footSpread = 1.0f;      // stance width multiplier
            float armForwardDeg = 0.0f;
        };

        // Crouching folds at the hip and knee with the pelvis kept upright. Pitching the pelvis
        // instead threw the legs out behind and read as a ski jump rather than a squat. The feet
        // sit slightly *forward* of the hips, because squatting sends the hips back over the heels.
        StancePose stand{0.0f, 0.0f, 0.00f, 1.00f, 0.0f};
        StancePose crouch{0.0f, 32.0f, -0.12f, 1.20f, 10.0f};
        // Prone lays the pelvis flat so the spine continues horizontally. The feet go almost a full
        // leg length back so the legs lie out straight; leaving slack let the knees fold up into the
        // air, because once the pelvis is flat the knee's bend direction points at the sky.
        // Arms need no extra rotation here: with the pelvis flat, "hanging down" in body space
        // already points forward along the ground.
        StancePose prone{87.0f, 0.0f, 0.97f, 0.80f, 0.0f};

        float stanceBlendSpeed = 7.0f;

        // Hips follow the direction of travel; the torso twists back to stay aimed where the player
        // is looking. Locking the whole body to the camera makes it look welded to the view, because
        // nothing ever rotates relative to it.
        float hipTurnSpeedMoving = 9.0f;
        float hipTurnSpeedIdle = 6.0f;
        float maxTorsoTwistDegrees = 55.0f; // past this, the hips turn to catch up
        float turnInPlaceDegrees = 42.0f;   // how far a turn-in-place swings the hips

        // Crawling. Prone movement is driven by the arms: the hands reach forward, plant, and pull
        // the body along, with the legs pushing on the opposite beat.
        float crawlCycleLength = 1.1f; // metres of travel per full reach-and-pull cycle
        float crawlReach = 0.30f;      // how far the hands swing fore and aft
        float crawlLift = 0.10f;       // how far a hand lifts while swinging forward
        float crawlHandForward = 0.42f; // where the hands plant relative to the shoulders
        float crawlLegDraw = 0.40f;     // how far a knee swings out to the side as it is drawn up
        float crawlShoulderRollDegrees = 9.0f; // shoulders roll as each arm reaches and pulls
        // Prone turning is slow and deliberate: the body pivots towards where you are crawling.
        float proneTurnSpeed = 3.2f;
        // Look this far from the way the body is lying and it starts coming up onto its side. From
        // here to straight behind, how far over the body is tracks how far round you are looking,
        // so up on one shoulder is a position you can hold and aim from rather than a frame of a
        // transition. Straight behind is flat on your back.
        float proneRollOverDegrees = 95.0f;
        // How long a full roll from front to back takes. The rate is constant, so a quarter turn
        // takes a quarter as long, and it reads as a body turning over rather than a pose changing.
        float proneRollSeconds = 0.7f;

        // In the air. Legs tuck on the way up and reach on the way down; the arms come out either
        // way. Without any of this the legs simply stretch straight down towards a floor that is
        // metres away, which is what a jump looked like.
        float airBlendSpeed = 9.0f;
        float airRiseReference = 4.5f;  // vertical speed counted as fully rising or fully falling
        float airTuck = 0.30f;          // how far the feet come up under the hips when rising
        float airReach = 0.16f;         // how far they stretch down when falling
        float airArmDegrees = 46.0f;    // how far the arms come out
        float airLeanDegrees = 9.0f;    // torso tips back rising, forward falling

        // Leaning to peek round cover.
        float leanAngleDegrees = 16.0f;
        float leanOffset = 0.32f; // metres the eye shifts sideways at full lean
        float leanSpeed = 9.0f;

        // Presentation
        float responsiveness = 14.0f; // smoothing rate for posture changes
        // Where the camera sits relative to the head bone: eyes are in front of and above the skull
        // pivot. The body is anchored so the head lands exactly here, rather than being placed by a
        // guessed offset and hoping it lines up. Guessing meant the head drifted off the camera
        // whenever the hips turned away from the view, such as when strafing.
        // Only the vertical part. A horizontal offset here is measured along the view, so turning
        // round swung the whole body through twice that distance: the body slid out from under the
        // player exactly when they turned to look at it. The skull is drawn behind this point
        // instead, in the head bone's own frame, where turning the head moves the skull and not the
        // body, which is what actually happens.
        float eyeForwardOfHead = 0.0f;
        float eyeAboveHead = 0.085f;
        float skullBehindEye = 0.042f;
        bool hideHead = true; // the camera lives inside it
        bool visible = true;
    };

    void Build(Scene& scene, MeshLibrary& meshes, const PlayerConfig& playerConfig);
    // Skeleton only, with nothing to draw. Lets the posing be exercised in tests, which need no
    // renderer and where uploading meshes would mean standing up a GPU device.
    void BuildForSimulation(const PlayerConfig& playerConfig);
    void Destroy(Scene& scene);

    // Runs once per frame after the view is updated. Presentation only: it reads the simulation but
    // never writes to it.
    void Update(Scene& scene, const PlayerState& state, const PlayerView& view,
                const PlayerConfig& playerConfig, PhysicsWorld& physics, float dt);

    void DebugDraw(class DebugDraw& draw) const;

    Config& Tuning() { return m_config; }
    const Config& Tuning() const { return m_config; }
    const Skeleton& GetSkeleton() const { return m_skeleton; }
    const Pose& GetPose() const { return m_pose; }
    const HumanoidRig& Rig() const { return m_rig; }

    void SetVisible(Scene& scene, bool visible);

    // How the weapon is being held this frame. The body owns where it sits and which way it points;
    // the game owns what it is and what it is doing.
    struct WeaponPose
    {
        float aim = 0.0f;    // 0 held ready, 1 sighted
        float reload = 0.0f; // 0 at the start of a reload, 1 at the end; negative when not reloading
        float kick = 0.0f;   // 0 to 1, decaying after each shot
        float draw = 1.0f;   // 0 as a weapon is brought up, 1 once it is ready
        bool reloading = false;
    };

    // Puts a weapon in the character's hands, built from its data entry. Passing nothing takes it
    // away again.
    void SetWeapon(Scene& scene, MeshLibrary& meshes, const WeaponDefinition* definition);
    // The same, with nothing to draw. Lets the hold be exercised in tests, which have no renderer
    // and where uploading a mesh would mean standing up a GPU device.
    void SetWeaponForSimulation(const WeaponDefinition* definition);
    void SetWeaponPose(const WeaponPose& pose) { m_weaponPose = pose; }
    bool HasWeapon() const { return m_hasWeapon; }
    // Where a round would appear to leave the model, for the muzzle flash. Not where rounds are
    // actually traced from: that comes from the eye, so what is under the crosshair is what is hit.
    glm::vec3 MuzzlePoint() const;
    // Where the weapon is held. Exposed so a test can check the hand is actually on it.
    glm::vec3 WeaponOrigin() const { return m_weaponTransform.position; }
    // Which way the body is lying or facing. Exposed for tests; nothing in the game reads it.
    float DebugBodyYaw() const { return m_bodyYaw; }

private:
    // How a piece of gear is oriented. Limb bones are re-solved by IK and end up with an arbitrary
    // roll about their own axis, so anything that has to face forwards (knee pads, pouches, the
    // holster) is aligned to the body instead of to the bone it hangs from.
    enum class PartFrame : uint8_t
    {
        BodyYaw,  // upright, facing the way the body faces
        BoneFrame // inherits the bone's full orientation, used for the head
    };

    // One drawn piece. With `to` set it is a limb or torso link that stretches between two joints;
    // without it, a fixed-size piece of equipment attached at a joint.
    struct Part
    {
        BoneIndex from = kInvalidBone;
        BoneIndex to = kInvalidBone;
        glm::vec3 size{0.1f}; // stretched parts use x and z as width and depth
        glm::vec3 offset{0.0f};
        PartFrame frame = PartFrame::BodyYaw;
        Entity entity;
        MeshHandle mesh;
        float bindLength = 0.0f;
        bool hiddenInFirstPerson = false;
    };

    struct FootState
    {
        glm::vec3 position{0.0f};  // smoothed, and what actually gets drawn
        glm::vec3 plant{0.0f};     // the world point this foot is standing on
        glm::vec3 swingFrom{0.0f}; // where the current step started
        bool inSwing = false;
        bool planted = true;
    };

    void BuildSkeleton(const PlayerConfig& playerConfig);
    void BuildParts(Scene& scene, MeshLibrary& meshes);
    // Places the weapon and puts both hands on it. Returns false when there is nothing to hold, so
    // the caller can fall through to whatever the arms would otherwise be doing.
    // Places the weapon and puts the hands on it. Returns false when there is nothing to hold.
    bool UpdateWeaponHold(const PlayerView& view, float dt);
    void DestroyWeapon(Scene& scene);
    // The reach-and-pull crawl. With a weapon in hand only the support arm crawls; the other keeps
    // hold of the gun.
    void UpdateCrawlArms(const PlayerState& state, const PlayerView& view, PhysicsWorld& physics,
                         float dt, bool supportArmOnly);
    void UpdatePosture(const PlayerState& state, const PlayerView& view, const PlayerConfig& playerConfig,
                       float dt);
    // Arms are solved before legs on purpose: writing a global transform rebuilds every bone after
    // it, and the legs come last in the hierarchy, so doing it the other way round would undo the
    // leg IK every frame.
    void UpdateArms(const PlayerState& state, const PlayerView& view, PhysicsWorld& physics, float dt);
    void UpdateLegs(const PlayerState& state, const PlayerView& view, const PlayerConfig& playerConfig,
                    PhysicsWorld& physics, float dt);
    void PushToScene(Scene& scene);

    Skeleton m_skeleton;
    Pose m_pose;
    HumanoidRig m_rig;
    Config m_config;
    std::vector<Part> m_parts;

    // One entity per model part, so a reload can take the magazine out and an authored clip can
    // move anything it likes.
    std::vector<Entity> m_weaponParts;
    std::vector<Transform> m_weaponPartTransforms;
    Entity m_weaponEntity;  // the first part, and what HasWeapon asks about
    Entity m_muzzleFlashEntity;
    Transform m_weaponTransform;
    Transform m_muzzleFlashTransform;
    WeaponVisual m_weaponVisual;
    WeaponId m_weaponId = kInvalidWeapon;
    bool m_hasWeapon = false;
    WeaponPose m_weaponPose;
    // The weapon lags the view a little when the player turns, then catches up. Held on the weapon
    // rather than on the hands, so the grip never separates from the gun.
    glm::vec2 m_weaponSway{0.0f};
    float m_lastViewYaw = 0.0f;
    float m_lastViewPitch = 0.0f;
    glm::vec2 m_viewRate{0.0f}; // radians per second, so sway does not depend on the frame rate
    float m_swayClock = 0.0f;   // drives the breathing movement
    std::array<FootState, 2> m_feet;
    std::array<FootState, 2> m_hands; // same shape: a smoothed target and whether it is planted
    float m_flatness = 0.0f;          // 0 upright, 1 fully prone
    // Signed: negative rolls left, positive rolls right, so you go over the way you turned. The
    // magnitude is how far through the roll it is, and most of the pose only cares about that.
    float m_proneRoll = 0.0f;
    float m_proneRollT = 0.0f;        // 0 to 1 through the roll, advanced at a constant rate
    float m_proneRollTarget = 0.0f;   // where the roll is heading, from how far round you are looking
    float m_proneRollAngle = 0.0f;    // radians about the body length, signed; limbs are placed with it
    float m_proneRollSign = 1.0f;
    float m_proneRollAmount = 0.0f;  // how far over, ignoring which way
    float m_airborne = 0.0f;          // 0 on the ground, 1 fully in the air
    float m_airRise = 0.0f;           // +1 rising, -1 falling
    bool m_proneOnBack = false;       // the state the roll is heading towards

    glm::quat BodyRotation() const;

    glm::vec3 m_rootPosition{0.0f};
    float m_bodyYaw = 0.0f;
    float m_lean = 0.0f;
    // The live posture, smoothed towards the target stance so transitions animate rather than snap.
    Config::StancePose m_pose_blend;
    float m_stridePhase = 0.0f;
    float m_gaitWeight = 0.0f;
    bool m_built = false;
};

} // namespace pred
