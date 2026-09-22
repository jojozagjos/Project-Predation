#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/CreatureAnatomy.h"
#include "Game/Creature/CreatureBrain.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace pred
{

class MeshLibrary;
class NavMesh;

// The creature in the world: a body that walks where its brain sends it.
//
// The brain decides; this carries it out. It follows a route over the navigation mesh, sliding along
// the surface so it can never step off the walkable area, turns towards where it is going or what it
// is looking at, and moves a physics box with it so rounds hit it and loose objects are pushed out of
// its way.
//
// The body is the one its seed makes (CreatureAnatomy): four, six or two legs, however long and heavy,
// with however many eyes. What that body can do -- how fast it runs, how far it sees and hears, how
// hard it hits, how much killing it takes -- comes from it too, and is handed to the brain as it is
// made. Its legs are placed each frame by solving each one to where its foot should be, so a crouch
// bends the knees and a stride is a stride of the legs it actually has.
//
// Only the authority runs one of these. Everybody else is shown where it is.
class Creature
{
public:
    // `healthScale` multiplies the health its body gives it, for tuning without touching the body.
    Creature(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const NavMesh* nav,
             const CreatureTraits& traits, const glm::vec3& spawn, float healthScale = 1.0f);
    ~Creature();
    Creature(const Creature&) = delete;
    Creature& operator=(const Creature&) = delete;

    // One tick. `senses` carries the players, the noises and the line-of-sight question; the
    // creature fills in what it knows about itself before its brain sees it.
    void Update(CreatureSenses senses, float time, float dt);
    // Drawn where the body is. Separate from Update so a client showing somebody else's creature can
    // set the state it was sent and draw that.
    void UpdateVisual(float dt);

    // Shot, or otherwise hurt. `byPlayer` is -1 when nobody in particular.
    void TakeDamage(float amount, int byPlayer, const glm::vec3& from, float time);

    // For the game, which applies strikes and runs the inspector.
    const CreatureBrain& Brain() const { return m_brain; }
    CreatureBrain& Brain() { return m_brain; }
    const glm::vec3& Position() const { return m_position; }
    float Yaw() const { return m_yaw; }
    glm::vec3 Forward() const;
    glm::vec3 Eye() const;
    BodyHandle Body() const { return m_body; }
    float Health() const { return m_health; }
    float MaxHealth() const { return m_maxHealth; }
    bool Alive() const { return m_health > 0.0f; }
    float Speed() const { return m_speed; }
    const CreatureAnatomy& Anatomy() const { return m_anatomy; }
    const CreatureCapabilities& Capabilities() const { return m_caps; }

    // Set from outside, for a creature this machine only shows.
    void SetShownState(const glm::vec3& position, float yaw, float speed, float windup, bool alive,
                       bool down = false, float crouch = 0.0f);

    // Lying as if dead -- really dead, or playing it. The two look the same from outside, which is
    // the point of playing it.
    bool Down() const { return !Alive() || m_down; }
    // How low it is carrying itself, 0 to 1.
    float Crouch() const { return m_crouchTarget; }

    // Which creature this is on the wire. The host numbers them as it makes them; a client's copy
    // carries the number it was sent.
    uint8_t NetId() const { return m_netId; }
    void SetNetId(uint8_t id) { m_netId = id; }

    // On a machine that is only shown it: the newest state the host sent, and, each frame, easing
    // the body towards it. A little ahead of it, too, by the speed it was going, because the state
    // is already a few hundredths of a second old when it arrives and a creature drawn where it was
    // is drawn behind where it is.
    void Receive(const glm::vec3& position, float yaw, float speed, float windup, float healthFraction,
                 bool alive, bool down = false, float crouch = 0.0f);
    void FollowReceived(float dt);

    // Where it is and how it is lying, with where its torso is drawn, for the console.
    std::string DescribePose() const;

    void Destroy();

private:
    void Move(const CreatureIntent& intent, const std::vector<glm::vec3>& others, float dt);
    void BuildVisual(MeshLibrary& meshes);
    // The traits it was made with, with what its body decides written over them: speeds, senses, reach.
    static CreatureTraits WithBody(CreatureTraits traits, const CreatureCapabilities& caps);
    void SyncBody(float dt);

    Scene& m_scene;
    PhysicsWorld& m_physics;
    const NavMesh* m_nav = nullptr;
    // Declared before the brain, which is made from what these decide.
    CreatureAnatomy m_anatomy;
    CreatureCapabilities m_caps;
    CreatureAnatomy::RestPose m_rest;
    CreatureBrain m_brain;
    // Radians of stride per metre covered: one full step cycle is two strides of the legs it has.
    float m_strideRate = 3.6f;

    glm::vec3 m_position{0.0f};
    float m_yaw = 0.0f;
    float m_speed = 0.0f;
    float m_health = 160.0f;
    float m_maxHealth = 160.0f;
    float m_windup = 0.0f;
    // How far the legs have gone through their cycle, advanced by distance rather than time so a
    // creature that is not moving is not treading water.
    float m_stride = 0.0f;
    // How far over it is, 0 standing to 1 lying on its side, eased both ways: down when it dies or
    // plays dead, back up when it gets up. On the visuals' own clock rather than the brain's, which
    // only runs where the brain does -- on a machine that is only shown the creature it never moves,
    // and a death roll timed against it never played.
    float m_collapse = 0.0f;
    float m_fallSide = 1.0f; // which side it goes over onto: 1 its left, -1 its right
    bool m_down = false;
    float m_crouch = 0.0f;
    float m_crouchTarget = 0.0f;
    float m_shownTime = 0.0f;
    float m_time = 0.0f;

    // The route and when it was worked out. Replanned when the destination moves or on a timer,
    // not every tick: a route across the map is cheap, but not free, and it does not change much
    // between frames.
    std::vector<glm::vec3> m_route;
    glm::vec3 m_routeGoal{0.0f};
    float m_routeAge = 1.0e9f;

    uint8_t m_netId = 0;
    struct Received
    {
        glm::vec3 position{0.0f};
        float yaw = 0.0f;
        float speed = 0.0f;
        float windup = 0.0f;
        bool alive = true;
        bool down = false;
        float crouch = 0.0f;
    };
    Received m_received;
    float m_receivedAge = 0.0f;
    bool m_hasReceived = false;
    bool m_followedOnce = false;

    BodyHandle m_body;
    enum class PieceKind : uint8_t
    {
        Body,     // fixed to the body
        Front,    // the head end: moves back and up when it gathers itself to strike
        LegUpper, // hip to knee
        LegLower, // knee to ankle
        LegFoot   // ankle to toe
    };
    struct Piece
    {
        Entity entity;
        PieceKind kind = PieceKind::Body;
        // Where it sits on the body, in the body's own frame. Legs are placed each frame instead.
        glm::mat4 local{1.0f};
        int leg = -1; // which of the rest pose's legs
        // How brightly it glows when the creature is up and alive: the eyes, which go out as it falls.
        glm::vec3 glow{0.0f};
    };
    std::vector<Piece> m_pieces;
};

} // namespace pred
