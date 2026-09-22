// The creature, as the game runs it: navigation, spawning, senses, noises, strikes, and the tools for
// looking inside its head.
//
// A separate file from PredationGame.cpp because that file is already the length of a short book,
// and this is one subject with a clear edge: everything here is about the creature and nothing else
// needs to read it.

#include "Game/PredationGame.h"

#include "Engine/Render/Primitives.h"
#include "Game/World/HiveMesh.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Render/DebugDraw.h"

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

CVar<int> cv_aiCreatures{"ai.creatures", 1, "How many creatures a new game starts with"};
CVar<int> cv_aiSeed{"ai.seed", 0,
                    "The seed a new game's creature is made from; 0 picks a different one each game"};
CVar<float> cv_aiArrival{"ai.arrival_seconds", 40.0f,
                         "Roughly how long into a game the creature arrives; 0 for straight away"};

CVar<float> cv_aiHealthScale{"ai.health_scale", 1.0f,
                             "Multiplies how much health a creature's body gives it, for tuning"};
CVar<bool> cv_aiFreeze{"ai.freeze", false,
                        "Creatures stand where they are and do nothing, for looking at them: ai.freeze 1"};

// A strike's reach as the game checks it when the blow lands: the body's own reach and a little more,
// because the brain decided to swing when somebody was in reach and they get this much room to have
// stepped back during the wind-up before it misses. How hard it hits is the body's too.
constexpr float kStrikeGrace = 0.3f;

float BodyHeight(PlayerStance stance)
{
    switch (stance)
    {
    case PlayerStance::Crouching:
        return 1.15f;
    case PlayerStance::Prone:
        return 0.45f;
    case PlayerStance::Standing:
    default:
        return 1.8f;
    }
}

float Horizontal(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = b - a;
    return std::sqrt(d.x * d.x + d.z * d.z);
}

} // namespace

void PredationGame::BuildNavigation()
{
    std::string error;
    if (!m_nav.Build(m_app->GetPhysics().StaticTriangles(), NavSettings{}, &error))
    {
        PRED_LOG_ERROR(AI, "No navigation mesh, so no creature can move: {}", error);
    }
    // Where a creature stands at each nest it has built: the walkable floor nearest the mound.
    for (Nest& nest : m_nests)
    {
        glm::vec3 stand = nest.at;
        if (!m_nav.NearestPoint(nest.at, 4.0f, stand))
        {
            stand = nest.at;
        }
        nest.stand = stand;
    }
}

void PredationGame::ClearCreatures()
{
    // Nobody is left held by something that is not there any more, or wrapped up at its nest.
    while (!m_grips.empty())
    {
        ReleaseGrip(m_grips.begin()->first, "gone");
    }
    for (const Cocoon& cocoon : m_cocoons)
    {
        PinPlayer(cocoon.player, kNotHeld, false, cocoon.feet, cocoon.yaw);
    }
    m_cocoons.clear();
    m_calls.clear();
    m_callsHeard.clear();
    m_doorBlows.clear();
    ClearNests();
    m_creatures.clear();
    m_arrivalsPending = 0;
    m_noises.clear();
    m_lastVoiceNoise.clear();
}

bool PredationGame::SpawnCreature(uint32_t seed, const glm::vec3& awayFrom, const glm::vec3* exactly)
{
    if (!m_nav.Valid())
    {
        return false;
    }
    glm::vec3 best{0.0f};
    float bestDistance = -1.0f;
    if (exactly != nullptr)
    {
        if (!m_nav.NearestPoint(*exactly, 4.0f, best))
        {
            return false;
        }
        bestDistance = Horizontal(best, awayFrom);
    }
    else
    {
        // Somewhere far from the players and on the mesh: several candidates, the furthest wins. A
        // creature that starts in sight of the spawn is a creature met in the first second, which is
        // the opposite of the game.
        uint32_t pick = seed * 747796405u + 2891336453u;
        for (int i = 0; i < 24; ++i)
        {
            glm::vec3 candidate;
            if (m_nav.RandomPointNear(awayFrom, 40.0f, pick, candidate))
            {
                const float distance = Horizontal(candidate, awayFrom);
                if (distance > bestDistance)
                {
                    bestDistance = distance;
                    best = candidate;
                }
            }
        }
        if (bestDistance < 0.0f)
        {
            return false;
        }
    }
    if (m_creatures.size() >= kMaxCreatures)
    {
        return false;
    }
    const CreatureTraits traits = CreatureTraits::FromSeed(seed);
    m_creatures.push_back(std::make_unique<Creature>(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                                     &m_nav, traits, best, cv_aiHealthScale.Get()));
    m_creatures.back()->SetNetId(m_nextCreatureId++);
    PRED_LOG_INFO(AI, "Creature spawned {:.0f} m away at {:.1f} {:.1f} {:.1f}: {}; {}; {}", bestDistance, best.x,
                  best.y, best.z, traits.Describe(), m_creatures.back()->Anatomy().Describe(),
                  m_creatures.back()->Capabilities().Describe());
    return true;
}

void PredationGame::SpawnCreatures()
{
    ClearCreatures();
    m_creatureClock = 0.0f;
    if (!IsAuthority())
    {
        return;
    }
    m_arrivalsPending = std::clamp(cv_aiCreatures.Get(), 0, static_cast<int>(kMaxCreatures));
    // A fixed seed when one is set, so a strange behaviour can be had again; otherwise the clock, so
    // every game is a different animal.
    m_arrivalSeed = static_cast<uint32_t>(cv_aiSeed.Get());
    if (m_arrivalSeed == 0)
    {
        m_arrivalSeed = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() & 0x7FFFFFFF);
    }
    // Not yet. A creature standing in the level when the game begins has always been there, and so
    // has nothing to arrive from; one that turns up later, from somewhere, is a thing that happened
    // to the players. When is varied a little from the seed so it cannot be timed.
    SeededRandom jitter(m_arrivalSeed ^ 0xA5A5A5A5u);
    m_arrivalAt = std::max(cv_aiArrival.Get(), 0.0f) * jitter.Range(0.75f, 1.25f);
    if (m_arrivalsPending > 0)
    {
        PRED_LOG_INFO(AI, "{} creature(s) to arrive in about {:.0f} s", m_arrivalsPending, m_arrivalAt);
    }
}

bool PredationGame::FindUnseenPoint(uint32_t seed, glm::vec3& out) const
{
    if (!m_nav.Valid())
    {
        return false;
    }
    // Everybody it has to stay out of sight of.
    std::vector<glm::vec3> eyes;
    eyes.push_back(m_player.View().eyePosition);
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : m_host.Remotes())
        {
            if (remote.alive)
            {
                eyes.push_back(remote.position + glm::vec3(0.0f, 1.6f, 0.0f));
            }
        }
    }

    const PhysicsWorld& physics = m_app->GetPhysics();
    uint32_t pick = seed * 2654435761u + 97u;
    bool found = false;
    float best = -1.0e9f;
    for (int i = 0; i < 64; ++i)
    {
        glm::vec3 candidate;
        if (!m_nav.RandomPointNear(eyes.front(), 55.0f, pick, candidate))
        {
            continue;
        }
        float nearest = 1.0e9f;
        bool seen = false;
        const glm::vec3 body = candidate + glm::vec3(0.0f, 0.7f, 0.0f);
        for (const glm::vec3& eye : eyes)
        {
            nearest = std::min(nearest, Horizontal(candidate, eye));
            const glm::vec3 along = body - eye;
            const float length = glm::length(along);
            if (length > 1e-3f && !physics.RayCast(eye, along / length, length))
            {
                seen = true;
            }
        }
        if (nearest < 20.0f)
        {
            continue;
        }
        // Out of sight above everything, then about thirty metres off: far enough to have come from
        // somewhere, near enough that it arrives into the game rather than into an empty corner.
        const float score = (seen ? 0.0f : 100.0f) - std::abs(nearest - 30.0f);
        if (score > best)
        {
            best = score;
            out = candidate;
            found = true;
        }
    }
    return found;
}

