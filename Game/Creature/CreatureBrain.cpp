#include "Game/Creature/CreatureBrain.h"
#include "Game/Creature/CreatureTuning.h"

#include "Engine/Navigation/NavMesh.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

constexpr float kPerceiveInterval = 1.0f / 15.0f;
constexpr float kDecideInterval = 1.0f / 10.0f;

// Sight. The cone is wide because an animal's is; the edge of it sees poorly, which is what makes
// creeping along a wall at the side of its view worth doing.
// Anything right beside it is noticed whichever way it is facing: close enough to touch is close
// enough to smell, and a creature that could be walked round in a circle would be a joke.
// How fast watching somebody fills the exposure meter, and how fast looking away empties it. At full
// visibility a little under half a second is a sighting; a dim, crouched, distant figure takes many
// seconds, and a glance across a lit doorway is only a glance.

// Striking.

// How long each blow takes from start to finish, and how far through it lands. Quick: the swipe lands a
// quarter of a second after it starts, which is about how long a person takes to notice it has.
struct AttackTiming
{
    float duration;
    float lands;
};
AttackTiming TimingOf(AttackKind kind)
{
    switch (kind)
    {
    case AttackKind::Swipe:
        return {0.55f, 0.45f};
    case AttackKind::Bite:
        return {0.62f, 0.5f};
    case AttackKind::Lunge:
        return {0.95f, 0.55f};
    case AttackKind::Grab:
        return {0.6f, 0.5f};
    case AttackKind::None:
        break;
    }
    return {0.5f, 0.5f};
}

// A decision already being carried out needs this much of a lead to be abandoned. Without it two
// options scoring 0.50 and 0.51 in turn make a creature that twitches between them.

constexpr size_t kTimelineLength = 64;
constexpr float kHeardMemorySeconds = 12.0f;

float Horizontal(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = b - a;
    return std::sqrt(d.x * d.x + d.z * d.z);
}

glm::vec3 Flat(const glm::vec3& v)
{
    const glm::vec3 flat{v.x, 0.0f, v.z};
    const float length = glm::length(flat);
    return length > 1e-4f ? flat / length : glm::vec3(0.0f);
}

std::string Format(const char* pattern, float a, float b = 0.0f)
{
    char line[160];
    std::snprintf(line, sizeof(line), pattern, a, b);
    return line;
}

} // namespace

const char* AttackKindName(AttackKind kind)
{
    switch (kind)
    {
    case AttackKind::None:
        return "none";
    case AttackKind::Swipe:
        return "swipe";
    case AttackKind::Bite:
        return "bite";
    case AttackKind::Lunge:
        return "lunge";
    case AttackKind::Grab:
        return "grab";
    }
    return "?";
}

const char* BehaviorName(Behavior behavior)
{
    switch (behavior)
    {
    case Behavior::Roam:
        return "Roam";
    case Behavior::Investigate:
        return "Investigate";
    case Behavior::Hunt:
        return "Hunt";
    case Behavior::Attack:
        return "Attack";
    case Behavior::Retreat:
        return "Retreat";
    case Behavior::Stalk:
        return "Stalk";
    case Behavior::PlayDead:
        return "PlayDead";
    case Behavior::Search:
        return "Search";
    case Behavior::Observe:
        return "Observe";
    case Behavior::Drag:
        return "Drag";
    case Behavior::Nest:
        return "Nest";
    case Behavior::Avoid:
        return "Avoid";
    case Behavior::Warn:
        return "Warn";
    case Behavior::Ambush:
        return "Ambush";
    case Behavior::Flank:
        return "Flank";
    case Behavior::Lure:
        return "Lure";
    }
    return "?";
}

CreatureBrain::CreatureBrain(const CreatureTraits& traits)
    : m_traits(traits), m_random(static_cast<uint64_t>(traits.seed) * 0x2545F4914F6CDD1Dull + 1u)
{
    m_goal = "wandering";
}

void CreatureBrain::Log(float time, std::string what)
{
    m_timeline.push_back({time, std::move(what)});
    while (m_timeline.size() > kTimelineLength)
    {
        m_timeline.pop_front();
    }
}

CreatureBrain::Track* CreatureBrain::FindTrack(int id)
{
    for (Track& track : m_tracks)
    {
        if (track.id == id)
        {
            return &track;
        }
    }
    return nullptr;
}

CreatureBrain::Track& CreatureBrain::TrackFor(const SensedPlayer& player)
{
    if (Track* found = FindTrack(player.id))
    {
        found->name = player.name;
        return *found;
    }
    Track fresh;
    fresh.id = player.id;
    fresh.name = player.name;
    fresh.lastKnown = player.feet;
    m_tracks.push_back(fresh);
    return m_tracks.back();
}

const SensedPlayer* CreatureBrain::FindPlayer(const CreatureSenses& senses, int id) const
{
    for (const SensedPlayer& player : senses.players)
    {
        if (player.id == id)
        {
            return &player;
        }
    }
    return nullptr;
}

void CreatureBrain::OnDamaged(float amount, int byPlayer, const glm::vec3& from, float time, float maxHealth)
{
    if (m_dead)
    {
        return;
    }
    m_lastHurt = time;
    if (m_behavior == Behavior::PlayDead)
    {
        m_hurtWhileDown = true;
    }
    const float scale = 160.0f / std::max(maxHealth, 1.0f);
    m_state.pain = std::min(m_state.pain + amount * scale / 60.0f, 2.0f);
    m_state.arousal = std::min(m_state.arousal + 0.5f, 1.0f);
    m_threat = from;
    if (Track* track = FindTrack(byPlayer))
    {
        // Being shot says exactly where the shooter is, and makes them somebody to be wary of.
        track->harm += amount * scale / 100.0f;
        track->provokedAt = time;
        track->confidence = 1.0f;
        track->lastKnown = from;
        ResolveInterestNear(from);
    }
    Log(time, Format("hurt for %.0f", amount) +
                  (FindTrack(byPlayer) != nullptr ? " by " + FindTrack(byPlayer)->name : ""));

    // Shot off whoever it has hold of: enough of that, from the others coming to help, and it lets go.
    if (m_holding >= 0)
    {
        m_holdDamage += amount;
        if (m_holdDamage > maxHealth * 0.06f)
        {
            m_state.fear = std::min(m_state.fear + 0.35f, 1.0f);
            OnReleased(time, "shot off them");
        }
    }
}

void CreatureBrain::OnNestAttacked(int byPlayer, const glm::vec3& where, float time, bool destroyed)
{
    if (m_dead)
    {
        return;
    }
    // Its brood. Whatever it was afraid of matters less than this, and whoever is doing it has made an
    // enemy of it whatever its temperament.
    m_state.arousal = 1.0f;
    m_state.fear = std::max(m_state.fear - (destroyed ? 0.6f : 0.3f), 0.0f);
    m_retreatUntil = 0.0f;
    m_committedUntil = 0.0f;
    if (Track* track = FindTrack(byPlayer))
    {
        track->provokedAt = time;
        if (!track->visible)
        {
            // Not seen, but somebody shooting a heart is standing in sight of it.
            track->lastKnown = where;
            track->confidence = std::max(track->confidence, 0.75f);
        }
    }
    m_interest.position = where;
    m_interest.strength = 1.0f;
    m_interest.time = time;
    m_interest.what = destroyed ? "its nest, destroyed" : "its nest, under attack";
    m_interest.resolved = false;
    m_nestAttackedAt = time;
    m_nestAttacker = byPlayer;
    Log(time, destroyed ? "its nest has been destroyed" : "something is at its nest");
}

void CreatureBrain::OnGrabbed(int player, float time)
{
    if (m_dead)
    {
        return;
    }
    m_holding = player;
    m_holdStarted = time;
    m_holdDamage = 0.0f;
    m_haveDragPoint = false;
    m_dragArrived = false;
    const Track* track = FindTrack(player);
    Switch(Behavior::Drag, player, "has hold of " + (track != nullptr ? track->name : std::string("somebody")), time);
}

void CreatureBrain::OnReleased(float time, const std::string& why)
{
    if (m_holding < 0)
    {
        return;
    }
    const int was = m_holding;
    m_holding = -1;
    m_intent.holding = -1;
    Log(time, "lets go: " + why);
    if (m_behavior == Behavior::Drag)
    {
        Switch(Behavior::Hunt, was, "lost its hold", time);
    }
}

void CreatureBrain::StartAttack(AttackKind kind, const glm::vec3& at, float time)
{
    m_attack = kind;
    m_attackStarted = time;
    m_attackLanded = false;
    m_attackSide = m_random.Unit() < 0.5f ? 1 : -1;
    (void)at;
}

bool CreatureBrain::PickDragPoint(const CreatureSenses& senses, int victim, glm::vec3& out)
{
    // Home, sometimes. Not every time: a creature that always went home made the nest the only thing
    // that ever happened, and being dragged into a dark corner and eaten there is worse.
    if (senses.hasHive && m_random.Unit() < 0.4f)
    {
        out = senses.hive;
        return true;
    }
    if (senses.nav == nullptr)
    {
        return false;
    }
    // Otherwise away from everybody else: as far as it can get from the rest of them, out of their sight,
    // not so far it will be shot to pieces getting there.
    float bestScore = -1.0e9f;
    bool found = false;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 20; ++i)
    {
        glm::vec3 candidate;
        if (!senses.nav->RandomPointNear(senses.position, 22.0f, seed, candidate))
        {
            continue;
        }
        const float far = Horizontal(candidate, senses.position);
        if (far < 7.0f)
        {
            continue;
        }
        float nearestOther = 60.0f;
        bool seen = false;
        for (const SensedPlayer& player : senses.players)
        {
            if (player.id == victim || !player.alive)
            {
                continue;
            }
            nearestOther = std::min(nearestOther, Horizontal(candidate, player.feet));
            if (senses.clearLine && senses.clearLine(player.feet + glm::vec3(0.0f, 1.6f, 0.0f), candidate + glm::vec3(0.0f, 0.9f, 0.0f)))
            {
                seen = true;
            }
        }
        const float score = nearestOther - far * 0.3f + (seen ? 0.0f : 10.0f);
        if (score > bestScore)
        {
            bestScore = score;
            out = candidate;
            found = true;
        }
    }
    return found;
}

bool CreatureBrain::PickNestSite(const CreatureSenses& senses, glm::vec3& out)
{
    if (senses.nav == nullptr)
    {
        return false;
    }
    // Somewhere dark, out of everybody's sight, away from where it has found people, and far enough from
    // here that it is somewhere rather than under its feet.
    float bestScore = -1.0e9f;
    bool found = false;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 24; ++i)
    {
        glm::vec3 candidate;
        if (!senses.nav->RandomPointNear(senses.position, 34.0f, seed, candidate))
        {
            continue;
        }
        const float away = Horizontal(candidate, senses.position);
        if (away < 8.0f)
        {
            continue;
        }
        float score = away * 0.2f;
        // Dark is what it wants most.
        if (senses.lightAt)
        {
            score += (1.0f - std::clamp(senses.lightAt(candidate), 0.0f, 1.0f)) * 14.0f;
        }
        // Not where people go, and not where anybody can see.
        for (const HeatCell& cell : m_heat)
        {
            const glm::vec3 warm{(static_cast<float>(cell.x) + 0.5f) * kHeatCell, candidate.y,
                                 (static_cast<float>(cell.z) + 0.5f) * kHeatCell};
            if (Horizontal(warm, candidate) < kHeatCell)
            {
                score -= cell.heat * 8.0f;
            }
        }
        if (!SeenFrom(senses, candidate))
        {
            score += 8.0f;
        }
        if (score > bestScore)
        {
            bestScore = score;
            out = candidate;
            found = true;
        }
    }
    return found;
}

bool CreatureBrain::DoorInTheWay(const CreatureSenses& senses, DoorSense& door) const
{
    if (!m_intent.move)
    {
        return false;
    }
    // From here towards the next corner of the route, as far as it would get in a second or so.
    glm::vec3 next = m_route.empty() ? m_intent.destination : m_route.front();
    glm::vec2 from{senses.position.x, senses.position.z};
    glm::vec2 to{next.x, next.z};
    const glm::vec2 along = to - from;
    const float length = glm::length(along);
    if (length < 1e-3f)
    {
        return false;
    }
    to = from + along / length * std::min(length, 1.6f);
    const auto cross = [](const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; };
    for (const DoorSense& candidate : senses.doors)
    {
        if (!candidate.shut || std::abs(candidate.a.y - senses.position.y) > 1.5f)
        {
            continue;
        }
        const glm::vec2 p{candidate.a.x, candidate.a.z};
        const glm::vec2 q{candidate.b.x, candidate.b.z};
        const glm::vec2 r = to - from;
        const glm::vec2 s = q - p;
        const float denominator = cross(r, s);
        if (std::abs(denominator) < 1e-6f)
        {
            continue;
        }
        const float t = cross(p - from, s) / denominator;
        const float u = cross(p - from, r) / denominator;
        if (t >= 0.0f && t <= 1.0f && u >= -0.05f && u <= 1.05f)
        {
            door = candidate;
            return true;
        }
    }
    return false;
}

void CreatureBrain::OnDied(float time)
{
    if (m_dead)
    {
        return;
    }
    m_dead = true;
    // Everything it meant to do goes with it. The intent is what the game reads each tick, and a
    // creature killed on the tick a strike was due left that strike standing: the game went on
    // applying it, every tick, from a corpse.
    m_intent = CreatureIntent{};
    m_goal = "dead";
    Log(time, "died");
}

void CreatureBrain::ResolveInterestNear(const glm::vec3& where)
{
    // Something it was going to look into has been explained: it was whoever is at `where`. The
    // question was "what was that", and now it knows. Left open, the question went on competing with
    // hunting the answer -- a creature missed and then hit by the same shooter kept walking over to
    // look at the first shot.
    if (!m_interest.resolved && glm::distance(m_interest.position, where) < 6.0f)
    {
        m_interest.resolved = true;
    }
}

