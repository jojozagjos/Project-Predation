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


// Sounds as they are played: footsteps by what is underfoot, what a wall between muffles, and the world's sounds each frame.

namespace pred
{

namespace
{
float StanceLoudness(PlayerStance stance)
{
    switch (stance)
    {
    case PlayerStance::Crouching:
        return 0.45f;
    case PlayerStance::Prone:
        return 0.25f;
    case PlayerStance::Standing:
    default:
        return 1.0f;
    }
}
} // namespace

void PredationGame::LoadFootsteps(AudioEngine& audio)
{
    m_footsteps.clear();
    m_footstepSurface = 0;

    const std::filesystem::path file = Paths::AssetsRoot() / "Data" / "footsteps.json";
    std::ifstream stream(file);
    if (!stream)
    {
        PRED_LOG_INFO(Gameplay, "No {}; footsteps fall back to the synthesised one",
                      file.filename().string());
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        PRED_LOG_WARN(Gameplay, "{} is not valid JSON; footsteps fall back to the synthesised one",
                      file.filename().string());
        return;
    }

    const std::filesystem::path folder = Paths::AssetsRoot() / "Audio" / "Footsteps";
    const auto surfaces = root.find("surfaces");
    if (surfaces == root.end() || !surfaces->is_object())
    {
        PRED_LOG_WARN(Gameplay, "{} has no surfaces", file.filename().string());
        return;
    }

    size_t trimmed = 0;
    for (const auto& [name, value] : surfaces->items())
    {
        if (!value.is_object())
        {
            continue;
        }
        FootstepSurface surface;
        surface.name = name;
        const auto gain = value.find("gain");
        if (gain != value.end() && gain->is_number())
        {
            surface.gain = gain->get<float>();
        }
        const auto clips = value.find("clips");
        if (clips == value.end() || !clips->is_array())
        {
            continue;
        }
        for (const auto& clip : *clips)
        {
            if (!clip.is_string())
            {
                continue;
            }
            const std::string leaf = clip.get<std::string>();
            const std::filesystem::path path = folder / leaf;
            std::ifstream wav(path, std::ios::binary);
            if (!wav)
            {
                PRED_LOG_WARN(Gameplay, "Footstep clip missing: {}", path.string());
                continue;
            }
            const std::string bytes((std::istreambuf_iterator<char>(wav)),
                                    std::istreambuf_iterator<char>());
            SoundData data;
            std::string error;
            if (!LoadWav(bytes.data(), bytes.size(), data, error))
            {
                PRED_LOG_WARN(Gameplay, "Footstep clip {}: {}", leaf, error);
                continue;
            }
            trimmed += TrimTrailingSilence(data);
            const SoundId id = audio.Add("step/" + name + "/" + leaf, std::move(data));
            if (id != kInvalidSound)
            {
                surface.clips.push_back(id);
            }
        }
        if (!surface.clips.empty())
        {
            m_footsteps.push_back(std::move(surface));
        }
    }

    // Whichever surface the file names, or the first one it described.
    const auto preferred = root.find("default_surface");
    if (preferred != root.end() && preferred->is_string())
    {
        const std::string want = preferred->get<std::string>();
        for (size_t i = 0; i < m_footsteps.size(); ++i)
        {
            if (m_footsteps[i].name == want)
            {
                m_footstepSurface = static_cast<int>(i);
                break;
            }
        }
    }

    if (m_footsteps.empty())
    {
        PRED_LOG_WARN(Gameplay,
                      "No footstep clips loaded; steps fall back to Assets/Audio/step_hard");
        return;
    }
    int clips = 0;
    for (const FootstepSurface& surface : m_footsteps)
    {
        clips += static_cast<int>(surface.clips.size());
    }
    PRED_LOG_INFO(Gameplay, "Footsteps: {} clips over {} surfaces, on '{}'. Trimmed {:.2f}s of "
                            "trailing silence.",
                  clips, m_footsteps.size(), m_footsteps[m_footstepSurface].name,
                  static_cast<double>(trimmed) / 48000.0);
}

int PredationGame::SurfaceIndexAt(const glm::vec3& position) const
{
    // What the foot is on: what the body under it is made of (PhysicsWorld::Tag), and the test map's surface row, standing on
    // it -- with a surface that has no clips of its own falling back on the nearest that has.
    const char* name = SurfaceUnderfoot(position.x, position.z);
    if (name == nullptr)
    {
        const RayHit under = m_app->GetPhysics().RayCastStatic(position + glm::vec3(0.0f, 0.4f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 1.2f);
        name = under ? Surfaces::StepName(m_app->GetPhysics().Tag(under.body)) : nullptr;
        // On a site's own ground, where its rock shows through it (Surfaces::OutcropAt, as the ground is drawn): the rock's.
        if (under && m_facility.Built() && m_facility.Contains(position) && !m_facility.GetLook().steep.empty())
        {
            const SiteTerrain& ground = m_facility.Terrain();
            const SiteMap::Look& look = m_facility.GetLook();
            const bool onGround = std::abs(under.position.y - ground.Height(position.x, position.z)) < 0.15f;
            if (onGround && Surfaces::OutcropAt(position.x, position.z, ground.Normal(position.x, position.z).y))
            {
                name = Surfaces::StepName(Surfaces::StepOfSet(look.steep, false));
            }
            // Or where its second ground lies across it (Surfaces::BlendAt): that one's.
            else if (onGround && !look.blend.empty() &&
                     Surfaces::BlendAt(position.x, position.z, under.position.y, look.blendShare, ground.HasWater() ? ground.WaterLevel() : -1.0e9f,
                                       ground.HasWater() ? look.blendShore : 0.0f))
            {
                name = Surfaces::StepName(Surfaces::StepOfSet(look.blend, false));
            }
        }
    }
    for (int hops = 0; name != nullptr && hops < 3; ++hops)
    {
        for (size_t i = 0; i < m_footsteps.size(); ++i)
        {
            if (m_footsteps[i].name == name && !m_footsteps[i].clips.empty())
            {
                return static_cast<int>(i);
            }
        }
        const std::string missing = name;
        name = missing == "snow" || missing == "sand" || missing == "grass" || missing == "dirt" ? "gravel"
               : missing == "ice"                                                                                   ? "stone"
                                                                                                                    : nullptr;
    }
    return m_footstepSurface;
}

SoundId PredationGame::PickFootstep(float& gain, int surfaceIndex) const
{
    if (m_footsteps.empty())
    {
        return m_sounds.step.Pick();
    }
    const FootstepSurface& surface =
        m_footsteps[static_cast<size_t>(std::clamp(surfaceIndex, 0,
                                                   static_cast<int>(m_footsteps.size()) - 1))];
    if (surface.clips.empty())
    {
        return m_sounds.step.Pick();
    }
    // Not round-robin. Two clips alternating strictly is a pattern the ear finds within about six
    // steps; picking at random and refusing an immediate repeat is not.
    size_t pick = static_cast<size_t>(m_footstepRandom() % surface.clips.size());
    if (surface.clips.size() > 1 && surface.clips[pick] == m_lastFootstep)
    {
        pick = (pick + 1) % surface.clips.size();
    }
    m_lastFootstep = surface.clips[pick];
    gain *= surface.gain;
    return m_lastFootstep;
}

void PredationGame::PlaySound(SoundId sound, const glm::vec3& at, float gain, float pitch,
                              bool positioned, bool helmet)
{
    if (sound == kInvalidSound)
    {
        return;
    }
    // Every sound, by name, at debug level: sound is the one thing with nothing to look at, and "what
    // is that noise" is otherwise a question with no way to answer it. Checked first, because the
    // name lookup walks a table.
    if (Log::Get(LogCategory::Audio).should_log(spdlog::level::debug))
    {
        PRED_LOG_DEBUG(Audio, "Sound '{}' gain {:.2f} pitch {:.2f}{}", m_app->GetAudio().NameOf(sound), gain,
                       pitch, positioned ? "" : " (not placed)");
    }
    AudioEngine::PlayDesc desc;
    desc.sound = sound;
    desc.position = at;
    desc.positioned = positioned;
    desc.gain = gain * std::clamp(cv_effectsVolume.Get(), 0.0f, 2.0f);
    desc.pitch = pitch;
    desc.occlusion = positioned ? OcclusionAt(at) : 0.0f;
    desc.helmet = helmet;
    m_app->GetAudio().Play(desc);
}

float PredationGame::OcclusionAt(const glm::vec3& at) const
{
    // Straight from the ears, and again from a little above them: a wall between is both lines
    // blocked, and a crate or a doorframe in the way only one -- round a corner a sound is thinner, not
    // gone.
    const PhysicsWorld& physics = m_app->GetPhysics();
    const auto blocked = [&](const glm::vec3& from, const glm::vec3& to)
    {
        const glm::vec3 along = to - from;
        const float length = glm::length(along);
        if (length < 1.5f)
        {
            return false;
        }
        const RayHit hit = physics.RayCastStatic(from, along / length, length - 0.3f);
        return static_cast<bool>(hit);
    };
    const glm::vec3 source = at + glm::vec3(0.0f, 0.3f, 0.0f);
    const bool straight = blocked(m_renderEye, source);
    if (!straight)
    {
        return 0.0f;
    }
    const bool over = blocked(m_renderEye + glm::vec3(0.0f, 0.8f, 0.0f), source + glm::vec3(0.0f, 0.8f, 0.0f));
    return over ? 1.0f : 0.5f;
}

void PredationGame::UpdateSounds(float dt)
{
    AudioEngine& audio = m_app->GetAudio();

    // Where the ears are. The camera rather than the body, because the camera is what the player is
    // looking through: spectating a teammate, their ears are the ones that matter.
    audio.SetListener(m_renderEye, m_renderForward, glm::vec3(0.0f, 1.0f, 0.0f));

    // Walking on the spot for the sound panel. Above the check below on purpose, so it keeps going
    // while the game is paused: judging a footstep means listening to it and not to the game.
    if (m_footstepAudition > 0.0f)
    {
        m_footstepAudition += dt;
        if (m_footstepAudition >= m_footstepCadence)
        {
            m_footstepAudition -= m_footstepCadence;
            float gain = 0.30f;
            const SoundId clip = PickFootstep(gain, m_footstepSurface);
            PlaySound(clip, m_camera.position, gain, 0.96f + 0.08f * m_footstepAudition, false);
        }
    }

    UpdateAmbience(dt);
    UpdateTension(dt);
    UpdateMusic(dt);
    if (m_screen != Screen::Playing)
    {
        return;
    }
    UpdateWorldSounds(dt);

    // Footsteps, from the stride the body is already walking to.
    //
    // Not from a timer of its own: the leg animation runs on a phase that goes round once per pair
    // of steps, and a foot lands as it passes nought and a half. Reading that is the only way the
    // sound and the foot arrive together, and a footstep that lands a tenth of a second from the
    // foot is worse than none.
    // How far a step carries, for the creature, which is not how loud it is in anybody's ears: a
    // creature listening for prey hears a sprint across the building and a crawl from arm's length.
    const auto stepReach = [](PlayerStance stance, const glm::vec3& velocity)
    {
        const float speed = glm::length(glm::vec2(velocity.x, velocity.z));
        if (stance == PlayerStance::Prone)
        {
            return NoiseReach::kFootstepProne;
        }
        if (stance == PlayerStance::Crouching)
        {
            return NoiseReach::kFootstepCrouch;
        }
        if (speed > 4.5f)
        {
            return NoiseReach::kFootstepSprint;
        }
        // Walking slowly on purpose is most of the way to crouching.
        return speed < 2.2f ? NoiseReach::kFootstepCrouch + 0.5f : NoiseReach::kFootstepWalk;
    };

    const auto footfalls = [&](float phase, float& previous, const glm::vec3& where, const glm::vec3& velocity, bool moving,
                               float gain, bool positioned, int player, float reach)
    {
        if (!moving)
        {
            previous = phase;
            return;
        }
        const auto crossed = [&](float mark)
        { return (previous < mark && phase >= mark) || (previous > phase && mark < 0.25f); };
        if (crossed(0.0f) || crossed(0.5f))
        {
            // A little variation in pitch, or twenty identical steps in a row read as a machine.
            // With recorded clips the random pick does most of that work and the pitch nudge only
            // has to stop two plays of the same clip being identical, so it is gentler than it was.
            float loudness = gain;
            const int surface = SurfaceIndexAt(where);
            const SoundId clip = PickFootstep(loudness, surface);
            const float wobble = 0.96f + 0.08f * std::fmod(std::abs(phase) * 7.13f, 1.0f);
            // How deep in water (or acid) the foot comes down, where there is any. (Not on ice: that is ground.)
            const SiteMap::Look::Liquid liquid = m_facility.GetLook().liquid;
            const bool wet = m_facility.Built() && m_facility.Contains(where) && (liquid == SiteMap::Look::Liquid::Water || liquid == SiteMap::Look::Liquid::Acid);
            const float depth = wet ? m_facility.Terrain().WaterDepth(where.x, where.z) : 0.0f;
            // The ground underfoot, less of it the deeper the water over it: by the knees, only the water is heard.
            PlaySound(clip, where, loudness * (1.0f - glm::smoothstep(0.05f, 0.35f, depth)), wobble, positioned);
            MakeNoise(NoiseKind::Footstep, where, reach, player);
            // In water: the foot going through it, louder the deeper; past the knees a heavier slosh as well, and the drops
            // thrown up falling back.
            if (depth > 0.03f)
            {
                const float level = m_facility.Terrain().WaterLevel();
                const float deep = std::min(depth / 0.6f, 1.0f);
                PlayNamed("Player/wade", {where.x, level, where.z}, gain * (0.45f + 0.55f * deep), wobble * (1.05f - 0.1f * deep), positioned);
                ParticleLook splash;
                splash.colour = splash.endColour = liquid == SiteMap::Look::Liquid::Acid ? glm::vec3(0.55f, 0.6f, 0.2f) : glm::vec3(0.55f, 0.6f, 0.65f);
                splash.glow = 0.05f;
                splash.size = 0.025f;
                splash.endSize = 0.012f;
                splash.life = 0.5f;
                splash.speed = 1.6f + 1.4f * std::min(depth, 0.6f);
                splash.spread = 0.7f;
                splash.gravity = 9.8f;
                splash.drag = 0.6f;
                splash.streak = true;
                splash.vanishOnLanding = true;
                m_particles.Emit(m_scene, m_app->GetMeshes(), {where.x, level + 0.02f, where.z}, glm::vec3(0.0f, 1.0f, 0.0f), 10, splash, level);
                if (depth > 0.4f)
                {
                    PlayNamed("World/splash", {where.x, level, where.z}, 0.25f * glm::smoothstep(0.4f, 1.0f, depth), 0.9f + 0.2f * glm::fract(where.x * 13.7f + where.z * 7.3f), true);
                }
            }
            // And in soft ground at a site, a print where the foot came down: the left foot at one mark, the right at the other --
            // in sand, snow and soft earth, in any ground with snow lying on it, and in grass or earth gone to mud in the rain; on
            // the ground itself (not a pad or a floor). Filled in sooner while snow is falling or sand blowing, and in rain.
            if (surface >= 0 && surface < static_cast<int>(m_footsteps.size()) && m_facility.Built() && m_facility.Contains(where))
            {
                const std::string& underfoot = m_footsteps[static_cast<size_t>(surface)].name;
                const bool onGround = std::abs(where.y - m_facility.Terrain().Height(where.x, where.z)) < 0.25f;
                const bool soft = underfoot == "sand" || underfoot == "snow" || underfoot == "dirt";
                const bool snowedOver = m_surfaceSnow > 0.3f;
                const bool muddy = m_surfaceWet > 0.5f && (underfoot == "grass" || underfoot == "dirt");
                if (onGround && depth < 0.02f && (soft || snowedOver || muddy))
                {
                    const SiteMap::Look& look = m_facility.GetLook();
                    const Weather::Kind kind = Weather::KindOf(look.weather);
                    const float amount = std::clamp(look.weatherAmount, 0.0f, 1.0f);
                    double last = Footprints::kLast;
                    if (kind == Weather::Kind::Snow)
                    {
                        last = glm::mix(Footprints::kLast, 30.0, static_cast<double>(glm::smoothstep(0.1f, 0.8f, amount)));
                    }
                    else if (kind == Weather::Kind::Dust)
                    {
                        last = glm::mix(Footprints::kLast, 25.0, static_cast<double>(glm::smoothstep(0.2f, 0.9f, amount)));
                    }
                    else if (kind == Weather::Kind::Rain)
                    {
                        last = glm::mix(Footprints::kLast, 70.0, static_cast<double>(amount));
                    }
                    const float yaw = std::atan2(velocity.x, -velocity.z);
                    m_footprints.Leave(m_scene, m_app->GetMeshes(), m_facility, where, yaw, crossed(0.0f), m_campaign.clock, last);
                }
            }
        }
        previous = phase;
    };

    const PlayerState& local = m_player.State();
    const bool localMoving = local.alive && local.grounded &&
                             glm::length(glm::vec2(local.velocity.x, local.velocity.z)) > 0.8f;
    // Your own steps are not placed in the world either: they come from under you, and panning
    // them puts your own feet in one ear.
    footfalls(local.stridePhase, m_lastStridePhase, local.position, local.velocity, localMoving,
              0.30f * StanceLoudness(local.stance), false, LocalPlayerId(),
              stepReach(local.stance, local.velocity));

    for (const std::unique_ptr<RemoteAvatar>& avatar : m_avatars)
    {
        const bool moving = avatar->state.alive && avatar->state.grounded &&
                            glm::length(glm::vec2(avatar->state.velocity.x,
                                                  avatar->state.velocity.z)) > 0.8f;
        footfalls(avatar->state.stridePhase, avatar->lastStridePhase, avatar->drawn, avatar->state.velocity, moving,
                  0.75f * StanceLoudness(avatar->state.stance), true, avatar->id,
                  stepReach(avatar->state.stance, avatar->state.velocity));
    }

    // And landing, which is an event rather than a phase.
    if (local.landedThisTick && local.fallPeakSpeed > 2.0f)
    {
        const float force = std::clamp(local.fallPeakSpeed / 9.0f, 0.2f, 1.0f);
        PlaySound(m_sounds.land.Pick(), local.position, force * 0.7f, 1.0f, false);
        MakeNoise(NoiseKind::Impact, local.position, NoiseReach::kImpact * force, LocalPlayerId());
    }

}


} // namespace pred
