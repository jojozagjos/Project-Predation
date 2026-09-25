#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/CreatureAnatomy.h"
#include "Game/Creature/CreatureBrain.h"
#include "Game/Creature/CreatureRagdoll.h"
#include "Game/Creature/CreatureRig.h"
#include "Game/Creature/CreatureSkin.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <string>
#include <vector>

namespace pred
{

class MeshLibrary;
class NavMesh;

// The creature in the world: a body that walks where its brain sends it.
//
// The brain decides; this carries it out. It follows a route over the navigation mesh, sliding along
// the surface so it can never step off the walkable area, and turns towards where it is going or what
// it is looking at.
//
// The body is the one its seed makes (CreatureAnatomy), drawn as one skin over a skeleton
// (CreatureSkin), posed every frame by procedural animation (CreatureRig): feet planted and stepping,
// spine bending, head tracking, jaw working, tail swinging. It dies -- or plays dead -- as a ragdoll
// (CreatureRagdoll), and gets back up out of one by easing from where it lies to standing. Rounds find it
// through a capsule on every bone, so a head is a head and a hand is a hand; a box round its torso is
// what the players bump into.
//
// What that body can do -- how fast it runs, how far it sees and hears, how hard it hits, how much
// killing it takes -- comes from it too, and is handed to the brain as it is made.
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

    // Shot, or otherwise hurt. `byPlayer` is -1 when nobody in particular. `hit` is what the round
    // found, when it was one of this creature's bones: a head takes more than a hand.
    void TakeDamage(float amount, int byPlayer, const glm::vec3& from, float time, BodyHandle hit = {});
    // How much a round that found `body` counts for: more for the head, less for a hand or a tail.
    float DamageScale(BodyHandle body) const;
    // Which bone a body is, -1 for none of them.
    int BoneOf(BodyHandle body) const;
    // Whether a physics body is part of this creature: the box round its torso, a bone's capsule, or a
    // piece of it lying on the floor.
    bool Owns(BodyHandle body) const;

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
    // Back to health, a little at a time, at its nest.
    void Heal(float amount)
    {
        if (Alive())
        {
            m_health = std::min(m_health + amount, m_maxHealth);
        }
    }
    float Speed() const { return m_speed; }
    const CreatureAnatomy& Anatomy() const { return m_anatomy; }
    const CreatureCapabilities& Capabilities() const { return m_caps; }
    const CreatureSkin& Skin() const { return *m_skin; }
    const CreatureRig& Rig() const { return m_rig; }
    std::vector<glm::vec3> TakeFootfalls() { return m_rig.TakeFootfalls(); }
    bool Ragdolled() const { return m_ragdoll.Active(); }
    // Where a bone is in the world right now, drawn or lying.
    glm::mat4 BoneWorld(int bone) const;

    // What the body is doing beyond walking, for everybody else to be shown.
    struct Action
    {
        RigAction kind = RigAction::None;
        float phase = 0.0f;
        int side = 1;
        glm::vec3 target{0.0f};
    };
    const Action& CurrentAction() const { return m_action; }
    // How far through a jump it is, 0 on the ground.
    float Airborne() const { return m_airborne; }
    // What its head is turned towards, when anything.
    bool Looking() const { return m_look; }
    const glm::vec3& LookingAt() const { return m_lookAt; }

    // Set from outside, for a creature this machine only shows.
    void SetShownState(const glm::vec3& position, float yaw, float speed, float windup, bool alive,
                       bool down = false, float crouch = 0.0f);
    // And what it is doing with its limbs and head, and whether it is in the air.
    void SetShownAction(const Action& action, float airborne, bool look, const glm::vec3& lookAt);
    // What it has in mind: the brain's own answer where the brain runs, the host's where it is only shown.
    Behavior Doing() const { return m_hasReceived ? m_shownBehavior : m_brain.Current(); }
    void SetShownBehavior(Behavior behavior) { m_shownBehavior = behavior; }

