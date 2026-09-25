#include "Game/World/LevelLights.h"

#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

// A number from 0 to 1 that is the same for the same seed and step on every machine.
float Hash01(uint32_t seed, int32_t step)
{
    uint32_t h = seed * 0x9E3779B1u ^ static_cast<uint32_t>(step) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

struct KindLook
{
    glm::vec3 color;
    float intensity;
    float range;
    float inner;
    float outer;
    float sourceRadius;
    glm::vec3 fittingSize;
    glm::vec3 housing; // the fitting's own colour
};

KindLook LookOf(LightKind kind)
{
    switch (kind)
    {
    case LightKind::Ceiling:
        // A strip light: cold, a little green, as old fluorescent tubes are.
        return {{0.86f, 0.95f, 1.0f}, 7.0f, 9.0f, 60.0f, 115.0f, 0.55f, {0.9f, 0.07f, 0.22f}, {0.55f, 0.56f, 0.58f}};
    case LightKind::Wall:
        // A caged bulb: warm and low, lighting a doorway rather than a room.
        return {{1.0f, 0.74f, 0.46f}, 4.0f, 6.5f, 70.0f, 125.0f, 0.3f, {0.22f, 0.26f, 0.14f}, {0.3f, 0.3f, 0.28f}};
    case LightKind::Emergency:
        return {{1.0f, 0.1f, 0.06f}, 7.0f, 11.0f, 180.0f, 180.0f, 0.25f, {0.18f, 0.12f, 0.18f}, {0.22f, 0.05f, 0.05f}};
    case LightKind::Flood:
        return {{0.95f, 0.97f, 1.0f}, 55.0f, 32.0f, 22.0f, 42.0f, 0.6f, {0.55f, 0.35f, 0.3f}, {0.25f, 0.26f, 0.27f}};
    }
    return {{1.0f, 1.0f, 1.0f}, 6.0f, 8.0f, 180.0f, 180.0f, 0.4f, {0.3f, 0.3f, 0.3f}, {0.4f, 0.4f, 0.4f}};
}

} // namespace

const char* LightKindName(LightKind kind)
{
    switch (kind)
    {
    case LightKind::Ceiling: return "ceiling";
    case LightKind::Wall: return "wall";
    case LightKind::Emergency: return "emergency";
    case LightKind::Flood: return "flood";
    }
    return "?";
}

const char* LightMoodName(LightMood mood)
{
    switch (mood)
    {
    case LightMood::Steady: return "steady";
    case LightMood::Flicker: return "flicker";
    case LightMood::Failing: return "failing";
    case LightMood::Dead: return "dead";
    case LightMood::Pulse: return "pulse";
    }
    return "?";
}

float LevelLights::MoodLevel(LightMood mood, uint32_t seed, float time)
{
    switch (mood)
    {
    case LightMood::Steady:
        return 1.0f;
    case LightMood::Dead:
        return 0.0f;
    case LightMood::Pulse:
    {
        // A slow swell, never quite out, offset per light so a row of them does not breathe as one.
        const float phase = time / 1.7f + Hash01(seed, 0);
        const float wave = 0.5f + 0.5f * std::sin(phase * 6.2831853f);
        return 0.3f + 0.7f * wave * wave;
    }
    case LightMood::Flicker:
    {
        // On, and every so often a stutter: a few steps of a twentieth of a second where it drops.
        const int32_t step = static_cast<int32_t>(std::floor(time * 20.0f));
        const float roll = Hash01(seed, step);
        if (roll < 0.05f)
        {
            return 0.15f + 0.4f * Hash01(seed + 1u, step);
        }
        return 0.92f + 0.08f * Hash01(seed + 2u, step);
    }
    case LightMood::Failing:
    {
        // Off for a few seconds at a time; then back for a moment, stuttering, and gone again. The
        // time is cut into spans of uneven length, each either dark or a burst.
        const float span = 2.4f;
        const int32_t slot = static_cast<int32_t>(std::floor(time / span));
        const float into = time - static_cast<float>(slot) * span;
        const float burst = 0.35f + 0.9f * Hash01(seed, slot);
        if (Hash01(seed + 7u, slot) < 0.55f || into > burst)
        {
            return 0.0f;
        }
        const int32_t step = static_cast<int32_t>(std::floor(time * 24.0f));
        return Hash01(seed + 3u, step) < 0.45f ? 0.0f : 0.6f + 0.4f * Hash01(seed + 4u, step);
    }
    }
    return 1.0f;
}

