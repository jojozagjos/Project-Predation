#include "Game/Creature/CreatureBrain.h"

#include "Engine/Navigation/NavMesh.h"

#include <glm/geometric.hpp>
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
constexpr float kSightRange = 26.0f;
constexpr float kHalfFieldDegrees = 65.0f;
constexpr float kEdgeOfView = 0.35f;
// Anything right beside it is noticed whichever way it is facing: close enough to touch is close
// enough to smell, and a creature that could be walked round in a circle would be a joke.
constexpr float kCloseSense = 1.6f;
// How fast watching somebody fills the exposure meter, and how fast looking away empties it. At full
// visibility a little under half a second is a sighting; a dim, crouched, distant figure takes many
// seconds, and a glance across a lit doorway is only a glance.
constexpr float kExposureGain = 2.4f;
constexpr float kExposureDecay = 0.6f;
constexpr float kSuspicion = 0.35f;

// Striking.
constexpr float kStrikeReach = 2.3f;
constexpr float kWindupSeconds = 0.45f;
constexpr float kStrikeCooldown = 1.3f;

// A decision already being carried out needs this much of a lead to be abandoned. Without it two
// options scoring 0.50 and 0.51 in turn make a creature that twitches between them.
constexpr float kCommitment = 0.12f;

constexpr size_t kTimelineLength = 64;
constexpr float kHeardMemorySeconds = 12.0f;

float Horizontal(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = b - a;
    return std::sqrt(d.x * d.x + d.z * d.z);
}

std::string Format(const char* pattern, float a, float b = 0.0f)
{
    char line[160];
    std::snprintf(line, sizeof(line), pattern, a, b);
    return line;
}

} // namespace

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

void CreatureBrain::OnDamaged(float amount, int byPlayer, const glm::vec3& from, float time)
{
    m_state.pain = std::min(m_state.pain + amount / 60.0f, 2.0f);
    m_state.arousal = std::min(m_state.arousal + 0.5f, 1.0f);
    m_threat = from;
    if (Track* track = FindTrack(byPlayer))
    {
        // Being shot says exactly where the shooter is, and makes them somebody to be wary of.
        track->harm += amount / 100.0f;
        track->confidence = 1.0f;
        track->lastKnown = from;
        ResolveInterestNear(from);
    }
    Log(time, Format("hurt for %.0f", amount) +
                  (FindTrack(byPlayer) != nullptr ? " by " + FindTrack(byPlayer)->name : ""));
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
    m_lastTime = senses.time;
    m_intent.strikeTarget = -1;
    m_pendingNoises.insert(m_pendingNoises.end(), senses.noises.begin(), senses.noises.end());

    m_perceiveTimer += dt;
    if (m_perceiveTimer >= kPerceiveInterval)
    {
        Perceive(senses, m_perceiveTimer);
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
    const float sightRange = kSightRange * m_traits.perception;
    const float cosHalfField = std::cos(glm::radians(kHalfFieldDegrees));
    glm::vec3 flatForward{senses.forward.x, 0.0f, senses.forward.z};
    flatForward = glm::length(flatForward) > 1e-4f ? glm::normalize(flatForward) : glm::vec3(0, 0, -1);

    // --- Sight --------------------------------------------------------------------------------
    for (const SensedPlayer& player : senses.players)
    {
        Track& track = TrackFor(player);
        float visibility = 0.0f;
        if (player.alive && !player.hidden)
        {
            const glm::vec3 chest = player.feet + glm::vec3(0.0f, player.height * 0.6f, 0.0f);
            const glm::vec3 toward = chest - senses.eye;
            const float distance = glm::length(toward);
            if (distance < sightRange && distance > 1e-3f)
            {
                glm::vec3 flat{toward.x, 0.0f, toward.z};
                const float flatLength = glm::length(flat);
                const float facing = flatLength > 1e-4f ? glm::dot(flat / flatLength, flatForward) : 1.0f;
                float field = 0.0f;
                if (facing >= cosHalfField)
                {
                    // Full in the middle of the view, falling to the edge value at the rim.
                    const float across = (1.0f - facing) / (1.0f - cosHalfField);
                    field = 1.0f - (1.0f - kEdgeOfView) * across;
                }
                if (distance < kCloseSense)
                {
                    field = 1.0f;
                }

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
                    const float nearness = 1.0f - (distance / sightRange) * (distance / sightRange);
                    const float size = std::clamp(player.height / 1.8f, 0.25f, 1.0f);
                    const float speed = glm::length(glm::vec3(player.velocity.x, 0.0f, player.velocity.z));
                    const float motion = std::clamp(0.7f + speed * 0.12f, 0.7f, 1.3f);
                    const float light = std::clamp(player.light, 0.08f, 1.0f);
                    visibility = field * exposed * nearness * size * motion * light;
                }
            }
        }

        track.visible = false;
        if (visibility > 0.001f)
        {
            track.exposure = std::min(track.exposure + visibility * kExposureGain * dt, 1.0f);
            if (track.exposure < 1.0f)
            {
                // Not made out yet, but something is there: stop and look at it.
                m_alertPoint = player.feet + glm::vec3(0.0f, player.height * 0.6f, 0.0f);
                m_alertUntil = senses.time + 0.6f;
            }
        }
        else
        {
            track.exposure = std::max(track.exposure - kExposureDecay * dt, 0.0f);
        }

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
            m_state.arousal = std::min(m_state.arousal + 0.2f * dt * 10.0f, 1.0f);
        }
        else if (track.exposure >= kSuspicion && visibility > 0.001f)
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
            track.confidence = std::max(track.confidence - dt / m_traits.persistence, 0.0f);
        }
    }

    // --- Hearing ------------------------------------------------------------------------------
    for (const Noise& noise : m_pendingNoises)
    {
        float reach = noise.reach * m_traits.perception;
        const glm::vec3 ear = senses.eye;
        if (senses.clearLine && !senses.clearLine(noise.position + glm::vec3(0.0f, 0.3f, 0.0f), ear))
        {
            // Through a wall: still heard, not as far.
            reach *= 0.5f;
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
        if (pull > current)
        {
            if (m_interest.resolved || pull > current + 0.2f)
            {
                Log(senses.time, std::string("heard a ") + NoiseKindName(noise.kind) +
                                     Format(" (%.0f%%)", pull * 100.0f));
            }
            m_interest = {noise.position, pull, senses.time, NoiseKindName(noise.kind), false};
        }
    }
    while (!m_heard.empty() && senses.time - m_heard.front().time > kHeardMemorySeconds)
    {
        m_heard.pop_front();
    }

    // --- Feelings -----------------------------------------------------------------------------
    m_state.pain = std::max(m_state.pain - 0.06f * dt, 0.0f);
    m_state.arousal = std::max(m_state.arousal - 0.1f * dt, 0.0f);
    const float injury = 1.0f - std::clamp(senses.healthFraction, 0.0f, 1.0f);
    m_state.fear = std::clamp(m_state.pain * (0.4f + m_traits.fear) + injury * 0.5f * m_traits.fear,
                              0.0f, 1.0f);
}

