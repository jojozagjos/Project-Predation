// Using what is in your hand: a medical kit, a battery, a keycard, a flare, a sample container.
//
// Fire, with one of them held rather than a weapon, uses it. A use takes as long as items.json says,
// moves the hand along the keys it gives, and everybody sees the hand move; what the use actually does
// -- health, a door, a flare in the world, one fewer of something -- is the host's to decide, the same
// as everything else that changes the world. The torch's battery is the exception: it is only ever
// this player's own torch, so it is theirs.

#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

CVar<float> cv_torchMinutes{"game.torch_minutes", 8.0f, "How many minutes a full torch cell lasts"};
// How far in front of you somebody can be and still be the one a medical kit is used on.
constexpr float kHealReach = 2.2f;
constexpr float kDoorReach = 2.6f;
constexpr size_t kMostFlares = 12;

float Unit()
{
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

} // namespace

// --- Your own hands ------------------------------------------------------------------------------

float PredationGame::TorchStrength() const
{
    // Full until the last fifth of the cell, then fading, and stuttering right at the end the way a
    // torch does with nothing left in it.
    if (m_torchCharge > 0.2f)
    {
        return 1.0f;
    }
    float strength = 0.3f + 0.7f * (m_torchCharge / 0.2f);
    if (m_torchCharge < 0.07f)
    {
        const float t = static_cast<float>(m_time);
        const float stutter = std::sin(t * 23.0f) * std::sin(t * 7.3f + 1.0f);
        strength *= stutter > 0.55f ? 0.2f : 1.0f;
    }
    return strength;
}

