#include "Engine/Render/Waves.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{
constexpr float kGravity = 9.81f;
constexpr float kTau = 6.2831853f;

// Each wave's own start along its way (radians), the same here and in vs_water.
float PhaseOf(int i)
{
    return static_cast<float>(i) * 1.7f;
}

float Hash01(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x8DA6B343u ^ b * 0xD8163841u;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}
} // namespace

Waves Waves::Raised(float wind, float angle, float storm, bool openSea, uint32_t seed)
{
    Waves sea;
    // A sea as the wind raises it fully (Pierson and Moskowitz): its significant height a fiftieth of the square of the wind,
    // its longest waves most of the square of it long. A storm blows harder than the wind it is listed with.
    // (Never quite flat: on the open sea, swell from weather far off rolls in on the stillest day.)
    const float blowing = std::max(wind, openSea ? 5.0f : 1.5f) + 9.0f * std::clamp(storm, 0.0f, 1.0f);
    float significant = 0.0214f * blowing * blowing;
    float longest = 0.83f * blowing * blowing;
    if (!openSea)
    {
        // A lake has not the room to raise more than a chop.
        significant = std::min(significant, 0.35f);
        longest = std::min(longest, 9.0f);
    }
    significant = std::min(significant, 14.0f);
    longest = std::clamp(longest, 4.0f, 260.0f);
    // Six waves from the longest down, each shorter and lower, spread either side of the wind's way.
    constexpr float kLength[kMost] = {1.0f, 0.71f, 0.52f, 0.37f, 0.26f, 0.18f};
    constexpr float kHeight[kMost] = {0.5f, 0.38f, 0.28f, 0.2f, 0.14f, 0.1f};
    for (int i = 0; i < kMost; ++i)
    {
        const float turn = angle + (Hash01(seed, static_cast<uint32_t>(i)) - 0.5f) * 1.4f;
        // (Together, at their highest where they meet, a little under the significant height over the level.)
        sea.waves[static_cast<size_t>(i)] = {std::cos(turn), std::sin(turn), longest * kLength[i], significant * kHeight[i] * 0.55f};
    }
    sea.count = kMost;
    // Sharper crests the harder it blows.
    sea.steepness = std::clamp(0.25f + 0.04f * blowing, 0.25f, 0.85f);
    return sea;
}

glm::vec3 Waves::Moved(const glm::vec2& at, float seconds, float depth, float spacing) const
{
    glm::vec3 moved{0.0f};
    for (int i = 0; i < count; ++i)
    {
        const glm::vec4& wave = waves[static_cast<size_t>(i)];
        if (wave.w <= 0.0f || wave.z <= 0.0f)
        {
            continue;
        }
        const glm::vec2 way{wave.x, wave.y};
        const float k = kTau / wave.z;
        const float speed = std::sqrt(kGravity * k);
        const float phase = k * glm::dot(way, at) - speed * seconds + PhaseOf(i);
        // (Sharpened no further than its crest just comes to a point, shared between them all.)
        const float sharp = steepness / (k * wave.w * static_cast<float>(count));
        const float drawn = spacing > 0.0f ? glm::smoothstep(4.0f * spacing, 6.0f * spacing, wave.z) : 1.0f;
        // (Dying down in the shallows -- and, where the surface is drawn coarsely, far off, over more of them: a crest raised
        // there drew the water on up over the land to the next line, a hundred metres in.)
        const float height = wave.w * drawn * std::clamp(depth / std::max({wave.w * 3.0f, spacing * 0.5f, 0.01f}), 0.0f, 1.0f);
        const float across = sharp * height * std::cos(phase);
        moved += glm::vec3(way.x * across, height * std::sin(phase), way.y * across);
    }
    // And never up over the shallows by more than a little of how deep they are: water at the shore does not stand up the beach.
    moved.y = std::min(moved.y, std::max(depth - 0.3f, 0.0f) * 0.4f);
    return moved;
}

float Waves::Rise(const glm::vec2& at, float seconds, float depth, float spacing) const
{
    if (count == 0)
    {
        return 0.0f;
    }
    // The water over `at` came from where it rests less how it was moved: found by stepping back a few times.
    glm::vec2 from = at;
    for (int step = 0; step < 3; ++step)
    {
        const glm::vec3 moved = Moved(from, seconds, depth, spacing);
        from = at - glm::vec2(moved.x, moved.z);
    }
    return Moved(from, seconds, depth, spacing).y;
}

float Waves::Highest() const
{
    float highest = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        highest += std::max(waves[static_cast<size_t>(i)].w, 0.0f);
    }
    return highest;
}

} // namespace pred