void CreatureBrain::Update(const CreatureSenses& senses, float dt)
{
    if (m_dead)
    {
        return;
    }
    m_lastTime = senses.time;
    m_intent.strikeTarget = -1;
    m_intent.openHidingPlace = -1;
    // One memory per hiding place, before anything reads them.
    m_places.resize(senses.hidingPlaces.size());
    m_pendingNoises.insert(m_pendingNoises.end(), senses.noises.begin(), senses.noises.end());

    // Its ground is wherever it first found itself, until it has a nest: then the nest is.
    if (!m_haveHome || senses.hasHive)
    {
        m_home = senses.hasHive ? senses.hive : senses.position;
        m_haveHome = true;
    }

    m_perceiveTimer += dt;
    if (m_perceiveTimer >= kPerceiveInterval)
    {
        Perceive(senses, m_perceiveTimer);
        UpdateHostility(senses, m_perceiveTimer);
        m_perceiveTimer = 0.0f;
        m_pendingNoises.clear();
    }
    m_decideTimer += dt;
    if (m_decideTimer >= kDecideInterval)
    {
        Decide(senses);
        m_decideTimer = 0.0f;
    }
    Act(senses, dt);
}

void CreatureBrain::Perceive(const CreatureSenses& senses, float dt)
{
    // Its eyes decide how far it sees; one with none sees nothing at all, but still knows by touch and
    // smell when somebody is right beside it.
    const bool hasEyes = m_traits.sight > 0.0f;
    const float sightRange = SightRange();
    const float senseRange = std::max(sightRange, Tuning().closeSense);
    const float cosHalfField = std::cos(glm::radians(Tuning().halfFieldDegrees));
    glm::vec3 flatForward{senses.forward.x, 0.0f, senses.forward.z};
    flatForward = glm::length(flatForward) > 1e-4f ? glm::normalize(flatForward) : glm::vec3(0, 0, -1);

    // --- Sight --------------------------------------------------------------------------------

    for (const SensedPlayer& player : senses.players)
    {
        Track& track = TrackFor(player);
        const bool wasVisible = track.visible;
        float visibility = 0.0f;
        Track::SightCheck check;
        check.verdict = !player.alive ? "dead" : "hidden in a locker";
        if (player.alive && !player.hidden)
        {
            const glm::vec3 chest = player.feet + glm::vec3(0.0f, player.height * 0.6f, 0.0f);
            const glm::vec3 toward = chest - senses.eye;
            const float distance = glm::length(toward);
            check.distance = distance;
            check.verdict = "too far away";
            if (distance < senseRange && distance > 1e-3f)
            {
                glm::vec3 flat{toward.x, 0.0f, toward.z};
                const float flatLength = glm::length(flat);
                const float facing = flatLength > 1e-4f ? glm::dot(flat / flatLength, flatForward) : 1.0f;
                float field = 0.0f;
                if (hasEyes && distance < sightRange && facing >= cosHalfField)
                {
                    // Full in the middle of the view, falling to the edge value at the rim.
                    const float across = (1.0f - facing) / (1.0f - cosHalfField);
                    field = 1.0f - (1.0f - Tuning().edgeOfView) * across;
                }
                const bool touching = distance < Tuning().closeSense;
                if (touching)
                {
                    field = 1.0f;
                }
                check.field = field;
                check.verdict = !hasEyes ? "no eyes, and not close enough to touch"
                                : distance >= sightRange ? "beyond what its eyes can reach"
                                                         : "outside its field of view";

                if (field > 0.0f)
                {
                    // Head, chest and hips: somebody behind a waist-high crate is partly visible, and
                    // somebody whose head alone shows over a wall is mostly not.
                    int clear = 0;
                    for (const float at : {0.92f, 0.6f, 0.3f})
                    {
                        const glm::vec3 point = player.feet + glm::vec3(0.0f, player.height * at, 0.0f);
                        if (!senses.clearLine || senses.clearLine(senses.eye, point))
                        {
                            ++clear;
                        }
                    }
                    const float exposed = static_cast<float>(clear) / 3.0f;
                    const float nearness =
                        touching ? 1.0f : 1.0f - (distance / sightRange) * (distance / sightRange);
                    const float size = std::clamp(player.height / 1.8f, 0.25f, 1.0f);
                    const float speed = glm::length(glm::vec3(player.velocity.x, 0.0f, player.velocity.z));
                    const float motion = std::clamp(0.7f + speed * 0.12f, 0.7f, 1.3f);
                    // Touch and smell do not care how dark it is.
                    const float light = hasEyes ? std::clamp(player.light, 0.08f, 1.0f) : 1.0f;
                    visibility = field * exposed * nearness * size * motion * light;
                    check.clear = clear;
                    check.nearness = nearness;
                    check.size = size;
                    check.motion = motion;
                    check.light = light;
                    check.verdict = clear == 0 ? "something solid in the way" : "in view";
                }
            }
        }
        check.visibility = visibility;

        track.visible = false;
        track.inView = visibility > 0.001f;
        if (visibility > 0.001f)
        {
            track.exposure = std::min(track.exposure + visibility * Tuning().exposureGain * dt, 1.0f);
            if (track.exposure < 1.0f)
            {
                // Not made out yet, but something is there: stop and look at it.
                m_alertPoint = player.feet + glm::vec3(0.0f, player.height * 0.6f, 0.0f);
                m_alertUntil = senses.time + 0.6f;
            }
        }
        else
        {
            track.exposure = std::max(track.exposure - Tuning().exposureDecay * dt, 0.0f);
        }

        if (visibility > 0.001f)
        {
            check.verdict = track.exposure >= 1.0f ? "seen" : "making them out";
        }
        track.sight = check;

        if (track.exposure >= 1.0f && visibility > 0.001f)
        {
            if (track.lastSeen < 0.0f || senses.time - track.lastSeen > 3.0f)
            {
                Log(senses.time, "sees " + player.name);
            }
            track.visible = true;
            track.confidence = 1.0f;
            track.lastKnown = player.feet;
            // Whatever it was going to look into there has been looked into: it is them. Without
            // this a curious creature went on investigating the glimpse of somebody it could now see
            // plainly, because the glimpse was still an unanswered question.
            if (!m_interest.resolved && glm::distance(m_interest.position, player.feet) < 5.0f)
            {
                m_interest.resolved = true;
            }
            track.lastVelocity = player.velocity;
            track.lastSeen = senses.time;
            Warm(player.feet, dt);
            m_state.arousal = std::min(m_state.arousal + 0.2f * dt * 10.0f, 1.0f);
        }
        else if (track.exposure >= Tuning().suspicion && visibility > 0.001f)
        {
            // Not sure yet. Worth going to look, which is how a glimpse turns into a hunt -- or into
            // nothing, if it arrives and finds the place empty.
            const float strength = 0.35f + 0.4f * track.exposure;
            if (m_interest.resolved || strength > m_interest.strength)
            {
                if (m_interest.resolved)
                {
                    Log(senses.time, "glimpsed something near " + player.name);
                }
                m_interest = {player.feet, strength, senses.time, "a glimpse", false};
            }
        }

        if (!track.visible)
        {
            // Half as fast for somebody it is stalking: it put the wall between them itself, and knows
            // they were there a moment ago. Without this a patient stalker, waiting behind cover it
            // could not see out of, simply forgot who it was waiting for and wandered off.
            //
            // And a quarter as fast for somebody it is lying in wait for: it chose the spot because they
            // have to come past it, and waiting is the whole of what it is doing. A creature at the mouth
            // of a crawlspace forgot the person in it after fifteen seconds and wandered off, when there
            // was nowhere they could have gone but past it.
            const bool stalkingThem = m_behavior == Behavior::Stalk && m_target == track.id;
            const bool waitingForThem = m_behavior == Behavior::Ambush && m_target == track.id;
            track.confidence = std::max(
                track.confidence - dt / m_traits.persistence * (waitingForThem ? 0.25f : stalkingThem ? 0.5f : 1.0f), 0.0f);
        }

        // Gone from sight just now, beside a hiding place: the place is where they might have gone.
        // Right at the door -- it watched them step up to it -- is as good as seeing them get in.
        if (wasVisible && !track.visible)
        {
            for (size_t i = 0; i < senses.hidingPlaces.size(); ++i)
            {
                const float distance = Horizontal(track.lastKnown, senses.hidingPlaces[i].inside);
                if (distance < 1.5f)
                {
                    Suspect(static_cast<int>(i), 0.9f, track.id);
                }
                else if (distance < 3.5f)
                {
                    Suspect(static_cast<int>(i), 0.35f + 0.35f * HidingHabit(), track.id);
                }
            }
        }
    }

    // --- Being watched, and who is alone ------------------------------------------------------
    //
    // Whether each player is looking at it. Only for somebody it can at least half make out: it
    // reads a face turned towards it, and a face it cannot see tells it nothing. And how far each
    // one is from the nearest other living player, because somebody on their own is an opening in
    // a way that somebody with company is not.
    const glm::vec3 body = senses.onCeiling ? senses.eye : senses.position + glm::vec3(0.0f, m_traits.bodyMiddle, 0.0f);
    const float cosWatched = std::cos(glm::radians(50.0f));
    for (const SensedPlayer& player : senses.players)
    {
        Track* track = FindTrack(player.id);
        if (track == nullptr)
        {
            continue;
        }
        // What it can see, it knows. What it cannot -- turned away to run, or behind cover, which
        // blocks the view both ways -- it goes on believing for a few seconds and then does not know:
        // it only just saw them looking, and they are probably still looking.
        //
        // Their gaze, not their line of sight. Somebody staring at the wall it is crouched behind
        // cannot see it, but they are looking its way, and stepping out would put it straight in
        // front of them -- which is what it did, when "cannot see me" counted as "not looking".
        // Being out of sight is what cover is for; the opening is them looking elsewhere.
        if (player.alive && !player.hidden && track->inView)
        {
            const glm::vec3 eye = player.feet + glm::vec3(0.0f, player.height * 0.93f, 0.0f);
            const glm::vec3 toMe = body - eye;
            const float distance = glm::length(toMe);
            const bool looking = glm::length(player.forward) > 1e-4f && distance > 1e-3f &&
                                 glm::dot(glm::normalize(player.forward), toMe / distance) > cosWatched;
            track->lookedLooking = looking && distance < 45.0f;
            track->lookedAt = senses.time;
        }
        track->watchKnown = senses.time - track->lookedAt < 4.0f;
        track->watching = track->watchKnown && track->lookedLooking;
        track->isolation = 99.0f;
        for (const SensedPlayer& other : senses.players)
        {
            if (other.id != player.id && other.alive)
            {
                track->isolation = std::min(track->isolation, Horizontal(player.feet, other.feet));
            }
        }
    }

    // --- Hearing ------------------------------------------------------------------------------
    for (const Noise& noise : m_pendingNoises)
    {
        float reach = noise.reach * m_traits.perception * m_traits.hearing;
        const glm::vec3 ear = senses.eye;
        if (senses.clearLine && !senses.clearLine(noise.position + glm::vec3(0.0f, 0.3f, 0.0f), ear))
        {
            // Through a wall: still heard, not as far.
            reach *= Tuning().throughWalls;
        }
        const float distance = glm::distance(noise.position, ear);
        if (distance >= reach)
        {
            continue;
        }
        // How loud it seems, which falls off like hearing rather than in a straight line: a gunshot
        // twenty metres off through a wall is still unmistakably a gunshot, not a quarter of one.
        // Straight-line falloff scored exactly that, too faint to be worth getting up for.
        const float strength = std::clamp(std::sqrt(1.0f - distance / reach), 0.05f, 1.0f);
        m_heard.push_back({noise, strength, senses.time});
        if (noise.kind == NoiseKind::Voice && noise.player >= 0 &&
            std::find(m_voicesHeard.begin(), m_voicesHeard.end(), noise.player) == m_voicesHeard.end())
        {
            m_voicesHeard.push_back(noise.player);
            if (m_traits.Mimics())
            {
                Log(senses.time, "listens to somebody talking");
            }
        }
        if (noise.player >= 0)
        {
            Warm(noise.position, 0.3f * strength);
        }
        m_state.arousal = std::min(m_state.arousal + strength * 0.3f, 1.0f);

        // Whose it was, when it was somebody's: a footstep puts them roughly there.
        bool knownSource = false;
        if (noise.player >= 0)
        {
            if (Track* track = FindTrack(noise.player); track != nullptr)
            {
                // It knows who that was when it can see them, or when they are the one who hurt it.
                // Then the sound is not a question to go and answer; it is where they are, and
                // hunting them is already an option. Left as a question of its own, the two competed,
                // and a creature shot in the back walked off to "investigate the gunshot" rather than
                // going for the person it knew had fired it.
                knownSource = track->visible || track->harm > 0.0f;
                if (!track->visible)
                {
                    track->lastKnown = noise.position;
                    track->lastHeard = senses.time;
                    const float floor = knownSource ? 0.5f + 0.5f * strength : 0.3f + 0.4f * strength;
                    track->confidence = std::max(track->confidence, floor);
                }
                if (knownSource && noise.kind == NoiseKind::Gunshot)
                {
                    Log(senses.time, "heard " + track->name + " fire" + Format(" (%.0f%%)", strength * 100.0f));
                }
            }
        }
        // A locker door, heard and not seen: somebody has just got into it, or out. It cannot tell
        // which, so it suspects it -- and if the person was in plain sight, it saw where they went and
        // needs no noise to tell it.
        if (noise.kind == NoiseKind::Door)
        {
            const Track* source = noise.player >= 0 ? FindTrack(noise.player) : nullptr;
            const bool seen = source != nullptr && source->visible;
            for (size_t i = 0; i < senses.hidingPlaces.size() && !seen; ++i)
            {
                if (glm::distance(noise.position, senses.hidingPlaces[i].inside) < 1.5f)
                {
                    Suspect(static_cast<int>(i), 0.9f, noise.player);
                    ++m_lockersHeard;
                    Log(senses.time, "heard a locker door");
                }
            }
        }

        if (knownSource)
        {
            ResolveInterestNear(noise.position);
            continue;
        }

        // A gunshot or a door is worth more than a footstep at the same loudness: it is the sound of
        // somebody doing something.
        const float weight = noise.kind == NoiseKind::Gunshot  ? 1.0f
                             : noise.kind == NoiseKind::Impact ? 0.7f
                             : noise.kind == NoiseKind::Door   ? 0.8f
                                                               : 0.6f;
        const float pull = strength * weight;
        const float current = m_interest.resolved
                                  ? 0.0f
                                  : m_interest.strength *
                                        std::max(0.0f, 1.0f - (senses.time - m_interest.time) * 0.05f);
        const bool gunshot = noise.kind == NoiseKind::Gunshot;
        if (gunshot && (m_traits.temperament == Temperament::Timid || m_traits.fear > 0.7f))
        {
            // Somewhere with shooting in it is somewhere the fearful keep away from for a while.
            RememberDanger(noise.position, senses.time + 40.0f);
        }
        // The same shooting again: another shot from about the same place.
        const bool again = !m_interest.resolved && m_interest.gunfire && gunshot &&
                           Horizontal(m_interest.position, noise.position) < 8.0f && senses.time - m_interest.time < 8.0f;
        if (pull > current)
        {
            if (m_interest.resolved || pull > current + 0.2f)
            {
                Log(senses.time, std::string("heard a ") + NoiseKindName(noise.kind) +
                                     Format(" (%.0f%%)", pull * 100.0f));
            }
            m_interest = {noise.position, pull, senses.time, NoiseKindName(noise.kind), false, gunshot,
                          again ? m_interest.shots + 1 : 0};
        }
        else if (again)
        {
            ++m_interest.shots;
            m_interest.time = senses.time;
        }
    }
    while (!m_heard.empty() && senses.time - m_heard.front().time > kHeardMemorySeconds)
    {
        m_heard.pop_front();
    }

    PerceivePlaces(senses, dt);

    // Where people go, cooling: a place nobody has been seen in for a few minutes is forgotten.
    for (HeatCell& cell : m_heat)
    {
        cell.heat -= cell.heat * dt / 150.0f;
    }
    std::erase_if(m_heat, [](const HeatCell& cell) { return cell.heat < 0.02f; });

    // --- Feelings -----------------------------------------------------------------------------
    m_state.pain = std::max(m_state.pain - 0.06f * dt, 0.0f);
    m_state.arousal = std::max(m_state.arousal - 0.1f * dt, 0.0f);
    const float injury = 1.0f - std::clamp(senses.healthFraction, 0.0f, 1.0f);
    // A wound frightens it while whatever gave it the wound is about: somebody in sight and close,
    // or a round in the last twenty seconds. Alone in the dark it is still hurt, and much less
    // afraid. Fear that came from the wound alone never went away -- nothing mends a creature that
    // has no nest -- so a timid one shot badly enough spent the rest of the match running from
    // nobody, back and forth between a hiding place and the next hiding place.
    m_threatened = senses.time - m_lastHurt < 20.0f;
    for (const Track& track : m_tracks)
    {
        if (track.visible && Horizontal(senses.position, track.lastKnown) < 14.0f)
        {
            m_threatened = true;
        }
    }
    const float woundFear = injury * m_traits.fear * (m_threatened ? 0.5f : 0.12f);
    m_state.fear = std::clamp(m_state.pain * (0.4f + m_traits.fear) + woundFear, 0.0f, 1.0f);
}

