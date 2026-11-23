// A survey set up by hand (Phase 6): its markers lie in a crate in the shuttle's cabin, each carried out to its point and planted
// there, its legs kicked out, levelled and switched on; readings are taken while three or more are working at once. Something that
// walks into one knocks it over, and it stops reading until it is stood up and levelled again. Once its reading is taken it can be
// switched off, folded and pulled up, and carried back aboard: each one brought home pays. The host works it all and tells everybody
// how the markers stand (WorldEventKind::Survey); each player's own marker leaves their hands as they plant it (SpendItem).

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Game/Creature/Creature.h"
#include "Game/PredationGameShared.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

// How often a working marker pings while it reads, and how far that is heard (by whatever listens: it may come to see).
constexpr float kPingEvery = 6.0f;
constexpr float kPingReach = 30.0f;

uint32_t PackMarker(const MissionState::Marker& marker)
{
    const uint32_t read = static_cast<uint32_t>(std::lround(std::clamp(marker.read, 0.0f, 1.0f) * 255.0f));
    return (marker.setup & 0x7Fu) | ((marker.legs & 0x3Fu) << 7) | ((marker.ground & 0x3Fu) << 13) | (read << 19);
}

MissionState::Marker UnpackMarker(uint32_t word)
{
    MissionState::Marker marker;
    marker.setup = static_cast<uint8_t>(word & 0x7Fu);
    marker.legs = static_cast<uint8_t>((word >> 7) & 0x3Fu);
    marker.ground = static_cast<uint8_t>((word >> 13) & 0x3Fu);
    marker.read = static_cast<float>((word >> 19) & 0xFFu) / 255.0f;
    return marker;
}

bool SameSetup(const MissionState& a, const MissionState& b)
{
    for (size_t m = 0; m < a.survey.size(); ++m)
    {
        if (a.survey[m].setup != b.survey[m].setup || a.survey[m].legs != b.survey[m].legs || a.survey[m].ground != b.survey[m].ground)
        {
            return false;
        }
    }
    return true;
}

} // namespace

void PredationGame::PutMarkersInShuttle()
{
    // In their crate on the cabin's right-hand bench, lying side by side along it.
    if (m_missionPlan.kind != MissionPlan::Kind::Survey || m_markerItem == kInvalidItem)
    {
        return;
    }
    for (const glm::vec3& where : m_missionProps.CrateSlots())
    {
        const int index = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, m_markerItem, 1, where,
                                              glm::vec3(0.0f), -1, -1);
        if (WorldObjects::Pickup* placed = index >= 0 ? m_world.GetPickup(index) : nullptr; placed != nullptr && placed->body.IsValid())
        {
            Transform lying;
            lying.position = where;
            lying.rotation = m_missionProps.CrateTurn();
            m_app->GetPhysics().SetTransform(placed->body, lying);
        }
        if (m_sessionMode == SessionMode::Host && index >= 0)
        {
            m_host.Broadcast(PickupSpawnedEvent(static_cast<uint8_t>(index), static_cast<uint16_t>(m_markerItem), 1, LoadToWire(-1), LoadToWire(-1), where,
                                                glm::vec3(0.0f)));
        }
    }
    if (!m_missionProps.CrateSlots().empty())
    {
        const glm::vec3 first = m_missionProps.CrateSlots().front();
        PRED_LOG_INFO(Gameplay, "The survey's markers: {} in their crate in the shuttle, the first at {:.1f} {:.1f} {:.1f}", m_missionProps.CrateSlots().size(), first.x, first.y, first.z);
    }
}

uint8_t PredationGame::DealGround()
{
    // How far down the ground is under each of a marker's three feet, 0 to 2 steps -- never all alike, so it wants levelling.
    for (;;)
    {
        m_surveyDice ^= m_surveyDice << 13;
        m_surveyDice ^= m_surveyDice >> 7;
        m_surveyDice ^= m_surveyDice << 17;
        const uint8_t a = static_cast<uint8_t>(m_surveyDice % 3u);
        const uint8_t b = static_cast<uint8_t>((m_surveyDice >> 8) % 3u);
        const uint8_t c = static_cast<uint8_t>((m_surveyDice >> 16) % 3u);
        if (a != b || b != c)
        {
            return static_cast<uint8_t>(a | (b << 2) | (c << 4));
        }
    }
}

