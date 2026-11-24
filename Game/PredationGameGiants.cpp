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
    // (A door shut across the way is as solid as a wall: the level alone left them out -- they move -- and a strider reached in after
    // somebody through a shut airlock -- you, 2026-11-17. Anything else the line meets -- somebody, a creature, its own strands -- it
    // goes on through.)
    world.clear = [this, &physics](const glm::vec3& from, const glm::vec3& to)
    {
        const glm::vec3 along = to - from;
        const float length = glm::length(along);
        if (length < 1.0f)
        {
            return true;
        }
        const glm::vec3 way = along / length;
        if (physics.RayCastStatic(from, way, length - 0.6f))
        {
            return false;
        }
        glm::vec3 start = from;
        float left = length - 0.6f;
        for (int pass = 0; pass < 6 && left > 0.0f; ++pass)
        {
            const RayHit hit = physics.RayCast(start, way, left);
            if (!hit)
            {
                return true;
            }
            for (const WorldObjects::Door& door : m_world.Doors())
            {
                if (door.body == hit.body)
                {
                    return false;
                }
            }
            const float past = hit.distance + 0.05f;
            start += way * past;
            left -= past;
        }
        return true;
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
        // And what stands out in the open big enough to matter: the landing pad (on the sea, the floating one and the posts it is
        // moored on, which the leviathan swam through) with the shuttle on it, tanks, masts, containers, the field generator.
        const float ground = site.Terrain().GroundHeight(x, z);
        const auto standing = [&](const glm::vec3& middle, float radius, float top)
        {
            const glm::vec2 apart{x - middle.x, z - middle.z};
            if (glm::dot(apart, apart) < radius * radius)
            {
                highest = std::max(highest, std::max(top - ground, 1.0f));
            }
        };
        const SitePlan& plan = site.Plan();
        standing(plan.landing, 16.0f, plan.landing.y + 8.0f);
        for (const SitePlan::Block& block : plan.blocks)
        {
            if (block.kind == SitePlan::BlockKind::Pad || block.kind == SitePlan::BlockKind::Tank || block.kind == SitePlan::BlockKind::Mast ||
                block.kind == SitePlan::BlockKind::Container)
            {
                standing(block.centre, std::max(block.size.x, block.size.z) * 0.6f + 6.0f, block.centre.y + block.size.y * 0.5f);
            }
        }
        return highest;
    };
    // A roof's top where one is (its building's own walls, no margin round them): what a strider can put its foot on.
    world.roof = [&site](float x, float z)
    {
        float top = -1.0e9f;
        for (const FacilityLayout& building : site.Plan().buildings)
        {
            glm::vec2 low;
            glm::vec2 high;
            // (Not a half-fallen one: its roof is broken, and a foot put on it would stand on nothing.)
            if (building.ruin.max.x >= building.ruin.min.x)
            {
                continue;
            }
            SitePlan::Footprint(building, 0.0f, low, high);
            if (x > low.x && x < high.x && z > low.y && z < high.y)
            {
                top = std::max(top, building.origin.y + static_cast<float>(building.floors) * FacilityLayout::kStorey);
            }
        }
        return top;
    };
    const SitePlan& plan = site.Plan();
    world.middle = glm::vec2(plan.origin.x, plan.origin.z) + glm::vec2(plan.size * 0.5f);
    world.reach = std::max(plan.size * 0.5f + 140.0f, 220.0f);
    return world;
}

