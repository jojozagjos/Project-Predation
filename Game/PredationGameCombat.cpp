#include "Game/PredationGame.h"
#include "Game/OverlayLayer.h"
#include "Game/PredationGameShared.h"

#include "Engine/Render/DepthConvention.h"
#include "Game/Creature/CreatureTuning.h"

#include "Engine/Core/BuildInfo.h"
#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Assets/GltfImport.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/World/TestMap.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/Surfaces.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Debug/FrameStats.h"
#include "Engine/Platform/Window.h"
#include "Engine/Audio/SoundDesign.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <limits>
#include <string>


// Shooting and being shot: aim, recoil, rounds, tracers, bullet holes, damage, ammunition.

namespace pred
{

glm::vec3 PredationGame::MuzzleOf(uint8_t player, const glm::vec3& eye, const glm::vec3& direction) const
{
    // Where that player's weapon is being drawn on this machine. Falls back to a point in front of
    // the eye when there is no body for them, which is better than the eye itself: a round leaving
    // somebody's face is the thing this exists to stop.
    for (const auto& avatar : m_avatars)
    {
        if (avatar->id == player && avatar->body.HasWeapon())
        {
            return avatar->body.MuzzlePoint();
        }
    }
    return eye + direction * 0.45f - glm::vec3(0.0f, 0.12f, 0.0f);
}

std::vector<NetHost::PlayerPose> PredationGame::PosesNow() const
{
    // Who is where at this instant. The host's own shots are aimed at what is on its own screen,
    // which is the present, so there is nothing to rewind.
    std::vector<NetHost::PlayerPose> poses;
    poses.push_back({LocalPlayerId(), m_player.State().position, m_player.State().stance,
                     m_player.State().alive});
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        poses.push_back({remote.id, remote.position, remote.stance, remote.alive});
    }
    return poses;
}

void PredationGame::ResolvePlayerHits(const FireEvent& shot, uint8_t shooter, ShotResult& worldHit,
                                      const std::vector<NetHost::PlayerPose>& poses)
{
    if (!cv_friendlyFire.Get())
    {
        return;
    }

    // Players are tested against directly rather than through the physics world, because a
    // character capsule is a moving query volume rather than a body a ray can find. Doing it here
    // also puts every hit decision in one place, which is where rewinding for lag will go.
    const float maxDistance = worldHit ? worldHit.distance : shot.range;
    const float radius = m_player.Config().radius;

    uint8_t bestPlayer = kMaxPlayers;
    float bestDistance = maxDistance;
    glm::vec3 bestPoint{0.0f};

    const auto testPlayer = [&](uint8_t id, const glm::vec3& feet, PlayerStance stance)
    {
        if (id == shooter)
        {
            return;
        }
        const float height = m_player.Config().HeightForStance(stance);
        // The capsule as a segment from its lower to its upper centre, which is what the character
        // controller actually is.
        const glm::vec3 low = feet + glm::vec3(0.0f, radius, 0.0f);
        const glm::vec3 high = feet + glm::vec3(0.0f, std::max(height - radius, radius), 0.0f);

        // Closest approach between the shot ray and the capsule's axis.
        const glm::vec3 axis = high - low;
        const glm::vec3 toLow = low - shot.origin;
        const float axisLengthSq = glm::dot(axis, axis);
        const float rayDotAxis = glm::dot(shot.direction, axis);
        const float rayDotToLow = glm::dot(shot.direction, toLow);
        const float axisDotToLow = glm::dot(axis, toLow);

        const float denominator = axisLengthSq - rayDotAxis * rayDotAxis;
        float alongRay = 0.0f;
        float alongAxis = 0.0f;
        if (std::abs(denominator) > 1e-5f)
        {
            alongRay = (rayDotToLow * axisLengthSq - axisDotToLow * rayDotAxis) / denominator;
            alongAxis = (alongRay * rayDotAxis - axisDotToLow) / std::max(axisLengthSq, 1e-5f);
        }
        else
        {
            alongRay = rayDotToLow;
        }
        alongRay = std::clamp(alongRay, 0.0f, bestDistance);
        alongAxis = std::clamp(alongAxis, 0.0f, 1.0f);

        const glm::vec3 onRay = shot.origin + shot.direction * alongRay;
        const glm::vec3 onAxis = low + axis * alongAxis;
        if (glm::distance(onRay, onAxis) > radius || alongRay <= 0.05f || alongRay >= bestDistance)
        {
            return;
        }
        bestPlayer = id;
        bestDistance = alongRay;
        bestPoint = onRay;
    };

    for (const NetHost::PlayerPose& pose : poses)
    {
        if (pose.alive)
        {
            testPlayer(pose.id, pose.position, pose.stance);
        }
    }

    if (bestPlayer >= kMaxPlayers)
    {
        return;
    }

    // A player in the way stops the round before whatever the world trace found.
    worldHit.hit = true;
    worldHit.position = bestPoint;
    worldHit.distance = bestDistance;
    // And a person is not somewhere a hole can stay. Whatever surface the trace found behind them
    // is no longer what was struck, so the mark belongs nowhere: they walk away and it would be
    // left hanging in the air.
    worldHit.surface = false;
    ApplyPlayerDamage(bestPlayer, shot.damage, shooter, shot.direction);
}

