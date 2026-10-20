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


// Playing together: hosting, joining and the lobby; keeping everybody's world and players the same.

namespace pred
{

bool PredationGame::PlayerPositionIfKnown(uint8_t player, glm::vec3& out) const
{
    if (player == LocalPlayerId())
    {
        out = m_player.State().position;
        return true;
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.id == player)
        {
            out = remote.position;
            return true;
        }
    }
    return false;
}

glm::vec3 PredationGame::PlayerPosition(uint8_t player) const
{
    glm::vec3 where{0.0f};
    return PlayerPositionIfKnown(player, where) ? where : m_player.State().position;
}

void PredationGame::ServeClientRequests()
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }

    for (const NetHost::InteractRequest& request : m_host.TakeInteractRequests())
    {
        // Every kind there is. This stopped at the ammunition crate, which left the cocoon -- added after it --
        // out: a guest could never cut anybody free.
        if (request.kind > static_cast<uint8_t>(InteractionKind::Deploy))
        {
            continue;
        }
        const auto kind = static_cast<InteractionKind>(request.kind);

        // A client says what it wants, never where it is. The host checks the distance itself
        // against the position it simulated, so reach cannot be claimed.
        const Interactable* target = m_interactions.FindByPayload(kind, request.index);
        if (target == nullptr || !target->enabled)
        {
            continue;
        }
        const Transform* transform = m_scene.GetTransform(target->entity);
        if (transform == nullptr)
        {
            continue;
        }
        const glm::vec3 focus = transform->position + target->focusOffset;
        const glm::vec3 from = PlayerPosition(request.player) + glm::vec3(0.0f, 1.2f, 0.0f);
        // A little slack over the prompt's own range, because the client asked at a position the
        // host has since moved them on from.
        if (glm::distance(focus, from) > target->range + 1.0f)
        {
            PRED_LOG_DEBUG(Network, "Player {} asked for something out of reach", request.player);
            continue;
        }
        PerformInteraction(kind, request.index, request.player);
    }

    for (const NetHost::ShotRequest& request : m_host.TakeShotRequests())
    {
        // The client has already played the shot for itself. The host decides what it hit, with the
        // weapon the client is holding -- not the host's own, which is what it used to use: a client
        // with a carbine fired pistol rounds whenever the host had a pistol out.
        const WeaponDefinition* definition = WeaponHeldBy(request.player);
        FireEvent shot;
        shot.origin = request.shot.origin;
        shot.direction = glm::normalize(request.shot.direction);
        shot.range = definition != nullptr ? definition->range : 60.0f;
        shot.damage = definition != nullptr ? definition->damage : 20.0f;
        shot.sequence = request.shot.shotNumber;

        ShotResult result = ResolveShot(m_app->GetPhysics(), shot);
        // Rewound to the moment this client says it was looking at, clamped by the host to what a
        // playable connection could justify.
        ResolvePlayerHits(shot, request.player, result, m_host.PosesAt(request.shot.renderTick));
        OnShotResolved(result, shot.origin, request.player);

        WorldEventMessage event;
        event.kind = WorldEventKind::ShotFired;
        event.player = request.player;
        // The trace starts at the eye, so that what is under the crosshair is what gets hit. The
        // drawn line has to start at the muzzle instead, or everyone else watches rounds come out
        // of the shooter's face. Where their muzzle is, is where this machine is drawing their gun.
        event.position = MuzzleOf(request.player, shot.origin, shot.direction);
        event.direction = result ? result.position : shot.origin + shot.direction * shot.range;
        event.flag = result.hit;
        event.flag2 = result.surface;
        m_host.Broadcast(event);

        Tracer tracer;
        // The host has the eye this was traced from, because the client sent it.
        tracer.from = event.position;
        tracer.origin = shot.origin;
        tracer.to = event.direction;
        tracer.hit = event.flag;
        tracer.surface = event.flag2;
        tracer.normal = SurfaceNormalAt(m_app->GetPhysics(), tracer.origin, tracer.to, &tracer.body);
        AnchorTracer(tracer);
        m_tracers.push_back(tracer);
        PlayImpact(tracer);
        // And heard. Every other machine plays this when the event reaches it, but the host is not
        // sent its own broadcast, and so for as long as there has been multiplayer the host has
        // watched other people fire in silence.
        PlaySound(GunshotFor(definition).Pick(), event.position, 1.0f, 1.0f, true);
        QueueSound(0.4f, "Weapons/casing", PlayerPosition(request.player) + glm::vec3(0.0f, 0.05f, 0.0f), 0.4f);

        // The host draws the shooter too, so their weapon has to kick here as well. The event goes
        // out to everybody else; nobody sends it back to the machine that made it.
        if (RemoteAvatar* avatar = AvatarFor(request.player))
        {
            avatar->weaponKick = 1.0f;
        }
    }

    for (NetHost::DropRequest request : m_host.TakeDropRequests())
    {
        // Only what the host actually handed them. Otherwise dropping is a way to make items out of
        // nothing, which is what let one get duplicated.
        if (!m_host.TakeCarried(request.player, request.drop.item, request.drop.count))
        {
            continue;
        }
        // Where they asked, but not through a wall from where they stand.
        request.drop.position = ClearDropPoint(PlayerPosition(request.player) + glm::vec3(0.0f, 1.55f, 0.0f), request.drop.position);
        const int index = m_world.SpawnPickup(
            m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
            static_cast<ItemId>(request.drop.item), request.drop.count, request.drop.position,
            request.drop.velocity, LoadFromWire(request.drop.rounds),
            LoadFromWire(request.drop.reserve));
        if (index < 0)
        {
            continue;
        }
        m_host.Broadcast(PickupSpawnedEvent(static_cast<uint8_t>(index), request.drop.item,
                                            request.drop.count, request.drop.rounds,
                                            request.drop.reserve, request.drop.position,
                                            request.drop.velocity));
    }

    for (const NetHost::ItemUseRequest& request : m_host.TakeItemUses())
    {
        HandleItemUse(request.player, request.use);
    }

    m_host.SetVoiceEverywhere(m_cine.Active());

    for (const NetHost::LoadoutRequest& request : m_host.TakeLoadouts())
    {
        ServeLoadout(request.player, request.kit);
    }

    // The world's console commands, sent by a client (developer builds): run here, for them, where they stand. Only the
    // ones that are the world's -- a client does not get to run anything it likes on somebody else's machine.
    for (const NetHost::CommandRequest& request : m_host.TakeCommands())
    {
#if PRED_DEV_TOOLS
        static const char* const kWorldCommands[] = {"spawn_creature", "grab_me",   "creature_clear", "nest_here",
                                                     "creature_climb", "nest_grow", "nest_hurt"};
        const std::string name = request.line.substr(0, request.line.find(' '));
        if (std::find_if(std::begin(kWorldCommands), std::end(kWorldCommands), [&](const char* world) { return name == world; }) ==
            std::end(kWorldCommands))
        {
            PRED_LOG_WARN(Network, "Player {} asked to run '{}', which is not one of the world's commands", request.player, name);
            continue;
        }
        PRED_LOG_INFO(Network, "Running '{}' for player {}", request.line, request.player);
        m_commandFrom = request.player;
        m_app->GetConsole().Execute(request.line);
        m_commandFrom = 0xFF;
#else
        (void)request;
#endif
    }

    for (const uint8_t player : m_host.TakeJoined())
    {
        // The game's rules first: they are the host's, whatever the newcomer has set for themselves. And the campaign, so
        // the lobby can say what they are joining.
        SendRules(player);
        SendCampaign(player);
        // Put where everybody is now -- the host's idea of where to put a newcomer was decided when it started, which
        // can be before it went anywhere -- and then told everything else.
        const glm::vec3 place = ArrivalFor(player);
        m_host.RespawnPlayer(player, place);
        WorldEventMessage event;
        event.kind = WorldEventKind::PlayerRespawned;
        event.player = player;
        event.position = place;
        m_host.SendTo(player, event);
        SendWorldToPlayer(player);
    }

    // Whatever somebody walked out with goes back on the floor where they were standing. It came
    // out of the world and it belongs to the world: a player who quits holding the keycard
    // otherwise takes it with them, and everybody else is left looking at an item hanging in the
    // air where their body used to be.
    for (const NetHost::Departure& departure : m_host.TakeDeparted())
    {
        for (const auto& [item, count] : departure.carried)
        {
            const int index = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(),
                                                  m_interactions, m_items, static_cast<ItemId>(item),
                                                  count, departure.position + glm::vec3(0.0f, 0.4f, 0.0f),
                                                  glm::vec3(0.0f));
            if (index < 0)
            {
                continue;
            }
            WorldEventMessage event;
            event.kind = WorldEventKind::PickupSpawned;
            event.index = static_cast<uint8_t>(index);
            event.item = item;
            event.other = static_cast<uint8_t>(count);
            event.position = departure.position + glm::vec3(0.0f, 0.4f, 0.0f);
            m_host.Broadcast(event);
        }
        ForgetPlayer(departure.player);
    }
}

