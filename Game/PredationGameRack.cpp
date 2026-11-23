// The data in a server rack (Phase 6: data got out by hand): its drive bay on a rack's face, worked up close. Its cover opened, its drive
// pulled out on its sled and carried away; a damaged drive first drawn out to its board and fitted with one taken out of another rack's
// service hatch (a multitool undoes it); or an archive too big for a drive copied onto the copy unit brought from the shuttle,
// connected to the rack's port -- loud while it copies -- and disconnected and carried back. The host works it and says how it stands
// with the mission (MissionState::rack); what is used up leaves its carrier's hands (SpendItem), what comes out is dropped into the world.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"

#include <glm/geometric.hpp>

namespace pred
{

void PredationGame::PutCopyUnitInShuttle()
{
    // On the cabin's right-hand bench, for an archive.
    if (m_missionPlan.data != MissionPlan::Data::Archive || m_copyItem == kInvalidItem || !m_facility.Shuttle().Built())
    {
        return;
    }
    DropIntoWorld(m_copyItem, 1, -1, -1, m_missionProps.BenchSpot(), glm::vec3(0.0f));
    const glm::vec3 at = m_missionProps.BenchSpot();
    PRED_LOG_INFO(Gameplay, "The copy unit: on the shuttle's bench at {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z);
}

bool PredationGame::PerformRackInteraction(int control, uint8_t player)
{
    // What the control wants carried: the board for the slot, the copy unit for the port, a multitool for the other rack's board.
    const ItemId wanted = control == MissionRules::kRackSlot   ? m_boardItem
                          : control == MissionRules::kRackPort ? m_copyItem
                          : control == MissionRules::kDonorBoard ? m_items.IdOf("multitool")
                                                                 : kInvalidItem;
    const bool carrying = wanted != kInvalidItem && Carries(player, wanted);
    const MissionState was = m_mission;
    const MissionRules::Worked worked = MissionRules::WorkRack(m_mission, m_missionPlan, control, carrying);
    if (worked == MissionRules::Worked::Nothing)
    {
        return false;
    }
    const glm::vec3 bay = m_missionPlan.terminal;
    const glm::vec3 out = glm::vec3(std::sin(m_missionPlan.terminalYaw) * -1.0f, 0.0f, -std::cos(m_missionPlan.terminalYaw));
    switch (control)
    {
    case MissionRules::kRackCover:
    case MissionRules::kDonorHatch:
        ShareSound("World/locker", control == MissionRules::kRackCover ? bay : m_missionPlan.donor, 0.6f);
        break;
    case MissionRules::kRackSled:
        if (worked == MissionRules::Worked::Done)
        {
            // Pulled: the drive out on its sled, into the hand -- dropped just in front of the bay, for whoever is to carry it.
            DropIntoWorld(m_driveItem, 1, -1, -1, m_missionPlan.driveAt, out * 0.4f);
            ShareSound("World/pickup", bay, 0.8f);
            PRED_LOG_INFO(Gameplay, "Player {} pulled the drive from the rack", player);
        }
        else
        {
            ShareSound("World/crate_open", bay, 0.45f);
        }
        break;
    case MissionRules::kRackSlot:
        SpendItem(player, m_boardItem);
        ShareSound("World/switch", bay, 0.7f);
        PRED_LOG_INFO(Gameplay, "Player {} fitted the rack's drive with a new board", player);
        break;
    case MissionRules::kRackPort:
        SpendItem(player, m_copyItem);
        ShareSound("World/switch", bay, 0.7f);
        ShareSound("World/terminal_beep", bay, 0.7f);
        MakeNoise(NoiseKind::Item, bay, 20.0f, player);
        m_missionHumIn = 2.0f;
        // (Dead with the building's power out: connected, but copying nothing until it is back.)
        m_missionFoundNoPower = m_missionFoundNoPower || !(m_mission.powered && m_mission.fed);
        PRED_LOG_INFO(Gameplay, "Player {} connected the copy unit: copying{}", player, m_mission.powered && m_mission.fed ? "" : " (no power)");
        break;
    case MissionRules::kRackUnit:
        // Disconnected: the unit, the archive on it, to be carried back.
        DropIntoWorld(m_driveItem, 1, -1, -1, m_missionProps.UnitAt() + glm::vec3(0.0f, 0.1f, 0.0f), glm::vec3(0.0f));
        ShareSound("World/crate_close", m_missionProps.UnitAt(), 0.7f);
        PRED_LOG_INFO(Gameplay, "Player {} disconnected the copy unit", player);
        break;
    case MissionRules::kDonorBoard:
    {
        // Out of its hatch, and dropped in front of it.
        const glm::vec3 donorOut{-std::sin(m_missionPlan.donorYaw), 0.0f, -std::cos(m_missionPlan.donorYaw)};
        DropIntoWorld(m_boardItem, 1, -1, -1, m_missionPlan.donor + donorOut * 0.25f, donorOut * 0.3f);
        ShareSound("World/switch", m_missionPlan.donor, 0.6f);
        PRED_LOG_INFO(Gameplay, "Player {} took the board out of the other rack", player);
        break;
    }
    default: break;
    }
    if (was.stage != m_mission.stage || was.rack != m_mission.rack)
    {
        BroadcastMission();
    }
    return true;
}

} // namespace pred