int LevelLights::Add(Scene& scene, MeshLibrary& meshes, LightKind kind, LightMood mood, const glm::vec3& at,
                     const glm::vec3& direction, int circuit, uint32_t seed, float range)
{
    const KindLook look = LookOf(kind);
    Light light;
    light.kind = kind;
    light.mood = mood;
    light.direction = glm::length(direction) > 1e-4f ? glm::normalize(direction) : glm::vec3(0.0f, -1.0f, 0.0f);
    light.color = look.color;
    light.intensity = look.intensity;
    light.range = range > 0.0f ? range : look.range;
    light.innerAngle = look.inner;
    light.outerAngle = look.outer;
    light.sourceRadius = look.sourceRadius;
    light.circuit = circuit;
    light.seed = seed != 0 ? seed : static_cast<uint32_t>(m_lights.size() * 2654435761u + 17u);
    light.glow = look.color * (kind == LightKind::Emergency ? 2.5f : 1.8f);
    // The light itself sits a hand's width in front of the fitting, so the fitting is not inside the
    // brightest part of its own light.
    light.position = at + light.direction * 0.12f;

    // The fitting: one mesh per kind, shared.
    const size_t which = static_cast<size_t>(kind);
    if (!m_fittingMeshes[which].IsValid())
    {
        m_fittingMeshes[which] = meshes.Upload(Primitives::Box(look.fittingSize),
                                               std::string("light_fitting_") + LightKindName(kind));
    }
    Transform where;
    where.position = at;
    // Turned so its face is towards where it shines: its local -Y along the direction.
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    const float along = glm::dot(down, light.direction);
    if (along < 0.999f)
    {
        where.rotation = along < -0.999f ? glm::angleAxis(3.14159265f, glm::vec3(1.0f, 0.0f, 0.0f))
                                         : glm::rotation(down, light.direction);
    }
    Material material = Material::Diffuse(look.housing, 0.6f);
    material.emissive = light.glow;
    light.fitting = scene.CreateMeshEntity(std::string("light_") + LightKindName(kind), where,
                                           m_fittingMeshes[which], material);
    if (MeshRenderer* renderer = scene.GetMeshRenderer(light.fitting))
    {
        // A lamp does not shadow the room it lights.
        renderer->castsShadow = false;
    }
    m_lights.push_back(light);
    return static_cast<int>(m_lights.size()) - 1;
}

void LevelLights::Clear(Scene& scene)
{
    for (const Light& light : m_lights)
    {
        if (light.fitting.IsValid())
        {
            scene.Destroy(light.fitting);
        }
    }
    m_lights.clear();
    m_unpowered.clear();
}

void LevelLights::RemoveFrom(Scene& scene, size_t first)
{
    for (size_t i = first; i < m_lights.size(); ++i)
    {
        if (m_lights[i].fitting.IsValid())
        {
            scene.Destroy(m_lights[i].fitting);
        }
    }
    if (first < m_lights.size())
    {
        m_lights.resize(first);
    }
}

void LevelLights::Bound(int index, const glm::vec3& min, const glm::vec3& max)
{
    if (index < 0 || static_cast<size_t>(index) >= m_lights.size())
    {
        return;
    }
    Light& light = m_lights[static_cast<size_t>(index)];
    light.bounded = true;
    // A little bigger than asked: a wall face lying exactly on the edge of the box flickers in and out of
    // it from one pixel to the next, in stripes. Five centimetres is still well inside any wall.
    light.boundsMin = glm::min(min, max) - glm::vec3(0.05f);
    light.boundsMax = glm::max(min, max) + glm::vec3(0.05f);
}

void LevelLights::SetPowered(int circuit, bool powered)
{
    const auto found = std::find(m_unpowered.begin(), m_unpowered.end(), circuit);
    if (powered && found != m_unpowered.end())
    {
        m_unpowered.erase(found);
    }
    else if (!powered && found == m_unpowered.end())
    {
        m_unpowered.push_back(circuit);
    }
}

bool LevelLights::Powered(int circuit) const
{
    return std::find(m_unpowered.begin(), m_unpowered.end(), circuit) == m_unpowered.end();
}

void LevelLights::Update(Scene& scene, float time)
{
    for (Light& light : m_lights)
    {
        // On a battery, an emergency light does not care about the circuit.
        const bool powered = light.kind == LightKind::Emergency || Powered(light.circuit);
        light.level = powered ? MoodLevel(light.mood, light.seed, time) : 0.0f;
        if (MeshRenderer* renderer = scene.GetMeshRenderer(light.fitting))
        {
            renderer->material.emissive = light.glow * light.level;
        }
    }
}

void LevelLights::Gather(std::vector<PunctualLight>& out) const
{
    for (const Light& light : m_lights)
    {
        if (light.level <= 0.01f)
        {
            continue;
        }
        PunctualLight punctual;
        punctual.position = light.position;
        punctual.direction = light.direction;
        punctual.color = light.color;
        punctual.intensity = light.intensity * light.level;
        punctual.range = light.range;
        punctual.innerAngle = light.innerAngle;
        punctual.outerAngle = light.outerAngle;
        punctual.sourceRadius = light.sourceRadius;
        punctual.bounded = light.bounded;
        punctual.boundsMin = light.boundsMin;
        punctual.boundsMax = light.boundsMax;
        out.push_back(punctual);
    }
}

} // namespace pred