void PredationGame::UpdateItemUse(float dt)
{
    const bool alive = m_player.State().alive && m_screen == Screen::Playing;

    // The torch runs its cell down while it is on, and goes out when it is flat.
    if (m_torchOn && alive)
    {
        m_torchCharge = std::max(m_torchCharge - dt / std::max(cv_torchMinutes.Get() * 60.0f, 1.0f), 0.0f);
        if (m_torchCharge <= 0.0f)
        {
            m_torchOn = false;
            PlayNamed("Player/torch_off", m_player.State().position, 0.45f, 0.8f, false);
            m_app->GetConsole().Print("The torch has gone out. It needs a fresh battery.");
        }
    }

    const ItemDefinition* held = m_heldItem != kInvalidItem ? m_items.Get(m_heldItem) : nullptr;

    // A struck flare burns down in the hand. Put away, it is not put in a pocket still burning: it is
    // dropped where you stand, and goes on burning there.
    if (m_flareBurn > 0.0f)
    {
        m_flareBurn -= dt;
        const bool stillHeld = held != nullptr && held->use.kind == ItemUseKind::Flare && alive;
        if (!stillHeld || m_flareBurn <= 0.0f)
        {
            if (m_flareBurn > 0.0f)
            {
                const glm::vec3 feet = m_player.State().position + glm::vec3(0.0f, 0.3f, 0.0f);
                ItemUseMessage drop;
                drop.phase = ItemUseMessage::Throw;
                drop.item = static_cast<uint16_t>(m_items.IdOf("flare"));
                drop.position = feet;
                drop.velocity = glm::vec3(0.0f, -0.5f, 0.0f);
                if (m_sessionMode == SessionMode::Client)
                {
                    m_client.SendItemUse(drop);
                }
                else
                {
                    ThrowFlare(LocalPlayerId(), feet, drop.velocity, m_flareBurn, true);
                }
            }
            m_flareBurn = 0.0f;
            m_flareHeldBy.erase(LocalPlayerId());
            // Burnt out or let go of, it is gone from the bag either way.
            const Inventory::Slot& slot = m_inventory.Selected();
            if (stillHeld && !slot.IsEmpty())
            {
                m_inventory.RemoveFromSlot(m_inventory.SelectedSlot(), 1);
            }
        }
    }
    m_body.SetHeldItemGlow(m_scene, m_flareBurn > 0.0f ? glm::vec3(2.4f, 0.32f, 0.12f) * (0.8f + 0.2f * Unit())
                                                      : (held != nullptr ? held->color * held->emissive : glm::vec3(0.0f)));

    if (m_itemUse.active)
    {
        const ItemDefinition* item = m_items.Get(m_itemUse.item);
        // Anything else in the hand, or nobody left to hold it, and the use stops where it is.
        if (item == nullptr || m_heldItem != m_itemUse.item || !alive || m_hidingSpot >= 0)
        {
            StopItemUse();
            return;
        }
        m_itemUse.time += dt;
        const float t = m_itemUse.time / std::max(m_itemUse.seconds, 0.05f);
        const ItemMotionKey pose = SampleMotion(m_itemUse.second ? item->use.secondMotion : item->use.motion, t);
        m_body.SetHeldItemMotion(pose.offset, pose.turn);
        // A flare leaves the hand partway through the throw, not at the end of the follow-through.
        if (m_itemUse.second && !m_itemUse.released && t >= 0.6f)
        {
            m_itemUse.released = true;
            const PlayerView& view = m_player.View();
            const glm::vec3 from = m_body.HeldItemOrigin();
            const glm::vec3 velocity = view.Forward() * 11.0f + glm::vec3(0.0f, 2.5f, 0.0f) + m_player.State().velocity * 0.6f;
            const float burn = std::max(m_flareBurn, 1.0f);
            m_flareBurn = 0.0f;
            if (m_sessionMode == SessionMode::Client)
            {
                ItemUseMessage thrown;
                thrown.phase = ItemUseMessage::Throw;
                thrown.item = static_cast<uint16_t>(item->id);
                thrown.position = from;
                thrown.velocity = velocity;
                m_client.SendItemUse(thrown);
                // Seen leaving the hand here, at once; the host's own copy is the one everybody else sees.
                ThrowFlare(LocalPlayerId(), from, velocity, burn, false);
            }
            else
            {
                ThrowFlare(LocalPlayerId(), from, velocity, burn, true);
            }
            PlayNamed(item->use.secondSound, from, 0.8f);
        }
        if (t >= 1.0f)
        {
            FinishItemUse();
        }
        return;
    }

    // A lit flare is carried held up in front, the way anybody carries a light.
    m_body.SetHeldItemMotion(m_flareBurn > 0.0f ? glm::vec3(-0.05f, 0.15f, 0.06f) : glm::vec3(0.0f),
                             m_flareBurn > 0.0f ? glm::vec3(-20.0f, 0.0f, 0.0f) : glm::vec3(0.0f));
    const bool pressed = m_app->GetInput().WasActionPressed("fire") && !m_paused && !m_inventoryOpen &&
                         !ImGui::GetIO().WantCaptureMouse && !m_app->IsConsoleOpen();
    if (pressed && alive && held != nullptr && held->use.kind != ItemUseKind::None && !m_weapon.HasWeapon() &&
        m_hidingSpot < 0 && !m_body.ArmsAreClimbing())
    {
        StartItemUse(*held);
    }
}

void PredationGame::StartItemUse(const ItemDefinition& item)
{
    ItemUseState use;
    use.item = item.id;
    use.seconds = item.use.seconds;
    const PlayerView& view = m_player.View();
    switch (item.use.kind)
    {
    case ItemUseKind::Heal:
    {
        // On whoever is right in front of you, close enough to reach; otherwise on yourself.
        float best = kHealReach;
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            const glm::vec3 chest = remote.position + glm::vec3(0.0f, 1.1f, 0.0f);
            const glm::vec3 toward = chest - view.eyePosition;
            const float distance = glm::length(toward);
            // Only somebody who actually needs it: a kit is not spent on somebody at full health.
            if (!remote.alive || remote.health >= PlayerState::kMaxHealth - 0.5f || distance > best || distance < 1e-3f ||
                glm::dot(toward / distance, view.Forward()) < 0.75f)
            {
                continue;
            }
            best = distance;
            use.target = remote.id;
        }
        if (use.target == 0xFF && m_player.State().health >= PlayerState::kMaxHealth - 0.5f)
        {
            m_app->GetConsole().Print("You are not hurt.");
            return;
        }
        break;
    }
    case ItemUseKind::Recharge:
        if (m_torchCharge > 0.97f)
        {
            m_app->GetConsole().Print("The torch cell is still full.");
            return;
        }
        break;
    case ItemUseKind::Unlock:
    {
        // The locked door you are looking at, if you are close enough to reach it. A swipe at nothing
        // is still a swipe.
        const InteractionSystem::Focus& focus = m_interactions.CurrentFocus();
        if (focus.valid && focus.kind == InteractionKind::Door && focus.distance < kDoorReach)
        {
            if (const WorldObjects::Door* door = m_world.GetDoor(focus.payload); door != nullptr && door->locked)
            {
                use.door = static_cast<uint8_t>(focus.payload);
            }
        }
        // Nothing locked in front of you: nothing happens, not even the swipe.
        if (use.door == 0xFF)
        {
            m_app->GetConsole().Print("There is no locked door here to use it on.");
            return;
        }
        break;
    }
    case ItemUseKind::Flare:
        // Struck already: this is the throw.
        if (m_flareBurn > 0.0f)
        {
            use.second = true;
            use.seconds = item.use.secondSeconds;
        }
        break;
    case ItemUseKind::Inspect:
    case ItemUseKind::None:
        break;
    }
    use.active = true;
    m_itemUse = use;
    if (!use.second)
    {
        PlayNamed(item.use.startSound, m_body.HeldItemOrigin(), 0.7f);
    }
    TellItemUse(ItemUseMessage::Start, item);
}

