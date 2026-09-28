// The support drone a dead player drives, as the game uses it: when one arrives, how it is driven and
// shown, what knocks it about, and how everybody else sees theirs. The drone itself -- its body, its
// tracks, its camera -- is SupportDrone; this is everything round it.

#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Debug/FrameStats.h"

#include <bgfx/bgfx.h>

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

namespace pred
{

namespace
{

CVar<float> cv_droneSeconds{"game.drone_seconds", 3.0f, "How long after dying the support drone arrives"};
// For trying the drone on your own: a lone dead player gets one, and nothing ends. Back with the respawn key.
CVar<bool> cv_droneWhenAlone{"game.drone_when_alone", false,
                             "Give a player who dies on their own a drone anyway, rather than ending the deployment "
                             "(for trying the drone; come back with the respawn key)"};
CVar<float> cv_wipeSeconds{"game.wipe_seconds", 10.0f,
                           "With everybody down for good, how long before the deployment is over and everybody is back"};

// How long a creature leaves between swipes at the same drone, and how hard they are.
constexpr float kSwatEvery = 1.8f;
constexpr float kSwatReach = 1.6f;
constexpr float kSwatDamage = 34.0f;
// How far a drone has to have moved to have been heard doing it, and how far its motor carries.
constexpr float kMotorHeardEvery = 0.6f;
constexpr float kMotorReach = 4.0f;
// A light on a mast, not a hand torch: a little dimmer and shorter.
constexpr float kDroneLampStrength = 0.7f;
constexpr float kDroneLampReach = 0.8f;
// Where a timer is parked for a death that is not coming back.
constexpr float kForGood = 1.0e9f;

} // namespace

void PredationGame::RegisterDroneCommands()
{
#if PRED_DEV_TOOLS
    Console& console = m_app->GetConsole();
    console.RegisterCommand("hurt_drone", "Strike your support drone: hurt_drone <damage> [shove, in kg m/s]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!m_supportDrone.Active() || !IsAuthority())
                                {
                                    m_app->GetConsole().PrintError("No drone of yours to strike, or not the host.");
                                    return;
                                }
                                const float damage = args.size() >= 2 ? std::strtof(args[1].c_str(), nullptr) : 30.0f;
                                const float shove = args.size() >= 3 ? std::strtof(args[2].c_str(), nullptr) : 0.0f;
                                const glm::vec3 side = m_supportDrone.Rotation() * glm::vec3(1.0f, 0.0f, 0.0f);
                                HitDrone(LocalPlayerId(), side * shove + glm::vec3(0.0f, shove * 0.5f, 0.0f), damage);
                            });
    console.RegisterCommand("perf_report", "Average the frame's timings over some frames and log them: perf_report [frames]",
                            [this](const std::vector<std::string>& args)
                            {
                                m_perfFramesLeft = args.size() >= 2 ? std::max(std::atoi(args[1].c_str()), 1) : 300;
                                m_perfFrames = 0;
                                m_perfTimings.clear();
                                m_perfFrameMs = m_perfWorstMs = m_perfGpuMs = m_perfDraws = 0.0;
                            });
    console.RegisterCommand("drone", "Play as a support drone, from where you stand; again to come back where it is",
                            [this](const std::vector<std::string>&)
                            {
                                if (!IsAuthority() || m_screen != Screen::Playing)
                                {
                                    m_app->GetConsole().PrintError("Only in a game, and only as the host.");
                                    return;
                                }
                                PhysicsWorld& physics = m_app->GetPhysics();
                                if (m_supportDrone.Active())
                                {
                                    const glm::vec3 at = m_supportDrone.Position() + glm::vec3(0.0f, 0.3f, 0.0f);
                                    RespawnLocalPlayer(at);
                                    if (m_sessionMode == SessionMode::Host)
                                    {
                                        WorldEventMessage event;
                                        event.kind = WorldEventKind::PlayerRespawned;
                                        event.player = LocalPlayerId();
                                        event.position = at;
                                        m_host.Broadcast(event);
                                    }
                                    return;
                                }
                                // Out of the body and into a drone at your feet, straight away, with nothing ending
                                // because you are down.
                                m_playingDrone = true;
                                const glm::vec3 feet = m_player.State().position;
                                const glm::vec3 ahead{std::sin(m_lookYaw), 0.0f, -std::cos(m_lookYaw)};
                                const float yaw = m_lookYaw;
                                if (m_player.State().alive)
                                {
                                    KillPlayer(LocalPlayerId(), glm::vec3(0.0f));
                                }
                                m_supportDrone.Deploy(m_scene, m_app->GetMeshes(), physics, feet + ahead * 0.8f, yaw);
                                m_lookYaw = yaw;
                                m_lookPitch = 0.0f;
                            });
    console.RegisterCommand("site_stairs", "Stand at the foot of a stairwell of one of the site's buildings, facing up it: "
                            "site_stairs <building> <stairwell>",
                            [this](const std::vector<std::string>& args)
                            {
                                const SitePlan& plan = m_facility.Plan();
                                const size_t b = args.size() >= 2 ? std::strtoul(args[1].c_str(), nullptr, 10) : 0;
                                const size_t w = args.size() >= 3 ? std::strtoul(args[2].c_str(), nullptr, 10) : 0;
                                if (b >= plan.buildings.size() || w >= plan.buildings[b].stairwells.size())
                                {
                                    m_app->GetConsole().PrintError("No such building or stairwell.");
                                    return;
                                }
                                const FacilityLayout& building = plan.buildings[b];
                                const FacilityLayout::Stairwell& well = building.stairwells[w];
                                // The middle of its low end, half a cell in, and which way the flight climbs.
                                const glm::vec2 low = well.alongX ? glm::vec2(well.rising ? well.min.x : well.max.x + 1, well.min.y + 0.5f)
                                                                  : glm::vec2(well.min.x + 0.5f, well.rising ? well.min.y : well.max.y + 1);
                                const glm::vec2 climb = well.alongX ? glm::vec2(well.rising ? 1.0f : -1.0f, 0.0f)
                                                                    : glm::vec2(0.0f, well.rising ? 1.0f : -1.0f);
                                const glm::vec3 at = FacilityMap::ToWorld(building, well.floor, low + climb * 0.2f);
                                m_player.Teleport(at + glm::vec3(0.0f, 0.1f, 0.0f));
                                m_lookYaw = std::atan2(climb.x, -climb.y);
                                m_lookPitch = glm::radians(15.0f);
                            });
    console.RegisterCommand("site_room", "Go to the middle of a room of one of the site's buildings: site_room <building> <room>",
                            [this](const std::vector<std::string>& args)
                            {
                                const SitePlan& plan = m_facility.Plan();
                                const size_t b = args.size() >= 2 ? std::strtoul(args[1].c_str(), nullptr, 10) : 0;
                                const size_t r = args.size() >= 3 ? std::strtoul(args[2].c_str(), nullptr, 10) : 0;
                                if (b >= plan.buildings.size() || r >= plan.buildings[b].rooms.size())
                                {
                                    m_app->GetConsole().PrintError("No such building or room.");
                                    return;
                                }
                                const FacilityLayout& building = plan.buildings[b];
                                const FacilityLayout::Room& room = building.rooms[r];
                                const glm::vec3 at = FacilityMap::ToWorld(building, room.floor, glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f);
                                m_player.Teleport(at + glm::vec3(0.0f, 0.1f, 0.0f));
                                const glm::vec3 lo = FacilityMap::ToWorld(building, room.floor, glm::vec2(room.min));
                                const glm::vec3 hi = FacilityMap::ToWorld(building, room.floor, glm::vec2(room.max + glm::ivec2(1)));
                                char corners[96];
                                std::snprintf(corners, sizeof(corners), ", from %.1f %.1f to %.1f %.1f", lo.x, lo.z, hi.x, hi.z);
                                m_app->GetConsole().Print("Room " + std::to_string(r) + " of building " + std::to_string(b) + ", floor " +
                                                          std::to_string(room.floor) + (room.dark ? ", dark" : "") + corners);
                                PRED_LOG_INFO(Gameplay, "site_room: building {} room {} floor {}{}", b, r, room.floor, corners);
                            });
    console.RegisterCommand("hurt_player", "As the host, hurt somebody: hurt_player <id> [amount]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (!IsAuthority() || args.size() < 2)
                                {
                                    m_app->GetConsole().PrintError("usage, as the host: hurt_player <id> [amount]");
                                    return;
                                }
                                const auto player = static_cast<uint8_t>(std::strtoul(args[1].c_str(), nullptr, 10));
                                const float amount = args.size() >= 3 ? std::strtof(args[2].c_str(), nullptr) : 40.0f;
                                ApplyPlayerDamage(player, amount, kNoKiller, glm::vec3(0.0f, 0.0f, 1.0f), "console");
                            });
    console.RegisterCommand("drone_here", "Move your support drone to where the free camera is looking from",
                            [this](const std::vector<std::string>&)
                            {
                                if (!m_supportDrone.Active())
                                {
                                    return;
                                }
                                Transform at;
                                at.position = m_camera.position;
                                m_app->GetPhysics().SetTransform(m_supportDrone.Body(), at);
                            });