    // Lying as if dead -- really dead, or playing it. The two look the same from outside, which is
    // the point of playing it.
    bool Down() const { return !Alive() || m_down; }
    // Where it is clinging: the floor, a wall on its way up, the ceiling, or falling from it.
    enum class Cling : uint8_t
    {
        Floor,
        Wall,
        Ceiling,
        Dropping
    };
    Cling Clinging() const { return m_cling; }
    // The floor under it, on the navigation mesh: where it is when it is on the floor, and the floor it
    // is over when it is up a wall or across a ceiling. What its brain plans from.
    const glm::vec3& Anchor() const { return m_anchor; }
    // The wall it is on, as the way the wall faces, in the same sense as a yaw.
    float WallYaw() const { return std::atan2(m_wallNormal.x, -m_wallNormal.z); }
    // For a creature this machine only shows: what the host says it is clinging to.
    void SetShownCling(Cling cling, float wallYaw);
    // The turn from the world to its body as drawn: its heading, about the up of whatever it is on.
    glm::quat Orientation() const;
    // For looking at climbing and for its tests: up onto the ceiling whatever its brain wants (1), down off
    // it (0), or as its brain decides (-1).
    void SetClimbOverride(int climb) { m_climbOverride = climb; }

    // How low it is carrying itself, 0 to 1, and how flat to its belly, 0 to 1, to fit a crawlspace.
    float Crouch() const { return m_crouchTarget; }
    float Squeeze() const { return m_squeeze; }

    // Which creature this is on the wire. The host numbers them as it makes them; a client's copy
    // carries the number it was sent.
    uint8_t NetId() const { return m_netId; }
    void SetNetId(uint8_t id) { m_netId = id; }
    // The walkable surface it plans over. Given again when the level changes shape and the mesh is
    // rebuilt into a different object.
    void SetNav(const NavMesh* nav) { m_nav = nav; }

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
    void Move(const CreatureIntent& asked, const std::vector<glm::vec3>& others, float dt);
    // Up a wall, across a ceiling, or dropping from it, for one that climbs.
    void MoveClinging(const CreatureIntent& intent, float dt);
    // The nearest wall it can go up from here to a ceiling it can hang from; fills m_wall*.
    bool FindWall();
    // Letting go of the wall or the ceiling, to land on the floor at `onto` or as near it as it can.
    void StartDrop(const glm::vec3& onto);
    // How high the ceiling is over a point on the floor, or 0 where there is none within six metres.
    float CeilingAbove(const glm::vec3& floor) const;
    // Its heading about the up of whatever it is on, for the rig.
    float SurfaceYaw() const;
    void BuildVisual(MeshLibrary& meshes);
    // The traits it was made with, with what its body decides written over them: speeds, senses, reach.
    static CreatureTraits WithBody(CreatureTraits traits, const CreatureCapabilities& caps);
    void SyncBody(float dt);
    // Falling limp, and getting up again.
    void GoLimp();
    void GetUp();
    // The capsules rounds find, onto the bones, or out of the way when it is lying as a ragdoll.
    void PlaceHitboxes(const std::vector<glm::mat4>& local, bool away);

    Scene& m_scene;
    PhysicsWorld& m_physics;
    MeshLibrary* m_meshes = nullptr;
    const NavMesh* m_nav = nullptr;
    // Declared before the brain, which is made from what these decide.
    CreatureAnatomy m_anatomy;
    CreatureCapabilities m_caps;
    CreatureAnatomy::RestPose m_rest;
    CreatureBrain m_brain;

    glm::vec3 m_position{0.0f};
    float m_yaw = 0.0f;
    // How fast it is turning, radians a second: a body turns up to speed and slows out of a turn,
    // rather than snapping between turning flat out and not at all.
    float m_yawRate = 0.0f;
    float m_speed = 0.0f;
    glm::vec3 m_velocity{0.0f}; // as drawn: how fast the body is actually moving, for the feet
    glm::vec3 m_lastShown{0.0f};
    bool m_shownOnce = false;
    float m_health = 160.0f;
    float m_maxHealth = 160.0f;
    float m_windup = 0.0f;
    bool m_down = false;
    float m_crouch = 0.0f;
    float m_crouchTarget = 0.0f;
    float m_shownTime = 0.0f;
    float m_time = 0.0f;
    Action m_action;
    float m_airborne = 0.0f;
    bool m_look = false;
    glm::vec3 m_lookAt{0.0f};

