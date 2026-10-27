#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"

#include <algorithm>

// The site's lifts (Lifts): built with each site, run by the host -- which decides when a car goes and tells everybody, and
// opens its doors when it is asked to -- and everybody carried who is in a car when it goes, each machine its own player (and
// the host each client's body too), held in it until it gets there.

namespace pred
{

bool PredationGame::LiftPowered(size_t lift) const
{
    // Only the mission's building is ever without power (its breaker out): a lift there goes nowhere until it is reset.
    return !(m_missionPlan.Valid() && !m_mission.powered && m_lifts.Def(lift).building == m_missionPlan.building);
}

void PredationGame::BuildLifts()
{
    EndLiftRides();
    if (!m_facility.Built())
    {
        m_lifts.Clear(m_scene, m_app->GetPhysics(), m_interactions);
        m_liftRiders.clear();
        m_liftPhases.clear();
        return;
    }
    m_lifts.Build(m_facility.Lifts(), m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions);
    m_liftRiders.assign(m_lifts.Count(), {});
    m_liftPhases.assign(m_lifts.Count(), LiftState::Phase::Shut);
    if (m_lifts.Count() > 0)
    {
        PRED_LOG_INFO(Gameplay, "{} lifts on the site", m_lifts.Count());
    }
}

bool PredationGame::PerformLiftInteraction(int payload, uint8_t player)
{
    const size_t lift = Lifts::LiftOf(payload);
    if (lift >= m_lifts.Count())
    {
        return false;
    }
    LiftState& state = m_lifts.State(lift);
    // The inside button that opens its doors: the stop it stands at, asked for again.
    const int stop = Lifts::StopOf(payload) == Lifts::kOpenButton ? state.at : Lifts::StopOf(payload);
    if (stop < 0 || stop >= static_cast<int>(m_lifts.Def(lift).stops.size()))
    {
        return false;
    }
    PlayNamed("UI/confirm", m_lifts.Doorway(lift, stop), 0.35f, 1.4f, false);
    const LiftRules::Pressed pressed = LiftRules::Press(state, stop, LiftPowered(lift));
    (void)player;
    if (pressed == LiftRules::Pressed::Opened && m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::Lift;
        event.index = static_cast<uint8_t>(lift);
        event.item = static_cast<uint16_t>(stop);
        m_host.Broadcast(event);
    }
    return pressed != LiftRules::Pressed::Nothing;
}

void PredationGame::ApplyLiftEvent(const WorldEventMessage& event)
{
    const size_t lift = event.index;
    if (lift >= m_lifts.Count() || event.item >= m_lifts.Def(lift).stops.size() || event.other >= m_lifts.Def(lift).stops.size())
    {
        return;
    }
    LiftState& state = m_lifts.State(lift);
    if (event.flag2)
    {
        LiftRules::RestAt(state, event.item);
        return;
    }
    if (event.flag)
    {
        LiftRules::Depart(state, event.other, event.item);
        TakeLiftRiders(lift);
        return;
    }
    LiftRules::OpenAt(state, event.item);
}

void PredationGame::SendLiftsTo(uint8_t player)
{
    for (size_t lift = 0; lift < m_lifts.Count(); ++lift)
    {
        const LiftState& state = m_lifts.State(lift);
        WorldEventMessage event;
        event.kind = WorldEventKind::Lift;
        event.index = static_cast<uint8_t>(lift);
        // (Somewhere between two, where it is going.)
        event.item = static_cast<uint16_t>(state.phase == LiftState::Phase::Moving ? state.to : state.at);
        event.flag2 = true;
        event.quiet = true;
        m_host.SendTo(player, event);
    }
}

void PredationGame::TakeLiftRiders(size_t lift)
{
    std::vector<LiftRider>& riders = m_liftRiders[lift];
    riders.clear();
    const glm::vec3 floor = m_lifts.CarFloor(lift);
    // This machine's own player, standing in the car (not dead, not held by something).
    const PlayerState& self = m_player.State();
    if (self.alive && !m_player.IsAttached() && m_hidingSpot < 0 && m_lifts.InCar(lift, self.position))
    {
        riders.push_back({LocalPlayerId(), self.position - floor});
        if (m_sessionMode == SessionMode::Client)
        {
            m_client.SetRiding(true);
        }
    }
    // And on the host, each client's body as it has it.
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (remote.alive && m_lifts.InCar(lift, remote.position))
            {
                riders.push_back({remote.id, remote.position - floor});
            }
        }
    }
}