void PredationGame::FinishItemUse()
{
    const ItemDefinition* item = m_items.Get(m_itemUse.item);
    const ItemUseState use = m_itemUse;
    m_itemUse = ItemUseState{};
    m_body.SetHeldItemMotion(glm::vec3(0.0f), glm::vec3(0.0f));
    if (item == nullptr)
    {
        return;
    }
    if (use.second)
    {
        // Thrown: it left the hand already, and the flare in the bag with it.
        const Inventory::Slot& slot = m_inventory.Selected();
        if (!slot.IsEmpty() && slot.item == item->id)
        {
            m_inventory.RemoveFromSlot(m_inventory.SelectedSlot(), 1);
        }
        return;
    }
    PlayNamed(item->use.doneSound, m_body.HeldItemOrigin(), 0.75f);
    bool usedUp = item->use.consumed;
    switch (item->use.kind)
    {
    case ItemUseKind::Recharge:
        // This player's own torch, so this player's own business.
        m_torchCharge = std::min(m_torchCharge + std::max(item->use.amount, 0.0f), 1.0f);
        m_app->GetConsole().Print("A fresh cell in the torch.");
        break;
    case ItemUseKind::Flare:
        // Struck: it burns in the hand until it is thrown, and is used up when it goes.
        m_flareBurn = std::max(item->use.amount, 1.0f);
        usedUp = false;
        break;
    case ItemUseKind::Unlock:
        if (use.door == 0xFF)
        {
            m_app->GetConsole().Print("Nothing here to open with it.");
        }
        break;
    default:
        break;
    }
    ItemUseMessage finished;
    finished.phase = ItemUseMessage::Finish;
    finished.item = static_cast<uint16_t>(item->id);
    finished.target = use.target;
    finished.door = use.door;
    if (m_sessionMode == SessionMode::Client)
    {
        m_client.SendItemUse(finished);
    }
    else
    {
        HandleItemUse(LocalPlayerId(), finished);
    }
    if (usedUp)
    {
        const Inventory::Slot& slot = m_inventory.Selected();
        if (!slot.IsEmpty() && slot.item == item->id)
        {
            m_inventory.RemoveFromSlot(m_inventory.SelectedSlot(), 1);
        }
    }
}

void PredationGame::StopItemUse()
{
    if (!m_itemUse.active)
    {
        return;
    }
    if (const ItemDefinition* item = m_items.Get(m_itemUse.item))
    {
        TellItemUse(ItemUseMessage::Stop, *item);
    }
    m_itemUse = ItemUseState{};
    m_body.SetHeldItemMotion(glm::vec3(0.0f), glm::vec3(0.0f));
}