void PredationGame::UpdateArrivals()
{
    if (m_arrivalsPending <= 0 || m_creatureClock < m_arrivalAt)
    {
        return;
    }
    const uint32_t seed = m_arrivalSeed + static_cast<uint32_t>(m_creatures.size()) * 7919u;
    glm::vec3 at;
    // Where there is a nest, the next one comes out of it: that is what a nest is for.
    bool fromNest = false;
    if (const Nest* nest = NestNear(m_player.State().position, 90.0f); nest != nullptr)
    {
        const float angle = static_cast<float>(seed % 628u) * 0.01f;
        at = nest->at + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * 2.6f;
        fromNest = true;
    }
    if ((fromNest || FindUnseenPoint(seed, at)) && SpawnCreature(seed, m_player.State().position, &at))
    {
        PRED_LOG_INFO(AI, "A creature has arrived, out of everybody's sight");
    }
    // Once, whether or not somewhere was found: a level with nowhere unseen is not going to grow one.
    --m_arrivalsPending;
    m_arrivalAt = m_creatureClock + 5.0f;
}

Creature* PredationGame::CreatureForBody(BodyHandle body)
{
    if (!body.IsValid())
    {
        return nullptr;
    }
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        if (creature->Owns(body))
        {
            return creature.get();
        }
    }
    return nullptr;
}

void PredationGame::MakeNoise(NoiseKind kind, const glm::vec3& at, float reach, int player)
{
    // Only where a creature is listening. On a client this is a no-op: the host hears everything
    // that happens, including what the client did, because it is the host that resolves it.
    if (!IsAuthority() || m_creatures.empty() || m_screen != Screen::Playing)
    {
        return;
    }
    if (kind == NoiseKind::Voice && player >= 0)
    {
        float& last = m_lastVoiceNoise[player];
        if (m_creatureClock - last < 0.5f)
        {
            return;
        }
        last = m_creatureClock;
    }
    Noise noise;
    noise.kind = kind;
    noise.position = at;
    noise.reach = reach;
    noise.player = player;
    // Bounded: a flood of noises in one tick is a bug somewhere else, and should not become a stall.
    if (m_noises.size() < 256)
    {
        m_noises.push_back(noise);
    }
}

bool PredationGame::OnShotResolved(ShotResult& result, const glm::vec3& origin, int shooter)
{
    // A gunshot is the loudest thing a player can do, and is heard from the shooter's position:
    // where it hit says nothing about where it came from.
    MakeNoise(NoiseKind::Gunshot, origin, NoiseReach::kGunshot, shooter);
    if (!result)
    {
        return false;
    }
    Creature* creature = CreatureForBody(result.body);
    if (creature == nullptr)
    {
        // A round striking a wall is a sound of its own, somewhere else: enough to turn a head
        // towards the wrong place, which is a thing a player can use.
        if (result.surface)
        {
            MakeNoise(NoiseKind::Impact, result.position, NoiseReach::kImpact, -1);
        }
        return false;
    }
    if (IsAuthority())
    {
        creature->TakeDamage(result.damage, shooter, origin, m_creatureClock, result.body);
    }
    result.surface = false;
    return true;
}

float PredationGame::LightAt(const glm::vec3& feet, bool torchOn) const
{
    // Under a roof is dark and under the sky is not. Crude, and right for the level there is: the dark
    // room is dark because it has a roof, and nothing else in it pretends otherwise. A torch that is
    // on makes whoever carries it the brightest thing in any room.
    const RayHit roof = m_app->GetPhysics().RayCast(feet + glm::vec3(0.0f, 1.7f, 0.0f),
                                                    glm::vec3(0.0f, 1.0f, 0.0f), 40.0f);
    const float light = roof ? 0.18f : 1.0f;
    return torchOn ? std::max(light, 0.95f) : light;
}