// Everything the world was still holding on that player's behalf, let go of.
//
// A body that leaves takes nothing with it, but the world keeps things on its side of the line: a
// locker with their name on it, a creature with them in its jaws, a cocoon at a nest. None of it
// has an owner any more. The locker is the one that mattered: a player who quit while hidden left
// the spot occupied for the rest of the match, and if they came back they were usually given the
// same player number, so every creature went on believing they were inside a locker across the map
// and looked straight through them for good.
void PredationGame::ForgetPlayer(uint8_t player)
{
    ReleaseGrip(player, "left the game");
    // The map, if they had it.
    if (m_mapHolder == player)
    {
        m_mapHolder = kNobody;
        SendMapHolder();
    }
    m_voiceMemory.Forget(player);
    m_grabImmunity.erase(player);
    const auto wrapped = std::find_if(m_cocoons.begin(), m_cocoons.end(),
                                      [&](const Cocoon& cocoon) { return cocoon.player == player; });
    if (wrapped != m_cocoons.end())
    {
        m_cocoons.erase(wrapped);
    }

    for (size_t i = 0; i < m_world.HidingSpots().size(); ++i)
    {
        WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(static_cast<int>(i));
        if (spot == nullptr || !spot->occupied || spot->occupant != player)
        {
            continue;
        }
        spot->occupied = false;
        spot->occupant = 0;
        m_world.SetDoorOpen(spot->doorIndex, true, m_interactions);
        m_interactions.SetVerb(spot->entity, "Hide in");
        PRED_LOG_INFO(Gameplay, "Player {} left locker {} behind them", player, i);
        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::LockerUsed;
            event.index = static_cast<uint8_t>(i);
            event.player = player;
            event.flag = false;
            m_host.Broadcast(event);
        }
    }
}

void PredationGame::SendWorldToPlayer(uint8_t player)
{
    // Which facility first: every door, locker and item after this is numbered in it.
    {
        WorldEventMessage facility;
        facility.kind = WorldEventKind::FacilityChanged;
        facility.item = m_facility.Seed();
        facility.quiet = true;
        m_host.SendTo(player, facility);
    }
    // And how the mission stands on it: the power, the download, a launch counting down. And where everybody is.
    BroadcastMission(true, player);
    SendShipState(player);
    SendCampaign(player);
    SendMapHolder(player);

    // Somebody who has just walked in has to be told what has already happened, or every door that
    // was opened before they arrived is shut on their screen for the rest of the game.
    for (size_t i = 0; i < m_world.Doors().size(); ++i)
    {
        const WorldObjects::Door& door = m_world.Doors()[i];
        if (!door.IsOpen())
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::DoorMoved;
        event.index = static_cast<uint8_t>(i);
        event.flag = true;
        event.quiet = true;
        m_host.SendTo(player, event);
    }

    // Every slot, in both directions. Telling them only what has been taken leaves out everything
    // that has been dropped since, and leaves a slot that has been taken and then filled with
    // something else showing whatever the map originally put there. Taking first and then spawning
    // is also how a slot gets corrected: a spawn at an occupied index replaces what is there.
    for (size_t i = 0; i < m_world.Pickups().size(); ++i)
    {
        if (m_world.Pickups()[i].alive)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupTaken;
        event.index = static_cast<uint8_t>(i);
        event.player = 0;
        event.quiet = true;
        m_host.SendTo(player, event);
    }

    for (size_t i = 0; i < m_world.Pickups().size(); ++i)
    {
        const WorldObjects::Pickup& pickup = m_world.Pickups()[i];
        if (!pickup.alive)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::PickupSpawned;
        event.index = static_cast<uint8_t>(i);
        event.item = static_cast<uint16_t>(pickup.item);
        event.other = static_cast<uint8_t>(pickup.count);
        event.rounds = LoadToWire(pickup.rounds);
        event.reserve = LoadToWire(pickup.reserve);
        event.position = m_app->GetPhysics().IsValid(pickup.body)
                             ? m_app->GetPhysics().GetTransform(pickup.body).position
                             : pickup.netPosition;
        event.quiet = true;
        m_host.SendTo(player, event);
    }

    for (size_t i = 0; i < m_world.HidingSpots().size(); ++i)
    {
        if (!m_world.HidingSpots()[i].occupied)
        {
            continue;
        }
        WorldEventMessage event;
        event.kind = WorldEventKind::LockerUsed;
        event.index = static_cast<uint8_t>(i);
        event.player = 0;
        event.flag = true;
        event.quiet = true;
        m_host.SendTo(player, event);
    }
    for (size_t i = 0; i < m_world.Doors().size(); ++i)
    {
        if (!m_world.Doors()[i].locked && !m_world.Doors()[i].IsOpen())
        {
            WorldEventMessage unlocked;
            unlocked.kind = WorldEventKind::DoorUnlocked;
            unlocked.index = static_cast<uint8_t>(i);
            unlocked.quiet = true;
            m_host.SendTo(player, unlocked);
        }
    }
    for (const BurningFlare& flare : m_burningFlares)
    {
        if (flare.burn > 0.0f)
        {
            WorldEventMessage burning;
            burning.kind = WorldEventKind::FlareThrown;
            burning.player = 0;
            burning.position = m_app->GetPhysics().GetTransform(flare.body).position + glm::vec3(0.0f, 0.1f, 0.0f);
            burning.amount = flare.burn;
            burning.quiet = true;
            m_host.SendTo(player, burning);
        }
    }
    for (const auto& [holder, left] : m_flareHeldBy)
    {
        WorldEventMessage lit;
        lit.kind = WorldEventKind::ItemUsed;
        lit.player = holder;
        lit.item = static_cast<uint16_t>(m_items.IdOf("flare"));
        lit.index = ItemUseMessage::Finish;
        lit.amount = left;
        lit.quiet = true;
        m_host.SendTo(player, lit);
    }
    // Every nest, as old as it is, and how hurt.
    for (size_t i = 0; i < m_nests.size(); ++i)
    {
        const Nest& nest = m_nests[i];
        WorldEventMessage built;
        built.kind = WorldEventKind::NestBuilt;
        built.index = static_cast<uint8_t>(i);
        built.item = nest.seed;
        built.position = nest.at;
        built.amount = nest.age;
        built.quiet = true;
        m_host.SendTo(player, built);
        if (nest.dead || nest.health < NestWholeHealth())
        {
            WorldEventMessage wounded;
            wounded.kind = WorldEventKind::NestWounded;
            wounded.index = static_cast<uint8_t>(i);
            wounded.amount = nest.dead ? 0.0f : nest.health / NestWholeHealth();
            wounded.quiet = true;
            m_host.SendTo(player, wounded);
        }
    }
}