    // The route and when it was worked out. Replanned when the destination moves or on a timer,
    // not every tick: a route across the map is cheap, but not free, and it does not change much
    // between frames.
    std::vector<glm::vec3> m_route;
    glm::vec3 m_routeGoal{0.0f};
    float m_routeAge = 1.0e9f;
    // Which corners of the route are the take-off of a jump, and whether the route gets all the way.
    std::vector<uint8_t> m_routeJumps;
    bool m_routeReached = true;
    // A jump in progress: from where, to where, how far through, how long it takes.
    bool m_jumping = false;
    glm::vec3 m_jumpFrom{0.0f};
    glm::vec3 m_jumpTo{0.0f};
    float m_jumpTime = 0.0f;
    float m_jumpDuration = 0.5f;
    // How long its calls and its blows against a door go on being shown.
    float m_roarUntil = -1.0f;
    float m_bashStarted = -1.0f;
    // The jumps its body can make, as the navigation mesh names them, and crawlspaces when it fits them.
    uint16_t m_jumps = 0;
    // Climbing. What it is on, the floor it is over, and the wall it went up: where at the foot of it,
    // which way the wall faces, how far the wall is from that foot, and how high the ceiling is there.
    Cling m_cling = Cling::Floor;
    glm::vec3 m_anchor{0.0f};
    glm::vec3 m_wallFoot{0.0f};
    glm::vec3 m_wallNormal{0.0f, 0.0f, 1.0f};
    float m_wallDepth = 0.4f;
    float m_wallTop = 0.0f;
    // Where it is on the wall, sideways from the foot it started at, in metres, and which way it is going
    // across it -- up, down, along or on a slant. It walks a wall, not only up it.
    float m_wallAlong = 0.0f;
    glm::vec3 m_wallHeading{0.0f, 1.0f, 0.0f};
    // Up at the top where there is no ceiling to go over onto: it goes back along to where there was.
    bool m_wallBack = false;
    bool m_haveWall = false;
    float m_wallSearchAt = 0.0f;
    // How far up the wall, or down through the drop, 0 to 1, and the drop's ends and length.
    float m_climbT = 0.0f;
    glm::vec3 m_dropFrom{0.0f};
    glm::vec3 m_dropTo{0.0f};
    float m_dropDuration = 0.5f;
    // The ceiling's height over where it is, checked now and then.
    float m_ceilingHeight = 0.0f;
    float m_ceilingCheck = 0.0f;
    // Its body turned to what it is on, eased towards `m_surfaceUp` rather than snapped to it.
    glm::quat m_surface{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 m_surfaceUp{0.0f, 1.0f, 0.0f};
    mutable float m_lastSurfaceYaw = 0.0f;
    int m_climbOverride = -1;
    // Whether it is in a crawlspace now, when it was last asked, and how flat it is lying to fit.
    bool m_inCrawlspace = false;
    float m_crawlCheck = 0.0f;
    float m_squeeze = 0.0f;

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
    Behavior m_shownBehavior = Behavior::Roam;

    // What the players bump into: a box round the torso. Rounds find the bones' capsules instead, which
    // stand out of it wherever there is a limb or a head.
    BodyHandle m_body;
    glm::vec3 m_bodyHalfExtents{0.2f};
    glm::vec3 m_bodyCentre{0.0f};
    struct Hitbox
    {
        BodyHandle body;
        int bone = -1;
        float halfLength = 0.0f;
    };
    std::vector<Hitbox> m_hitboxes;

    // The body: one skin, shared between every creature of the same seed, bent by the rig or lying as a
    // ragdoll.
    std::shared_ptr<const CreatureSkin> m_skin;
    CreatureRig m_rig;
    CreatureRagdoll m_ragdoll;
    Entity m_skinEntity;
    Entity m_glintEntity;
    MeshHandle m_skinMesh;
    MeshHandle m_glintMesh;
    glm::vec3 m_glow{0.0f};
    std::vector<MeshVertex> m_skinned;
    std::vector<glm::mat4> m_pose; // each bone in the creature's own frame, as last drawn
    // Getting up out of a heap: where each bone lay in the world, and how far it is through standing.
    std::vector<glm::mat4> m_fallen;
    float m_getUp = 0.0f;
    // The push the last hit gave it, for which way it falls.
    int m_lastHitBone = -1;
    glm::vec3 m_lastHitPush{0.0f};
};

} // namespace pred
