// The creature's tactics: where to hide, where to lie in wait, and how to come at shooting.
//
// Part of CreatureBrain, in a file of its own because the brain is long enough already and this is one
// subject with an edge: every function here is a question about places -- which of them is cover, which
// is on the way somebody has to go, which is out of sight of somewhere -- answered from the navigation
// mesh and the line-of-sight and shelter questions the game answers. None of it is a list of places a
// map names. A doorway, a crawlspace or a corner added to a level is used without being told about.

#include "Game/Creature/CreatureBrain.h"
#include "Game/Creature/CreatureTuning.h"

#include "Engine/Navigation/NavMesh.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

// Across the floor, whatever the difference in height of a block or a crouch -- but not between storeys:
// somebody on the floor above, straight overhead, is a flight of stairs away and not here, and taking them
// for here is what kept a creature standing under where it wanted to be.
float Horizontal(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = b - a;
    const float flat = std::sqrt(d.x * d.x + d.z * d.z);
    const float rise = std::abs(d.y);
    return rise > 2.8f ? flat + rise * 2.0f : flat;
}

glm::vec3 Flat(const glm::vec3& v)
{
    const glm::vec3 flat{v.x, 0.0f, v.z};
    const float length = glm::length(flat);
    return length > 1e-4f ? flat / length : glm::vec3(0.0f);
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

std::string Format(const char* pattern, float a, float b = 0.0f)
{
    char line[160];
    std::snprintf(line, sizeof(line), pattern, a, b);
    return line;
}

} // namespace

float CreatureBrain::ShelterOf(const CreatureSenses& senses, const glm::vec3& point) const
{
    return senses.shelterAt ? std::clamp(senses.shelterAt(point), 0.0f, 1.0f) : 0.5f;
}

bool CreatureBrain::TightCover(const CreatureSenses& senses, const glm::vec3& point, const glm::vec3& from) const
{
    if (!senses.clearLine)
    {
        return false;
    }
    const glm::vec3 body = point + glm::vec3(0.0f, m_traits.bodyMiddle, 0.0f);
    const glm::vec3 toward = from + glm::vec3(0.0f, 1.6f, 0.0f) - body;
    const float distance = glm::length(toward);
    if (distance < 1.5f)
    {
        return false;
    }
    // Within a couple of strides of it, towards them: a wall it is behind, rather than a corner forty
    // metres off that happens to be in the way.
    return !senses.clearLine(body, body + toward / distance * std::min(2.5f, distance - 0.5f));
}

bool CreatureBrain::OutOfItsReach(const CreatureSenses& senses, const SensedPlayer& player) const
{
    return !m_traits.fitsVents && senses.nav != nullptr && senses.nav->InCrawlspace(player.feet);
}

void CreatureBrain::RememberDanger(const glm::vec3& at, float until)
{
    for (Danger& danger : m_dangers)
    {
        if (Horizontal(danger.at, at) < 6.0f)
        {
            danger.at = at;
            danger.until = std::max(danger.until, until);
            return;
        }
    }
    if (m_dangers.size() >= 8)
    {
        m_dangers.erase(std::min_element(m_dangers.begin(), m_dangers.end(),
                                         [](const Danger& a, const Danger& b) { return a.until < b.until; }));
    }
    m_dangers.push_back({at, until});
}

bool CreatureBrain::NearDanger(const glm::vec3& point, float within, float now) const
{
    return std::any_of(m_dangers.begin(), m_dangers.end(), [&](const Danger& danger) {
        return danger.until > now && Horizontal(danger.at, point) < within;
    });
}