void PredationGame::ApplyPlayerDamage(uint8_t player, float amount, uint8_t killer,
                                      const glm::vec3& direction, const char* cause)
{
    if (m_sessionMode == SessionMode::Client)
    {
        return; // clients never decide damage
    }

    float remaining = 0.0f;
    if (player == 0)
    {
        m_player.ApplyDamage(amount, cause);
        remaining = m_player.State().health;
    }
    else
    {
        // Into the controller the host is simulating for them. The published view is rebuilt from
        // that controller every tick, so subtracting from the view changed nothing and their health
        // came back the moment it was read again: nobody could be killed by anything short of a
        // single fatal round, and the two machines then disagreed about who was alive.
        m_host.ApplyDamageTo(player, amount, cause);
        remaining = m_host.HealthOf(player);
    }

    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerDamaged;
        event.player = player;
        event.other = killer;
        event.amount = amount;
        m_host.Broadcast(event);
    }
    // Heard here. Everybody else hears it from the event; this machine is not sent its own, which is
    // why a host, or anybody playing alone, never heard themselves or anybody else being hurt.
    if (remaining > 0.0f)
    {
        PlayerSound(player, "Player/hurt", 0.8f);
    }

    if (remaining <= 0.0f)
    {
        KillPlayer(player, direction);
    }
}

void PredationGame::KillPlayer(uint8_t player, const glm::vec3& direction)
{
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerDied;
        event.player = player;
        event.direction = DeathPush(direction);
        event.amount = DeathIsPermanent() ? 0.0f : RespawnSecondsForDeath();
        event.flag = DeathIsPermanent();
        m_host.Broadcast(event);
    }
    if (player == LocalPlayerId())
    {
        m_player.State().alive = false;
        m_player.State().health = 0.0f;
        m_deathImpulse = DeathPush(direction);
        m_respawnTimer = RespawnSecondsForDeath();
        m_deadForGood = DeathIsPermanent();
        m_spectating = -1;
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        // The host runs the clock for everybody, because the host is what decides they are dead.
        m_remoteRespawnTimers[player] = RespawnSecondsForDeath();
    }
    if (m_sessionMode != SessionMode::Client)
    {
        PlayerSound(player, "Player/death", 1.0f);
    }
    PRED_LOG_INFO(Gameplay, "Player {} died", player);
}

void PredationGame::ApplyWeaponClipTimings()
{
    for (const WeaponDefinition& weapon : m_weaponData.All())
    {
        WeaponDefinition* editable = m_weaponData.Mutable(weapon.id);
        const std::shared_ptr<const ModelAsset> model = LoadWeaponModel(weapon.model);
        if (editable != nullptr && model != nullptr && ApplyClipTimings(*editable, *model))
        {
            PRED_LOG_INFO(Gameplay, "{} reloads in {:.2f} s ({:.2f} s from empty), from its model's clips",
                          editable->key, editable->reloadSeconds, editable->ReloadSecondsFrom(true));
        }
    }
}

