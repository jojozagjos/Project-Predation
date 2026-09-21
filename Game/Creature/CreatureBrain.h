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
};

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
    const NavMesh* nav = nullptr;
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
};

enum class Behavior : uint8_t
{
    Roam,
    Investigate,
    Hunt,
    Attack,
    Retreat
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
    void OnDamaged(float amount, int byPlayer, const glm::vec3& from, float time);

    const CreatureIntent& Intent() const { return m_intent; }
    const CreatureTraits& Traits() const { return m_traits; }
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
    float m_attackCooldownUntil = 0.0f;
    float m_windupStarted = -1.0f;
};

} // namespace pred