PredationGame::RemoteAvatar* PredationGame::AvatarFor(uint8_t id)
{
    for (auto& candidate : m_avatars)
    {
        if (candidate->id == id)
        {
            return candidate.get();
        }
    }
    return nullptr;
}

void PredationGame::ApplyWorldEvent(const WorldEventMessage& event)
{
    switch (event.kind)
    {
    case WorldEventKind::DoorMoved:
        m_world.SetDoorOpen(event.index, event.flag, m_interactions);
        // Heard where the door is rather than where the player is, which is the point of a door
        // opening somewhere else in the building.
        if (const WorldObjects::Door* moved = m_world.GetDoor(event.index); moved != nullptr && !event.quiet)
        {
            PlaySound(m_sounds.door.Pick(), moved->hinge, 0.8f);
        }
        break;

    case WorldEventKind::PickupTaken:
    {
        // This is the host answering. Asking put nothing in the bag, because asking is all a client
        // does, so the item goes in now. Without this the thing simply vanished: the host took it
        // out of the world and nobody ever gave it to anyone.
        //
        // What it was is read from this machine's own copy rather than sent, because both ends
        // built the same world and have applied the same events, so the index means the same thing
        // on both. Sending it again would be sending something already known.
        const WorldObjects::Pickup* pickup = m_world.GetPickup(event.index);
        if (pickup != nullptr && pickup->alive && event.player == LocalPlayerId())
        {
            // With whatever it was carrying, which is the whole point of a dropped weapon keeping
            // its magazine. This asked for the item and the count and nothing else, so a client
            // picking up a rifle with three rounds left in it got a full one, and a client picking
            // up a full one that the host had emptied got an empty one: the ammunition was the one
            // thing about a pickup that only ever crossed the wire in one direction.
            const int stored = m_inventory.Add(m_items, pickup->item, pickup->count, pickup->rounds,
                                               pickup->reserve);
            if (stored < pickup->count)
            {
                // The bag filled between asking and being answered. Rare, and worth saying out loud
                // rather than losing the difference in silence.
                PRED_LOG_WARN(Gameplay, "Only had room for {} of {}", stored, pickup->count);
            }
        }
        if (!event.quiet)
        {
            PlaySound(m_sounds.pickup.Pick(), event.position, 0.6f, 1.0f, event.player != LocalPlayerId());
        }
        m_world.ConsumePickup(event.index, m_scene, m_app->GetPhysics(), m_interactions);
        break;
    }

    case WorldEventKind::PickupSpawned:
        // At the index the host chose, not one of this machine's own choosing. Everything after
        // this refers to the pickup by that number.
        m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items,
                            static_cast<ItemId>(event.item), static_cast<int>(event.other),
                            event.position, event.direction, LoadFromWire(event.rounds),
                            LoadFromWire(event.reserve), static_cast<int>(event.index));
        if (!event.quiet)
        {
            PlaySound(m_sounds.drop.Pick(), event.position, 0.7f);
        }
        break;

    case WorldEventKind::LockerUsed:
        if (WorldObjects::HidingSpot* spot = m_world.GetHidingSpot(event.index))
        {
            spot->occupied = event.flag;
            spot->occupant = event.flag ? event.player : 0;
            m_world.SetDoorOpen(spot->doorIndex, !event.flag, m_interactions);
            // Including when it is about us.
            //
            // A client asks and does nothing else: the host decides, and the answer comes back as
            // this event. Skipping it when the name matched -- on the reasoning that we had already
            // done it when we asked -- meant a client could never get into a locker at all. It is
            // only the host that has already applied its own, and the host does not receive its own
            // broadcasts.
            if (event.player == LocalPlayerId() && m_sessionMode != SessionMode::Host)
            {
                if (event.flag)
                {
                    EnterHidingSpot(static_cast<int>(event.index));
                }
                else if (m_hidingSpot == static_cast<int>(event.index))
                {
                    LeaveHidingSpot();
                }
            }
            if (!event.flag)
            {
                m_interactions.SetVerb(spot->entity, "Hide in");
            }
        }
        break;

    case WorldEventKind::AmmoTaken:
        // The crate goes down by one wherever it is drawn from. When it was this machine's player who
        // asked, this is also the answer to their request: they only get the rounds here. Without that a
        // client could use a crate all day and never gain a single round -- the host took them out of
        // the crate and nobody put them in the weapon.
        m_world.DrawFromAmmoCrate(event.index, m_interactions);
        if (event.player == LocalPlayerId())
        {
            GiveAmmunition(event.index);
        }
        break;

    case WorldEventKind::ShotFired:
        if (event.player != LocalPlayerId())
        {
            // Somebody else's round. Drawn, never resolved: what it hit was decided by the host.
            Tracer tracer;
            tracer.from = event.position;
            // Their eye never crosses the wire, so the muzzle is the best origin there is.
            tracer.origin = event.position;
            tracer.to = event.direction;
            tracer.hit = event.flag;
            tracer.surface = event.flag2;
            tracer.normal = SurfaceNormalAt(m_app->GetPhysics(), tracer.origin, tracer.to, &tracer.body);
            AnchorTracer(tracer);
            m_tracers.push_back(tracer);
            PlayImpact(tracer);
            // And it is heard where it was fired from, which is most of what tells a player there
            // is somebody else in the building and roughly where.
            PlaySound(GunshotFor(WeaponHeldBy(event.player)).Pick(), event.position, 1.0f, 1.0f, true);
            QueueSound(0.4f, "Weapons/casing", PlayerPosition(event.player) + glm::vec3(0.0f, 0.05f, 0.0f), 0.4f);

            // And their weapon kicks and flashes. A snapshot cannot carry this: firing happens on
            // one frame and snapshots go out on others, so the moment would be missed most times.
            // It rides the shot message instead, which is the one thing that is sent exactly when
            // a round leaves the barrel.
            if (RemoteAvatar* avatar = AvatarFor(event.player))
            {
                avatar->weaponKick = 1.0f;
            }
        }
        break;

    case WorldEventKind::PlayerDamaged:
        if (event.player == LocalPlayerId())
        {
            // The host is the authority on health, so this is set rather than subtracted.
            m_player.State().health = std::max(m_player.State().health - event.amount, 0.0f);
            PlaySound(m_sounds.hurt.Pick(), m_player.State().position, 0.8f, 1.0f, false);
        }
        else
        {
            PlayerSound(event.player, "Player/hurt", 0.8f);
        }
        break;

    case WorldEventKind::PlayerDied:
        if (event.player == LocalPlayerId())
        {
            m_player.State().health = 0.0f;
            m_player.State().alive = false;
            m_respawnTimer = event.amount;
            m_deadForGood = event.flag;
            m_deathImpulse = event.direction;
            PlaySound(m_sounds.death.Pick(), m_player.State().position, 1.0f, 1.0f, false);
        }
        else
        {
            PlayerSound(event.player, "Player/death", 1.0f);
        }
        break;

    case WorldEventKind::NestBuilt:
        // Something built a nest over there. Every machine grows it from the same place on the same
        // clock, so this is all that is ever said about it until somebody shoots it.
        BuildNest(event.position, event.item, 0, false, event.index, event.amount);
        break;

    case WorldEventKind::NestWounded:
        SetNestHealth(event.index, event.amount, event.quiet);
        break;

    case WorldEventKind::Rules:
        ApplyRules(event);
        break;

    case WorldEventKind::MapHeld:
        // Somebody else got to it first: the map is theirs, and ours closes.
        m_mapHolder = event.player;
        if (m_mapOpen && m_mapHolder != LocalPlayerId())
        {
            m_mapOpen = false;
            m_mapHovered = -1;
            m_mapDragButton = -1;
            m_wantMouseCaptured = !m_paused;
            UpdateMouseCapture();
            m_app->GetConsole().Print(NameOfPlayer(m_mapHolder) + " has the navigation map.");
            PRED_LOG_INFO(Gameplay, "{} has the navigation map", NameOfPlayer(m_mapHolder));
        }
        break;

    case WorldEventKind::NestAged:
        if (event.index < m_nests.size())
        {
            m_nests[event.index].age = event.amount;
        }
        break;

    case WorldEventKind::NestsCleared:
        ClearNests();
        break;

    case WorldEventKind::DroneHit:
        OnDroneHitEvent(event);
        break;

    case WorldEventKind::EverybodyDown:
        m_everybodyDownFor = event.amount;
        break;

    case WorldEventKind::Mission:
        ApplyMissionEvent(event);
        break;

    case WorldEventKind::Cinematic:
        // The host has started one (or stopped it): the same one, from the same moment, here.
        if (!event.flag)
        {
            StopCinematic(true);
            break;
        }
        for (const auto& [name, cinematic] : m_cinematics)
        {
            if (SoundKey(name) == event.item)
            {
                ParkVehicles();
                CloseOverlays();
                m_cine.Play(cinematic, CinematicBindingsNow(), m_scene, m_app->GetMeshes(), *this, event.amount);
                m_cineHandBack = 0.0f;
                break;
            }
        }
        break;

    case WorldEventKind::CorpseCarried:
        // The body lying there is in that creature's jaws now.
        if (Corpse* corpse = CorpseNear(event.position, 3.0f); corpse != nullptr)
        {
            corpse->carriedBy = event.index;
            for (const std::unique_ptr<Creature>& creature : m_creatures)
            {
                if (creature->NetId() == event.index)
                {
                    corpse->carryYaw = creature->Yaw();
                }
            }
        }
        break;

    case WorldEventKind::CorpseDropped:
        for (Corpse& corpse : m_corpses)
        {
            if (corpse.carriedBy == static_cast<int>(event.index))
            {
                corpse.carriedBy = -1;
                PlaceCorpse(corpse, event.position, 0.0f);
            }
        }
        break;

    case WorldEventKind::CorpseBitten:
        // The host's creature bit the body there: the same wound here, and the same piece gone.
        if (Corpse* corpse = CorpseNear(event.position, 2.5f); corpse != nullptr)
        {
            if (event.flag)
            {
                TearPart(*corpse, event.index, event.position);
            }
            else if (corpse->marks.size() < 18)
            {
                MarkBite(*corpse, event.index, event.position);
            }
            corpse->meat = event.amount;
        }
        break;

    case WorldEventKind::FacilityChanged:
        // The host has built another facility: the same one, here, from the same seed.
        if (event.item != m_facility.Seed())
        {
            ChangeFacility(event.item);
        }
        break;

    case WorldEventKind::ItemUsed:
        OnItemUsedEvent(event);
        break;

    case WorldEventKind::FlareThrown:
        // Our own was put in the world the moment it left the hand; this is the host's copy of it.
        if (event.player != LocalPlayerId() || event.quiet)
        {
            ThrowFlare(event.player, event.position, event.direction, event.amount, false);
            if (!event.quiet)
            {
                PlayerSound(event.player, "Items/flare_throw", 0.7f);
            }
        }
        break;

    case WorldEventKind::DoorUnlocked:
        if (WorldObjects::Door* door = m_world.GetDoor(event.index); door != nullptr)
        {
            door->locked = false;
            if (!event.quiet)
            {
                PlayNamed("Items/keycard_accept", door->hinge + glm::vec3(0.0f, 1.2f, 0.0f), 0.8f);
            }
        }
        break;

    case WorldEventKind::ShipState:
        m_map = static_cast<MapChoice>(event.index);
        m_shipOrbiting = event.item;
        m_shipReady = event.flag;
        m_shipTravel = event.flag2 ? std::max(event.amount, 0.01f) : 0.0f;
        m_shipTravelTotal = event.flag2 ? std::max(event.direction.x, m_shipTravel) : 0.0f;
        // The orders as the host has them; a briefing joined part way through is joined where it is.
        if (static_cast<OrderState>(event.other) != m_order || event.rounds != m_orderSite)
        {
            const OrderState was = m_order;
            m_order = static_cast<OrderState>(event.other);
            m_orderSite = event.rounds;
            PrepareBriefing(m_orderSite);
            if (m_order == OrderState::Briefing && was != OrderState::Briefing)
            {
                m_briefingAt = event.direction.y;
                m_briefingCue = 0;
                while (m_briefingCue < m_briefingTimeline.cues.size() && m_briefingTimeline.cues[m_briefingCue].at < m_briefingAt - 0.3f)
                {
                    ++m_briefingCue;
                }
            }
        }
        break;

    case WorldEventKind::PlayerRespawned:
        if (event.player == LocalPlayerId())
        {
            RespawnLocalPlayer(event.position);
        }
        break;

    case WorldEventKind::Sound:
        HearSharedSound(event);
        break;

    case WorldEventKind::Count:
        break;
    }
}