void PredationGame::CarryLiftRiders(size_t lift, bool release)
{
    std::vector<LiftRider>& riders = m_liftRiders[lift];
    const glm::vec3 floor = m_lifts.CarFloor(lift);
    // Nobody dead is carried: their body is let go of where it is (and whoever comes back comes back where they come back).
    riders.erase(std::remove_if(riders.begin(), riders.end(),
                                [&](const LiftRider& rider)
                                {
                                    if (rider.player == LocalPlayerId())
                                    {
                                        const bool dead = !m_player.State().alive;
                                        if (dead && m_sessionMode == SessionMode::Client)
                                        {
                                            m_client.SetRiding(false);
                                        }
                                        return dead;
                                    }
                                    for (const RemotePlayerView& remote : RemotePlayers())
                                    {
                                        if (remote.id == rider.player && !remote.alive)
                                        {
                                            if (m_sessionMode == SessionMode::Host)
                                            {
                                                m_host.SetPlayerRiding(rider.player, false, remote.position);
                                            }
                                            return true;
                                        }
                                    }
                                    return false;
                                }),
                 riders.end());
    for (const LiftRider& rider : riders)
    {
        // A couple of centimetres over the car's floor, carried and put down.
        const glm::vec3 feet = floor + glm::vec3(rider.offset.x, 0.02f, rider.offset.z);
        if (rider.player == LocalPlayerId())
        {
            if (release)
            {
                m_player.Detach(feet);
                if (m_sessionMode == SessionMode::Client)
                {
                    m_client.SetRiding(false);
                }
            }
            else
            {
                m_player.Attach(feet, m_player.State().yaw);
            }
        }
        else if (m_sessionMode == SessionMode::Host)
        {
            m_host.SetPlayerRiding(rider.player, !release, feet);
        }
    }
    if (release)
    {
        riders.clear();
    }
}

void PredationGame::EndLiftRides()
{
    for (size_t lift = 0; lift < m_liftRiders.size() && lift < m_lifts.Count(); ++lift)
    {
        CarryLiftRiders(lift, true);
    }
    m_liftRiders.clear();
}

void PredationGame::UpdateLifts(float dt)
{
    for (size_t lift = 0; lift < m_lifts.Count(); ++lift)
    {
        LiftState& state = m_lifts.State(lift);
        // Somebody in its doorway where it stands open: anybody, as this machine sees them.
        bool blocked = false;
        if (state.phase == LiftState::Phase::Open || state.phase == LiftState::Phase::Closing)
        {
            blocked = m_player.State().alive && m_lifts.InDoorway(lift, state.at, m_player.State().position);
            for (const RemotePlayerView& remote : RemotePlayers())
            {
                blocked = blocked || (remote.alive && m_lifts.InDoorway(lift, state.at, remote.position));
            }
        }
        const LiftRules::Ticked ticked = LiftRules::Tick(state, m_lifts.Def(lift), dt, IsAuthority(), LiftPowered(lift), blocked);
        if (ticked.departed)
        {
            TakeLiftRiders(lift);
            if (m_sessionMode == SessionMode::Host)
            {
                WorldEventMessage event;
                event.kind = WorldEventKind::Lift;
                event.index = static_cast<uint8_t>(lift);
                event.other = static_cast<uint8_t>(state.at);
                event.item = static_cast<uint16_t>(state.to);
                event.flag = true;
                m_host.Broadcast(event);
            }
        }
        if (state.phase == LiftState::Phase::Moving)
        {
            CarryLiftRiders(lift, false);
        }
        if (ticked.arrived)
        {
            CarryLiftRiders(lift, true);
            PlayNamed("UI/confirm", m_lifts.Doorway(lift, state.at) + glm::vec3(0.0f, 2.4f, 0.0f), 0.5f, 1.8f, false);
        }
        // Its doors, heard where they are, starting to open or to shut -- here, or as the host said.
        LiftState::Phase& seen = m_liftPhases[lift];
        if (seen != state.phase && (state.phase == LiftState::Phase::Opening || state.phase == LiftState::Phase::Closing))
        {
            PlaySound(m_sounds.door.Pick(), m_lifts.Doorway(lift, state.at) + glm::vec3(0.0f, 1.2f, 0.0f), 0.55f, 1.3f);
        }
        seen = state.phase;
        // Its light, with its car: shining down from its ceiling, kept to the shaft about the car.
        const FacilityMap::Lift& def = m_lifts.Def(lift);
        if (def.lamp >= 0 && static_cast<size_t>(def.lamp) < m_levelLights.Count())
        {
            const glm::vec3 floor = m_lifts.CarFloor(lift);
            const float inside = FacilityLayout::kCell * 0.5f - FacilitySpec::kHalfWall;
            m_levelLights.Place(m_scene, def.lamp, floor + glm::vec3(0.0f, FacilitySpec::kLiftCarHeight - 0.05f, 0.0f), {0.0f, -1.0f, 0.0f});
            m_levelLights.Bound(def.lamp, floor - glm::vec3(inside, 0.05f, inside), floor + glm::vec3(inside, FacilitySpec::kLiftCarHeight + 0.1f, inside));
        }
    }
    m_lifts.Show(m_scene, m_app->GetPhysics());
}

} // namespace pred
