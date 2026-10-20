#include "Game/World/Splashes.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{
constexpr size_t kMost = 600;
constexpr float kGravity = 9.81f;
} // namespace

float Splashes::Random()
{
    m_state = m_state * 1664525u + 1013904223u;
    return static_cast<float>(m_state >> 8) / static_cast<float>(1u << 24);
}

void Splashes::Emit(const glm::vec3& at, float strength, const glm::vec3& tint)
{
    const float hard = std::clamp(strength, 0.0f, 1.0f);
    // Drops: more and faster the harder it broke, thrown up and out every way, a few higher than the rest.
    const int drops = 6 + static_cast<int>(hard * 22.0f);
    for (int i = 0; i < drops && m_drops.size() < kMost; ++i)
    {
        const float angle = Random() * 6.2831853f;
        const float out = (0.3f + 0.9f * Random()) * (0.6f + 0.8f * hard);
        const float up = (1.2f + 2.2f * Random() * Random()) * (0.6f + 0.9f * hard);
        Drop drop;
        drop.at = at + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * (0.05f + 0.1f * Random());
        drop.velocity = {std::cos(angle) * out, up, std::sin(angle) * out};
        drop.tint = tint;
        drop.level = at.y;
        drop.life = 0.4f + 0.6f * Random();
        drop.size = 0.012f + 0.02f * Random();
        m_drops.push_back(drop);
    }
    // And the white where it broke, spreading and fading quickly.
    for (int i = 0; i < 2 + static_cast<int>(hard * 2.0f) && m_drops.size() < kMost; ++i)
    {
        Drop foam;
        foam.at = at + glm::vec3(Random() - 0.5f, 0.02f, Random() - 0.5f) * 0.2f;
        foam.tint = glm::mix(tint, glm::vec3(1.0f), 0.6f);
        foam.level = at.y - 1.0f;
        foam.life = 0.35f + 0.25f * Random();
        foam.size = 0.12f + 0.13f * hard;
        foam.foam = true;
        m_drops.push_back(foam);
    }
}

void Splashes::Update(float dt)
{
    for (Drop& drop : m_drops)
    {
        drop.age += dt;
        if (!drop.foam)
        {
            drop.velocity.y -= kGravity * dt;
            drop.velocity *= std::max(1.0f - 0.4f * dt, 0.0f);
            drop.at += drop.velocity * dt;
        }
    }
    // Gone once its time is up, or once it falls back through the water.
    m_drops.erase(std::remove_if(m_drops.begin(), m_drops.end(),
                                 [](const Drop& drop) { return drop.age >= drop.life || (!drop.foam && drop.velocity.y < 0.0f && drop.at.y < drop.level); }),
                  m_drops.end());
}

void Splashes::Sprites(std::vector<Sprite>& out, const glm::vec3& light, const glm::vec3& fog, float fogStart, float fogEnd, const glm::vec3& eye) const
{
    for (const Drop& drop : m_drops)
    {
        const float t = drop.age / std::max(drop.life, 1.0e-3f);
        const float distance = glm::length(drop.at - eye);
        const float fogged = std::clamp((distance - fogStart) / std::max(fogEnd - fogStart, 0.01f), 0.0f, 1.0f);
        Sprite sprite;
        sprite.at = drop.at;
        sprite.colour = glm::mix(drop.tint * light, fog, fogged);
        if (drop.foam)
        {
            // Spreading as it fades.
            sprite.size = drop.size * (1.0f + 1.2f * t);
            sprite.alpha = 0.55f * (1.0f - t) * (1.0f - t);
        }
        else
        {
            // Streaked along its flight, as quick things are seen.
            const float speed = glm::length(drop.velocity);
            sprite.size = drop.size;
            sprite.alpha = 0.8f * (1.0f - t * t);
            if (speed > 0.5f)
            {
                sprite.length = std::min(speed * 0.03f, 0.25f);
                sprite.along = drop.velocity / speed;
            }
        }
        out.push_back(sprite);
    }
}

} // namespace pred