bool CreatureBrain::PlanAmbush(const CreatureSenses& senses, const Track& track, const SensedPlayer* player,
                               AmbushPlan& out) const
{
    out = AmbushPlan{};
    if (senses.nav == nullptr)
    {
        return false;
    }
    const glm::vec3 them = track.visible && player != nullptr ? player->feet : track.lastKnown;

    // In a crawlspace it cannot get into: beside whichever mouth they are nearest, or heading for, out
    // of the line of the tunnel so that looking out of it they do not see it waiting.
    if (player != nullptr && OutOfItsReach(senses, *player))
    {
        const glm::vec3 heading = Flat(track.lastVelocity);
        float best = 0.0f;
        for (const glm::vec3& mouth : senses.nav->CrawlMouths())
        {
            const float distance = Horizontal(mouth, them);
            if (distance > 30.0f)
            {
                continue;
            }
            float score = 1.0f - distance / 35.0f;
            if (glm::length(heading) > 0.5f)
            {
                score *= 0.6f + 0.4f * std::max(glm::dot(heading, Flat(mouth - them)), 0.0f);
            }
            if (score <= best)
            {
                continue;
            }
            // Beside the opening: square to the way the tunnel runs, which is roughly from them to it.
            const glm::vec3 along = Flat(mouth - them);
            const glm::vec3 side{-along.z, 0.0f, along.x};
            for (const float s : {1.0f, -1.0f})
            {
                glm::vec3 spot;
                if (senses.nav->NearestPoint(mouth + side * s * 1.3f + along * 0.3f, 1.0f, spot) &&
                    Horizontal(spot, mouth) > 0.6f && Horizontal(spot, mouth) < 2.4f)
                {
                    best = score;
                    out.valid = true;
                    out.kind = AmbushKind::Crawlspace;
                    out.point = spot;
                    out.watch = mouth;
                    out.target = track.id;
                    out.quality = 1.0f;
                    break;
                }
            }
        }
        if (out.valid)
        {
            return true;
        }
    }
    if (!PlanDoorAmbushNear(senses, them, out))
    {
        return false;
    }
    out.target = track.id;
    return true;
}

bool CreatureBrain::PlanDoorAmbushNear(const CreatureSenses& senses, const glm::vec3& them, AmbushPlan& out) const
{
    out = AmbushPlan{};
    if (senses.nav == nullptr)
    {
        return false;
    }
    float best = 0.0f;
    for (const DoorSense& door : senses.doors)
    {
        const glm::vec3 middle = (door.a + door.b) * 0.5f;
        const float away = Horizontal(middle, them);
        // A locked door is not a way out of anywhere; one far from them is not one they are about to use.
        if (door.locked || away > 16.0f)
        {
            continue;
        }
        const glm::vec3 along = Flat(door.b - door.a);
        const float half = Horizontal(door.a, door.b) * 0.5f;
        const glm::vec3 across{-along.z, 0.0f, along.x};
        const float theirSide = glm::dot(them - middle, across);
        if (std::abs(theirSide) < 0.6f)
        {
            continue; // standing in the doorway: that is not an ambush, that is a meeting
        }
        const float sign = theirSide > 0.0f ? 1.0f : -1.0f;
        for (const float s : {1.0f, -1.0f})
        {
            // On the far side from them, flat against the wall beside the frame, where somebody coming
            // through sees it only once they are through.
            glm::vec3 spot;
            const glm::vec3 wanted = middle - across * sign * 0.75f + along * s * (half + 0.6f);
            if (!senses.nav->NearestPoint(wanted, 0.8f, spot) || Horizontal(spot, wanted) > 0.7f)
            {
                continue;
            }
            // Not somewhere it would have to walk past them to get to: on their side of the door, the way
            // there is through the doorway, and not when they are standing by it.
            const bool theirSideToo = glm::dot(senses.position - middle, across) * theirSide > 0.0f;
            if (theirSideToo && Passes(senses.position, middle, them) < 3.0f)
            {
                continue;
            }
            const bool hidden = !senses.clearLine ||
                                !senses.clearLine(them + glm::vec3(0.0f, 1.6f, 0.0f), spot + glm::vec3(0.0f, m_traits.bodyMiddle, 0.0f));
            float score = hidden ? 1.0f : 0.15f;
            score *= 1.0f - 0.3f * away / 16.0f;
            score *= 0.8f + 0.2f * ShelterOf(senses, spot);
            // Near enough to get to before they move on.
            score *= 1.0f - std::min(Horizontal(senses.position, spot) / 60.0f, 0.4f);
            if (score > best)
            {
                best = score;
                out.valid = true;
                out.kind = AmbushKind::Door;
                out.point = spot;
                out.watch = middle;
                out.door = door.index;
                out.quality = score;
            }
        }
    }
    return out.valid && out.quality > 0.2f;
}

