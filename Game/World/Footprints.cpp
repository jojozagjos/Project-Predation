#include "Game/World/Footprints.h"

#include "Engine/Scene/Scene.h"
#include "Game/World/SiteMap.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace pred
{

namespace
{

// Across and along the print, from its middle (-1 to 1 each way), over the ground round it: how deep the sole has pressed
// it (0 to 1) and how far it is pushed up into a lip round it (0 to 1).
void Sole(float u, float v, bool left, float& pressed, float& lip)
{
    // Narrow at the heel, narrower still at the arch -- more on the inside of the foot -- and widest across the ball.
    const float arch = std::exp(-((v - 0.05f) / 0.25f) * ((v - 0.05f) / 0.25f));
    const float inside = (left ? u : -u) > 0.0f ? 0.3f : 0.1f;
    const float width = 0.72f + 0.28f * glm::smoothstep(-0.1f, 0.6f, v) - inside * arch;
    const float d = std::sqrt((u / width) * (u / width) + v * v);
    pressed = 1.0f - glm::smoothstep(0.78f, 1.0f, d);
    // Deepest under the heel and the ball, where the weight goes.
    pressed *= 0.75f + 0.25f * (std::exp(-((v + 0.65f) / 0.25f) * ((v + 0.65f) / 0.25f)) + std::exp(-((v - 0.55f) / 0.25f) * ((v - 0.55f) / 0.25f)));
    lip = std::exp(-((d - 1.05f) / 0.09f) * ((d - 1.05f) / 0.09f));
}

} // namespace

void Footprints::Leave(Scene& scene, MeshLibrary& meshes, const SiteMap& site, const glm::vec3& at, float yaw, bool left, double now)
{
    if (site.Seed() != m_site)
    {
        Clear(scene);
        m_site = site.Seed();
    }
    if (m_prints.size() < kMost)
    {
        m_prints.resize(kMost);
    }
    Print& print = m_prints[m_next];
    m_next = (m_next + 1) % kMost;

    // A sole 11 by 28 centimetres, a foot's width to the side of where the body went.
    constexpr int kAcross = 9;
    constexpr int kAlong = 17;
    constexpr float kHalfWide = 0.075f; // the ground round it, as well as the sole
    constexpr float kHalfLong = 0.17f;
    const glm::vec3 forward{std::sin(yaw), 0.0f, -std::cos(yaw)};
    const glm::vec3 right{std::cos(yaw), 0.0f, std::sin(yaw)};
    const glm::vec3 middle = at + right * (left ? -0.09f : 0.09f);
    const SiteTerrain& ground = site.Terrain();
    MeshData mesh;
    for (int k = 0; k < kAlong; ++k)
    {
        for (int i = 0; i < kAcross; ++i)
        {
            const float u = (static_cast<float>(i) / static_cast<float>(kAcross - 1)) * 2.0f - 1.0f;
            const float v = (static_cast<float>(k) / static_cast<float>(kAlong - 1)) * 2.0f - 1.0f;
            float pressed = 0.0f;
            float lip = 0.0f;
            Sole(u * 1.25f, v * 1.18f, left, pressed, lip);
            // The dent's slopes, for its shading: how the pressed depth changes across and along.
            float pressedAcross = 0.0f;
            float pressedAlong = 0.0f;
            float unused = 0.0f;
            Sole((u + 0.05f) * 1.25f, v * 1.18f, left, pressedAcross, unused);
            Sole(u * 1.25f, (v + 0.05f) * 1.18f, left, pressedAlong, unused);
            const float depth = 0.016f;
            const float slopeAcross = -(pressedAcross - pressed) * depth / (0.05f * kHalfWide);
            const float slopeAlong = -(pressedAlong - pressed) * depth / (0.05f * kHalfLong);
            const glm::vec3 place = middle + right * (u * kHalfWide) + forward * (v * kHalfLong);
            MeshVertex vertex;
            // Over the ground, never into it: the lip a few millimetres higher still.
            vertex.position = {place.x, ground.Height(place.x, place.z) + 0.004f + 0.003f * lip, place.z};
            const glm::vec3 slope = right * slopeAcross + forward * slopeAlong;
            vertex.normal = glm::normalize(ground.Normal(place.x, place.z) - glm::vec3(slope.x, 0.0f, slope.z));
            vertex.uv = {place.x, place.z};
            // The ground's own colour, darker where it is pressed down and in the shade of the dent.
            const glm::vec3 tint = glm::clamp(site.GroundTint(place.x, place.z) * (1.0f - 0.3f * pressed), glm::vec3(0.0f), glm::vec3(1.0f));
            const auto channel = [](float c) { return static_cast<uint32_t>(c * 255.0f + 0.5f); };
            vertex.color = channel(tint.r) | (channel(tint.g) << 8) | (channel(tint.b) << 16) | (255u << 24);
            mesh.vertices.push_back(vertex);
        }
    }
    for (int k = 0; k + 1 < kAlong; ++k)
    {
        for (int i = 0; i + 1 < kAcross; ++i)
        {
            const auto a = static_cast<uint32_t>(k * kAcross + i);
            const uint32_t b = a + 1;
            const auto c = static_cast<uint32_t>(a + kAcross);
            const uint32_t d = c + 1;
            // (Wound to face up: along runs the way the toe points, which turns the grid over against the ground's.)
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    }
    // Each slot its own mesh, its shape made again for each print laid in it.
    if (!print.mesh.IsValid())
    {
        print.mesh = meshes.Upload(mesh, "footprint#" + std::to_string(&print - m_prints.data()));
    }
    else
    {
        print.mesh = meshes.Replace(print.mesh, mesh);
    }
    if (!print.entity.IsValid())
    {
        print.entity = scene.CreateMeshEntity("footprint", Transform{}, print.mesh, site.GroundMaterial());
    }
    if (MeshRenderer* renderer = scene.GetMeshRenderer(print.entity))
    {
        renderer->mesh = print.mesh;
        renderer->material = site.GroundMaterial();
        renderer->castsShadow = false;
        renderer->blocksSky = false;
        renderer->visible = true;
    }
    print.made = now;
    print.shown = true;
}

void Footprints::Update(Scene& scene, const SiteMap& site, double now)
{
    if (!m_prints.empty() && (!site.Built() || site.Seed() != m_site))
    {
        Clear(scene);
        return;
    }
    for (Print& print : m_prints)
    {
        if (print.shown && now - print.made > kLast)
        {
            if (MeshRenderer* renderer = scene.GetMeshRenderer(print.entity))
            {
                renderer->visible = false;
            }
            print.shown = false;
        }
    }
}

void Footprints::Clear(Scene& scene)
{
    for (Print& print : m_prints)
    {
        if (print.entity.IsValid())
        {
            scene.Destroy(print.entity);
        }
        print = Print{};
    }
    m_prints.clear();
    m_next = 0;
}

} // namespace pred