void CreatureBrain::UpdateHostility(const CreatureSenses& senses, float dt)
{
    const float now = senses.time;
    for (Track& track : m_tracks)
    {
        const SensedPlayer* player = FindPlayer(senses, track.id);
        const bool was = track.hostile;
        if (player == nullptr || !player->alive)
        {
            track.hostile = false;
            continue;
        }
        const glm::vec3 them = track.visible ? player->feet : track.lastKnown;
        const float distance = Horizontal(senses.position, them);
        // Hurt by them in the last minute: whatever it is, it is dangerous to them now.
        const bool provoked = now - track.provokedAt < 60.0f;
        bool hostile = true;
        switch (m_traits.temperament)
        {
        case Temperament::Predator:
            hostile = true;
            break;
        case Temperament::Curious:
            hostile = provoked;
            break;
        case Temperament::Timid:
            // Only at arm's length, and only once it has been hurt by them or has run out of room.
            hostile = distance < m_traits.strikeReach + 0.8f && (provoked || m_cornered > 2.5f);
            break;
        case Temperament::Territorial:
        {
            // On its ground after being warned off it, the time they stay counts against them; off
            // it, the count wears away. Coming right up to it is the same as staying.
            const bool onGround = Horizontal(them, m_home) < kTerritory;
            if (onGround && track.visible && now - track.warnedAt < 30.0f)
            {
                track.trespass += dt;
            }
            else if (!onGround)
            {
                track.trespass = std::max(track.trespass - dt * 0.5f, 0.0f);
            }
            const bool pressing = onGround && (distance < 5.0f || track.trespass > 5.0f);
            // Chased off its ground and a little beyond it, but not across the map.
            const bool nearGround = Horizontal(them, m_home) < kTerritory + 12.0f;
            hostile = provoked || pressing || (was && nearGround);
            break;
        }
        case Temperament::Count:
            break;
        }
        track.hostile = hostile;
        if (hostile != was)
        {
            Log(now, hostile ? track.name + " is a threat to it" : "leaves " + track.name + " be");
        }
    }
}