void PredationGame::TellItemUse(uint8_t phase, const ItemDefinition& item, float amount)
{
    // Only the moving of the hand, for everybody else to see. A finish goes through HandleItemUse,
    // which decides what it did.
    if (m_sessionMode == SessionMode::Client)
    {
        ItemUseMessage message;
        message.phase = phase;
        message.item = static_cast<uint16_t>(item.id);
        m_client.SendItemUse(message);
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::ItemUsed;
        event.player = LocalPlayerId();
        event.item = static_cast<uint16_t>(item.id);
        event.index = phase;
        event.other = m_flareBurn > 0.0f ? 1 : 0;
        event.amount = amount;
        m_host.Broadcast(event);
    }
}

// --- The host deciding what a use did --------------------------------------------------------------

void PredationGame::HandleItemUse(uint8_t player, const ItemUseMessage& use)
{
    if (!IsAuthority())
    {
        return;
    }
    const ItemDefinition* item = m_items.Get(static_cast<ItemId>(use.item));
    if (item == nullptr || item->use.kind == ItemUseKind::None)
    {
        return;
    }
    const bool local = player == LocalPlayerId();
    // Somebody else has to actually have one. The host keeps a tally of what each client picked up.
    if (!local && m_sessionMode == SessionMode::Host && m_host.CarriedCount(player, use.item) <= 0)
    {
        PRED_LOG_WARN(Gameplay, "Player {} used a {} they do not have", player, item->key);
        return;
    }
    glm::vec3 at{0.0f};
    const bool placed = PlayerPositionIfKnown(player, at);

    WorldEventMessage event;
    event.kind = WorldEventKind::ItemUsed;
    event.player = player;
    event.item = use.item;
    event.index = use.phase;

    switch (use.phase)
    {
    case ItemUseMessage::Start:
    case ItemUseMessage::Stop:
        if (!local)
        {
            // Their hand, as this machine draws them.
            if (use.phase == ItemUseMessage::Start)
            {
                const bool throwing = m_flareHeldBy.count(player) != 0 && item->use.kind == ItemUseKind::Flare;
                m_remoteUses[player] = {item->id, throwing, 0.0f, throwing ? item->use.secondSeconds : item->use.seconds};
                PlayerSound(player, throwing ? item->use.secondSound : item->use.startSound, 0.7f);
            }
            else
            {
                m_remoteUses.erase(player);
            }
            event.other = m_flareHeldBy.count(player) != 0 ? 1 : 0;
        }
        break;

    case ItemUseMessage::Finish:
    {
        event.other = use.target;
        switch (item->use.kind)
        {
        case ItemUseKind::Heal:
        {
            // On somebody close enough to reach and alive, or else on themselves.
            uint8_t target = use.target == 0xFF ? player : use.target;
            glm::vec3 them{0.0f};
            if (target != player && (!PlayerPositionIfKnown(target, them) || !placed || glm::distance(them, at) > kHealReach + 1.0f))
            {
                target = player;
            }
            const float health = target == LocalPlayerId() ? m_player.State().health
                                 : m_sessionMode == SessionMode::Host ? m_host.HealthOf(target)
                                                                     : PlayerState::kMaxHealth;
            if (health >= PlayerState::kMaxHealth - 0.5f)
            {
                return; // nobody hurt: nothing done, nothing used
            }
            ApplyHeal(player, target, item->use.amount);
            event.other = target;
            event.amount = item->use.amount;
            break;
        }
        case ItemUseKind::Unlock:
            if (use.door != 0xFF)
            {
                const WorldObjects::Door* door = m_world.GetDoor(use.door);
                if (door != nullptr && door->locked && placed && glm::distance(door->hinge, at) < kDoorReach + 1.5f)
                {
                    UnlockDoor(use.door, player);
                }
            }
            break;
        case ItemUseKind::Flare:
            m_flareHeldBy[player] = std::max(item->use.amount, 1.0f);
            event.amount = item->use.amount;
            break;
        default:
            break;
        }
        if (!local)
        {
            m_remoteUses.erase(player);
            PlayerSound(player, item->use.doneSound, 0.75f);
            // One fewer of it in their tally.
            if (item->use.consumed && item->use.kind != ItemUseKind::Flare && m_sessionMode == SessionMode::Host)
            {
                m_host.TakeCarried(player, use.item, 1);
            }
        }
        break;
    }

    case ItemUseMessage::Throw:
    {
        const float burn = m_flareHeldBy.count(player) != 0 ? m_flareHeldBy[player] : item->use.amount;
        m_flareHeldBy.erase(player);
        m_remoteUses.erase(player);
        if (!local)
        {
            // Not from further away than they are standing: whoever sent it could say anything.
            glm::vec3 from = use.position;
            if (placed && glm::distance(from, at + glm::vec3(0.0f, 1.2f, 0.0f)) > 2.5f)
            {
                from = at + glm::vec3(0.0f, 1.2f, 0.0f);
            }
            const glm::vec3 velocity = glm::length(use.velocity) > 20.0f ? glm::normalize(use.velocity) * 20.0f : use.velocity;
            ThrowFlare(player, from, velocity, burn, true);
            if (m_sessionMode == SessionMode::Host)
            {
                m_host.TakeCarried(player, use.item, 1);
            }
        }
        return; // announced by ThrowFlare
    }

    default:
        return;
    }

    if (m_sessionMode == SessionMode::Host)
    {
        m_host.Broadcast(event);
    }
}