void PredationGame::UpdateCreatures(float dt)
{
    // Paused alone, it waits too. The pause menu does not stop the world -- with other people in it,
    // it cannot -- but a game played alone is nobody else's, and being eaten in the settings menu is
    // not a thing that should happen to anybody.
    const bool pausedAlone = m_paused && m_sessionMode == SessionMode::Offline;
    if (!IsAuthority() || m_screen != Screen::Playing || pausedAlone)
    {
        m_noises.clear();
        return;
    }
    m_creatureClock += dt;
    UpdateArrivals();
    if (m_creatures.empty())
    {
        m_noises.clear();
        UpdateCocoons(dt);
        return;
    }

    // Everybody it could see or hear: this machine's player, and, when hosting, everybody else.
    std::vector<SensedPlayer> players;
    {
        const PlayerState& local = m_player.State();
        SensedPlayer me;
        me.id = LocalPlayerId();
        me.name = PlayerName();
        me.feet = local.position;
        me.velocity = local.velocity;
        me.height = BodyHeight(local.stance);
        me.alive = local.alive;
        me.hidden = m_hidingSpot >= 0;
        me.hidingPlace = m_hidingSpot;
        me.light = LightAt(local.position, m_torchOn);
        me.forward = m_player.View().Forward();
        players.push_back(me);
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : m_host.Remotes())
        {
            if (remote.id == LocalPlayerId())
            {
                continue;
            }
            SensedPlayer other;
            other.id = remote.id;
            other.name = remote.name;
            other.feet = remote.position;
            other.velocity = remote.velocity;
            other.height = BodyHeight(remote.stance);
            other.alive = remote.alive;
            other.light = LightAt(remote.position, remote.torchOn);
            other.forward = glm::vec3(std::sin(remote.yaw) * std::cos(remote.pitch), std::sin(remote.pitch),
                                      -std::cos(remote.yaw) * std::cos(remote.pitch));
            for (size_t i = 0; i < m_world.HidingSpots().size(); ++i)
            {
                const WorldObjects::HidingSpot& spot = m_world.HidingSpots()[i];
                // Their name on the locker is not enough: their body has to be in it. A record that
                // says somebody is hidden while they are walking about in the open is a player
                // nothing can ever see, which is worse than a player nothing should see being seen.
                if (spot.occupied && spot.occupant == remote.id &&
                    glm::distance(remote.position, spot.insidePosition) < 1.5f)
                {
                    other.hidden = true;
                    other.hidingPlace = static_cast<int>(i);
                }
            }
            players.push_back(other);
        }
    }

    // The lockers: where to stand to open one -- a step further out than where somebody climbing out
    // stands, so whoever is dragged out is not dropped on top of it -- and whether each is shut.
    std::vector<HidingPlace> places;
    for (const WorldObjects::HidingSpot& spot : m_world.HidingSpots())
    {
        HidingPlace place;
        glm::vec3 out = spot.exitPosition - spot.insidePosition;
        out.y = 0.0f;
        out = glm::length(out) > 1e-3f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
        place.front = spot.exitPosition + out * 0.9f;
        place.inside = spot.insidePosition;
        place.shut = spot.occupied;
        places.push_back(place);
    }

    // The doors, as a creature knows them: where each stands shut, whether it is, and whether it is
    // locked. Not the lockers' doors, which are inside lockers and in nobody's way.
    std::vector<DoorSense> doors;
    for (size_t d = 0; d < m_world.Doors().size(); ++d)
    {
        const bool locker = std::any_of(m_world.HidingSpots().begin(), m_world.HidingSpots().end(),
                                        [&](const WorldObjects::HidingSpot& spot) { return spot.doorIndex == static_cast<int>(d); });
        if (locker)
        {
            continue;
        }
        const WorldObjects::Door& door = m_world.Doors()[d];
        const glm::quat closed = glm::angleAxis(door.closedYaw, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 across = closed * glm::vec3(door.panelOffset.x * 2.0f, 0.0f, door.panelOffset.z * 2.0f);
        DoorSense sense;
        sense.index = static_cast<int>(d);
        sense.a = {door.hinge.x, door.hinge.y, door.hinge.z};
        sense.b = sense.a + glm::vec3(across.x, 0.0f, across.z);
        sense.shut = std::abs(door.angle - door.closedYaw) < 0.35f;
        sense.locked = door.locked;
        doors.push_back(sense);
    }

    PhysicsWorld& physics = m_app->GetPhysics();
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        // Dead is dead: no senses, no thoughts, no strikes. (Lying still on purpose is a different
        // thing, and is still alive -- its brain runs, watching.)
        if (!creature->Alive())
        {
            continue;
        }
        CreatureSenses senses;
        senses.players = players;
        senses.noises = m_noises;
        senses.doors = doors;
        // Its own nest, or one of the brood's it has come across.
        for (const Nest& nest : m_nests)
        {
            const bool mine = nest.owner == creature->NetId();
            const float away = Horizontal(nest.at, creature->Position());
            if (!mine && away > 45.0f)
            {
                continue;
            }
            if (!senses.hasHive || mine || away < Horizontal(senses.hive, creature->Position()))
            {
                senses.hasHive = true;
                senses.hive = nest.stand;
            }
            if (mine)
            {
                break;
            }
        }
        // The others calling. Not its own call, which it knows it made.
        for (const Call& call : m_callsHeard)
        {
            if (call.by != creature->NetId())
            {
                Noise noise;
                noise.kind = NoiseKind::Call;
                noise.position = call.at;
                noise.reach = NoiseReach::kCall;
                senses.noises.push_back(noise);
            }
        }
        const BodyHandle self = creature->Body();
        // Whether anything solid is between two points, stopping a little short of the far end, which
        // is usually somebody with a shape of their own.
        //
        // Creatures do not block it, its own body or any other. Its eyes are inside its own box, and
        // two big bodies of a pack standing close put one's head inside the other's box -- which, with
        // a ray that starts inside something counting as a hit, blinded it completely.
        senses.clearLine = [this, &physics, self](const glm::vec3& from, const glm::vec3& to)
        {
            const glm::vec3 along = to - from;
            const float length = glm::length(along);
            if (length < 0.4f)
            {
                return true;
            }
            const glm::vec3 direction = along / length;
            glm::vec3 start = from;
            float left = length - 0.35f;
            for (int pass = 0; pass < 4 && left > 0.0f; ++pass)
            {
                const RayHit hit = physics.RayCast(start, direction, left, self);
                if (!hit)
                {
                    return true;
                }
                if (CreatureForBody(hit.body) == nullptr)
                {
                    return false;
                }
                // Through the creature and on from the other side of it.
                const float past = hit.distance + 0.05f;
                start += direction * past;
                left -= past;
            }
            return true;
        };
        senses.lightAt = [this](const glm::vec3& at) { return LightAt(at, false); };
        senses.hidingPlaces = places;
        for (const std::unique_ptr<Creature>& other : m_creatures)
        {
            if (other.get() != creature.get())
            {
                senses.others.push_back(other->Position());
            }
        }
        if (cv_aiFreeze.Get())
        {
            continue;
        }
        creature->Update(std::move(senses), m_creatureClock, dt);

        // A locker pulled open. Whoever is inside comes out by the same path as climbing out -- so the
        // door, the sound, and everybody else's machines all follow -- and the creature, which knows
        // now, is already going for them.
        if (const int opened = creature->Brain().Intent().openHidingPlace; opened >= 0)
        {
            if (const WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(opened); spot != nullptr)
            {
                if (spot->occupied)
                {
                    const uint8_t occupant = spot->occupant;
                    PRED_LOG_INFO(AI, "Creature pulled player {} out of locker {}", occupant, opened);
                    PerformInteraction(InteractionKind::HidingSpot, opened, occupant);
                }
                else
                {
                    PlaySound(m_sounds.locker.Pick(), spot->insidePosition, 0.7f, 0.9f);
                }
            }
        }

        const CreatureIntent& intent = creature->Brain().Intent();
        // Calling the others: heard by them next tick.
        if (intent.roar)
        {
            m_calls.push_back({creature->Position(), creature->NetId()});
            PlaySound(m_sounds.death.Pick(), creature->Eye(), 1.0f, 0.55f);
        }
        if (intent.openDoor >= 0)
        {
            OpenDoorForCreature(intent.openDoor);
        }
        if (intent.bashDoor >= 0)
        {
            BashDoor(intent.bashDoor, *creature);
        }
        if (intent.grabTarget >= 0)
        {
            TryGrab(*creature, intent.grabTarget, players);
        }
        if (intent.cocoonTarget >= 0)
        {
            WrapInCocoon(static_cast<uint8_t>(intent.cocoonTarget), *creature);
        }
        if (intent.buildHive)
        {
            BuildNest(intent.hiveAt, static_cast<uint16_t>(creature->Brain().Traits().seed & 0xFFFFu), creature->NetId(), true);
        }
        // Licking its wounds at the nest.
        if (creature->Brain().Current() == Behavior::Retreat)
        {
            if (NestNear(creature->Position(), 3.5f) != nullptr)
            {
                creature->Heal(creature->MaxHealth() * 0.03f * dt);
            }
        }

        // A strike landing. Checked again here rather than trusted: somebody who stepped back
        // during the wind-up is out of reach now, and that is the whole reason there is one.
        const int target = creature->Brain().Intent().strikeTarget;
        if (target < 0)
        {
            continue;
        }
        for (const SensedPlayer& player : players)
        {
            if (player.id != target || !player.alive)
            {
                continue;
            }
            // Somebody it has hold of is in reach whatever the numbers say. A lunge carries it further; up
            // on something, it can reach as high as it can rear.
            const bool holdingThem = creature->Brain().Holding() == player.id;
            const float reach = Horizontal(creature->Position(), player.feet);
            const float rise = player.feet.y - creature->Position().y;
            const float reachLimit = creature->Capabilities().strikeReach + kStrikeGrace +
                                     (intent.strikeKind == AttackKind::Lunge ? 0.8f : 0.0f);
            if (!holdingThem && (reach > reachLimit || rise > creature->Capabilities().verticalReach || rise < -1.6f))
            {
                PRED_LOG_INFO(AI, "Strike at {} (player {}) missed: {:.1f} m away", player.name, player.id, reach);
                break;
            }
            // Through the same door as a bullet, so everything that follows from being hurt follows
            // from this too: the victim is told (their health bar, their hurt sound), and a blow that
            // kills them kills them properly -- ragdoll, dropped kit, respawn. Taking health off
            // directly did none of that, and a client struck by it never saw their health fall.
            glm::vec3 blow = player.feet - creature->Position();
            blow.y = 0.0f;
            blow = glm::length(blow) > 1e-3f ? glm::normalize(blow) : creature->Forward();
            // Claws are quicker than teeth and do less; teeth into somebody it is holding down do most.
            float damage = creature->Capabilities().strikeDamage;
            switch (intent.strikeKind)
            {
            case AttackKind::Swipe:
                damage *= 0.75f;
                break;
            case AttackKind::Bite:
                damage *= holdingThem ? 1.5f : 1.15f;
                break;
            case AttackKind::Lunge:
                damage *= 1.05f;
                break;
            default:
                break;
            }
            ApplyPlayerDamage(static_cast<uint8_t>(player.id), damage, kNoKiller, blow, "creature");
            PlaySound(m_sounds.hurt.Pick(), player.feet + glm::vec3(0.0f, 1.2f, 0.0f), 0.9f, 0.85f);
            PRED_LOG_INFO(AI, "Strike at {} (player {}) landed", player.name, player.id);
            break;
        }
    }
    m_noises.clear();
    m_callsHeard = std::move(m_calls);
    m_calls.clear();
    UpdateGrips(dt);
    UpdateCocoons(dt);
}

void PredationGame::UpdateCreatureVisuals(float dt)
{
    ShowCocoons();
    // A client's creatures have no mind here. They are eased towards what the host last said.
    const bool shownOnly = !IsAuthority();
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        if (shownOnly)
        {
            creature->FollowReceived(dt);
        }
        creature->UpdateVisual(dt);
    }
}

void PredationGame::SendCreatureState()
{
    CreatureStateMessage state;
    state.sequence = ++m_creatureSequence;
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        if (state.count >= kMaxCreatures)
        {
            break;
        }
        CreatureSnapshot& entry = state.creatures[state.count++];
        entry.id = creature->NetId();
        entry.seed = creature->Brain().Traits().seed;
        entry.position = creature->Position();
        entry.yaw = creature->Yaw();
        entry.speed = creature->Speed();
        entry.windup = creature->Brain().Intent().windup;
        entry.health = creature->Health() / creature->MaxHealth();
        entry.alive = creature->Alive();
        // Whether it is really dead is not sent separately from whether it is lying there: a client
        // draws both the same, and that is the trick.
        entry.down = creature->Down() && creature->Alive();
        entry.crouch = creature->Crouch();
        const Creature::Action& action = creature->CurrentAction();
        entry.action = static_cast<uint8_t>(action.kind);
        entry.actionPhase = action.phase;
        entry.actionSide = static_cast<int8_t>(action.side > 0 ? 1 : -1);
        entry.actionTarget = action.target;
        entry.airborne = creature->Airborne();
        entry.look = creature->Looking();
        entry.lookAt = creature->LookingAt();
    }
    m_host.SendCreatureState(state);
}