bool CreatureBrain::PickFlankPoint(const CreatureSenses& senses, const glm::vec3& source, glm::vec3& out)
{
    if (senses.nav == nullptr)
    {
        return false;
    }
    const glm::vec3 direct = Flat(senses.position - source);
    float best = 0.0f;
    bool found = false;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 32; ++i)
    {
        glm::vec3 candidate;
        if (!senses.nav->RandomPointNear(source, 15.0f, seed, candidate))
        {
            continue;
        }
        // On their floor: round to them is not the floor below them.
        const float distance = Horizontal(candidate, source);
        if (distance < 6.0f || distance > 14.0f || std::abs(candidate.y - source.y) > 1.5f)
        {
            continue;
        }
        // Whoever fired should not see it arrive.
        const bool hidden = !senses.clearLine || !senses.clearLine(source + glm::vec3(0.0f, 1.5f, 0.0f),
                                                                   candidate + glm::vec3(0.0f, m_traits.bodyMiddle, 0.0f));
        float score = hidden ? 1.0f : 0.12f;
        // Off to the side of the way straight in, which is the way they are looking if they are looking
        // for it: square to it is best, straight along it worst.
        if (glm::length(direct) > 0.5f)
        {
            const float along = glm::dot(direct, Flat(candidate - source));
            score *= 0.3f + 0.7f * std::sqrt(std::max(1.0f - along * along, 0.0f));
        }
        score *= 0.4f + 0.6f * ShelterOf(senses, candidate);
        score *= 1.0f - std::min(Horizontal(candidate, senses.position) / 50.0f, 0.6f);
        // And not reached by walking up to them first.
        const float passes = Passes(senses.position, candidate, source);
        if (passes < 5.0f)
        {
            score *= 0.1f + 0.9f * passes / 5.0f;
        }
        // And near them by walking, not only as the crow flies: a floor above them, or the far side of a
        // wall with the way round it through a stairwell, is not round to them at all.
        if (score > best)
        {
            std::vector<glm::vec3> corners;
            bool reached = false;
            senses.nav->FindPath(source, candidate, corners, &reached);
            float walk = 0.0f;
            for (size_t c = 1; c < corners.size(); ++c)
            {
                walk += glm::distance(corners[c - 1], corners[c]);
            }
            if (!reached || walk > distance * 1.7f + 4.0f)
            {
                continue;
            }
            best = score;
            out = candidate;
            found = true;
        }
    }
    return found && best > 0.15f;
}

bool CreatureBrain::PickHidingSpot(const CreatureSenses& senses, const glm::vec3& awayFrom, glm::vec3& out)
{
    if (senses.nav == nullptr)
    {
        return false;
    }
    float best = 0.0f;
    bool found = false;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 24; ++i)
    {
        glm::vec3 candidate;
        if (!senses.nav->RandomPointNear(senses.position, 16.0f, seed, candidate))
        {
            continue;
        }
        const float distance = Horizontal(candidate, awayFrom);
        if (distance < 8.0f)
        {
            continue;
        }
        float score = SeenFrom(senses, candidate) ? 0.05f : 1.0f;
        score *= 0.2f + 0.8f * ShelterOf(senses, candidate);
        if (senses.lightAt)
        {
            score *= 1.1f - 0.5f * std::clamp(senses.lightAt(candidate + glm::vec3(0.0f, 0.5f, 0.0f)), 0.0f, 1.0f);
        }
        score *= 1.0f - std::min(Horizontal(candidate, senses.position) / 30.0f, 0.6f);
        if (Passes(senses.position, candidate, awayFrom) < 4.0f)
        {
            score *= 0.2f;
        }
        if (score > best)
        {
            best = score;
            out = candidate;
            found = true;
        }
    }
    return found && best > 0.1f;
}