void PredationGame::ApplyHeal(uint8_t user, uint8_t target, float amount)
{
    if (target == LocalPlayerId())
    {
        m_player.Heal(amount);
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        m_host.HealPlayer(target, amount);
    }
    PRED_LOG_INFO(Gameplay, "Player {} patched up player {} for {:.0f}", user, target, amount);
    if (target != user && target == LocalPlayerId())
    {
        m_app->GetConsole().Print("Somebody has patched you up.");
    }
}

void PredationGame::UnlockDoor(int door, uint8_t player)
{
    WorldObjects::Door* opened = m_world.GetDoor(door);
    if (opened == nullptr || !opened->locked)
    {
        return;
    }
    opened->locked = false;
    PlayNamed("Items/keycard_accept", opened->hinge + glm::vec3(0.0f, 1.2f, 0.0f), 0.8f);
    PRED_LOG_INFO(Gameplay, "Player {} unlocked door {}", player, door);
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::DoorUnlocked;
        event.index = static_cast<uint8_t>(door);
        event.player = player;
        m_host.Broadcast(event);
    }
    // And open it: that is what somebody unlocking a door is about to do anyway.
    PerformInteraction(InteractionKind::Door, door, player);
}

void PredationGame::OnItemUsedEvent(const WorldEventMessage& event)
{
    if (event.player == LocalPlayerId())
    {
        return; // our own hands are ours
    }
    const ItemDefinition* item = m_items.Get(static_cast<ItemId>(event.item));
    if (item == nullptr)
    {
        return;
    }
    switch (event.index)
    {
    case ItemUseMessage::Start:
    {
        const bool throwing = event.other == 1 && item->use.kind == ItemUseKind::Flare;
        m_remoteUses[event.player] = {item->id, throwing, 0.0f, throwing ? item->use.secondSeconds : item->use.seconds};
        if (!event.quiet)
        {
            PlayerSound(event.player, throwing ? item->use.secondSound : item->use.startSound, 0.7f);
        }
        break;
    }
    case ItemUseMessage::Stop:
        m_remoteUses.erase(event.player);
        break;
    case ItemUseMessage::Finish:
        m_remoteUses.erase(event.player);
        if (!event.quiet)
        {
            PlayerSound(event.player, item->use.doneSound, 0.75f);
        }
        if (item->use.kind == ItemUseKind::Flare)
        {
            m_flareHeldBy[event.player] = std::max(event.amount, 1.0f);
        }
        if (item->use.kind == ItemUseKind::Heal && event.other == LocalPlayerId() && !event.quiet)
        {
            m_app->GetConsole().Print("Somebody has patched you up.");
        }
        break;
    default:
        break;
    }
}

