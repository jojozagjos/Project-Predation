#include "Game/PredationGame.h"

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Game/Creature/Noise.h"

#include <glm/geometric.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

// The sounds of things happening, as opposed to the sounds of things being done.
//
// Most of what a player hears is something changing: a magazine coming out, a creature's foot coming
// down, a door that will not open. Where the change is already on every machine -- a reload is in the
// snapshot, a creature's feet are posed everywhere -- each machine notices it for itself and makes the
// sound, and nothing is sent. Where it is not, the host says so with a Sound event (ShareSound), which
// carries the name of the sound and where. Either way a sound is only ever made by the machine that
// hears it, which is the only way nobody hears one twice.
//
// Everything here plays files from Assets/Audio by the name of their folder. Nothing is generated.

namespace pred
{

// How loud the building is: the hum, the air in the ducts, the things settling out of sight.
CVar<float> cv_ambienceVolume{"audio.ambience", 1.0f, "How loud the building's own sounds are", CVarFlags::Archive};

namespace
{

float Random01()
{
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

// How big a creature sounds: small ones higher and lighter, large ones lower and heavier.
float CreaturePitch(const Creature& creature)
{
    switch (creature.Capabilities().size)
    {
    case SizeClass::Small: return 1.3f;
    case SizeClass::Large: return 0.78f;
    default: break;
    }
    return 1.0f;
}

} // namespace

uint16_t PredationGame::SoundKey(const std::string& name)
{
    // FNV-1a, folded to sixteen bits: the same on every machine, and a sound's name rather than its
    // position in a list, so a machine with a folder more or less still agrees about every other one.
    uint32_t hash = 2166136261u;
    for (const char c : name)
    {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return static_cast<uint16_t>((hash >> 16) ^ (hash & 0xFFFFu));
}

void PredationGame::PlayNamed(const std::string& name, const glm::vec3& at, float gain, float pitch, bool positioned)
{
    PlaySound(Sounds(name).Pick(), at, gain, pitch, positioned);
}

void PredationGame::ShareSound(const std::string& name, const glm::vec3& at, float gain)
{
    PlayNamed(name, at, gain);
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::Sound;
        event.item = SoundKey(name);
        event.position = at;
        event.amount = gain;
        m_host.Broadcast(event);
    }
}

void PredationGame::HearSharedSound(const WorldEventMessage& event)
{
    const auto found = m_soundKeys.find(event.item);
    if (found != m_soundKeys.end())
    {
        PlayNamed(found->second, event.position, event.amount);
    }
}

void PredationGame::PlayerSound(uint8_t player, const std::string& name, float gain)
{
    // Your own is in your head, flat in both ears. Anybody else's comes from their body.
    if (player == LocalPlayerId())
    {
        PlayNamed(name, m_player.State().position, gain, 1.0f, false);
        return;
    }
    glm::vec3 at;
    if (PlayerPositionIfKnown(player, at))
    {
        PlayNamed(name, at + glm::vec3(0.0f, 1.5f, 0.0f), gain);
    }
}

void PredationGame::QueueSound(float delay, const std::string& name, const glm::vec3& at, float gain, bool positioned)
{
    m_queuedSounds.push_back({m_soundClock + delay, name, at, gain, positioned});
}

void PredationGame::PlayImpact(const Tracer& tracer)
{
    if (!tracer.hit)
    {
        return;
    }
    // Where it landed. A round into a wall across the room says where somebody is shooting at, and a
    // round that found flesh says it found flesh.
    PlayNamed(tracer.surface ? "Weapons/impact_hard" : "Weapons/impact_flesh", tracer.to, tracer.surface ? 0.65f : 0.9f,
              0.92f + 0.16f * Random01());
}

void PredationGame::PlayReloadCues(const WeaponDefinition* weapon, bool empty, float from, float to,
                                   const glm::vec3& at, bool positioned, float gain, int player)
{
    if (weapon == nullptr || to <= from)
    {
        return;
    }
    for (const WeaponSoundCue& cue : weapon->ReloadSoundsFrom(empty))
    {
        if (cue.at > from && cue.at <= to)
        {
            PlayNamed(cue.sound, at, gain, 0.96f + 0.08f * Random01(), positioned);
            // A magazine going home is not quiet, and something listening for people hears it.
            if (IsAuthority())
            {
                MakeNoise(NoiseKind::Item, at, NoiseReach::kItem * 0.8f, player);
            }
        }
    }
}

void PredationGame::UpdateWorldSounds(float dt)
{
    m_soundClock += dt;

    // Whatever was put off -- a casing that has not hit the floor yet -- once its moment comes.
    for (size_t i = 0; i < m_queuedSounds.size();)
    {
        if (m_queuedSounds[i].at <= m_soundClock)
        {
            const QueuedSound sound = m_queuedSounds[i];
            m_queuedSounds.erase(m_queuedSounds.begin() + static_cast<ptrdiff_t>(i));
            PlayNamed(sound.name, sound.position, sound.gain, 0.94f + 0.12f * Random01(), sound.positioned);
            continue;
        }
        ++i;
    }

    // --- This player's hands and lungs ---------------------------------------------------------------
    const PlayerState& local = m_player.State();
    if (m_weapon.weapon != m_soundWeapon)
    {
        // Brought up, or put away. Not on the first frame of a game, when nothing was in hand to change.
        if (m_soundWeapon != kInvalidWeapon || m_weapon.weapon != kInvalidWeapon)
        {
            PlayNamed(m_weapon.weapon != kInvalidWeapon ? "Weapons/draw" : "Weapons/holster", local.position, 0.45f, 1.0f,
                      false);
        }
        m_soundWeapon = m_weapon.weapon;
        m_soundReloading = false;
    }
    {
        const bool reloading = m_weapon.IsReloading();
        const float progress = reloading ? ReloadProgress() : 0.0f;
        if (reloading)
        {
            PlayReloadCues(EquippedWeapon(), m_weapon.reloadFromEmpty, m_soundReloading ? m_soundReload : 0.0f, progress,
                           MuzzlePosition(), false, 0.7f, LocalPlayerId());
        }
        m_soundReloading = reloading;
        m_soundReload = progress;
    }
    // Off the ground on purpose: the effort of it, quietly.
    if (m_soundGrounded && !local.grounded && local.velocity.y > 1.5f && local.alive)
    {
        PlayNamed("Player/jump", local.position, 0.3f, 0.95f + 0.1f * Random01(), false);
    }
    m_soundGrounded = local.grounded;
    // Run until there is nothing left and it is heard, until there is something again.
    if (local.alive && local.stamina < 0.05f)
    {
        m_exhausted = true;
    }
    else if (local.stamina > 0.6f || !local.alive)
    {
        m_exhausted = false;
    }
    if (m_exhausted && m_soundClock >= m_breathAt)
    {
        PlayNamed("Player/out_of_breath", local.position, 0.45f, 0.95f + 0.1f * Random01(), false);
        m_breathAt = m_soundClock + 1.15f + 0.2f * Random01();
    }

    // --- Everybody else's --------------------------------------------------------------------------
    //
    // What they are doing arrives with them thirty times a second: a reload and how far through it, what
    // is in their hands, their torch, whether they are on the ground. The sounds are those changing.
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        HeardPlayer& heard = m_heardPlayers[remote.id];
        const glm::vec3 chest = remote.position + glm::vec3(0.0f, 1.3f, 0.0f);
        if (!heard.known)
        {
            heard.known = true;
            heard.held = remote.heldItem;
            heard.torch = remote.torchOn;
            heard.grounded = remote.grounded;
        }
        if (remote.reloading)
        {
            PlayReloadCues(WeaponHeldBy(remote.id), remote.reloadEmpty, heard.reloading ? heard.reload : 0.0f,
                           remote.reloadProgress, chest, true, 0.75f, remote.id);
        }
        heard.reloading = remote.reloading;
        heard.reload = remote.reloadProgress;
        if (remote.heldItem != heard.held)
        {
            if (WeaponHeldBy(remote.id) != nullptr)
            {
                PlayNamed("Weapons/draw", chest, 0.5f);
            }
            heard.held = remote.heldItem;
        }
        if (remote.torchOn != heard.torch)
        {
            PlayNamed(remote.torchOn ? "Player/torch_on" : "Player/torch_off", chest, 0.35f);
            heard.torch = remote.torchOn;
        }
        if (!remote.grounded)
        {
            heard.fall = std::max(heard.fall, -remote.velocity.y);
        }
        if (heard.grounded && !remote.grounded && remote.velocity.y > 1.5f && remote.alive)
        {
            PlayNamed("Player/jump", chest, 0.35f);
        }
        if (!heard.grounded && remote.grounded)
        {
            if (heard.fall > 3.0f)
            {
                PlayNamed("Player/land", remote.position, std::clamp(heard.fall / 9.0f, 0.25f, 1.0f) * 0.8f);
            }
            heard.fall = 0.0f;
        }
        heard.grounded = remote.grounded;
    }
    // Forgotten when they go, so somebody who rejoins under the same number starts fresh.
    for (auto it = m_heardPlayers.begin(); it != m_heardPlayers.end();)
    {
        const bool present = std::any_of(RemotePlayers().begin(), RemotePlayers().end(),
                                          [&](const RemotePlayerView& remote) { return remote.id == it->first; });
        it = present ? std::next(it) : m_heardPlayers.erase(it);
    }