void PredationGame::ApplyCreatureState(const CreatureStateMessage& state)
{
    // Whatever the host no longer has goes. Matched on the seed as well as the number, because the
    // host numbers creatures from nought again each game: the same number with a different seed is a
    // different animal, and has to be built as one.
    std::erase_if(m_creatures,
                  [&](const std::unique_ptr<Creature>& creature)
                  {
                      for (uint8_t i = 0; i < state.count; ++i)
                      {
                          if (state.creatures[i].id == creature->NetId() &&
                              state.creatures[i].seed == creature->Brain().Traits().seed)
                          {
                              return false;
                          }
                      }
                      return true;
                  });

    for (uint8_t i = 0; i < state.count; ++i)
    {
        const CreatureSnapshot& shown = state.creatures[i];
        Creature* creature = nullptr;
        for (const std::unique_ptr<Creature>& existing : m_creatures)
        {
            if (existing->NetId() == shown.id)
            {
                creature = existing.get();
                break;
            }
        }
        if (creature == nullptr)
        {
            // Built from the same seed, so it is the same animal the host has -- which matters more
            // once its body is generated from that seed rather than from boxes.
            m_creatures.push_back(std::make_unique<Creature>(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                                             &m_nav, CreatureTraits::FromSeed(shown.seed),
                                                             shown.position, cv_aiHealthScale.Get()));
            creature = m_creatures.back().get();
            creature->SetNetId(shown.id);
            PRED_LOG_INFO(AI, "Shown the host's creature {} (seed {})", shown.id, shown.seed);
        }
        creature->Receive(shown.position, shown.yaw, shown.speed, shown.windup, shown.health, shown.alive,
                          shown.down, shown.crouch);
        Creature::Action action;
        action.kind = shown.action < 8 ? static_cast<RigAction>(shown.action) : RigAction::None;
        action.phase = shown.actionPhase;
        action.side = shown.actionSide;
        action.target = shown.actionTarget;
        creature->SetShownAction(action, shown.airborne, shown.look, shown.lookAt);
    }
}

void PredationGame::DrawCreatureOverlays(DebugDraw& draw)
{
    if (DebugCategories::IsEnabled(DebugCategory::Navigation))
    {
        m_nav.Draw(draw);
    }
    const bool mind = DebugCategories::IsEnabled(DebugCategory::AI) || m_showBrain;
    const bool senses = DebugCategories::IsEnabled(DebugCategory::Perception) || m_showBrain;
    if (!mind && !senses)
    {
        return;
    }
    // A marker is left out when the camera is inside or right beside it. Most of them are about the
    // player -- where it heard them, where it thinks they are -- so in first person they sit on your
    // own head, and a wire sphere drawn round the camera is lines across the whole screen that say
    // nothing.
    // Where the picture is taken from, worked out the way the renderer does it.
    const glm::vec3 viewer =
        m_cameraMode == CameraMode::Fly ? m_camera.position : m_player.View().eyePosition;
    const auto marker = [&](const glm::vec3& centre, float radius, uint32_t colour, int segments)
    {
        if (glm::distance(centre, viewer) > radius + 1.5f)
        {
            draw.Sphere(centre, radius, colour, segments);
        }
    };

    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        const CreatureBrain& brain = creature->Brain();
        const glm::vec3 eye = creature->Eye();
        if (senses)
        {
            // The field of view: its two edges and an arc at the range it sees to, level with its eye.
            const float range = std::max(brain.SightRange(), CreatureBrain::CloseSense());
            const float half = glm::radians(65.0f);
            const float yaw = creature->Yaw();
            const auto along = [&](float angle)
            { return glm::vec3(std::sin(yaw + angle), 0.0f, -std::cos(yaw + angle)); };
            const uint32_t cone = Color::RGBA(240, 200, 90, 150);
            draw.Line(eye, eye + along(-half) * range, cone);
            draw.Line(eye, eye + along(half) * range, cone);
            glm::vec3 previous = eye + along(-half) * range;
            for (int i = 1; i <= 16; ++i)
            {
                const glm::vec3 next = eye + along(-half + 2.0f * half * static_cast<float>(i) / 16.0f) * range;
                draw.Line(previous, next, cone);
                previous = next;
            }

            // Each player it knows of: a line coloured by how much of a sighting it has, and a marker
            // where it thinks they are.
            for (const CreatureBrain::Track& track : brain.Tracks())
            {
                const float e = track.exposure;
                const uint32_t colour = track.visible ? Color::kRed
                                                      : Color::RGBA(static_cast<uint8_t>(90 + 150 * e),
                                                                    static_cast<uint8_t>(200 - 120 * e), 90, 200);
                const glm::vec3 end = track.lastKnown + glm::vec3(0.0f, 1.1f, 0.0f);
                // Not when it ends on the viewer: a line into the camera is a streak across the view.
                if ((e > 0.01f || track.visible) && glm::distance(end, viewer) > 2.0f)
                {
                    draw.Line(eye, end, colour);
                }
                // Only once they are out of sight, which is when "where it thinks they are" and
                // "where they are" can differ. While it can see them the marker is on their feet --
                // and drawn at your own feet it slices through the whole first-person view.
                if (!track.visible && track.confidence > 0.05f)
                {
                    marker(track.lastKnown + glm::vec3(0.0f, 0.1f, 0.0f), 0.15f + 0.2f * track.confidence,
                                Color::kMagenta, 10);
                }
            }

            // What it has heard, as a ring where the sound was, fading.
            for (const CreatureBrain::Heard& heard : brain.RecentNoises())
            {
                const float age = std::clamp((m_creatureClock - heard.time) / 12.0f, 0.0f, 1.0f);
                const auto alpha = static_cast<uint8_t>(220.0f * (1.0f - age));
                marker(heard.noise.position, 0.25f + heard.strength * 0.6f, Color::RGBA(90, 180, 255, alpha), 12);
            }
        }
        if (mind)
        {
            // Where it is going, and why: the route, and what it is interested in.
            const std::vector<glm::vec3>& route = brain.Route();
            glm::vec3 from = creature->Position() + glm::vec3(0.0f, 0.1f, 0.0f);
            for (const glm::vec3& corner : route)
            {
                const glm::vec3 to = corner + glm::vec3(0.0f, 0.1f, 0.0f);
                draw.Line(from, to, Color::kGreen);
                from = to;
            }
            if (!brain.Interest().resolved)
            {
                marker(brain.Interest().position + glm::vec3(0.0f, 0.3f, 0.0f), 0.3f, Color::kCyan, 10);
            }

            // Its search, as a chain from where it is through the places still to look, lockers marked.
            if (brain.Current() == Behavior::Search)
            {
                glm::vec3 chain = creature->Position() + glm::vec3(0.0f, 0.3f, 0.0f);
                for (size_t i = brain.SearchStep(); i < brain.SearchPlan().size(); ++i)
                {
                    const CreatureBrain::SearchStop& stop = brain.SearchPlan()[i];
                    const glm::vec3 to = stop.point + glm::vec3(0.0f, 0.3f, 0.0f);
                    draw.Line(chain, to, Color::kYellow);
                    marker(to, stop.place >= 0 ? 0.35f : 0.2f, stop.place >= 0 ? Color::kRed : Color::kYellow, 8);
                    chain = to;
                }
            }

            // Where it expects people: a square on the ground for each warm cell, redder for warmer.
            for (const CreatureBrain::HeatCell& cell : brain.Heat())
            {
                const float x0 = static_cast<float>(cell.x) * CreatureBrain::kHeatCell;
                const float z0 = static_cast<float>(cell.z) * CreatureBrain::kHeatCell;
                const float x1 = x0 + CreatureBrain::kHeatCell;
                const float z1 = z0 + CreatureBrain::kHeatCell;
                const float y = creature->Position().y + 0.05f;
                const float warm = std::clamp(cell.heat / 5.0f, 0.1f, 1.0f);
                const uint32_t colour =
                    Color::RGBA(255, static_cast<uint8_t>(200 - 160 * warm), 60, static_cast<uint8_t>(60 + 150 * warm));
                draw.Line({x0, y, z0}, {x1, y, z0}, colour);
                draw.Line({x1, y, z0}, {x1, y, z1}, colour);
                draw.Line({x1, y, z1}, {x0, y, z1}, colour);
                draw.Line({x0, y, z1}, {x0, y, z0}, colour);
            }

            // The cover it last weighed while stalking: a post at each candidate, as tall as it scored,
            // purple where a player could see it and orange where none could, and the one it chose
            // ringed. Why it waits where it waits, at a glance.
            if (brain.Current() == Behavior::Stalk)
            {
                for (const CreatureBrain::CoverCandidate& candidate : brain.Cover())
                {
                    const uint32_t colour =
                        candidate.hidden ? Color::RGBA(240, 150, 60, 220) : Color::RGBA(150, 90, 200, 160);
                    draw.Line(candidate.position, candidate.position + glm::vec3(0.0f, 0.2f + 2.0f * candidate.score, 0.0f),
                              colour);
                }
                if (brain.HasCoverPoint())
                {
                    marker(brain.CoverPoint() + glm::vec3(0.0f, 0.2f, 0.0f), 0.45f, Color::kYellow, 12);
                }
            }
        }
    }
}

