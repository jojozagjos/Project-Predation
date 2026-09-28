#include "Game/Cinematic/CinematicPlayer.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace pred
{
namespace
{

constexpr size_t kHistory = 40;

glm::mat4 Matrix(const CinePose& pose)
{
    return glm::translate(glm::mat4(1.0f), pose.position) * glm::mat4_cast(pose.rotation);
}

// The last of an actor's clips started by `time`, and how far into it.
const ClipEvent* ClipAt(const Cinematic& cinematic, const std::string& actor, float time)
{
    const ClipEvent* found = nullptr;
    for (const ClipEvent& clip : cinematic.clips)
    {
        if (clip.actor == actor && clip.time <= time)
        {
            found = &clip;
        }
    }
    return found;
}

// Whether anything in the cinematic moves this actor: otherwise a bound one is left where the game has it.
bool Moved(const Cinematic& cinematic, const std::string& actor)
{
    return std::any_of(cinematic.actorTracks.begin(), cinematic.actorTracks.end(),
                       [&](const ActorTrack& track) { return track.actor == actor && !track.keys.empty(); }) ||
           std::any_of(cinematic.paths.begin(), cinematic.paths.end(), [&](const PathFollow& follow) { return follow.actor == actor; });
}

} // namespace

std::shared_ptr<ModelAsset> CinematicPlayer::LoadModel(const std::string& name)
{
    const auto found = m_models.find(name);
    if (found != m_models.end())
    {
        return found->second;
    }
    auto model = std::make_shared<ModelAsset>();
    if (!model->LoadFromFile(ModelPath(name)))
    {
        PRED_LOG_WARN(Gameplay, "Cinematic: no model '{}' in Assets/Models", name);
        model = nullptr;
    }
    m_models[name] = model;
    return model;
}

void CinematicPlayer::Play(const Cinematic& cinematic, const CinematicBindings& bindings, Scene& scene, MeshLibrary& meshes, Host& host,
                           float from)
{
    Stop(scene);
    m_cinematic = cinematic;
    m_cinematic.Tidy();
    m_bindings = bindings;
    m_history.clear();
    for (const ActorDef& actor : m_cinematic.actors)
    {
        Instance instance;
        instance.name = actor.name;
        instance.bind = actor.bind;
        if (!actor.bind.empty())
        {
            CinePose where;
            if (host.CineFindBound(actor.bind, where))
            {
                m_bindings.actorRest[actor.name] = where;
            }
            m_instances.push_back(std::move(instance));
            continue;
        }
        instance.model = LoadModel(actor.model);
        if (instance.model != nullptr)
        {
            for (const ModelPart& part : instance.model->parts)
            {
                Material material = Material::Diffuse(part.color, part.roughness);
                material.metallic = part.metallic;
                material.emissive = part.color * part.emissive;
                const MeshHandle mesh = meshes.Upload(instance.model->BuildPartMesh(part), "cine_" + actor.model + "_" + part.name);
                instance.parts.push_back(scene.CreateMeshEntity("cine_" + actor.name + "_" + part.name, Transform{}, mesh, material));
            }
        }
        m_instances.push_back(std::move(instance));
    }
    m_time = std::clamp(from, 0.0f, m_cinematic.duration);
    m_state = State::Playing;
    Place(scene, host);
    // What is at the very start happens now.
    Happen(m_time - 1.0e-4f, m_time, host);
    PRED_LOG_INFO(Gameplay, "Cinematic '{}' playing, {:.1f} s", m_cinematic.name, m_cinematic.duration);
}

void CinematicPlayer::Stop(Scene& scene)
{
    for (const Instance& instance : m_instances)
    {
        for (const Entity part : instance.parts)
        {
            scene.Destroy(part);
        }
    }
    m_instances.clear();
    m_state = State::Stopped;
    m_time = 0.0f;
}

void CinematicPlayer::SetPaused(bool paused)
{
    if (m_state == State::Playing && paused)
    {
        m_state = State::Paused;
    }
    else if (m_state == State::Paused && !paused)
    {
        m_state = State::Playing;
    }
}

void CinematicPlayer::Update(float dt, Scene& scene, Host& host)
{
    if (m_state != State::Playing)
    {
        if (m_state == State::Paused || m_state == State::Finished)
        {
            Place(scene, host);
        }
        return;
    }
    const float from = m_time;
    m_time += dt * m_speed;
    if (m_time >= m_cinematic.duration)
    {
        if (m_cinematic.loop && m_cinematic.duration > 0.0f)
        {
            Happen(from, m_cinematic.duration, host);
            m_time = std::fmod(m_time, m_cinematic.duration);
            Happen(-1.0e-4f, m_time, host);
        }
        else
        {
            Happen(from, m_cinematic.duration, host);
            m_time = m_cinematic.duration;
            m_state = State::Finished;
        }
    }
    else
    {
        Happen(from, m_time, host);
    }
    Place(scene, host);
}

void CinematicPlayer::Seek(float time, Scene& scene, Host& host)
{
    if (m_state == State::Stopped)
    {
        return;
    }
    m_time = std::clamp(time, 0.0f, m_cinematic.duration);
    if (m_state == State::Finished && m_time < m_cinematic.duration)
    {
        m_state = State::Paused;
    }
    Place(scene, host);
}

void CinematicPlayer::Refresh(Scene& scene, Host& host)
{
    m_cinematic.Tidy();
    Place(scene, host);
}

void CinematicPlayer::Place(Scene& scene, Host& host)
{
    const CinematicSampler sampler(m_cinematic, m_bindings);
    for (const Instance& instance : m_instances)
    {
        const CinePose pose = sampler.Actor(instance.name, m_time);
        const ClipEvent* clip = ClipAt(m_cinematic, instance.name, m_time);
        const float clipTime = clip != nullptr ? (m_time - clip->time) * clip->speed : 0.0f;
        if (!instance.bind.empty())
        {
            if (Moved(m_cinematic, instance.name))
            {
                host.CineMoveBound(instance.bind, pose);
            }
            if (clip != nullptr)
            {
                host.CinePoseBound(instance.bind, clip->clip, clipTime);
            }
            continue;
        }
        if (instance.model == nullptr)
        {
            continue;
        }
        const AnimationClip* animation = clip != nullptr ? instance.model->FindClip(clip->clip) : nullptr;
        const glm::mat4 root = Matrix(pose);
        for (size_t i = 0; i < instance.parts.size() && i < instance.model->parts.size(); ++i)
        {
            float visible = 1.0f;
            const glm::mat4 world = root * instance.model->PartMatrixAt(instance.model->parts[i], animation, clipTime, &visible);
            if (Transform* transform = scene.GetTransform(instance.parts[i]))
            {
                transform->position = glm::vec3(world[3]);
                transform->rotation = glm::normalize(glm::quat_cast(glm::mat3(world)));
            }
            if (MeshRenderer* renderer = scene.GetMeshRenderer(instance.parts[i]))
            {
                renderer->visible = visible > 0.5f;
            }
        }
    }
    for (const LightTrack& light : m_cinematic.lights)
    {
        host.CineLight(light.light, sampler.LightIntensity(light, m_time));
    }
}

void CinematicPlayer::Happen(float from, float to, Host& host)
{
    const CinematicSampler sampler(m_cinematic, m_bindings);
    for (const CinematicHappening& happening : HappeningsBetween(m_cinematic, from, to))
    {
        std::string line;
        switch (happening.kind)
        {
        case CinematicHappening::Kind::Marker:
        {
            const pred::Marker& marker = m_cinematic.markers[happening.index];
            host.CineMarker(marker);
            line = "marker " + marker.name + (marker.value.empty() ? "" : " " + marker.value);
            break;
        }
        case CinematicHappening::Kind::Sound:
        {
            const SoundEvent& sound = m_cinematic.sounds[happening.index];
            glm::vec3 at;
            const bool placed = sampler.Where(sound.at, happening.time, at);
            host.CineSound(sound, placed ? &at : nullptr);
            line = "sound " + sound.sound;
            break;
        }
        case CinematicHappening::Kind::Particle:
        {
            const ParticleEvent& particle = m_cinematic.particles[happening.index];
            glm::vec3 at{0.0f};
            sampler.Where(particle.at, happening.time, at);
            const CinePose frame = sampler.Frame(particle.at, happening.time);
            host.CineParticle(particle, at + frame.rotation * particle.offset);
            line = "particles " + particle.effect;
            break;
        }
        case CinematicHappening::Kind::Clip:
            line = "clip " + m_cinematic.clips[happening.index].actor + " " + m_cinematic.clips[happening.index].clip;
            break;
        case CinematicHappening::Kind::Text:
            line = "text " + (m_cinematic.texts[happening.index].lines.empty() ? std::string() : m_cinematic.texts[happening.index].lines.front());
            break;
        }
        m_history.emplace_back(happening.time, line);
        if (m_history.size() > kHistory)
        {
            m_history.erase(m_history.begin());
        }
    }
}

CameraState CinematicPlayer::Picture(std::string* shot, float* blend) const
{
    return CinematicSampler(m_cinematic, m_bindings).Picture(m_time, shot, blend);
}

} // namespace pred