    // --- The world's own ---------------------------------------------------------------------------
    //
    // A crate's lid swings on every machine, so each hears it open and fall shut for itself.
    const std::vector<WorldObjects::AmmoCrate>& crates = m_world.AmmoCrates();
    m_crateLids.resize(crates.size(), 0.0f);
    for (size_t i = 0; i < crates.size(); ++i)
    {
        const float was = m_crateLids[i];
        const float now = crates[i].lidAngle;
        if (was < 0.02f && now >= 0.02f)
        {
            PlayNamed("World/crate_open", crates[i].lidRest, 0.7f);
            PlayNamed("World/ammo_take", crates[i].lidRest + glm::vec3(0.0f, 0.2f, 0.0f), 0.6f);
        }
        else if (was >= 0.02f && now < 0.02f)
        {
            PlayNamed("World/crate_close", crates[i].lidRest, 0.7f);
        }
        m_crateLids[i] = now;
    }

    UpdateCreatureSounds(dt);
}

void PredationGame::UpdateCreatureSounds(float dt)
{
    (void)dt;
    // Everything a creature sounds like comes from what every machine already has of it: where its feet
    // come down, what its body is doing, how hurt it is, and what it has in mind. So the host and every
    // client hear the same animal without a single sound being sent for it.
    for (const std::unique_ptr<Creature>& owned : m_creatures)
    {
        Creature& creature = *owned;
        HeardCreature& heard = m_heardCreatures[creature.NetId()];
        const float pitch = CreaturePitch(creature);
        const glm::vec3 head = creature.Eye();
        const float mass = creature.Capabilities().mass;
        const bool heavy = mass > 170.0f;
        if (!heard.known)
        {
            heard.known = true;
            heard.health = creature.Health() / std::max(creature.MaxHealth(), 1.0f);
            heard.alive = creature.Alive();
            heard.breathAt = m_soundClock + 1.0f + 3.0f * Random01();
            heard.voiceAt = m_soundClock + 4.0f + 6.0f * Random01();
        }

        // What it has in mind decides how much of it is heard. Something stalking, lying in wait, going round
        // to shooting or luring somebody is all but silent -- a stalker that could be heard across the room
        // could not stalk anybody -- and only something running flat out is loud.
        const Behavior intent = creature.Doing();
        const bool sneaking = intent == Behavior::Stalk || intent == Behavior::Ambush || intent == Behavior::Lure ||
                              intent == Behavior::Flank || intent == Behavior::Search;
        const float listenerAway = glm::distance(m_renderEye, head);

        // Its feet, as they come down. Creeping is quieter, which is what creeping is for.
        std::vector<glm::vec3> footfalls = creature.TakeFootfalls();
        const float creep = 1.0f - 0.6f * creature.Crouch();
        const float pace = std::clamp(creature.Speed() / 4.5f, 0.35f, 1.0f);
        const float stepGain = std::clamp(0.2f + mass / 500.0f, 0.2f, 0.8f) * creep * pace * (sneaking ? 0.15f : 1.0f);
        for (size_t i = 0; i < footfalls.size() && i < 2; ++i)
        {
            // Up a wall, across a ceiling or down a crawlspace, what is heard is claws scraping -- and heard
            // it is, through the wall or the roof: that is how anybody knows it is up there at all.
            const bool clinging = creature.Clinging() != Creature::Cling::Floor;
            const bool inCrawlspace = m_nav.Valid() && m_nav.InCrawlspace(creature.Position());
            const bool scraping = clinging || inCrawlspace;
            PlayNamed(scraping ? "Creature/climb" : (heavy ? "Creature/step_heavy" : "Creature/step_light"), footfalls[i],
                      scraping ? std::max(stepGain * 0.8f, sneaking ? 0.08f : 0.3f) : stepGain, pitch * (0.93f + 0.14f * Random01()));
        }

        // What its body has just started doing.
        const RigAction action = creature.CurrentAction().kind;
        if (action != heard.action && creature.Alive())
        {
            switch (action)
            {
            case RigAction::Swipe: PlayNamed("Creature/swipe", head, 0.8f, pitch); break;
            case RigAction::Bite: PlayNamed("Creature/bite", head, 0.9f, pitch); break;
            case RigAction::Lunge: PlayNamed("Creature/lunge", head, 1.0f, pitch); break;
            case RigAction::Grab: PlayNamed("Creature/grab", head, 0.9f, pitch); break;
            case RigAction::Roar:
                // Rearing up is two different things: calling the others, and warning somebody off.
                PlayNamed(creature.Doing() == Behavior::Warn ? "Creature/warning" : "Creature/call", head, 1.0f, pitch);
                break;
            default: break;
            }
        }
        heard.action = action;

        // Hurt, and dying.
        const float health = creature.Health() / std::max(creature.MaxHealth(), 1.0f);
        if (creature.Alive() && health < heard.health - 0.004f && m_soundClock - heard.hurtAt > 0.6f)
        {
            PlayNamed("Creature/hurt", head, 0.9f, pitch * (0.94f + 0.12f * Random01()));
            heard.hurtAt = m_soundClock;
        }
        heard.health = health;
        if (heard.alive && !creature.Alive())
        {
            PlayNamed("Creature/death", head, 1.0f, pitch);
        }
        heard.alive = creature.Alive();
        if (!creature.Alive() || creature.Down())
        {
            // Dead is silent. So is one playing dead, which is the whole of the trick.
            continue;
        }

        // Breathing, all the time, faster when it has been running. Quiet: it is heard close to, which
        // is exactly when it should be.
        const Behavior doing = creature.Doing();
        const bool stalking = doing == Behavior::Stalk;
        // Sneaking, it holds its breath -- until it is close enough to be heard breathing right behind you.
        const bool heldBreath = sneaking && listenerAway > 3.5f;
        // Breaking cover to go for somebody: a shriek, the first thing about it anybody hears.
        const bool wasHiding = heard.doing == Behavior::Stalk || heard.doing == Behavior::Ambush || heard.doing == Behavior::Lure;
        if (wasHiding && (intent == Behavior::Hunt || intent == Behavior::Attack))
        {
            PlayNamed("Creature/call", head, 1.0f, pitch * 1.15f);
        }
        // Something close turning on you: the sting, in both ears, over everything. Not every time --
        // a sting that comes with every chase is a sound effect, not a fright.
        const bool nowAfter = intent == Behavior::Hunt || intent == Behavior::Attack;
        const bool wasAfter = heard.doing == Behavior::Hunt || heard.doing == Behavior::Attack || heard.doing == Behavior::Drag;
        if (nowAfter && !wasAfter && listenerAway < 18.0f && m_player.State().alive && m_soundClock >= m_stingReadyAt)
        {
            m_stingReadyAt = m_soundClock + 25.0f;
            PlayNamed("Music/sting", m_renderEye, 0.7f, 1.0f, false);
        }
        heard.doing = intent;
        const CreatureTraits& habits = creature.Brain().Traits();
        const bool silent = habits.Has(Quirk::Silent);
        // A clicker, in the dark or looking for something, clicks: a dry, quick run of it every few seconds.
        if (habits.Has(Quirk::Clicker) && !sneaking && m_soundClock >= heard.clickAt)
        {
            PlayNamed("Creature/chitter", head, 0.45f, pitch * 1.6f);
            heard.clickAt = m_soundClock + 2.0f + 3.0f * Random01();
        }
        if (m_soundClock >= heard.breathAt && !heldBreath && !silent)
        {
            PlayNamed("Creature/breath", head, stalking || sneaking ? 0.22f : 0.3f, pitch * (0.95f + 0.1f * Random01()));
            const float breathPace = creature.Speed() > 3.0f ? 0.55f : 1.0f;
            heard.breathAt = m_soundClock + (2.6f + 2.2f * Random01()) * breathPace;
        }

        // And what it has in mind, now and then: a growl when it means somebody harm, chittering when it
        // is looking into something. A stealthy one is quieter about it, and a stalker says nothing.
        if (m_soundClock >= heard.voiceAt)
        {
            const float stealth = creature.Brain().Traits().stealth;
            std::string voice;
            float gain = 0.8f;
            switch (doing)
            {
            case Behavior::Hunt:
            case Behavior::Attack:
            case Behavior::Drag:
                voice = "Creature/growl";
                break;
            case Behavior::Investigate:
            case Behavior::Observe:
            case Behavior::Roam:
                // Low, and only now and then: something that chatters to itself all the time is
                // never where you did not expect it.
                voice = "Creature/chitter";
                gain = 0.3f;
                break;
            case Behavior::Warn:
                voice = "Creature/growl";
                gain = 0.6f;
                break;
            default:
                break;
            }
            if (!voice.empty() && !(silent && doing != Behavior::Attack))
            {
                PlayNamed(voice, head, gain, pitch * (0.95f + 0.1f * Random01()));
            }
            const float busy = doing == Behavior::Roam ? 2.0f : 1.0f;
            heard.voiceAt = m_soundClock + (4.0f + 5.0f * Random01()) * busy * (1.0f + 1.5f * stealth);
        }
    }
    for (auto it = m_heardCreatures.begin(); it != m_heardCreatures.end();)
    {
        const bool present = std::any_of(m_creatures.begin(), m_creatures.end(),
                                         [&](const std::unique_ptr<Creature>& c) { return c->NetId() == it->first; });
        it = present ? std::next(it) : m_heardCreatures.erase(it);
    }
}

void PredationGame::UpdateTension(float dt)
{
    AudioEngine& audio = m_app->GetAudio();
    const bool playing = m_screen == Screen::Playing && m_player.State().alive;
    // How much danger there is: how afraid the picture is (something close and after you, being held,
    // badly hurt), and a floor under it while anything is about at all.
    float danger = playing ? m_fearShown : 0.0f;
    if (playing)
    {
        for (const std::unique_ptr<Creature>& creature : m_creatures)
        {
            if (creature->Alive())
            {
                danger = std::max(danger, 0.12f * std::clamp(1.0f - glm::distance(creature->Position(), m_renderEye) / 40.0f, 0.0f, 1.0f));
            }
        }
        danger = std::max(danger, m_menace * 0.5f);
    }
    const float level = std::clamp(cv_ambienceVolume.Get(), 0.0f, 2.0f);

    // The drone: always there in a game, barely, and swelling as things get worse. It comes up quickly
    // and goes down slowly, so relief is something that happens over a while.
    const float target = playing ? 0.03f + 0.55f * danger : 0.0f;
    const float rate = target > m_droneLevel ? 1.2f : 0.25f;
    m_droneLevel += (target - m_droneLevel) * (1.0f - std::exp(-rate * dt));
    if (m_drone != kInvalidVoice && !audio.IsPlaying(m_drone))
    {
        m_drone = kInvalidVoice;
    }
    if (m_drone == kInvalidVoice && playing)
    {
        AudioEngine::PlayDesc desc;
        desc.sound = Sounds("Music/drone").Pick();
        desc.loop = true;
        desc.positioned = false;
        desc.gain = 0.0f;
        desc.reverbSend = 0.0f;
        if (desc.sound != kInvalidSound)
        {
            m_drone = audio.Play(desc);
        }
    }
    if (m_drone != kInvalidVoice)
    {
        audio.SetVoiceGain(m_drone, m_droneLevel * level);
    }

    // Your own heart, when you are frightened -- or hidden, with something near. Faster the worse it is.
    bool hiddenNear = false;
    if (m_hidingSpot >= 0)
    {
        for (const std::unique_ptr<Creature>& creature : m_creatures)
        {
            hiddenNear = hiddenNear || (creature->Alive() && glm::distance(creature->Position(), m_renderEye) < 12.0f);
        }
    }
    const float beat = std::max(danger, hiddenNear ? 0.6f : 0.0f);
    m_heartbeatAt -= dt;
    if (playing && beat > 0.3f && m_heartbeatAt <= 0.0f)
    {
        PlayNamed("Player/heartbeat", m_renderEye, 0.25f + 0.45f * beat, 1.0f, false);
        m_heartbeatAt = 60.0f / (75.0f + 75.0f * beat);
    }
}

void PredationGame::UpdateAmbience(float dt)
{
    // The building: a hum under everything, the air moving in the ducts, and now and then something
    // settling or falling somewhere out of sight. Each machine has its own -- none of it is anything
    // happening, it is the place. Only in a game: the title screen is quiet until it has music of its own.
    AudioEngine& audio = m_app->GetAudio();
    const float level = std::clamp(cv_ambienceVolume.Get(), 0.0f, 2.0f);
    const float inGame = m_screen == Screen::Playing ? 1.0f : 0.0f;
    // Where the ears are, a few times a second. Under a roof is inside; a roof within a couple of metres
    // of the floor is somewhere tight -- a vent, the crawlspace; and a nest near enough is heard, and
    // smelt, before it is seen. The loops fade across as you move, over about a second, so walking out
    // of a door is the room tone giving way to the wind rather than one switching off.
    m_zoneProbeAt -= dt;
    if (m_zoneProbeAt <= 0.0f)
    {
        m_zoneProbeAt = 0.25f;
        const glm::vec3 ear = m_renderEye;
        const RayHit roof = m_app->GetPhysics().RayCastStatic(ear, glm::vec3(0.0f, 1.0f, 0.0f), 30.0f);
        const RayHit floor = m_app->GetPhysics().RayCastStatic(ear, glm::vec3(0.0f, -1.0f, 0.0f), 4.0f);
        const float headroom = roof && floor ? roof.distance + floor.distance : 99.0f;
        m_zoneIndoor = roof ? 1.0f : 0.0f;
        m_zoneTight = headroom < 2.2f ? 1.0f : 0.0f;
        float nest = 0.0f;
        for (const Nest& built : m_nests)
        {
            if (!built.dead)
            {
                nest = std::max(nest, std::clamp(1.0f - (glm::distance(built.heart, ear) - 4.0f) / 14.0f, 0.0f, 1.0f));
            }
        }
        m_zoneNest = nest;

        // The room, as the ears hear it: how far the walls are round about, and the roof. A vent is
        // small and hard and rings; a big hall is a long tail; outside there is almost nothing to
        // come back.
        float sum = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            const float angle = static_cast<float>(i) * (glm::two_pi<float>() / 8.0f);
            const RayHit wall = m_app->GetPhysics().RayCastStatic(ear, {std::cos(angle), 0.0f, std::sin(angle)}, 30.0f);
            sum += wall ? wall.distance : 30.0f;
        }
        const float across = sum / 8.0f;
        AudioEngine::Room room;
        if (m_zoneTight > 0.5f)
        {
            room = {0.2f, 0.15f, 0.32f};
        }
        else if (m_zoneIndoor > 0.5f)
        {
            const float size = std::clamp((across - 2.0f) / 16.0f + (roof ? std::min(roof.distance, 10.0f) / 40.0f : 0.0f), 0.0f, 1.0f);
            room = {size, 0.45f - 0.2f * size, 0.14f + 0.22f * size};
        }
        else
        {
            room = {0.35f, 0.8f, 0.04f};
        }
        audio.SetRoom(room);
    }
    const float targets[5] = {
        m_zoneIndoor * (1.0f - m_zoneTight) * (1.0f - 0.6f * m_zoneNest), // the room
        m_zoneIndoor * (1.0f - m_zoneTight) * 0.8f + 0.1f,                // air in the ducts, faint outside
        (1.0f - m_zoneIndoor) + 0.08f * m_zoneIndoor,                     // wind, a breath of it indoors
        m_zoneTight,                                                      // inside the vent itself
        m_zoneNest,                                                       // the nest
    };
    float faded[5];
    for (int i = 0; i < 5; ++i)
    {
        faded[i] = targets[i] * inGame;
    }
    const float ease = 1.0f - std::exp(-1.6f * dt);
    for (int i = 0; i < 5; ++i)
    {
        AmbienceLoop& loop = m_ambienceLoops[i];
        loop.weight += (faded[i] - loop.weight) * ease;
        if (loop.voice != kInvalidVoice && !audio.IsPlaying(loop.voice))
        {
            loop.voice = kInvalidVoice;
        }
        if (loop.voice == kInvalidVoice)
        {
            AudioEngine::PlayDesc desc;
            desc.sound = Sounds(loop.name).Pick();
            if (desc.sound == kInvalidSound)
            {
                continue;
            }
            desc.loop = true;
            desc.positioned = false;
            desc.gain = 0.0f;
            loop.voice = audio.Play(desc);
        }
        if (loop.voice != kInvalidVoice)
        {
            audio.SetVoiceGain(loop.voice, loop.gain * loop.weight * level);
        }
    }

