#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Game/PredationGameShared.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

// The giants (Giant): put in a site, thought for on the host, shown and heard everywhere, felt through the ground.

namespace pred
{

GiantWorld PredationGame::GiantWorldHere()
{
    GiantWorld world;
    if (!m_facility.Built())
    {
        return world;
    }
    const SiteMap& site = m_facility;
    PhysicsWorld& physics = m_app->GetPhysics();
    world.ground = [&site](float x, float z) { return site.Terrain().GroundHeight(x, z); };
    world.water = [&site](const glm::vec3& at, float& surface) { return site.WaterAt(at, surface); };
    world.roofed = [&site](const glm::vec3& at) { return site.Indoors(at) || site.Sealed(at); };
    world.clear = [&physics](const glm::vec3& from, const glm::vec3& to)
    {
        const glm::vec3 along = to - from;
        const float length = glm::length(along);
        return length < 1.0f || !physics.RayCastStatic(from, along / length, length - 0.6f);
    };
    // How high what is built stands at a place, over the ground there (0: nothing): its buildings' roofs, a margin round them.
    world.builtUp = [&site](float x, float z)
    {
        float highest = 0.0f;
        for (const FacilityLayout& building : site.Plan().buildings)
        {
            glm::vec2 low;
            glm::vec2 high;
            SitePlan::Footprint(building, 6.0f, low, high);
            if (x > low.x && x < high.x && z > low.y && z < high.y)
            {
                const float top = building.origin.y + static_cast<float>(building.floors) * FacilityLayout::kStorey - site.Terrain().GroundHeight(x, z);
                highest = std::max(highest, std::max(top, 1.0f));
            }
        }
        return highest;
    };
    const SitePlan& plan = site.Plan();
    world.middle = glm::vec2(plan.origin.x, plan.origin.z) + glm::vec2(plan.size * 0.5f);
    world.reach = std::max(plan.size * 0.5f + 140.0f, 220.0f);
    return world;
}

bool PredationGame::SpawnGiant(GiantKind kind, uint32_t seed, const glm::vec3& at, float yaw)
{
    if (!m_facility.Built())
    {
        return false;
    }
    m_giant = std::make_unique<Giant>(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), kind, seed, at, yaw);
    return true;
}

void PredationGame::ClearGiant()
{
    ReleaseGiantHold("gone");
    m_giant.reset();
}

void PredationGame::UpdateGiant(float dt)
{
    if (!m_giant)
    {
        return;
    }
    const GiantWorld world = GiantWorldHere();
    if (IsAuthority())
    {
        // Everybody, as it could know them: where, how lit, under a roof or not, in the water or not.
        std::vector<GiantSensed> people;
        const auto sense = [&](int id, const glm::vec3& feet, const glm::vec3& velocity, PlayerStance stance, bool alive, bool torch)
        {
            GiantSensed who;
            who.id = id;
            who.feet = feet;
            who.velocity = velocity;
            who.height = BodyHeight(stance);
            who.alive = alive;
            who.light = LightAt(feet, torch);
            const glm::vec3 chest = feet + glm::vec3(0.0f, 1.0f, 0.0f);
            who.roofed = m_facility.Indoors(chest) || m_facility.Sealed(chest);
            float surface = 0.0f;
            who.inWater = !who.roofed && m_facility.WaterAt(chest, surface) > 0.0f;
            people.push_back(who);
        };
        const PlayerState& local = m_player.State();
        sense(LocalPlayerId(), local.position, local.velocity, local.stance, local.alive && m_hidingSpot < 0, m_torchOn || m_flareBurn > 0.0f);
        if (m_sessionMode == SessionMode::Host)
        {
            for (const RemotePlayerView& remote : m_host.Remotes())
            {
                if (remote.id != LocalPlayerId())
                {
                    sense(remote.id, remote.position, remote.velocity, remote.stance, remote.alive, remote.torchOn);
                }
            }
        }
        m_giant->Think(world, people, m_creatureClock, dt);
        for (const Giant::Blow& blow : m_giant->TakeBlows())
        {
            if (blow.grab)
            {
                // Taken up: held where it holds them until it is done with them, they struggle free, or it is shot into letting go.
                m_giantHeld = blow.player;
                m_giantStruggle = 0.0f;
                ShareSound("Creature/grab", blow.at, 0.8f);
                continue;
            }
            if (blow.player == m_giantHeld)
            {
                ReleaseGiantHold("it was done with them");
            }
            if (blow.damage > 0.0f)
            {
                ApplyPlayerDamage(static_cast<uint8_t>(blow.player), blow.damage, kNoKiller, glm::normalize(blow.push + glm::vec3(0.0f, 0.01f, 0.0f)), "giant");
            }
            if (blow.player == LocalPlayerId())
            {
                m_player.State().velocity += blow.push;
            }
        }
        UpdateGiantHold(dt);
        // The leviathan heard in the water now and then: a long low call, carrying.
        if (m_giant->Kind() == GiantKind::Swimmer && m_creatureClock >= m_giantMoanAt)
        {
            m_giantMoanAt = m_creatureClock + 18.0f + static_cast<float>(std::rand() % 22);
            PlayGiant(std::rand() % 2 == 0 ? "Creature/call" : "Creature/growl", m_giant->Head(), 1.0f, 0.22f + 0.06f * static_cast<float>(std::rand() % 4) / 4.0f);
        }
        if (m_giant->TakeCall())
        {
            PlayGiant("Creature/call", m_giant->Head(), 1.0f, m_giant->Kind() == GiantKind::Swimmer ? 0.3f : 0.38f);
        }
    }
    m_giant->Animate(world, m_creatureClock, dt);
    // Its feet coming down: heard a long way off, felt through the ground near by, and a noise to everything else that hears.
    for (const glm::vec3& foot : m_giant->TakeFootfalls())
    {
        PlayGiant("Creature/step_heavy", foot, 1.0f, std::clamp(0.55f - m_giant->Size() * 0.008f, 0.28f, 0.45f));
        const float away = glm::distance(foot, m_renderEye);
        m_tremor = std::max(m_tremor, std::clamp(1.0f - away / (m_giant->Size() * 4.0f), 0.0f, 1.0f));
        MakeNoise(NoiseKind::Impact, foot, 60.0f, -1);
    }
    m_tremor = std::max(m_tremor - dt * 2.2f, 0.0f);
}