void CreatureBrain::Decide(const CreatureSenses& senses)
{
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
        add(Behavior::Investigate, -1, "Investigate " + m_interest.what,
            {{"how strong", m_interest.strength},
             {"curiosity", 0.4f + 0.6f * m_traits.curiosity},
             {"not afraid", 0.3f + 0.7f * calm},
             {"recent", recent}});
    }

    // Each person it knows of, separately: hunting one and ignoring another is a real choice.
    for (const Track& track : m_tracks)
    {
        const SensedPlayer* player = FindPlayer(senses, track.id);
        if (player == nullptr || !player->alive || track.confidence <= 0.05f)
        {
            continue;
        }
        const float distance = Horizontal(senses.position, track.visible ? player->feet : track.lastKnown);
        // Closing the distance is what hunting is. Once they are within reach there is nothing left to
        // close, and hunting scores low -- otherwise a creature standing on top of somebody went on
        // "hunting" them, because for an aggressive one the two scored within the commitment margin.
        const float closeness = distance < kStrikeReach ? 0.5f : std::clamp(1.2f - distance / 30.0f, 0.4f, 1.0f);
        const float grudge = std::min(1.0f + track.harm * 0.5f, 1.5f);
        add(Behavior::Hunt, track.id, "Hunt " + track.name,
            {{"sure where", track.confidence},
             {"aggression", 0.35f + 0.65f * m_traits.aggression},
             {"not afraid", calm},
             {"still to close", closeness},
             {"grudge", grudge}});

        if (track.visible && Horizontal(senses.position, player->feet) < kStrikeReach + 0.4f)
        {
            add(Behavior::Attack, track.id, "Attack " + track.name,
                {{"in reach", 1.0f},
                 {"aggression", 0.55f + 0.45f * m_traits.aggression},
                 {"not afraid", 0.2f + 0.8f * calm}});
        }
    }

    // Getting away. Once it has decided to, it keeps going for a while rather than turning back the
    // moment the pain fades a little -- a wounded animal does not stop running at the first corner.
    const bool retreating = m_behavior == Behavior::Retreat && now < m_retreatUntil;
    if (retreating || (m_state.fear > 0.3f && now >= m_retreatCooldownUntil))
    {
        add(Behavior::Retreat, -1, "Retreat",
            {{"afraid", retreating ? std::max(m_state.fear, 0.6f) : m_state.fear},
             {"timid", 0.6f + 0.6f * m_traits.fear}});
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
    const float commitment = m_behavior == Behavior::Roam ? 0.0f : kCommitment;
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
    m_behavior = behavior;
    m_target = target;
    m_behaviorStarted = time;
    m_arrived = false;
    m_windupStarted = -1.0f;
    m_haveRoamPoint = false;
    m_lookAroundUntil = 0.0f;
    if (behavior == Behavior::Retreat)
    {
        m_haveFleePoint = false;
        m_retreatUntil = time + 6.0f + 6.0f * m_traits.fear;
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
        if (score > bestScore)
        {
            bestScore = score;
            out = candidate;
            found = true;
        }
    }
    return found;
}

