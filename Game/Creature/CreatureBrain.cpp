#include "Game/Creature/CreatureBrain.h"

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
    case Behavior::Stalk:
        return "Stalk";
    case Behavior::PlayDead:
        return "PlayDead";
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
    if (m_dead)
    {
        return;
    }
    m_lastHurt = time;
    if (m_behavior == Behavior::PlayDead)
    {
        m_hurtWhileDown = true;
    }
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
        track.inView = visibility > 0.001f;
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
            // Half as fast for somebody it is stalking: it put the wall between them itself, and knows
            // they were there a moment ago. Without this a patient stalker, waiting behind cover it
            // could not see out of, simply forgot who it was waiting for and wandered off.
            const bool stalkingThem = m_behavior == Behavior::Stalk && m_target == track.id;
            track.confidence =
                std::max(track.confidence - dt / m_traits.persistence * (stalkingThem ? 0.5f : 1.0f), 0.0f);
        }
    }

    // --- Being watched, and who is alone ------------------------------------------------------
    //
    // Whether each player is looking at it. Only for somebody it can at least half make out: it
    // reads a face turned towards it, and a face it cannot see tells it nothing. And how far each
    // one is from the nearest other living player, because somebody on their own is an opening in
    // a way that somebody with company is not.
    const glm::vec3 body = senses.position + glm::vec3(0.0f, 0.7f, 0.0f);
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
    if (senses.time < m_committedUntil || (m_behavior == Behavior::Attack && m_windupStarted >= 0.0f))
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
        // How much it cares that this is a good moment. A brazen creature comes regardless; a
        // stealthy one comes when they are alone, looking away, or out of its patience.
        const float opening = Opening(track, distance);
        const float timing = 1.0f - m_traits.stealth * (1.0f - opening);
        add(Behavior::Hunt, track.id, "Hunt " + track.name,
            {{"sure where", track.confidence},
             {"aggression", 0.35f + 0.65f * m_traits.aggression},
             {"not afraid", calm},
             {"still to close", closeness},
             {"grudge", grudge},
             {"the moment", timing}});

        // Shadowing them from cover instead. Only somebody it has actually seen: somebody it has
        // only heard is a question to go and answer, not a person to follow.
        if (!player->hidden && track.lastSeen >= 0.0f && track.confidence > 0.3f && distance < 35.0f &&
            distance > kStrikeReach + 0.4f)
        {
            const float patienceLeft =
                std::clamp(1.0f - track.stalked / m_traits.StalkPatienceSeconds(), 0.05f, 1.0f);
            add(Behavior::Stalk, track.id, "Stalk " + track.name,
                {{"sure where", track.confidence},
                 {"stealthy", 0.2f + 0.8f * m_traits.stealth},
                 {"not afraid", 0.3f + 0.7f * calm},
                 {"patience left", patienceLeft},
                 {"waiting for a chance", 1.0f - 0.8f * opening}});
        }

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
        if (score > bestScore)
        {
            bestScore = score;
            out = candidate;
            found = true;
        }
    }
    return found;
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
    const glm::vec3 body = point + glm::vec3(0.0f, 0.7f, 0.0f);
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
    const glm::vec3 centre = target.visible && player != nullptr ? player->feet : target.lastKnown;
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
    m_intent.windup = 0.0f;
    m_intent.crouch = 0.0f;
    m_intent.down = false;

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

        // Whether where it is waiting is still cover: they move, and a spot out of sight a moment ago
        // may be in plain view now. Checked a few times a second rather than every tick.
        if (now >= m_stalkCheckAt)
        {
            m_stalkCheckAt = now + 0.7f;
            m_stalkExposed = SeenFrom(senses, senses.position);
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

        // Peeking. Cover it cannot be seen from is cover it cannot see out of, so every few seconds it
        // leans out -- to a spot beside it with a view of them -- takes a look, and slips back. A look
        // that finds them turned away is the opening it has been waiting for.
        const bool inCover = m_haveStalkPoint && Horizontal(senses.position, m_stalkPoint) <= 0.8f;
        if (!m_peeking && inCover && !m_stalkExposed && now >= m_nextPeekAt && senses.nav != nullptr &&
            senses.clearLine)
        {
            const glm::vec3 theirEye = them + glm::vec3(0.0f, 1.6f, 0.0f);
            uint32_t seed = static_cast<uint32_t>(m_random.Next());
            float nearest = 1.0e9f;
            for (int i = 0; i < 10; ++i)
            {
                glm::vec3 spot;
                if (senses.nav->RandomPointNear(m_stalkPoint, 3.0f, seed, spot) &&
                    senses.clearLine(spot + glm::vec3(0.0f, 1.0f, 0.0f), theirEye) &&
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
        else
        {
            m_goal = m_haveStalkPoint ? "watching " + track->name + " from cover"
                                      : "no cover, holding still, watching " + track->name;
            m_intent.face = true;
            m_intent.facePoint = them;
        }
        break;
    }

    case Behavior::PlayDead:
        UpdatePlayingDead(senses);
        break;
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