void PredationGame::UpdateRemoteItemUse(RemoteAvatar& avatar, uint8_t id, float dt)
{
    // Burning in their hand, it glows there; out of it, it does not.
    auto held = m_flareHeldBy.find(id);
    if (held != m_flareHeldBy.end())
    {
        held->second -= dt;
        if (held->second <= 0.0f)
        {
            m_flareHeldBy.erase(held);
            held = m_flareHeldBy.end();
        }
    }
    avatar.body.SetHeldItemGlow(m_scene, held != m_flareHeldBy.end() ? glm::vec3(2.4f, 0.32f, 0.12f) : glm::vec3(0.0f));

    const auto found = m_remoteUses.find(id);
    if (found == m_remoteUses.end())
    {
        const bool lit = held != m_flareHeldBy.end();
        avatar.body.SetHeldItemMotion(lit ? glm::vec3(-0.05f, 0.15f, 0.06f) : glm::vec3(0.0f),
                                      lit ? glm::vec3(-20.0f, 0.0f, 0.0f) : glm::vec3(0.0f));
        return;
    }
    RemoteUse& use = found->second;
    use.time += dt;
    const ItemDefinition* item = m_items.Get(use.item);
    const float t = use.time / std::max(use.seconds, 0.05f);
    if (item == nullptr || t > 1.3f)
    {
        m_remoteUses.erase(found);
        avatar.body.SetHeldItemMotion(glm::vec3(0.0f), glm::vec3(0.0f));
        return;
    }
    const ItemMotionKey pose = SampleMotion(use.second ? item->use.secondMotion : item->use.motion, t);
    avatar.body.SetHeldItemMotion(pose.offset, pose.turn);
}

// --- Flares ---------------------------------------------------------------------------------------

void PredationGame::ThrowFlare(uint8_t player, const glm::vec3& from, const glm::vec3& velocity, float burn, bool announce)
{
    const ItemDefinition* item = m_items.Find("flare");
    if (item == nullptr)
    {
        return;
    }
    // One fewer: the oldest burnt-out one, and then the oldest of all.
    if (m_burningFlares.size() >= kMostFlares)
    {
        auto oldest = std::find_if(m_burningFlares.begin(), m_burningFlares.end(), [](const BurningFlare& f) { return f.burn <= 0.0f; });
        if (oldest == m_burningFlares.end())
        {
            oldest = m_burningFlares.begin();
        }
        m_scene.Destroy(oldest->entity);
        m_app->GetPhysics().DestroyBody(oldest->body);
        m_app->GetAudio().Stop(oldest->hiss);
        m_burningFlares.erase(oldest);
    }
    if (!m_flareMesh.IsValid())
    {
        m_flareMesh = m_app->GetMeshes().Upload(ItemMesh(*item), "flare_burning");
    }
    BurningFlare flare;
    Transform where;
    where.position = from;
    Material material = ItemMaterial(*item);
    material.emissive = glm::vec3(2.4f, 0.32f, 0.12f);
    flare.entity = m_scene.CreateMeshEntity("flare", where, m_flareMesh, material);
    PhysicsWorld& physics = m_app->GetPhysics();
    flare.body = physics.CreateBox(item->size * 0.5f, where, BodyMotion::Dynamic, 500.0f, PhysicsLayer::Debris);
    physics.SetLinearVelocity(flare.body, velocity);
    physics.SetAngularVelocity(flare.body, glm::vec3(Unit() - 0.5f, Unit() - 0.5f, Unit() - 0.5f) * 14.0f);
    flare.burn = std::max(burn, 0.5f);
    AudioEngine::PlayDesc hiss;
    hiss.sound = Sounds("Items/flare_burn").Pick();
    hiss.position = from;
    hiss.loop = true;
    hiss.gain = 0.5f;
    hiss.nearDistance = 1.5f;
    hiss.farDistance = 18.0f;
    if (hiss.sound != kInvalidSound)
    {
        flare.hiss = m_app->GetAudio().Play(hiss);
    }
    m_burningFlares.push_back(flare);
    m_flareHeldBy.erase(player);
    PRED_LOG_INFO(Gameplay, "Player {} threw a flare, burning for {:.0f} s", player, burn);

    if (announce && m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::FlareThrown;
        event.player = player;
        event.position = from;
        event.direction = velocity;
        event.amount = burn;
        m_host.Broadcast(event);
    }
}

