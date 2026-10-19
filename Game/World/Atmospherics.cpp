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

constexpr float kTau = 6.28318530718f;

// SplitMix64, as the sites are planned with: the same sequence on every machine.
struct Random
{
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    float Range(float a, float b) { return a + (b - a) * Unit(); }
};

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

float FogAt(const Environment& light, float distance)
{
    return std::clamp((distance - light.fogStart) / std::max(light.fogEnd - light.fogStart, 1.0f), 0.0f, 1.0f);
}

} // namespace

Plan Make(uint32_t seed, const std::string& biome, const SiteTerrain& terrain)
{
    Plan made;
    Random random((static_cast<uint64_t>(seed) << 29) ^ 0xA7305ull);
    // Mist lying in the low ground of a jungle or along a sea: the lowest of the land round the site.
    if (biome == "jungle" || biome == "oceanic")
    {
        struct Low
        {
            glm::vec3 at;
            float below;
        };
        std::vector<Low> lows;
        constexpr float kCell = 34.0f;
        const glm::vec2 from = terrain.Min();
        const int cells = static_cast<int>(terrain.Extent() / kCell);
        for (int j = 0; j < cells; ++j)
        {
            for (int i = 0; i < cells; ++i)
            {
                const glm::vec2 at = from + (glm::vec2(static_cast<float>(i), static_cast<float>(j)) + glm::vec2(random.Range(0.2f, 0.8f), random.Range(0.2f, 0.8f))) * kCell;
                const float h = terrain.Height(at.x, at.y);
                // How far below the ground round it: a hollow.
                float round = 0.0f;
                for (int k = 0; k < 6; ++k)
                {
                    const float b = kTau * static_cast<float>(k) / 6.0f;
                    round += terrain.Height(at.x + std::cos(b) * 30.0f, at.y + std::sin(b) * 30.0f);
                }
                lows.push_back({{at.x, std::max(h, terrain.HasWater() ? terrain.WaterLevel() : h), at.y}, round / 6.0f - h});
            }
        }
        std::sort(lows.begin(), lows.end(), [](const Low& a, const Low& b) { return a.below > b.below; });
        for (size_t k = 0; k < lows.size() && k < 22; ++k)
        {
            made.mist.push_back(lows[k].at);
        }
    }
    return made;
}

void Sprites(const Plan& plan, const std::string& biome, double seconds, const glm::vec3& wind, const Environment& light, const glm::vec3& eye,
             const glm::vec2& travel, const SiteTerrain& terrain, std::vector<Sprite>& out)
{
    const glm::vec3 lit = Lit(light);
    const float t = static_cast<float>(seconds);
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
    // Mist: wide, faint banks lying in the hollows, drifting a little and thinning and thickening.
    if (!plan.mist.empty())
    {
        const bool jungle = biome == "jungle";
        for (size_t m = 0; m < plan.mist.size(); ++m)
        {
            for (int k = 0; k < 4; ++k)
            {
                const float h0 = Hash(static_cast<uint32_t>(m), static_cast<uint32_t>(k));
                const float h1 = Hash(static_cast<uint32_t>(m) + 101u, static_cast<uint32_t>(k));
                const glm::vec3 drift{std::sin(t * 0.021f + h0 * 9.0f) * 9.0f, 0.0f, std::cos(t * 0.017f + h1 * 9.0f) * 9.0f};
                const glm::vec3 at = plan.mist[m] + glm::vec3((h0 - 0.5f) * 30.0f, 1.5f + k * 1.2f, (h1 - 0.5f) * 30.0f) + drift;
                const float distance = glm::distance(eye, at);
                if (distance > 700.0f)
                {
                    continue;
                }
                Sprite sprite;
                sprite.at = at;
                sprite.size = 26.0f + 20.0f * h1;
                sprite.alpha = (jungle ? 0.24f : 0.16f) * (0.7f + 0.3f * std::sin(t * 0.05f + h0 * 6.0f)) * glm::smoothstep(4.0f, 25.0f, distance);
                sprite.colour = glm::mix(glm::vec3(0.82f, 0.85f, 0.86f) * lit, light.fogColor, FogAt(light, distance) * 0.7f);
                out.push_back(sprite);
            }
        }
    }
}

} // namespace Atmospherics

} // namespace pred