void CreatureBrain::Decide(const CreatureSenses& senses)
{
    // Lying still is not weighed tick by tick against everything else: an act abandoned the moment
    // something else scored a little higher would not be an act. It ends on its own terms, in
    // UpdatePlayingDead.
    if (m_behavior == Behavior::PlayDead)
    {
        return;
    }
    // Nor is a blow it has already started. Half way through a wind-up -- or up off the floor and
    // going for somebody who walked up to its body -- the moment for weighing it has gone; a creature
    // that sprang from playing dead and then thought better of it, fear being what it was, was
    // standing up in front of somebody to run away from them.
    if (senses.time < m_committedUntil || (m_behavior == Behavior::Attack && m_attack != AttackKind::None))
    {
        return;
    }
    // Nor carrying somebody off: that ends when it has done with them, or is made to let go.
    if (m_behavior == Behavior::Drag && m_holding >= 0)
    {
        return;
    }
    // Nor building: a nest half dug is no nest at all.
    if (m_behavior == Behavior::Nest && m_nestWorkStarted >= 0.0f)
    {
        return;
    }
    m_options.clear();
    const float now = senses.time;
    const auto add = [this](Behavior behavior, int target, std::string label,
                            std::vector<Consideration> considerations)
    {
        Option option;
        option.behavior = behavior;
        option.target = target;
        option.label = std::move(label);
        option.considerations = std::move(considerations);
        option.score = 1.0f;
        for (const Consideration& c : option.considerations)
        {
            option.score *= c.value;
        }
        m_options.push_back(std::move(option));
    };

    const float calm = 1.0f - m_state.fear;

    // Wandering is always there, and always poor: it is what it does when nothing else scores.
    add(Behavior::Roam, -1, "Roam", {{"nothing better", 0.15f}});

    // Going to look at whatever it last noticed.
    if (!m_interest.resolved)
    {
        const float age = now - m_interest.time;
        const float recent = std::clamp(1.0f - age / 20.0f, 0.0f, 1.0f);
        // Not towards somebody it has just been keeping away from. A noise near a person a timid one
        // saw a moment ago is that person, and going to look is walking back to them: it did, saw them,
        // backed off, heard them, and went to look again, over and over.
        float wary = 1.0f;
        if (m_traits.temperament == Temperament::Timid)
        {
            for (const Track& track : m_tracks)
            {
                if (track.lastSeen >= 0.0f && now - track.lastSeen < 25.0f &&
                    Horizontal(track.lastKnown, m_interest.position) < 12.0f)
                {
                    wary = 0.1f;
                }
            }
        }
        // Anywhere it has lately been frightened -- by somebody it kept away from, or by shooting.
        if (NearDanger(m_interest.position, 14.0f, now))
        {
            wary = std::min(wary, m_traits.temperament == Temperament::Timid ? 0.1f : 0.5f);
        }
        // Shooting is not walked straight at by anything with sense: it goes round, out of sight, and
        // listens before it goes in. Only the brazen come straight, and anything already close.
        const float caution = std::max(m_traits.stealth, 0.8f * m_traits.fear + 0.3f * (1.0f - m_traits.aggression));
        const bool goRound = m_interest.gunfire && caution > 0.3f &&
                             (Horizontal(senses.position, m_interest.position) > 9.0f || m_behavior == Behavior::Flank);
        // A lot of it is a fight, which the fearful keep out of.
        const float fight = m_interest.shots >= 2 ? 1.0f - 0.6f * m_traits.fear : 1.0f;
        if (goRound)
        {
            add(Behavior::Flank, -1, "Go round to the " + m_interest.what,
                {{"how strong", m_interest.strength},
                 {"wants to know", 0.45f + 0.55f * std::max(m_traits.curiosity, m_traits.aggression)},
                 {"not afraid", 0.3f + 0.7f * calm},
                 {"recent", std::max(recent, m_behavior == Behavior::Flank ? 0.6f : 0.0f)},
                 {"not them again", wary},
                 {"a fight", fight}});
        }
        else
        {
            add(Behavior::Investigate, -1, "Investigate " + m_interest.what,
                {{"how strong", m_interest.strength},
                 {"curiosity", 0.4f + 0.6f * m_traits.curiosity},
                 {"not afraid", 0.3f + 0.7f * calm},
                 {"recent", recent},
                 {"not them again", wary},
                 {"a fight", fight}});
        }
    }

    // Each person it knows of, separately: hunting one and ignoring another is a real choice.
    for (const Track& track : m_tracks)
    {
        const SensedPlayer* player = FindPlayer(senses, track.id);
        const bool searchingThem = m_behavior == Behavior::Search && m_target == track.id;
        const bool ambushingThem = m_behavior == Behavior::Ambush && m_target == track.id;
        if (player == nullptr || !player->alive || (track.confidence <= 0.05f && !searchingThem && !ambushingThem))
        {
            continue;
        }
        const float distance = Horizontal(senses.position, track.visible ? player->feet : track.lastKnown);

        // Keeping away from them: what a timid one does about anybody near, until it has no room left.
        // Kept up for a while after it loses sight of them, rather than dropped the moment they are out
        // of view: out of view is where it was trying to get to, not a reason to turn round.
        const bool keptAway = m_behavior == Behavior::Avoid && m_target == track.id && now - track.lastSeen < 12.0f;
        if (m_traits.temperament == Temperament::Timid && !track.hostile && distance < 18.0f &&
            (track.visible || keptAway || (track.confidence > 0.6f && distance < 10.0f)))
        {
            add(Behavior::Avoid, track.id, "Keep away from " + track.name,
                {{"timid", 0.6f + 0.4f * m_traits.fear},
                 {"too close", std::clamp(1.3f - distance / 18.0f, 0.3f, 1.0f)}});
        }
        // Warning them off: a territorial one, at somebody on its ground it has not yet decided to fight.
        if (m_traits.temperament == Temperament::Territorial && !track.hostile && track.visible &&
            distance < 14.0f && Horizontal(player->feet, m_home) < kTerritory)
        {
            add(Behavior::Warn, track.id, "Warn off " + track.name,
                {{"on its ground", 1.0f},
                 {"close", std::clamp(1.2f - distance / 14.0f, 0.4f, 1.0f)},
                 {"not afraid", 0.4f + 0.6f * calm}});
        }

        // Nothing below is for somebody it does not mean harm to, apart from watching them. A timid one
        // that does mean them harm only ever means it at arm's length: it does not chase.
        const bool chases = track.hostile && m_traits.temperament != Temperament::Timid;

        // Closing the distance is what hunting is. Once they are within reach there is nothing left to
        // close, and hunting scores low -- otherwise a creature standing on top of somebody went on
        // "hunting" them, because for an aggressive one the two scored within the commitment margin.
        const float closeness = distance < m_traits.strikeReach ? 0.5f : std::clamp(1.2f - distance / 30.0f, 0.4f, 1.0f);
        const float grudge = std::min(1.0f + track.harm * 0.5f, 1.5f);
        // How much it cares that this is a good moment. A brazen creature comes regardless; a
        // stealthy one comes when they are alone, looking away, or out of its patience.
        const float opening = Opening(track, distance);
        const float timing = 1.0f - m_traits.stealth * (1.0f - opening);
        // In a crawlspace too small for it: going after them only puts it on the roof of the thing.
        const bool beyond = OutOfItsReach(senses, *player);
        const float follow = beyond ? 0.15f : 1.0f;
        if (chases)
        {
            add(Behavior::Hunt, track.id, "Hunt " + track.name,
                {{"sure where", track.confidence},
                 {"aggression", 0.35f + 0.65f * m_traits.aggression},
                 {"not afraid", calm},
                 {"still to close", closeness},
                 {"grudge", grudge},
                 {"the moment", timing},
                 {"can follow", follow}});
        }

        // Lying in wait for them, where they will have to come: the mouth of the crawlspace it cannot
        // follow them into, or -- for the patient -- the far side of a door from them, once they are out
        // of sight. Planned a couple of times a second at most, and kept while it is waiting.
        const bool waitingForThem = m_behavior == Behavior::Ambush && m_ambush.valid && m_ambush.target == track.id;
        if (chases && !player->hidden && (beyond || (!track.visible && track.lastSeen >= 0.0f && now - track.lastSeen > 1.5f &&
                                                     track.confidence > 0.3f && m_traits.stealth > 0.35f) ||
                                          waitingForThem))
        {
            if (!waitingForThem && now - m_ambush.plannedAt > 1.5f)
            {
                AmbushPlan plan;
                if (PlanAmbush(senses, track, player, plan))
                {
                    plan.plannedAt = now;
                    m_ambush = plan;
                }
                else
                {
                    m_ambush.valid = false;
                    m_ambush.plannedAt = now;
                }
            }
            if (m_ambush.valid && m_ambush.target == track.id)
            {
                const float waited = waitingForThem && m_ambushArrived ? now - m_ambushSince : 0.0f;
                add(Behavior::Ambush, track.id, std::string(m_ambush.kind == AmbushKind::Crawlspace ? "Wait at the crawlspace for "
                                                                                                    : "Wait by the door for ") +
                                                    track.name,
                    {{"sure where", std::max(track.confidence, beyond ? 0.9f : 0.0f)},
                     {beyond ? "cannot follow" : "stealthy", beyond ? 1.0f : 0.35f + 0.65f * m_traits.stealth},
                     {"patient", 0.4f + 0.6f * m_traits.patience},
                     {"a way they must come", std::max(m_ambush.quality, 0.3f)},
                     {"not afraid", 0.3f + 0.7f * calm},
                     {"still waiting", std::clamp(1.0f - waited / (Tuning().ambushPatience + Tuning().ambushPatienceRange * m_traits.patience), 0.1f, 1.0f)}});
            }
        }

        // Drawing them to it with a voice they know: the cunning ones, at somebody not in its sight or a
        // good way off, once in a while -- a trick used every minute is a trick everybody knows.
        const bool luring = m_behavior == Behavior::Lure && m_target == track.id;
        if (chases && m_traits.Mimics() && !beyond && track.lastSeen >= 0.0f &&
            (luring || ((!track.visible || distance > 14.0f) && now - m_lastLureAt > Tuning().lureEvery)))
        {
            const int voice = luring ? m_lureVoice : VoiceFor(senses, track.id);
            if (voice >= 0)
            {
                add(Behavior::Lure, track.id, "Lure " + track.name,
                    {{"sure where", std::max(track.confidence, 0.4f)},
                     {"cunning", 0.4f + 0.6f * m_traits.stealth},
                     {"a voice they know", voice != track.id ? 1.0f : 0.6f},
                     {"patient", 0.5f + 0.5f * m_traits.patience},
                     {"not afraid", 0.3f + 0.7f * calm},
                     {"still at it", luring ? std::clamp(1.0f - static_cast<float>(m_lureSpoken) / 5.0f, 0.2f, 1.0f) : 1.0f}});
            }
        }

        // Shadowing them from cover instead. Only somebody it has actually seen: somebody it has
        // only heard is a question to go and answer, not a person to follow.
        if (chases && !player->hidden && track.lastSeen >= 0.0f && track.confidence > 0.3f && distance < 35.0f &&
            distance > m_traits.strikeReach + 0.4f)
        {
            const float patienceLeft =
                std::clamp(1.0f - track.stalked / m_traits.StalkPatienceSeconds(), 0.05f, 1.0f);
            add(Behavior::Stalk, track.id, "Stalk " + track.name,
                {{"sure where", track.confidence},
                 {"stealthy", 0.2f + 0.8f * m_traits.stealth},
                 {"not afraid", 0.3f + 0.7f * calm},
                 {"patience left", patienceLeft},
                 {"waiting for a chance", 1.0f - 0.8f * opening},
                 {"can follow", beyond ? 0.3f : 1.0f},
                 // Shadowing is for somebody it can see. Out of sight for a while, lying in wait where they
                 // will come is the better use of patience.
                 {"in sight", track.visible || now - track.lastSeen < 2.0f || !m_ambush.valid || m_ambush.target != track.id
                                  ? 1.0f
                                  : std::max(0.35f, 1.0f - 0.13f * (now - track.lastSeen - 2.0f))}});
        }

        // Watching them, out of curiosity rather than hunger: a creature that has never seen one of
        // these before and is not sure yet what it is. Somebody who has hurt it is not a curiosity.
        // Wears off with watching -- sooner for a less curious one -- and what it does next is
        // whatever else was scoring underneath.
        // Somebody it is already watching stays watched through a glance away: losing sight for a
        // moment is not a reason to stop, and without this it dropped them every time it turned.
        const bool watchingThem = m_behavior == Behavior::Observe && m_target == track.id &&
                                  now - track.lastSeen < 3.0f;
        if ((track.visible || watchingThem) && track.harm <= 0.0f)
        {
            add(Behavior::Observe, track.id, "Watch " + track.name,
                {{"curious", 0.3f + 0.7f * m_traits.curiosity},
                 {"not hungry", 0.2f + 0.8f * (1.0f - m_traits.aggression)},
                 {"not afraid", 0.3f + 0.7f * calm},
                 {"still new", std::clamp(1.0f - track.observed / m_traits.BoredomSeconds(), 0.0f, 1.0f)}});
        }

        // Searching for them: somebody it had, and lost. As sure-where fades, going to where they
        // were stops being enough and going through where they could have gone takes over.
        // Once it has started, it is not a question of how sure it still is -- that is what searching
        // is for -- but of how long ago it had them, which its persistence decides.
        if (chases && !track.visible && track.lastSeen >= 0.0f &&
            ((track.confidence > 0.05f && track.confidence < 0.65f) || searchingThem))
        {
            const float since = now - std::max(track.lastSeen, track.lastHeard);
            add(Behavior::Search, track.id, "Search for " + track.name,
                {{"can follow", follow},
                 {"lost them", std::max(1.0f - track.confidence, 0.5f)},
                 {"persistent", 0.4f + 0.6f * std::min(m_traits.persistence / 24.0f, 1.0f)},
                 {"wants them", 0.5f + 0.5f * m_traits.aggression},
                 {"not afraid", 0.3f + 0.7f * calm},
                 {"fresh", std::clamp(1.0f - since / (m_traits.persistence * 3.0f), 0.0f, 1.0f)}});
        }

        // Going for them: in reach -- up on something too, if it can rear up that high -- or near enough
        // to throw itself at them.
        const float rise = player->feet.y - senses.position.y;
        const bool canRise = rise < senses.verticalReach && rise > -1.6f;
        const float gap = Horizontal(senses.position, player->feet);
        // And nothing solid between: in reach through a wall, or through the roof of a crawlspace, is not in reach.
        const bool clearToThem = !senses.clearLine ||
                                 senses.clearLine(senses.eye, player->feet + glm::vec3(0.0f, player->height * 0.6f, 0.0f));
        const bool inReach = gap < m_traits.strikeReach + 0.4f && clearToThem;
        // Not into a crawlspace it cannot follow them into: a swipe in at arm's length is all it has there.
        const bool pounce = gap > 2.6f && gap < 7.0f && std::abs(rise) < 0.8f && now >= m_lungeReadyAt && !beyond && clearToThem;
        if (track.hostile && track.visible && canRise && (inReach || (pounce && chases)))
        {
            add(Behavior::Attack, track.id, "Attack " + track.name,
                {{inReach ? "in reach" : "pounce", inReach ? 1.0f : 0.55f + 0.4f * m_traits.aggression},
                 {"aggression", 0.3f + 0.7f * m_traits.aggression},
                 {"not afraid", 0.2f + 0.8f * calm},
                 // Out of hiding at somebody who has walked into it: the moment all the waiting was for.
                 {"from hiding", m_behavior == Behavior::Ambush || m_behavior == Behavior::Stalk ||
                                         m_behavior == Behavior::Lure
                                     ? 1.4f
                                     : 1.0f}});
        }
    }

    // Building a nest: what the sort that nests does when nothing is happening.
    if (m_traits.Nests() && !senses.hasHive && senses.mayBuildNest)
    {
        float quiet = 1.0f;
        for (const Track& track : m_tracks)
        {
            quiet = std::min(quiet, 1.0f - track.confidence);
        }
        add(Behavior::Nest, -1, "Build a nest",
            {{"a nester", m_traits.nesting},
             {"nothing else about", quiet},
             {"not afraid", 0.3f + 0.7f * calm}});
    }

    // Getting away. Once it has decided to, it keeps going for a while rather than turning back the
    // moment the pain fades a little -- a wounded animal does not stop running at the first corner.
    const bool retreating = m_behavior == Behavior::Retreat && now < m_retreatUntil;
    if (m_behavior == Behavior::Retreat && !retreating && now >= m_retreatCooldownUntil)
    {
        // It has run for as long as it meant to. Running is then off the table for a while, even
        // afraid: left on it, the same fear that started the retreat chose it again the moment it
        // ended, and the creature never came back. What it does instead is up to everything else --
        // a frightened one will still prefer to keep its distance, just not by fleeing from nothing.
        m_retreatCooldownUntil = now + 12.0f;
    }
    if (retreating || (m_state.fear > 0.3f && now >= m_retreatCooldownUntil))
    {
        add(Behavior::Retreat, -1, "Retreat",
            {{"afraid", retreating ? std::max(m_state.fear, 0.6f) : m_state.fear},
             {"timid", 0.6f + 0.6f * m_traits.fear}});
    }

    // Dropping as if dead, instead of running: the other answer an animal has to something it cannot
    // outrun. Only straight after a wound that has left it badly hurt, with somebody close -- a
    // creature that lay down at random would be found out -- and not more than twice, because the
    // second time is already a trick the players know. The patient, stealthy ones do it; the rest run.
    float nearest = 1.0e9f;
    for (const Track& track : m_tracks)
    {
        if (track.confidence > 0.3f)
        {
            nearest = std::min(nearest, Horizontal(senses.position, track.lastKnown));
        }
    }
    if (m_playDeadCount < 2 && now - m_lastHurt < 1.2f && senses.healthFraction < 0.6f && nearest < 15.0f)
    {
        add(Behavior::PlayDead, -1, "Play dead",
            {{"afraid", m_state.fear},
             {"cunning", 0.3f + 0.45f * m_traits.stealth + 0.45f * m_traits.patience},
             {"badly hurt", std::clamp(1.5f - senses.healthFraction, 0.5f, 1.0f)},
             {"cannot outrun them", nearest < 8.0f ? 1.0f : 0.6f}});
    }

    // Going straight back to what it has only just given up needs a better reason than it had for giving
    // it up, fading over eight seconds. Striking is never held back like this, nor wandering.
    const float sinceLeft = now - m_leftAt;
    if (sinceLeft < 8.0f && m_leftBehavior != m_behavior && m_leftBehavior != Behavior::Attack &&
        m_leftBehavior != Behavior::Roam)
    {
        for (Option& option : m_options)
        {
            if (option.behavior == m_leftBehavior && option.target == m_leftTarget)
            {
                const float settle = 0.6f + 0.4f * sinceLeft / 8.0f;
                option.considerations.push_back({"only just gave it up", settle});
                option.score *= settle;
            }
        }
    }

    std::sort(m_options.begin(), m_options.end(),
              [](const Option& a, const Option& b) { return a.score > b.score; });

    // What it is doing now gets a head start.
    const Option* current = nullptr;
    for (const Option& option : m_options)
    {
        if (option.behavior == m_behavior && option.target == m_target)
        {
            current = &option;
            break;
        }
    }
    const Option& best = m_options.front();
    // No head start for wandering: the margin is there to stop two real plans taking turns, and
    // wandering is not a plan worth holding on to against anything at all.
    const float commitment = m_behavior == Behavior::Roam ? 0.0f : Tuning().commitment;
    if (current != nullptr && (current == &best || best.score < current->score + commitment))
    {
        return;
    }

    // The reason is the considerations of the winner, the strongest first, because "Hunt Joe 0.71:
    // sure where 1.00, aggression 0.84" says why at a glance and "switched to Hunt" does not.
    std::string reason = Format("%.2f", best.score);
    std::vector<Consideration> ordered = best.considerations;
    std::sort(ordered.begin(), ordered.end(),
              [](const Consideration& a, const Consideration& b) { return a.value > b.value; });
    for (size_t i = 0; i < ordered.size() && i < 3; ++i)
    {
        reason += std::string(i == 0 ? " (" : ", ") + ordered[i].name + Format(" %.2f", ordered[i].value);
    }
    if (!ordered.empty())
    {
        reason += ")";
    }
    Switch(best.behavior, best.target, best.label + " " + reason, now);
}

void CreatureBrain::Switch(Behavior behavior, int target, const std::string& reason, float time)
{
    const Behavior previous = m_behavior;
    if (previous != behavior || m_target != target)
    {
        m_leftBehavior = previous;
        m_leftTarget = m_target;
        m_leftAt = time;
    }
    m_behavior = behavior;
    m_target = target;
    m_behaviorStarted = time;
    m_arrived = false;
    m_windupStarted = -1.0f;
    m_haveRoamPoint = false;
    m_opening = -1;
    m_openStarted = -1.0f;
    m_searchPlan.clear();
    m_searchStep = 0;
    m_lookAroundUntil = 0.0f;
    m_attack = AttackKind::None;
    m_lookBaseSet = false;
    m_nestWorkStarted = -1.0f;
    m_unreachableSince = -1.0f;
    m_door = -1;
    if (behavior == Behavior::Retreat)
    {
        m_haveFleePoint = false;
        m_retreatUntil = time + 6.0f + 6.0f * m_traits.fear;
    }
    if (behavior == Behavior::Stalk)
    {
        m_haveStalkPoint = false;
        m_stalkCheckAt = time;
        m_peeking = false;
        m_nextPeekAt = time + 2.0f;
    }
    if (behavior == Behavior::Attack)
    {
        // It went for them: the waiting is over, and a later stalk starts its patience afresh.
        if (Track* track = FindTrack(target))
        {
            track->stalked = 0.0f;
        }
    }
    if (behavior == Behavior::Ambush && previous != Behavior::Ambush)
    {
        m_ambushArrived = false;
    }
    if (behavior == Behavior::Flank)
    {
        m_flankStage = 0;
    }
    if (behavior == Behavior::Avoid && previous != Behavior::Avoid)
    {
        m_avoidFleeing = false;
        // A while since it last had to, it is willing to stand and watch again.
        if (time - m_avoidLastAt > 60.0f)
        {
            m_avoidPushed = 0;
        }
    }
    if (previous == Behavior::Avoid)
    {
        m_avoidLastAt = time;
    }
    if (behavior == Behavior::Lure && previous != Behavior::Lure)
    {
        m_haveLureSpot = false;
        m_lureSpoken = 0;
        m_lureVoice = -1;
    }
    if (previous == Behavior::Lure && behavior != Behavior::Lure)
    {
        m_lastLureAt = time;
    }
    if (behavior == Behavior::PlayDead)
    {
        // As long as it can bear to, which is as long as it is patient.
        m_playDeadUntil = time + 12.0f + 25.0f * m_traits.patience;
        ++m_playDeadCount;
        m_hurtWhileDown = false;
    }
    if (previous == Behavior::Retreat && behavior != Behavior::Retreat)
    {
        m_retreatCooldownUntil = time + 12.0f;
    }
    Log(time, std::string(BehaviorName(previous)) + " -> " + reason);
}