void PredationGame::UpdateFlares(float dt)
{
    PhysicsWorld& physics = m_app->GetPhysics();
    AudioEngine& audio = m_app->GetAudio();
    for (size_t i = 0; i < m_burningFlares.size();)
    {
        BurningFlare& flare = m_burningFlares[i];
        const Transform body = physics.GetTransform(flare.body);
        if (Transform* transform = m_scene.GetTransform(flare.entity))
        {
            transform->position = body.position;
            transform->rotation = body.rotation;
        }
        if (flare.burn > 0.0f)
        {
            flare.burn -= dt;
            // Heard landing: a light that has just come to rest across the room is something to look at.
            flare.noiseIn -= dt;
            if (flare.noiseIn <= 0.0f && flare.noiseIn > -dt)
            {
                MakeNoise(NoiseKind::Impact, body.position, NoiseReach::kImpact, -1);
            }
            audio.SetVoicePosition(flare.hiss, body.position);
            if (flare.burn <= 0.0f)
            {
                audio.Stop(flare.hiss);
                flare.hiss = kInvalidVoice;
                if (MeshRenderer* renderer = m_scene.GetMeshRenderer(flare.entity))
                {
                    renderer->material.emissive = glm::vec3(0.0f);
                }
            }
        }
        else
        {
            flare.spent += dt;
        }
        // Burnt out and cold for a while, it goes.
        if (flare.spent > 25.0f)
        {
            m_scene.Destroy(flare.entity);
            physics.DestroyBody(flare.body);
            m_burningFlares.erase(m_burningFlares.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PredationGame::ClearFlares()
{
    for (BurningFlare& flare : m_burningFlares)
    {
        m_scene.Destroy(flare.entity);
        m_app->GetPhysics().DestroyBody(flare.body);
        m_app->GetAudio().Stop(flare.hiss);
    }
    m_burningFlares.clear();
    m_flareHeldBy.clear();
    m_remoteUses.clear();
    m_flareBurn = 0.0f;
    m_itemUse = ItemUseState{};
}

void PredationGame::GatherItemLights(std::vector<PunctualLight>& lights) const
{
    const auto flareLight = [&](const glm::vec3& at, float strength)
    {
        PunctualLight light;
        light.position = at;
        light.color = glm::vec3(1.0f, 0.32f, 0.16f);
        // Flickering, the way burning magnesium does.
        const float t = static_cast<float>(m_time) + at.x * 3.1f;
        light.intensity = 3.2f * strength * (0.85f + 0.15f * std::sin(t * 31.0f) * std::sin(t * 17.0f + 2.0f));
        light.range = 10.0f;
        light.innerAngle = 180.0f;
        light.outerAngle = 180.0f;
        light.sourceRadius = 0.25f;
        lights.push_back(light);
    };
    for (const BurningFlare& flare : m_burningFlares)
    {
        if (flare.burn > 0.0f)
        {
            const Transform body = m_app->GetPhysics().GetTransform(flare.body);
            // Guttering in its last seconds.
            flareLight(body.position + glm::vec3(0.0f, 0.12f, 0.0f), std::clamp(flare.burn / 3.0f, 0.15f, 1.0f));
        }
    }
    if (m_flareBurn > 0.0f)
    {
        flareLight(m_body.HeldItemOrigin() + glm::vec3(0.0f, 0.1f, 0.0f), 1.0f);
    }
    for (const auto& [id, left] : m_flareHeldBy)
    {
        for (const std::unique_ptr<RemoteAvatar>& avatar : m_avatars)
        {
            if (avatar->id == id && avatar->built)
            {
                flareLight(avatar->body.HeldItemOrigin() + glm::vec3(0.0f, 0.1f, 0.0f), 1.0f);
            }
        }
    }
}

float PredationGame::FlareLightAt(const glm::vec3& point) const
{
    // Anybody near a burning flare, with nothing between them and it, is lit by it.
    float light = 0.0f;
    const PhysicsWorld& physics = m_app->GetPhysics();
    for (const BurningFlare& flare : m_burningFlares)
    {
        if (flare.burn <= 0.0f)
        {
            continue;
        }
        const glm::vec3 at = physics.GetTransform(flare.body).position + glm::vec3(0.0f, 0.15f, 0.0f);
        const glm::vec3 toward = point - at;
        const float distance = glm::length(toward);
        if (distance > 9.0f)
        {
            continue;
        }
        if (distance > 0.5f && physics.RayCastStatic(at, toward / distance, distance - 0.3f))
        {
            continue;
        }
        light = std::max(light, 0.95f * (1.0f - distance / 9.0f) + 0.2f);
    }
    return std::min(light, 1.0f);
}

} // namespace pred
