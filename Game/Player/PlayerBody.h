#pragma once

#include "Engine/Animation/Skeleton.h"
#include "Engine/Scene/Scene.h"
#include "Game/Player/PlayerTypes.h"

#include <glm/gtc/quaternion.hpp>
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
        float strideLength = 1.55f;   // metres per full two-step cycle
        float stepHeight = 0.14f;     // how far a swinging foot lifts
        float footPlantSmoothing = 22.0f;
        float hipSwayAmount = 0.035f;
        float hipBobAmount = 0.030f;

        // Posture
        float leanPerSpeed = 1.4f;    // degrees of forward lean per m/s
        float maxLean = 12.0f;
        float armSwingDegrees = 22.0f;
        float armRestDegrees = 8.0f;

        // One posture per stance, blended between. Limb lengths never change: lowering the hips and
        // letting the knees bend is what actually happens when someone crouches.
        struct StancePose
        {
            float pelvisRatio = 0.530f;   // hip height as a fraction of standing height
            float pelvisPitchDeg = 0.0f;  // tips the whole body; this is what lays it down for prone
            float spineLeanDeg = 0.0f;    // torso folds forward over the hips; this is the crouch
            float footBackRatio = 0.0f;   // feet behind the hips, as a fraction of total leg length
            float footSpread = 1.0f;      // stance width multiplier
            float armForwardDeg = 0.0f;
        };

        // Crouching folds at the hip and knee with the pelvis kept upright. Pitching the pelvis
        // instead threw the legs out behind and read as a ski jump rather than a squat. The feet
        // sit slightly *forward* of the hips, because squatting sends the hips back over the heels.
        StancePose stand{0.530f, 0.0f, 0.0f, 0.00f, 1.00f, 0.0f};
        StancePose crouch{0.375f, 0.0f, 32.0f, -0.12f, 1.20f, 10.0f};
        // Prone lays the pelvis flat so the spine continues horizontally. The feet go almost a full
        // leg length back so the legs lie out straight; leaving slack let the knees fold up into the
        // air, because once the pelvis is flat the knee's bend direction points at the sky.
        // Arms need no extra rotation here: with the pelvis flat, "hanging down" in body space
        // already points forward along the ground.
        StancePose prone{0.130f, 87.0f, 0.0f, 0.97f, 0.80f, 0.0f};

        float stanceBlendSpeed = 7.0f;

        // Hips follow the direction of travel; the torso twists back to stay aimed where the player
        // is looking. Locking the whole body to the camera makes it look welded to the view, because
        // nothing ever rotates relative to it.
        float hipTurnSpeedMoving = 9.0f;
        float hipTurnSpeedIdle = 6.0f;
        float maxTorsoTwistDegrees = 55.0f; // past this, the hips turn to catch up
        float turnInPlaceDegrees = 42.0f;   // how far a turn-in-place swings the hips

        // Presentation
        float responsiveness = 14.0f; // smoothing rate for posture changes
        // Eyes sit in front of the neck, not on top of it. Without this the body stands on the
        // camera's own axis and the chest fills the screen the moment you look down, hiding the
        // legs entirely.
        float eyeForwardOffset = 0.07f;
        bool hideHead = true; // the camera lives inside it
        bool visible = true;
    };

    void Build(Scene& scene, MeshLibrary& meshes, const PlayerConfig& playerConfig);
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
        glm::vec3 position{0.0f};
        glm::vec3 plantPosition{0.0f};
        bool planted = true;
    };

    void BuildSkeleton(const PlayerConfig& playerConfig);
    void BuildParts(Scene& scene, MeshLibrary& meshes);
    void UpdatePosture(const PlayerState& state, const PlayerView& view, const PlayerConfig& playerConfig,
                       float dt);
    void UpdateLegs(const PlayerState& state, const PlayerView& view, const PlayerConfig& playerConfig,
                    PhysicsWorld& physics, float dt);
    void PushToScene(Scene& scene);

    Skeleton m_skeleton;
    Pose m_pose;
    HumanoidRig m_rig;
    Config m_config;
    std::vector<Part> m_parts;
    std::array<FootState, 2> m_feet;

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