bool CreatureBrain::PickFleePoint(const CreatureSenses& senses, glm::vec3& out)
{
    if (senses.nav == nullptr)
    {
        return false;
    }
    // A handful of candidates, each scored on being far from what hurt it and out of sight of
    // everybody it knows about. Hiding emerges from that rather than from a list of hiding places.
    float bestScore = -1.0e9f;
    bool found = false;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 16; ++i)
    {
        glm::vec3 candidate;
        if (!senses.nav->RandomPointNear(senses.position, 30.0f, seed, candidate))
        {
            continue;
        }
        float score = Horizontal(candidate, m_threat) - Horizontal(candidate, senses.position) * 0.25f;
        bool seen = false;
        for (const Track& track : m_tracks)
        {
            if (track.confidence > 0.1f && senses.clearLine &&
                senses.clearLine(track.lastKnown + glm::vec3(0.0f, 1.6f, 0.0f),
                                 candidate + glm::vec3(0.0f, 0.9f, 0.0f)))
            {
                seen = true;
            }
        }
        if (!seen)
        {
            score += 12.0f;
        }
        score += 8.0f * ShelterOf(senses, candidate);
        if (score > bestScore)
        {
            bestScore = score;
            out = candidate;
            found = true;
        }
    }
    return found;
}

float CreatureBrain::SightRange() const
{
    return Tuning().sightRange * m_traits.perception * std::max(m_traits.sight, 0.0f);
}

float CreatureBrain::CloseSense()
{
    return Tuning().closeSense;
}

float CreatureBrain::Opening(const Track& track, float distance) const
{
    float opening = 1.0f;
    // Looking straight at it: the worst moment there is. Not knowing whether they are is half as bad.
    if (track.watching)
    {
        opening *= 0.2f;
    }
    else if (!track.watchKnown)
    {
        opening *= 0.6f;
    }
    // With somebody nearby, who would see it happen and come.
    if (track.isolation < 10.0f)
    {
        opening *= 1.0f - 0.7f * m_traits.isolationPreference;
    }
    // Close enough that waiting gains nothing.
    if (distance < 4.0f)
    {
        opening = 1.0f;
    }
    // And past the end of its patience, any moment is the moment.
    const float impatience = std::clamp(track.stalked / m_traits.StalkPatienceSeconds(), 0.0f, 1.0f);
    return std::max(opening, impatience);
}

bool CreatureBrain::SeenFrom(const CreatureSenses& senses, const glm::vec3& point) const
{
    if (!senses.clearLine)
    {
        return false;
    }
    // From where it believes each of them is, not where they are: it can only hide from what it
    // knows about.
    const glm::vec3 body = point + glm::vec3(0.0f, m_traits.bodyMiddle, 0.0f);
    for (const Track& track : m_tracks)
    {
        if (track.confidence < 0.2f)
        {
            continue;
        }
        const glm::vec3 eye = track.lastKnown + glm::vec3(0.0f, 1.6f, 0.0f);
        if (glm::distance(eye, body) < 45.0f && senses.clearLine(eye, body))
        {
            return true;
        }
    }
    return false;
}

bool CreatureBrain::FindCover(const CreatureSenses& senses, const Track& target, const SensedPlayer* player,
                              glm::vec3& out)
{
    m_cover.clear();
    if (senses.nav == nullptr)
    {
        return false;
    }
    // Where they will be in a moment, not where they are: it is following somebody who is moving.
    glm::vec3 centre = target.visible && player != nullptr ? player->feet : target.lastKnown;
    const glm::vec3 moving{target.lastVelocity.x, 0.0f, target.lastVelocity.z};
    const float pace = glm::length(moving);
    if (target.visible && pace > 0.5f)
    {
        glm::vec3 ahead;
        if (senses.nav->NearestPoint(centre + moving * 1.5f, 2.0f, ahead))
        {
            centre = ahead;
        }
    }
    glm::vec3 facing{0.0f};
    if (player != nullptr && target.visible)
    {
        facing = glm::vec3(player->forward.x, 0.0f, player->forward.z);
    }
    if (glm::length(facing) > 1e-3f)
    {
        facing = glm::normalize(facing);
    }

    // Candidates scattered round them, each scored on named grounds and multiplied, the same way the
    // options are. The place it is already in is one of them, with a little credit for being where
    // it already is, so it does not hop between two nearly equal spots.
    // Round them, where cover is wanted, and round itself, where cover is quickest to reach -- the
    // navigation mesh's random points bunch towards the middle of the circle asked for, so asking
    // round both finds far more of the ring in between than asking round either.
    std::vector<glm::vec3> candidates;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 48; ++i)
    {
        glm::vec3 candidate;
        const bool nearThem = i < 32;
        if (senses.nav->RandomPointNear(nearThem ? centre : senses.position, nearThem ? 22.0f : 12.0f, seed,
                                        candidate))
        {
            candidates.push_back(candidate);
        }
    }
    if (m_haveStalkPoint)
    {
        candidates.push_back(m_stalkPoint);
    }

    float best = 0.0f;
    bool found = false;
    for (const glm::vec3& candidate : candidates)
    {
        const float distance = Horizontal(candidate, centre);
        if (distance < 6.0f || distance > 20.0f)
        {
            continue;
        }
        CoverCandidate scored;
        scored.position = candidate;
        scored.hidden = !SeenFrom(senses, candidate);
        // Out of sight is nearly everything. Somewhere they can see it is barely cover at all -- low
        // enough that no combination of the other reasons lifts it over somewhere hidden, and only
        // chosen when there is nowhere hidden at all.
        float score = scored.hidden ? 1.0f : 0.01f;
        // About eleven metres: near enough to close in a couple of seconds, far enough not to be heard.
        score *= 1.0f - std::min(std::abs(distance - 11.0f) / 12.0f, 0.7f);
        // In the dark.
        if (senses.lightAt)
        {
            score *= 1.1f - 0.5f * std::clamp(senses.lightAt(candidate + glm::vec3(0.0f, 0.5f, 0.0f)), 0.0f, 1.0f);
        }
        // Behind them rather than in front.
        if (glm::length(facing) > 1e-3f)
        {
            const glm::vec3 away = glm::normalize(glm::vec3(candidate.x - centre.x, 0.0f, candidate.z - centre.z));
            score *= 0.75f - 0.25f * glm::dot(facing, away);
        }
        // Not on the far side of the map: somewhere forty metres away is a journey, not cover.
        score *= 1.0f - std::min(Horizontal(candidate, senses.position) / 40.0f, 0.6f);
        // Against something. Out of their sight in the middle of an empty floor is not hiding, it is
        // standing where they happen not to be looking from -- a step to either side and it is in plain
        // view -- and it is what it did, over and over. Behind a wall between it and them is best.
        score *= 0.2f + 0.8f * ShelterOf(senses, candidate);
        if (TightCover(senses, candidate, centre))
        {
            score *= 1.6f;
        }
        // The patient wait ahead of somebody on the move, where they are going; the rest follow on.
        if (pace > 1.0f && m_traits.patience > 0.55f)
        {
            score *= 0.8f + 0.4f * std::max(glm::dot(moving / pace, Flat(candidate - centre)), 0.0f);
        }
        // And not on the far side of them. The best hiding place in the world is no use if the way to
        // it is straight past their nose -- which is what it did, walking slowly at somebody watching
        // it to reach cover behind them. Measured by how close the straight line there passes them.
        {
            const glm::vec2 from{senses.position.x, senses.position.z};
            const glm::vec2 to{candidate.x, candidate.z};
            const glm::vec2 them{centre.x, centre.z};
            const glm::vec2 along = to - from;
            const float length2 = glm::dot(along, along);
            const float t = length2 > 1e-4f ? std::clamp(glm::dot(them - from, along) / length2, 0.0f, 1.0f) : 0.0f;
            const float passes = glm::length(from + along * t - them);
            if (passes < 6.0f)
            {
                score *= 0.1f + 0.9f * passes / 6.0f;
            }
        }
        if (m_haveStalkPoint && Horizontal(candidate, m_stalkPoint) < 0.5f)
        {
            score *= 1.15f;
        }
        scored.score = score;
        m_cover.push_back(scored);
        if (score > best)
        {
            best = score;
            out = candidate;
            found = true;
        }
    }
    return found;
}

void CreatureBrain::Warm(const glm::vec3& where, float amount)
{
    const int x = static_cast<int>(std::floor(where.x / kHeatCell));
    const int z = static_cast<int>(std::floor(where.z / kHeatCell));
    for (HeatCell& cell : m_heat)
    {
        if (cell.x == x && cell.z == z)
        {
            cell.heat = std::min(cell.heat + amount, 10.0f);
            return;
        }
    }
    // Bounded: a match is not long enough to fill it, but a bug that warmed every square should not
    // turn into memory it cannot give back.
    if (m_heat.size() < 512)
    {
        m_heat.push_back({x, z, std::min(amount, 10.0f)});
    }
}

bool CreatureBrain::PickWarmPlace(const CreatureSenses& senses, glm::vec3& out)
{
    if (senses.nav == nullptr)
    {
        return false;
    }
    // Weighted by warmth, among the squares within reach and warm enough to mean something.
    float total = 0.0f;
    for (const HeatCell& cell : m_heat)
    {
        const glm::vec3 centre{(static_cast<float>(cell.x) + 0.5f) * kHeatCell, senses.position.y,
                               (static_cast<float>(cell.z) + 0.5f) * kHeatCell};
        if (cell.heat > 0.3f && Horizontal(centre, senses.position) < 45.0f)
        {
            total += cell.heat;
        }
    }
    if (total <= 0.0f)
    {
        return false;
    }
    float pick = m_random.Unit() * total;
    for (const HeatCell& cell : m_heat)
    {
        const glm::vec3 centre{(static_cast<float>(cell.x) + 0.5f) * kHeatCell, senses.position.y,
                               (static_cast<float>(cell.z) + 0.5f) * kHeatCell};
        if (cell.heat <= 0.3f || Horizontal(centre, senses.position) >= 45.0f)
        {
            continue;
        }
        pick -= cell.heat;
        if (pick <= 0.0f)
        {
            uint32_t seed = static_cast<uint32_t>(m_random.Next());
            return senses.nav->RandomPointNear(centre, kHeatCell * 0.6f, seed, out);
        }
    }
    return false;
}

float CreatureBrain::HidingHabit() const
{
    // Everybody it has pulled out of a locker, and every locker door it has heard, teaches it that
    // people hide. Learnt over one match and forgotten with it.
    return std::min(1.0f, 0.25f * static_cast<float>(m_foundHiding) + 0.1f * static_cast<float>(m_lockersHeard));
}

void CreatureBrain::Suspect(int place, float suspicion, int who)
{
    if (place < 0 || static_cast<size_t>(place) >= m_places.size())
    {
        return;
    }
    PlaceMemory& memory = m_places[static_cast<size_t>(place)];
    if (suspicion > memory.suspicion)
    {
        memory.suspicion = suspicion;
        if (who >= 0)
        {
            memory.suspect = who;
        }
    }
}

void CreatureBrain::PerceivePlaces(const CreatureSenses& senses, float dt)
{
    for (size_t i = 0; i < senses.hidingPlaces.size(); ++i)
    {
        const HidingPlace& place = senses.hidingPlaces[i];
        PlaceMemory& memory = m_places[i];
        // Suspicion fades over a minute or so: whoever got in has as likely got out again.
        memory.suspicion = std::max(memory.suspicion - 0.012f * dt, 0.0f);

        // What it can see of it: the door, open or shut, from where it stands.
        const glm::vec3 door = place.front + glm::vec3(0.0f, 1.0f, 0.0f);
        if (glm::distance(senses.eye, door) > 18.0f || !senses.clearLine || !senses.clearLine(senses.eye, door))
        {
            continue;
        }
        if (!place.shut)
        {
            // Standing open: nobody is in there, whatever it thought.
            memory.suspicion = 0.0f;
            memory.suspect = -1;
            memory.seenShut = false;
            continue;
        }
        // Shut. What that means is something it has to learn: nothing much, until the first time it
        // opens a shut one and finds somebody inside, after which a shut locker is a question it asks.
        if (!memory.seenShut)
        {
            memory.seenShut = true;
            if (m_shutMeansSomebody > 0.3f)
            {
                Log(senses.time, "sees a shut locker");
            }
        }
        Suspect(static_cast<int>(i), 0.1f + 0.8f * m_shutMeansSomebody, -1);
    }
}