#endif
}

void PredationGame::UpdatePerfReport()
{
    if (m_perfFramesLeft <= 0)
    {
        return;
    }
    const FrameStats& stats = FrameStats::Instance();
    for (const FrameStats::Timing& timing : stats.Timings())
    {
        m_perfTimings[timing.name] += timing.milliseconds;
    }
    m_perfFrameMs += stats.FrameMs();
    m_perfWorstMs = std::max(m_perfWorstMs, static_cast<double>(stats.FrameMs()));
    if (const bgfx::Stats* gpu = m_app->GetRenderer().Stats())
    {
        m_perfDraws += gpu->numDraw;
        if (gpu->gpuTimerFreq > 0)
        {
            m_perfGpuMs += 1000.0 * static_cast<double>(gpu->gpuTimeEnd - gpu->gpuTimeBegin) / static_cast<double>(gpu->gpuTimerFreq);
        }
    }
    ++m_perfFrames;
    if (--m_perfFramesLeft > 0)
    {
        return;
    }
    const double n = std::max(m_perfFrames, 1);
    PRED_LOG_INFO(Gameplay, "perf over {} frames: {:.2f} ms a frame ({:.0f} fps), worst {:.1f} ms; GPU {:.2f} ms; {:.0f} draw calls",
                  m_perfFrames, m_perfFrameMs / n, 1000.0 * n / std::max(m_perfFrameMs, 1.0e-3), m_perfWorstMs, m_perfGpuMs / n,
                  m_perfDraws / n);
    for (const auto& [name, total] : m_perfTimings)
    {
        PRED_LOG_INFO(Gameplay, "perf   {:<24} {:.2f} ms", name, total / n);
    }
    size_t lamps = m_scene.GetEnvironment().sceneLights.size();
    size_t entities = m_scene.EntityCount();
    PRED_LOG_INFO(Gameplay, "perf   {} lamps gathered, {} entities", lamps, entities);
    const SceneRenderer::ClusterStats& clusters = m_app->GetSceneRenderer().LastClusterStats();
    PRED_LOG_INFO(Gameplay, "perf   clusters: {} lamps, {} references, busiest cell {}{}", clusters.lights, clusters.references,
                  clusters.busiestCell, clusters.truncated ? ", SOME LEFT OUT" : "");
}