void PredationGame::DrawBrainInspector()
{
#if PRED_DEV_TOOLS
    // Open when asked for with ai_brain, or with the AI category on while the overlay is up.
    const bool byCategory = DebugCategories::IsEnabled(DebugCategory::AI) && m_app->IsOverlayVisible();
    if (!m_showBrain && !byCategory)
    {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 470.0f, 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 640.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    const bool shown = ImGui::Begin("Creature brain", &open);
    if (!open)
    {
        m_showBrain = false;
        DebugCategories::SetEnabled(DebugCategory::AI, false);
    }
    if (!shown)
    {
        ImGui::End();
        return;
    }
    if (!IsAuthority())
    {
        // The copies here have a brain object, but it never runs: showing it would be showing a mind
        // that is not thinking. What this machine does know is what it is being shown.
        ImGui::TextWrapped("The host runs the creatures; their minds are on the host's machine. This one "
                           "is only shown where they are.");
        for (const std::unique_ptr<Creature>& creature : m_creatures)
        {
            ImGui::Text("#%u  seed %u  %s  %.1f m/s", creature->NetId(), creature->Brain().Traits().seed,
                        creature->Alive() ? "alive" : "dead", creature->Speed());
            ImGui::ProgressBar(creature->Health() / creature->MaxHealth(), ImVec2(-1.0f, 0.0f), "health");
        }
        ImGui::End();
        return;
    }
    if (m_creatures.empty())
    {
        if (m_arrivalsPending > 0)
        {
            ImGui::Text("Arrives in %.0f s, somewhere nobody is looking.",
                        std::max(m_arrivalAt - m_creatureClock, 0.0f));
        }
        ImGui::TextDisabled("spawn_creature [seed] [ahead] makes one now.");
        ImGui::End();
        return;
    }
    m_inspectedCreature = std::clamp(m_inspectedCreature, 0, static_cast<int>(m_creatures.size()) - 1);
    if (m_creatures.size() > 1)
    {
        ImGui::SliderInt("Creature", &m_inspectedCreature, 0, static_cast<int>(m_creatures.size()) - 1);
    }
    const Creature& creature = *m_creatures[static_cast<size_t>(m_inspectedCreature)];
    const CreatureBrain& brain = creature.Brain();

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", brain.Traits().Describe().c_str());
    ImGui::PopTextWrapPos();
    ImGui::ProgressBar(creature.Health() / creature.MaxHealth(), ImVec2(-1.0f, 0.0f),
                       creature.Alive() ? "health" : "dead");
    if (brain.Dead())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "Dead. Its mind has stopped; nothing below changes.");
    }
    else if (creature.Down())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1.0f), "Playing dead. Alive, watching, and waiting.");
    }

    ImGui::SeparatorText("Now");
    ImGui::Text("%s", BehaviorName(brain.Current()));
    ImGui::SameLine();
    ImGui::TextDisabled("-- %s", brain.CurrentGoal().c_str());
    const CreatureBrain::State& feelings = brain.Feelings();
    ImGui::ProgressBar(std::min(feelings.pain, 1.0f), ImVec2(140.0f, 0.0f), "pain");
    ImGui::SameLine();
    ImGui::ProgressBar(feelings.fear, ImVec2(140.0f, 0.0f), "fear");
    ImGui::SameLine();
    ImGui::ProgressBar(feelings.arousal, ImVec2(140.0f, 0.0f), "arousal");

    ImGui::SeparatorText("What it weighed");
    // Best first, with every consideration behind each score, because the number alone never says
    // why.
    for (const CreatureBrain::Option& option : brain.Options())
    {
        const bool chosen = option.behavior == brain.Current() && option.target == brain.CurrentTarget();
        char header[160];
        std::snprintf(header, sizeof(header), "%-28s %.2f###%s", option.label.c_str(), option.score,
                      option.label.c_str());
        if (chosen)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.95f, 0.6f, 1.0f));
        }
        const bool expanded = ImGui::TreeNode(header);
        if (chosen)
        {
            ImGui::PopStyleColor();
        }
        if (expanded)
        {
            for (const CreatureBrain::Consideration& c : option.considerations)
            {
                ImGui::Text("%-16s %.2f", c.name, c.value);
            }
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Who it knows about");
    for (const CreatureBrain::Track& track : brain.Tracks())
    {
        ImGui::Text("%s%s", track.name.c_str(), track.visible ? "  (in sight)" : "");
        ImGui::ProgressBar(track.exposure, ImVec2(150.0f, 0.0f), "seen");
        ImGui::SameLine();
        ImGui::ProgressBar(track.confidence, ImVec2(150.0f, 0.0f), "sure where");
        if (track.harm > 0.0f)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "hurt it %.0f", track.harm * 100.0f);
        }
        // What stalking weighs: whether they are looking at it, whether they have company, and how
        // much of its patience they have used up.
        ImGui::TextDisabled("%s  %s  stalked %.0f of %.0f s", track.watching ? "WATCHING IT" : "not looking",
                            track.isolation > 10.0f ? "alone" : "with company", track.stalked,
                            brain.Traits().StalkPatienceSeconds());
    }
    if (!brain.Interest().resolved)
    {
        ImGui::Text("Interested in %s, %.0f%%", brain.Interest().what.c_str(), brain.Interest().strength * 100.0f);
    }

    // What it believes about the places people hide, and what it has learnt about them this match.
    if (!brain.Places().empty())
    {
        ImGui::SeparatorText("Lockers");
        ImGui::TextDisabled("found %d hiding; a shut door means somebody %.0f%%", brain.FoundHiding(),
                            brain.ShutMeansSomebody() * 100.0f);
        for (size_t i = 0; i < brain.Places().size(); ++i)
        {
            const CreatureBrain::PlaceMemory& place = brain.Places()[i];
            char label[64];
            std::snprintf(label, sizeof(label), "#%zu%s%s", i, place.seenShut ? " shut" : "",
                          place.suspect >= 0 ? " (has somebody in mind)" : "");
            ImGui::ProgressBar(place.suspicion, ImVec2(-1.0f, 0.0f), label);
        }
    }
    if (brain.Current() == Behavior::Search)
    {
        ImGui::Text("Searching: place %zu of %zu", brain.SearchStep() + 1, brain.SearchPlan().size());
    }
    if (!brain.Heat().empty())
    {
        ImGui::TextDisabled("Remembers %zu places where people have been", brain.Heat().size());
    }

    ImGui::SeparatorText("Timeline");
    // Newest first, with how long ago.
    ImGui::BeginChild("##timeline", ImVec2(0.0f, 180.0f), true);
    const auto& timeline = brain.Timeline();
    for (auto it = timeline.rbegin(); it != timeline.rend(); ++it)
    {
        ImGui::TextDisabled("%5.1fs ago", m_creatureClock - it->time);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(it->what.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Overlays: the AI and Perception debug categories, or this window open.");
    ImGui::End();
#endif
}

void PredationGame::RegisterCreatureCommands()
{
#if PRED_DEV_TOOLS
    Console& console = m_app->GetConsole();
    console.RegisterCommand(
        "spawn_creature", "Make a creature, far from you, or in front of you with 'ahead' (7 m, or the distance given)",
        [this](const std::vector<std::string>& args)
        {
            if (!IsAuthority())
            {
                m_app->GetConsole().PrintError("Only the host can make a creature.");
                return;
            }
            const uint32_t seed = args.size() >= 2
                                      ? static_cast<uint32_t>(std::strtoul(args[1].c_str(), nullptr, 10))
                                      : static_cast<uint32_t>(std::rand());
            const bool ahead = args.size() >= 3 && args[2] == "ahead";
            const PlayerView& view = m_player.View();
            const glm::vec3 flat = glm::normalize(glm::vec3(view.Forward().x, 0.0f, view.Forward().z) +
                                                  glm::vec3(0.0f, 0.0f, 1e-4f));
            const float howFar = args.size() >= 4 ? std::clamp(std::strtof(args[3].c_str(), nullptr), 1.5f, 40.0f) : 7.0f;
            const glm::vec3 inFront = m_player.State().position + flat * howFar;
            if (SpawnCreature(seed, m_player.State().position, ahead ? &inFront : nullptr))
            {
                // The one just made is the one somebody wants to watch.
                m_inspectedCreature = static_cast<int>(m_creatures.size()) - 1;
                if (ahead)
                {
                    // Facing you: one put in front of you to be looked at is there to look back.
                    Creature& made = *m_creatures.back();
                    const glm::vec3 toYou = m_player.State().position - made.Position();
                    // Or turned by however many degrees are asked for, to see it from the side.
                    const float turn = args.size() >= 5 ? glm::radians(std::strtof(args[4].c_str(), nullptr)) : 0.0f;
                    made.SetShownState(made.Position(), std::atan2(toYou.x, -toYou.z) + turn, 0.0f, 0.0f, true);
                }
                m_app->GetConsole().Print("Creature " + std::to_string(seed) + ": " +
                                          m_creatures.back()->Brain().Traits().Describe());
            }
            else
            {
                m_app->GetConsole().PrintError("Could not place one: no walkable ground there, or there are already " +
                                               std::to_string(kMaxCreatures) + ".");
            }
        },
        "spawn_creature [seed] [ahead [metres [turn degrees]]]");
    console.RegisterCommand(
        "hide", "Get into a locker, or out of the one you are in, as pressing interact at it does: hide [index]",
        [this](const std::vector<std::string>& args)
        {
            const int index = m_hidingSpot >= 0 ? m_hidingSpot
                                                : (args.size() >= 2 ? std::atoi(args[1].c_str()) : 0);
            if (m_world.GetHidingSpot(index) == nullptr)
            {
                m_app->GetConsole().PrintError("No locker " + std::to_string(index));
                return;
            }
            // The real interaction, so everything that follows from using a locker follows: the door,
            // the sound, the noise the creature hears, and everybody else being told. A client asks the
            // host, exactly as pressing the key does.
            if (m_sessionMode == SessionMode::Client)
            {
                m_client.SendInteract(static_cast<uint8_t>(InteractionKind::HidingSpot), static_cast<uint8_t>(index));
                return;
            }
            if (!PerformInteraction(InteractionKind::HidingSpot, index, LocalPlayerId()))
            {
                m_app->GetConsole().PrintError("Somebody else is in that one.");
            }
        },
        "hide [index]");
    console.RegisterCommand(
        "creature_mind", "Print the inspected creature's timeline and what it is weighing, to the console and the log",
        [this](const std::vector<std::string>&)
        {
            if (m_creatures.empty())
            {
                return;
            }
            const int which = std::clamp(m_inspectedCreature, 0, static_cast<int>(m_creatures.size()) - 1);
            const CreatureBrain& brain = m_creatures[static_cast<size_t>(which)]->Brain();
            const auto say = [this](const std::string& line)
            {
                m_app->GetConsole().Print(line);
                PRED_LOG_INFO(AI, "{}", line);
            };
            say(std::string("mind: ") + BehaviorName(brain.Current()) + " -- " + brain.CurrentGoal());
            for (const CreatureBrain::TimelineEntry& entry : brain.Timeline())
            {
                char line[256];
                std::snprintf(line, sizeof(line), "  %7.2f  %s", entry.time, entry.what.c_str());
                say(line);
            }
            for (const CreatureBrain::Option& option : brain.Options())
            {
                char line[160];
                std::snprintf(line, sizeof(line), "  option %-28s %.2f", option.label.c_str(), option.score);
                say(line);
            }
        });
    console.RegisterCommand("creature_pose", "Where each creature is, how it is lying, and where it is drawn",
                            [this](const std::vector<std::string>&)
                            {
                                for (const std::unique_ptr<Creature>& creature : m_creatures)
                                {
                                    const std::string line = "creature " + std::to_string(creature->NetId()) +
                                                             ": " + creature->DescribePose();
                                    m_app->GetConsole().Print(line);
                                    PRED_LOG_INFO(AI, "{}", line);
                                }
                            });
    console.RegisterCommand("creature_clear", "Remove every creature",
                            [this](const std::vector<std::string>&) { ClearCreatures(); });
    console.RegisterCommand("nest_here", "Build a nest where you stand, as a creature would, to look at one",
                            [this](const std::vector<std::string>&)
                            {
                                if (!IsAuthority())
                                {
                                    m_app->GetConsole().PrintError("Only the host has nests.");
                                    return;
                                }
                                const uint16_t seed = static_cast<uint16_t>(std::rand() & 0xFFFF);
                                BuildNest(m_player.State().position, seed, 0, true);
                            });
    console.RegisterCommand("ai_brain", "Show or hide the creature brain inspector",
                            [this](const std::vector<std::string>&) { m_showBrain = !m_showBrain; });
    console.RegisterCommand(
        "creature_hurt", "Hurt the first creature as though you had shot it: creature_hurt [amount]",
        [this](const std::vector<std::string>& args)
        {
            if (m_creatures.empty())
            {
                return;
            }
            const float amount = args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : 30.0f;
            m_creatures.front()->TakeDamage(amount, LocalPlayerId(),
                                            m_player.View().eyePosition, m_creatureClock);
        },
        "creature_hurt [amount]");
#endif
}



// --- Nests --------------------------------------------------------------------------------------
//
// A nest is not part of a map. It is there because a creature that builds them found somewhere dark and
// out of the way and spent a while working at it. Everybody gets told when one appears, so it is solid
// and visible on every machine; only the host rebuilds the walkable surface round it, because only the
// host has anything that walks by it.

const PredationGame::Nest* PredationGame::NestNear(const glm::vec3& point, float reach) const
{
    const Nest* best = nullptr;
    float nearest = reach;
    for (const Nest& nest : m_nests)
    {
        const float away = Horizontal(nest.at, point);
        if (away <= nearest)
        {
            nearest = away;
            best = &nest;
        }
    }
    return best;
}

void PredationGame::BuildNest(const glm::vec3& at, uint16_t seed, uint8_t owner, bool announce)
{
    if (m_nests.size() >= 8 || NestNear(at, 6.0f) != nullptr)
    {
        return;
    }
    Nest nest;
    nest.at = at;
    nest.stand = at;
    nest.owner = owner;
    nest.seed = seed;
    constexpr float kRadius = 1.7f;
    const MeshData mound = BuildHiveMesh(seed, kRadius, 4.5f);
    if (!mound.vertices.empty())
    {
        nest.mesh = m_app->GetMeshes().Upload(mound, "nest_" + std::to_string(seed) + "_" + std::to_string(m_nests.size()));
        Transform where;
        where.position = at;
        nest.entity = m_scene.CreateMeshEntity("nest", where, nest.mesh, Material::Diffuse(glm::vec3(1.0f), 0.5f));
    }
    // Solid: people walk round it, not through it.
    Transform solid;
    solid.position = at + glm::vec3(0.0f, 0.9f, 0.0f);
    nest.body = m_app->GetPhysics().CreateBox({kRadius * 0.75f, 0.9f, kRadius * 0.75f}, solid, BodyMotion::Static);
    m_nests.push_back(nest);

    PlaySound(m_sounds.locker.Pick(), at + glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.5f);
    PRED_LOG_INFO(AI, "Creature {} built a nest at {:.1f} {:.1f} {:.1f}", owner, at.x, at.y, at.z);

    if (announce)
    {
        // The floor beside it, for anything that wants to stand there, and a mesh that goes round it.
        RequestNavRebuild();
        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::NestBuilt;
            event.index = static_cast<uint8_t>(m_nests.size() - 1);
            event.item = seed;
            event.position = at;
            m_host.Broadcast(event);
        }
    }
}