void CreatureBrain::PlanSearch(const CreatureSenses& senses, const Track& track)
{
    m_searchPlan.clear();
    m_searchStep = 0;

    // Lockers first, the likeliest first: ones it has reason to suspect, and, the more it has learnt
    // that people hide, any near where they vanished.
    std::vector<std::pair<float, int>> places;
    const float habit = HidingHabit();
    for (size_t i = 0; i < senses.hidingPlaces.size(); ++i)
    {
        const PlaceMemory& memory = m_places[i];
        const float distance = Horizontal(senses.hidingPlaces[i].front, track.lastKnown);
        if (distance > 15.0f)
        {
            continue;
        }
        float likely = memory.suspicion;
        if (memory.suspect == track.id)
        {
            likely += 0.3f;
        }
        if (distance < 6.0f && m_random.Unit() < 0.15f + 0.6f * habit)
        {
            likely = std::max(likely, 0.3f);
        }
        if (likely > 0.25f)
        {
            places.push_back({likely - distance * 0.02f, static_cast<int>(i)});
        }
    }
    std::sort(places.begin(), places.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [likely, place] : places)
    {
        m_searchPlan.push_back({senses.hidingPlaces[static_cast<size_t>(place)].front, place});
    }

    // Then the ground they could have covered: a few places spread round where they were heading.
    if (senses.nav != nullptr)
    {
        const glm::vec3 heading = track.lastKnown + track.lastVelocity * 2.0f;
        glm::vec3 centre = track.lastKnown;
        senses.nav->NearestPoint(heading, 3.0f, centre);
        uint32_t seed = static_cast<uint32_t>(m_random.Next());
        for (int i = 0; i < 12 && m_searchPlan.size() < places.size() + 3; ++i)
        {
            glm::vec3 point;
            if (!senses.nav->RandomPointNear(centre, 9.0f, seed, point))
            {
                continue;
            }
            // Spread out: somewhere already on the list, or where it already is, is not a new place.
            bool spread = Horizontal(point, senses.position) > 3.0f;
            for (const SearchStop& stop : m_searchPlan)
            {
                spread = spread && Horizontal(point, stop.point) > 4.0f;
            }
            if (spread)
            {
                m_searchPlan.push_back({point, -1});
            }
        }
    }
    Log(senses.time, "searching for " + track.name + Format(": %.0f places, ", static_cast<float>(m_searchPlan.size())) +
                         Format("%.0f of them lockers", static_cast<float>(places.size())));
}

bool CreatureBrain::OpenPlace(const CreatureSenses& senses, int place)
{
    if (place < 0 || static_cast<size_t>(place) >= senses.hidingPlaces.size())
    {
        return false;
    }
    const float now = senses.time;
    const HidingPlace& hidingPlace = senses.hidingPlaces[static_cast<size_t>(place)];
    m_goal = "going to check a locker";
    if (Horizontal(senses.position, hidingPlace.front) > 0.9f)
    {
        m_openStarted = -1.0f;
        m_intent.move = true;
        m_intent.destination = hidingPlace.front;
        m_intent.speed = m_traits.walkSpeed * 1.6f;
        return true;
    }

    // At the door: it takes hold of it -- the same drawing back a strike has, a beat to see coming
    // from inside -- and pulls.
    m_goal = "opening a locker";
    m_intent.face = true;
    m_intent.facePoint = hidingPlace.inside;
    if (m_openStarted < 0.0f)
    {
        m_openStarted = now;
    }
    m_intent.windup = std::clamp((now - m_openStarted) / 0.6f, 0.0f, 1.0f);
    if (now - m_openStarted < 0.6f)
    {
        return true;
    }
    m_openStarted = -1.0f;
    m_intent.openHidingPlace = place;
    PlaceMemory& memory = m_places[static_cast<size_t>(place)];
    memory.suspicion = 0.0f;
    memory.suspect = -1;
    memory.checkedAt = now;

    // Only now, with the door open, does it know who is inside.
    for (const SensedPlayer& player : senses.players)
    {
        if (player.hidden && player.hidingPlace == place && player.alive)
        {
            ++m_foundHiding;
            if (hidingPlace.shut)
            {
                // The lesson: a shut locker had somebody in it.
                m_shutMeansSomebody = std::min(1.0f, m_shutMeansSomebody + 0.6f);
            }
            Track& track = TrackFor(player);
            track.confidence = 1.0f;
            track.lastKnown = hidingPlace.front;
            Switch(Behavior::Attack, player.id, "pulls " + player.name + " out of the locker", now);
            m_committedUntil = now + 1.5f;
            return false;
        }
    }
    Log(now, "opens a locker: empty");
    return false;
}

void CreatureBrain::UpdatePlayingDead(const CreatureSenses& senses)
{
    const float now = senses.time;
    m_intent.down = true;
    m_goal = "playing dead";

    if (m_hurtWhileDown)
    {
        // Shot while lying there: the act has failed, and staying down is only dying slowly.
        m_hurtWhileDown = false;
        m_playDeadCount = 99; // and it will not be believed again
        Switch(Behavior::Retreat, -1, "hurt again, gives up the act and runs", now);
        return;
    }

    // Who is near, and whether any of them is looking.
    const SensedPlayer* closest = nullptr;
    float closestDistance = 1.0e9f;
    bool anyoneWatching = false;
    for (const SensedPlayer& player : senses.players)
    {
        if (!player.alive || player.hidden)
        {
            continue;
        }
        const float distance = Horizontal(senses.position, player.feet);
        if (distance < closestDistance)
        {
            closestDistance = distance;
            closest = &player;
        }
        if (const Track* track = FindTrack(player.id); track != nullptr && track->watching && distance < 20.0f)
        {
            anyoneWatching = true;
        }
    }

    // Somebody close enough to touch: the whole point of lying still.
    if (closest != nullptr && closestDistance < 2.4f)
    {
        Switch(Behavior::Attack, closest->id, "springs up at " + closest->name, now);
        m_committedUntil = now + 1.5f;
        return;
    }
    // Long enough, and nobody looking: it gets up and slips away. With somebody looking it holds on,
    // up to a limit -- lying there for ever is only waiting to be found out.
    const bool waitedLongEnough = now >= m_playDeadUntil;
    if ((waitedLongEnough && !anyoneWatching) || now >= m_playDeadUntil + 15.0f)
    {
        Switch(Behavior::Retreat, -1, "gets up quietly while nobody is looking", now);
        return;
    }
    // Everybody has gone: no reason to keep it up.
    if (closestDistance > 25.0f && now - m_behaviorStarted > 4.0f)
    {
        Switch(Behavior::Retreat, -1, "gets up, they have gone", now);
    }
}