void CreatureBrain::ActAmbush(const CreatureSenses& senses, float dt)
{
    (void)dt;
    const float now = senses.time;
    Track* track = m_ambush.target >= 0 ? FindTrack(m_ambush.target) : nullptr;
    const std::string who = track != nullptr ? track->name : std::string("whoever comes");
    if (!m_ambush.valid)
    {
        Switch(Behavior::Roam, -1, "nowhere to wait", now);
        return;
    }
    if (!m_ambushArrived)
    {
        if (Horizontal(senses.position, m_ambush.point) < 0.7f)
        {
            m_ambushArrived = true;
            m_ambushSince = now;
            Log(now, m_ambush.kind == AmbushKind::Crawlspace ? "waits at the mouth of the crawlspace for " + who
                                                              : "lies in wait by the door for " + who);
        }
        else
        {
            const bool seen = SeenFrom(senses, senses.position);
            m_goal = (m_ambush.kind == AmbushKind::Crawlspace ? "going round to where " : "getting ahead of ") + who;
            m_intent.move = true;
            m_intent.destination = m_ambush.point;
            // Quickly while it is still far from where they are, and quietly as it gets close.
            const bool close = Horizontal(senses.position, m_ambush.watch) < 10.0f;
            m_intent.speed = seen || !close ? m_traits.runSpeed * 0.8f : m_traits.walkSpeed * 1.3f;
            m_intent.crouch = close ? 0.6f : 0.0f;
            return;
        }
    }
    // Waiting: low, still, facing the way they will come, its head on the doorway or the opening.
    m_goal = (m_ambush.kind == AmbushKind::Crawlspace ? "waiting at the crawlspace for " : "waiting by the door for ") + who;
    m_intent.crouch = 1.0f;
    m_intent.face = true;
    m_intent.facePoint = m_ambush.watch;
    m_intent.look = true;
    m_intent.lookAt = m_ambush.watch + glm::vec3(0.0f, 0.8f, 0.0f);
    // One that climbs waits overhead, where nobody looks.
    if (m_traits.climbs && senses.ceilingAt)
    {
        const float ceiling = senses.ceilingAt(senses.position);
        m_intent.climb = ceiling > 2.2f && ceiling < 4.8f;
    }
    // Longer for a patient one, and never for ever.
    const float patience = Tuning().ambushPatience + Tuning().ambushPatienceRange * m_traits.patience;
    const bool movedOn = track != nullptr && !track->visible && track->confidence > 0.3f &&
                         Horizontal(track->lastKnown, m_ambush.watch) > 26.0f;
    if (now - m_ambushSince > patience || movedOn)
    {
        Log(now, movedOn ? who + " went somewhere else" : "gives up waiting for " + who);
        if (track != nullptr)
        {
            track->confidence = std::min(track->confidence, 0.5f);
        }
        m_ambush.valid = false;
        m_intent.climb = false;
        Switch(track != nullptr ? Behavior::Search : Behavior::Roam, track != nullptr ? track->id : -1,
               "the wait came to nothing", now);
    }
}

int CreatureBrain::VoiceFor(const CreatureSenses& senses, int target) const
{
    const auto usable = [&](int player)
    {
        return std::find(m_voicesHeard.begin(), m_voicesHeard.end(), player) != m_voicesHeard.end() &&
               std::find(senses.voices.begin(), senses.voices.end(), player) != senses.voices.end();
    };
    const SensedPlayer* them = FindPlayer(senses, target);
    // A friend of theirs, best of all one who is not beside them: a voice from over there that they
    // know is somewhere else is a reason to go and look.
    int best = -1;
    float furthest = -1.0f;
    for (const SensedPlayer& player : senses.players)
    {
        if (player.id == target || !usable(player.id))
        {
            continue;
        }
        const float away = them != nullptr ? Horizontal(player.feet, them->feet) : 10.0f;
        if (away > furthest)
        {
            furthest = away;
            best = player.id;
        }
    }
    if (best >= 0)
    {
        return best;
    }
    // Alone, or only ever heard them: their own voice, out of the dark.
    return usable(target) ? target : -1;
}

