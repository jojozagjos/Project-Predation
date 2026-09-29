#include "Game/World/Snowfall.h"

#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"

#include <cmath>

namespace pred
{

namespace
{

// The box the snow fills round the eye: wide, and taller above than below.
constexpr glm::vec3 kBox{34.0f, 16.0f, 34.0f};
constexpr float kAbove = 11.0f;
constexpr int kFlakes = 720;

} // namespace

void Snowfall::Update(Scene& scene, MeshLibrary& meshes, const glm::vec3& eye, float dt, bool falling, const glm::vec3& wind,
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
        const MeshHandle mesh = meshes.Upload(Primitives::Box({0.042f, 0.042f, 0.042f}), "snow_flake");
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
            flake.at = eye + glm::vec3((random() - 0.5f) * kBox.x, kAbove - random() * kBox.y, (random() - 0.5f) * kBox.z);
            flake.fall = 0.8f + random() * 0.9f;
            flake.sway = random() * 6.2831853f;
            m_flakes.push_back(flake);
        }
    }
    m_falling = true;
    m_clock += dt;
    for (Flake& flake : m_flakes)
    {
        const float swayX = std::sin(m_clock * 1.3f + flake.sway) * 0.35f;
        const float swayZ = std::cos(m_clock * 1.1f + flake.sway * 1.7f) * 0.35f;
        flake.at += (glm::vec3(wind.x + swayX, -flake.fall, wind.z + swayZ)) * dt;
        // Round again at the other side of the box when it leaves it -- below, it starts again at the top.
        glm::vec3 local = flake.at - eye;
        local.x = std::fmod(local.x + kBox.x * 1.5f, kBox.x) - kBox.x * 0.5f;
        local.z = std::fmod(local.z + kBox.z * 1.5f, kBox.z) - kBox.z * 0.5f;
        if (local.y < kAbove - kBox.y)
        {
            local.y += kBox.y;
        }
        else if (local.y > kAbove)
        {
            local.y -= kBox.y;
        }
        flake.at = eye + local;
        const bool show = !indoors(flake.at);
        if (Transform* transform = scene.GetTransform(flake.entity))
        {
            transform->position = flake.at;
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