float PredationGame::ReloadProgress() const
{
    // 0 at the start of a reload, 1 at the end. One function, because there were two: the pose used
    // the fraction and the wire sent one minus the seconds remaining, which is the same number only
    // for a reload that happens to take a second. At 2.2 seconds it stayed at zero until the last
    // one and then ran the whole movement inside it, so everybody else saw a reload begin as it
    // ended and play at more than twice the speed.
    if (!m_weapon.IsReloading())
    {
        return 0.0f;
    }
    // Against how long this particular reload takes, which the state knows: an empty magazine can take
    // longer than a half-full one.
    const float seconds = std::max(m_weapon.reloadTotal, 0.01f);
    return glm::clamp(1.0f - m_weapon.reloadRemaining / seconds, 0.0f, 1.0f);
}

const WeaponDefinition* PredationGame::EquippedWeapon() const
{
    return m_weaponData.Get(m_weapon.weapon);
}

const SoundVariants& PredationGame::GunshotFor(const WeaponDefinition* weapon) const
{
    // Each weapon its own report, from Assets/Audio/Weapons/<key>_shot/, so a pistol and a carbine can be
    // told apart through a wall. A weapon with no folder of its own sounds like every other gun.
    if (weapon != nullptr)
    {
        const SoundVariants& own = Sounds("Weapons/" + weapon->key + "_shot");
        if (!own.ids.empty())
        {
            return own;
        }
    }
    return m_sounds.gunshot;
}

const WeaponDefinition* PredationGame::WeaponHeldBy(uint8_t player) const
{
    if (player == LocalPlayerId())
    {
        return EquippedWeapon();
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.id == player)
        {
            const ItemDefinition* item = m_items.Get(static_cast<ItemId>(remote.heldItem));
            return item != nullptr ? m_weaponData.Get(m_weaponData.ForItem(item->key)) : nullptr;
        }
    }
    return nullptr;
}