void PredationGame::ClearNests()
{
    for (Nest& nest : m_nests)
    {
        if (nest.entity.IsValid())
        {
            m_scene.Destroy(nest.entity);
        }
        if (nest.body.IsValid())
        {
            m_app->GetPhysics().DestroyBody(nest.body);
        }
        m_app->GetMeshes().Release(nest.mesh);
    }
    const bool had = !m_nests.empty();
    m_nests.clear();
    if (had)
    {
        RequestNavRebuild();
    }
}

void PredationGame::RequestNavRebuild()
{
    if (m_navRebuilding)
    {
        return; // one at a time; the next one picks up whatever the level looks like then
    }
    // The level's shape as it is now, taken here rather than on the worker: the physics world is the
    // main thread's.
    std::vector<glm::vec3> triangles = m_app->GetPhysics().StaticTriangles();
    m_navSpare = std::make_unique<NavMesh>();
    m_navRebuilding = true;
    NavMesh* spare = m_navSpare.get();
    m_navRebuild = std::async(std::launch::async,
                              [spare, triangles = std::move(triangles)]()
                              {
                                  std::string error;
                                  if (!spare->Build(triangles, NavSettings{}, &error))
                                  {
                                      PRED_LOG_ERROR(AI, "Could not rebuild navigation: {}", error);
                                  }
                              });
}