void CreatureBrain::Act(const CreatureSenses& senses, float dt)
{
    (void)dt;
    const float now = senses.time;
    m_intent.move = false;
    m_intent.face = false;
    m_intent.windup = 0.0f;

    const auto lookAround = [&](float until)
    {
        // Turning its head slowly one way and the other, which is what searching looks like.
        const float angle = std::sin((now - m_behaviorStarted) * 1.3f) * 1.4f;
        const glm::vec3 base = glm::length(senses.forward) > 1e-4f ? senses.forward : glm::vec3(0, 0, -1);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        m_intent.face = true;
        m_intent.facePoint =
            senses.position + glm::vec3(base.x * c - base.z * s, 0.0f, base.x * s + base.z * c) * 3.0f;
        return now < until;
    };

    switch (m_behavior)
    {
    case Behavior::Roam:
    {
        if (!m_haveRoamPoint && now >= m_pauseUntil && senses.nav != nullptr)
        {
            uint32_t seed = static_cast<uint32_t>(m_random.Next());
            m_haveRoamPoint = senses.nav->RandomPointNear(senses.position, 14.0f, seed, m_roamPoint);
            m_goal = "wandering";
        }
        if (m_haveRoamPoint)
        {
            if (Horizontal(senses.position, m_roamPoint) < 0.8f)
            {
                m_haveRoamPoint = false;
                m_pauseUntil = now + m_random.Range(1.5f, 3.5f);
                m_goal = "pausing";
            }
            else
            {
                m_intent.move = true;
                m_intent.destination = m_roamPoint;
                m_intent.speed = m_traits.walkSpeed;
            }
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
            Log(now, "arrived at the " + m_interest.what + ", looking around");
        }
        if (m_arrived)
        {
            m_goal = "looking around";
            if (!lookAround(m_lookAroundUntil))
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
            m_intent.speed = m_traits.walkSpeed * 1.6f;
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
            // Up to them, not onto them: it stops at striking distance and faces them.
            if (Horizontal(senses.position, player->feet) > 1.3f)
            {
                m_intent.move = true;
                m_intent.destination = player->feet;
                m_intent.speed = m_traits.runSpeed;
            }
            m_intent.face = true;
            m_intent.facePoint = player->feet;
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
                track->lastVelocity = glm::vec3(0.0f);
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
        if (player == nullptr)
        {
            break;
        }
        m_goal = "striking at " + player->name;
        m_intent.face = true;
        m_intent.facePoint = player->feet;
        const float distance = Horizontal(senses.position, player->feet);
        if (distance > 1.5f && m_windupStarted < 0.0f)
        {
            m_intent.move = true;
            m_intent.destination = player->feet;
            m_intent.speed = m_traits.runSpeed;
        }
        if (m_windupStarted < 0.0f && now >= m_attackCooldownUntil && distance < kStrikeReach)
        {
            m_windupStarted = now;
        }
        if (m_windupStarted >= 0.0f)
        {
            m_intent.windup = std::clamp((now - m_windupStarted) / kWindupSeconds, 0.0f, 1.0f);
            if (now - m_windupStarted >= kWindupSeconds)
            {
                // The strike itself. The game decides whether it connected: somebody who stepped back
                // during the wind-up is not hit, which is what makes the wind-up worth watching.
                m_intent.strikeTarget = m_target;
                m_windupStarted = -1.0f;
                m_attackCooldownUntil = now + kStrikeCooldown;
                Log(now, "strikes at " + player->name);
            }
        }
        break;
    }

    case Behavior::Retreat:
    {
        if (!m_haveFleePoint)
        {
            m_haveFleePoint = PickFleePoint(senses, m_fleePoint);
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
            m_goal = "hiding, recovering";
            lookAround(1.0e9f);
        }
        break;
    }
    }

    // The orienting response, over whatever it was doing if that was only wandering or looking into a
    // noise: something moved at the edge of its view, so it stops and faces it until it has made it
    // out or it has gone. Not while hunting, attacking or running, which already have somewhere to
    // look.
    if ((m_behavior == Behavior::Roam || m_behavior == Behavior::Investigate) && now < m_alertUntil)
    {
        m_intent.move = false;
        m_intent.face = true;
        m_intent.facePoint = m_alertPoint;
        m_goal = "something caught its eye";
    }
}

} // namespace pred
