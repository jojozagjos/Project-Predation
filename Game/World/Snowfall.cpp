#include "Game/World/Snowfall.h"

#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace pred
{

namespace
{

// The box the snow fills round the eye: wide, and taller above than below.
constexpr glm::vec3 kBox{34.0f, 16.0f, 34.0f};
constexpr float kAbove = 11.0f;
constexpr int kFlakes = 720;
// Of them, this many are spindrift: blown along low over the ground, faster than the wind that falls.
constexpr int kDrift = 170;

} // namespace

void Snowfall::Update(Scene& scene, MeshLibrary& meshes, const glm::vec3& eye, float ground, float dt, bool falling, const glm::vec3& wind,
                      const std::function<bool(const glm::vec3&)>& indoors)
{
    const auto random = [this]()
    {
        m_seed = m_seed * 1664525u + 1013904223u;
        return static_cast<float>(m_seed >> 8) / static_cast<float>(1u << 24);
    };
    if (!falling)
    {
        if (m_falling)
        {
            for (Flake& flake : m_flakes)
            {
                if (MeshRenderer* renderer = scene.GetMeshRenderer(flake.entity))
                {
                    renderer->visible = false;
                }
                flake.shown = false;
            }
        }
        m_falling = false;
        return;
    }
    if (m_flakes.empty())
    {
        // Flat, a flake more than a crumb, so it turns over as it falls and catches the light now and then.
        const MeshHandle mesh = meshes.Upload(Primitives::Box({1.0f, 0.35f, 1.0f}), "snow_flake");
        Material white = Material::Diffuse({0.92f, 0.94f, 0.97f}, 1.0f);
        // A little of its own, so it shows against the dark as snow does, catching whatever light there is.
        white.emissive = {0.16f, 0.165f, 0.18f};
        for (int i = 0; i < kFlakes; ++i)
        {
            Flake flake;
            flake.entity = scene.CreateMeshEntity("snow", Transform{}, mesh, white);
            if (MeshRenderer* renderer = scene.GetMeshRenderer(flake.entity))
            {
                renderer->castsShadow = false;
                renderer->blocksSky = false;
                renderer->visible = false;
            }
            flake.drift = i < kDrift;
            flake.at = eye + glm::vec3((random() - 0.5f) * kBox.x, kAbove - random() * kBox.y, (random() - 0.5f) * kBox.z);
            flake.fall = flake.drift ? 0.0f : 0.7f + random() * 1.0f;
            flake.sway = random() * 6.2831853f;
            flake.size = flake.drift ? 0.02f + random() * 0.02f : 0.026f + random() * 0.038f;
            flake.spin = glm::normalize(glm::vec3(random() - 0.5f, random() - 0.5f, random() - 0.5f) + glm::vec3(0.0f, 0.001f, 0.0f));
            flake.spinRate = 1.5f + random() * 4.0f;
            flake.height = 0.05f + random() * random() * 0.7f;
            m_flakes.push_back(flake);
        }
    }
    m_falling = true;
    m_clock += dt;
    // Gusting: the wind rises and falls, now and then hard.
    const float gust = 0.7f + 0.6f * std::sin(m_clock * 0.31f) * std::sin(m_clock * 0.77f + 1.3f) + 0.35f * std::max(std::sin(m_clock * 0.13f), 0.0f);
    const glm::vec3 blowing = wind * gust;
    for (Flake& flake : m_flakes)
    {
        glm::vec3 local = flake.at - eye;
        if (flake.drift)
        {
            // Low over the ground, streaming with the wind, lifting and settling.
            flake.at += (blowing * 3.2f + glm::vec3(0.0f, 0.0f, 0.0f)) * dt;
            flake.at.y = ground + flake.height + 0.12f * std::sin(m_clock * 2.3f + flake.sway);
            local = flake.at - eye;
        }
        else
        {
            const float swayX = std::sin(m_clock * 1.3f + flake.sway) * 0.35f;
            const float swayZ = std::cos(m_clock * 1.1f + flake.sway * 1.7f) * 0.35f;
            flake.at += glm::vec3(blowing.x + swayX, -flake.fall, blowing.z + swayZ) * dt;
            local = flake.at - eye;
            // Below, it starts again at the top.
            if (local.y < kAbove - kBox.y)
            {
                local.y += kBox.y;
            }
            else if (local.y > kAbove)
            {
                local.y -= kBox.y;
            }
        }
        // Round again at the other side of the box when it leaves it.
        local.x = std::fmod(local.x + kBox.x * 1.5f, kBox.x) - kBox.x * 0.5f;
        local.z = std::fmod(local.z + kBox.z * 1.5f, kBox.z) - kBox.z * 0.5f;
        flake.at.x = eye.x + local.x;
        flake.at.z = eye.z + local.z;
        if (!flake.drift)
        {
            flake.at.y = eye.y + local.y;
        }
        const bool show = !indoors(flake.at);
        if (Transform* transform = scene.GetTransform(flake.entity))
        {
            transform->position = flake.at;
            transform->rotation = glm::angleAxis(m_clock * flake.spinRate + flake.sway, flake.spin);
            transform->scale = glm::vec3(flake.size);
        }
        if (show != flake.shown)
        {
            flake.shown = show;
            if (MeshRenderer* renderer = scene.GetMeshRenderer(flake.entity))
            {
                renderer->visible = show;
            }
        }
    }
}

void Snowfall::Clear(Scene& scene)
{
    for (const Flake& flake : m_flakes)
    {
        scene.Destroy(flake.entity);
    }
    m_flakes.clear();
    m_falling = false;
}

} // namespace pred