bool CreatureBrain::PickLureSpot(const CreatureSenses& senses, const glm::vec3& them, glm::vec3& out)
{
    if (senses.nav == nullptr)
    {
        return false;
    }
    float best = 0.0f;
    bool found = false;
    uint32_t seed = static_cast<uint32_t>(m_random.Next());
    for (int i = 0; i < 28; ++i)
    {
        glm::vec3 candidate;
        if (!senses.nav->RandomPointNear(them, 15.0f, seed, candidate))
        {
            continue;
        }
        const float distance = Horizontal(candidate, them);
        if (distance < 6.0f || distance > 15.0f)
        {
            continue;
        }
        // Heard, not seen: round a corner from them, against something, in the dark.
        float score = SeenFrom(senses, candidate) ? 0.05f : 1.0f;
        score *= 0.3f + 0.7f * ShelterOf(senses, candidate);
        if (TightCover(senses, candidate, them))
        {
            score *= 1.4f;
        }
        if (senses.lightAt)
        {
            score *= 1.1f - 0.5f * std::clamp(senses.lightAt(candidate + glm::vec3(0.0f, 0.5f, 0.0f)), 0.0f, 1.0f);
        }
        score *= 1.0f - std::min(Horizontal(candidate, senses.position) / 45.0f, 0.6f);
        if (Passes(senses.position, candidate, them) < 4.0f && Horizontal(senses.position, them) > 4.0f)
        {
            score *= 0.2f;
        }
        if (score > best)
        {
            best = score;
            out = candidate;
            found = true;
        }
    }
    return found && best > 0.15f;
}

void CreatureBrain::ActLure(const CreatureSenses& senses, float dt)
{
    (void)dt;
    const float now = senses.time;
    Track* track = FindTrack(m_target);
    const SensedPlayer* player = FindPlayer(senses, m_target);
    if (track == nullptr || player == nullptr)
    {
        Switch(Behavior::Roam, -1, "nobody to lure", now);
        return;
    }
    const glm::vec3 them = track->visible ? player->feet : track->lastKnown;
    if (m_lureVoice < 0)
    {
        m_lureVoice = VoiceFor(senses, m_target);
        if (m_lureVoice < 0)
        {
            Switch(Behavior::Stalk, m_target, "has no voice to use", now);
            return;
        }
    }
    if (!m_haveLureSpot)
    {
        m_haveLureSpot = PickLureSpot(senses, them, m_lureSpot);
        if (!m_haveLureSpot)
        {
            Switch(Behavior::Stalk, m_target, "nowhere to call from", now);
            return;
        }
        Log(now, Format("goes somewhere out of sight %.0f m from ", Horizontal(m_lureSpot, them)) + track->name + " to call");
        m_nextMimicAt = now;
    }
    if (Horizontal(senses.position, m_lureSpot) > 0.8f)
    {
        m_goal = "creeping somewhere to call " + track->name + " from";
        m_intent.move = true;
        m_intent.destination = m_lureSpot;
        const bool close = Horizontal(senses.position, them) < 16.0f;
        m_intent.speed = close ? m_traits.walkSpeed * 1.3f : m_traits.runSpeed * 0.8f;
        m_intent.crouch = close ? 0.8f : 0.2f;
        return;
    }
    // There: low, still, facing the way they will come, and every so often, a voice they know.
    m_goal = "calling " + track->name + " in a voice they know";
    m_intent.crouch = 1.0f;
    m_intent.face = true;
    m_intent.facePoint = them;
    if (m_traits.climbs && senses.ceilingAt)
    {
        const float ceiling = senses.ceilingAt(senses.position);
        m_intent.climb = ceiling > 2.2f && ceiling < 4.8f;
    }
    if (now >= m_nextMimicAt && m_lureSpoken < 4)
    {
        m_intent.mimic = m_lureVoice;
        ++m_lureSpoken;
        m_nextMimicAt = now + m_random.Range(6.0f, 13.0f);
        Log(now, "speaks in " + (m_lureVoice == m_target ? track->name + "'s own voice" : std::string("a friend's voice")));
    }
    // Four times, and a while to see whether anybody comes. Then it gives it up for now.
    if (m_lureSpoken >= 4 && now >= m_nextMimicAt + 6.0f)
    {
        Switch(Behavior::Stalk, m_target, "nobody came", now);
    }
}