void PredationGame::SyncEquippedWeapon()
{
    // The selected slot decides what is in your hands. Keeping it that way means there is no second
    // notion of "equipped" to fall out of step with the inventory.
    const int slotNow = m_inventory.SelectedSlot();
    const Inventory::Slot slot = m_inventory.Selected();
    const ItemDefinition* item = m_items.Get(slot.item);
    const WeaponId wanted = item != nullptr ? m_weaponData.ForItem(item->key) : kInvalidWeapon;

    const ItemId heldNow = (item != nullptr && wanted == kInvalidWeapon) ? item->id : kInvalidItem;

    if (slotNow == m_ammoSlot && wanted == m_weapon.weapon && heldNow == m_heldItem)
    {
        // Nothing is changing hands, so whatever was being put away is back.
        m_weaponHolster = 1.0f;
        return;
    }

    // A weapon already in the hands goes away before the next thing comes out. Two tenths of a
    // second is enough to see it leave, and it is what a model's "unequip" clip runs on: without it
    // a swap is one weapon vanishing and another appearing in the same frame, and there is no
    // moment for an authored put-away to happen in.
    //
    // Everything waits on it, the next weapon and the next item alike. The item used to be put in
    // the hand at the top of this function, before the wait, so selecting a medical kit while
    // holding a rifle put the kit in the hand and left the rifle beside it for a fifth of a second.
    if (m_weapon.HasWeapon() && m_weaponHolster > 0.0f)
    {
        m_weaponHolster = std::max(m_weaponHolster - m_lastFrameSeconds / 0.20f, 0.0f);
        if (m_weaponHolster > 0.0f)
        {
            return;
        }
    }

    // Anything that is not a weapon is still carried in a hand. Selecting a medical kit should put
    // the medical kit in your hand, not leave you empty-handed holding an inventory entry.
    if (heldNow != m_heldItem)
    {
        m_heldItem = heldNow;
        if (heldNow == kInvalidItem)
        {
            m_body.ClearHeldItem(m_scene);
        }
        else
        {
            m_body.SetHeldItem(m_scene, m_app->GetMeshes(), item->key,
                               ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
            m_body.SetHeldItemPlacement(item->holdOffset, item->holdRotation, item->holdHand);
        }
    }
    m_weaponHolster = 1.0f;

    // The magazine goes back in the slot it came out of before anything else changes hands.
    // Without this, putting a weapon away and taking it out again refilled it, which the key that
    // now holsters what is already out made trivial to do.
    if (m_ammoSlot != Inventory::kNoSlot && m_weapon.HasWeapon())
    {
        m_inventory.SetSlotAmmo(m_ammoSlot, m_weapon.rounds, m_weapon.reserve);
    }
    m_ammoSlot = slotNow;

    // Anything arriving in the hands is brought up rather than appearing already held.
    m_weaponDraw = 0.0f;

    if (wanted == kInvalidWeapon)
    {
        m_weapon = WeaponState{};
        return;
    }
    if (const WeaponDefinition* definition = m_weaponData.Get(wanted))
    {
        // Rounds do not carry between weapons; each comes with its own magazine and reserve.
        // Swapping back and forth would otherwise be a free reload.
        WeaponSim::Equip(*definition, m_weapon);
        // Unless this particular one has been carried before, in which case it is however the
        // player left it.
        if (slot.rounds >= 0)
        {
            m_weapon.rounds = slot.rounds;
            m_weapon.reserve = slot.reserve;
        }
        PRED_LOG_INFO(Gameplay, "Equipped {} ({} rounds, {} spare)", definition->name, m_weapon.rounds,
                      m_weapon.reserve);
    }
}

// Where the player is pointing, with the weapon's kick on top. One function, because the camera and
// the rounds both read it and they must never disagree: the whole reason the crosshair can be
// trusted while a weapon climbs is that the thing it climbs is the same thing the rounds leave
// along. Written out twice, they would drift the first time either was tuned.
glm::vec2 PredationGame::AimAngles() const
{
    // Just where the player is looking. Recoil is not an offset on top of this any more -- it has
    // already been added to the look angles themselves, a tick at a time, and left there. See
    // ApplyRecoilToView.
    return {m_lookYaw, std::clamp(m_lookPitch, glm::radians(-89.0f), glm::radians(89.0f))};
}

// Hands this tick's recoil to the player's own aim, where it stays.
//
// This is the difference between recoil you fight and recoil you watch. Added as a decaying offset,
// the weapon climbs on screen and then un-climbs, and the player's aim ends up exactly where it
// started -- there is never anything to pull down against. Added to the look angles, the climb is
// theirs: it stays until they move the mouse, and moving the mouse is the fight.
//
// It also means compensating works the way it should without anything being written to make it
// work. Pulling down moves m_lookPitch down while the recoil is moving it up, and the two simply
// sum; there is no second system trying to return the view to a remembered place and arguing with
// the player about where they were aiming.
void PredationGame::ApplyRecoilToView()
{
    if (std::abs(m_weapon.kickPitch) < 1e-5f && std::abs(m_weapon.kickYaw) < 1e-5f)
    {
        return;
    }
    m_lookPitch = std::clamp(m_lookPitch + glm::radians(m_weapon.kickPitch), glm::radians(-89.0f),
                             glm::radians(89.0f));
    m_lookYaw += glm::radians(m_weapon.kickYaw);
    if (m_lookYaw > glm::pi<float>())
    {
        m_lookYaw -= glm::two_pi<float>();
    }
    else if (m_lookYaw < -glm::pi<float>())
    {
        m_lookYaw += glm::two_pi<float>();
    }
}

glm::vec3 PredationGame::AimDirection() const
{
    // Rounds leave along the same line the camera looks down, so what you see under the crosshair is
    // what you hit.
    const glm::vec2 aimed = AimAngles();
    const float cp = std::cos(aimed.y);
    return {std::sin(aimed.x) * cp, std::sin(aimed.y), -std::cos(aimed.x) * cp};
}

glm::vec3 PredationGame::MuzzlePosition() const
{
    const WeaponDefinition* definition = EquippedWeapon();
    const glm::vec3 eye = m_player.View().eyePosition;
    if (definition == nullptr)
    {
        return eye;
    }
    // From the eye rather than from the model's barrel. A round that starts at the visible muzzle
    // can be on the far side of a doorframe the player is peering round, so they shoot the wall
    // they can see past. Starting at the eye means what is under the crosshair is what is hit.
    return eye + AimDirection() * (definition->muzzleForward * 0.5f);
}

glm::vec3 PredationGame::DrawnMuzzle() const
{
    // The tracer used MuzzlePosition, the round's starting point a few centimetres in front of the
    // eye -- so a shot was drawn coming out of the player's face, most plainly with the weapon held
    // low or to the side, and everybody else saw the host's rounds leave the host's head. The comment
    // at every tracer said "drawn from the muzzle"; this is the muzzle.
    if (m_hidingSpot < 0 && m_body.HasWeapon())
    {
        return m_body.MuzzlePoint();
    }
    return MuzzlePosition();
}

void PredationGame::AgeTracers(float dt)
{
    for (Tracer& tracer : m_tracers)
    {
        tracer.age += dt;
    }
    std::erase_if(m_tracers, [](const Tracer& tracer) { return tracer.age > kTracerSeconds; });
}

void PredationGame::ResolveShots()
{
    if (m_shots.empty())
    {
        return;
    }
    PhysicsWorld& physics = m_app->GetPhysics();
    m_weaponKick = 1.0f;
    // A different flash every shot: rolled to a new angle and squashed a different way. Gas leaves a
    // barrel unevenly, and a flash that is identical each time is the thing the eye picks out as a
    // repeated picture rather than as fire.
    m_body.SetMuzzleFlashShape(
        glm::two_pi<float>() * static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX),
        0.78f + 0.44f * static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX));
    // Your own shot is played flat in both ears rather than placed in the world. It comes from a
    // weapon a forearm's length from your face: positioning it means panning it hard to whichever
    // side the barrel is on, which is both wrong and the most obvious way a mix sounds broken.
    for (size_t i = 0; i < m_shots.size(); ++i)
    {
        PlaySound(GunshotFor(EquippedWeapon()).Pick(), MuzzlePosition(), 0.85f, 1.0f, false);
        // And the brass, a moment later, at your feet.
        QueueSound(0.35f + 0.3f * static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX), "Weapons/casing",
                   m_player.State().position + glm::vec3(0.35f, 0.05f, 0.0f), 0.35f);
    }

    for (const FireEvent& shot : m_shots)
    {
        if (m_sessionMode == SessionMode::Client)
        {
            // Fired for feel and sent up. The tracer is drawn straight away because a weapon that
            // waits a round trip to go off feels broken; what it actually hit is the host's answer.
            ShotMessage message;
            message.shotNumber = shot.sequence;
            message.origin = shot.origin;
            message.direction = shot.direction;
            // The moment this client was drawing everyone else at, so the host can test the shot
            // against what was actually on screen when the trigger went down.
            message.renderTick = m_client.RenderTick();
            m_client.SendShot(message);

            ShotResult predicted = ResolveShot(physics, shot);
            // A creature here is only shown, but it is where the host's is, so a round that finds
            // it has found flesh: no hole, the same as the host will decide.
            if (CreatureForBody(predicted.body) != nullptr || NestForBody(predicted.body) >= 0)
            {
                predicted.surface = false;
            }
            Tracer tracer;
            // Traced from the eye so that what is under the crosshair is hit, drawn from the muzzle
            // so it looks like it came out of the gun. Those are different points and the round is
            // entitled to both.
            tracer.from = DrawnMuzzle();
            tracer.origin = shot.origin;
            tracer.to = predicted ? predicted.position : shot.origin + shot.direction * shot.range;
            tracer.hit = predicted.hit;
            tracer.surface = predicted.surface;
            tracer.body = predicted.body;
            tracer.normal = predicted.normal;
            AnchorTracer(tracer);
            m_tracers.push_back(tracer);
            PlayImpact(tracer);
            continue;
        }

        ShotResult result = ResolveShot(physics, shot);
        ResolvePlayerHits(shot, LocalPlayerId(), result, PosesNow());
        const bool struckCreature = OnShotResolved(result, shot.origin, LocalPlayerId());

        Tracer tracer;
        tracer.from = DrawnMuzzle();
        tracer.origin = shot.origin;
        tracer.to = result ? result.position : shot.origin + shot.direction * shot.range;
        tracer.hit = result.hit;
        tracer.surface = result.surface;
        tracer.body = result.body;
        tracer.normal = result.normal;
        AnchorTracer(tracer);
        m_tracers.push_back(tracer);
        PlayImpact(tracer);

        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::ShotFired;
            event.player = 0;
            event.position = DrawnMuzzle();
            event.direction = tracer.to;
            event.flag = tracer.hit;
            event.flag2 = tracer.surface;
            m_host.Broadcast(event);
        }

        if (!result)
        {
            continue;
        }
        // Anything loose is shoved by what hit it. The creature has already taken its damage, and is
        // not something a round pushes about.
        if (!struckCreature && physics.IsValid(result.body))
        {
            physics.AddImpulse(result.body, shot.direction * (result.damage * 0.35f));
        }
    }
}