void CreatureBrain::Act(const CreatureSenses& senses, float dt)
{
    const float now = senses.time;
    m_intent.move = false;
    m_intent.face = false;
    m_intent.look = false;
    m_intent.windup = 0.0f;
    m_intent.crouch = 0.0f;
    m_intent.down = false;
    m_intent.attack = AttackKind::None;
    m_intent.attackPhase = 0.0f;
    m_intent.strikeKind = AttackKind::None;
    m_intent.grabTarget = -1;
    m_intent.cocoonTarget = -1;
    m_intent.roar = false;
    m_intent.display = false;
    m_intent.buildHive = false;
    m_intent.openDoor = -1;
    m_intent.bashDoor = -1;
    m_intent.bashing = false;
    m_intent.holding = m_holding;
    m_intent.climb = false;
    m_intent.drop = false;
    m_intent.mimic = -1;

    const glm::vec3 eye = senses.eye;
    const auto lookAround = [&](float until)
    {
        // Its head going slowly one way and the other, which is what searching looks like -- the head,
        // not the body. Swung either side of the way it was facing when it began, which stays put: swung
        // either side of the way it is facing now, it chased its own nose round in circles.
        if (!m_lookBaseSet)
        {
            m_lookBase = glm::length(senses.forward) > 1e-4f ? senses.forward : glm::vec3(0.0f, 0.0f, -1.0f);
            m_lookBaseSet = true;
        }
        const float angle = std::sin((now - m_behaviorStarted) * 1.1f) * 1.15f;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        m_intent.look = true;
        m_intent.lookAt = eye + glm::vec3(m_lookBase.x * c - m_lookBase.z * s, -0.15f, m_lookBase.x * s + m_lookBase.z * c) * 4.0f;
        return now < until;
    };
    // Its eyes on whoever it is after, when it can see them.
    const auto watch = [&](const SensedPlayer& player)
    {
        m_intent.look = true;
        m_intent.lookAt = player.feet + glm::vec3(0.0f, std::max(player.height - 0.25f, 0.3f), 0.0f);
    };

    switch (m_behavior)
    {
    case Behavior::Roam:
    {
        if (!m_haveRoamPoint && now >= m_pauseUntil && senses.nav != nullptr)
        {
            // More often than not, towards somewhere it has found people before: prowling rather than
            // wandering. Not always, or it would wear a path between two rooms and never find a third.
            // Where it wanders depends on what it is. A territorial one walks its own ground and
            // nowhere else; a timid one keeps clear of where people have been; the rest prowl there.
            const Temperament temperament = m_traits.temperament;
            const bool prowls = temperament == Temperament::Predator || temperament == Temperament::Curious;
            if (temperament == Temperament::Territorial && m_haveHome)
            {
                uint32_t seed = static_cast<uint32_t>(m_random.Next());
                m_haveRoamPoint = senses.nav->RandomPointNear(m_home, kTerritory * 0.75f, seed, m_roamPoint);
                m_goal = "walking its ground";
            }
            else if (prowls && m_random.Unit() < 0.6f && PickWarmPlace(senses, m_roamPoint))
            {
                m_haveRoamPoint = true;
                m_goal = "prowling where people go";
            }
            else
            {
                uint32_t seed = static_cast<uint32_t>(m_random.Next());
                m_haveRoamPoint = senses.nav->RandomPointNear(senses.position, 14.0f, seed, m_roamPoint);
                m_goal = "wandering";
            }
        }
        if (m_haveRoamPoint)
        {
            if (Horizontal(senses.position, m_roamPoint) < 0.8f)
            {
                m_haveRoamPoint = false;
                m_pauseUntil = now + m_random.Range(1.5f, 3.5f);
                m_lookAroundUntil = m_pauseUntil;
                m_lookBaseSet = false;
                m_goal = "pausing";
            }
            else
            {
                m_intent.move = true;
                m_intent.destination = m_roamPoint;
                m_intent.speed = m_traits.walkSpeed;
            }
        }
        else if (now < m_lookAroundUntil)
        {
            lookAround(m_lookAroundUntil);
        }
        break;
    }

    case Behavior::Investigate:
    {
        m_goal = "going to look at " + m_interest.what;
        if (!m_arrived && Horizontal(senses.position, m_interest.position) < 1.4f)
        {
            m_arrived = true;
            m_lookAroundUntil = now + 3.0f;
            m_lookBaseSet = false;
            Log(now, "arrived at the " + m_interest.what + ", looking around");
        }
        if (m_arrived && m_opening == -1)
        {
            // Come to look at a noise that was a locker door: the thing to look into is the locker.
            for (size_t i = 0; i < senses.hidingPlaces.size() && m_opening == -1; ++i)
            {
                if (m_places[i].suspicion > 0.3f &&
                    Horizontal(senses.hidingPlaces[i].front, m_interest.position) < 2.5f)
                {
                    m_opening = static_cast<int>(i);
                }
            }
        }
        if (m_arrived && m_opening >= 0)
        {
            if (!OpenPlace(senses, m_opening))
            {
                m_opening = -2; // done with it, whatever it found
                m_lookAroundUntil = now + 2.0f;
            }
        }
        else if (m_arrived)
        {
            m_goal = "looking around";
            if (!lookAround(m_lookAroundUntil) && !m_interest.resolved)
            {
                m_interest.resolved = true;
                m_interest.strength = 0.0f;
                Log(now, "found nothing");
            }
        }
        else
        {
            m_intent.move = true;
            m_intent.destination = m_interest.position;
            // A call from one of its own is answered at a run.
            m_intent.speed = m_interest.what == "call" ? m_traits.runSpeed * 0.9f : m_traits.walkSpeed * 1.6f;
            m_intent.look = true;
            m_intent.lookAt = m_interest.position + glm::vec3(0.0f, 0.8f, 0.0f);
        }
        break;
    }

    case Behavior::Hunt:
    {
        Track* track = FindTrack(m_target);
        const SensedPlayer* player = FindPlayer(senses, m_target);
        if (track == nullptr || player == nullptr)
        {
            break;
        }
        if (track->visible)
        {
            m_goal = "chasing " + track->name;
            m_arrived = false;
            watch(*player);
            const float rise = player->feet.y - senses.position.y;
            const float gap = Horizontal(senses.position, player->feet);
            // Up on something it cannot get onto and cannot reach: it paces underneath, looking up,
            // calling the others, and in the end goes to wait somewhere they will have to come down to.
            if (!senses.routeReached && gap < 5.0f && rise > senses.verticalReach)
            {
                if (m_unreachableSince < 0.0f)
                {
                    m_unreachableSince = now;
                    Log(now, "cannot reach " + track->name + " up there");
                }
                m_goal = "pacing under " + track->name + ", out of reach";
                if (now >= m_nextPaceAt && senses.nav != nullptr)
                {
                    uint32_t seed = static_cast<uint32_t>(m_random.Next());
                    glm::vec3 below{player->feet.x, senses.position.y, player->feet.z};
                    if (!senses.nav->RandomPointNear(below, 2.5f, seed, m_pacePoint))
                    {
                        m_pacePoint = senses.position;
                    }
                    m_nextPaceAt = now + m_random.Range(1.2f, 2.4f);
                }
                if (Horizontal(senses.position, m_pacePoint) > 0.5f)
                {
                    m_intent.move = true;
                    m_intent.destination = m_pacePoint;
                    m_intent.speed = m_traits.walkSpeed * 1.5f;
                }
                if (now >= m_nextRoarAt)
                {
                    m_intent.roar = true;
                    m_nextRoarAt = now + m_random.Range(5.0f, 9.0f);
                    Log(now, "calls the others");
                }
                if (now - m_unreachableSince > 12.0f)
                {
                    Switch(Behavior::Stalk, m_target, "gives up reaching " + track->name + " and waits", now);
                }
                break;
            }
            m_unreachableSince = -1.0f;
            // Up to them, not onto them: it stops at striking distance. Out of hiding it comes in a burst,
            // faster than it can keep up: the moment it stops waiting is the moment it is on you.
            const bool charging = (m_leftBehavior == Behavior::Stalk || m_leftBehavior == Behavior::Ambush ||
                                   m_leftBehavior == Behavior::Lure) && now - m_behaviorStarted < 2.5f;
            if (gap > m_traits.strikeReach * 0.8f)
            {
                m_intent.move = true;
                m_intent.destination = player->feet;
                m_intent.speed = m_traits.runSpeed * (charging ? 1.3f : 1.0f);
            }
            if (gap < 6.0f)
            {
                m_intent.face = true;
                m_intent.facePoint = player->feet;
            }
        }
        else
        {
            // Where they were, pushed a second along the way they were going: people keep moving
            // after they drop out of sight, and a creature that searched only the exact spot would
            // always be a step behind.
            glm::vec3 guess = track->lastKnown + track->lastVelocity * 1.0f;
            glm::vec3 onMesh;
            if (senses.nav != nullptr && senses.nav->NearestPoint(guess, 2.0f, onMesh))
            {
                guess = onMesh;
            }
            if (!m_arrived && Horizontal(senses.position, guess) < 1.2f)
            {
                m_arrived = true;
                m_lookAroundUntil = now + 2.5f;
                m_lookBaseSet = false;
                track->lastVelocity = glm::vec3(0.0f);
                // They are not here. That is news, and it lowers how sure it is of where they are --
                // which is what hands over to searching the places they could have gone instead.
                track->confidence = std::min(track->confidence, 0.45f);
                Log(now, "lost " + track->name + ", searching");
            }
            if (m_arrived)
            {
                m_goal = "searching for " + track->name;
                lookAround(m_lookAroundUntil);
            }
            else
            {
                m_goal = "going where " + track->name + " was";
                m_intent.move = true;
                m_intent.destination = guess;
                m_intent.speed = track->confidence > 0.5f ? m_traits.runSpeed : m_traits.walkSpeed * 1.6f;
            }
        }
        break;
    }

    case Behavior::Attack:
    {
        const SensedPlayer* player = FindPlayer(senses, m_target);
        Track* track = FindTrack(m_target);
        if (player == nullptr)
        {
            m_attack = AttackKind::None;
            break;
        }
        const float gap = Horizontal(senses.position, player->feet);
        const float rise = player->feet.y - senses.position.y;
        m_goal = std::string(m_attack == AttackKind::None ? "going for " : "attacking ") + player->name;
        watch(*player);
        if (senses.onCeiling)
        {
            // Up on the ceiling, going for somebody is letting go on top of them.
            m_goal = "dropping onto " + player->name;
            m_intent.climb = true;
            m_intent.drop = true;
            m_intent.dropAt = player->feet;
            if (!m_dropping)
            {
                Log(now, "drops from the ceiling onto " + player->name);
                m_dropping = true;
            }
            break;
        }
        m_dropping = false;
        if (gap > 0.6f)
        {
            m_intent.face = true;
            m_intent.facePoint = player->feet;
        }
        // A blow already under way runs its course. How fast depends on how much it wants this.
        const float pace = 1.12f - 0.25f * m_traits.aggression;
        if (m_attack != AttackKind::None)
        {
            const AttackTiming timing = TimingOf(m_attack);
            const float t = (now - m_attackStarted) / (timing.duration * pace);
            m_intent.attack = m_attack;
            m_intent.attackPhase = std::clamp(t, 0.0f, 1.0f);
            m_intent.attackSide = m_attackSide;
            m_intent.attackAt = player->feet + glm::vec3(0.0f, std::max(player->height * 0.6f, 0.4f), 0.0f);
            m_intent.windup = t < timing.lands ? t / timing.lands : 0.0f;
            if (m_attack == AttackKind::Lunge && t > 0.3f && t < 0.62f && gap > 0.9f)
            {
                // The leap itself: across the gap faster than it could ever run it.
                m_intent.move = true;
                m_intent.destination = player->feet;
                m_intent.speed = m_traits.runSpeed * 2.4f;
            }
            if (!m_attackLanded && t >= timing.lands)
            {
                // The game decides whether it connects: somebody who got out of the way is not hit, and
                // seeing it coming is what the wind-up is for.
                m_attackLanded = true;
                if (m_attack == AttackKind::Grab)
                {
                    m_intent.grabTarget = m_target;
                }
                else
                {
                    m_intent.strikeTarget = m_target;
                    m_intent.strikeKind = m_attack;
                }
                Log(now, std::string(AttackKindName(m_attack)) + " at " + player->name);
            }
            if (t >= 1.0f)
            {
                if (m_attack == AttackKind::Lunge)
                {
                    m_lungeReadyAt = now + m_random.Range(3.5f, 6.0f);
                }
                // Barely a pause between blows for something that means it.
                m_attackCooldownUntil = now + (m_attack == AttackKind::Lunge ? 0.5f : 0.12f + 0.35f * (1.0f - m_traits.aggression));
                m_attack = AttackKind::None;
            }
            break;
        }

        const bool canRise = rise < senses.verticalReach && rise > -1.6f;
        if (now >= m_attackCooldownUntil && canRise)
        {
            if (gap < m_traits.strikeReach)
            {
                // Somebody on their own is somebody to take away. Otherwise claws and teeth, in turn.
                const bool alone = track != nullptr && track->isolation > 6.0f;
                const float grabChance = (alone ? 0.35f : 0.08f) * (0.5f + m_traits.isolationPreference) *
                                         (player->feet.y - senses.position.y < 0.6f ? 1.0f : 0.0f);
                if (m_traits.Captures() && m_holding < 0 && !player->hidden && m_random.Unit() < grabChance)
                {
                    StartAttack(AttackKind::Grab, player->feet, now);
                }
                else
                {
                    StartAttack(m_random.Unit() < 0.55f ? AttackKind::Swipe : AttackKind::Bite, player->feet, now);
                }
                m_committedUntil = now + TimingOf(m_attack).duration * pace;
                break;
            }
            const glm::vec3 toward = player->feet - senses.position;
            const bool facing = glm::dot(glm::normalize(glm::vec3(toward.x, 0.0f, toward.z)), senses.forward) > 0.8f;
            if (gap > 2.6f && gap < 7.0f && now >= m_lungeReadyAt && std::abs(rise) < 0.8f && facing && !OutOfItsReach(senses, *player) &&
                (!senses.clearLine || senses.clearLine(eye, player->feet + glm::vec3(0.0f, 1.0f, 0.0f))))
            {
                StartAttack(AttackKind::Lunge, player->feet, now);
                m_committedUntil = now + TimingOf(AttackKind::Lunge).duration * pace;
                break;
            }
        }
        if (gap > m_traits.strikeReach * 0.8f)
        {
            m_intent.move = true;
            m_intent.destination = player->feet;
            m_intent.speed = m_traits.runSpeed;
        }
        break;
    }

    case Behavior::Drag:
    {
        const SensedPlayer* victim = FindPlayer(senses, m_holding);
        if (victim == nullptr || !victim->alive)
        {
            OnReleased(now, "nothing left to hold");
            break;
        }
        m_intent.holding = m_holding;
        if (!m_haveDragPoint)
        {
            m_haveDragPoint = PickDragPoint(senses, m_holding, m_dragPoint);
            if (!m_haveDragPoint)
            {
                m_dragArrived = true;
            }
            else
            {
                Log(now, "drags " + victim->name + Format(" away, %.0f m", Horizontal(senses.position, m_dragPoint)));
            }
        }
        const bool atHive = senses.hasHive && Horizontal(senses.position, senses.hive) < 3.0f;
        if (!m_dragArrived && (Horizontal(senses.position, m_dragPoint) < 1.3f || now - m_holdStarted > 16.0f))
        {
            m_dragArrived = true;
            Log(now, "arrived with " + victim->name);
        }
        if (!m_dragArrived)
        {
            m_goal = "dragging " + victim->name + " away";
            m_intent.move = true;
            m_intent.destination = m_dragPoint;
            m_intent.speed = m_traits.walkSpeed * 1.7f;
            break;
        }
        if (atHive)
        {
            // Home: wrapped up and left there, alive, for later. Somebody who comes for them in time can
            // cut them out.
            m_intent.cocoonTarget = m_holding;
            Log(now, "wraps " + victim->name + " at the nest");
            m_holding = -1;
            m_intent.holding = -1;
            Switch(Behavior::Roam, -1, "left its catch at the nest", now);
            break;
        }
        // Nowhere to take them: away from the others, it throws them down and lets go, and it is a
        // fight again -- with nobody near to help. It never kills anybody while it has hold of them.
        {
            const int was = m_holding;
            m_holding = -1;
            m_intent.holding = -1;
            Log(now, "throws " + victim->name + " down");
            m_attackCooldownUntil = now + 0.9f;
            Switch(Behavior::Attack, was, "threw them down", now);
        }
        break;
    }

    case Behavior::Nest:
    {
        if (senses.hasHive || !senses.mayBuildNest)
        {
            m_haveNestSite = false;
            m_nestWorkStarted = -1.0f;
            Switch(Behavior::Roam, -1, "there is a nest already", now);
            break;
        }
        if (!m_haveNestSite && now >= m_nestThoughtAt)
        {
            m_haveNestSite = PickNestSite(senses, m_nestSite);
            m_nestThoughtAt = now + 8.0f;
            if (m_haveNestSite)
            {
                Log(now, Format("has somewhere in mind for a nest, %.0f m away",
                                Horizontal(senses.position, m_nestSite)));
            }
        }
        if (!m_haveNestSite)
        {
            m_goal = "looking for somewhere to build";
            break;
        }
        if (Horizontal(senses.position, m_nestSite) > 1.2f)
        {
            m_goal = "going somewhere to build";
            m_intent.move = true;
            m_intent.destination = m_nestSite;
            m_intent.speed = m_traits.walkSpeed * 1.3f;
            break;
        }
        // Working at it: hunched over, hands at the floor, for a good while.
        if (m_nestWorkStarted < 0.0f)
        {
            m_nestWorkStarted = now;
            Log(now, "starts building a nest");
        }
        m_goal = "building a nest";
        m_intent.crouch = 1.0f;
        m_intent.bashing = true;
        m_intent.look = true;
        m_intent.lookAt = senses.position + senses.forward * 0.8f;
        if (now - m_nestWorkStarted > 14.0f)
        {
            m_intent.buildHive = true;
            m_intent.hiveAt = senses.position;
            m_nestWorkStarted = -1.0f;
            m_haveNestSite = false;
            Log(now, "its nest is built");
            Switch(Behavior::Roam, -1, "the nest is built", now);
        }
        break;
    }

    case Behavior::Retreat:
    {
        if (!m_haveFleePoint)
        {
            // Back to the nest, when it has one, to lick its wounds.
            if (senses.hasHive)
            {
                m_fleePoint = senses.hive;
                m_haveFleePoint = true;
            }
            else
            {
                m_haveFleePoint = PickFleePoint(senses, m_fleePoint);
            }
            if (m_haveFleePoint)
            {
                Log(now, Format("retreating to %.0f, %.0f", m_fleePoint.x, m_fleePoint.z));
            }
        }
        if (m_haveFleePoint && Horizontal(senses.position, m_fleePoint) > 1.0f)
        {
            m_goal = "getting away";
            m_intent.move = true;
            m_intent.destination = m_fleePoint;
            m_intent.speed = m_traits.runSpeed * 1.1f;
        }
        else
        {
            const bool atNest = senses.hasHive && Horizontal(senses.position, senses.hive) < 3.5f;
            m_goal = atNest ? "at the nest, healing" : "hiding";
            lookAround(1.0e9f);
            // Only its nest mends it. Anywhere else a wound stays a wound, which is what makes hurting
            // one worth something -- and what makes the nest worth finding.
            m_intent.recover = atNest ? 0.004f : 0.0f;
            // And at the nest it stays to mend while it is badly hurt and nothing is near, up to a
            // point: an animal that has gone to ground comes back out.
            if (atNest && senses.healthFraction < 0.5f && !m_threatened && now - m_behaviorStarted < 40.0f)
            {
                m_retreatUntil = std::max(m_retreatUntil, now + 1.0f);
            }
        }
        break;
    }

    case Behavior::Stalk:
    {
        Track* track = FindTrack(m_target);
        const SensedPlayer* player = FindPlayer(senses, m_target);
        if (track == nullptr || player == nullptr)
        {
            break;
        }
        track->stalked += dt;
        m_intent.crouch = 1.0f;
        const glm::vec3 them = track->visible ? player->feet : track->lastKnown;
        if (track->visible)
        {
            watch(*player);
        }

        // Whether where it is waiting is still cover: they move, and a spot out of sight a moment ago
        // may be in plain view now. Checked a few times a second rather than every tick.
        if (now >= m_stalkCheckAt)
        {
            m_stalkCheckAt = now + 0.7f;
            if (senses.onCeiling)
            {
                // Overhead, nobody sees it who is not looking up at it.
                m_stalkExposed = std::any_of(m_tracks.begin(), m_tracks.end(), [&](const Track& other) {
                    return other.watching && Horizontal(other.lastKnown, senses.position) < 25.0f;
                });
            }
            else
            {
                m_stalkExposed = SeenFrom(senses, senses.position);
            }
            const float fromThem = m_haveStalkPoint ? Horizontal(m_stalkPoint, them) : 0.0f;
            if (!m_haveStalkPoint || fromThem < 5.0f || fromThem > 22.0f || SeenFrom(senses, m_stalkPoint))
            {
                glm::vec3 point;
                if (FindCover(senses, *track, player, point))
                {
                    if (!m_haveStalkPoint || Horizontal(point, m_stalkPoint) > 1.0f)
                    {
                        Log(now, Format("moving to cover %.0f m from ", Horizontal(point, them)) + track->name);
                    }
                    m_stalkPoint = point;
                    m_haveStalkPoint = true;
                }
            }
        }

        // Seen, a good way off, the stealthy ones do not bolt at once: they stop dead and stare back for a
        // moment first, and then they are gone. Being looked at by something that knows it has been seen
        // and is in no hurry is worse than seeing something run.
        {
            const float away = Horizontal(senses.position, them);
            if (m_stalkExposed && !m_stareWasExposed && track->visible && away > 8.0f && now - m_behaviorStarted > 3.0f &&
                m_random.Unit() < 0.35f + 0.5f * m_traits.stealth)
            {
                m_stareUntil = now + m_random.Range(1.2f, 2.8f);
                Log(now, "stares back at " + track->name);
            }
            m_stareWasExposed = m_stalkExposed;
            if (now < m_stareUntil)
            {
                m_goal = "staring at " + track->name;
                m_intent.crouch = 0.3f;
                m_intent.face = true;
                m_intent.facePoint = them;
                watch(*player);
                break;
            }
        }

        // Peeking. Cover it cannot be seen from is cover it cannot see out of, so every few seconds it
        // leans out -- to a spot beside it with a view of them -- takes a look, and slips back. A look
        // that finds them turned away is the opening it has been waiting for.
        const bool inCover = m_haveStalkPoint && Horizontal(senses.position, m_stalkPoint) <= 0.8f;
        // Up on the ceiling it can see them from where it is: it has no need to lean out. And it goes on
        // across the ceiling to cover that has one over it, rather than coming down to walk there.
        const auto overhead = [&](const glm::vec3& point)
        {
            if (!m_traits.climbs || !senses.ceilingAt)
            {
                return false;
            }
            const float ceiling = senses.ceilingAt(point);
            return ceiling > 2.2f && ceiling < 4.8f;
        };
        if (senses.onCeiling && m_haveStalkPoint && overhead(m_stalkPoint))
        {
            m_intent.climb = true;
        }
        if (!m_peeking && inCover && !senses.onCeiling && !m_stalkExposed && now >= m_nextPeekAt && senses.nav != nullptr &&
            senses.clearLine)
        {
            const glm::vec3 theirEye = them + glm::vec3(0.0f, 1.6f, 0.0f);
            uint32_t seed = static_cast<uint32_t>(m_random.Next());
            float nearest = 1.0e9f;
            for (int i = 0; i < 10; ++i)
            {
                glm::vec3 spot;
                if (senses.nav->RandomPointNear(m_stalkPoint, 3.0f, seed, spot) &&
                    senses.clearLine(spot + glm::vec3(0.0f, m_traits.eyeHeight, 0.0f), theirEye) &&
                    Horizontal(spot, m_stalkPoint) < nearest)
                {
                    nearest = Horizontal(spot, m_stalkPoint);
                    m_peekPoint = spot;
                    m_peeking = true;
                }
            }
            m_peekUntil = -1.0f;
            m_nextPeekAt = now + m_random.Range(2.5f, 5.0f);
            if (m_peeking)
            {
                Log(now, "peeks out at " + track->name);
            }
        }
        if (m_peeking)
        {
            m_goal = "peeking at " + track->name;
            m_intent.crouch = 1.0f;
            if (Horizontal(senses.position, m_peekPoint) > 0.5f && m_peekUntil < 0.0f)
            {
                m_intent.move = true;
                m_intent.destination = m_peekPoint;
                m_intent.speed = m_traits.walkSpeed;
            }
            else
            {
                if (m_peekUntil < 0.0f)
                {
                    m_peekUntil = now + 1.2f;
                }
                m_intent.face = true;
                m_intent.facePoint = them;
                if (now >= m_peekUntil)
                {
                    m_peeking = false;
                    m_nextPeekAt = now + m_random.Range(2.5f, 5.0f);
                }
            }
        }
        else if (m_haveStalkPoint && Horizontal(senses.position, m_stalkPoint) > 0.8f)
        {
            // Out in the open where they can see it, it gets out of sight, fast: creeping slowly
            // across somebody's view is only a slower way of being seen. Once it is hidden it creeps.
            m_intent.move = true;
            m_intent.destination = m_stalkPoint;
            if (m_stalkExposed)
            {
                m_goal = "getting out of " + track->name + "'s sight";
                m_intent.speed = m_traits.runSpeed * 0.75f;
                m_intent.crouch = 0.5f;
            }
            else
            {
                m_goal = "creeping to cover near " + track->name;
                m_intent.speed = m_traits.walkSpeed * 1.3f;
            }
        }
        else if (!m_haveStalkPoint && m_stalkExposed)
        {
            // No cover anywhere near them, and in their sight where it stands: it backs off somewhere
            // out of it, against something, rather than standing in the open to be looked at.
            glm::vec3 spot;
            if (now >= m_stalkCheckAt - 0.35f && PickHidingSpot(senses, them, spot))
            {
                m_stalkPoint = spot;
                m_haveStalkPoint = true;
                Log(now, "no cover near " + track->name + "; backs off out of sight");
            }
            m_goal = "no cover near " + track->name + ", backing off";
            m_intent.face = true;
            m_intent.facePoint = them;
        }
        else
        {
            m_goal = m_haveStalkPoint ? "watching " + track->name + " from cover"
                                      : "holding still out of sight, watching " + track->name;
            m_intent.face = true;
            m_intent.facePoint = them;
            // One that climbs watches from overhead where it can, where nobody looks.
            if (m_haveStalkPoint && overhead(senses.position))
            {
                m_intent.climb = true;
            }
        }
        break;
    }

    case Behavior::PlayDead:
        UpdatePlayingDead(senses);
        break;

    case Behavior::Ambush:
        ActAmbush(senses, dt);
        break;

    case Behavior::Flank:
        ActFlank(senses, dt);
        break;

    case Behavior::Lure:
        ActLure(senses, dt);
        break;

    case Behavior::Observe:
    {
        Track* track = FindTrack(m_target);
        const SensedPlayer* player = FindPlayer(senses, m_target);
        if (track == nullptr || player == nullptr)
        {
            break;
        }
        track->observed += dt;
        const float distance = Horizontal(senses.position, player->feet);
        watch(*player);
        m_intent.face = true;
        m_intent.facePoint = player->feet;
        // Near enough to see them well, not so near it has to decide anything. Closer than that and it
        // gives ground; further, and it follows.
        if (distance < 5.0f && senses.nav != nullptr)
        {
            m_goal = "backing away from " + track->name;
            glm::vec3 away = senses.position - player->feet;
            away.y = 0.0f;
            away = glm::length(away) > 1e-3f ? glm::normalize(away) : glm::vec3(0.0f, 0.0f, 1.0f);
            glm::vec3 back;
            // Backwards, still facing them: a wary animal does not turn its back on what it is wary of.
            if (senses.nav->NearestPoint(senses.position + away * 3.0f, 2.0f, back))
            {
                m_intent.move = true;
                m_intent.destination = back;
                m_intent.speed = m_traits.walkSpeed * 1.3f;
            }
        }
        else if (distance > 10.0f)
        {
            m_goal = "following " + track->name + ", watching";
            m_intent.move = true;
            m_intent.destination = player->feet;
            m_intent.speed = m_traits.walkSpeed;
            m_intent.face = false;
        }
        else
        {
            m_goal = "watching " + track->name;
        }
        break;
    }

    case Behavior::Avoid:
    {
        Track* track = FindTrack(m_target);
        const SensedPlayer* player = FindPlayer(senses, m_target);
        if (track == nullptr || player == nullptr)
        {
            break;
        }
        const glm::vec3 them = track->visible ? player->feet : track->lastKnown;
        const float distance = Horizontal(senses.position, them);
        m_threat = them;
        // Wherever they are is somewhere to keep clear of for a while after: not somewhere to go back to
        // and look into the moment it has got away.
        RememberDanger(them, now + 45.0f);
        // Somewhere away from them and out of their sight, picked again whenever the old place is no
        // longer away from them, or it has got there and they are still close.
        //
        // Moving off once they come within ten metres, and not stopping until they are eighteen away or out of
        // sight: one line for both had it walking off, turning to look, being too close again, and walking
        // off, over and over. And pushed three times it stops trying to watch and leaves.
        if (!m_avoidFleeing && distance < 10.0f)
        {
            m_avoidFleeing = true;
            m_haveFleePoint = false;
            ++m_avoidPushed;
            if (m_avoidPushed == 3)
            {
                Log(now, "has had enough of " + track->name + " and leaves");
                RememberDanger(them, now + 120.0f);
            }
        }
        else if (m_avoidFleeing && (distance > 18.0f || (!track->visible && distance > 12.0f)))
        {
            m_avoidFleeing = false;
        }
        const bool leaving = m_avoidPushed >= 3;
        const bool stale = m_haveFleePoint && (Horizontal(m_fleePoint, them) < distance + 2.0f ||
                                               Horizontal(senses.position, m_fleePoint) < 1.0f);
        if ((m_avoidFleeing || leaving) && (!m_haveFleePoint || stale))
        {
            m_haveFleePoint = PickFleePoint(senses, m_fleePoint);
        }
        if ((m_avoidFleeing || leaving) && m_haveFleePoint)
        {
            m_goal = leaving ? "leaving, away from " + track->name : "keeping away from " + track->name;
            m_intent.move = true;
            m_intent.destination = m_fleePoint;
            m_intent.speed = distance < 6.0f ? m_traits.runSpeed : m_traits.walkSpeed * 1.4f;
        }
        else
        {
            // Far enough. It stops and watches them, ready to go again.
            m_goal = "watching " + track->name + " from a distance";
            watch(*player);
            m_intent.face = true;
            m_intent.facePoint = them;
        }
        // Cornered: they are close and staying close, and it is getting nowhere. That is what turns a
        // timid animal round (UpdateHostility).
        m_cornered = distance < 3.0f ? m_cornered + dt : std::max(m_cornered - dt, 0.0f);
        break;
    }

    case Behavior::Warn:
    {
        Track* track = FindTrack(m_target);
        const SensedPlayer* player = FindPlayer(senses, m_target);
        if (track == nullptr || player == nullptr)
        {
            break;
        }
        // Up on its legs, facing them, holding its ground -- and every few seconds, showing it.
        m_goal = "warning " + track->name + " off its ground";
        watch(*player);
        m_intent.face = true;
        m_intent.facePoint = player->feet;
        m_intent.crouch = 0.0f;
        if (now - track->warnedAt > 3.5f)
        {
            if (now - track->warnedAt > 30.0f)
            {
                Log(now, "warns " + track->name + " off its ground");
            }
            track->warnedAt = now;
            m_intent.display = true;
        }
        break;
    }

    case Behavior::Search:
    {
        Track* track = FindTrack(m_target);
        if (track == nullptr)
        {
            break;
        }
        if (m_searchPlan.empty() && m_searchStep == 0)
        {
            PlanSearch(senses, *track);
            if (m_searchPlan.empty())
            {
                m_searchStep = 1; // nowhere to look; falls through to giving up below
            }
        }
        if (m_searchStep >= m_searchPlan.size())
        {
            // Everywhere it could think of, and nothing. It lets them go -- for now: a sound or a
            // glimpse brings them straight back.
            Log(now, "gives up looking for " + track->name);
            track->confidence = 0.0f;
            Switch(Behavior::Roam, -1, "nothing found", now);
            break;
        }
        const SearchStop& stop = m_searchPlan[m_searchStep];
        if (stop.place >= 0)
        {
            if (!OpenPlace(senses, stop.place))
            {
                if (m_behavior != Behavior::Search)
                {
                    break; // it found somebody, and is going for them
                }
                ++m_searchStep;
                m_arrived = false;
            }
            break;
        }
        m_goal = "searching for " + track->name;
        if (!m_arrived && Horizontal(senses.position, stop.point) < 1.2f)
        {
            m_arrived = true;
            m_lookAroundUntil = now + 1.6f;
            m_lookBaseSet = false;
        }
        if (m_arrived)
        {
            if (!lookAround(m_lookAroundUntil))
            {
                ++m_searchStep;
                m_arrived = false;
            }
        }
        else
        {
            m_intent.move = true;
            m_intent.destination = stop.point;
            m_intent.speed = m_traits.walkSpeed * 1.6f;
        }
        break;
    }
    }

    // The orienting response, over whatever it was doing if that was only wandering or looking into a
    // noise: something moved at the edge of its view, so it stops and looks at it -- turning its head,
    // and its body only when the thing is well round to the side -- until it has made it out or it has
    // gone. Not while hunting, attacking or running, which already have somewhere to look.
    if ((m_behavior == Behavior::Roam || m_behavior == Behavior::Investigate) && now < m_alertUntil)
    {
        m_intent.move = false;
        m_intent.look = true;
        m_intent.lookAt = m_alertPoint + glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 toward = m_alertPoint - senses.position;
        const glm::vec3 flat{toward.x, 0.0f, toward.z};
        if (glm::length(flat) > 0.5f && glm::dot(glm::normalize(flat), senses.forward) < 0.3f)
        {
            m_intent.face = true;
            m_intent.facePoint = m_alertPoint;
        }
        m_goal = "something caught its eye";
    }

    // A shut door across its way: it pulls it open, or, locked, throws itself at it until it gives.
    DoorSense door;
    if (m_intent.move && DoorInTheWay(senses, door))
    {
        if (m_door != door.index)
        {
            m_door = door.index;
            m_doorStarted = now;
            m_nextBash = now + 0.45f;
            Log(now, door.locked ? "a locked door in the way" : "a door in the way");
        }
        const glm::vec3 middle = (door.a + door.b) * 0.5f;
        m_intent.move = false;
        m_intent.face = true;
        m_intent.facePoint = middle;
        m_intent.look = true;
        m_intent.lookAt = middle + glm::vec3(0.0f, 1.0f, 0.0f);
        if (!door.locked)
        {
            m_goal = "pulling a door open";
            if (now - m_doorStarted > 0.4f)
            {
                m_intent.openDoor = door.index;
                m_door = -1;
            }
        }
        else
        {
            m_goal = "breaking a door down";
            m_intent.bashing = true;
            if (now >= m_nextBash)
            {
                m_intent.bashDoor = door.index;
                m_nextBash = now + 0.9f;
            }
        }
    }
    else if (!m_intent.move)
    {
        m_door = -1;
    }

    // Moving again, the next look round starts from wherever it ends up facing.
    if (m_intent.move)
    {
        m_lookBaseSet = false;
    }
}

} // namespace pred
