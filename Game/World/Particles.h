#pragma once

#include "Engine/Scene/Scene.h"

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

namespace pred
{

class MeshLibrary;

// How a burst of particles looks and moves: their colour and how much they glow, how big they start and end, how long
// they last, how fast they leave and how widely, and what pulls on them. `streak` draws each one stretched along the way
// it is going -- a spark -- rather than as a chip.
struct ParticleLook
{
    glm::vec3 colour{1.0f};
    glm::vec3 endColour{1.0f};
    float glow = 0.0f;       // emissive, at the start; it fades out over the life
    float size = 0.03f;
    float endSize = 0.01f;
    float life = 0.4f;       // seconds, give or take a third
    float speed = 4.0f;      // metres a second, give or take a half
    float spread = 0.6f;     // radians either side of the direction
    float gravity = 9.8f;
    float drag = 1.0f;       // how much of its speed it loses a second, as a fraction
    float bounce = 0.3f;     // off the floor it meets: how much of its fall it keeps (0: stops)
    bool streak = false;
};

// Sparks, chips, smoke-less bits: small things thrown off something that has just happened, drawn as little boxes from a
// pool of a few hundred, each lit by its own glow as it cools and falls. Kept simple: nothing is sorted or blended.
class Particles
{
public:
    // A burst of `count`, from `at`, about `direction` (a unit vector; zero for every way).
    // `floor` is how high the ground is where they fall, to land and bounce on (far below: none).
    void Emit(Scene& scene, MeshLibrary& meshes, const glm::vec3& at, const glm::vec3& direction, int count, const ParticleLook& look,
              float floor = -1.0e9f);
    void Update(Scene& scene, float dt);
    void Clear(Scene& scene);

    // The looks the game uses.
    static ParticleLook Sparks();
    static ParticleLook Chips(const glm::vec3& colour);
    static ParticleLook FlareSparks();
    static ParticleLook Exhaust();
    static ParticleLook SnowSpray();

private:
    struct Particle
    {
        Entity entity;
        glm::vec3 at{0.0f};
        glm::vec3 velocity{0.0f};
        float age = 0.0f;
        float life = 0.0f;
        float floor = -1.0e9f;
        bool alive = false;
        ParticleLook look;
    };
    Particle* Take(Scene& scene, MeshLibrary& meshes);
    std::vector<Particle> m_pool;
    size_t m_next = 0;
    uint32_t m_seed = 0xC0FFEEu;
};

} // namespace pred