void PredationGame::SendDynamicBodies()
{
    // Nobody to send to is the common case for a game somebody opened and then played alone, and
    // walking every loose body in the world to build a packet for an empty room is the most
    // expensive thing hosting does.
    if (m_sessionMode != SessionMode::Host || m_host.ConnectedCount() == 0)
    {
        return;
    }

    // At the snapshot rate, not the tick rate. Sending loose objects sixty times a second doubles
    // what they cost for a value nobody can see change twice in a thirtieth of a second, and it is
    // the largest message the host sends.
    m_worldStateTimer += 1.0f / 60.0f;
    constexpr float kInterval = 1.0f / 30.0f;
    if (m_worldStateTimer < kInterval)
    {
        return;
    }
    m_worldStateTimer = std::fmod(m_worldStateTimer, kInterval);

    // Loose objects are sent as state rather than as events: a crate sliding across the floor is a
    // value that will be sent again in a thirtieth of a second, so losing one costs nothing.
    WorldStateMessage state;
    PhysicsWorld& physics = m_app->GetPhysics();

    for (size_t i = 0; i < m_world.Pickups().size() && state.count < kMaxDynamicBodies; ++i)
    {
        const WorldObjects::Pickup& pickup = m_world.Pickups()[i];
        if (!pickup.alive || !physics.IsValid(pickup.body))
        {
            continue;
        }
        const Transform transform = physics.GetTransform(pickup.body);
        DynamicBodyState& entry = state.bodies[state.count++];
        entry.id = static_cast<uint8_t>(i);
        entry.position = transform.position;
        entry.rotation = transform.rotation;
    }

    m_host.SendWorldState(state);
    // The creatures go at the same rate, for the same reason: they are shown on every other
    // machine, and the showing is only as smooth as what it is shown from.
    SendCreatureState();
}

void PredationGame::ApplyDynamicBodies(const WorldStateMessage& state)
{
    // Recorded, not applied. A client running its own physics for a dropped rifle would disagree
    // with everyone else within a second, so the host is the authority; but the host speaks thirty
    // times a second and the screen draws at least twice that, so setting the transform on arrival
    // drew a falling item in visible steps. WorldObjects::FollowNetworkState eases towards these
    // every frame instead.
    for (uint8_t i = 0; i < state.count; ++i)
    {
        const DynamicBodyState& entry = state.bodies[i];
        m_world.SetNetworkState(entry.id, entry.position, entry.rotation);
    }
}