bool PredationGame::DeathIsPermanent() const
{
    // Always: nobody comes back until the deployment is over.
    return true;
}

float PredationGame::RespawnSecondsForDeath() const
{
    return kForGood;
}

void PredationGame::UpdateDrone(const PlayerInput& input, float dt)
{
    PhysicsWorld& physics = m_app->GetPhysics();
    m_everybodyDownFor = std::max(m_everybodyDownFor - dt, 0.0f);
    const bool dead = m_screen == Screen::Playing && !m_player.State().alive;
    if (!dead)
    {
        if (m_supportDrone.Active())
        {
            m_supportDrone.Remove(m_scene, physics);
        }
        m_droneArrivesIn = -1.0f;
        return;
    }

    if (!m_supportDrone.Active())
    {
        if (m_droneArrivesIn < 0.0f)
        {
            m_droneArrivesIn = cv_droneSeconds.Get();
        }
        m_droneArrivesIn -= dt;
        // A death that is only a pause is not worth a drone arriving for the last moment of it.
        const bool backSoon = !m_deadForGood && m_respawnTimer < 2.0f;
        // Nor is one sent for nobody: with everybody down, the deployment is over.
        const bool over = IsAuthority() && m_wipeTimer > 0.0f;
        if (m_droneArrivesIn > 0.0f || backSoon || over)
        {
            return;
        }
        // Put down where the team was: the insertion point. Side by side when there are several, so
        // two dead players' drones do not arrive inside one another.
        const glm::vec3 across{std::cos(m_spawnYaw), 0.0f, std::sin(m_spawnYaw)};
        const glm::vec3 at = m_spawnPoint + across * ((static_cast<float>(LocalPlayerId()) - 1.5f) * 0.7f);
        m_supportDrone.Deploy(m_scene, m_app->GetMeshes(), physics, at, m_spawnYaw);
        m_spectating = -1;
        m_lookYaw = m_spawnYaw;
        m_lookPitch = 0.0f;
        PlayNamed("Items/battery_in", at, 0.6f, 0.9f, true);
        PRED_LOG_INFO(Gameplay, "Support drone deployed at {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z);
        return;
    }

    SupportDrone::Controls controls;
    controls.move = input.move;
    controls.lookYaw = input.yaw;
    controls.lookPitch = input.pitch;
    controls.rightItself = input.jump;
    controls.jump = input.jump;
    const bool wasDisabled = m_supportDrone.Disabled();
    m_supportDrone.Step(physics, controls, dt);
    if (wasDisabled && !m_supportDrone.Disabled())
    {
        PlayNamed("Items/battery_in", m_supportDrone.Position(), 0.5f, 1.1f, true);
    }
}

DroneState PredationGame::LocalDroneState() const
{
    DroneState state;
    if (!m_supportDrone.Active())
    {
        return state;
    }
    state.active = true;
    state.position = m_supportDrone.Position();
    state.rotation = m_supportDrone.Rotation();
    state.headYaw = m_supportDrone.HeadYaw();
    state.headPitch = m_supportDrone.HeadPitch();
    state.health = m_supportDrone.Health();
    state.rebootLeft = m_supportDrone.RebootLeft();
    state.lightOn = m_supportDrone.lightOn;
    return state;
}

void PredationGame::UpdateDroneVisuals(float dt)
{
    PhysicsWorld& physics = m_app->GetPhysics();
    if (m_supportDrone.Active())
    {
        m_supportDrone.UpdateVisual(m_scene, physics, dt);
        m_supportDrone.SetHeadHidden(m_scene, m_cameraMode == CameraMode::FirstPerson);
        // The mouse cannot look further than the mast tips: otherwise it winds on past the stop, and has
        // to be wound back before the picture moves again.
        const float most = glm::radians(SupportDrone::kMostPitchDegrees);
        m_lookPitch = std::clamp(m_lookPitch, -most, most);
    }

    // Everybody else's, where their machines say, eased between what they say.
    std::set<uint8_t> shown;
    if (m_sessionMode != SessionMode::Offline && m_screen == Screen::Playing)
    {
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (!remote.drone.active)
            {
                continue;
            }
            shown.insert(remote.id);
            SupportDrone& proxy = m_remoteDrones[remote.id];
            if (!proxy.Active())
            {
                proxy.DeployProxy(m_scene, m_app->GetMeshes(), physics, remote.drone.position, remote.drone.rotation);
            }
            proxy.SetPose(remote.drone.position, remote.drone.rotation, remote.drone.headYaw, remote.drone.headPitch);
            proxy.SetStatus(remote.drone.health, remote.drone.rebootLeft);
            proxy.lightOn = remote.drone.lightOn;
            proxy.UpdateVisual(m_scene, physics, dt);
        }
    }
    for (auto it = m_remoteDrones.begin(); it != m_remoteDrones.end();)
    {
        if (shown.count(it->first) == 0)
        {
            it->second.Remove(m_scene, physics);
            it = m_remoteDrones.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void PredationGame::RemoveAllDrones()
{
    PhysicsWorld& physics = m_app->GetPhysics();
    m_supportDrone.Remove(m_scene, physics);
    for (auto& [owner, proxy] : m_remoteDrones)
    {
        proxy.Remove(m_scene, physics);
    }
    m_remoteDrones.clear();
    m_droneArrivesIn = -1.0f;
    m_droneSwatCooldown.clear();
    m_droneHeardAt.clear();
}

int PredationGame::DroneOwnerOf(BodyHandle body) const
{
    if (!body.IsValid())
    {
        return -1;
    }
    if (m_supportDrone.Active() && m_supportDrone.Body() == body)
    {
        return LocalPlayerId();
    }
    for (const auto& [owner, proxy] : m_remoteDrones)
    {
        if (proxy.Active() && proxy.Body() == body)
        {
            return owner;
        }
    }
    return -1;
}

bool PredationGame::DroneOf(uint8_t player, glm::vec3& eye) const
{
    if (player == LocalPlayerId())
    {
        if (m_supportDrone.Active())
        {
            eye = m_supportDrone.Eye();
            return true;
        }
        return false;
    }
    const auto found = m_remoteDrones.find(player);
    if (found != m_remoteDrones.end() && found->second.Active())
    {
        eye = found->second.Eye();
        return true;
    }
    return false;
}

void PredationGame::HitDrone(uint8_t owner, const glm::vec3& impulse, float damage)
{
    // Its owner's machine is the one moving it, so that is where the blow lands; here it is only heard.
    PRED_LOG_INFO(Gameplay, "Player {}'s drone struck: {:.0f} damage, shoved {:.0f}", owner, damage, glm::length(impulse));
    if (owner == LocalPlayerId() && m_supportDrone.Active())
    {
        m_supportDrone.Hit(m_app->GetPhysics(), impulse, damage);
    }
    glm::vec3 at{0.0f};
    if (DroneOf(owner, at))
    {
        PlayNamed("Weapons/impact_hard", at, 0.7f, 1.3f, true);
    }
    if (m_sessionMode == SessionMode::Host)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::DroneHit;
        event.player = owner;
        event.direction = impulse;
        event.amount = damage;
        m_host.Broadcast(event);
    }
}

void PredationGame::OnDroneHitEvent(const WorldEventMessage& event)
{
    if (event.player == LocalPlayerId() && m_supportDrone.Active())
    {
        m_supportDrone.Hit(m_app->GetPhysics(), event.direction, event.amount);
    }
    glm::vec3 at{0.0f};
    if (DroneOf(event.player, at))
    {
        PlayNamed("Weapons/impact_hard", at, 0.7f, 1.3f, true);
    }
}

void PredationGame::UpdateDroneThreats(float dt)
{
    if (!IsAuthority() || m_screen != Screen::Playing)
    {
        return;
    }
    struct Seen
    {
        uint8_t owner;
        glm::vec3 at;
    };
    std::vector<Seen> drones;
    if (m_supportDrone.Active())
    {
        drones.push_back({LocalPlayerId(), m_supportDrone.Position()});
    }
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (remote.drone.active)
        {
            drones.push_back({remote.id, remote.drone.position});
        }
    }
    for (auto& [owner, left] : m_droneSwatCooldown)
    {
        left = std::max(left - dt, 0.0f);
    }

    for (const Seen& drone : drones)
    {
        // Its motor is heard when it goes anywhere: a whirr that does not carry far, and says only that
        // something moved there, not who.
        const auto heard = m_droneHeardAt.find(drone.owner);
        if (heard == m_droneHeardAt.end())
        {
            m_droneHeardAt[drone.owner] = drone.at;
        }
        else if (glm::distance(heard->second, drone.at) > kMotorHeardEvery)
        {
            MakeNoise(NoiseKind::Footstep, drone.at, kMotorReach, -1);
            heard->second = drone.at;
        }

        // And anything big enough that passes close enough swipes it out of the way.
        if (m_droneSwatCooldown[drone.owner] > 0.0f)
        {
            continue;
        }
        for (const std::unique_ptr<Creature>& creature : m_creatures)
        {
            if (!creature->Alive() || creature->Ragdolled())
            {
                continue;
            }
            const glm::vec3 from = creature->Position();
            glm::vec3 away = drone.at - from;
            away.y = 0.0f;
            const float apart = glm::length(away);
            if (apart > kSwatReach || std::abs(drone.at.y - from.y) > 1.5f)
            {
                continue;
            }
            away = apart > 0.05f ? away / apart : glm::vec3(1.0f, 0.0f, 0.0f);
            HitDrone(drone.owner, away * 38.0f + glm::vec3(0.0f, 22.0f, 0.0f), kSwatDamage);
            PlayNamed("Creature/swipe", drone.at, 0.8f, 1.0f, true);
            m_droneSwatCooldown[drone.owner] = kSwatEvery;
            break;
        }
    }
}

void PredationGame::UpdateWipe(float dt)
{
    // Everybody down is the end of the deployment, and everybody goes back -- to the testing area until
    // there is a ship to go back to.
    if (!IsAuthority() || m_screen != Screen::Playing || cv_droneWhenAlone.Get() || m_playingDrone)
    {
        m_wipeTimer = 0.0f;
        return;
    }
    bool anybodyUp = m_player.State().alive;
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        anybodyUp = anybodyUp || remote.alive;
    }
    if (anybodyUp)
    {
        m_wipeTimer = 0.0f;
        return;
    }
    // At a site, the deployment has failed: the crawler takes itself away, empty, and the shuttle goes, and the end of that
    // takes everybody back aboard. Not at once -- a moment to see it has happened.
    if (m_wipeTimer > 2.5f && m_map == MapChoice::Facility && HasCinematic("surface_wipe") && !m_missionLeaving &&
        m_mission.stage != MissionState::Stage::Over && m_mission.stage != MissionState::Stage::None)
    {
        m_missionLeaving = true;
        MissionRules::Finish(m_mission, false, 0);
        OnMissionOver();
        BroadcastMission();
        PlayCinematic("surface_wipe");
    }
    if (m_missionLeaving && m_map == MapChoice::Facility)
    {
        m_wipeTimer += 1.0e-3f;
        return;
    }
    if (m_wipeTimer <= 0.0f)
    {
        // Said to everybody, and on everybody's screen: otherwise the way back looks like any other.
        m_everybodyDownFor = cv_wipeSeconds.Get();
        if (m_sessionMode == SessionMode::Host)
        {
            WorldEventMessage event;
            event.kind = WorldEventKind::EverybodyDown;
            event.amount = m_everybodyDownFor;
            m_host.Broadcast(event);
        }
    }
    m_wipeTimer += dt;
    if (m_wipeTimer < cv_wipeSeconds.Get())
    {
        return;
    }
    m_wipeTimer = 0.0f;
    m_remoteRespawnTimers.clear();
    m_respawnTimer = 0.0f;
    GoToMap(MapChoice::TestMap);
}

