#include "Game/World/Particles.h"

#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

constexpr size_t kPool = 600;

} // namespace

ParticleLook Particles::Sparks()
{
    // Hot metal thrown off where a round struck something hard: white going to orange going out, fast, falling.
    ParticleLook look;
    look.colour = {1.0f, 0.85f, 0.55f};
    look.endColour = {1.0f, 0.35f, 0.08f};
    look.glow = 6.0f;
    look.size = 0.012f;
    look.endSize = 0.006f;
    look.life = 0.35f;
    look.speed = 5.5f;
    look.spread = 0.9f;
    look.gravity = 9.8f;
    look.drag = 1.2f;
    look.bounce = 0.35f;
    look.streak = true;
    return look;
}

ParticleLook Particles::Chips(const glm::vec3& colour)
{
    // Bits of whatever was struck, knocked out of it and falling.
    ParticleLook look;
    look.colour = colour;
    look.endColour = colour * 0.8f;
    look.glow = 0.0f;
    look.size = 0.02f;
    look.endSize = 0.014f;
    look.life = 0.7f;
    look.speed = 2.6f;
    look.spread = 0.8f;
    look.gravity = 9.8f;
    look.drag = 0.8f;
    look.bounce = 0.2f;
    return look;
}

ParticleLook Particles::FlareSparks()
{
    // A burning flare spitting: red, slower, drifting up a little before they fall.
    ParticleLook look;
    look.colour = {1.0f, 0.55f, 0.35f};
    look.endColour = {0.9f, 0.08f, 0.04f};
    look.glow = 5.0f;
    look.size = 0.01f;
    look.endSize = 0.004f;
    look.life = 0.55f;
    look.speed = 1.6f;
    look.spread = 1.2f;
    look.gravity = 2.5f;
    look.drag = 1.8f;
    look.bounce = 0.1f;
    look.streak = true;
    return look;
}

ParticleLook Particles::Exhaust()
{
    // An engine's: hot and fast out of it, cooling as it goes.
    ParticleLook look;
    look.colour = {1.0f, 0.75f, 0.4f};
    look.endColour = {0.6f, 0.25f, 0.1f};
    look.glow = 4.0f;
    look.size = 0.05f;
    look.endSize = 0.01f;
    look.life = 0.6f;
    look.speed = 9.0f;
    look.spread = 0.25f;
    look.gravity = 0.0f;
    look.drag = 1.5f;
    look.bounce = 0.0f;
    look.streak = true;
    return look;
}

ParticleLook Particles::SnowSpray()
{
    // Snow thrown up off the ground by something landing on it or leaving it.
    ParticleLook look;
    look.colour = {0.9f, 0.93f, 0.97f};
    look.endColour = {0.85f, 0.88f, 0.93f};
    look.glow = 0.15f;
    look.size = 0.04f;
    look.endSize = 0.02f;
    look.life = 1.4f;
    look.speed = 5.0f;
    look.spread = 1.3f;
    look.gravity = 3.0f;
    look.drag = 1.4f;
    look.bounce = 0.0f;
    return look;
}

Particles::Particle* Particles::Take(Scene& scene, MeshLibrary& meshes)
{
    if (m_pool.empty())
    {
        const MeshHandle mesh = meshes.Upload(Primitives::Box({1.0f, 1.0f, 1.0f}), "particle");
        m_pool.resize(kPool);
        for (Particle& particle : m_pool)
        {
            particle.entity = scene.CreateMeshEntity("particle", Transform{}, mesh, Material::Diffuse(glm::vec3(1.0f), 0.8f));
            if (MeshRenderer* renderer = scene.GetMeshRenderer(particle.entity))
            {
                renderer->castsShadow = false;
                renderer->blocksSky = false;
                renderer->visible = false;
            }
        }
    }
    // The oldest goes when every one is in use: the newest thing is what matters.
    for (size_t tries = 0; tries < m_pool.size(); ++tries)
    {
        Particle& particle = m_pool[m_next];
        m_next = (m_next + 1) % m_pool.size();
        if (!particle.alive)
        {
            return &particle;
        }
    }
    Particle& oldest = m_pool[m_next];
    m_next = (m_next + 1) % m_pool.size();
    return &oldest;
}