void PredationGame::UpdateGiantHold(float dt)
{
    if (m_giantHeld < 0 || !m_giant)
    {
        return;
    }
    if (m_giant->Holding() != m_giantHeld)
    {
        ReleaseGiantHold("it let go");
        return;
    }
    // Struggling: every fresh press of jump works them a little loose (harder than out of a creature's grip).
    int presses = 0;
    if (m_giantHeld == LocalPlayerId())
    {
        presses = m_lastInput.jump && !m_giantJumpWasDown ? 1 : 0;
        m_giantJumpWasDown = m_lastInput.jump;
    }
    else if (m_sessionMode == SessionMode::Host)
    {
        presses = m_host.TakeJumpPresses(static_cast<uint8_t>(m_giantHeld));
    }
    m_giantStruggle = std::max(m_giantStruggle + 0.06f * static_cast<float>(presses) - 0.04f * dt, 0.0f);
    if (m_giantStruggle >= 1.0f)
    {
        m_giant->Drop(m_creatureClock);
        ReleaseGiantHold("struggled free");
        return;
    }
    PinPlayer(static_cast<uint8_t>(m_giantHeld), kGiantHolder, m_giant->HoldPoint(), m_giant->Yaw());
}

void PredationGame::ReleaseGiantHold(const char* why)
{
    if (m_giantHeld < 0)
    {
        return;
    }
    const uint8_t who = static_cast<uint8_t>(m_giantHeld);
    m_giantHeld = -1;
    PinPlayer(who, kNotHeld, m_giant ? m_giant->HoldPoint() : m_player.State().position, 0.0f);
    PRED_LOG_INFO(AI, "The giant let player {} go: {}", who, why);
}

void PredationGame::PlayGiant(const std::string& name, const glm::vec3& at, float gain, float pitch)
{
    // Heard a long way off, as something that size is: full within thirty metres, gone past seven hundred.
    AudioEngine::PlayDesc desc;
    desc.sound = Sounds(name).Pick();
    if (desc.sound == kInvalidSound)
    {
        return;
    }
    desc.position = at;
    desc.gain = gain * std::clamp(cv_effectsVolume.Get(), 0.0f, 2.0f);
    desc.pitch = pitch;
    desc.nearDistance = 30.0f;
    desc.farDistance = 700.0f;
    desc.occlusion = OcclusionAt(at) * 0.6f;
    m_app->GetAudio().Play(desc);
}

void PredationGame::FillGiantState(GiantSnapshot& out) const
{
    out = GiantSnapshot{};
    if (!m_giant)
    {
        return;
    }
    out.present = true;
    out.kind = static_cast<uint8_t>(m_giant->Kind());
    out.seed = m_giant->Seed();
    out.position = m_giant->Position();
    out.yaw = m_giant->Yaw();
    out.mode = static_cast<uint8_t>(m_giant->CurrentMode());
    out.lookAt = m_giant->LookAt();
    out.strike = m_giant->StrikePhase();
    out.strikeAt = m_giant->StrikeAt();
}

void PredationGame::ApplyGiantState(const GiantSnapshot& state)
{
    if (!state.present)
    {
        ClearGiant();
        return;
    }
    const GiantKind kind = state.kind == 1 ? GiantKind::Swimmer : GiantKind::Walker;
    if (!m_giant || m_giant->Seed() != state.seed || m_giant->Kind() != kind)
    {
        if (!SpawnGiant(kind, state.seed, state.position, state.yaw))
        {
            return;
        }
    }
    m_giant->SetShown(state.position, state.yaw, static_cast<Giant::Mode>(std::min<uint8_t>(state.mode, static_cast<uint8_t>(Giant::Mode::Count) - 1)),
                      state.lookAt, state.strike, state.strikeAt);
}

} // namespace pred