void PredationGame::TakeAmmunition(int crateIndex)
{
    if (EquippedWeapon() == nullptr)
    {
        m_app->GetConsole().Print("Nothing to load");
        return;
    }
    if (!CanTakeAmmunition())
    {
        m_app->GetConsole().Print("Already carrying full spare magazines");
        return;
    }
    if (!m_world.DrawFromAmmoCrate(crateIndex, m_interactions))
    {
        m_app->GetConsole().Print("The crate is empty");
        return;
    }
    GiveAmmunition(crateIndex);
}

// Whether this player could take anything from a crate at all, which is worth knowing before asking the
// host for some: a client that asks when it is already full spends a crate's worth of nothing.
bool PredationGame::CanTakeAmmunition() const
{
    const WeaponDefinition* definition = EquippedWeapon();
    return definition != nullptr && m_weapon.reserve < definition->reserveOnPickup;
}

// The rounds themselves, once whoever decides has decided. On the host this follows straight on from
// taking them; on a client it happens when the host says so.
void PredationGame::GiveAmmunition(int crateIndex)
{
    const WeaponDefinition* definition = EquippedWeapon();
    if (definition == nullptr || m_weapon.reserve >= definition->reserveOnPickup)
    {
        return;
    }

    // Fills the spare rounds, not the magazine. What is in the weapon still has to be reloaded, so
    // resupplying never doubles as a free reload in the middle of a fight.
    const int taken = definition->reserveOnPickup - m_weapon.reserve;
    m_weapon.reserve = definition->reserveOnPickup;
    if (const WorldObjects::AmmoCrate* crate = m_world.GetAmmoCrate(crateIndex); crate != nullptr)
    {
        if (const Transform* where = m_scene.GetTransform(crate->entity); where != nullptr)
        {
            PlaySound(m_sounds.pickup.Pick(), where->position, 0.55f, 0.85f);
        }
    }
    m_app->GetConsole().Print("Took " + std::to_string(taken) + " rounds for the " + definition->name);
    PRED_LOG_INFO(Gameplay, "Resupplied {} rounds from crate {}", taken, crateIndex);
}

