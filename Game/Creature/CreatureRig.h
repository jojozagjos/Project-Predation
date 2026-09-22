#pragma once

#include "Game/Creature/CreatureAnatomy.h"
#include "Game/Creature/CreatureSkin.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace pred
{

// Something the body is doing with its limbs and head beyond walking about, and how far through it.
enum class RigAction : uint8_t
{
    None,
    Swipe,  // one arm drawn back and raked across whatever is in front of it
    Lunge,  // the whole body thrown forward, both arms reaching, jaws wide
    Bite,   // the head drawn back and snapped forward
    Grab,   // both hands closing on something and holding it
    Carry,  // holding something against its chest while it moves
    Roar,   // head up, jaw wide: calling to the others
    Bash    // shouldering into something in front of it, a door
};

const char* RigActionName(RigAction action);

// Everything the pose is made from, each frame.
struct RigInput
{
    glm::vec3 position{0.0f}; // its feet, in the world
    float yaw = 0.0f;         // radians; forward is -Z at 0, turning right as it grows
    glm::vec3 velocity{0.0f}; // metres a second, in the world
    float crouch = 0.0f;      // 0 to 1: how low it is creeping
    float windup = 0.0f;      // 0 to 1: gathering itself to strike
    RigAction action = RigAction::None;
    float actionPhase = 0.0f;   // 0 to 1 through the action
    int actionSide = 1;         // which arm, for a swipe: +1 its right
    glm::vec3 actionTarget{0.0f}; // where in the world the action is aimed
    bool look = false;
    glm::vec3 lookAt{0.0f};     // where in the world it is looking
    // How far through a jump it is, 0 on the ground; the body's own position already follows the arc.
    float airborne = 0.0f;
    float time = 0.0f;
    float dt = 0.0f;
    // The height of the ground under a point, looking down from `from` no further than `drop`. False
    // where there is none. Without it the ground is flat at the height of its feet.
    std::function<bool(const glm::vec3& from, float drop, float& height)> ground;
};

// The body's procedural animation: every bone placed every frame from what the creature is doing, with
// nothing authored.
//
// Feet are planted in the world and stay planted while the body moves over them, and a foot steps --
// lifted, carried and put down again, where the ground actually is -- only once the body has moved far
// enough from it, and only when the feet it walks in step with are down. That one rule makes a walk, a
// trot, a turn on the spot and a scramble up stairs without any of them being made separately. The spine
// bends into a turn and bobs with the stride, the head follows what it is looking at within what a neck
// can do, the tail swings on springs, the chest breathes and the jaw works; a hit makes it flinch.
class CreatureRig
{
public:
    void Init(const CreatureAnatomy& anatomy, const CreatureSkin& skin, const glm::vec3& position, float yaw);
    void Update(const RigInput& input);
    // Something hit it: the body flinches away from `direction` at that bone, and settles back.
    void Flinch(int bone, const glm::vec3& direction, float strength);

    // Each bone's frame now, in the creature's own frame (the one the skin was built in).
    const std::vector<glm::mat4>& Bones() const { return m_bones; }
    // From the creature's own frame to the world.
    const glm::mat4& Root() const { return m_root; }
    // The bones' skinning matrices: from their rest frames to where they are now.
    const std::vector<glm::mat4>& Skinning() const { return m_skinning; }

    struct Foot
    {
        glm::vec3 planted{0.0f}; // in the world
        bool stepping = false;
        float progress = 0.0f;
        float duration = 0.25f;
        glm::vec3 from{0.0f};
        glm::vec3 to{0.0f};
        float yaw = 0.0f;       // which way it pointed when put down
        float fromYaw = 0.0f;
        float toYaw = 0.0f;
        int group = 0;          // feet in the same group step together
        bool reaching = false;  // lifted off the floor by an action, not walking
    };
    const std::vector<Foot>& Feet() const { return m_feet; }
    // How far the head is turned from straight ahead, in radians, for the inspector.
    float HeadTurn() const { return m_headYaw; }

private:
    void PlaceFeet(const RigInput& input, float legScale);

    const CreatureAnatomy* m_anatomy = nullptr;
    const CreatureSkin* m_skin = nullptr;
    CreatureAnatomy::RestPose m_rest;
    std::vector<glm::mat4> m_bones;
    std::vector<glm::mat4> m_skinning;
    glm::mat4 m_root{1.0f};
    std::vector<Foot> m_feet;
    float m_lastYaw = 0.0f;
    float m_turnRate = 0.0f;  // radians a second, smoothed
    float m_bend = 0.0f;      // how far the spine is bent into a turn
    float m_stride = 0.0f;    // metres walked, for the bob
    float m_lift = 0.0f;      // the body's height above its feet, smoothed
    float m_pitch = 0.0f;     // tipped up a slope or a stair
    float m_headYaw = 0.0f;
    float m_headPitch = 0.0f;
    float m_twitchAt = 1.0f;
    float m_twitchUntil = 0.0f;
    glm::vec2 m_twitch{0.0f};
    uint32_t m_random = 1;
    // The tail, as points in the world swinging after the body.
    std::vector<glm::vec3> m_tailPoints;
    std::vector<glm::vec3> m_tailVelocity;
    // A flinch: how far the chest and head are knocked, and how fast that is changing.
    glm::vec3 m_flinch{0.0f};
    glm::vec3 m_flinchVelocity{0.0f};
    bool m_initialised = false;
};

} // namespace pred