    // A failing lamp buzzes, and the buzz stutters with it: the nearest one within earshot, from where
    // it hangs, as loud as it is lit.
    m_lightClock += dt;
    const LevelLights::Light* buzzing = nullptr;
    float nearest = 10.0f;
    for (const LevelLights::Light& light : m_levelLights.Lights())
    {
        const bool unsteady = light.mood == LightMood::Failing || light.mood == LightMood::Flicker;
        const float away = glm::distance(light.position, m_renderEye);
        if (unsteady && away < nearest)
        {
            nearest = away;
            buzzing = &light;
        }
    }
    if (m_buzz != kInvalidVoice && !audio.IsPlaying(m_buzz))
    {
        m_buzz = kInvalidVoice;
    }
    if (buzzing != nullptr && m_buzz == kInvalidVoice)
    {
        AudioEngine::PlayDesc desc;
        desc.sound = Sounds("Ambience/buzz").Pick();
        desc.loop = true;
        desc.positioned = true;
        desc.position = buzzing->position;
        desc.gain = 0.0f;
        desc.nearDistance = 1.5f;
        desc.farDistance = 10.0f;
        if (desc.sound != kInvalidSound)
        {
            m_buzz = audio.Play(desc);
        }
    }
    if (m_buzz != kInvalidVoice)
    {
        if (buzzing != nullptr)
        {
            audio.SetVoicePosition(m_buzz, buzzing->position);
        }
        audio.SetVoiceGain(m_buzz, buzzing != nullptr ? 0.5f * buzzing->level * level * inGame : 0.0f);
    }