bool PredationGame::DroneLamp(PunctualLight& light) const
{
    if (!m_supportDrone.Active() || !m_supportDrone.lightOn || m_supportDrone.Disabled())
    {
        return false;
    }
    DroneLampFor(m_supportDrone, light);
    return true;
}

void PredationGame::DroneLampFor(const SupportDrone& drone, PunctualLight& light) const
{
    const glm::vec3 forward = drone.LookDirection();
    light.position = drone.Eye() + forward * 0.08f;
    light.direction = forward;
    light.color = glm::vec3(1.0f, 0.97f, 0.9f);
    light.intensity = TorchIntensity() * kDroneLampStrength;
    light.range = TorchRange() * kDroneLampReach;
    light.innerAngle = TorchInnerAngle();
    light.outerAngle = TorchOuterAngle();
    light.sourceRadius = 0.03f;
}

void PredationGame::DrawDroneHud()
{
    if (!m_supportDrone.Active() || m_screen != Screen::Playing)
    {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 min = viewport->Pos;
    const ImVec2 max{viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
    const float width = viewport->Size.x;
    const float height = viewport->Size.y;

    // A camera's picture rather than eyes: its corners marked, and faint lines across it.
    const ImU32 frame = IM_COL32(210, 215, 220, 150);
    const float inset = 28.0f;
    const float arm = 36.0f;
    const ImVec2 corners[4] = {{min.x + inset, min.y + inset},
                               {max.x - inset, min.y + inset},
                               {min.x + inset, max.y - inset},
                               {max.x - inset, max.y - inset}};
    for (int i = 0; i < 4; ++i)
    {
        const float sx = (i % 2 == 0) ? 1.0f : -1.0f;
        const float sy = (i < 2) ? 1.0f : -1.0f;
        draw->AddLine(corners[i], {corners[i].x + arm * sx, corners[i].y}, frame, 2.0f);
        draw->AddLine(corners[i], {corners[i].x, corners[i].y + arm * sy}, frame, 2.0f);
    }
    for (float y = min.y; y < max.y; y += 4.0f)
    {
        draw->AddLine({min.x, y}, {max.x, y}, IM_COL32(0, 0, 0, 22), 1.0f);
    }

    // Shut down: no picture at all, only the noise of a feed with nothing on it, and how far along its
    // reboot is.
    if (m_supportDrone.Disabled())
    {
        std::minstd_rand noise(static_cast<uint32_t>(m_time * 60.0));
        std::uniform_int_distribution<int> grey(10, 90);
        draw->AddRectFilled(min, max, IM_COL32(12, 12, 14, 255));
        for (float y = min.y; y < max.y; y += 3.0f)
        {
            const int g = grey(noise);
            draw->AddRectFilled({min.x, y}, {max.x, y + 3.0f}, IM_COL32(g, g, g + 4, 255));
        }
        std::uniform_real_distribution<float> along(0.0f, 1.0f);
        for (int speck = 0; speck < 700; ++speck)
        {
            const float x = min.x + along(noise) * width;
            const float y = min.y + along(noise) * height;
            const int g = grey(noise) + 90;
            draw->AddRectFilled({x, y}, {x + 6.0f, y + 2.0f}, IM_COL32(g, g, g, 255));
        }
        const float progress = m_supportDrone.RebootProgress();
        const ImVec2 centre{min.x + width * 0.5f, min.y + height * 0.5f};
        const char* label = "NO SIGNAL";
        const ImVec2 size = ImGui::CalcTextSize(label);
        draw->AddRectFilled({centre.x - 110.0f, centre.y - 34.0f}, {centre.x + 110.0f, centre.y + 30.0f},
                            IM_COL32(0, 0, 0, 200));
        draw->AddText({centre.x - size.x * 0.5f, centre.y - 26.0f}, IM_COL32(230, 230, 230, 255), label);
        const char* rebooting = "Rebooting";
        const ImVec2 rebootSize = ImGui::CalcTextSize(rebooting);
        draw->AddText({centre.x - rebootSize.x * 0.5f, centre.y - 8.0f}, IM_COL32(170, 170, 170, 255), rebooting);
        draw->AddRect({centre.x - 90.0f, centre.y + 12.0f}, {centre.x + 90.0f, centre.y + 20.0f},
                      IM_COL32(170, 170, 170, 255));
        draw->AddRectFilled({centre.x - 88.0f, centre.y + 14.0f},
                            {centre.x - 88.0f + 176.0f * progress, centre.y + 18.0f}, IM_COL32(200, 200, 200, 255));
        return;
    }

    // What it has left, bottom left, where a living player's own condition is shown (the top left is the
    // microphone's).
    const ImVec2 at{min.x + inset + 12.0f, max.y - inset - 58.0f};
    draw->AddText(at, IM_COL32(220, 225, 230, 230), "DRONE");
    const float health = std::clamp(m_supportDrone.Health() / SupportDrone::kMaxHealth, 0.0f, 1.0f);
    const ImU32 barColour = health > 0.35f ? IM_COL32(200, 205, 210, 220) : IM_COL32(230, 110, 90, 230);
    draw->AddRect({at.x, at.y + 20.0f}, {at.x + 122.0f, at.y + 28.0f}, IM_COL32(200, 205, 210, 160));
    draw->AddRectFilled({at.x + 2.0f, at.y + 22.0f}, {at.x + 2.0f + 118.0f * health, at.y + 26.0f}, barColour);
    if (m_supportDrone.lightOn)
    {
        draw->AddText({at.x, at.y + 36.0f}, IM_COL32(220, 225, 230, 200), "LAMP");
    }

    // What can be done with it, bottom middle, and only what applies.
    std::string hint = "[" + KeyFor(m_app->GetInput(), "flashlight") + "] lamp";
    if (m_supportDrone.Upended())
    {
        hint = "[" + KeyFor(m_app->GetInput(), "jump") + "] right it    " + hint;
    }

    const ImVec2 hintSize = ImGui::CalcTextSize(hint.c_str());
    draw->AddText({min.x + (width - hintSize.x) * 0.5f, max.y - inset - 24.0f}, IM_COL32(200, 205, 210, 200),
                  hint.c_str());
}

} // namespace pred