void PredationGame::UpdateHostLost()
{
    // The host has gone. The game is theirs -- the campaign is saved on their machine and nowhere else -- so it ends here
    // too, and the title says why.
    if (m_sessionMode != SessionMode::Client || !m_client.HostLost())
    {
        return;
    }
    PRED_LOG_INFO(Network, "The host left: back to the title");
    ReturnToTitle();
    m_titlePage = TitlePage::Browse;
    m_titleStatus = "The host left the game.";
}

std::string PredationGame::LobbyName() const
{
    if (m_lobbyName[0] != '\0')
    {
        return m_lobbyName;
    }
    // Named after whoever is hosting, so an unnamed game in a list is still the one thing anybody
    // needs to tell it apart: whose it is.
    return PlayerName() + "'s game";
}

bool PredationGame::LobbyServerConfigured() const
{
    return !LobbyServerAddress().empty() || !g_lobbyOverride.empty();
}

void PredationGame::StartBrowsing()
{
    if (!m_browser.Running() && !m_browser.Start() && m_titleStatus.empty())
    {
        m_titleStatus = m_browser.Message();
    }
}

void PredationGame::StopBrowsing()
{
    m_browser.Stop();
}

void PredationGame::UpdateDiscovery(float frameDeltaSeconds)
{
    // Every frame, wherever the player is. Browsing stops when the menu is left, but hosting does
    // not: people join a game that is already running.
    m_browser.Tick(frameDeltaSeconds);

    // Said in the log when the list changes, not every frame. When somebody says a game is not
    // showing up, the question is whether its beacon never arrived or arrived and was refused, and
    // only the log can tell those apart.
    for (const LanLobby& lobby : m_browser.Lobbies())
    {
        const std::string where = lobby.address + ":" + std::to_string(lobby.port);
        if (std::find(m_seenOnLan.begin(), m_seenOnLan.end(), where) == m_seenOnLan.end())
        {
            m_seenOnLan.push_back(where);
            PRED_LOG_INFO(Network, "Found a game on this network: {} at {} ({}/{}, protocol {})",
                          lobby.name, where, lobby.players, lobby.maxPlayers, lobby.protocol);
        }
    }
    m_seenOnLan.erase(std::remove_if(m_seenOnLan.begin(), m_seenOnLan.end(),
                                     [this](const std::string& where)
                                     {
                                         for (const LanLobby& lobby : m_browser.Lobbies())
                                         {
                                             if (where == lobby.address + ":" +
                                                              std::to_string(lobby.port))
                                             {
                                                 return false;
                                             }
                                         }
                                         PRED_LOG_INFO(Network, "That game is gone: {}", where);
                                         return true;
                                     }),
                      m_seenOnLan.end());

    // The public list: web requests only, no socket.

    UpdateJoining(frameDeltaSeconds);

    const int players =
        1 + (m_sessionMode == SessionMode::Host ? static_cast<int>(m_host.ConnectedCount()) : 0);
    if (m_sessionMode == SessionMode::Host && m_lobby.GetRole() == LobbyClient::Role::Host)
    {
        m_lobby.SetStatus(static_cast<uint8_t>(std::min(players, 15)), m_host.Started(), LobbyName());
        // The router's outside address, once it has agreed to let people in: one more way to reach
        // this machine, for a guest whose own router cannot be punched through.
        if (!m_portsAnnounced && m_ports.Status() == PortMapper::State::Open)
        {
            m_portsAnnounced = true;
            LobbyEndpoint outside;
            if (!IsInsideAddress(m_ports.ExternalAddress()) &&
                LobbyEndpoint::Parse(m_ports.ExternalAddress(), static_cast<uint16_t>(m_hostPort), outside))
            {
                m_lobby.AddCandidate(outside);
            }
        }
        // The host's transport is polled by the session; this reads what it kept aside.
        m_lobby.Poll(frameDeltaSeconds, m_host.GetTransport());
    }

    if (m_beacon.Running())
    {
        LanLobby mine;
        mine.name = LobbyName();
        mine.port = static_cast<uint16_t>(m_hostPort);
        mine.players = static_cast<uint8_t>(std::min(players, 15));
        mine.maxPlayers = kMaxPlayers;
        mine.started = m_sessionMode == SessionMode::Host && m_host.Started();
        mine.protocol = kProtocolVersion;
        m_beacon.Describe(mine);
        m_beacon.Tick(frameDeltaSeconds);
    }
}

void PredationGame::StartHosting()
{
    StopSession();
    StopBrowsing();
    NetHost::Config config;
    config.port = static_cast<uint16_t>(m_hostPort);
    config.name = PlayerName();
    auto transport = CreateUdpTransport();
    transport->SetConditions(m_simulatedConditions);
    if (!m_host.Start(std::move(transport), config, m_app->GetPhysics(), m_player.Config(),
                      m_spawnPoint))
    {
        m_titleStatus = "Could not open port " + std::to_string(m_hostPort) +
                        ". Something else on this machine is probably already using it.";
        return;
    }
    m_sessionMode = SessionMode::Host;
    m_host.SetStarted(false);

    // Every way in at once, so nobody has to know in advance which of them their situation needs.
    // The network beacon, for anybody in the same house.
    if (!m_beacon.Start())
    {
        PRED_LOG_WARN(Network, "Not announcing the game on the network: {}", m_beacon.Message());
    }
    // A code, for everybody else.
    if (LobbyServerConfigured())
    {
        m_lobby.Host(LobbySettings(), LobbyName(), kMaxPlayers,
                     static_cast<uint16_t>(m_hostPort));
    }
    // And the router asked to let people straight in, for a guest whose own router cannot be punched
    // through. Quietly: if it says no, the code still works for almost everybody.
    m_ports.Open(static_cast<uint16_t>(m_hostPort));
    m_portsAnnounced = false;

    m_inLobby = true;
    m_titleStatus.clear();
}

void PredationGame::StartTheGame()
{
    m_inLobby = false;
    if (m_sessionMode == SessionMode::Host)
    {
        // Everybody in the lobby goes in with the host: they are told by the roster, which says the
        // game has started, and each goes in the moment it arrives.
        m_host.SetStarted(true);
    }
    EnterWorld();
}

bool PredationGame::JoinAddress(const std::string& address, int port)
{
    if (address.empty())
    {
        m_titleStatus = "Nothing to join.";
        return false;
    }
    // A join by code has a transport already, and it has to be that one: the lobby server saw it and
    // the holes through both routers were punched for it. Taken before StopSession, which would
    // otherwise throw it away.
    std::unique_ptr<Transport> transport = std::move(m_joinTransport);
    StopSession();
    if (transport == nullptr)
    {
        transport = CreateUdpTransport();
        transport->SetConditions(m_simulatedConditions);
    }

    NetClient::Config config;
    if (!m_client.Connect(std::move(transport), address, static_cast<uint16_t>(port), PlayerName(),
                          config))
    {
        m_titleStatus = "Could not reach " + address + ":" + std::to_string(port) +
                        ". Check they are still hosting and that their firewall is letting the "
                        "game through.";
        return false;
    }

    std::snprintf(m_joinAddress, sizeof(m_joinAddress), "%s", address.c_str());
    m_joinPort = port;
    cv_lastAddress.Set(m_joinAddress);
    cv_lastPort.Set(m_joinPort);
    m_sessionMode = SessionMode::Client;
    // A client predicts where it will be, never whether it is alive.
    m_player.SetDecidesDamage(false);
    m_titleStatus.clear();
    StopBrowsing();
    return true;
}

