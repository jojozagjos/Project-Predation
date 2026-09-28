#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

// The CIRRA support drone a player goes on in once their body is dead: a small tracked machine about
// knee high, with a camera on a mast, that they drive about the level to see and be heard through.
//
// It is a thing in the world, not a free camera. It has a body the level stops, it knows only what its
// camera sees, and it can be knocked about -- swatted, shot by a friend, flung by something big -- and
// knocked over, when it rights itself with a kick of its arm. Hurt enough, it shuts down: its picture
// goes, it reboots, and after a while it comes back. It cannot hurt anything.
//
// One of these is driven by its owner's machine, which simulates it; everybody else is shown where it is
// (see DroneProxy in PredationGame), and the host decides what happens to it.
class SupportDrone
{
public:
    // How it is driven this tick: which way the stick is pushed (x right, y forward), where the camera is
    // looking, and a press to right it when it is on its back or side.
    struct Controls
    {
        glm::vec2 move{0.0f};
        float lookYaw = 0.0f;
        float lookPitch = 0.0f;
        bool rightItself = false;
        // On its tracks: a hop, over a step or a body in the way.
        bool jump = false;
    };

    static constexpr float kMaxHealth = 100.0f;
    // Knocked out, how long it takes to come back, and how much of itself it comes back with.
    static constexpr float kRebootSeconds = 10.0f;
    static constexpr float kHealthAfterReboot = 60.0f;
    // How far its camera tips up or down on its mast.
    static constexpr float kMostPitchDegrees = 70.0f;

    bool Active() const { return m_active; }
    void Deploy(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const glm::vec3& at, float yaw);
    // Somebody else's, shown where their machine says it is: its body is moved, not simulated, so shots
    // and anything else looking for it still find it.
    void DeployProxy(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const glm::vec3& at,
                     const glm::quat& rotation);
    bool IsProxy() const { return m_proxy; }
    // Whether its camera head is drawn: not while its driver is looking out of it.
    void SetHeadHidden(Scene& scene, bool hidden);
    void Remove(Scene& scene, PhysicsWorld& physics);

    // Before the physics step: drives it, rights it, counts down a reboot.
    void Step(PhysicsWorld& physics, const Controls& controls, float dt);
    // After it: where the body is, onto the drawn pieces.
    void UpdateVisual(Scene& scene, PhysicsWorld& physics, float dt);

    // Struck by something: shoved, and hurt. Enough hurt and it shuts down.
    void Hit(PhysicsWorld& physics, const glm::vec3& impulse, float damage);

    // A proxy's pose and state, from its owner: eased towards over the next few frames by UpdateVisual.
    void SetPose(const glm::vec3& position, const glm::quat& rotation, float headYaw, float headPitch);
    void SetStatus(float health, float rebootLeft);

    glm::vec3 Position() const { return m_position; }
    glm::quat Rotation() const { return m_rotation; }
    BodyHandle Body() const { return m_body; }
    float Health() const { return m_health; }
    float RebootLeft() const { return m_rebootLeft; }
    float HeadYaw() const { return m_headYaw; }
    float HeadPitch() const { return m_headPitch; }
    bool Disabled() const { return m_rebootLeft > 0.0f; }
    // 0 just shut down, 1 back.
    float RebootProgress() const { return Disabled() ? 1.0f - m_rebootLeft / kRebootSeconds : 1.0f; }
    // On its back or its side, and not getting up by itself.
    bool Upended() const { return m_upendedFor > 0.8f; }

    // Where its camera is and how it looks, for the picture: the mast turns and tips with where its driver
    // looks, and the whole of it tips with the chassis -- knocked on its side, the picture is on its side.
    glm::vec3 Eye() const;
    glm::mat4 ViewMatrix() const;
    glm::vec3 LookDirection() const;

    // The light on its mast, which its driver switches.
    bool lightOn = false;

private:
    void Build(Scene& scene, MeshLibrary& meshes, const Transform& transform);
    glm::quat HeadRotation() const;

    bool m_active = false;
    bool m_proxy = false;
    BodyHandle m_body;
    // A proxy's pose as last sent, which it eases towards.
    glm::vec3 m_sentPosition{0.0f};
    glm::quat m_sentRotation{1.0f, 0.0f, 0.0f, 0.0f};
    float m_sentHeadYaw = 0.0f;
    float m_sentHeadPitch = 0.0f;
    std::vector<Entity> m_chassisParts;
    std::vector<glm::vec3> m_chassisRest; // where each piece sits on the chassis
    std::vector<Entity> m_headParts;
    std::vector<glm::vec3> m_headRest; // and on the camera head, from its pivot
    glm::vec3 m_position{0.0f};
    glm::quat m_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float m_health = kMaxHealth;
    float m_rebootLeft = 0.0f;
    float m_upendedFor = 0.0f;
    float m_rightingCooldown = 0.0f;
    // How fast its tracks are carrying it, forwards, and turning it.
    float m_trackSpeed = 0.0f;
    float m_trackTurn = 0.0f;
    float m_shovedFor = 0.0f;
    float m_rightingFor = 0.0f;
    float m_hopCooldown = 0.0f;
    bool m_onTracks = false;
    // Where its camera points, relative to the chassis: turned and tipped, eased after the driver's look.
    float m_headYaw = 0.0f;
    float m_headPitch = 0.0f;
    float m_lookYaw = 0.0f;
    float m_lookPitch = 0.0f;
};

} // namespace pred
