#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Entity.h"
#include "Game/Creature/CreatureAnatomy.h"
#include "Game/Creature/CreatureSkin.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pred
{

class Scene;
class MeshLibrary;

// The giants (you, 2026-11-13; rebuilt 2026-11-15): huge -- on land a strider tens of metres tall, stepping between the buildings
// over their roofs; in the sea a leviathan longer than a building, the enemy on most water worlds. Each its own from its seed, so no
// two are alike and nobody knows what the next will be.
//
// The strider is a hull hung high between three to eight long jointed legs, a head low at its front, strands hanging under it on
// some -- each its own shape -- one skinned mesh, ball joints at its hips and knees, its feet put down on the ground. The leviathan
// is a body of its own (one
// surface, Sdf and SurfaceNets, skinned to a chain of bones down its spine): a sea serpent, a whale, an armoured fish, a ray or a
// squid-like thing trailing its arms -- each swimming as its kind does.
//
// Never afraid of the people it finds: to a thing that size they are toys. It comes to look, noses at them, and picks them up -- the
// strider lifts them to its eyes, then eats them or throws them away; the leviathan takes them in its jaws and drags them down -- and
// lets go only for a reason: struggled out of, shot enough, done with them, or dead. Fair: it sees what it can see from where its eyes
// are, in the open; under a roof, in the shuttle, or out of the water from a swimmer, they are beyond it. Its blows are seen coming.
// And it can be killed: a great deal of shooting, the head the place to aim.
//
// Not a Creature: too big for the navigation, the doors, the ragdolls. Its own mind, run on the host and shown everywhere else from
// what the host sends (GiantSnapshot).
enum class GiantKind : uint8_t
{
    Walker,  // on land: a strider
    Swimmer, // in the sea: a leviathan
};

// What a giant asks of the world it is in.
struct GiantWorld
{
    std::function<float(float, float)> ground;                      // how high the ground (or the sea's floor) is at (x, z)
    std::function<float(const glm::vec3&, float&)> water;           // how deep the water is at a point, and its surface (0: none)
    std::function<bool(const glm::vec3&)> roofed;                   // under a roof: in a building, beyond it
    std::function<bool(const glm::vec3&, const glm::vec3&)> clear;  // whether nothing solid is between two points
    std::function<float(float, float)> builtUp;                     // how high what is built at (x, z) stands (0: nothing)
    std::function<float(float, float)> roof;                        // the top of a building's roof at (x, z), in the world (-1e9: none there)
    glm::vec2 middle{0.0f};                                          // the site's middle
    float reach = 260.0f;                                            // how far from it the giant goes
};

// Somebody, as a giant knows them.
struct GiantSensed
{
    int id = 0;
    glm::vec3 feet{0.0f};
    glm::vec3 velocity{0.0f};
    float height = 1.8f;
    float light = 1.0f; // how lit they are, 0 to 1
    bool alive = true;
    bool roofed = false; // under a roof (or in the shuttle): out of its reach and its sight
    bool inWater = false;
    glm::vec3 look{0.0f, 0.0f, -1.0f}; // which way they are looking
};

class Giant
{
public:
    // What it is doing, as sent to every machine.
    enum class Mode : uint8_t
    {
        Roam,     // going about
        Notice,   // has seen something: stopped, its head turned to it, making it out
        Approach, // going to it
        Hunt,     // over them, sizing them up, closing in -- hunting them, not playing (you, 2026-11-16)
        Strike,   // a reach or a rush: wound up (seen coming), then in
        Linger,   // they went where it cannot follow: it waits about the place
        Hold,     // it has somebody: lifted to its eyes and eaten or thrown (the strider), dragged down in its jaws (the leviathan)
        Dead,     // killed: fallen (the strider), floating up belly first (the leviathan)
        Count
    };
    // A leviathan's kind of body, and how it swims.
    enum class Form : uint8_t
    {
        // Serpents and things with a serpent's look (you, 2026-11-16), segmented, swimming in waves from side to side, each kind its own:
        Serpent,  // a sea serpent, a crest down its back on some
        Eel,      // flattened side to side, a fin the length of its back and under its tail, waving wider
        Armoured, // thick, its rings overlapping plates, ridged along its back, stiffer, sinking dead
        Frilled,  // a frill of webbed spines round its neck
        Barbed,   // a barb standing up from every ring of its back
        Paddled,  // thicker, four flippers and a fluke at its tail: a sea reptile's, long and sinuous
        Dragon,   // a sea dragon: long whiskers from its snout, a mane of spines down its neck, fins at its shoulders
        Count
    };

    Giant(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, GiantKind kind, uint32_t seed, const glm::vec3& at, float yaw);
    ~Giant();
    Giant(const Giant&) = delete;
    Giant& operator=(const Giant&) = delete;

    // The host: what it makes of everybody and what it does about it.
    void Think(const GiantWorld& world, const std::vector<GiantSensed>& people, float time, float dt);
    // Every machine: its body carried and posed from where it is and what it is doing.
    void Animate(const GiantWorld& world, float time, float dt);
    // A client: where the host says it is.
    void SetShown(const glm::vec3& at, float yaw, Mode mode, const glm::vec3& lookAt, float strike, const glm::vec3& strikeAt);

    // Its parts, for rounds. A round into one is felt -- not feared -- and hurts it (the head most); enough of them kill it. Hurt
    // enough while it holds somebody, it lets them go.
    bool Owns(BodyHandle body) const;
    void TakeDamage(float amount, int byPlayer, float time, BodyHandle hit = BodyHandle{});
    float Health() const { return m_health; }
    float MaxHealth() const { return m_maxHealth; }
    bool Dead() const { return m_mode == Mode::Dead; }
    // Whom it holds (-1 nobody), and where they are held (in its hand or its jaws).
    int Holding() const { return m_holding; }
    const glm::vec3& HoldPoint() const { return m_holdPoint; }
    // Let them go (struggled free), and why.
    void Drop(float time, const char* why = "they struggled free");

    // What has happened since last asked, for the game: a blow that landed on somebody (or a grab, or a bite on somebody held -- and
    // whether it lets them go with it), its feet coming down, its call.
    struct Blow
    {
        int player = -1;
        float damage = 0.0f;
        glm::vec3 at{0.0f};
        glm::vec3 push{0.0f};
        bool grab = false;    // taken hold of
        bool release = false; // let go of with this (thrown, or done with)
    };
    std::vector<Blow> TakeBlows();
    std::vector<glm::vec3> TakeFootfalls();
    bool TakeCall();
    // A leviathan's rush beginning, since last asked: heard (you, 2026-11-16: it made no sound when it attacked).
    bool TakeRush() { return std::exchange(m_rushHeard, false); }

    GiantKind Kind() const { return m_kind; }
    uint32_t Seed() const { return m_seed; }
    const glm::vec3& Position() const { return m_position; }
    float Yaw() const { return m_yaw; }
    Mode CurrentMode() const { return m_mode; }
    const glm::vec3& LookAt() const { return m_lookAt; }
    float StrikePhase() const { return m_strike; }
    const glm::vec3& StrikeAt() const { return m_strikeAt; }
    const glm::vec3& Head() const { return m_head; }
    // A leviathan's body as something solid: the way out of it for a point inside it, padded by `pad` (nought: not inside).
    glm::vec3 OutOfBody(const glm::vec3& point, float pad) const;
    // For testing (giant_do): put straight into a mode, after somebody (the host's).
    void Force(Mode mode, int target, const glm::vec3& at, float time);
    // The way its head points (a leviathan: along its spine's front; a strider: the way it faces).
    glm::vec3 HeadForward() const;
    // Whether it has strands under it to take people with (a strider with none stamps on them instead).
    bool HasStrands() const { return !m_strands.empty(); }
    // Where a strider's feet are (none for a leviathan).
    std::vector<glm::vec3> Feet() const
    {
        std::vector<glm::vec3> feet;
        for (const Leg& leg : m_legs)
        {
            feet.push_back(leg.foot);
        }
        return feet;
    }
    // How far open a leviathan's jaw is, 0 shut to 1 wide.
    float Gape() const { return m_gape; }
    float Size() const { return m_size; }
    const std::string& Goal() const { return m_goal; }
    std::string Describe() const;
    // Who it has its mind on (-1 nobody); how far it has made each out (0 to 1.5); how roughly it is hunting (0 to 1).
    int Target() const { return m_target; }
    const std::vector<std::pair<int, float>>& Awareness() const { return m_aware; }
    float Annoyed() const { return m_annoyed; }

    static const char* ModeName(Mode mode);
    static const char* FormName(Form form);

private:
    struct BoneBox
    {
        int bone = -1;        // the skin's bone (the strider), or the spine's (the leviathan; -1 its head)
        float halfLength = 0.0f;
        BodyHandle body;
        bool head = false;    // a round here does most
    };

    void BuildWalker(MeshLibrary& meshes);
    void BuildSwimmer(MeshLibrary& meshes);
    void MakeVisual(MeshLibrary& meshes, const MeshData& skin, const MeshData* glow, const MeshData* eyes, bool eyesGlow, bool dynamicEyes);
    void AnimateWalker(const GiantWorld& world, float time, float dt);
    glm::vec3 FootFor(const GiantWorld& world, size_t leg, const glm::vec3& ahead) const;
    void AnimateSwimmer(const GiantWorld& world, float time, float dt);

    // Towards `goal`; given `faceTo` (a strider), facing that as it goes -- sideways, round somebody, its eyes on them.
    void Steer(const GiantWorld& world, const glm::vec3& goal, float speed, float dt, const glm::vec3* faceTo = nullptr);
    // Which way round somebody it is circling (Approach, Hunt), and till when.
    float m_circleSide = 1.0f;
    float m_circleUntil = 0.0f;
    // Waiting about where they went (Linger), since when.
    float m_waitSince = 0.0f;
    // How many times it has thrown somebody away: the next time, it eats them.
    int m_slams = 0;
    // A leviathan's way of coming at somebody (Hunt): 0 off into the murk, 1 round to behind them out there, 2 in; and since when.
    int m_flankPhase = 0;
    float m_flankSince = 0.0f;
    // Getting nowhere (Steer): how near it has come, since when, and the way round it is taking instead, till when.
    float m_steerClock = 0.0f;
    float m_bestDistance = 1.0e9f;
    float m_progressSince = 0.0f;
    glm::vec3 m_detour{0.0f};
    float m_detourUntil = 0.0f;
    bool Sees(const GiantWorld& world, const GiantSensed& who) const;
    void Enter(Mode mode, float time, const char* goal);

    Scene& m_scene;
    MeshLibrary& m_meshes;
    PhysicsWorld& m_physics;
    GiantKind m_kind;
    uint32_t m_seed;
    CreatureAnatomy m_look; // its body (the strider's), its colours, its markings, its lights (a creature's of its family)

    glm::vec3 m_position{0.0f}; // walker: under its body on the ground; swimmer: the front of it
    float m_yaw = 0.0f;
    glm::vec3 m_velocity{0.0f};
    float m_size = 24.0f;       // walker: how tall; swimmer: how long
    float m_speed = 3.0f;       // its walk
    glm::vec3 m_head{0.0f};
    glm::vec3 m_lookAt{0.0f};

    // The mind.
    Mode m_mode = Mode::Roam;
    std::string m_goal = "going about";
    float m_modeSince = 0.0f;
    glm::vec3 m_roamTo{0.0f};
    // Given up waiting where somebody went in (Linger): it goes off, and comes back later to see whether they have come out -- where,
    // and when (-1: not coming back).
    glm::vec3 m_comeBackTo{0.0f};
    float m_comeBackAt = -1.0f;
    bool m_haveRoam = false;
    int m_target = -1;
    glm::vec3 m_targetAt{0.0f};
    float m_lastSeen = -100.0f;
    std::vector<std::pair<int, float>> m_aware; // who, how far made out
    float m_annoyed = 0.0f;                     // shot at: rougher for a while
    float m_lastHurt = -1000.0f;                // when it was last shot: hurt, it never stands still to be looked at
    float m_hunger = 0.35f;                     // 0 fed to 1 starving: eats what it holds rather than throwing it
    float m_nextStrikeAt = 0.0f;
    float m_strike = 0.0f;                      // 0 none; 0..1 through a blow
    glm::vec3 m_strikeAt{0.0f};
    bool m_struck = false;                      // this blow has landed
    float m_huntUntil = 0.0f;
    float m_health = 4000.0f;
    float m_maxHealth = 4000.0f;
    float m_deadSince = -1.0f;
    // Holding somebody: whom, since when, where, what it means to do with them, and how much it has been hurt since it took them.
    int m_holding = -1;
    float m_holdSince = 0.0f;
    glm::vec3 m_holdPoint{0.0f};
    glm::vec3 m_holdWanted{0.0f};
    bool m_eats = false;
    // Done looking and not eating: bringing what it holds down near the ground to throw it away (thrown from up at its eyes, a fall of
    // fifty metres, it was always a death).
    bool m_lowering = false;
    float m_nextBiteAt = 0.0f;
    float m_hurtHolding = 0.0f;
    // The leviathan: up at the surface for a while, rarely.
    float m_surfaceUntil = -1.0f;
    float m_nextSurfaceAt = 360.0f;
    float m_depthAim = 0.0f; // how far under the surface it means to be, eased: no bobbing up and back down

    // The body (both): one skin, its lights, its eyes; and what rounds find.
    MeshHandle m_skinMesh;
    MeshHandle m_glowMesh;
    MeshHandle m_eyeMesh;
    Entity m_skinEntity;
    Entity m_glowEntity;
    Entity m_eyeEntity;
    std::vector<MeshVertex> m_skinned;
    std::vector<MeshVertex> m_glowSkinned;
    std::vector<MeshVertex> m_eyeSkinned;
    std::vector<BoneBox> m_boxes;

    // The strider's: its hull, its head and its legs, each a bone of the one mesh (0 the hull, 1 the head, then each leg's thigh and
    // shin); its strands, a tube each, made again each frame from where their links are.
    struct Leg
    {
        glm::vec3 hip{0.0f};  // on the hull, in its own frame
        glm::vec3 foot{0.0f}; // planted, in the world
        glm::vec3 from{0.0f};
        glm::vec3 to{0.0f};
        float step = -1.0f;   // how far through a step (below nought: planted)
        float arc = 0.0f;     // how high this step lifts it: over whatever is built under the way it swings
        float duration = 1.0f; // how long this step takes: longer the higher it lifts and climbs
        float upper = 10.0f;
        float lower = 12.0f;
    };
    struct Strand
    {
        glm::vec3 root{0.0f};
        std::vector<glm::vec3> points;
        std::vector<glm::vec3> last;
        float length = 1.0f; // of each link
        float stiffness = 0.5f;
        float thick = 0.3f;
        float phase = 0.0f; // its own sway, out of step with the others
        int skin = -1;      // the skin's vertex it grows out of (found the first time it is drawn): its root goes with the skin
    };
    std::vector<Leg> m_legs;
    std::vector<Strand> m_strands;
    MeshHandle m_strandMesh;
    Entity m_strandEntity;
    MeshData m_strandShape;
    // The strand doing the work -- reaching, striking, holding -- chosen once as it starts and kept to the end (-1: none): picked
    // afresh every frame as the nearest, two of them swapped back and forth, each snapping to the shape and back (you, 2026-11-16:
    // the tentacles spasm).
    int m_holder = -1;
    float m_bodyHeight = 20.0f;
    glm::vec3 m_bodyRadii{4.0f};
    glm::vec3 m_neck{0.0f}; // where its head hangs from, on its hull
    float m_bodyY = 0.0f;   // how high its hull rides, eased: no popping up and down over what it steps across
    float m_dip = 0.0f;
    float m_dipSpeed = 0.0f;
    float m_bob = 0.0f;
    glm::vec2 m_lean{0.0f};
    glm::quat m_headTurn{1.0f, 0.0f, 0.0f, 0.0f};
    float m_topple = 0.0f;
    float m_toppleSide = 1.0f;
    // Bent down to look at somebody close -- or in at a window at where they went (0 standing to 1 right down); and dead, how fast its
    // body is coming down.
    float m_peek = 0.0f;
    float m_peekWanted = 0.0f;
    float m_fallSpeed = 0.0f;
    bool m_landed = false; // dead, down on the ground and still
    // The frightening part: how far its lights are put out (a leviathan stalking, dark; a strider watched, its eyes dimmed), and
    // whether it is standing dead still because it is being looked at.
    float m_dark = 0.0f;
    bool m_frozen = false;
    // Whether it freezes this time it is looked at (you, 2026-11-17: only sometimes -- it nearly always did): decided as it is first
    // looked at, kept a while; some striders never do, and one that does, not every time.
    bool m_freezeNow = false;
    // Throwing what it holds: when it began drawing it back (-1: not throwing), and how long the drawing back takes.
    float m_throwSince = -1.0f;
    // Reaching in at the way somebody went in, with a strand (Linger): through the doorway after them, just inside it -- where the
    // doorway is, and until when what it takes is drawn out through it before it is lifted (you picked: 2026-11-17).
    bool m_reachingIn = false;
    glm::vec3 m_reachDoor{0.0f};
    glm::vec3 m_reachInside{0.0f}; // where it took them, inside
    float m_pullOutUntil = -1.0f;
    static constexpr float kThrowWindup = 1.1f;
    float m_freezeDecidedUntil = -1.0f;
    int m_freezeLooks = 0;
    bool m_eyesGlow = true; // (its eyes lit, or dark glass)

    // The leviathan's: its form, its rest mesh and the bones it is skinned to -- the spine, head first, then its wings (a ray's), then
    // its arms (a squid's), each a chain -- and the way its head has come.
    Form m_form = Form::Serpent;
    MeshData m_rest;
    std::vector<SkinWeights> m_weights;
    MeshData m_glowRest;
    std::vector<SkinWeights> m_glowWeights;
    MeshData m_eyeRest;
    std::vector<SkinWeights> m_eyeWeights;
    std::vector<glm::mat4> m_restInverse;
    std::vector<glm::mat4> m_skinning;
    int m_spineBones = 12;
    std::vector<float> m_radius;          // the spine's thickness at each bone
    std::deque<glm::vec3> m_trail;        // where its front has been, newest first
    // Dead, its spine a chain let go (AnimateSwimmer): each joint now and a step before.
    std::vector<glm::vec3> m_deadPoints;
    std::vector<glm::vec3> m_deadPrevious;
    float m_swimPhase = 0.0f;
    // Its jaw (the bone after its spine's): how far open, eased; where it hinges, in its head's own frame.
    float m_gape = 0.0f;
    glm::vec3 m_jawHinge{0.0f};
    // Where its spine is now, head first, and how near anything solid it found ahead of its head (Steer).
    std::vector<glm::vec3> m_spineAt;
    float m_passedAt = -1.0f; // (a rush: when it was nearest whoever it went for)
    float m_rushNearest = 1.0e9f;
    float m_roll = 0.0f;                  // dead, turned over

    std::vector<Blow> m_blows;
    std::vector<glm::vec3> m_footfalls;
    bool m_call = false;
    bool m_rushHeard = false;
    bool m_rushStarted = false;
    bool m_shownOnly = false;
    glm::vec3 m_shownAt{0.0f};
    float m_shownYaw = 0.0f;
};

} // namespace pred