bool PredationGame::JoinCode(uint32_t code)
{
    if (!LobbyServerConfigured())
    {
        m_titleStatus = "Codes need a lobby server, and none is set. Join by address instead, or set "
                        "the lobby server in Settings.";
        return false;
    }
    StopSession();
    StopBrowsing();
    m_joinTransport = CreateUdpTransport();
    m_joinTransport->SetConditions(m_simulatedConditions);
    if (!m_joinTransport->Open(0))
    {
        m_titleStatus = "Could not open a network port on this machine.";
        m_joinTransport.reset();
        return false;
    }
    m_lobby.Join(LobbySettings(), code, m_joinTransport->LocalPort());
    m_titleStatus.clear();
    return true;
}

void PredationGame::UpdateJoining(float frameDeltaSeconds)
{
    if (m_joinTransport == nullptr || m_lobby.GetRole() != LobbyClient::Role::Guest)
    {
        return;
    }
    // Nothing else polls this transport yet: the game only gets it once the host has answered.
    std::vector<NetPacket> none;
    m_joinTransport->Poll(frameDeltaSeconds, none);
    m_lobby.Poll(frameDeltaSeconds, m_joinTransport.get());
    if (m_lobby.Status() == LobbyClient::State::Reached)
    {
        // Through both routers. From here it is an ordinary join, over the same socket.
        const LobbyEndpoint host = m_lobby.Reached();
        JoinAddress(host.AddressText(), host.port);
    }
}

bool PredationGame::JoinTyped(const std::string& text)
{
    // One box, and the game works out which of the two it was handed. A lobby code is six
    // characters of a known alphabet and nothing else looks like one, so there is nothing to ask
    // the player about.
    uint32_t code = 0;
    if (EncodeLobbyCode(text, code))
    {
        return JoinCode(code);
    }

    // An address, then, with the port on the end of it if there is one. That is how anybody writes
    // one down.
    std::string address = text;
    address.erase(0, address.find_first_not_of(" \t\r\n"));
    address.erase(address.find_last_not_of(" \t\r\n") + 1);
    int port = m_joinPort;
    if (const size_t colon = address.rfind(':'); colon != std::string::npos)
    {
        const int typed = std::atoi(address.c_str() + colon + 1);
        if (typed >= 1024 && typed <= 65535)
        {
            port = typed;
        }
        address = address.substr(0, colon);
    }
    if (address.empty())
    {
        m_titleStatus = "Nothing to join. Paste the code or address they sent you.";
        return false;
    }
    return JoinAddress(address, port);
}

void PredationGame::SendRules(int player)
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }
    WorldEventMessage event;
    event.kind = WorldEventKind::Rules;
    event.flag = GetSettingBool("game.friendly_fire", true);
    if (player >= 0)
    {
        m_host.SendTo(static_cast<uint8_t>(player), event);
    }
    else
    {
        m_host.Broadcast(event);
    }
}

void PredationGame::ApplyRules(const WorldEventMessage& event)
{
    // The host's, for as long as this is its game.
    SetSetting("game.friendly_fire", event.flag ? "true" : "false");
}

std::string PredationGame::PlayerName() const
{
    // Trimmed and never empty. A blank name on somebody else's player list is a row with nothing in
    // it, and a name of pure spaces is the same thing with more effort.
    std::string name = m_playerName;
    const size_t first = name.find_first_not_of(" \t");
    const size_t last = name.find_last_not_of(" \t");
    name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    return name.empty() ? std::string("operator") : name;
}

const std::vector<RemotePlayerView>& PredationGame::RemotePlayers() const
{
    static const std::vector<RemotePlayerView> kNone;
    switch (m_sessionMode)
    {
    case SessionMode::Host: return m_host.Remotes();
    case SessionMode::Client: return m_client.Remotes();
    case SessionMode::Offline: break;
    }
    return kNone;
}

void PredationGame::StopSession()
{
    m_mapHolder = kNobody;
    // The lobby first, while the host's transport still exists to tell the server the code is done.
    // A code that went on working for fifteen seconds after its game closed would send whoever typed
    // it to nothing.
    m_lobby.Close();
    m_joinTransport.reset();
    m_inLobby = false;
    if (m_sessionMode == SessionMode::Host)
    {
        m_host.Stop();
    }
    else if (m_sessionMode == SessionMode::Client)
    {
        m_client.Disconnect();
    }
    // And shut the door the router was asked to open. A forwarded port pointing at a machine that
    // is no longer listening is worse than none.
    m_ports.Close();

    // Stop announcing, whichever end this was. A beacon still going for a game that has ended is a
    // row in everybody's list that fails when it is clicked, which is worse than not being there.
    m_beacon.Stop();
    for (auto& avatar : m_avatars)
    {
        avatar->body.Destroy(m_scene);
    }
    m_avatars.clear();
    m_sessionMode = SessionMode::Offline;
    // On its own again, so the player is once more the authority on its own health.
    m_player.SetDecidesDamage(true);
    m_networkTick = 0;
}

bool PredationGame::StepSession(const PlayerInput& input, float dt)
{
    ++m_networkTick;

    // What is in the local player's hands, so everyone else sees it.
    const ItemDefinition* held = m_items.Get(m_inventory.Selected().item);
    const uint8_t heldId = held != nullptr ? static_cast<uint8_t>(held->id) : 0;
    // A fraction of the reload, not the seconds left of it. See the note on the client's copy of
    // this below: the two were wrong in the same way and for the same reason.
    const float reloadPlay = ReloadProgress();
    // During a reload, whether it began from empty; otherwise whether the magazine is empty now, which
    // is what holds a slide back on everybody else's screen.
    const bool emptyNow = m_weapon.IsReloading() ? m_weapon.reloadFromEmpty
                                                 : (m_weapon.rounds <= 0 && EquippedWeapon() != nullptr);

    if (m_sessionMode == SessionMode::Host)
    {
        m_host.SetPlayerHeld(0, heldId, m_weapon.aim, m_weapon.IsReloading(), reloadPlay, emptyNow);
        m_host.SetPlayerTorch(0, m_torchOn);

        // The host is a player too: it steps itself first, then runs everyone else from what they
        // sent, then tells them all where everybody ended up.
        m_player.Step(input, dt);
        m_host.Tick(m_networkTick, m_player.State(), dt);
        ServeClientRequests();
        SendDynamicBodies();
        return true;
    }

    if (m_sessionMode == SessionMode::Client)
    {
        // Told to the host with the next input, or nobody else ever sees this player holding
        // anything: the host cannot see inside another machine.
        // How far through the reload is, as a fraction. It used to send one minus the seconds left,
        // which is a fraction only for a reload that takes exactly one second: with a 2.2 second
        // one it stayed at zero until the last second and then ran the whole movement in it, so
        // everyone else saw the reload start when it was nearly over and play at twice the speed.
        m_client.SetHeld(heldId, m_weapon.aim, m_weapon.IsReloading(), reloadPlay, emptyNow);
        m_client.SetTorch(m_torchOn);

        // The client's step happens inside prediction, so the same call is used for the first guess
        // and for every replay of it. Doing it here as well would run each input twice.
        m_client.Tick(input, m_player, dt);
        for (const WorldEventMessage& event : m_client.TakeWorldEvents())
        {
            ApplyWorldEvent(event);
        }
        if (m_client.HasWorldState())
        {
            ApplyDynamicBodies(m_client.LatestWorldState());
        }
        if (m_client.CreatureStatesReceived() != m_appliedCreatureStates)
        {
            m_appliedCreatureStates = m_client.CreatureStatesReceived();
            ApplyCreatureState(m_client.LatestCreatureState());
        }
        return true;
    }

    return false;
}