bool PredationGame::InSiteShuttle(const glm::vec3& at) const
{
    // Inside the shuttle's cabin, on its pad: out of a giant's reach and its sight, as under a roof (you, 2026-11-15: it reached in).
    CinePose cabin;
    if (!m_facility.Shuttle().Built() || !m_facility.Shuttle().SocketShown("lamp", cabin))
    {
        return false;
    }
    glm::vec3 low;
    glm::vec3 high;
    ShipSpec::ShuttleCabinBounds(cabin, low, high);
    return glm::all(glm::greaterThan(at, low - glm::vec3(0.4f))) && glm::all(glm::lessThan(at, high + glm::vec3(0.4f)));
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
        const auto sense = [&](int id, const glm::vec3& feet, const glm::vec3& velocity, PlayerStance stance, bool alive, bool torch, float yaw, float pitch)
        {
            GiantSensed who;
            who.look = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
            who.id = id;
            who.feet = feet;
            who.velocity = velocity;
            who.height = BodyHeight(stance);
            who.alive = alive;
            who.light = LightAt(feet, torch);
            const glm::vec3 chest = feet + glm::vec3(0.0f, 1.0f, 0.0f);
            who.roofed = m_facility.Indoors(chest) || m_facility.Sealed(chest) || InSiteShuttle(chest);
            float surface = 0.0f;
            // (In it: under its surface, not only over water -- WaterAt says how deep the water is there, whatever the height asked at,
            // so anybody up on the landing platform over the sea counted as in it, and the leviathan went on hunting them and circling
            // under the deck: you, 2026-11-24; FIX 752. Half a metre of it over the chest, or swimming.)
            who.inWater = !who.roofed && m_facility.WaterAt(chest, surface) > 0.0f && chest.y < surface + 0.3f;
            people.push_back(who);
        };
        const PlayerState& local = m_player.State();
        sense(LocalPlayerId(), local.position, local.velocity, local.stance, local.alive && m_hidingSpot < 0, m_torchOn || m_flareBurn > 0.0f, m_lookYaw, m_lookPitch);
        if (m_sessionMode == SessionMode::Host)
        {
            for (const RemotePlayerView& remote : m_host.Remotes())
            {
                if (remote.id != LocalPlayerId())
                {
                    sense(remote.id, remote.position, remote.velocity, remote.stance, remote.alive, remote.torchOn, remote.yaw, remote.pitch);
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
            }
            // Let go only when it lets go (Giant::Blow::release): thrown, done with, shot off, or killed -- never on a bite.
            if (blow.release && blow.player == m_giantHeld)
            {
                ReleaseGiantHold(m_giant->Goal().c_str());
            }
            // A leviathan's bite heard as it lands.
            if (blow.damage > 0.0f && m_giant->Kind() == GiantKind::Swimmer)
            {
                PlayGiant("Creature/bite", blow.at, 1.0f, 0.5f);
            }
            if (blow.damage > 0.0f)
            {
                ApplyPlayerDamage(static_cast<uint8_t>(blow.player), blow.damage, kNoKiller, glm::normalize(blow.push + glm::vec3(0.0f, 0.01f, 0.0f)), "giant");
            }
            if (blow.player == LocalPlayerId() && glm::length(blow.push) > 0.0f)
            {
                m_player.State().velocity += blow.push;
            }
        }
        UpdateGiantHold(dt);
        // The leviathan heard in the water now and then: a long low call, carrying.
        // (Never dead: it went on calling in the water after it was killed -- you, 2026-11-23.)
        if (m_giant->Kind() == GiantKind::Swimmer && !m_giant->Dead() && m_creatureClock >= m_giantMoanAt)
        {
            m_giantMoanAt = m_creatureClock + 18.0f + static_cast<float>(std::rand() % 22);
            PlayGiant(std::rand() % 2 == 0 ? "Creature/call" : "Creature/growl", m_giant->Head(), 1.0f, 0.22f + 0.06f * static_cast<float>(std::rand() % 4) / 4.0f);
        }
        // Its rush heard, a roar coming at you through the water.
        if (m_giant->TakeRush())
        {
            PlayGiant("Creature/lunge", m_giant->Head(), 1.0f, 0.4f);
        }
        if (m_giant->TakeCall())
        {
            PlayGiant("Creature/call", m_giant->Head(), 1.0f, m_giant->Kind() == GiantKind::Swimmer ? 0.3f : 0.38f);
        }
    }
    m_giant->Animate(world, m_creatureClock, dt);
    // A leviathan's body is solid to whoever is in the water with it (you, 2026-11-16: it went round you and through you): your own body,
    // here on your own machine, shoved out of its by the way out nearest, and carried along by it as it passes.
    if (m_giant->Kind() == GiantKind::Swimmer && m_player.State().alive && m_giantHeld != LocalPlayerId())
    {
        PlayerState& state = m_player.State();
        const glm::vec3 chest = state.position + glm::vec3(0.0f, 0.9f, 0.0f);
        const glm::vec3 out = m_giant->OutOfBody(chest, 0.45f);
        const float deep = glm::length(out);
        if (deep > 0.01f)
        {
            const glm::vec3 way = out / deep;
            const float into = glm::dot(state.velocity, way);
            if (into < 0.0f)
            {
                state.velocity -= way * into;
            }
            state.velocity += way * std::min(deep * 10.0f, 7.0f);
        }
    }
    // Its feet coming down: heard a long way off, felt through the ground near by, and a noise to everything else that hears.
    for (const glm::vec3& foot : m_giant->TakeFootfalls())
    {
        PlayGiant("Creature/step_heavy", foot, 1.0f, std::clamp(0.55f - m_giant->Size() * 0.008f, 0.28f, 0.45f));
        const float away = glm::distance(foot, m_renderEye);
        // (Felt within one and a half times its size, not four: a big one walking a couple of hundred metres off kept the picture -- the
        // weapon, the hands -- shaking as you stood still.)
        m_tremor = std::max(m_tremor, std::clamp(1.0f - away / (m_giant->Size() * 1.5f), 0.0f, 1.0f));
        MakeNoise(NoiseKind::Impact, foot, 60.0f, -1);
    }
    // Its blow coming down: the ground jumping under it, heard as a great weight hitting the ground, and anybody near it thrown back
    // off their feet whether it had them or not -- each machine its own player.
    for (const glm::vec3& slam : m_giant->TakeSlams())
    {
        PlayGiant("Creature/step_heavy", slam, 1.0f, 0.25f);
        PlayGiant("World/door_bash", slam, 1.0f, 0.35f);
        const float away = glm::distance(slam, m_renderEye);
        m_tremor = std::max(m_tremor, 1.6f * std::clamp(1.0f - away / (m_giant->Size() * 1.2f), 0.0f, 1.0f));
        MakeNoise(NoiseKind::Impact, slam, 120.0f, -1);
        PlayerState& state = m_player.State();
        const glm::vec3 off{state.position.x - slam.x, 0.0f, state.position.z - slam.z};
        const float near = glm::length(off);
        if (state.alive && near < 9.0f && !m_player.IsAttached())
        {
            const glm::vec3 way = near > 0.1f ? off / near : glm::vec3(1.0f, 0.0f, 0.0f);
            const float hard = 1.0f - near / 9.0f;
            state.velocity += way * (4.0f + 7.0f * hard) + glm::vec3(0.0f, 2.5f + 3.0f * hard, 0.0f);
        }
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
        m_giant->Drop(m_creatureClock, "they struggled free");
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
    const Giant::ShownExtras extras = m_giant->Extras(m_creatureClock);
    out.peek = std::clamp(extras.peek, 0.0f, 1.0f);
    out.flags = static_cast<uint8_t>((extras.frozen ? 1u : 0u) | (extras.reachingIn ? 2u : 0u) | (extras.eats ? 4u : 0u) | (extras.lowering ? 8u : 0u));
    out.holding = static_cast<int8_t>(std::clamp(extras.holding, -1, 6));
    out.reachDoor = extras.reachDoor;
    out.reachInside = extras.reachInside;
    out.thrown = extras.thrown;
    out.pullOut = extras.pullOut;
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
    Giant::ShownExtras extras;
    extras.peek = state.peek;
    extras.frozen = (state.flags & 1u) != 0u;
    extras.reachingIn = (state.flags & 2u) != 0u;
    extras.eats = (state.flags & 4u) != 0u;
    extras.lowering = (state.flags & 8u) != 0u;
    extras.holding = state.holding;
    extras.reachDoor = state.reachDoor;
    extras.reachInside = state.reachInside;
    extras.thrown = state.thrown;
    extras.pullOut = state.pullOut;
    m_giant->SetShownExtras(extras, m_creatureClock);
}

} // namespace pred
