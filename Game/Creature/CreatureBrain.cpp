#include "Game/Creature/CreatureBrain.h"
#include "Game/Creature/TacticLearner.h"
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

// How close the straight line from `from` to `to` passes `past`, on the floor.
float Passes(const glm::vec3& from, const glm::vec3& to, const glm::vec3& past)
{
    const glm::vec2 a{from.x, from.z};
    const glm::vec2 along = glm::vec2(to.x, to.z) - a;
    const glm::vec2 p{past.x, past.z};
    const float length2 = glm::dot(along, along);
    const float t = length2 > 1e-4f ? std::clamp(glm::dot(p - a, along) / length2, 0.0f, 1.0f) : 0.0f;
    return glm::length(a + along * t - p);
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
    case Behavior::Feed:
        return "Feed";
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

Behavior CreatureBrain::RecentTactic(float time) const
{
    if (TacticLearner::Of(m_behavior) != TacticLearner::Count)
    {
        return m_behavior;
    }
    if (time - m_leftAt < 4.0f && TacticLearner::Of(m_leftBehavior) != TacticLearner::Count)
    {
        return m_leftBehavior;
    }
    return Behavior::Roam;
}

bool CreatureBrain::PickPlaceOfInterest(const CreatureSenses& senses, glm::vec3& out)
{
    // Somewhere that means something to a hunting animal: a way in or out of a room, the mouth of a
    // crawlspace, a locker somebody could be in, its own nest. Not the one it has just come from.
    if (senses.nav == nullptr)
    {
        return false;
    }
    struct Place
    {
        glm::vec3 at;
        glm::vec3 watch;
        const char* what;
    };
    std::vector<Place> places;
    for (const DoorSense& door : senses.doors)
    {
        const glm::vec3 middle = (door.a + door.b) * 0.5f;
        places.push_back({middle, middle, "a doorway"});
    }
    for (const glm::vec3& mouth : senses.nav->CrawlMouths())
    {
        places.push_back({mouth, mouth, "the mouth of a crawlspace"});
    }
    if (HidingHabit() > 0.2f || m_traits.curiosity > 0.6f)
    {
        for (const HidingPlace& place : senses.hidingPlaces)
        {
            places.push_back({place.front, place.inside, "a locker"});
        }
    }
    if (senses.hasHive)
    {
        places.push_back({senses.hive, senses.hive, "the nest"});
    }
    // Near enough to be a walk, not a journey, and not where it already is.
    std::vector<const Place*> near;
    for (const Place& place : places)
    {
        const float away = Horizontal(place.at, senses.position);
        if (away > 3.0f && away < 26.0f)
        {
            near.push_back(&place);
        }
    }
    if (near.empty())
    {
        return false;
    }
    const Place& chosen = *near[static_cast<size_t>(m_random.Unit() * static_cast<float>(near.size())) % near.size()];
    // Standing off it a little, facing it: not in the doorway, but where it can see through it.
    glm::vec3 stand = chosen.at;
    const glm::vec3 back = Flat(senses.position - chosen.at);
    if (glm::length(back) > 1e-3f)
    {
        stand += glm::normalize(back) * 1.6f;
    }
    if (!senses.nav->NearestPoint(stand, 2.0f, out))
    {
        return false;
    }
    m_watchPoint = chosen.watch;
    m_roamWhy = chosen.what;
    return true;
}

void CreatureBrain::StartPastime(const CreatureSenses& senses, float now)
{
    m_lookBaseSet = false;
    const bool somewhere = std::string(m_roamWhy) != "somewhere";
    const float dark = senses.lightAt ? 1.0f - std::clamp(senses.lightAt(senses.position + glm::vec3(0.0f, 0.5f, 0.0f)), 0.0f, 1.0f) : 0.5f;
    // What it does here depends on what it is and where it has got to.
    // Blind, stopping to listen is most of what it does.
    const float listen = (0.25f + 0.5f * m_traits.hearing / 1.6f) * (m_traits.sight <= 0.0f ? 3.0f : 1.0f);
    const float sniff = 0.3f + 0.4f * m_traits.curiosity;
    // Not straight after it has had somebody and lost them: it is restless then, and keeps moving.
    const bool restless = std::any_of(m_tracks.begin(), m_tracks.end(),
                                      [&](const Track& track) { return track.lastSeen >= 0.0f && now - track.lastSeen < 90.0f; });
    const float rest = restless ? 0.0f : (0.1f + 0.4f * m_traits.patience) * (0.3f + dark);
    const float watch = somewhere ? (0.6f + 0.6f * m_traits.patience) * (m_traits.Has(Quirk::Watcher) ? 2.5f : 1.0f) : 0.0f;
    if (m_traits.Has(Quirk::Pacer) && senses.nav != nullptr && m_random.Unit() < 0.6f)
    {
        // Back and forth, back and forth, between here and a few steps away.
        uint32_t seed = static_cast<uint32_t>(m_random.Next());
        glm::vec3 other;
        if (senses.nav->RandomPointNear(senses.position, 3.5f, seed, other) && Horizontal(other, senses.position) > 1.8f)
        {
            m_pastime = Pastime::Pace;
            m_paceFrom = senses.position;
            m_paceTo = other;
            m_pauseUntil = now + m_random.Range(8.0f, 16.0f);
            m_lookAroundUntil = m_pauseUntil;
            return;
        }
    }
    const float look = 0.35f;
    float roll = m_random.Unit() * (listen + sniff + rest + watch + look);
    if ((roll -= watch) < 0.0f)
    {
        m_pastime = Pastime::Watch;
        m_pauseUntil = now + m_random.Range(3.0f, 6.0f + 7.0f * m_traits.patience);
    }
    else if ((roll -= rest) < 0.0f)
    {
        m_pastime = Pastime::Rest;
        m_pauseUntil = now + m_random.Range(6.0f, 10.0f + 12.0f * m_traits.patience);
    }
    else if ((roll -= listen) < 0.0f)
    {
        m_pastime = Pastime::Listen;
        m_pauseUntil = now + m_random.Range(2.5f, 6.0f);
    }
    else if ((roll -= sniff) < 0.0f)
    {
        m_pastime = Pastime::Sniff;
        m_pauseUntil = now + m_random.Range(2.0f, 4.5f);
    }
    else
    {
        m_pastime = Pastime::LookAround;
        m_pauseUntil = now + m_random.Range(1.5f, 3.5f);
    }
    m_lookAroundUntil = m_pauseUntil;
}

bool CreatureBrain::TargetKnownAt(glm::vec3& out) const
{
    for (const Track& track : m_tracks)
    {
        if (track.id == m_target && track.confidence > 0.3f)
        {
            out = track.lastKnown;
            return true;
        }
    }
    return false;
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
        ++m_downHits;
        m_downDamage += amount / std::max(maxHealth, 1.0f);
        m_downHurtBy = byPlayer;
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
    // Lit up and then shot, twice over: the light is what comes before the rounds.
    if (time - m_lastLitAt < 2.0f && byPlayer >= 0 && !m_litCounted)
    {
        m_litCounted = true;
        if (++m_litThenShot >= 2 && !m_lightShy)
        {
            m_lightShy = true;
            Log(time, "has learnt that the light comes before the shooting");
        }
    }
    // Shot in one encounter after another, it learns. Not machine learning: a thing it now knows, which
    // opens other ways of going about people -- round the side, from hiding, one blow and away.
    if (byPlayer >= 0)
    {
        if (time - m_lastShotAt > 30.0f)
        {
            ++m_timesShot;
            if (m_timesShot >= 3 && !m_wary)
            {
                m_wary = true;
                Log(time, "has learnt to be careful of them");
            }
        }
        m_lastShotAt = time;
    }

    // Found out, wherever it was: it thinks again at once rather than at its next leisure, and the cover,
    // the spot it was waiting in, the body it was eating are all given up as found.
    m_shotAt = time;
    m_shotDuring = m_behavior;
    m_shotBy = byPlayer;
    m_decideTimer = 1.0f;
    if (m_attack == AttackKind::None)
    {
        m_committedUntil = std::min(m_committedUntil, time);
    }
    if (m_behavior == Behavior::Stalk || m_behavior == Behavior::Ambush || m_behavior == Behavior::Lure)
    {
        m_haveStalkPoint = false;
        m_peeking = false;
    }
    if (m_behavior == Behavior::Feed)
    {
        m_fedUntil = time + 20.0f;
    }
    // Shot while it runs: that way is no good, so another; and shot again and again by somebody close
    // who is keeping up with it, it is cornered, and turns.
    if (m_behavior == Behavior::Retreat)
    {
        m_haveFleePoint = false;
        if (time - m_firstRunningHit > 4.0f)
        {
            m_firstRunningHit = time;
            m_hitsWhileRunning = 0;
        }
        ++m_hitsWhileRunning;
        const Track* shooter = FindTrack(byPlayer);
        if (shooter != nullptr && m_hitsWhileRunning >= 2 && glm::distance(from, m_lastPosition) < 6.0f &&
            m_traits.aggression > 0.35f)
        {
            m_hitsWhileRunning = 0;
            Switch(Behavior::Attack, byPlayer, "cornered; turns on " + shooter->name, time);
            m_committedUntil = time + 1.2f;
        }
    }

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

void CreatureBrain::OnKinHurt(int byPlayer, float amount, float time, float maxHealth)
{
    if (m_dead)
    {
        return;
    }
    if (Track* track = FindTrack(byPlayer); track != nullptr)
    {
        const float scale = 160.0f / std::max(maxHealth, 1.0f);
        if (track->harm <= 0.0f)
        {
            Log(time, "sees " + track->name + " hurt one of its kind");
        }
        track->harm += amount * scale / 100.0f * 0.3f;
        track->provokedAt = time;
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

void CreatureBrain::DirectorHint(const glm::vec3& where, float time)
{
    if (!m_interest.resolved && m_interest.strength > 0.45f)
    {
        return; // it has something better to go on
    }
    m_interest = {where, 0.45f, time, "a feeling", false};
    Log(time, "has a feeling about somewhere");
}

bool CreatureBrain::AskToWithdraw(float seconds, float time)
{
    // Commitment. Whatever it is in the middle of that is about somebody, it finishes first: a creature
    // that heard something and then wandered off before looking would not seem to have heard it at all.
    bool busy = false;
    switch (m_behavior)
    {
    case Behavior::Investigate:
    case Behavior::Stalk:
    case Behavior::Search:
    case Behavior::Attack:
    case Behavior::Drag:
    case Behavior::Ambush:
    case Behavior::Flank:
    case Behavior::Lure:
    case Behavior::PlayDead:
    case Behavior::Nest:
        busy = true;
        break;
    case Behavior::Hunt:
        busy = std::any_of(m_tracks.begin(), m_tracks.end(), [&](const Track& track) { return track.id == m_target && track.visible; });
        break;
    default:
        break;
    }
    if (busy || m_dead)
    {
        return false;
    }
    m_withdrawUntil = time + seconds;
    Log(time, Format("slips away out of sight for %.0f s", seconds));
    return true;
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
    if (m_traits.Has(Quirk::LightShy))
    {
        m_lightShy = true;
    }
    m_lastTime = senses.time;
    m_lastPosition = senses.position;
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
    // Head down in a body, it sees and hears a good deal less: somebody can get past it, or up to it.
    const bool feeding = Feeding();
    const float sightRange = SightRange() * (feeding ? 0.45f : 1.0f);
    // In a fight it is keyed up, and feels somebody coming up at its back well before they touch it: the
    // one it is busy with is not the only one in the room.
    const bool fighting = m_behavior == Behavior::Hunt || m_behavior == Behavior::Attack || m_behavior == Behavior::Drag;
    const float closeSense = Tuning().closeSense * (fighting ? 2.8f : 1.0f);
    // Somebody near is felt whichever way it faces, less so with its head down in a body.
    const float presence = Tuning().presenceRange * (fighting ? 1.5f : 1.0f) * (feeding ? 0.6f : 1.0f);
    const float senseRange = std::max({sightRange, closeSense, presence});
    // Whoever it is busy with it keeps its head on: they are not lost by being off to one side of the
    // way its body happens to point.
    const bool followsTarget = m_behavior == Behavior::Hunt || m_behavior == Behavior::Attack ||
                               m_behavior == Behavior::Stalk || m_behavior == Behavior::Observe ||
                               m_behavior == Behavior::Warn || m_behavior == Behavior::Avoid;
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
                const bool headOnThem = followsTarget && player.id == m_target && track.lastSeen >= 0.0f &&
                                        senses.time - track.lastSeen < 3.0f;
                if (hasEyes && distance < sightRange && (facing >= cosHalfField || headOnThem))
                {
                    // Full in the middle of the view, falling to the edge value at the rim.
                    const float across = headOnThem ? 0.0f : (1.0f - facing) / (1.0f - cosHalfField);
                    field = 1.0f - (1.0f - Tuning().edgeOfView) * across;
                }
                const bool touching = distance < closeSense;
                const bool felt = distance < presence;
                if (touching || felt)
                {
                    field = std::max(field, 0.5f);
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
                        touching || distance >= sightRange ? 1.0f : 1.0f - (distance / sightRange) * (distance / sightRange);
                    const float size = std::clamp(player.height / 1.8f, Tuning().smallSight, 1.0f);
                    const float speed = glm::length(glm::vec3(player.velocity.x, 0.0f, player.velocity.z));
                    const float motion = std::clamp(0.7f + speed * 0.12f, 0.7f, 1.3f);
                    // Touch and smell do not care how dark it is.
                    const float light = hasEyes ? std::clamp(player.light, Tuning().darkSight, 1.0f) : 1.0f;
                    visibility = hasEyes && distance < sightRange ? field * exposed * nearness * size * motion * light : 0.0f;
                    // Close by, felt rather than seen: not how lit or how small, only how near and whether
                    // they move -- and right against it, no question at all.
                    if (clear > 0 && felt)
                    {
                        const float closeness = 1.0f - distance / presence;
                        visibility = std::max(visibility, closeness * (0.35f + 0.25f * std::clamp(speed, 0.0f, 2.0f)));
                    }
                    if (clear > 0 && touching)
                    {
                        visibility = 1.0f;
                    }
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
            // With no eyes there is nothing to stop and stare with: it goes on feeling for them instead.
            if (track.exposure < 1.0f && senses.time >= m_alertIgnoreUntil && hasEyes)
            {
                // Not made out yet, but something is there: stop and look at it.
                if (senses.time > m_alertUntil)
                {
                    m_alertStarted = senses.time;
                }
                m_alertPoint = player.feet + glm::vec3(0.0f, player.height * 0.6f, 0.0f);
                m_alertFeet = player.feet;
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

        // Made out a moment ago and not now: it keeps them through a pillar passing between, or a turn of
        // its head, as anything does that has just been watching somebody move.
        // Only somebody still about where it last saw them and within its sight: a moment of not seeing is not
        // knowing where somebody has gone who is somewhere else entirely.
        const bool held = visibility <= 0.001f && wasVisible && senses.time < track.heldUntil && player.alive && !player.hidden &&
                          Horizontal(player.feet, track.lastKnown) < 1.5f + 1.2f * glm::length(track.lastVelocity) &&
                          glm::distance(player.feet, senses.eye) < std::max(sightRange, presence);
        if (track.exposure >= 1.0f && visibility > 0.001f)
        {
            track.heldUntil = senses.time + Tuning().sightHold;
        }
        if (held)
        {
            track.visible = true;
            track.inView = true;
            track.lastKnown = player.feet;
            track.lastVelocity = player.velocity;
            track.confidence = 1.0f;
            check.verdict = "just lost from view; still following";
            track.sight = check;
        }
        if (track.exposure >= 1.0f && visibility > 0.001f)
        {
            if (track.lastSeen < 0.0f || senses.time - track.lastSeen > 3.0f)
            {
                Log(senses.time, "sees " + player.name);
                if (m_traits.Has(Quirk::Shrieker) && track.hostile)
                {
                    m_shriekPending = true;
                }
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
            track.lastForward = player.forward;
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
            // And slower still for somebody who has hurt it: it does not forget where they were.
            const float remembers = 1.0f + std::min(track.harm, 2.0f) * 0.5f;
            track.confidence = std::max(
                track.confidence - dt / (m_traits.persistence * remembers) * (waitingForThem ? 0.25f : stalkingThem ? 0.5f : 1.0f), 0.0f);
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
        float reach = noise.reach * m_traits.perception * m_traits.hearing * (feeding ? 0.55f : 1.0f);
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

    // --- Being lit ---------------------------------------------------------------------------
    //
    // Somebody's torch turned full on it, near enough to matter, with nothing in the way: a thing that
    // lives in the dark feels that. What it does about it is its own affair (see Decide).
    {
        const int was = m_litBy;
        m_litBy = -1;
        const glm::vec3 middle = senses.onCeiling ? senses.eye : senses.position + glm::vec3(0.0f, m_traits.bodyMiddle, 0.0f);
        for (const SensedPlayer& player : senses.players)
        {
            if (!player.alive || player.hidden || !player.torchOn || glm::length(player.forward) < 1e-4f)
            {
                continue;
            }
            const glm::vec3 eye = player.feet + glm::vec3(0.0f, player.height * 0.9f, 0.0f);
            const glm::vec3 toMe = middle - eye;
            const float distance = glm::length(toMe);
            if (distance > 14.0f || distance < 1e-3f ||
                glm::dot(glm::normalize(player.forward), toMe / distance) < std::cos(glm::radians(22.0f)) ||
                (senses.clearLine && !senses.clearLine(eye, middle)))
            {
                continue;
            }
            m_litBy = player.id;
            m_lastLitAt = senses.time;
            if (was != player.id)
            {
                m_litAt = senses.time;
                m_litCounted = false;
                Log(senses.time, "caught in " + player.name + "'s light");
            }
            // Its eyes, dazzled; its nerves, if it is the nervous sort or has learnt to be.
            m_state.arousal = std::min(m_state.arousal + 0.4f * dt, 1.0f);
            if (m_traits.fear > 0.6f || m_lightShy)
            {
                m_state.fear = std::min(m_state.fear + 0.25f * dt, 1.0f);
            }
            break;
        }
    }

    // --- The others of its kind --------------------------------------------------------------
    //
    // One it can see running at somebody, or crouched watching somebody, is telling it where that
    // somebody is. It does not need to have seen them itself.
    for (const CreatureSenses::Kin& other : senses.kin)
    {
        const bool onToSomebody = other.target >= 0 && other.knowsWhere &&
                                  (other.doing == Behavior::Hunt || other.doing == Behavior::Attack ||
                                   other.doing == Behavior::Stalk || other.doing == Behavior::Drag);
        if (!onToSomebody || glm::distance(other.position, senses.position) > 30.0f ||
            (senses.clearLine && !senses.clearLine(senses.eye, other.position + glm::vec3(0.0f, 0.5f, 0.0f))))
        {
            continue;
        }
        const SensedPlayer* player = FindPlayer(senses, other.target);
        if (player == nullptr || !player->alive)
        {
            continue;
        }
        Track& track = TrackFor(*player);
        if (!track.visible && track.confidence < 0.55f)
        {
            if (track.confidence < 0.2f)
            {
                Log(senses.time, "sees another of its kind after " + track.name);
            }
            track.lastKnown = other.targetAt;
            track.lastHeard = senses.time;
            track.confidence = 0.55f;
            if (track.lastSeen < 0.0f)
            {
                // Enough to go after them as if it had glimpsed them itself.
                track.lastSeen = senses.time - 5.0f;
            }
        }
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

    // The nearest of everybody it means harm to and knows where to find: the others are not ignored for
    // the one it happened to pick first.
    float nearestThreat = 1.0e9f;
    for (const Track& track : m_tracks)
    {
        if (track.hostile && track.confidence > 0.3f)
        {
            nearestThreat = std::min(nearestThreat, Horizontal(senses.position, track.lastKnown));
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
        // Whoever hurt it most is who it wants most, and it does not let that go.
        const float grudge = std::min(1.0f + track.harm * 0.6f, 2.0f);
        // How much it cares that this is a good moment. A brazen creature comes regardless; a
        // stealthy one comes when they are alone, looking away, or out of its patience.
        const float opening = Opening(track, distance);
        const float timing = 1.0f - m_traits.stealth * (1.0f - opening);
        // In a crawlspace too small for it: going after them only puts it on the roof of the thing.
        const bool beyond = OutOfItsReach(senses, *player);
        const float follow = beyond ? 0.15f : 1.0f;
        // Somebody nearer than them is the more pressing: every one of them in the room counts.
        const float nearer = std::clamp(0.55f + 0.45f * (nearestThreat + 1.0f) / (distance + 1.0f), 0.55f, 1.0f);
        // Another of its kind already going straight at them, and closer: that one drives them, and this
        // one does better to go round, wait where they will run, or shadow them -- not pile in behind.
        bool driven = false;
        for (const CreatureSenses::Kin& other : senses.kin)
        {
            if (other.target == track.id && (other.doing == Behavior::Hunt || other.doing == Behavior::Attack) &&
                Horizontal(other.position, player->feet) + 1.0f < distance && distance > 6.0f)
            {
                driven = true;
            }
        }
        if (chases)
        {
            add(Behavior::Hunt, track.id, "Hunt " + track.name,
                {{"sure where", track.confidence},
                 {"aggression", 0.35f + 0.65f * m_traits.aggression},
                 {"not afraid", calm},
                 {"still to close", closeness},
                 {"grudge", grudge},
                 {"the moment", timing},
                 {"can follow", follow},
                 {"nearest of them", nearer},
                 {"another drives them", driven ? 0.55f : 1.0f},
                 {"learnt to be careful", m_wary ? 0.7f : 1.0f},
                 // Somebody shining a light in its face: the bold go for the light; the ones that have
                 // learnt what follows it, do not.
                 {"into the light", m_litBy == track.id ? (m_lightShy ? 0.5f : (m_traits.aggression > 0.7f || m_traits.Has(Quirk::LightChaser) ? 1.3f : 1.0f)) : 1.0f},
                 {"carries a light", m_traits.Has(Quirk::LightChaser) && player->torchOn ? 1.4f : 1.0f},
                 // Not while it is creeping up on them from behind: it goes on creeping until it is close
                 // enough that running is quicker than being heard.
                 {"creeping instead", m_creeping && m_behavior == Behavior::Stalk && m_target == track.id && distance > 4.5f ? 0.35f : 1.0f}});
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
                     {"the others drive them to it", driven ? 1.4f : 1.0f},
                     {"learnt to be careful", m_wary ? 1.25f : 1.0f},
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
                     {"still at it", luring ? std::clamp(1.0f - static_cast<float>(m_lureSpoken) / 5.0f, 0.2f, 1.0f) : 1.0f},
                     // On its way to somewhere to call from, it sees that through.
                     {"on its way to call", luring && m_lureSpoken == 0 && m_haveLureSpot ? 1.35f : 1.0f}});
            }
        }

        // Shadowing them from cover instead. Only somebody it has actually seen: somebody it has
        // only heard is a question to go and answer, not a person to follow.
        if (chases && !player->hidden && track.lastSeen >= 0.0f && track.confidence > 0.3f && distance < 35.0f &&
            distance > m_traits.strikeReach + 0.4f)
        {
            const float patienceLeft =
                std::clamp(1.0f - track.stalked / m_traits.StalkPatienceSeconds(), 0.05f, 1.0f);
            const bool creepingOnThem = m_creeping && m_behavior == Behavior::Stalk && m_target == track.id;
            add(Behavior::Stalk, track.id, "Stalk " + track.name,
                {{"creeping up on them", creepingOnThem ? 1.6f : 1.0f},
                 {"the others drive them", driven ? 1.3f : 1.0f},
                 {"learnt to be careful", m_wary ? 1.25f : 1.0f},
                 {"sure where", track.confidence},
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
// In a crawlspace it cannot get into, only somebody right at the mouth is in reach: a paw in at the
        // opening. Alongside the tunnel, a wall away, is not in reach, however close.
        bool atTheMouth = !beyond;
        if (beyond && senses.nav != nullptr)
        {
            for (const glm::vec3& mouth : senses.nav->CrawlMouths())
            {
                atTheMouth = atTheMouth || (Horizontal(mouth, senses.position) < 1.6f && Horizontal(mouth, player->feet) < m_traits.strikeReach);
            }
        }
        const bool inReach = gap < m_traits.strikeReach + 0.4f && clearToThem && atTheMouth;
        // Not into a crawlspace it cannot follow them into: a swipe in at arm's length is all it has there.
        const bool pounce = gap > 2.6f && gap < 7.0f && std::abs(rise) < 0.8f && now >= m_lungeReadyAt && !beyond && clearToThem;
        if (track.hostile && track.visible && canRise && (inReach || (pounce && chases)))
        {
            add(Behavior::Attack, track.id, "Attack " + track.name,
                {{inReach ? "in reach" : "pounce", inReach ? 1.0f : 0.55f + 0.4f * m_traits.aggression},
                 {"nearest of them", inReach ? 1.0f : nearer},
                 // Leaping at somebody staring straight at it is what the brazen do; the stealthy wait for
                 // them to look away, or for them to come the last step themselves.
                 {"not while watched", inReach || !track.watching ? 1.0f : 1.0f - 0.6f * m_traits.stealth},
                 {"aggression", 0.3f + 0.7f * m_traits.aggression},
                 {"not afraid", 0.2f + 0.8f * calm},
                 // Out of hiding at somebody who has walked into it: the moment all the waiting was for.
                 {"from hiding", m_behavior == Behavior::Ambush || m_behavior == Behavior::Stalk ||
                                         m_behavior == Behavior::Lure
                                     ? 1.4f
                                     : 1.0f}});
        }
    }

    // A body: something to eat, for the hungry sort, when nothing else is happening. Feeding it keeps up
    // while it is at it; afterwards it is not hungry again for a good while.
    {
        const bool atIt = m_behavior == Behavior::Feed;
        // Something left on it, and not in another's jaws: a body picked clean is not a meal, and one
        // being carried off by another of its kind is that one's.
        const BodySense* nearest = nullptr;
        for (const BodySense& body : senses.bodies)
        {
            if (body.meat > 0.1f && (body.carriedBy < 0 || body.carriedBy == senses.selfId) &&
                Horizontal(body.at, senses.position) < 30.0f &&
                (nearest == nullptr || Horizontal(body.at, senses.position) < Horizontal(nearest->at, senses.position)))
            {
                nearest = &body;
            }
        }
        const bool eats = m_traits.temperament == Temperament::Predator || m_traits.temperament == Temperament::Territorial;
        if (nearest != nullptr && eats && (atIt || now >= m_fedUntil))
        {
            // Nobody about, not merely nobody in sight: somebody it has just chased round a corner is still
            // there, and turning from them to a body was dropping a hunt for a meal mid-stride.
            float quiet = 1.0f;
            for (const Track& track : m_tracks)
            {
                if (track.visible)
                {
                    quiet = std::min(quiet, 0.1f);
                }
                else if (track.confidence > 0.3f && Horizontal(track.lastKnown, senses.position) < 25.0f)
                {
                    quiet = std::min(quiet, 0.2f);
                }
                // And somebody it was after a moment ago goes on mattering until the trail is cold: half a
                // minute, not the two seconds it takes to lose them round a corner.
                if (track.hostile && track.lastSeen >= 0.0f)
                {
                    quiet = std::min(quiet, 0.15f + 0.85f * std::clamp((now - std::max(track.lastSeen, track.lastHeard) - 5.0f) / 25.0f, 0.0f, 1.0f));
                }
            }
            // Nor in the middle of going through where they could have gone.
            if (m_behavior == Behavior::Search)
            {
                quiet *= 0.5f;
            }
            // Already at it, and whoever has turned up is not close: it does not give up its meal, it takes it
            // somewhere quieter (see the act). Close, and the meal can wait.
            if (atIt)
            {
                float nearestThem = 1.0e9f;
                for (const Track& track : m_tracks)
                {
                    if (track.visible || track.confidence > 0.35f)
                    {
                        nearestThem = std::min(nearestThem, Horizontal(track.lastKnown, nearest->at));
                    }
                }
                if (nearestThem > 8.0f)
                {
                    quiet = std::max(quiet, 0.5f);
                }
            }
            if (!atIt)
            {
                m_meal = nearest->at;
                m_mealId = nearest->id;
            }
            add(Behavior::Feed, -1, "Feed on a body",
                {{"hungry", 0.35f + 0.5f * m_traits.aggression},
                 {"nobody about", quiet},
                 {"not being shot at", now - m_lastHurt < 10.0f ? 0.15f : 1.0f},
                 {"not afraid", 0.3f + 0.7f * calm},
                 {"at it", atIt ? 1.3f : 1.0f}});
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

    // Giving them room, because the director asked: it goes somewhere out of sight and stays there. Anything
    // that matters -- a gunshot, somebody walking into it -- still scores above it.
    if (now < m_withdrawUntil)
    {
        add(Behavior::Retreat, -1, "Withdraw", {{"giving them room", 0.5f}});
    }

    // Out of the light: the nervous, and anything that has learnt to fear lights, get out of the beam.
    if (m_litBy >= 0 && (m_traits.fear > 0.6f || m_lightShy || (m_traits.stealth > 0.7f && m_behavior == Behavior::Stalk)))
    {
        add(Behavior::Retreat, -1, "Out of the light",
            {{"lit up", 0.5f + 0.3f * m_traits.fear + (m_lightShy ? 0.25f : 0.0f)},
             {"not cornered", m_cornered > 2.5f ? 0.3f : 1.0f}});
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
    const bool faker = m_traits.Has(Quirk::Faker);
    if (m_playDeadCount < (faker ? 4 : 2) && now - m_lastHurt < 1.2f && senses.healthFraction < (faker ? 0.85f : 0.6f) && nearest < 15.0f)
    {
        add(Behavior::PlayDead, -1, "Play dead",
            {{"afraid", faker ? std::max(m_state.fear, 0.7f) : m_state.fear},
             {"a faker", faker ? 1.5f : 1.0f},
             {"cunning", 0.3f + 0.45f * m_traits.stealth + 0.45f * m_traits.patience},
             {"badly hurt", std::clamp(1.5f - senses.healthFraction, 0.5f, 1.0f)},
             {"cannot outrun them", nearest < 8.0f ? 1.0f : 0.6f}});
    }

    // What the brood has learnt works against these people, and what has cost it.
    if (senses.learned != nullptr)
    {
        for (Option& option : m_options)
        {
            const float lean = senses.learned->Weight(TacticLearner::Of(option.behavior));
            if (std::abs(lean - 1.0f) > 0.01f)
            {
                option.considerations.push_back({"what the brood has learnt", lean});
                option.score *= lean;
            }
        }
    }

    // In the middle of a charge at somebody it can see, not far off, it goes through with it: turning
    // aside half way to watch from cover, look into a noise or run is what made it look as if it had
    // changed its mind for no reason. Only real fear stops it.
    {
        const SensedPlayer* them = m_target >= 0 ? FindPlayer(senses, m_target) : nullptr;
        const Track* track = m_target >= 0 ? FindTrack(m_target) : nullptr;
        const bool charging = (m_behavior == Behavior::Hunt || m_behavior == Behavior::Attack) && them != nullptr &&
                              track != nullptr && them->alive && track->visible && senses.routeReached &&
                              Horizontal(senses.position, them->feet) < 10.0f && !OutOfItsReach(senses, *them);
        if (charging)
        {
            const float hold = m_state.fear > 0.75f ? 0.8f : 0.4f;
            for (Option& option : m_options)
            {
                if ((option.behavior == Behavior::Hunt || option.behavior == Behavior::Attack) && option.target == m_target)
                {
                    continue;
                }
                option.considerations.push_back({"in the middle of a charge", hold});
                option.score *= hold;
            }
        }
    }
    // Shot where it was waiting, watching, eating or wandering: that place and that way of going about it
    // are found out. It goes for whoever did it if it has the stomach, and otherwise it goes -- but it
    // does not stay put.
    if (now - m_shotAt < 4.0f)
    {
        const Behavior was = m_shotDuring;
        const bool foundOut = was == Behavior::Stalk || was == Behavior::Ambush || was == Behavior::Observe ||
                              was == Behavior::Lure || was == Behavior::Feed || was == Behavior::Warn ||
                              was == Behavior::Roam || was == Behavior::Investigate || was == Behavior::Search ||
                              was == Behavior::Avoid || was == Behavior::Nest;
        for (Option& option : m_options)
        {
            float change = 1.0f;
            const char* why = "";
            if (foundOut && option.behavior == was)
            {
                change = 0.25f;
                why = "shot where it was";
            }
            else if ((option.behavior == Behavior::Hunt || option.behavior == Behavior::Attack) && option.target == m_shotBy &&
                     m_traits.temperament != Temperament::Timid)
            {
                change = senses.healthFraction > 0.6f ? 1.2f + 0.6f * m_traits.aggression : 1.0f;
                why = "goes for whoever shot it";
            }
            else if (option.behavior == Behavior::Retreat)
            {
                change = 1.0f + 0.5f * m_traits.fear;
                why = "shot";
            }
            if (change != 1.0f)
            {
                option.considerations.push_back({why, change});
                option.score *= change;
            }
        }
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
    m_creeping = false;
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
        m_backstage = Backstage::None;
        m_retreatUntil = std::max(time + 6.0f + 6.0f * m_traits.fear, m_withdrawUntil);
        // Going, it may take one swing at whoever is in reach on the way: the bold, and the ones that
        // have learnt that running with nothing to show for it only brings them back to be shot again.
        m_hitAndRunReady = previous != Behavior::Attack && (m_wary || m_traits.aggression > 0.55f || m_traits.Has(Quirk::HitAndRun));
    }
    if (behavior == Behavior::PlayDead)
    {
        m_haveCrawlSpot = false;
        m_crawlLogged = false;
        m_downWatched = false;
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
        // A trick used is not used again for a while. One never tried -- nowhere to call from -- can be
        // tried again soon, from somewhere else.
        m_lastLureAt = m_lureSpoken > 0 ? time : time - Tuning().lureEvery + 8.0f;
    }
    if (behavior == Behavior::PlayDead)
    {
        // As long as it can bear to, which is as long as it is patient.
        m_playDeadUntil = time + 12.0f + 25.0f * m_traits.patience;
        ++m_playDeadCount;
        m_hurtWhileDown = false;
        m_downHits = 0;
        m_downDamage = 0.0f;
        m_downHurtBy = -1;
        m_downClearSince = -1.0f;
    }
    if (behavior != Behavior::Retreat)
    {
        m_slinking = false;
    }
    if (behavior == Behavior::Feed && previous != Behavior::Feed)
    {
        m_feedStarted = -1.0f;
        m_baiting = false;
        m_carryingBody = false;
        m_bodyPlaced = false;
        m_bodyShareDecided = false;
        m_toFriends = false;
    }
    if (previous == Behavior::Feed && behavior != Behavior::Feed && m_feedStarted >= 0.0f)
    {
        m_fedUntil = time + 240.0f;
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
    // Out of sight is nearly everything: somewhere far away in plain view is not hiding, and a distance
    // score that could outweigh it is what sent it to stand in the open. Then away from what hurt it,
    // against something, dark, and not a journey.
    float bestScore = -1.0f;
    bool found = false;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 28; ++i)
    {
        glm::vec3 candidate;
        if (!senses.nav->RandomPointNear(senses.position, i < 18 ? 30.0f : 14.0f, seed, candidate))
        {
            continue;
        }
        const float away = Horizontal(candidate, m_threat);
        if (away < 7.0f)
        {
            continue;
        }
        float score = SeenFrom(senses, candidate) ? 0.04f : 1.0f;
        score *= std::clamp(away / 20.0f, 0.3f, 1.0f);
        score *= 0.3f + 0.7f * ShelterOf(senses, candidate);
        if (senses.lightAt)
        {
            score *= 1.1f - 0.5f * std::clamp(senses.lightAt(candidate + glm::vec3(0.0f, 0.5f, 0.0f)), 0.0f, 1.0f);
        }
        score *= 1.0f - std::min(Horizontal(candidate, senses.position) / 45.0f, 0.5f);
        // Not past them to get there.
        if (Passes(senses.position, candidate, m_threat) < 4.0f)
        {
            score *= 0.15f;
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

bool CreatureBrain::HuntByEar(const CreatureSenses& senses, const glm::vec3& heardAt, float heardAgo, const std::string& who)
{
    const float now = senses.time;
    const float gap = Horizontal(senses.position, heardAt);
    const glm::vec3 ear = heardAt + glm::vec3(0.0f, 0.6f, 0.0f);
    // A sound just now, and close: it goes for it, fast, head out towards it.
    if (heardAgo < 1.5f && gap < 6.0f)
    {
        m_goal = "going for the sound of " + who;
        m_intent.move = true;
        m_intent.destination = heardAt;
        m_intent.speed = m_traits.runSpeed * 1.1f;
        m_intent.look = true;
        m_intent.lookAt = ear;
        m_listenUntil = -1.0f;
        return true;
    }
    // Otherwise it stops dead now and again, head up and turned to where it last heard them, and listens;
    // and between, creeps a few metres nearer, low and quiet so as not to drown out what it is listening for.
    if (now < m_listenUntil)
    {
        m_goal = "listening for " + who;
        m_intent.crouch = 0.3f;
        m_intent.look = true;
        m_intent.lookAt = ear + glm::vec3(0.0f, 0.3f, 0.0f);
        return true;
    }
    if (now >= m_nextListenAt)
    {
        m_listenUntil = now + m_random.Range(0.8f, 1.7f);
        m_nextListenAt = m_listenUntil + m_random.Range(1.5f, 2.8f);
        m_intent.look = true;
        m_intent.lookAt = ear;
        return true;
    }
    m_goal = "creeping towards where it heard " + who;
    m_intent.move = true;
    m_intent.destination = heardAt;
    m_intent.speed = m_traits.walkSpeed * 1.25f;
    m_intent.crouch = 0.4f;
    m_intent.look = true;
    m_intent.lookAt = ear;
    return true;
}

bool CreatureBrain::FeelAbout(const CreatureSenses& senses, const glm::vec3& centre)
{
    // Three places round where the sound was, a stride out, nose down at each: touch and smell. Then done.
    if (Horizontal(m_sweepCentre, centre) > 1.0f)
    {
        m_sweepCentre = centre;
        m_sweep = 0;
    }
    if (m_sweep >= 3 || senses.nav == nullptr)
    {
        return false;
    }
    const float angle = static_cast<float>(m_sweep) * (glm::two_pi<float>() / 3.0f) + static_cast<float>(m_traits.seed % 7u);
    const glm::vec3 wanted = centre + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * 1.6f;
    if (!senses.nav->NearestPoint(wanted, 1.0f, m_sweepPoint))
    {
        ++m_sweep;
        return true;
    }
    if (Horizontal(senses.position, m_sweepPoint) < 0.5f)
    {
        ++m_sweep;
        return true;
    }
    m_goal = "feeling about where the sound was";
    m_intent.move = true;
    m_intent.destination = m_sweepPoint;
    m_intent.speed = m_traits.walkSpeed * 0.8f;
    m_intent.crouch = 0.8f;
    const glm::vec3 across = glm::cross(senses.forward, glm::vec3(0.0f, 1.0f, 0.0f));
    m_intent.look = true;
    m_intent.lookAt = senses.position + senses.forward * 0.8f + across * (0.4f * std::sin(senses.time * 2.5f));
    return true;
}

bool CreatureBrain::PickCeilingSpot(const CreatureSenses& senses, glm::vec3& out)
{
    // Somewhere with a ceiling it can get up onto, out of everybody's sight, not far. Without a way of
    // asking where the ceilings are, anywhere it would retreat to.
    if (!senses.ceilingAt || senses.nav == nullptr)
    {
        return PickFleePoint(senses, out);
    }
    float best = 0.0f;
    bool found = false;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 32; ++i)
    {
        glm::vec3 candidate;
        if (!senses.nav->RandomPointNear(senses.position, 25.0f, seed, candidate))
        {
            continue;
        }
        const float ceiling = senses.ceilingAt(candidate);
        if (ceiling < 2.2f || ceiling > 4.8f)
        {
            continue;
        }
        float score = SeenFrom(senses, candidate) ? 0.1f : 1.0f;
        score *= 1.0f - std::min(Horizontal(candidate, senses.position) / 40.0f, 0.6f);
        if (senses.lightAt)
        {
            score *= 1.1f - 0.5f * std::clamp(senses.lightAt(candidate + glm::vec3(0.0f, 0.5f, 0.0f)), 0.0f, 1.0f);
        }
        if (score > best)
        {
            best = score;
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
    const glm::vec3 top = point + glm::vec3(0.0f, std::max(m_traits.eyeHeight, m_traits.bodyMiddle * 1.5f), 0.0f);
    for (const Track& track : m_tracks)
    {
        if (track.confidence < 0.2f)
        {
            continue;
        }
        const glm::vec3 eye = track.lastKnown + glm::vec3(0.0f, 1.6f, 0.0f);
        if (glm::distance(eye, body) < 45.0f && (senses.clearLine(eye, body) || senses.clearLine(eye, top)))
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
    else if (senses.time - target.lastSeen < 8.0f)
    {
        facing = glm::vec3(target.lastForward.x, 0.0f, target.lastForward.z);
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
            // Well behind them is worth a good deal more than somewhere in front: that is the way it will come.
            score *= 0.65f - 0.35f * glm::dot(facing, away);
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
            if (passes < 8.0f)
            {
                const float share = passes / 8.0f;
                score *= 0.02f + 0.98f * share * share;
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
        senses.nav->NearestPoint(heading, 3.0f, centre, senses.crawl);
        // One that fits the crawlspaces thinks of them too: somebody who vanished near the mouth of one
        // has likely gone down it, and it goes in after them -- first, before the open floor.
        if (senses.crawl != 0)
        {
            // The mouth nearest where they were, and down it: a stop just inside, then one well in.
            const glm::vec3* nearestMouth = nullptr;
            for (const glm::vec3& mouth : senses.nav->CrawlMouths())
            {
                if (Horizontal(mouth, track.lastKnown) < 10.0f &&
                    (nearestMouth == nullptr || Horizontal(mouth, track.lastKnown) < Horizontal(*nearestMouth, track.lastKnown)))
                {
                    nearestMouth = &mouth;
                }
            }
            if (nearestMouth != nullptr)
            {
                const glm::vec3 mouth = *nearestMouth;
                glm::vec3 near = mouth;
                glm::vec3 deep = mouth;
                float nearest = 1.0e9f;
                float furthest = 0.0f;
                for (int probe = 0; probe < 64; ++probe)
                {
                    const float angle = static_cast<float>(probe % 8) * (glm::two_pi<float>() / 8.0f);
                    const float reach = 1.0f + static_cast<float>(probe / 8);
                    glm::vec3 candidate;
                    if (senses.nav->NearestPoint(mouth + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * reach, 0.8f, candidate,
                                                 NavMesh::kCrawl) &&
                        senses.nav->InCrawlspace(candidate))
                    {
                        const float in = Horizontal(candidate, mouth);
                        if (in < nearest)
                        {
                            nearest = in;
                            near = candidate;
                        }
                        if (in > furthest)
                        {
                            furthest = in;
                            deep = candidate;
                        }
                    }
                }
                if (furthest > nearest + 2.0f)
                {
                    m_searchPlan.insert(m_searchPlan.begin(), SearchStop{deep, -1});
                }
                m_searchPlan.insert(m_searchPlan.begin(), SearchStop{near, -1});
                Log(senses.time, Format("means to look down the crawlspace, %.0f m in", std::max(furthest, nearest)) +
                                     (senses.nav->InCrawlspace(near) ? "" : " (only the mouth)"));
            }
        }
        // Back searching the same place, it searches it closer and more thoroughly, not less: it was
        // sure there was something here, and it has not found it yet.
        const bool again = Horizontal(centre, m_lastSearchCentre) < 8.0f && senses.time - m_lastSearchAt < 120.0f;
        m_searchRound = again ? std::min(m_searchRound + 1, 3) : 0;
        m_lastSearchCentre = centre;
        m_lastSearchAt = senses.time;
        const float radius = 9.0f / (1.0f + 0.6f * static_cast<float>(m_searchRound));
        const float apart = std::max(4.0f / (1.0f + 0.4f * static_cast<float>(m_searchRound)), 2.0f);
        uint32_t seed = static_cast<uint32_t>(m_random.Next());
        for (int i = 0; i < 16 && m_searchPlan.size() < places.size() + 3 + static_cast<size_t>(m_searchRound); ++i)
        {
            glm::vec3 point;
            if (!senses.nav->RandomPointNear(centre, radius, seed, point, senses.crawl))
            {
                continue;
            }
            // Spread out: somewhere already on the list, or where it already is, is not a new place.
            bool spread = Horizontal(point, senses.position) > apart * 0.75f;
            for (const SearchStop& stop : m_searchPlan)
            {
                spread = spread && Horizontal(point, stop.point) > apart;
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

    // Who is near, whether any of them is looking, and whether any of them can see it at all.
    const SensedPlayer* closest = nullptr;
    float closestDistance = 1.0e9f;
    int watching = 0;
    int near = 0;
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
        if (distance < 20.0f)
        {
            ++near;
        }
        if (const Track* track = FindTrack(player.id); track != nullptr && track->watching && distance < 20.0f)
        {
            ++watching;
        }
    }
    const bool seen = near > 0 && SeenFrom(senses, senses.position + glm::vec3(0.0f, 0.3f, 0.0f));

    // Shot lying there. Somebody making sure: the patient take one small round without a twitch, which
    // is the one that tells them it really is dead. A second, or a bad one, and the act is over: it goes
    // for whoever fired if they are right there and it has the nerve, and otherwise it runs.
    // Shot lying there: somebody making sure. The cunning take it without a twitch -- one round, two for
    // the very cunning, three for a born faker -- because that is the round that tells them it really is
    // dead. Past what it can bear, the act is over: up and at whoever fired, if they are close and it has
    // the nerve; otherwise away, and for the bold, round and back at them from somewhere else.
    if (m_hurtWhileDown)
    {
        m_hurtWhileDown = false;
        const float cunning = m_traits.patience + m_traits.stealth;
        const bool faker = m_traits.Has(Quirk::Faker);
        const int canTake = (cunning > 0.9f ? 1 : 0) + (cunning > 1.3f ? 1 : 0) + (faker ? 1 : 0);
        const float canBear = 0.05f + 0.04f * cunning + (faker ? 0.06f : 0.0f);
        if (m_downHits <= canTake && m_downDamage < canBear)
        {
            Log(now, m_downHits == 1 ? "takes the shot without a twitch" : "takes another without a twitch");
            return;
        }
        m_playDeadCount = 99; // and it will not be believed again
        m_haveCrawlSpot = false;
        const SensedPlayer* shooter = m_downHurtBy >= 0 ? FindPlayer(senses, m_downHurtBy) : nullptr;
        const float gap = shooter != nullptr ? Horizontal(senses.position, shooter->feet) : 1.0e9f;
        if (shooter != nullptr && shooter->alive && gap < 5.5f && m_traits.aggression > 0.4f)
        {
            Switch(Behavior::Attack, shooter->id, "shot again; springs at " + shooter->name, now);
            m_committedUntil = now + 1.5f;
            return;
        }
        const bool comesBack = shooter != nullptr && (m_traits.aggression > 0.45f || cunning > 1.2f);
        Switch(Behavior::Retreat, -1, comesBack ? "shot again; bolts, meaning to come back" : "shot again; gives up the act and runs", now);
        m_comeBackFor = comesBack ? shooter->id : -1;
        return;
    }

    // Somebody right over it -- standing on it, prodding it -- gets it at once. Somebody near but with
    // their back to it gets it too. Somebody near and looking at it, with others about, it holds still
    // for: springing up in front of a room full of guns is not an ambush.
    if (closest != nullptr && closestDistance < 2.4f)
    {
        const Track* track = FindTrack(closest->id);
        const bool looking = track != nullptr && track->watching;
        const bool others = watching > (looking ? 1 : 0);
        if (closestDistance < 1.3f || !looking || (!others && m_traits.aggression > 0.4f))
        {
            Switch(Behavior::Attack, closest->id, "springs up at " + closest->name, now);
            m_committedUntil = now + 1.5f;
            return;
        }
        m_goal = "playing dead, somebody standing over it";
    }

    // Somebody near who was looking at it turned their back on it, and it is quick: up and at them before
    // they turn round. Only once the act is under way and somebody has been taken in by it.
    if (watching > 0)
    {
        m_downWatched = true;
    }
    if (closest != nullptr && closestDistance < 7.0f && m_traits.aggression > 0.5f && watching == 0 && m_downWatched &&
        now - m_behaviorStarted > 2.0f)
    {
        const Track* track = FindTrack(closest->id);
        if (track != nullptr && track->watchKnown && !track->lookedLooking && track->isolation > 4.0f)
        {
            Switch(Behavior::Attack, closest->id, closest->name + " turns their back; it springs", now);
            m_committedUntil = now + 1.5f;
            return;
        }
    }

    // Seen lying there, but nobody looking at it this moment: the cunning drag themselves a little way
    // towards somewhere out of sight, and lie still again the moment anybody looks. Whoever looks back
    // finds the body not quite where it was.
    // Nobody looking means it has seen each of them looking elsewhere, not that it does not know.
    bool lookedAway = near > 0;
    for (const SensedPlayer& player : senses.players)
    {
        const Track* track = FindTrack(player.id);
        if (player.alive && !player.hidden && Horizontal(senses.position, player.feet) < 20.0f &&
            (track == nullptr || !track->watchKnown || track->lookedLooking))
        {
            lookedAway = false;
        }
    }
    const float cunning = m_traits.patience + m_traits.stealth;
    if (seen && lookedAway && now - m_behaviorStarted > 2.0f && (cunning > 1.0f || m_traits.Has(Quirk::Faker)) &&
        closestDistance > 3.0f)
    {
        if (!m_haveCrawlSpot && closest != nullptr && senses.nav != nullptr)
        {
            uint32_t seed = static_cast<uint32_t>(m_random.Next());
            float best = 0.0f;
            for (int i = 0; i < 16; ++i)
            {
                glm::vec3 spot;
                if (!senses.nav->RandomPointNear(senses.position, 6.0f, seed, spot))
                {
                    continue;
                }
                float score = SeenFrom(senses, spot) ? 0.1f : 1.0f;
                score *= std::clamp(Horizontal(spot, closest->feet) - closestDistance + 1.0f, 0.1f, 4.0f);
                score *= 0.3f + 0.7f * ShelterOf(senses, spot);
                if (score > best)
                {
                    best = score;
                    m_crawlSpot = spot;
                    m_haveCrawlSpot = true;
                }
            }
        }
        if (m_haveCrawlSpot && Horizontal(senses.position, m_crawlSpot) > 0.6f)
        {
            if (!m_crawlLogged)
            {
                m_crawlLogged = true;
                Log(now, "drags itself away while nobody is looking");
            }
            m_goal = "playing dead, dragging itself towards cover";
            m_intent.move = true;
            m_intent.destination = m_crawlSpot;
            m_intent.speed = 0.4f;
        }
    }

    // Getting up only when nobody can see it, and has not for a few seconds: it waits for them to go.
    if (seen || watching > 0)
    {
        m_downClearSince = -1.0f;
    }
    else if (m_downClearSince < 0.0f)
    {
        m_downClearSince = now;
    }
    const bool clear = m_downClearSince >= 0.0f && now - m_downClearSince > 3.0f;
    const bool waited = now >= m_playDeadUntil || closestDistance > 25.0f || near == 0;
    if (clear && (waited || now - m_behaviorStarted > 6.0f))
    {
        // Up, and then: somebody on their own with their back turned is somebody to creep up on, for
        // the bold; for the rest, away, low and quiet.
        for (const Track& track : m_tracks)
        {
            const SensedPlayer* player = FindPlayer(senses, track.id);
            if (player != nullptr && player->alive && track.hostile && !track.watching && track.isolation > 8.0f &&
                Horizontal(senses.position, player->feet) < 18.0f && m_traits.aggression > 0.55f && m_traits.fear < 0.6f)
            {
                Switch(Behavior::Stalk, track.id, "gets up behind " + track.name, now);
                return;
            }
        }
        Switch(Behavior::Retreat, -1, "gets up unseen and slips away", now);
        m_slinking = true;
        return;
    }
    // Watched for far longer than it can bear: the act will not hold. Up and away at a run.
    if (now >= m_playDeadUntil + 25.0f)
    {
        Switch(Behavior::Retreat, -1, "cannot keep it up any longer; bolts", now);
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
    m_intent.echo.clear();
    m_intent.eat = -1;
    m_intent.carry = -1;
    // Mending is something it does this tick, like everything else here: left set, whatever set it last
    // went on healing it for good.
    m_intent.recover = 0.0f;

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
            else if (m_random.Unit() < 0.55f && PickPlaceOfInterest(senses, m_roamPoint))
            {
                m_haveRoamPoint = true;
                m_goal = std::string("going to ") + m_roamWhy;
            }
            else
            {
                uint32_t seed = static_cast<uint32_t>(m_random.Next());
                m_haveRoamPoint = senses.nav->RandomPointNear(senses.position, 14.0f, seed, m_roamPoint);
                m_roamWhy = "somewhere";
                m_goal = "wandering";
            }
            // A ceiling-dweller takes most legs of it overhead.
            m_roamOverhead = m_traits.Has(Quirk::CeilingDweller) && m_random.Unit() < 0.7f;
            // Its own pace for this leg of it: an amble, a steady walk, now and then a brisk trot.
            const float pace = m_random.Unit();
            m_roamPace = pace < 0.35f ? 0.65f : pace < 0.85f ? 1.0f : 1.35f;
            // Blind, it goes carefully: never at a trot into what it cannot see.
            if (m_traits.sight <= 0.0f)
            {
                m_roamPace = std::min(m_roamPace, 0.8f);
            }
            // Somewhere another of its kind already is, it leaves to that one: a brood spreads out.
            for (int attempt = 0; m_haveRoamPoint && attempt < 4; ++attempt)
            {
                const bool crowded = std::any_of(senses.kin.begin(), senses.kin.end(), [&](const CreatureSenses::Kin& other) {
                    return Horizontal(other.position, m_roamPoint) < 7.0f;
                });
                if (!crowded)
                {
                    break;
                }
                uint32_t seed = static_cast<uint32_t>(m_random.Next());
                m_haveRoamPoint = senses.nav->RandomPointNear(senses.position, 18.0f, seed, m_roamPoint);
            }
        }
        if (m_haveRoamPoint)
        {
            if (Horizontal(senses.position, m_roamPoint) < 0.8f)
            {
                m_haveRoamPoint = false;
                StartPastime(senses, now);
            }
            else
            {
                m_intent.move = true;
                m_intent.destination = m_roamPoint;
                m_intent.speed = m_traits.walkSpeed * m_roamPace;
                // Low and quiet on the way, the sneaking sort, even with nobody about.
                m_intent.crouch = m_traits.stealth > 0.75f ? 0.4f : 0.0f;
            }
        }
        else if (now < m_pauseUntil)
        {
            switch (m_pastime)
            {
            case Pastime::Listen:
                // Dead still, head up and a little on one side, turning slowly towards nothing.
                m_goal = "listening";
                m_intent.look = true;
                m_intent.lookAt = eye + glm::vec3(senses.forward.x, 0.25f, senses.forward.z) * 4.0f +
                                  glm::cross(senses.forward, glm::vec3(0.0f, 1.0f, 0.0f)) * (0.8f * std::sin((now - m_behaviorStarted) * 0.35f));
                break;
            case Pastime::Sniff:
            {
                // Nose to the floor in front of it, working from side to side.
                m_goal = "nosing at the floor";
                m_intent.crouch = 0.8f;
                const glm::vec3 across = glm::cross(senses.forward, glm::vec3(0.0f, 1.0f, 0.0f));
                m_intent.look = true;
                m_intent.lookAt = senses.position + senses.forward * 0.9f + across * (0.5f * std::sin((now - m_behaviorStarted) * 2.3f));
                break;
            }
            case Pastime::Rest:
                // Settled down low somewhere dark, head up now and then.
                m_goal = "lying low";
                m_intent.crouch = 1.0f;
                if (std::fmod(now - m_behaviorStarted, 7.0f) > 5.5f)
                {
                    lookAround(m_pauseUntil);
                }
                break;
            case Pastime::Watch:
                m_goal = std::string("watching ") + m_roamWhy;
                m_intent.crouch = 0.5f;
                m_intent.face = true;
                m_intent.facePoint = m_watchPoint;
                m_intent.look = true;
                m_intent.lookAt = m_watchPoint + glm::vec3(0.0f, 1.0f, 0.0f);
                break;
            case Pastime::LookAround:
                m_goal = "pausing";
                lookAround(m_pauseUntil);
                break;
            case Pastime::Pace:
            {
                m_goal = "pacing";
                const glm::vec3 to = Horizontal(senses.position, m_paceTo) < 0.6f ? m_paceFrom : m_paceTo;
                if (Horizontal(senses.position, m_paceTo) < 0.6f)
                {
                    std::swap(m_paceFrom, m_paceTo);
                }
                m_intent.move = true;
                m_intent.destination = to;
                m_intent.speed = m_traits.walkSpeed * 0.9f;
                break;
            }
            }
        }
        break;
    }

    case Behavior::Investigate:
    {
        m_goal = "going to look at " + m_interest.what;
        // Something new to look at, somewhere else: it has not got there yet, whatever it had got to before.
        if (m_arrived && Horizontal(m_interest.position, m_investigated) > 2.0f)
        {
            m_arrived = false;
            m_opening = -1;
        }
        if (!m_arrived && Horizontal(senses.position, m_interest.position) < 1.4f)
        {
            m_arrived = true;
            m_investigated = m_interest.position;
            m_lookAroundUntil = now + 3.0f;
            m_lookBaseSet = false;
            Log(now, "arrived at the " + m_interest.what + ", looking around");
        }
        // No eyes to look round with: it feels about the place instead.
        if (m_arrived && m_opening == -1 && m_traits.sight <= 0.0f && FeelAbout(senses, m_interest.position))
        {
            break;
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
        else if (m_traits.sight <= 0.0f && m_interest.what != "call")
        {
            HuntByEar(senses, m_interest.position, now - m_interest.time, "the " + m_interest.what);
        }
        else
        {
            m_intent.move = true;
            m_intent.destination = m_interest.position;
            // A call from one of its own is answered at a run.
            m_intent.speed = m_interest.what == "call" ? m_traits.runSpeed * 0.9f : m_traits.walkSpeed * 1.6f;
            if (m_interest.what == "a glimpse")
            {
                // The stealthy creep over to it; the bold just go and see.
                const bool creeps = m_traits.stealth > 0.45f;
                m_goal = creeps ? "creeping over to what it glimpsed" : "going to see what it glimpsed";
                m_intent.speed = m_traits.walkSpeed * (creeps ? 0.85f : 1.5f);
                m_intent.crouch = creeps ? 0.6f : 0.0f;
            }
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
            // Up on anything it cannot get onto and is not beside enough to strike: a crate too wide to reach
            // across is as out of reach as a tower. Running at its side over and over was all it did.
            const bool unreachable = !senses.routeReached && gap < 6.0f && rise > 0.45f &&
                                     (rise > senses.verticalReach || gap > m_traits.strikeReach + 0.3f);
            if (unreachable)
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
            if (senses.nav != nullptr && senses.nav->NearestPoint(guess, 2.0f, onMesh, senses.crawl))
            {
                guess = onMesh;
            }
            const bool blind = m_traits.sight <= 0.0f;
            // With no eyes, where it heard them last, as it heard it: not a guess pushed along by how they
            // were moving, which it could only have seen.
            if (blind)
            {
                guess = track->lastKnown;
                const float heardAgo = now - std::max(track->lastHeard, track->lastSeen);
                // A new sound from them since it got here: off again after it.
                if (m_arrived && heardAgo < 1.0f && Horizontal(senses.position, guess) > 1.6f)
                {
                    m_arrived = false;
                }
                if (!m_arrived && Horizontal(senses.position, guess) >= 1.2f)
                {
                    HuntByEar(senses, guess, heardAgo, track->name);
                    break;
                }
                if (!m_arrived)
                {
                    m_arrived = true;
                    m_lookAroundUntil = now + 2.5f;
                    track->confidence = std::min(track->confidence, 0.5f);
                    Log(now, "got to where it heard " + track->name + "; feels about");
                }
                if (FeelAbout(senses, guess))
                {
                    break;
                }
                m_goal = "listening for " + track->name;
                m_intent.look = true;
                m_intent.lookAt = senses.eye + glm::vec3(senses.forward.x, 0.3f, senses.forward.z) * 3.0f;
                break;
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
                if (m_strikeThenFlee || (m_traits.Has(Quirk::HitAndRun) && m_attackLanded && m_random.Unit() < 0.7f))
                {
                    m_strikeThenFlee = false;
                    const int victim = m_target;
                    Switch(Behavior::Retreat, -1, "and away, out of their sight", now);
                    m_retreatUntil = now + m_random.Range(5.0f, 8.0f);
                    m_comeBackFor = m_traits.aggression > 0.35f ? victim : -1;
                }
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
            // At a run: it has what it came for and wants it away from the others before they react.
            m_intent.speed = m_traits.runSpeed * 0.9f;
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
        if (m_hitAndRunReady && now - m_behaviorStarted < 2.5f)
        {
            for (const Track& track : m_tracks)
            {
                const SensedPlayer* player = FindPlayer(senses, track.id);
                if (player == nullptr || !player->alive || !track.visible || !track.hostile ||
                    Horizontal(senses.position, player->feet) > m_traits.strikeReach + 0.3f ||
                    (senses.clearLine && !senses.clearLine(senses.eye, player->feet + glm::vec3(0.0f, 1.0f, 0.0f))))
                {
                    continue;
                }
                m_hitAndRunReady = false;
                m_strikeThenFlee = true;
                const int who = track.id;
                const glm::vec3 feet = player->feet;
                Switch(Behavior::Attack, who, "one blow on the way out", now);
                StartAttack(AttackKind::Swipe, feet, now);
                m_committedUntil = now + TimingOf(AttackKind::Swipe).duration;
                break;
            }
            if (m_behavior != Behavior::Retreat)
            {
                break;
            }
        }
        // Withdrawing at the director's asking, and whole: it goes backstage -- down a crawlspace if its
        // body fits one, up on the ceiling if it climbs -- and moves about there, out of reach and out of
        // sight, where it can still be heard. The wounded go to the nest instead, as always.
        if (!m_haveFleePoint && now < m_withdrawUntil && senses.healthFraction > 0.5f && senses.nav != nullptr)
        {
            m_backstage = Backstage::None;
            if (senses.crawl != 0)
            {
                const glm::vec3* mouth = nullptr;
                for (const glm::vec3& candidate : senses.nav->CrawlMouths())
                {
                    if (Horizontal(candidate, senses.position) < 40.0f &&
                        (mouth == nullptr || Horizontal(candidate, senses.position) < Horizontal(*mouth, senses.position)))
                    {
                        mouth = &candidate;
                    }
                }
                for (int probe = 0; mouth != nullptr && probe < 64 && m_backstage == Backstage::None; ++probe)
                {
                    const float angle = static_cast<float>(probe % 8) * (glm::two_pi<float>() / 8.0f);
                    const float reach = 6.0f - static_cast<float>(probe / 8) * 0.6f;
                    glm::vec3 inside;
                    if (senses.nav->NearestPoint(*mouth + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * reach, 0.8f, inside,
                                                 NavMesh::kCrawl) &&
                        senses.nav->InCrawlspace(inside))
                    {
                        m_fleePoint = inside;
                        m_haveFleePoint = true;
                        m_backstage = Backstage::Crawlspace;
                        Log(now, "goes down a crawlspace, out of the way");
                    }
                }
            }
            if (m_backstage == Backstage::None && m_traits.climbs && PickCeilingSpot(senses, m_fleePoint))
            {
                m_haveFleePoint = true;
                m_backstage = Backstage::Ceiling;
                Log(now, "goes up out of the way, onto the ceiling");
            }
        }
        if (now >= m_withdrawUntil)
        {
            m_backstage = Backstage::None;
        }
        if (m_backstage == Backstage::Ceiling)
        {
            m_intent.climb = true;
        }
        // Backstage and arrived: it does not sit still up there. Now and then it moves on a little way,
        // and is heard doing it.
        if (m_backstage != Backstage::None && m_haveFleePoint && Horizontal(senses.position, m_fleePoint) <= 1.0f &&
            now >= m_backstageMoveAt && senses.nav != nullptr)
        {
            m_backstageMoveAt = now + m_random.Range(6.0f, 14.0f);
            uint32_t seed = static_cast<uint32_t>(m_random.Next());
            glm::vec3 next;
            const uint16_t allowed = m_backstage == Backstage::Crawlspace ? NavMesh::kCrawl : 0;
            if (senses.nav->RandomPointNear(senses.position, 5.0f, seed, next, allowed) &&
                (m_backstage != Backstage::Crawlspace || senses.nav->InCrawlspace(next)))
            {
                m_fleePoint = next;
            }
        }
        // Away after a blow, meaning to come back: out of that person's sight, not simply away.
        if (!m_haveFleePoint && m_comeBackFor >= 0)
        {
            const Track* victim = FindTrack(m_comeBackFor);
            if (victim != nullptr && PickHidingSpot(senses, victim->lastKnown, m_fleePoint))
            {
                m_haveFleePoint = true;
            }
        }
        // Got there, or run long enough: round at them again, from where it now is.
        if (m_comeBackFor >= 0 && ((m_haveFleePoint && Horizontal(senses.position, m_fleePoint) <= 1.2f) || now >= m_retreatUntil) &&
            senses.healthFraction > 0.3f)
        {
            const int who = m_comeBackFor;
            m_comeBackFor = -1;
            if (const Track* victim = FindTrack(who); victim != nullptr && victim->confidence > 0.2f)
            {
                Switch(Behavior::Stalk, who, "comes round at " + victim->name + " again", now);
                break;
            }
        }
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
            m_goal = m_backstage != Backstage::None ? "moving about out of the way" : m_slinking ? "slinking away" : "getting away";
            m_intent.move = true;
            m_intent.destination = m_fleePoint;
            m_intent.speed = m_slinking || now < m_withdrawUntil ? m_traits.walkSpeed * 1.1f : m_traits.runSpeed * 1.1f;
            m_intent.crouch = m_slinking ? 0.8f : 0.0f;
        }
        else
        {
            // Arrived, and they can see it here after all -- they moved, or it chose badly: somewhere else,
            // at once. Standing in view "hiding" is what it did.
            if (m_backstage == Backstage::None && now >= m_stalkCheckAt && senses.nav != nullptr)
            {
                m_stalkCheckAt = now + 0.8f;
                if (SeenFrom(senses, senses.position) && m_threatened)
                {
                    glm::vec3 better;
                    if (PickFleePoint(senses, better) && Horizontal(better, senses.position) > 2.0f)
                    {
                        m_fleePoint = better;
                        Log(now, "seen where it is hiding; moves on");
                        break;
                    }
                }
            }
            const bool atNest = senses.hasHive && Horizontal(senses.position, senses.hive) < 3.5f;
            m_goal = atNest ? "at the nest, healing"
                     : m_backstage == Backstage::Crawlspace ? "in the crawlspace, out of the way"
                     : m_backstage == Backstage::Ceiling    ? "up on the ceiling, out of the way"
                                                            : "hiding";
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
                m_stalkExposed = SeenFrom(senses, senses.position) || m_litBy >= 0;
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

        // Creeping up behind them. Somebody in sight with their back to it, not looking its way, is the
        // chance the waiting was for: it comes straight at them, low and slow and silent. If they turn it
        // stops dead -- and then either it is close enough to go for them, or it goes back to cover.
        {
            const glm::vec3 fromThem = Flat(senses.position - them);
            const float gap = glm::length(fromThem);
            const glm::vec3 theirFacing = Flat(player->forward);
            const float behindness = gap > 1e-3f && glm::length(theirFacing) > 1e-3f
                                         ? glm::dot(glm::normalize(theirFacing), fromThem / gap)
                                         : 1.0f;
            const bool canCreep = track->visible && !track->watching && behindness < -0.25f && gap < 16.0f &&
                                  gap > m_traits.strikeReach && !senses.onCeiling && !OutOfItsReach(senses, *player);
            if (m_creeping && (!track->visible || track->watching || behindness > 0.15f))
            {
                m_creeping = false;
                m_nextCreepAt = now + m_random.Range(2.0f, 4.0f);
                if (track->watching)
                {
                    // Caught in the open: it freezes, and whatever it does next it does from stillness.
                    m_stareUntil = now + m_random.Range(0.6f, 1.4f);
                    m_haveStalkPoint = false;
                    m_stalkCheckAt = now;
                    Log(now, track->name + " turns round; it freezes");
                }
            }
            else if (!m_creeping && canCreep && now >= m_nextCreepAt && now - m_behaviorStarted > 1.0f)
            {
                m_creeping = true;
                m_peeking = false;
                Log(now, "creeps up behind " + track->name);
            }
            if (m_creeping)
            {
                m_goal = "creeping up behind " + track->name;
                m_intent.move = true;
                m_intent.destination = them;
                m_intent.speed = m_traits.walkSpeed * (gap > 7.0f ? 1.15f : 0.8f);
                m_intent.crouch = 1.0f;
                watch(*player);
                break;
            }
        }

        // A knocker, hidden, raps on the wall now and then: something is there, and it wants them to know.
        if (m_traits.Has(Quirk::Knocker) && !m_stalkExposed && !track->visible && now >= m_nextKnockAt)
        {
            m_nextKnockAt = now + m_random.Range(10.0f, 20.0f);
            m_intent.echo = "World/door_bash";
            Log(now, "knocks on the wall");
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
        // Not while they were looking its way when it last saw them: leaning out into somebody staring at
        // the corner it is behind is how it got seen.
        const bool lookingThisWay = track->watchKnown && track->lookedLooking;
        if (!m_peeking && inCover && !senses.onCeiling && !m_stalkExposed && !lookingThisWay && now >= m_nextPeekAt &&
            senses.nav != nullptr && senses.clearLine)
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
        if (m_peeking && track->watching)
        {
            // Caught looking: straight back behind its cover, and it waits longer before the next look.
            m_peeking = false;
            m_nextPeekAt = now + m_random.Range(3.5f, 7.0f);
            Log(now, track->name + " is looking this way; it ducks back");
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

    case Behavior::Feed:
    {
        if (m_baiting)
        {
            // Lying low near the body, watching it: whoever comes for their friend comes to it.
            m_goal = "lying in wait by a body";
            m_intent.crouch = 1.0f;
            if (Horizontal(senses.position, m_baitSpot) > 0.8f)
            {
                m_intent.move = true;
                m_intent.destination = m_baitSpot;
                m_intent.speed = m_traits.walkSpeed;
            }
            else
            {
                m_intent.face = true;
                m_intent.facePoint = m_meal;
                m_intent.look = true;
                m_intent.lookAt = m_meal + glm::vec3(0.0f, 0.3f, 0.0f);
            }
            if (now - m_feedStarted > 45.0f + 60.0f * m_traits.patience)
            {
                Switch(Behavior::Roam, -1, "nobody came for the body", now);
            }
            break;
        }
        // The body as it is now: somebody else's jaws, or eaten down to nothing, and it is no meal.
        const BodySense* meal = nullptr;
        for (const BodySense& body : senses.bodies)
        {
            if ((m_mealId >= 0 && body.id == m_mealId) || (m_mealId < 0 && Horizontal(body.at, m_meal) < 2.0f))
            {
                meal = &body;
            }
        }
        if (meal == nullptr || meal->meat <= 0.06f || (meal->carriedBy >= 0 && meal->carriedBy != senses.selfId))
        {
            m_carryingBody = false;
            const bool gone = meal == nullptr || meal->meat <= 0.06f;
            if (m_feedStarted >= 0.0f)
            {
                m_fedUntil = now + 90.0f;
            }
            Switch(Behavior::Roam, -1, gone ? "nothing left of the body" : "another of its kind has taken the body", now);
            break;
        }
        m_meal = meal->at;
        const bool inMyJaws = meal->carriedBy == senses.selfId && senses.selfId >= 0;

        // Is it safe to eat here? Anybody it knows of about, near the body or watching it.
        float threatGap = 1.0e9f;
        glm::vec3 threatAt{0.0f};
        for (const Track& track : m_tracks)
        {
            if (!(track.visible || (track.confidence > 0.35f && now - std::max(track.lastSeen, track.lastHeard) < 15.0f)))
            {
                continue;
            }
            const float gap = Horizontal(track.lastKnown, m_meal);
            if (gap < threatGap)
            {
                threatGap = gap;
                threatAt = track.lastKnown;
            }
        }
        const bool unsafe = threatGap < 22.0f;

        // Where to eat it. Once, when it gets to the body: to the others of its kind now and then -- less
        // often the more of them there are to share with -- and away out of sight whenever it is not safe
        // where it lies. Its nest, when it has one near, over anywhere.
        if (!m_carryingBody && !m_bodyPlaced && Horizontal(senses.position, m_meal) < 2.5f)
        {
            if (!m_bodyShareDecided)
            {
                m_bodyShareDecided = true;
                const CreatureSenses::Kin* friendNear = nullptr;
                for (const CreatureSenses::Kin& other : senses.kin)
                {
                    if (Horizontal(other.position, m_meal) < 35.0f && Horizontal(other.position, m_meal) > 4.0f &&
                        (friendNear == nullptr || Horizontal(other.position, m_meal) < Horizontal(friendNear->position, m_meal)))
                    {
                        friendNear = &other;
                    }
                }
                const float share = 0.4f / std::sqrt(static_cast<float>(std::max<size_t>(senses.kin.size(), 1)));
                if (friendNear != nullptr && m_traits.temperament != Temperament::Territorial && m_random.Unit() < share &&
                    senses.nav != nullptr && senses.nav->NearestPoint(friendNear->position + Flat(m_meal - friendNear->position) * 2.0f, 2.5f, m_bodyTo))
                {
                    m_carryingBody = true;
                    m_toFriends = true;
                    Log(now, "takes the body to another of its kind");
                }
            }
            if (!m_carryingBody && unsafe)
            {
                if (senses.hasHive && Horizontal(senses.hive, m_meal) < 40.0f && Horizontal(senses.hive, threatAt) > 12.0f)
                {
                    m_bodyTo = senses.hive;
                    m_carryingBody = true;
                    Log(now, "drags the body back to its nest");
                }
                else if (PickHidingSpot(senses, threatAt, m_bodyTo))
                {
                    m_carryingBody = true;
                    Log(now, "not safe here; drags the body away out of sight");
                }
            }
        }
        if (m_carryingBody)
        {
            // Somebody right on top of it: it drops the body and deals with them (or not) -- which is the
            // rest of the mind's business, when feeding stops scoring.
            if (threatGap < 5.0f && m_feedStarted < 0.0f && !inMyJaws)
            {
                m_carryingBody = false;
            }
            else if (!inMyJaws && Horizontal(senses.position, m_meal) > 1.3f)
            {
                m_goal = "going to a body";
                m_intent.move = true;
                m_intent.destination = m_meal;
                m_intent.speed = m_traits.walkSpeed * 1.5f;
                break;
            }
            else
            {
                m_intent.carry = meal->id;
                m_goal = m_toFriends ? "taking a body to the others" : "dragging a body away";
                m_intent.move = true;
                m_intent.destination = m_bodyTo;
                // It is heavy.
                m_intent.speed = m_traits.walkSpeed * 1.25f;
                m_intent.crouch = 0.6f;
                if (Horizontal(senses.position, m_bodyTo) < 1.4f)
                {
                    m_carryingBody = false;
                    m_bodyPlaced = true;
                    m_intent.carry = -1; // put down here
                    m_intent.move = false;
                    Log(now, "puts the body down");
                }
                break;
            }
        }

        // Up to it, near enough to get its head down in it: a neck's length off, not standing on it.
        // Standing over it, near enough that getting its body down puts its mouth on it.
        const float eatFrom = std::clamp((m_traits.strikeReach - 1.0f) * 0.8f, 0.45f, 1.2f);
        if (Horizontal(senses.position, m_meal) > eatFrom + 0.2f)
        {
            m_goal = "going to a body";
            m_intent.move = true;
            m_intent.destination = m_meal;
            m_intent.speed = m_traits.walkSpeed * 1.4f;
            break;
        }
        if (m_feedStarted < 0.0f)
        {
            m_feedStarted = now;
            m_biteMovedAt = -1.0e9f;
            Log(now, "feeds on a body");
        }
        // Head down in it, tearing, moving about the body now and then; lifting its head every few seconds
        // to look round. Eating mends it: this is what it came for.
        if (now - m_biteMovedAt > 3.0f)
        {
            m_biteMovedAt = now;
            const float angle = m_random.Unit() * glm::two_pi<float>();
            m_biteAt = m_meal + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * m_random.Range(0.0f, 0.45f);
        }
        m_goal = "feeding";
        m_intent.crouch = 1.0f;
        m_intent.face = true;
        m_intent.facePoint = m_biteAt;
        const float cycle = std::fmod(now - m_feedStarted, 6.0f);
        if (cycle < 4.8f)
        {
            m_intent.eat = meal->id;
            m_intent.recover = 0.012f;
            m_intent.attackAt = m_biteAt + glm::vec3(0.0f, 0.12f, 0.0f);
            m_intent.look = true;
            m_intent.lookAt = m_biteAt;
            if (std::fmod(now - m_feedStarted, 2.4f) < 0.02f)
            {
                m_intent.echo = "Creature/bite";
            }
        }
        else
        {
            lookAround(now + 1.0f);
        }
        if (now - m_feedStarted > 18.0f + 20.0f * m_traits.aggression || meal->meat <= 0.08f)
        {
            // Done. The patient and stealthy do not leave the body: somebody will come for it.
            glm::vec3 spot;
            if (meal->meat > 0.08f && ((m_traits.stealth > 0.55f && m_traits.patience > 0.45f) || m_traits.Has(Quirk::Baiter)) &&
                PickHidingSpot(senses, m_meal, spot))
            {
                m_baiting = true;
                m_baitSpot = spot;
                m_feedStarted = now;
                Log(now, "leaves the body, and lies low near it");
            }
            else
            {
                Switch(Behavior::Roam, -1, meal->meat <= 0.08f ? "has picked the body clean" : "has fed", now);
            }
        }
        break;
    }

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
            // Copying them. A curious one watching something it does not understand does what it does:
            // gets down when they get down, steps the way they step, stands still when they stand still
            // -- with its head on one side -- and makes back the noises they make.
            m_goal = m_litBy == track->id ? "staring into " + track->name + "'s light" : "watching " + track->name;
            const bool copies = m_traits.curiosity > 0.45f;
            if (copies)
            {
                if (player->height < 1.35f)
                {
                    m_intent.crouch = 1.0f;
                    m_goal = "getting down as " + track->name + " does";
                }
                const glm::vec3 pace = Flat(player->velocity);
                const float speed = glm::length(pace);
                glm::vec3 step;
                if (speed > 0.6f && senses.nav != nullptr && senses.nav->NearestPoint(senses.position + pace * 0.8f, 1.5f, step))
                {
                    m_intent.move = true;
                    m_intent.destination = step;
                    m_intent.speed = std::min(speed, m_traits.walkSpeed * 1.4f);
                    m_goal = "moving as " + track->name + " moves";
                }
                else if (speed < 0.2f)
                {
                    // Head on one side.
                    const glm::vec3 across = glm::cross(Flat(player->feet - senses.position), glm::vec3(0.0f, 1.0f, 0.0f));
                    if (glm::length(across) > 1e-3f)
                    {
                        m_intent.lookAt += glm::normalize(across) * 0.35f * std::sin(now * 0.7f);
                    }
                }
                for (const Heard& heard : m_heard)
                {
                    if (heard.noise.player == track->id && heard.time > m_lastEchoAt + 0.5f && now - heard.time > 0.8f &&
                        now - heard.time < 2.5f && now - m_lastEchoAt > 6.0f &&
                        (heard.noise.kind == NoiseKind::Door || heard.noise.kind == NoiseKind::Item ||
                         heard.noise.kind == NoiseKind::Impact))
                    {
                        m_intent.echo = heard.noise.kind == NoiseKind::Door   ? "World/door_slam"
                                        : heard.noise.kind == NoiseKind::Item ? "World/drop"
                                                                              : "World/door_bash";
                        m_lastEchoAt = now;
                        Log(now, "makes back the sound " + track->name + " made");
                        break;
                    }
                }
            }
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
            // The persistent go round once more, closer, before they let it go.
            if (m_searchRound < 2 && m_traits.persistence > 16.0f && !m_searchPlan.empty())
            {
                Log(now, "not satisfied; looks again, closer");
                PlanSearch(senses, *track);
                if (!m_searchPlan.empty())
                {
                    break;
                }
            }
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
        // There, and at the same height: on the roof of a crawlspace, over the place, is not in it.
        if (!m_arrived && Horizontal(senses.position, stop.point) < 1.2f && std::abs(senses.position.y - stop.point.y) < 1.0f)
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
    // Stared at long enough without making it out: it goes to see what it is, low and slow.
    if ((m_behavior == Behavior::Roam || m_behavior == Behavior::Investigate) && now < m_alertUntil &&
        now - m_alertStarted > 0.5f + 1.2f * m_traits.stealth + 0.9f * m_traits.patience)
    {
        m_alertUntil = -1.0f;
        m_alertIgnoreUntil = now + 10.0f;
        // Not when it already knows who is over there: then it is not a question, it is them.
        const bool known = std::any_of(m_tracks.begin(), m_tracks.end(), [&](const Track& track) {
            return track.confidence > 0.5f && Horizontal(track.lastKnown, m_alertFeet) < 5.0f;
        });
        if (!known && (m_interest.resolved || m_interest.strength < 0.6f))
        {
            m_interest = {m_alertFeet, 0.6f, now, "a glimpse", false};
            Log(now, "cannot make it out; goes to look");
        }
    }
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

    // A shrieker, having seen somebody: the scream brings the rest.
    if (m_shriekPending)
    {
        m_shriekPending = false;
        m_intent.roar = true;
        Log(now, "shrieks");
    }
    // A ceiling-dweller goes about overhead when it wanders.
    if (m_behavior == Behavior::Roam && m_roamOverhead && m_traits.climbs)
    {
        m_intent.climb = true;
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