void PredationGame::AnchorTracer(Tracer& tracer) const
{
    tracer.anchored = false;
    const PhysicsWorld& physics = m_app->GetPhysics();
    if (!tracer.hit || !tracer.body.IsValid() || !physics.IsValid(tracer.body) ||
        physics.MotionOf(tracer.body) == BodyMotion::Static)
    {
        return;
    }
    // Taken now, before the shove from this same round has had a physics step to act on it.
    const Transform body = physics.GetTransform(tracer.body);
    const glm::quat inverse = glm::inverse(body.rotation);
    tracer.localTo = inverse * (tracer.to - body.position);
    tracer.localNormal = inverse * tracer.normal;
    tracer.anchored = true;
}

void PredationGame::PlaceBulletHole(const glm::vec3& at, const glm::vec3& normal, BodyHandle on)
{
    // Laid onto the surface it hit, wrapped round it.
    //
    // It used to be one flat disc on the tangent plane at the point of impact, which is exact on a
    // wall and a flat plate stuck to anything round. The level's pillars and its sphere are triangle
    // meshes, so the normal a round finds on them is the normal of the one facet it struck, and near
    // the top of the sphere that facet is nearly level: the mark lay flat as a table while the
    // surface fell away round it. Every vertex is now dropped onto the real surface under it, so the
    // mark bends with the curve and shrinks at an edge instead of hanging off it.
    if (m_bulletHoles.empty() || m_bulletHoleMeshes.size() != m_bulletHoles.size())
    {
        return;
    }

    const size_t slot = m_nextBulletHole;
    const Entity hole = m_bulletHoles[slot];
    m_nextBulletHole = (m_nextBulletHole + 1) % m_bulletHoles.size();

    Transform* transform = m_scene.GetTransform(hole);
    MeshRenderer* renderer = m_scene.GetMeshRenderer(hole);
    if (transform == nullptr || renderer == nullptr)
    {
        return;
    }

    // Turned to a different angle each time and sized a little differently, so a wall somebody has
    // emptied a magazine into does not read as the same stamp repeated.
    const float spin = glm::two_pi<float>() * static_cast<float>(std::rand()) /
                       static_cast<float>(RAND_MAX);
    const float size = 0.85f + 0.3f * static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);

    PhysicsWorld& physics = m_app->GetPhysics();
    const BulletHolePlacement placed =
        ConformBulletHole(m_bulletHoleShape, physics, at, normal, spin, size);
    renderer->mesh = m_app->GetMeshes().Replace(m_bulletHoleMeshes[slot], placed.mesh);
    transform->position = placed.position;
    transform->rotation = placed.rotation;
    // The size is in the vertices now, because it had to be before they were dropped onto the
    // surface: scaling a wrapped mark afterwards would lift its edge off the curve it was fitted to.
    transform->scale = glm::vec3(1.0f);
    renderer->visible = renderer->mesh.IsValid();

    // And what it is stuck to, so it goes where that goes.
    //
    // A wall never moves and needs nothing. A crate, a dropped weapon or anything else the physics
    // simulates does, and a mark left at the world position the round arrived at simply hangs there
    // once the thing slides out from under it. Kept in the body's own frame, so following it is a
    // multiply rather than a running correction.
    HoleAttachment& attachment = m_bulletHoleAttachments[slot];
    attachment = HoleAttachment{};
    if (on.IsValid() && physics.IsValid(on) && physics.MotionOf(on) != BodyMotion::Static)
    {
        const Transform body = physics.GetTransform(on);
        const glm::quat inverse = glm::inverse(body.rotation);
        attachment.body = on;
        attachment.localPosition = inverse * (transform->position - body.position);
        attachment.localRotation = inverse * transform->rotation;
    }
}