void Particles::Emit(Scene& scene, MeshLibrary& meshes, const glm::vec3& at, const glm::vec3& direction, int count, const ParticleLook& look,
                     float floor)
{
    const auto random = [this]()
    {
        m_seed = m_seed * 1664525u + 1013904223u;
        return static_cast<float>(m_seed >> 8) / static_cast<float>(1u << 24);
    };
    const bool aimed = glm::length(direction) > 0.5f;
    const glm::vec3 axis = aimed ? glm::normalize(direction) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 side = std::abs(axis.y) < 0.9f ? glm::normalize(glm::cross(axis, glm::vec3(0.0f, 1.0f, 0.0f)))
                                                   : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 other = glm::cross(axis, side);
    for (int i = 0; i < count; ++i)
    {
        Particle* particle = Take(scene, meshes);
        if (particle == nullptr)
        {
            return;
        }
        // A direction within the cone round the axis (or anywhere), and a speed about the look's.
        const float tilt = (aimed ? look.spread : 3.14159f) * std::sqrt(random());
        const float round = random() * 6.2831853f;
        const glm::vec3 way = axis * std::cos(tilt) + (side * std::cos(round) + other * std::sin(round)) * std::sin(tilt);
        particle->at = at;
        particle->velocity = way * look.speed * (0.5f + random());
        particle->age = 0.0f;
        particle->life = look.life * (0.67f + 0.66f * random());
        particle->alive = true;
        particle->floor = floor;
        particle->look = look;
        if (MeshRenderer* renderer = scene.GetMeshRenderer(particle->entity))
        {
            renderer->visible = true;
            renderer->material.baseColor = look.colour;
            renderer->material.emissive = look.colour * look.glow;
        }
    }
}

void Particles::Update(Scene& scene, float dt)
{
    for (Particle& particle : m_pool)
    {
        if (!particle.alive)
        {
            continue;
        }
        particle.age += dt;
        if (particle.age >= particle.life)
        {
            particle.alive = false;
            if (MeshRenderer* renderer = scene.GetMeshRenderer(particle.entity))
            {
                renderer->visible = false;
            }
            continue;
        }
        const ParticleLook& look = particle.look;
        particle.velocity.y -= look.gravity * dt;
        particle.velocity *= std::max(1.0f - look.drag * dt, 0.0f);
        particle.at += particle.velocity * dt;
        {
            const float floor = particle.floor;
            if (particle.at.y < floor)
            {
                particle.at.y = floor;
                particle.velocity.y = std::abs(particle.velocity.y) * look.bounce;
                particle.velocity.x *= 0.6f;
                particle.velocity.z *= 0.6f;
            }
        }
        const float t = particle.age / particle.life;
        const float size = look.size + (look.endSize - look.size) * t;
        const glm::vec3 colour = look.colour + (look.endColour - look.colour) * t;
        if (Transform* transform = scene.GetTransform(particle.entity))
        {
            transform->position = particle.at;
            const float speed = glm::length(particle.velocity);
            if (look.streak && speed > 0.2f)
            {
                // Long along the way it is going: a streak the length of a thirtieth of a second of it.
                const glm::vec3 along = particle.velocity / speed;
                transform->rotation = glm::quatLookAt(along, std::abs(along.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
                transform->scale = {size, size, std::max(size, speed * 0.03f)};
            }
            else
            {
                transform->scale = glm::vec3(size);
            }
        }
        if (MeshRenderer* renderer = scene.GetMeshRenderer(particle.entity))
        {
            renderer->material.baseColor = colour;
            const float cool = (1.0f - t) * (1.0f - t);
            renderer->material.emissive = colour * look.glow * cool;
        }
    }
}

void Particles::Clear(Scene& scene)
{
    for (const Particle& particle : m_pool)
    {
        scene.Destroy(particle.entity);
    }
    m_pool.clear();
    m_next = 0;
}

} // namespace pred