void PredationGame::FinishNavRebuild()
{
    if (!m_navRebuilding || !m_navRebuild.valid() ||
        m_navRebuild.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return;
    }
    m_navRebuild.get();
    m_navRebuilding = false;
    if (m_navSpare && m_navSpare->Valid())
    {
        // Put in place between ticks. Everything holds a pointer to m_nav itself, which does not move.
        m_nav.Swap(*m_navSpare);
        for (const std::unique_ptr<Creature>& creature : m_creatures)
        {
            creature->SetNav(&m_nav);
        }
        // Where a creature stands at each nest: the floor beside the mound.
        for (Nest& nest : m_nests)
        {
            glm::vec3 stand = nest.at;
            if (!m_nav.NearestPoint(nest.at, 4.0f, stand))
            {
                stand = nest.at;
            }
            nest.stand = stand;
        }
        PRED_LOG_INFO(AI, "Navigation rebuilt round {} nest(s)", m_nests.size());
    }
    m_navSpare.reset();
}

// --- Held, dragged and wrapped up --------------------------------------------------------------------

Creature* PredationGame::CreatureById(uint8_t id)
{
    for (const std::unique_ptr<Creature>& creature : m_creatures)
    {
        if (creature->NetId() == id)
        {
            return creature.get();
        }
    }
    return nullptr;
}

glm::vec3 PredationGame::GripPoint(const Creature& creature, float& yaw) const
{
    // Dragged along the floor in front of it, by the jaws or the hands, facing it: the last thing a
    // person being taken sees is what is taking them.
    const CreatureAnatomy& a = creature.Anatomy();
    const float ahead = a.length * 0.5f + a.neckLength + a.headLength * 0.7f + 0.2f;
    glm::vec3 feet = creature.Position() + creature.Forward() * ahead;
    glm::vec3 onMesh;
    if (m_nav.NearestPoint(feet, 1.0f, onMesh) && std::abs(onMesh.y - feet.y) < 0.6f)
    {
        feet.y = onMesh.y;
    }
    yaw = creature.Yaw() + glm::pi<float>();
    return feet;
}

void PredationGame::PinPlayer(uint8_t player, uint8_t by, bool cocooned, const glm::vec3& feet, float yaw)
{
    const bool release = by == kNotHeld;
    if (player == LocalPlayerId())
    {
        if (release)
        {
            if (m_player.IsAttached() && m_hidingSpot < 0)
            {
                m_player.Detach(feet);
            }
        }
        else
        {
            m_player.Attach(feet, yaw);
        }
    }
    if (m_sessionMode == SessionMode::Host)
    {
        m_host.SetPlayerGrabbed(player, by, cocooned, feet, yaw);
    }
}

bool PredationGame::HeldLocally() const
{
    if (m_sessionMode == SessionMode::Client)
    {
        return m_client.HeldByHost();
    }
    if (m_grips.count(LocalPlayerId()) != 0)
    {
        return true;
    }
    return std::any_of(m_cocoons.begin(), m_cocoons.end(),
                       [this](const Cocoon& cocoon) { return cocoon.player == LocalPlayerId(); });
}

void PredationGame::TryGrab(Creature& creature, int target, const std::vector<SensedPlayer>& players)
{
    for (const SensedPlayer& player : players)
    {
        if (player.id != target || !player.alive || player.hidden)
        {
            continue;
        }
        const uint8_t id = static_cast<uint8_t>(player.id);
        const float reach = Horizontal(creature.Position(), player.feet);
        const float rise = player.feet.y - creature.Position().y;
        // One at a time, and not somebody who has just got free: being taken again the instant you break
        // loose is not a fight, it is a cutscene.
        const bool taken = m_grips.count(id) != 0 ||
                           std::any_of(m_cocoons.begin(), m_cocoons.end(), [&](const Cocoon& c) { return c.player == id; });
        const bool busy = std::any_of(m_grips.begin(), m_grips.end(),
                                      [&](const auto& entry) { return entry.second.creature == creature.NetId(); });
        const auto safe = m_grabImmunity.find(id);
        const bool justFree = safe != m_grabImmunity.end() && m_creatureClock < safe->second;
        if (taken || busy || justFree || reach > creature.Capabilities().strikeReach + kStrikeGrace ||
            std::abs(rise) > 1.0f)
        {
            PRED_LOG_INFO(AI, "Grab at {} (player {}) missed: {:.1f} m away", player.name, player.id, reach);
            return;
        }
        m_grips[id] = Grip{creature.NetId(), 0.0f, false};
        creature.Brain().OnGrabbed(player.id, m_creatureClock);
        PlaySound(m_sounds.hurt.Pick(), player.feet + glm::vec3(0.0f, 1.2f, 0.0f), 0.9f, 0.7f);
        PRED_LOG_INFO(AI, "Creature {} has hold of {} (player {})", creature.NetId(), player.name, player.id);
        if (id == LocalPlayerId())
        {
            m_app->GetConsole().Print("Something has you. Struggle -- jump, over and over -- or hope somebody shoots it.");
        }
        return;
    }
}

void PredationGame::ReleaseGrip(uint8_t player, const char* why)
{
    const auto found = m_grips.find(player);
    if (found == m_grips.end())
    {
        return;
    }
    Creature* creature = CreatureById(found->second.creature);
    glm::vec3 feet = m_player.State().position;
    float yaw = 0.0f;
    if (creature != nullptr)
    {
        feet = GripPoint(*creature, yaw);
        creature->Brain().OnReleased(m_creatureClock, why);
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.id == player)
        {
            feet = remote.position;
        }
    }
    if (player == LocalPlayerId())
    {
        feet = m_player.State().position;
    }
    m_grips.erase(found);
    // A few seconds where nothing can take them again: long enough to run, or to be pulled up.
    m_grabImmunity[player] = m_creatureClock + 7.0f;
    PinPlayer(player, kNotHeld, false, feet, yaw);
    PRED_LOG_INFO(AI, "Player {} let go: {}", player, why);
}

void PredationGame::UpdateGrips(float dt)
{
    (void)dt;
    std::vector<uint8_t> letGo;
    std::vector<const char*> reasons;
    for (auto& [player, grip] : m_grips)
    {
        Creature* creature = CreatureById(grip.creature);
        bool alive = true;
        if (player == LocalPlayerId())
        {
            alive = m_player.State().alive;
        }
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (remote.id == player)
            {
                alive = remote.alive;
            }
        }
        if (creature == nullptr || !creature->Alive() || creature->Down() || creature->Brain().Holding() != player || !alive)
        {
            letGo.push_back(player);
            reasons.push_back(!alive ? "they died" : "it let go");
            continue;
        }
        // Struggling: every fresh press of jump works a hand free a little. Enough of them, quickly, and
        // they are out of its grip -- which is worth knowing, and worth being told.
        bool jump = false;
        if (player == LocalPlayerId())
        {
            jump = m_lastInput.jump;
        }
        else if (m_sessionMode == SessionMode::Host)
        {
            jump = m_host.LastInputOf(player).jump;
        }
        if (jump && !grip.jumpWasDown)
        {
            grip.struggle += 0.09f;
        }
        grip.jumpWasDown = jump;
        grip.struggle = std::max(grip.struggle - 0.05f * dt, 0.0f);
        if (grip.struggle >= 1.0f)
        {
            letGo.push_back(player);
            reasons.push_back("struggled free");
            continue;
        }
        float yaw = 0.0f;
        const glm::vec3 feet = GripPoint(*creature, yaw);
        PinPlayer(player, grip.creature, false, feet, yaw);
    }
    for (size_t i = 0; i < letGo.size(); ++i)
    {
        ReleaseGrip(letGo[i], reasons[i]);
    }
}

