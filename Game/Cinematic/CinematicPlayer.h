#pragma once

#include "Engine/Assets/ModelAsset.h"
#include "Engine/Scene/Scene.h"
#include "Game/Cinematic/Cinematic.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pred
{

class MeshLibrary;

// Plays a Cinematic in the world: puts its actors there (models from Assets/Models, or things the game already has),
// moves them, gives the game the picture to draw, and tells it what happens as the time passes -- sounds, markers,
// particles -- through a Host. Scrubbing to any moment shows exactly what playing to it would, because everything it
// shows is worked out from the time alone; only what *happens* (a sound, a marker) is fired, and only when time passes
// it going forwards.
class CinematicPlayer
{
public:
    enum class State : uint8_t
    {
        Stopped,
        Playing,
        Paused,
        Finished // at its last moment, for the game to hand the picture back from
    };

    // What the game does for it.
    class Host
    {
    public:
        virtual ~Host() = default;
        // Something the game has, named by an actor's `bind`, and where it is now; false when there is no such thing.
        virtual bool CineFindBound(const std::string& bind, CinePose& where) = 0;
        virtual void CineMoveBound(const std::string& bind, const CinePose& pose) = 0;
        // One of a bound thing's clips (doors, a ramp) at a moment of it.
        virtual void CinePoseBound(const std::string& bind, const std::string& clip, float clipTime) = 0;
        virtual void CineSound(const SoundEvent& sound, const glm::vec3* at) = 0;
        virtual void CineMarker(const pred::Marker& marker) = 0;
        virtual void CineParticle(const ParticleEvent& particle, const glm::vec3& at) = 0;
        virtual void CineLight(const std::string& light, float intensity) = 0;
    };

    // Starts it, from `from` seconds in. Its actors are put in the world at once, where it has them at that moment.
    void Play(const Cinematic& cinematic, const CinematicBindings& bindings, Scene& scene, MeshLibrary& meshes, Host& host, float from = 0.0f);
    // Takes away everything it put in the world. Bound things stay where it left them.
    void Stop(Scene& scene);
    // On by the frame: time passes, and what it passes happens.
    void Update(float dt, Scene& scene, Host& host);
    // Straight to a moment, showing it without anything happening on the way.
    void Seek(float time, Scene& scene, Host& host);
    void SetPaused(bool paused);
    // Put its actors where the moment says: after an edit to the cinematic, without starting again.
    void Refresh(Scene& scene, Host& host);

    State GetState() const { return m_state; }
    bool Active() const { return m_state != State::Stopped; }
    float Time() const { return m_time; }
    float Speed() const { return m_speed; }
    void SetSpeed(float speed) { m_speed = speed; }
    Cinematic& Edited() { return m_cinematic; } // the copy it plays, for the editor to change as it plays
    const Cinematic& Playing() const { return m_cinematic; }
    const CinematicBindings& Bindings() const { return m_bindings; }
    CinematicSampler Sampler() const { return CinematicSampler(m_cinematic, m_bindings); }

    // The picture now, with the shot it is from and how far that shot has blended in.
    CameraState Picture(std::string* shot = nullptr, float* blend = nullptr) const;
    // What happened most recently, for the debugging panel: the time and a line about it.
    const std::vector<std::pair<float, std::string>>& History() const { return m_history; }

private:
    struct Instance
    {
        std::string name;
        std::string bind;
        std::shared_ptr<ModelAsset> model;
        std::vector<Entity> parts;
    };

    void Place(Scene& scene, Host& host);
    void Happen(float from, float to, Host& host);
    std::shared_ptr<ModelAsset> LoadModel(const std::string& name);

    Cinematic m_cinematic;
    CinematicBindings m_bindings;
    std::vector<Instance> m_instances;
    State m_state = State::Stopped;
    float m_time = 0.0f;
    float m_speed = 1.0f;
    std::vector<std::pair<float, std::string>> m_history;
    std::map<std::string, std::shared_ptr<ModelAsset>> m_models;
};

} // namespace pred