// Every hole that is stuck to something, moved to where that something is now.
//
// Run every frame rather than on a change, because a physics body does not announce that it has
// moved. Ninety-six transforms is nothing next to the draw that follows it, and the alternative --
// asking each body whether it has settled -- costs more than doing the work.
void PredationGame::FollowBulletHoles()
{
    const PhysicsWorld& physics = m_app->GetPhysics();
    for (size_t i = 0; i < m_bulletHoles.size() && i < m_bulletHoleAttachments.size(); ++i)
    {
        HoleAttachment& attachment = m_bulletHoleAttachments[i];
        if (!attachment.body.IsValid())
        {
            continue;
        }
        MeshRenderer* renderer = m_scene.GetMeshRenderer(m_bulletHoles[i]);
        Transform* transform = m_scene.GetTransform(m_bulletHoles[i]);
        if (renderer == nullptr || transform == nullptr || !renderer->visible)
        {
            continue;
        }
        // The thing it was on has been destroyed -- a pickup taken, a prop cleared. The mark goes
        // with it rather than being left behind in the air, which is the same fault by another road.
        if (!physics.IsValid(attachment.body))
        {
            renderer->visible = false;
            attachment = HoleAttachment{};
            continue;
        }
        const Transform body = physics.GetTransform(attachment.body);
        transform->position = body.position + body.rotation * attachment.localPosition;
        transform->rotation = body.rotation * attachment.localRotation;
    }
}

// Every hole gone, for a new game.
//
// The pool is made once and reused, so nothing about starting a game touched it and a fresh match
// opened with the last one's gunfire still written on the walls.
void PredationGame::ClearBulletHoles()
{
    for (const Entity hole : m_bulletHoles)
    {
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(hole))
        {
            renderer->visible = false;
        }
    }
    m_bulletHoleAttachments.assign(m_bulletHoles.size(), HoleAttachment{});
    m_nextBulletHole = 0;
}


} // namespace pred
