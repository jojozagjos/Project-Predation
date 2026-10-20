#include "Game/World/Vents.h"

#include "Engine/Scene/Scene.h"
#include "Game/World/SitePlan.h"
#include "Game/World/SiteTerrain.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace Vents
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

uint32_t Paint(const glm::vec3& c)
{
    const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(c.r) | (byte(c.g) << 8) | (byte(c.b) << 16) | (255u << 24);
}

} // namespace

bool Has(const std::string& biome)
{
    return biome == "volcanic" || biome == "toxic";
}

std::vector<Vent> Plan(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain)
{
    std::vector<Vent> vents;
    if (!Has(biome))
    {
        return vents;
    }
    Random random((static_cast<uint64_t>(seed) << 27) ^ 0x7E47ull);
    // Over the site's ground and as far round it as anybody may go, a little further: some among the buildings' open
    // ground, most out on the world's own.
    const float reach = SiteTerrain::kRoam + 30.0f;
    const int wanted = 8 + static_cast<int>(random.Unit() * 7.0f);
    for (int tries = 0; tries < wanted * 12 && static_cast<int>(vents.size()) < wanted; ++tries)
    {
        const glm::vec2 at{plan.origin.x - reach + random.Range(0.0f, plan.size + reach * 2.0f),
                           plan.origin.z - reach + random.Range(0.0f, plan.size + reach * 2.0f)};
        const float strength = random.Range(0.6f, 1.3f);
        if (terrain.Wildness(at.x, at.y) < 0.85f || terrain.GroundNormal(at.x, at.y).y < 0.85f || terrain.WaterDepth(at.x, at.y) > -0.4f)
        {
            continue;
        }
        bool clear = true;
        for (const Vent& other : vents)
        {
            clear = clear && glm::distance(glm::vec2(other.at.x, other.at.z), at) > 14.0f;
        }
        if (!clear)
        {
            continue;
        }
        vents.push_back({{at.x, terrain.GroundHeight(at.x, at.y), at.y}, strength});
    }
    return vents;
}

MeshData Mound(const Vent& vent, const std::string& biome, uint32_t seed)
{
    // A low cone of rock round a mouth, its rim broken, crusted with what it breathes out -- the crust brightest at the
    // mouth and spattered down its sides.
    MeshData mesh;
    const bool toxic = biome == "toxic";
    const glm::vec3 crust = toxic ? glm::vec3(0.95f, 0.8f, 0.18f) : glm::vec3(0.85f, 0.83f, 0.78f);
    const glm::vec3 rock{0.55f, 0.53f, 0.5f};
    const float wide = 1.6f + 1.2f * vent.strength;
    const float high = 0.5f + 0.6f * vent.strength;
    constexpr int kSides = 14;
    constexpr int kRings = 6;
    const uint32_t key = seed ^ static_cast<uint32_t>(vent.at.x * 13.0f) ^ (static_cast<uint32_t>(vent.at.z * 7.0f) << 8);
    for (int r = 0; r <= kRings; ++r)
    {
        const float t = static_cast<float>(r) / kRings; // 0 at the foot, 1 at the mouth
        // Up the cone from its foot, then down into the mouth.
        const float radius = wide * (1.0f - 0.82f * t);
        const float y = r == kRings ? high * 0.55f : high * std::sin(t * 1.5708f);
        for (int s = 0; s < kSides; ++s)
        {
            const float a = kTau * static_cast<float>(s) / kSides;
            const float lump = 0.85f + 0.3f * Hash(key + static_cast<uint32_t>(r), static_cast<uint32_t>(s));
            MeshVertex vertex;
            vertex.position = vent.at + glm::vec3(std::cos(a) * radius * lump, y * lump - 0.15f, std::sin(a) * radius * lump);
            const float crusted = glm::smoothstep(0.35f, 0.95f, t) + (Hash(key ^ 0x51u, static_cast<uint32_t>(r * kSides + s)) > 0.75f ? 0.5f : 0.0f);
            vertex.color = Paint(glm::mix(rock, crust, std::min(crusted, 1.0f)));
            vertex.normal = glm::normalize(glm::vec3(std::cos(a) * high, wide * 0.6f, std::sin(a) * high));
            mesh.vertices.push_back(vertex);
        }
    }
    for (uint32_t r = 0; r < kRings; ++r)
    {
        for (uint32_t s = 0; s < kSides; ++s)
        {
            const uint32_t n = (s + 1) % kSides;
            const uint32_t a = r * kSides;
            const uint32_t b = a + kSides;
            mesh.indices.insert(mesh.indices.end(), {a + s, b + s, a + n, a + n, b + s, b + n});
        }
    }
    // And its throat: down from the mouth into the dark, narrowing, and closed just over the ground (the ground would hide it
    // any lower) in a dark that reads as deep from above. An open ring, it showed the ground straight through it.
    {
        const auto mouth = static_cast<uint32_t>(kRings * kSides);
        const auto throat = static_cast<uint32_t>(mesh.vertices.size());
        const glm::vec3 dark = toxic ? glm::vec3(0.06f, 0.05f, 0.01f) : glm::vec3(0.04f, 0.035f, 0.03f);
        for (int s = 0; s < kSides; ++s)
        {
            const float a = kTau * static_cast<float>(s) / kSides;
            MeshVertex vertex;
            vertex.position = vent.at + glm::vec3(std::cos(a) * wide * 0.1f, 0.06f, std::sin(a) * wide * 0.1f);
            vertex.color = Paint(dark);
            vertex.normal = glm::normalize(glm::vec3(-std::cos(a), 0.3f, -std::sin(a)));
            mesh.vertices.push_back(vertex);
        }
        MeshVertex bottom;
        bottom.position = vent.at + glm::vec3(0.0f, 0.03f, 0.0f);
        bottom.color = Paint(dark);
        bottom.normal = {0.0f, 1.0f, 0.0f};
        mesh.vertices.push_back(bottom);
        const auto end = static_cast<uint32_t>(mesh.vertices.size() - 1);
        for (uint32_t s = 0; s < kSides; ++s)
        {
            const uint32_t n = (s + 1) % kSides;
            // (Facing in and up, to an eye looking down into it.)
            mesh.indices.insert(mesh.indices.end(), {mouth + s, throat + s, mouth + n, mouth + n, throat + s, throat + n});
            mesh.indices.insert(mesh.indices.end(), {throat + s, end, throat + n});
        }
    }
    return mesh;
}