void PredationGame::WrapInCocoon(uint8_t player, const Creature& creature)
{
    m_grips.erase(player);
    // Against the side of the nest, one place round it for each person already there.
    glm::vec3 nest = creature.Position();
    if (const Nest* nearest = NestNear(creature.Position(), 12.0f); nearest != nullptr)
    {
        nest = nearest->at;
    }
    const float angle = static_cast<float>(m_cocoons.size()) * 1.3f + 0.4f;
    glm::vec3 feet = nest + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * 3.0f;
    glm::vec3 onMesh;
    if (m_nav.NearestPoint(feet, 2.0f, onMesh))
    {
        feet = onMesh;
    }
    const float yaw = std::atan2(feet.x - nest.x, -(feet.z - nest.z));
    m_cocoons.push_back({player, feet, yaw, 0.0f});
    PinPlayer(player, creature.NetId(), true, feet, yaw);
    PRED_LOG_INFO(AI, "Player {} wrapped up at the nest", player);
    if (player == LocalPlayerId())
    {
        m_app->GetConsole().Print("You are wrapped up at its nest. Somebody can cut you free -- before it kills you.");
    }
}

void PredationGame::UpdateCocoons(float dt)
{
    // Slowly: long enough for somebody to come, and not so long that there is no hurry.
    // Slowly. Long enough for the others to fight their way to the nest and cut somebody out, which is
    // the whole point of leaving them alive in there.
    constexpr float kBleed = 1.1f; // health a second: about a minute and a half from full
    for (size_t i = 0; i < m_cocoons.size();)
    {
        Cocoon& cocoon = m_cocoons[i];
        bool alive = true;
        if (cocoon.player == LocalPlayerId())
        {
            alive = m_player.State().alive;
        }
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (remote.id == cocoon.player)
            {
                alive = remote.alive;
            }
        }
        if (!alive)
        {
            PinPlayer(cocoon.player, kNotHeld, false, cocoon.feet, cocoon.yaw);
            m_cocoons.erase(m_cocoons.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        PinPlayer(cocoon.player, 0, true, cocoon.feet, cocoon.yaw);
        cocoon.owed += kBleed * dt;
        if (cocoon.owed >= 1.0f)
        {
            const float amount = std::floor(cocoon.owed);
            cocoon.owed -= amount;
            ApplyPlayerDamage(cocoon.player, amount, kNoKiller, glm::vec3(0.0f, -1.0f, 0.0f), "cocoon");
        }
        ++i;
    }
}

void PredationGame::FreeFromCocoon(uint8_t player, uint8_t helper)
{
    const auto found = std::find_if(m_cocoons.begin(), m_cocoons.end(), [&](const Cocoon& c) { return c.player == player; });
    if (found == m_cocoons.end())
    {
        return;
    }
    const Cocoon cocoon = *found;
    m_cocoons.erase(found);
    PinPlayer(player, kNotHeld, false, cocoon.feet + glm::vec3(0.0f, 0.1f, 0.0f), cocoon.yaw);
    PlaySound(m_sounds.locker.Pick(), cocoon.feet + glm::vec3(0.0f, 1.0f, 0.0f), 0.8f, 0.6f);
    MakeNoise(NoiseKind::Door, cocoon.feet, NoiseReach::kDoor, helper);
    PRED_LOG_INFO(AI, "Player {} cut free by player {}", player, helper);
}

void PredationGame::ShowCocoons()
{
    // Who is wrapped up, as this machine knows it: the host from its own list, anybody else from what
    // the host says about everybody.
    std::map<uint8_t, std::pair<glm::vec3, float>> wrapped;
    if (IsAuthority())
    {
        for (const Cocoon& cocoon : m_cocoons)
        {
            wrapped[cocoon.player] = {cocoon.feet, cocoon.yaw};
        }
    }
    else
    {
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (remote.cocooned)
            {
                wrapped[remote.id] = {remote.position, remote.yaw};
            }
        }
    }
    if (!m_cocoonMesh.IsValid() && !wrapped.empty())
    {
        // A husk of dried flesh and sinew the size of a person, lumpy, glistening where it is fresh.
        MeshData husk;
        const glm::mat4 identity(1.0f);
        husk.Append(Primitives::Ellipsoid({0.42f, 1.0f, 0.36f}, 18, 12), glm::translate(identity, {0.0f, 0.95f, 0.0f}));
        for (int k = 0; k < 9; ++k)
        {
            const float a = static_cast<float>(k) * 2.4f;
            const float y = 0.3f + 0.17f * static_cast<float>(k);
            husk.Append(Primitives::Ellipsoid({0.22f, 0.16f, 0.2f}, 10, 7),
                        glm::translate(identity, {std::cos(a) * 0.3f, y, std::sin(a) * 0.26f}));
        }
        for (MeshVertex& vertex : husk.vertices)
        {
            const float shade = 0.55f + 0.45f * std::abs(std::sin(vertex.position.y * 17.0f + vertex.position.x * 9.0f));
            const glm::vec3 colour = glm::vec3(0.34f, 0.27f, 0.22f) * shade;
            const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
            vertex.color = channel(colour.r) | (channel(colour.g) << 8) | (channel(colour.b) << 16) | (channel(0.5f) << 24);
        }
        m_cocoonMesh = m_app->GetMeshes().Upload(husk, "cocoon_husk");
    }
    // Gone ones taken away.
    for (auto it = m_cocoonEntities.begin(); it != m_cocoonEntities.end();)
    {
        if (wrapped.count(it->first) == 0)
        {
            m_interactions.Unregister(it->second);
            m_scene.Destroy(it->second);
            it = m_cocoonEntities.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const auto& [player, place] : wrapped)
    {
        auto found = m_cocoonEntities.find(player);
        if (found == m_cocoonEntities.end())
        {
            Transform transform;
            const Entity entity = m_scene.CreateMeshEntity("cocoon", transform, m_cocoonMesh, Material::Diffuse(glm::vec3(1.0f), 0.55f));
            if (player != LocalPlayerId())
            {
                Interactable interactable;
                interactable.entity = entity;
                interactable.kind = InteractionKind::Cocoon;
                interactable.verb = "Cut free";
                interactable.name = "Cocoon";
                interactable.payload = player;
                interactable.range = 2.2f;
                interactable.focusOffset = {0.0f, 1.0f, 0.0f};
                m_interactions.Register(interactable);
            }
            found = m_cocoonEntities.emplace(player, entity).first;
        }
        if (Transform* transform = m_scene.GetTransform(found->second))
        {
            transform->position = place.first;
            transform->rotation = glm::angleAxis(-place.second, glm::vec3(0.0f, 1.0f, 0.0f));
        }
    }
}

void PredationGame::OpenDoorForCreature(int door)
{
    const WorldObjects::Door* found = m_world.GetDoor(door);
    if (found == nullptr || found->IsOpen() || found->locked)
    {
        return;
    }
    // Through the same door as a player opening it, so the sound, the noise and the message to everybody
    // else all follow.
    PerformInteraction(InteractionKind::Door, door, kNoKiller);
}

void PredationGame::BashDoor(int door, const Creature& creature)
{
    WorldObjects::Door* found = m_world.GetDoor(door);
    if (found == nullptr)
    {
        return;
    }
    PlaySound(m_sounds.door.Pick(), found->hinge + glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.55f);
    MakeNoise(NoiseKind::Door, found->hinge, NoiseReach::kDoor * 1.8f, kNoKiller);
    // Heavier things break it sooner.
    const int needed = std::clamp(static_cast<int>(6.0f - creature.Capabilities().mass / 60.0f), 2, 6);
    if (++m_doorBlows[door] >= needed)
    {
        found->locked = false;
        m_doorBlows.erase(door);
        PRED_LOG_INFO(AI, "Creature {} broke door {} open", creature.NetId(), door);
        PerformInteraction(InteractionKind::Door, door, kNoKiller);
    }
}

} // namespace pred
