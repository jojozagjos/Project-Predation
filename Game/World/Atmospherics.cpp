#include "Game/World/Atmospherics.h"

#include "Engine/Scene/Scene.h"
#include "Game/World/SiteTerrain.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace Atmospherics
{

namespace
{

float Hash(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x8DA6B343u ^ b * 0xD8163841u;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// The light there is, on something in the air: mostly the sky's, a little of the sun's that it scatters.
glm::vec3 Lit(const Environment& light)
{
    return light.ambientSky * 0.9f + light.ambientGround * 0.3f + light.sunColor * std::max(light.sunIntensity, 0.0f) * 0.3f;
}

} // namespace

void Sprites(const std::string& biome, const glm::vec3& wind, const Environment& light, const glm::vec3& eye,
             const glm::vec2& travel, const SiteTerrain& terrain, std::vector<Sprite>& out)
{
    const glm::vec3 lit = Lit(light);
    // Spindrift: on a frozen world, or wherever snow lies, the wind lifting it off the ground in streams that run low across
    // it -- none in a light air, more the harder it blows -- round wherever the eye is. The wind is in metres a second, gusts
    // and all; snow starts to lift at about five and streams at ten. Low down it runs at about half the wind's speed.
    const float windSpeed = glm::length(glm::vec2(wind.x, wind.z));
    const float lifting = glm::smoothstep(5.0f, 11.0f, windSpeed) * (biome == "frozen" ? 1.0f : glm::smoothstep(0.3f, 0.7f, light.snowCover));
    if (lifting > 0.01f)
    {
        const glm::vec3 along = glm::vec3(wind.x, 0.0f, wind.z) / windSpeed;
        const glm::vec3 snow = glm::vec3(1.08f, 1.1f, 1.14f) * lit;
        constexpr int kStreams = 280;
        constexpr float kSpan = 70.0f; // metres round the eye they are kept within, wrapping as they blow through it
        for (int s = 0; s < kStreams; ++s)
        {
            const float hx = Hash(static_cast<uint32_t>(s), 501u);
            const float hz = Hash(static_cast<uint32_t>(s), 502u);
            const float share = 0.4f + 0.25f * Hash(static_cast<uint32_t>(s), 503u);
            const float speed = windSpeed * share;
            // Where it is, blown along as far as the wind has carried the air (each stream at its own share of it),
            // wrapped into the square round the eye.
            glm::vec2 at = glm::vec2(hx, hz) * kSpan + travel * share;
            at = at - glm::floor(at / kSpan) * kSpan;
            at += glm::vec2(std::floor(eye.x / kSpan), std::floor(eye.z / kSpan)) * kSpan;
            // (Back into the square centred on the eye.)
            at.x += at.x - eye.x > kSpan * 0.5f ? -kSpan : (eye.x - at.x > kSpan * 0.5f ? kSpan : 0.0f);
            at.y += at.y - eye.z > kSpan * 0.5f ? -kSpan : (eye.z - at.y > kSpan * 0.5f ? kSpan : 0.0f);
            const float ground = terrain.GroundHeight(at.x, at.y);
            Sprite sprite;
            sprite.at = {at.x, ground + 0.1f + 0.7f * Hash(static_cast<uint32_t>(s), 504u) * Hash(static_cast<uint32_t>(s), 505u), at.y};
            sprite.size = 0.6f + 0.9f * Hash(static_cast<uint32_t>(s), 506u);
            sprite.length = 2.0f + speed * 0.25f;
            sprite.along = along;
            const float distance = glm::distance(eye, sprite.at);
            sprite.alpha = 0.42f * lifting * (1.0f - glm::smoothstep(kSpan * 0.35f, kSpan * 0.5f, distance)) * glm::smoothstep(0.5f, 3.0f, distance);
            sprite.colour = snow;
            out.push_back(sprite);
        }
    }
}

} // namespace Atmospherics

} // namespace pred