void Plumes(const std::vector<Vent>& vents, const std::string& biome, double seconds, const glm::vec3& wind, const Environment& light,
            const glm::vec3& eye, std::vector<Sprite>& out)
{
    if (vents.empty())
    {
        return;
    }
    const bool toxic = biome == "toxic";
    // Steam white, gas a sickly yellow-green: lit by the sky and a little by the sun, which it scatters.
    const glm::vec3 own = toxic ? glm::vec3(0.62f, 0.68f, 0.18f) : glm::vec3(0.86f, 0.86f, 0.86f);
    const glm::vec3 lit = light.ambientSky * 0.9f + light.ambientGround * 0.3f + light.sunColor * std::max(light.sunIntensity, 0.0f) * 0.35f;
    constexpr int kPuffs = 44;
    constexpr float kLife = 12.0f; // seconds from the mouth to gone
    for (size_t v = 0; v < vents.size(); ++v)
    {
        const Vent& vent = vents[v];
        const float distance = glm::distance(eye, vent.at);
        if (distance > 450.0f)
        {
            continue;
        }
        const float fog = std::clamp((distance - light.fogStart) / std::max(light.fogEnd - light.fogStart, 1.0f), 0.0f, 1.0f);
        for (int p = 0; p < kPuffs; ++p)
        {
            // Each puff its own way through its life, the vent breathing harder and softer as it goes.
            const float offset = Hash(static_cast<uint32_t>(v) * 977u, static_cast<uint32_t>(p));
            const double cycle = seconds / kLife + offset;
            const float age = static_cast<float>(cycle - std::floor(cycle));
            const auto round = static_cast<uint32_t>(std::floor(cycle));
            const float side = Hash(static_cast<uint32_t>(p) * 31u + round, static_cast<uint32_t>(v)) * kTau;
            const float breath = 0.7f + 0.3f * std::sin(static_cast<float>(seconds) * 0.6f + static_cast<float>(v));
            const float rise = (1.0f - std::exp(-age * 2.2f)) * 20.0f * vent.strength;
            Sprite sprite;
            sprite.at = vent.at + glm::vec3(0.0f, 0.6f + rise, 0.0f) + wind * (age * kLife * 0.12f) +
                        glm::vec3(std::cos(side), 0.0f, std::sin(side)) * (0.3f + age * 3.5f);
            sprite.size = (1.2f + age * 9.0f) * vent.strength;
            sprite.alpha = (toxic ? 0.3f : 0.16f) * breath * glm::smoothstep(0.0f, 0.08f, age) * (1.0f - glm::smoothstep(0.45f, 1.0f, age));
            sprite.colour = glm::mix(own * lit * 0.75f, light.fogColor, fog);
            out.push_back(sprite);
        }
    }
}

} // namespace Vents

} // namespace pred