int PredationGame::CountAboard(ItemId item, uint8_t aboard) const
{
    if (item == kInvalidItem)
    {
        return 0;
    }
    int count = 0;
    for (uint8_t player = 0; player < kMaxPlayers; ++player)
    {
        if (((aboard >> player) & 1u) == 0u)
        {
            continue;
        }
        count += player == LocalPlayerId() ? m_inventory.CountOf(item)
                 : m_sessionMode == SessionMode::Host ? m_host.CarriedCount(player, static_cast<uint16_t>(item))
                                                       : 0;
    }
    for (const WorldObjects::Pickup& pickup : m_world.Pickups())
    {
        if (pickup.alive && pickup.item == item)
        {
            if (const Transform* where = m_scene.GetTransform(pickup.entity); where != nullptr && m_facility.Shuttle().Aboard(where->position))
            {
                count += std::max(pickup.count, 1);
            }
        }
    }
    return count;
}

bool PredationGame::PerformSurveyInteraction(int index, uint8_t player)
{
    const int marker = index / MissionRules::kMarkerControls;
    const int control = index % MissionRules::kMarkerControls;
    if (marker < 0 || marker >= m_mission.markers)
    {
        return false;
    }
    const glm::vec3 foot = m_missionPlan.markers[static_cast<size_t>(marker)];
    const MissionState::Marker before = m_mission.survey[static_cast<size_t>(marker)];
    const bool planted = (before.setup & MissionState::kPlanted) != 0u;
    // Its point's flag: a marker carried there, planted -- out of whoever planted it's hands.
    if (!planted && control == MissionRules::kMarkerStake)
    {
        if (!Carries(player, m_markerItem) || MissionRules::PlantMarker(m_mission, m_missionPlan, marker, DealGround()) == MissionRules::Worked::Nothing)
        {
            return false;
        }
        SpendItem(player, m_markerItem);
        ShareSound("World/door_bash", foot + glm::vec3(0.0f, 0.2f, 0.0f), 0.35f);
        MakeNoise(NoiseKind::Item, foot, 12.0f, player);
        BroadcastSurvey(false, -1);
        PRED_LOG_INFO(Gameplay, "Player {} planted survey marker {}", player, marker);
        return true;
    }
    const MissionRules::Worked worked = MissionRules::WorkMarker(m_mission, m_missionPlan, marker, control, DealGround());
    if (worked == MissionRules::Worked::Nothing)
    {
        return false;
    }
    const glm::vec3 head = foot + glm::vec3(0.0f, 1.1f, 0.0f);
    const MissionState::Marker& after = m_mission.survey[static_cast<size_t>(marker)];
    if (worked == MissionRules::Worked::Done)
    {
        // Pulled up and folded: laid on the ground beside where it stood, for whoever is to carry it.
        const glm::vec3 beside = foot + glm::vec3(0.35f, 0.25f, 0.2f);
        const int pickup = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, m_markerItem, 1, beside,
                                               glm::vec3(0.0f), -1, -1);
        if (m_sessionMode == SessionMode::Host && pickup >= 0)
        {
            m_host.Broadcast(PickupSpawnedEvent(static_cast<uint8_t>(pickup), static_cast<uint16_t>(m_markerItem), 1, LoadToWire(-1), LoadToWire(-1), beside,
                                                glm::vec3(0.0f)));
        }
        ShareSound("World/crate_close", foot + glm::vec3(0.0f, 0.4f, 0.0f), 0.7f);
        PRED_LOG_INFO(Gameplay, "Player {} packed survey marker {}", player, marker);
    }
    else if (control == MissionRules::kMarkerStake)
    {
        ShareSound("World/pickup", foot + glm::vec3(0.0f, 0.3f, 0.0f), 0.7f);
        PRED_LOG_INFO(Gameplay, "Player {} stood survey marker {} back up", player, marker);
    }
    else if (control < MissionRules::kMarkerClamp)
    {
        ShareSound("World/crate_open", foot + glm::vec3(0.0f, 0.6f, 0.0f), 0.45f);
    }
    else
    {
        ShareSound("World/switch", head, control == MissionRules::kMarkerSwitch ? 0.7f : 0.4f);
    }
    // Just come to be working: it beeps once, ready.
    if (!MissionRules::MarkerWorking(before) && MissionRules::MarkerWorking(after))
    {
        ShareSound("World/terminal_beep", head, 0.6f);
        PRED_LOG_INFO(Gameplay, "Survey marker {} is working ({} of {} working)", marker, MissionRules::MarkersWorking(m_mission), m_mission.markers);
    }
    BroadcastSurvey(false, -1);
    return true;
}

void PredationGame::SpendItem(uint8_t player, ItemId item)
{
    if (item == kInvalidItem)
    {
        return;
    }
    if (player == LocalPlayerId())
    {
        RemoveOne(item);
        return;
    }
    if (m_sessionMode == SessionMode::Host)
    {
        m_host.TakeCarried(player, static_cast<uint16_t>(item), 1);
        WorldEventMessage spent;
        spent.kind = WorldEventKind::ItemSpent;
        spent.player = player;
        spent.item = static_cast<uint16_t>(item);
        m_host.SendTo(player, spent);
    }
}

