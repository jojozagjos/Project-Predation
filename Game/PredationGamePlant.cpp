// A facility's plant got running as a chain, not a switch (Phase 6): fuel carried from where it is kept and poured into its generator's
// tank at its filler, its pump primed by hand stroke by stroke, its valves set to the line-up on their plate -- in any order, each in a
// room of its own -- and then the start at its console, which does nothing until all three are done. The host works it and says how it
// stands with the mission (MissionState::plant, primed, valves); the fuel can poured leaves its carrier's hands (SpendItem).

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"

namespace pred
{

bool PredationGame::PerformPlantInteraction(int control, uint8_t player)
{
    const bool carrying = control == MissionRules::kFillerPour && Carries(player, m_fuelItem);
    const MissionState was = m_mission;
    const MissionRules::Worked worked = MissionRules::WorkPlant(m_mission, m_missionPlan, control, carrying);
    if (worked == MissionRules::Worked::Nothing)
    {
        return false;
    }
    if (control == MissionRules::kFillerCap)
    {
        ShareSound("World/locker", m_missionPlan.filler, 0.5f);
    }
    else if (control == MissionRules::kFillerPour)
    {
        SpendItem(player, m_fuelItem);
        ShareSound("World/splash", m_missionPlan.filler, 0.6f);
        MakeNoise(NoiseKind::Item, m_missionPlan.filler, 10.0f, player);
        PRED_LOG_INFO(Gameplay, "Player {} fuelled the plant's generator", player);
    }
    else if (control == MissionRules::kPumpLever)
    {
        // A stroke: heard about the place -- the last one, the pump taking up, more so.
        ShareSound("World/crate_close", m_missionPlan.pump, 0.55f);
        MakeNoise(NoiseKind::Item, m_missionPlan.pump, 14.0f, player);
        if (worked == MissionRules::Worked::Done)
        {
            ShareSound("World/breaker", m_missionPlan.pump, 0.7f);
            PRED_LOG_INFO(Gameplay, "Player {} primed the plant's pump", player);
        }
    }
    else
    {
        ShareSound("World/switch", m_missionPlan.valves, 0.5f);
        if (worked == MissionRules::Worked::Tripped)
        {
            ShareSound("World/breaker", m_missionPlan.terminal, 1.0f);
            MakeNoise(NoiseKind::Door, m_missionPlan.terminal, 40.0f, player);
            PRED_LOG_INFO(Gameplay, "Player {} turned a valve while the plant started: it tripped", player);
        }
        else if ((m_mission.plant & MissionState::kLinedUp) != 0u && (was.plant & MissionState::kLinedUp) == 0u)
        {
            PRED_LOG_INFO(Gameplay, "The plant's valves are set to their line-up");
        }
    }
    if (was.plant != m_mission.plant || was.primed != m_mission.primed || was.valves != m_mission.valves || was.stage != m_mission.stage)
    {
        BroadcastMission();
    }
    return true;
}

} // namespace pred