    m_ambienceClock += dt;
    if (m_screen != Screen::Playing || m_ambienceClock < m_ambienceNext || level <= 0.0f)
    {
        return;
    }
    static const char* const kDistant[] = {"Ambience/groan", "Ambience/groan", "Ambience/distant_bang", "Ambience/drip", "Ambience/drip"};
    // A building's noises, heard as a building's: outside only something falling far off, and nothing at
    // all from inside a vent, where the fan covers it.
    const char* name = m_zoneIndoor > 0.5f ? kDistant[std::rand() % 5] : "Ambience/distant_bang";
    if (m_zoneTight > 0.5f)
    {
        m_ambienceNext = m_ambienceClock + 10.0f;
        return;
    }
    const float angle = glm::two_pi<float>() * Random01();
    const float distance = 18.0f + 25.0f * Random01();
    AudioEngine::PlayDesc desc;
    desc.sound = Sounds(name).Pick();
    desc.position = m_renderEye + glm::vec3(std::cos(angle) * distance, 2.0f + 3.0f * Random01(), std::sin(angle) * distance);
    desc.positioned = true;
    desc.gain = 0.55f * level;
    desc.pitch = 0.9f + 0.2f * Random01();
    // Heard from a long way off, as a building carries a sound.
    desc.nearDistance = 12.0f;
    desc.farDistance = 90.0f;
    audio.Play(desc);
    m_ambienceNext = m_ambienceClock + 18.0f + 30.0f * Random01();
}