void PredationGame::RemoveOne(ItemId item)
{
    for (int slot = 0; slot < m_inventory.SlotCount(); ++slot)
    {
        if (m_inventory.At(slot).item == item && m_inventory.At(slot).count > 0)
        {
            m_inventory.RemoveFromSlot(slot, 1);
            return;
        }
    }
}

void PredationGame::UpdateSurvey(float dt)
{
    if (m_missionPlan.kind != MissionPlan::Kind::Survey || !IsAuthority() || m_mission.stage == MissionState::Stage::None ||
        m_mission.stage == MissionState::Stage::Over)
    {
        return;
    }
    bool changed = false;
    // Something walked into a standing marker: knocked over -- heard as it clatters down.
    const auto knock = [&](int m, const char* by)
    {
        if (MissionRules::KnockMarker(m_mission, m))
        {
            const glm::vec3 foot = m_missionPlan.markers[static_cast<size_t>(m)];
            ShareSound("World/drop", foot + glm::vec3(0.0f, 0.3f, 0.0f), 0.9f);
            MakeNoise(NoiseKind::Item, foot, 15.0f, -1);
            PRED_LOG_INFO(Gameplay, "Survey marker {} knocked over by {}", m, by);
            changed = true;
        }
    };
    for (int m = 0; m < m_mission.markers; ++m)
    {
        const MissionState::Marker& marker = m_mission.survey[static_cast<size_t>(m)];
        if ((marker.setup & MissionState::kPlanted) == 0u || (marker.setup & MissionState::kFallen) != 0u)
        {
            continue;
        }
        const glm::vec3 foot = m_missionPlan.markers[static_cast<size_t>(m)];
        for (const auto& creature : m_creatures)
        {
            const glm::vec3 apart = creature->Position() - foot;
            const float reach = creature->Capabilities().outdoors ? 1.6f : 0.85f;
            if (!creature->Down() && glm::length(glm::vec2(apart.x, apart.z)) < reach && std::abs(apart.y) < 2.0f)
            {
                knock(m, "a creature");
                break;
            }
        }
        if (m_giant != nullptr)
        {
            for (const glm::vec3& giantFoot : m_giant->Feet())
            {
                if (glm::length(glm::vec2(giantFoot.x - foot.x, giantFoot.z - foot.z)) < 1.5f + m_giant->Size() * 0.03f)
                {
                    knock(m, "the giant");
                    break;
                }
            }
        }
    }
    // Working and reading: a ping now and then, each marker in its turn -- heard by whatever listens.
    if (m_mission.attended)
    {
        m_surveyPingIn -= dt;
        if (m_surveyPingIn <= 0.0f)
        {
            m_surveyPingIn = kPingEvery;
            for (int m = 0; m < m_mission.markers; ++m)
            {
                if (MissionRules::MarkerWorking(m_mission.survey[static_cast<size_t>(m)]) && ((m_mission.readings >> m) & 1u) == 0u)
                {
                    const glm::vec3 head = m_missionPlan.markers[static_cast<size_t>(m)] + glm::vec3(0.0f, 1.1f, 0.0f);
                    ShareSound("World/terminal_beep", head, 0.35f);
                    MakeNoise(NoiseKind::Item, head, kPingReach, -1);
                }
            }
        }
    }
    // Told to everybody as it changes, and every half second while readings come in.
    m_surveySendIn -= dt;
    if (changed || !SameSetup(m_mission, m_surveySent) || (m_mission.attended && m_surveySendIn <= 0.0f))
    {
        BroadcastSurvey(false, -1);
    }
}

void PredationGame::BroadcastSurvey(bool quiet, int player)
{
    m_surveySent = m_mission;
    m_surveySendIn = 0.5f;
    if (m_sessionMode != SessionMode::Host || m_missionPlan.kind != MissionPlan::Kind::Survey)
    {
        return;
    }
    WorldEventMessage event;
    event.kind = WorldEventKind::Survey;
    event.quiet = quiet;
    event.index = m_mission.markers;
    for (size_t m = 0; m < std::min<size_t>(m_mission.markers, event.packed.size()); ++m)
    {
        event.packed[m] = PackMarker(m_mission.survey[m]);
    }
    if (player >= 0)
    {
        m_host.SendTo(static_cast<uint8_t>(player), event);
    }
    else
    {
        m_host.Broadcast(event);
    }
}

void PredationGame::ApplySurveyEvent(const WorldEventMessage& event)
{
    m_mission.markers = std::min<uint8_t>(event.index, MissionSpec::kMaxMarkers);
    for (size_t m = 0; m < m_mission.markers; ++m)
    {
        m_mission.survey[m] = UnpackMarker(event.packed[m]);
    }
    ShowMission();
}

} // namespace pred
