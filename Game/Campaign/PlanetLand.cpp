#include "Game/Campaign/PlanetLand.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred::PlanetLand
{

namespace
{

// The shader's, line for line (planet_surface.sh): PlanetHash, PlanetGradient, PlanetNoise, PlanetNext, PlanetFbmN.
glm::vec3 Fract(const glm::vec3& v)
{
    return v - glm::floor(v);
}

float Fract(float v)
{
    return v - std::floor(v);
}

float Hash(glm::vec3 p)
{
    p = Fract(p * 0.1031f);
    p += glm::dot(p, glm::vec3(p.z, p.y, p.x) + glm::vec3(31.32f));
    return Fract((p.x + p.y) * p.z);
}

glm::vec3 Gradient(const glm::vec3& i)
{
    const float a = Hash(i) * 6.2831853f;
    const float z = Hash(i + glm::vec3(17.3f, 5.1f, 9.7f)) * 2.0f - 1.0f;
    const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
    return {r * std::cos(a), r * std::sin(a), z};
}

float Mix(float a, float b, float t)
{
    return a + (b - a) * t;
}

float Noise(const glm::vec3& p)
{
    const glm::vec3 i = glm::floor(p);
    const glm::vec3 f = p - i;
    const glm::vec3 u = f * f * f * (f * (f * 6.0f - glm::vec3(15.0f)) + glm::vec3(10.0f));
    const float a = glm::dot(Gradient(i), f);
    const float b = glm::dot(Gradient(i + glm::vec3(1.0f, 0.0f, 0.0f)), f - glm::vec3(1.0f, 0.0f, 0.0f));
    const float c = glm::dot(Gradient(i + glm::vec3(0.0f, 1.0f, 0.0f)), f - glm::vec3(0.0f, 1.0f, 0.0f));
    const float d = glm::dot(Gradient(i + glm::vec3(1.0f, 1.0f, 0.0f)), f - glm::vec3(1.0f, 1.0f, 0.0f));
    const float e = glm::dot(Gradient(i + glm::vec3(0.0f, 0.0f, 1.0f)), f - glm::vec3(0.0f, 0.0f, 1.0f));
    const float g = glm::dot(Gradient(i + glm::vec3(1.0f, 0.0f, 1.0f)), f - glm::vec3(1.0f, 0.0f, 1.0f));
    const float h = glm::dot(Gradient(i + glm::vec3(0.0f, 1.0f, 1.0f)), f - glm::vec3(0.0f, 1.0f, 1.0f));
    const float k = glm::dot(Gradient(i + glm::vec3(1.0f, 1.0f, 1.0f)), f - glm::vec3(1.0f, 1.0f, 1.0f));
    const float n = Mix(Mix(Mix(a, b, u.x), Mix(c, d, u.x), u.y), Mix(Mix(e, g, u.x), Mix(h, k, u.x), u.y), u.z);
    return std::clamp(0.5f + n * 1.1f, 0.0f, 1.0f);
}

glm::vec3 Next(const glm::vec3& p)
{
    return glm::vec3(1.60f * p.y + 1.20f * p.z, -1.60f * p.x + 0.72f * p.y - 0.96f * p.z, -1.20f * p.x - 0.96f * p.y + 1.28f * p.z) +
           glm::vec3(1.7f, 9.2f, 3.1f);
}

float Fbm(glm::vec3 p, int octaves)
{
    float value = 0.0f;
    float amount = 0.5f;
    float total = 0.0f;
    for (int octave = 0; octave < octaves; ++octave)
    {
        value += amount * Noise(p);
        total += amount;
        p = Next(p);
        amount *= 0.5f;
    }
    return value / total;
}

} // namespace

float Continents(const glm::vec3& n, float seed)
{
    const glm::vec3 offset{seed * 1.7f, seed * 0.9f, seed * 2.3f};
    const glm::vec3 warp{Fbm(n * 1.7f + offset + glm::vec3(5.2f, 1.3f, 7.7f), 4), Fbm(n * 1.7f + offset + glm::vec3(1.1f, 8.3f, 2.8f), 4),
                         Fbm(n * 1.7f + offset + glm::vec3(9.4f, 4.6f, 0.3f), 4)};
    return Fbm(n * 2.1f + offset + (warp - glm::vec3(0.5f)) * 0.9f, 7);
}

float Shore(float oceanAmount)
{
    return 0.5f + (oceanAmount - 0.5f) * 0.36f;
}

bool Land(const glm::vec3& n, float seed, float oceanAmount, float margin)
{
    // No sea at all: ground everywhere.
    if (oceanAmount <= 0.001f)
    {
        return true;
    }
    return Continents(n, seed) > Shore(oceanAmount) + margin;
}

float LookSeed(unsigned long long bodySeed)
{
    return static_cast<float>(bodySeed % 997ull) * 0.37f;
}

glm::vec3 Globe(const glm::vec2& latLon)
{
    const float lat = glm::radians(latLon.x);
    const float lon = glm::radians(latLon.y);
    return {std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)};
}

glm::vec2 Nearest(const glm::vec2& latLon, float seed, float oceanAmount, bool wantSea, float minLat, float maxLat, float reach)
{
    const auto fits = [&](const glm::vec2& at)
    {
        const glm::vec3 n = Globe(at);
        return wantSea ? !Land(n, seed, oceanAmount, -0.02f) : Land(n, seed, oceanAmount);
    };
    if (fits(latLon))
    {
        return latLon;
    }
    // Round and out from it on a spiral, a step of a degree and a half or so: the first that will do.
    constexpr float kGolden = 2.39996323f;
    for (int i = 1; i < 900; ++i)
    {
        const float distance = reach * std::sqrt(static_cast<float>(i) / 900.0f);
        const float angle = static_cast<float>(i) * kGolden;
        glm::vec2 at{latLon.x + distance * std::sin(angle), latLon.y + distance * std::cos(angle) / std::max(std::cos(glm::radians(latLon.x)), 0.2f)};
        if (at.x < minLat || at.x > maxLat)
        {
            continue;
        }
        at.y = std::remainder(at.y, 360.0f);
        if (fits(at))
        {
            return at;
        }
    }
    return latLon;
}

} // namespace pred::PlanetLand