void CreatureBrain::ActFlank(const CreatureSenses& senses, float dt)
{
    (void)dt;
    const float now = senses.time;
    if (m_interest.resolved)
    {
        Switch(Behavior::Roam, -1, "nothing left to look into", now);
        return;
    }
    const glm::vec3 source = m_interest.position;
    // The shooting has moved: where it is going round to was chosen for somewhere else.
    if (m_flankStage > 0 && Horizontal(source, m_flankSource) > 7.0f)
    {
        m_flankStage = 0;
    }
    const float fromSource = Horizontal(senses.position, source);
    m_intent.look = true;
    m_intent.lookAt = source + glm::vec3(0.0f, 1.0f, 0.0f);
    switch (m_flankStage)
    {
    case 0:
        m_flankSource = source;
        m_flankHeard = m_interest.time;
        if (PickFlankPoint(senses, source, m_flankPoint))
        {
            m_flankStage = 1;
            Log(now, Format("goes round to the shooting, %.0f m off to the side of it", Horizontal(m_flankPoint, source)));
        }
        else
        {
            m_flankStage = 3;
            Log(now, "no way round to the shooting; goes in carefully");
        }
        break;

    case 1:
        m_goal = "going round to the " + m_interest.what + ", out of sight of it";
        if (Horizontal(senses.position, m_flankPoint) < 1.0f)
        {
            m_flankStage = 2;
            m_flankUntil = now + 2.0f + 3.5f * m_traits.patience;
            m_lookBaseSet = false;
            Log(now, "stops to listen");
            break;
        }
        m_intent.move = true;
        m_intent.destination = m_flankPoint;
        m_intent.speed = fromSource > 18.0f ? m_traits.runSpeed * 0.8f : m_traits.walkSpeed * 1.4f;
        m_intent.crouch = fromSource < 18.0f ? 0.7f : 0.1f;
        break;

    case 2:
    {
        m_goal = "listening, out of sight of the " + m_interest.what;
        m_intent.crouch = 0.85f;
        // More shooting: whoever it is is still at it. It waits for it to stop -- up to a point.
        if (m_interest.time > m_flankHeard + 0.1f)
        {
            m_flankHeard = m_interest.time;
            m_flankUntil = std::min(std::max(m_flankUntil, now + 3.0f), now + 12.0f);
        }
        if (now < m_flankUntil)
        {
            break;
        }
        // The patient sort does not go in at all: it waits by the way out for whoever comes through it.
        AmbushPlan plan;
        if (m_traits.stealth > 0.55f && m_traits.patience > 0.45f && PlanDoorAmbushNear(senses, source, plan))
        {
            m_ambush = plan;
            m_ambushArrived = false;
            Switch(Behavior::Ambush, -1, "waits by the way out of where the shooting was", now);
            return;
        }
        m_flankStage = 3;
        Log(now, "goes in");
        break;
    }

    case 3:
        m_goal = "creeping in to where the " + m_interest.what + " was";
        if (fromSource < 1.4f)
        {
            m_flankStage = 4;
            m_flankUntil = now + 3.0f;
            m_lookBaseSet = false;
            break;
        }
        m_intent.move = true;
        m_intent.destination = source;
        m_intent.speed = m_traits.walkSpeed * 1.15f;
        m_intent.crouch = 0.75f;
        break;

    default:
    {
        m_goal = "looking around where the shooting was";
        m_intent.look = false;
        // The same look round as anywhere else it arrives, swung either side of the way it came in.
        if (!m_lookBaseSet)
        {
            m_lookBase = glm::length(senses.forward) > 1e-4f ? senses.forward : glm::vec3(0.0f, 0.0f, -1.0f);
            m_lookBaseSet = true;
        }
        const float angle = std::sin((now - m_flankUntil) * 1.1f) * 1.15f;
        m_intent.look = true;
        m_intent.lookAt = senses.eye + glm::vec3(m_lookBase.x * std::cos(angle) - m_lookBase.z * std::sin(angle), -0.15f,
                                                 m_lookBase.x * std::sin(angle) + m_lookBase.z * std::cos(angle)) *
                                           4.0f;
        if (now >= m_flankUntil)
        {
            m_interest.resolved = true;
            m_interest.strength = 0.0f;
            Log(now, "found nobody where the shooting was");
        }
        break;
    }
    }
}

} // namespace pred