void PredationGame::SyncRemoteAvatars(float frameDeltaSeconds)
{
    const std::vector<RemotePlayerView>& remotes = RemotePlayers();

    // Anyone who has gone gets their body taken away.
    for (size_t i = 0; i < m_avatars.size();)
    {
        const uint8_t id = m_avatars[i]->id;
        const bool present = std::any_of(remotes.begin(), remotes.end(),
                                         [&](const RemotePlayerView& view) { return view.id == id; });
        if (present)
        {
            ++i;
        }
        else
        {
            m_avatars[i]->body.Destroy(m_scene);
            m_avatars.erase(m_avatars.begin() + static_cast<ptrdiff_t>(i));
        }
    }

    const PlayerConfig& config = m_player.Config();
    for (const RemotePlayerView& remote : remotes)
    {
        RemoteAvatar* avatar = nullptr;
        for (auto& candidate : m_avatars)
        {
            if (candidate->id == remote.id)
            {
                avatar = candidate.get();
                break;
            }
        }
        if (avatar == nullptr)
        {
            auto fresh = std::make_unique<RemoteAvatar>();
            fresh->id = remote.id;
            fresh->body.Build(m_scene, m_app->GetMeshes(), config);
            fresh->body.SetTextureLibrary(m_app->GetTextures());
            // Other people have heads. The body hides its own by default because in first person
            // the camera lives inside it, which is true of exactly one body on this machine at a
            // time, and while spectating it is theirs rather than yours. Set below, every frame.
            fresh->body.Tuning().hideHead = false;
            fresh->view.eyeHeight = config.EyeHeightForStance(remote.stance);
            fresh->built = true;
            m_avatars.push_back(std::move(fresh));
            avatar = m_avatars.back().get();
        }

        // Only where they are and what they are doing crosses the wire. The walk cycle, the lean,
        // the arms and the head all come out of the same procedural body the local player uses, run
        // here from replicated state. A gait is expensive to send and cheap to reproduce.
        // Eased towards where they are rather than set to it. The host rebuilds what it publishes
        // about everyone else once per simulation tick and the screen draws whenever it likes, so
        // taking that position straight moves a body in steps; from inside their head it is the
        // camera jumping back and forth, and the body it is attached to smears with it. A long way
        // out means a respawn or a teleport, where arriving beats sliding across the room.
        if (!avatar->drawnValid || glm::distance(avatar->drawn, remote.position) > 2.0f)
        {
            avatar->drawn = remote.position;
            avatar->drawnValid = true;
        }
        else
        {
            const float blend = 1.0f - std::exp(-26.0f * frameDeltaSeconds);
            avatar->drawn = glm::mix(avatar->drawn, remote.position, blend);
        }

        // Where they are looking, eased for the same reason their position is.
        //
        // On the host, what it publishes about everybody else is rebuilt once per simulation tick
        // while the screen draws whenever it likes, so the angles arrive in steps even though the
        // position no longer does. On a body across the room that is barely visible; from inside
        // their head, spectating, it is the whole view jumping sixty times a second. The short way
        // round for the yaw, or looking across the back of north spins the camera all the way about.
        {
            const float blend = 1.0f - std::exp(-26.0f * frameDeltaSeconds);
            if (!avatar->lookValid)
            {
                avatar->lookYaw = remote.yaw;
                avatar->lookPitch = remote.pitch;
                avatar->lookValid = true;
            }
            else
            {
                float delta = remote.yaw - avatar->lookYaw;
                while (delta > glm::pi<float>()) delta -= glm::two_pi<float>();
                while (delta < -glm::pi<float>()) delta += glm::two_pi<float>();
                avatar->lookYaw += delta * blend;
                avatar->lookPitch += (remote.pitch - avatar->lookPitch) * blend;
            }
        }

        avatar->state.position = avatar->drawn;
        avatar->state.velocity = remote.velocity;
        avatar->state.yaw = avatar->lookYaw;
        avatar->state.pitch = avatar->lookPitch;
        avatar->state.stance = remote.stance;
        avatar->state.desiredStance = remote.stance;
        avatar->state.leanAmount = remote.leanAmount;
        avatar->state.stridePhase = remote.stridePhase;
        avatar->state.health = remote.health;
        avatar->state.alive = remote.alive;
        avatar->state.grounded = remote.grounded;
        // The climb, so a remote player is seen hauling themselves over rather than sliding up a
        // wall. The duration is nominal here: only the phase matters for the pose.
        //
        // The edge and the phase are remembered rather than read fresh every frame. The snapshot
        // carries them only while somebody is climbing, and the hands go on fading off the ledge
        // for a moment after they stop; taking the wire's zero during that fade dragged their
        // weapon towards the world origin at full strength and then let it snap back.
        if (remote.mantling)
        {
            avatar->mantleEdge = remote.mantleEdge;
            avatar->mantlePhase = remote.mantlePhase;
        }
        else
        {
            avatar->mantlePhase = 1.0f;
        }
        avatar->state.mantling = remote.mantling;
        avatar->state.swimming = remote.swimming;
        avatar->state.mantleDuration = 1.0f;
        avatar->state.mantleTime = avatar->mantlePhase;
        avatar->state.mantleEdge = avatar->mantleEdge;
        avatar->state.mantleFrom = avatar->drawn;
        avatar->state.mantleTo = avatar->drawn;

        // The eye eases towards the stance's height rather than being set to it, exactly as the
        // local player's own view does. The body is anchored to the eye, so setting it outright
        // dropped a remote player straight into a crouch in one frame while their own screen
        // showed them sinking into it.
        const float targetEye = remote.swimming ? config.swimEyeHeight : config.EyeHeightForStance(remote.stance);
        avatar->view.eyeHeight +=
            (targetEye - avatar->view.eyeHeight) *
            (1.0f - std::exp(-config.eyeTransitionSpeed * frameDeltaSeconds));

        avatar->view.renderPosition = avatar->drawn;
        avatar->view.eyePosition = avatar->drawn + glm::vec3(0.0f, avatar->view.eyeHeight, 0.0f);

        // Leaning moves the eye out sideways, and the body is anchored to the eye, so a remote
        // player who is leaning has to have their eye moved the same way here. Without it they saw
        // round a corner while their body stayed squarely behind it: they could see you and you
        // could not see them, which is the worst thing a peek can be.
        const glm::vec3 leanRight{std::cos(remote.yaw), 0.0f, std::sin(remote.yaw)};
        avatar->view.eyePosition += leanRight * (remote.leanAmount * config.leanSideOffset);
        // Their torch, which only the wire can say: nothing about a body implies it.
        avatar->torchOn = remote.torchOn;
        avatar->view.yaw = remote.yaw;
        avatar->view.pitch = remote.pitch;
        avatar->view.leanRoll = glm::radians(config.leanAngleDegrees) * remote.leanAmount;

        // Put in their hands what the snapshot says they are holding. This was replicated and then
        // never used, which is why everybody else appeared empty-handed however obviously they were
        // carrying a rifle.
        if (avatar->heldItem != remote.heldItem)
        {
            avatar->heldItem = remote.heldItem;
            const ItemDefinition* item = m_items.Get(static_cast<ItemId>(remote.heldItem));
            const WeaponDefinition* weapon =
                item != nullptr ? m_weaponData.Get(m_weaponData.ForItem(item->key)) : nullptr;

            avatar->body.SetWeapon(m_scene, m_app->GetMeshes(), weapon);
            if (weapon == nullptr && item != nullptr)
            {
                avatar->body.SetHeldItem(m_scene, m_app->GetMeshes(), item->key,
                                         ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
                avatar->body.SetHeldItemPlacement(item->holdOffset, item->holdRotation, item->holdHand);
            }
            else
            {
                avatar->body.ClearHeldItem(m_scene);
            }
            // Brought up rather than appearing already shouldered.
            avatar->weaponDraw = 0.0f;
        }

        // Decayed per frame, exactly as the local one is. Firing is an event rather than a state,
        // so it arrives through the shot message and fades from there; without it, everybody else's
        // weapon fired with no flash and no recoil, which is why shots from other players read as
        // coming from nowhere.
        UpdateRemoteItemUse(*avatar, remote.id, frameDeltaSeconds);
        avatar->weaponKick = std::max(avatar->weaponKick - frameDeltaSeconds * 7.0f, 0.0f);
        avatar->weaponDraw = std::min(avatar->weaponDraw + frameDeltaSeconds * 3.2f, 1.0f);

        PlayerBody::WeaponPose weaponPose;
        weaponPose.aim = remote.aim;
        weaponPose.reloading = remote.reloading;
        weaponPose.reload = remote.reloadProgress;
        weaponPose.reloadEmpty = remote.reloadEmpty;
        weaponPose.emptyHold = !remote.reloading && remote.reloadEmpty;
        weaponPose.kick = avatar->weaponKick;
        weaponPose.draw = avatar->weaponDraw;
        avatar->body.SetWeaponPose(weaponPose);

        // Somebody else going down collapses the same way, from the state the host sent.
        if (!remote.alive && !avatar->collapsed)
        {
            avatar->body.Collapse(remote.velocity * 0.5f + glm::vec3(0.0f, 1.0f, 0.0f));
            avatar->collapsed = true;
        }
        else if (remote.alive && avatar->collapsed)
        {
            LeaveCorpse(avatar->body, remote.id);
            avatar->body.Revive();
            avatar->collapsed = false;
        }

        // Spectating puts the camera in their head, so their head has to come off, exactly as your
        // own does when you are alive. Without it the view spends the whole time inside a skull.
        avatar->body.Tuning().hideHead = !m_player.State().alive &&
                                         m_cameraMode == CameraMode::FirstPerson &&
                                         static_cast<int>(avatar->id) == m_spectating;

        avatar->body.Update(m_scene, avatar->state, avatar->view, config, m_app->GetPhysics(),
                            frameDeltaSeconds);
    }
}

void PredationGame::DrawNetworkPanel()
{
    ImGui::SetNextWindowPos(ImVec2(420.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Network"))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Session: %s", SessionModeName(m_sessionMode));

    Transport* transport = m_sessionMode == SessionMode::Host     ? m_host.GetTransport()
                           : m_sessionMode == SessionMode::Client ? m_client.GetTransport()
                                                                  : nullptr;

    if (m_sessionMode == SessionMode::Offline)
    {
        ImGui::TextWrapped("Single player. Use net_host in the console to open a game, or net_join "
                           "<address> to enter one.");
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        ImGui::Text("Clients: %d of %d", static_cast<int>(m_host.ConnectedCount()), kMaxPlayers - 1);
        ImGui::Text("Starved ticks: %u", m_host.StarvedTicks());
        ImGui::TextDisabled("A starved tick is one where a client's input had not arrived and the "
                            "host repeated the last one.");
    }
    else
    {
        ImGui::Text("%s", m_client.Connected() ? "Connected" : "Joining");
        ImGui::Text("Player %u, tick %u", m_client.PlayerId(), m_client.Sequence());
        ImGui::Text("Corrections: %u", m_client.CorrectionCount());
        const ReconciliationResult& last = m_client.LastReconciliation();
        ImGui::Text("Last error: %.3f m over %u replayed ticks", static_cast<double>(last.errorDistance),
                    last.replayedTicks);
        ImGui::Text("Hidden offset: %.3f m", static_cast<double>(glm::length(m_client.VisualOffset())));
    }

    if (transport != nullptr)
    {
        const NetStats& stats = transport->Stats();
        ImGui::Separator();
        ImGui::Text("Sent %llu packets, %llu bytes", static_cast<unsigned long long>(stats.packetsSent),
                    static_cast<unsigned long long>(stats.bytesSent));
        ImGui::Text("Received %llu packets, %llu bytes",
                    static_cast<unsigned long long>(stats.packetsReceived),
                    static_cast<unsigned long long>(stats.bytesReceived));
        ImGui::Text("Dropped %llu", static_cast<unsigned long long>(stats.packetsDropped));

        ImGui::Separator();
        ImGui::TextUnformatted("Simulated conditions");
        bool changed = ImGui::SliderFloat("Latency ms", &m_simulatedConditions.latencyMs, 0.0f, 300.0f);
        changed |= ImGui::SliderFloat("Jitter ms", &m_simulatedConditions.jitterMs, 0.0f, 150.0f);
        changed |= ImGui::SliderFloat("Loss %", &m_simulatedConditions.lossPercent, 0.0f, 50.0f);
        if (changed)
        {
            transport->SetConditions(m_simulatedConditions);
        }
    }

    ImGui::Separator();
    ImGui::Text("Other players: %d", static_cast<int>(RemotePlayers().size()));
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        ImGui::Text("  %u %s  %.1f %.1f %.1f  %s", remote.id, remote.name.c_str(),
                    static_cast<double>(remote.position.x), static_cast<double>(remote.position.y),
                    static_cast<double>(remote.position.z), PlayerStanceName(remote.stance));
    }

    ImGui::End();
}

bool PredationGame::ForwardToHost(const std::vector<std::string>& args)
{
    if (m_sessionMode != SessionMode::Client || args.empty())
    {
        return false;
    }
    std::string line;
    for (const std::string& arg : args)
    {
        line += (line.empty() ? "" : " ") + arg;
    }
    m_client.SendCommand(line);
    m_app->GetConsole().Print("Asked the host to run: " + line);
    return true;
}

uint8_t PredationGame::CommandPlayer() const
{
    return m_commandFrom != 0xFF ? m_commandFrom : LocalPlayerId();
}

glm::vec3 PredationGame::CommandPosition() const
{
    return m_commandFrom != 0xFF ? PlayerPosition(m_commandFrom) : m_player.State().position;
}

glm::vec3 PredationGame::CommandForward() const
{
    if (m_commandFrom != 0xFF)
    {
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (remote.id == m_commandFrom)
            {
                return {std::sin(remote.yaw) * std::cos(remote.pitch), std::sin(remote.pitch), -std::cos(remote.yaw) * std::cos(remote.pitch)};
            }
        }
    }
    return m_player.View().Forward();
}

void PredationGame::DrawConnectionReadout()
{
    if (m_sessionMode == SessionMode::Offline)
    {
        return;
    }
    // Under the frame counter, top right.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 8.0f, 84.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##connection", nullptr, flags))
    {
        if (m_sessionMode == SessionMode::Client)
        {
            ImGui::Text("Ping %u ms", static_cast<unsigned>(m_client.PingMs()));
        }
        else
        {
            // The host has no ping of its own, so it sees everybody else's: the one number that says
            // whether a friend complaining of lag has a bad connection or the host does.
            const int players = 1 + static_cast<int>(m_host.ConnectedCount());
            ImGui::Text("Hosting, %d player%s", players, players == 1 ? "" : "s");
            for (const RemotePlayerView& remote : m_host.Remotes())
            {
                if (remote.id != 0)
                {
                    ImGui::Text("%s  %u ms", remote.name.c_str(), static_cast<unsigned>(remote.pingMs));
                }
            }
        }
    }
    ImGui::End();
}


} // namespace pred
