#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Entity.h"
#include "Game/Creature/CreatureAnatomy.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace pred
{

class Scene;
class MeshLibrary;

// The giants (you, 2026-11-13): huge -- on land a strider tens of metres tall on long legs, stepping between the buildings over
// their roofs; in the sea a leviathan longer than a building, the enemy on most water worlds. Each its own from its seed: how big,
// how many legs, how long, its colours (a creature's colour family), its lights. Never afraid of the people it finds: to a thing
// that size they are toys -- it comes to look at them, noses at them, plays with them roughly, and kills them doing it. Fair: it
// sees what it can see from where its eyes are, in the open, and is long enough making somebody out to be got away from; under a
// roof, or out of the water from a swimmer, they are beyond it. Its strikes are seen coming.
//
// Not a Creature: too big for the navigation, the doors, the ragdolls. Its own body of rigid parts (Sdf, SurfaceNets), its own
// mind, run on the host and shown everywhere else from what the host sends (GiantSnapshot).
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
    bool roofed = false; // under a roof: out of its reach and its sight
    bool inWater = false;
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
        Play,     // over them, looking at them, nosing at them
        Strike,   // a blow: wound up (seen coming), then down
        Linger,   // they went where it cannot follow: it waits about the place
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

    // Its parts, for rounds: a round into one is felt -- not feared.
    bool Owns(BodyHandle body) const;
    void TakeDamage(float amount, int byPlayer, float time);

    // What has happened since last asked, for the game: a blow that landed on somebody, its feet coming down, its call.
    struct Blow
    {
        int player = -1;
        float damage = 0.0f;
        glm::vec3 at{0.0f};
        glm::vec3 push{0.0f};
    };
    std::vector<Blow> TakeBlows();
    std::vector<glm::vec3> TakeFootfalls();
    bool TakeCall();

    GiantKind Kind() const { return m_kind; }
    uint32_t Seed() const { return m_seed; }
    const glm::vec3& Position() const { return m_position; }
    float Yaw() const { return m_yaw; }
    Mode CurrentMode() const { return m_mode; }
    const glm::vec3& LookAt() const { return m_lookAt; }
    float StrikePhase() const { return m_strike; }
    const glm::vec3& StrikeAt() const { return m_strikeAt; }
    const glm::vec3& Head() const { return m_head; }
    float Size() const { return m_size; }
    const std::string& Goal() const { return m_goal; }
    std::string Describe() const;
    // Who it has its mind on (-1 nobody), and how far it has made out each of them (0 to 1+).
    int Target() const { return m_target; }

    static const char* ModeName(Mode mode);

private:
    struct Part
    {
        Entity entity;
        MeshHandle mesh;
    };
    struct Leg
    {
        glm::vec3 hip{0.0f};  // on the body, in its own frame
        glm::vec3 foot{0.0f}; // planted, in the world
        glm::vec3 from{0.0f}; // where a step started
        glm::vec3 to{0.0f};   // where it comes down
        float step = -1.0f;   // how far through a step (below nought: planted)
        float upper = 10.0f;
        float lower = 12.0f;
        Part thigh;
        Part shin;
    };
    struct Strand
    {
        glm::vec3 root{0.0f}; // on the body, in its own frame
        std::vector<glm::vec3> points;
        std::vector<glm::vec3> last;
        std::vector<Part> parts;
        float length = 1.0f; // of each link
    };

    void BuildWalker(MeshLibrary& meshes);
    void BuildSwimmer(MeshLibrary& meshes);
    Part MakePart(MeshLibrary& meshes, const MeshData& mesh, const std::string& name, bool glow);
    void ShowPart(const Part& part, const glm::vec3& at, const glm::quat& turn);
    void MakeHitbox(float halfLength, float radius);

    void Steer(const GiantWorld& world, const glm::vec3& goal, float speed, float dt);
    bool Sees(const GiantWorld& world, const GiantSensed& who) const;
    void AnimateWalker(const GiantWorld& world, float time, float dt);
    void AnimateSwimmer(const GiantWorld& world, float time, float dt);
    glm::vec3 FootFor(const GiantWorld& world, const Leg& leg, const glm::vec3& ahead) const;

    Scene& m_scene;
    MeshLibrary& m_meshes;
    PhysicsWorld& m_physics;
    GiantKind m_kind;
    uint32_t m_seed;
    CreatureAnatomy m_look; // its colours, its markings, its lights (a creature's of its family)

    glm::vec3 m_position{0.0f}; // walker: under its body on the ground; swimmer: its head
    float m_yaw = 0.0f;
    glm::vec3 m_velocity{0.0f};
    float m_size = 24.0f;       // walker: how high its body rides; swimmer: how long it is
    float m_speed = 3.0f;       // its walk
    glm::vec3 m_head{0.0f};
    glm::vec3 m_lookAt{0.0f};
    glm::quat m_headTurn{1.0f, 0.0f, 0.0f, 0.0f};

    // The mind.
    Mode m_mode = Mode::Roam;
    std::string m_goal = "going about";
    float m_modeSince = 0.0f;
    glm::vec3 m_roamTo{0.0f};
    bool m_haveRoam = false;
    int m_target = -1;
    glm::vec3 m_targetAt{0.0f};
    float m_lastSeen = -100.0f;
    std::vector<std::pair<int, float>> m_aware; // who, how far made out
    float m_annoyed = 0.0f;                     // shot at: rougher for a while
    float m_nextStrikeAt = 0.0f;
    float m_strike = 0.0f;                      // 0 none; 0..1 through a blow
    glm::vec3 m_strikeAt{0.0f};
    bool m_struck = false;                      // this blow has landed
    float m_playUntil = 0.0f;
    float m_health = 1.0f;

    // The body.
    Part m_body;
    Part m_bodyGlow;
    Part m_headPart;
    Part m_eyes;
    Part m_jaw;
    std::vector<Leg> m_legs;
    std::vector<Strand> m_strands;
    float m_bodyHeight = 20.0f;
    float m_bob = 0.0f;
    // The swimmer's: its length as segments following the way its head went.
    std::vector<Part> m_segments;
    std::vector<Part> m_segmentGlows;
    std::vector<float> m_segmentRadius;
    float m_segmentLength = 3.0f;
    std::deque<glm::vec3> m_trail; // where its head has been, newest first
    std::vector<glm::vec3> m_spine;
    float m_swimPhase = 0.0f;
    std::vector<BodyHandle> m_hitboxes;
    std::vector<std::pair<int, float>> m_hitboxOn; // which part each follows: (leg or segment index, -1 the body), along it

    std::vector<Blow> m_blows;
    std::vector<glm::vec3> m_footfalls;
    bool m_call = false;
    bool m_shownOnly = false;
    glm::vec3 m_shownAt{0.0f};
    float m_shownYaw = 0.0f;
    std::vector<MeshHandle> m_ownMeshes;
};

} // namespace pred
