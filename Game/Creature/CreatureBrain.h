#pragma once

#include "Game/Creature/CreatureTraits.h"
#include "Game/Creature/Noise.h"

#include <glm/vec3.hpp>

#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pred
{

class NavMesh;

// A player, as the creature's senses are given them each tick. Built by the game from whatever it
// knows -- the local player, everybody the host is simulating -- so the brain never reaches into
// the world itself and a test can hand it whatever situation it wants.
struct SensedPlayer
{
    int id = 0;
    std::string name;
    glm::vec3 feet{0.0f};
    glm::vec3 velocity{0.0f};
    // How tall they are right now: about 1.8 standing, 1.1 crouched, 0.45 lying down. Smaller is
    // harder to see and is what crouching is for.
    float height = 1.8f;
    bool alive = true;
    // Inside a locker. Not visible, and not audible unless they make a noise.
    bool hidden = false;
    // How lit they are where they stand, 0 to 1. A torch that is on makes this high whatever the
    // room is doing: a light in the dark is the most visible thing in it.
    float light = 1.0f;
    // Which way they are looking. It is how the creature knows whether it is being watched, which is
    // the difference between an opening and a mistake.
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    // Which hiding place they are in, -1 when none. The brain is not allowed to look at this except
    // at the moment it opens that very place: nobody can see through a locker door.
    int hidingPlace = -1;
};

// Somewhere a player can hide -- a locker. Where to stand to open it, where the person inside would
// be, and whether its door is shut, which is the one thing about it anybody can see from outside: an
// empty locker stands open.
struct HidingPlace
{
    glm::vec3 front{0.0f};
    glm::vec3 inside{0.0f};
    bool shut = false;
};

// A door as the brain knows it: where its panel stands when shut, and whether it is shut and locked. A
// shut door is a wall across its route -- the navigation mesh runs straight through it -- so a creature
// that meets one pulls it open, or breaks it down.
struct DoorSense
{
    int index = -1;
    glm::vec3 a{0.0f}; // one end of the shut panel, at the floor
    glm::vec3 b{0.0f}; // the other end
    bool shut = false;
    bool locked = false;
};

// The ways it has of hurting somebody.
enum class AttackKind : uint8_t
{
    None,
    Swipe, // a claw raked across: quick, from close
    Bite,  // the jaws: slower, and worse
    Lunge, // thrown at somebody from a few metres off, knocking them flat
    Grab   // hold of them, to drag them away from the others
};

const char* AttackKindName(AttackKind kind);

// Everything the brain gets to know this tick.
struct CreatureSenses
{
    float time = 0.0f;                // seconds since it was made
    glm::vec3 position{0.0f};         // at its feet
    glm::vec3 eye{0.0f};              // where it sees from
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    float healthFraction = 1.0f;
    std::vector<SensedPlayer> players;
    // Sounds made since the last tick. The brain decides which it heard.
    std::vector<Noise> noises;
    // Whether anything solid is between two points. The game answers it with the physics world.
    std::function<bool(const glm::vec3&, const glm::vec3&)> clearLine;
    // How lit a point is, 0 to 1, for choosing somewhere dark to wait. Optional: without it every
    // place is as good as every other.
    std::function<float(const glm::vec3&)> lightAt;
    // Every hiding place in the level. Where they are is no secret -- they are furniture.
    std::vector<HidingPlace> hidingPlaces;
    // Where the other creatures are, standing or fallen. The body keeps its distance from them; a pack
    // that shares what it sees would start here.
    std::vector<glm::vec3> others;
    const NavMesh* nav = nullptr;
    // The doors in the level, shut or open.
    std::vector<DoorSense> doors;
    // Where its nest is, when it has one: where it takes what it catches, and goes back to heal.
    bool hasHive = false;
    glm::vec3 hive{0.0f};
    // Whether the last route it was sent along gets all the way there. False when somebody is up
    // somewhere it cannot follow.
    bool routeReached = true;
    // How far above its own feet it can strike, rearing up.
    float verticalReach = 1.5f;
};

// What the brain wants its body to do.
struct CreatureIntent
{
    bool move = false;
    glm::vec3 destination{0.0f};
    float speed = 0.0f;
    // Where to look, when that is not where it is going: a creature closing to strike turns to its
    // target rather than to the next corner of its path.
    bool face = false;
    glm::vec3 facePoint{0.0f};
    // Set on the one tick a strike lands. The game decides what it hit.
    int strikeTarget = -1;
    // How far through the wind-up of a strike it is, 0 when not striking. For the body to show.
    float windup = 0.0f;
    // How low it carries itself, 0 to 1: stalking, it creeps.
    float crouch = 0.0f;
    // Lying still as if dead. The body shows it exactly as it shows a death.
    bool down = false;
    // Set on the one tick it pulls open a hiding place. The game drags out whoever is inside.
    int openHidingPlace = -1;
    // Where its head is turned, whatever the body is doing. Searching is a head going from side to
    // side, not a body spinning on the spot.
    bool look = false;
    glm::vec3 lookAt{0.0f};
    // The blow it is in the middle of: which, how far through (0 to 1), which arm, and aimed where.
    AttackKind attack = AttackKind::None;
    float attackPhase = 0.0f;
    int attackSide = 1;
    glm::vec3 attackAt{0.0f};
    // What kind of blow `strikeTarget` is, on the tick it lands.
    AttackKind strikeKind = AttackKind::None;
    // Set on the one tick it closes its hands on somebody. The game decides whether it has them.
    int grabTarget = -1;
    // Who it is holding, while it holds them. The game carries them along with it.
    int holding = -1;
    // Set on the one tick it wraps somebody up at its nest. The game makes the cocoon.
    int cocoonTarget = -1;
    // Calling to the others, on the tick it does.
    bool roar = false;
    // Set on the one tick it finishes a nest, and where. The game builds it there.
    bool buildHive = false;
    glm::vec3 hiveAt{0.0f};
    // A door: set on the tick it pulls one open, and on each blow against one that is locked.
    int openDoor = -1;
    int bashDoor = -1;
    bool bashing = false;
    // Mending, as a fraction of its whole health a second, while it has gone to ground. The game
    // gives it the health.
    float recover = 0.0f;
};

enum class Behavior : uint8_t
{
    Roam,
    Investigate,
    Hunt,
    Attack,
    Retreat,
    // Shadowing somebody from out of their sight, waiting for them to look away or be alone.
    Stalk,
    // Lying still as if dead, badly hurt, until somebody comes close enough or turns their back.
    PlayDead,
    // Going through the places somebody it lost could have gone, lockers among them.
    Search,
    // Curious, not hungry: following somebody at a distance, openly, to watch them.
    Observe,
    // Carrying somebody it has hold of away from the others, to kill them, or to its nest.
    Drag,
    // Making a nest somewhere dark and out of the way: what it takes its catches back to.
    Nest
};

const char* BehaviorName(Behavior behavior);

// The creature's mind.
//
// Utility scoring, as Docs/AI.md sets out: every tick it considers each thing it could do, and each
// target it could do it to, and scores each option as the product of named considerations --
// distance, confidence, pain, aggression and so on. The highest wins, with a bonus for whatever it is
// already doing so that two options scoring nearly the same do not make it flicker between them.
// Nothing here is a scripted sequence of states; a creature that is hurt, frightened and curious at
// once simply scores its options differently.
//
// It is also written to be read. Every decision keeps the scores that produced it, with every
// consideration that went into each, and every change of mind goes on a timeline with the reason --
// because "why did it do that" is the question anybody tuning a creature is always asking, and a
// behaviour whose reasons are invisible cannot be tuned.
class CreatureBrain
{
public:
    explicit CreatureBrain(const CreatureTraits& traits);

    // Perceives, decides and acts. Perception and decisions run at their own lower rates inside; the
    // behaviour being carried out is stepped every call.
    void Update(const CreatureSenses& senses, float dt);

    // Somebody hurt it. `byPlayer` is -1 when nobody in particular did.
    // `maxHealth` is what the body can take in all, so a round hurts a big body less than a small one:
    // pain and harm are measured against it, as they were against the 160 every creature once had.
    void OnDamaged(float amount, int byPlayer, const glm::vec3& from, float time, float maxHealth = 160.0f);
    // It is dead, really. The mind stops here: nothing after this perceives, decides or moves, and
    // what it wanted to do is forgotten -- a strike it was in the middle of does not land later.
    void OnDied(float time);
    bool Dead() const { return m_dead; }
    // The game closed its hands on somebody for it: it has them, and carries them off.
    void OnGrabbed(int player, float time);
    // It lost hold of them: shot off them, struggled free, or they died.
    void OnReleased(float time, const std::string& why);
    int Holding() const { return m_holding; }
    AttackKind CurrentAttack() const { return m_attack; }

    const CreatureIntent& Intent() const { return m_intent; }
    const CreatureTraits& Traits() const { return m_traits; }
    // How far its eyes reach, in metres: 0 for one with none. Within touching distance it knows
    // somebody is there whatever its eyes, which is CloseSense().
    float SightRange() const;
    static float CloseSense();
    Behavior Current() const { return m_behavior; }
    int CurrentTarget() const { return m_target; }
    const std::string& CurrentGoal() const { return m_goal; }

    // --- What the inspector reads -------------------------------------------------------------

    struct Consideration
    {
        const char* name = "";
        float value = 1.0f;
    };
    struct Option
    {
        Behavior behavior = Behavior::Roam;
        int target = -1;
        std::string label;
        float score = 0.0f;
        std::vector<Consideration> considerations;
    };
    // Every option weighed at the last decision, best first.
    const std::vector<Option>& Options() const { return m_options; }

    // A player as the creature remembers them.
    struct Track
    {
        int id = 0;
        std::string name;
        glm::vec3 lastKnown{0.0f};
        glm::vec3 lastVelocity{0.0f};
        // How sure it is they are still near the last known place. Rises to 1 on seeing them and
        // fades with time; hearing them brings it partly back.
        float confidence = 0.0f;
        // How long it has been watching them, 0 to 1. A glimpse is not a sighting: this has to fill
        // before they count as seen, and empties again when they are out of view.
        float exposure = 0.0f;
        float lastSeen = -1.0f;
        float lastHeard = -1.0f;
        bool visible = false;
        // How much they have hurt it. Something that has been shot is wary of whoever shot it.
        float harm = 0.0f;
        // Whether they are looking at it, with nothing in the way -- as far as it knows. `watchKnown`
        // is whether it has seen them recently enough to know at all; when it has not, it cannot
        // tell an opening from a trap.
        bool watching = false;
        bool watchKnown = false;
        // Whether it can make them out at all at this moment, however faintly.
        bool inView = false;
        float lookedAt = -1.0e9f;
        bool lookedLooking = false;
        // How far they are from the nearest other living player, in metres; large when alone.
        float isolation = 99.0f;
        // Seconds it has spent stalking them, which is what its patience is measured against.
        float stalked = 0.0f;
        // Seconds it has spent just watching them. Curiosity wears off: this is what it wears off against.
        float observed = 0.0f;

        // Why it can or cannot see them at this moment, factor by factor. The visibility is these
        // multiplied together, so when the question is "why does it not see me" the answer is
        // whichever of them is nought -- and the verdict says which, in words.
        struct SightCheck
        {
            const char* verdict = "not looked for yet";
            float distance = 0.0f;
            float field = 0.0f;    // 1 dead ahead, down to the edge value at the rim of its view
            int clear = 0;         // of three lines to them -- head, chest, hips -- how many are open
            float nearness = 0.0f; // 1 close, 0 at the limit of its sight
            float size = 0.0f;     // standing is 1; crouched and prone are smaller
            float motion = 0.0f;   // moving catches the eye
            float light = 0.0f;    // how lit they are, to eyes that need light
            float visibility = 0.0f;
        };
        SightCheck sight;
    };
    const std::vector<Track>& Tracks() const { return m_tracks; }

    // Something worth going to look at: a noise, or a glimpse that did not become a sighting.
    struct Stimulus
    {
        glm::vec3 position{0.0f};
        float strength = 0.0f;
        float time = -1.0f;
        std::string what;
        bool resolved = true;
    };
    const Stimulus& Interest() const { return m_interest; }

    // Heard recently, for the inspector and the overlay.
    struct Heard
    {
        Noise noise;
        float strength = 0.0f;
        float time = 0.0f;
    };
    const std::deque<Heard>& RecentNoises() const { return m_heard; }

    // How it feels. Events push these; traits decide how far and how fast they come back.
    struct State
    {
        float pain = 0.0f;
        float fear = 0.0f;
        float arousal = 0.0f;
    };
    const State& Feelings() const { return m_state; }

    struct TimelineEntry
    {
        float time = 0.0f;
        std::string what;
    };
    const std::deque<TimelineEntry>& Timeline() const { return m_timeline; }

    // The places it last weighed as cover, scored, for the overlay: which it chose, and why the rest
    // were worse. Stalking, ambushing and hiding come from queries like this rather than from lists
    // of hiding places, so a vent or a dark corner added to a map is used without being named.
    struct CoverCandidate
    {
        glm::vec3 position{0.0f};
        float score = 0.0f;
        bool hidden = false; // out of every known player's sight
    };
    const std::vector<CoverCandidate>& Cover() const { return m_cover; }

    // What it believes about each hiding place, from what it has heard, seen and found.
    struct PlaceMemory
    {
        float suspicion = 0.0f; // 0 to 1: how likely it thinks it is that somebody is inside
        int suspect = -1;       // who, when it has a particular person in mind
        float checkedAt = -1.0e9f;
        bool seenShut = false;
    };
    const std::vector<PlaceMemory>& Places() const { return m_places; }
    // What it has learnt this match about lockers: how much a shut door means somebody behind it, 0
    // to 1. Nothing, until it has opened a shut one and found somebody.
    float ShutMeansSomebody() const { return m_shutMeansSomebody; }
    int FoundHiding() const { return m_foundHiding; }
    // Where it is going to look, in order, while searching.
    struct SearchStop
    {
        glm::vec3 point{0.0f};
        int place = -1; // a hiding place to open, or -1 for somewhere to stand and look around
    };
    const std::vector<SearchStop>& SearchPlan() const { return m_searchPlan; }
    size_t SearchStep() const { return m_searchStep; }

    // Where it has come to expect players: the ground in four-metre squares, each warmed by seeing or
    // hearing somebody there and cooling over a few minutes. Wandering drifts towards the warm ones,
    // so a creature left alone patrols where people go rather than where nobody does.
    static constexpr float kHeatCell = 4.0f;
    struct HeatCell
    {
        int x = 0;
        int z = 0;
        float heat = 0.0f;
    };
    const std::vector<HeatCell>& Heat() const { return m_heat; }
    bool HasCoverPoint() const { return m_haveStalkPoint; }
    const glm::vec3& CoverPoint() const { return m_stalkPoint; }

    // Where the body is being sent and the route there, filled in by the body, for the overlay.
    std::vector<glm::vec3>& Route() { return m_route; }
    const std::vector<glm::vec3>& Route() const { return m_route; }

private:
    void Perceive(const CreatureSenses& senses, float dt);
    void Decide(const CreatureSenses& senses);
    void Act(const CreatureSenses& senses, float dt);

    Track& TrackFor(const SensedPlayer& player);
    Track* FindTrack(int id);
    const SensedPlayer* FindPlayer(const CreatureSenses& senses, int id) const;
    void Log(float time, std::string what);
    void Switch(Behavior behavior, int target, const std::string& reason, float time);
    bool PickFleePoint(const CreatureSenses& senses, glm::vec3& out);
    // Closes whatever it was going to look into near `where`, now that it knows who was there.
    void ResolveInterestNear(const glm::vec3& where);
    // Somewhere near `target` that none of the players it knows about can see, a stalking distance
    // from them, preferably dark and behind them. Fills m_cover with every candidate it weighed.
    bool FindCover(const CreatureSenses& senses, const Track& target, const SensedPlayer* player,
                   glm::vec3& out);
    // Whether any player it knows about has a clear line to this point at body height.
    bool SeenFrom(const CreatureSenses& senses, const glm::vec3& point) const;
    // How good a moment it is to go for somebody, 0 to 1: alone, looking away, or already so close
    // there is no use waiting, and past the end of its patience any moment will do.
    float Opening(const Track& track, float distance) const;
    // Playing dead: whether to get up now, and what for.
    void UpdatePlayingDead(const CreatureSenses& senses);
    // Hiding places: what it can see of them, and suspicion fading.
    void PerceivePlaces(const CreatureSenses& senses, float dt);
    // Something happened at or near a hiding place: raise its suspicion, perhaps with a name on it.
    void Suspect(int place, float suspicion, int who);
    // The places to go through for somebody lost: likely lockers first, then where they were heading.
    void PlanSearch(const CreatureSenses& senses, const Track& track);
    // Going to a hiding place and pulling it open. True while still at it; false once it is done.
    bool OpenPlace(const CreatureSenses& senses, int place);
    // How much it has come to expect people to hide, 0 to 1, from lockers heard and people found.
    float HidingHabit() const;
    // Somebody was here: warm the square.
    void Warm(const glm::vec3& where, float amount);
    // Somewhere warm to wander towards, chosen with a weight on how warm; false when nowhere is.
    bool PickWarmPlace(const CreatureSenses& senses, glm::vec3& out);

    CreatureTraits m_traits;
    SeededRandom m_random;
    CreatureIntent m_intent;

    Behavior m_behavior = Behavior::Roam;
    int m_target = -1;
    std::string m_goal;
    float m_behaviorStarted = 0.0f;

    std::vector<Track> m_tracks;
    Stimulus m_interest;
    std::deque<Heard> m_heard;
    State m_state;
    std::vector<Option> m_options;
    std::deque<TimelineEntry> m_timeline;
    std::vector<glm::vec3> m_route;

    // The rates it thinks at: often enough to react inside a heartbeat, rarely enough that the cost
    // of a creature is a rounding error on the frame.
    float m_perceiveTimer = 0.0f;
    float m_decideTimer = 0.0f;
    float m_lastTime = 0.0f;

    // Noises that arrived since perception last ran. It runs fifteen times a second and the game
    // ticks sixty, so a sound made on a tick in between would otherwise simply never be heard --
    // which is what happened to the first gunshot anybody fired at it.
    std::vector<Noise> m_pendingNoises;

    // Something catching its eye that it has not made out yet: where, and until when it keeps
    // looking. An animal that sees movement at the edge of its view stops and turns to it; one that
    // wandered on regardless would lose every glimpse before it became a sighting.
    glm::vec3 m_alertPoint{0.0f};
    float m_alertUntil = -1.0f;

    // Behaviour-specific working state.
    glm::vec3 m_roamPoint{0.0f};
    bool m_haveRoamPoint = false;
    float m_pauseUntil = 0.0f;
    float m_lookAroundUntil = 0.0f;
    bool m_arrived = false;
    glm::vec3 m_fleePoint{0.0f};
    bool m_haveFleePoint = false;
    glm::vec3 m_threat{0.0f};
    float m_retreatUntil = 0.0f;
    float m_retreatCooldownUntil = 0.0f;
    // Somebody close enough, or recent enough, that its wounds are something to be afraid about.
    bool m_threatened = false;
    float m_attackCooldownUntil = 0.0f;
    float m_windupStarted = -1.0f;

    // Stalking: the cover it is going to or waiting in, and when to check that it still is cover.
    glm::vec3 m_stalkPoint{0.0f};
    bool m_haveStalkPoint = false;
    float m_stalkCheckAt = 0.0f;
    bool m_stalkExposed = false; // in sight of somebody it knows about, where it stands
    // Leaning out of cover to look: where to, until when, and when next.
    bool m_peeking = false;
    glm::vec3 m_peekPoint{0.0f};
    float m_peekUntil = -1.0f;
    float m_nextPeekAt = 0.0f;
    std::vector<CoverCandidate> m_cover;

    // Playing dead: until when at the longest, how often it has, and whether it has been hurt while
    // lying there -- which ends the act.
    float m_lastHurt = -1.0e9f;
    float m_playDeadUntil = 0.0f;
    int m_playDeadCount = 0;
    bool m_hurtWhileDown = false;
    // Until when it sees through what it has started rather than weighing it again.
    float m_committedUntil = 0.0f;

    // Hiding places and searching.
    std::vector<PlaceMemory> m_places;
    float m_shutMeansSomebody = 0.0f;
    int m_foundHiding = 0;
    int m_lockersHeard = 0;
    std::vector<SearchStop> m_searchPlan;
    size_t m_searchStep = 0;
    // The hiding place it is going to or opening, and when it took hold of the door.
    int m_opening = -1;
    float m_openStarted = -1.0f;

    std::vector<HeatCell> m_heat;
    // Attacking: which blow, when it started, whether it has landed, and which arm.
    void StartAttack(AttackKind kind, const glm::vec3& at, float time);
    AttackKind m_attack = AttackKind::None;
    float m_attackStarted = -1.0f;
    bool m_attackLanded = false;
    int m_attackSide = 1;
    float m_lungeReadyAt = 0.0f;
    // Holding somebody: who, how much it has been hurt since, where it is taking them, and when it next
    // bites.
    int m_holding = -1;
    float m_holdDamage = 0.0f;
    float m_holdStarted = 0.0f;
    glm::vec3 m_dragPoint{0.0f};
    bool m_haveDragPoint = false;
    bool m_dragArrived = false;
    float m_nextBite = 0.0f;
    bool PickDragPoint(const CreatureSenses& senses, int victim, glm::vec3& out);
    // Somewhere to build: dark, out of the way, and nowhere anybody walks.
    bool PickNestSite(const CreatureSenses& senses, glm::vec3& out);
    // Building: where, and when it started work.
    glm::vec3 m_nestSite{0.0f};
    bool m_haveNestSite = false;
    float m_nestWorkStarted = -1.0f;
    float m_nestThoughtAt = 0.0f;
    // Looking round: the way it was facing when it started, which the head swings either side of.
    glm::vec3 m_lookBase{0.0f, 0.0f, -1.0f};
    bool m_lookBaseSet = false;
    // Somebody up out of reach: since when, where it paces below them, and when it next calls.
    float m_unreachableSince = -1.0f;
    float m_nextPaceAt = 0.0f;
    glm::vec3 m_pacePoint{0.0f};
    float m_nextRoarAt = 0.0f;
    // A door in the way: which, and when it started on it.
    int m_door = -1;
    float m_doorStarted = -1.0f;
    float m_nextBash = 0.0f;
    // Whether a door stands between it and the next corner of its route; fills `door`.
    bool DoorInTheWay(const CreatureSenses& senses, DoorSense& door) const;

    bool m_dead = false;
};

} // namespace pred