void PredationGame::MenuSounds()
{
    // Only where there is a menu: the debug windows over a game in progress stay silent.
    const bool menu = m_screen == Screen::Title || m_paused || m_settingsOpen;
    const ImGuiID hovered = ImGui::GetCurrentContext() != nullptr ? ImGui::GetCurrentContext()->HoveredId : 0;
    if (menu && hovered != 0 && hovered != m_menuHovered)
    {
        PlayNamed("UI/hover", m_renderEye, 0.45f, 1.0f, false);
    }
    if (menu && hovered != 0 && ImGui::GetIO().MouseClicked[0])
    {
        PlayNamed("UI/click", m_renderEye, 0.6f, 1.0f, false);
    }
    m_menuHovered = hovered;

    // Into a game, and the pause menu coming and going.
    const int screen = static_cast<int>(m_screen);
    if (m_menuWasScreen >= 0 && screen != m_menuWasScreen && m_screen == Screen::Playing)
    {
        PlayNamed("UI/confirm", m_renderEye, 0.6f, 1.0f, false);
    }
    else if (m_paused != m_menuWasPaused && m_screen == Screen::Playing)
    {
        PlayNamed(m_paused ? "UI/click" : "UI/back", m_renderEye, 0.55f, 1.0f, false);
    }
    m_menuWasScreen = screen;
    m_menuWasPaused = m_paused;
}

} // namespace pred
