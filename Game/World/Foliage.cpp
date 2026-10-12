#include "Game/World/Foliage.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pred
{

namespace Foliage
{

namespace
{

constexpr int kSize = 1024;
constexpr int kCell = 512;
constexpr float kPi = 3.14159265f;

struct Random
{
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed) {}
    float Unit()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / static_cast<float>(1ull << 24);
    }
    float Range(float a, float b) { return a + (b - a) * Unit(); }
};

struct Canvas
{
    ImageData image;
    int x0 = 0;
    int y0 = 0; // the cell being drawn in, its corner

    void Put(int x, int y, const glm::vec3& colour)
    {
        if (x < 0 || y < 0 || x >= kCell || y >= kCell)
        {
            return;
        }
        uint8_t* p = &image.pixels[(static_cast<size_t>(y0 + y) * kSize + static_cast<size_t>(x0 + x)) * 4];
        p[0] = static_cast<uint8_t>(std::clamp(colour.r, 0.0f, 1.0f) * 255.0f);
        p[1] = static_cast<uint8_t>(std::clamp(colour.g, 0.0f, 1.0f) * 255.0f);
        p[2] = static_cast<uint8_t>(std::clamp(colour.b, 0.0f, 1.0f) * 255.0f);
        p[3] = 255;
    }
    // A line `r` pixels either side of it, from a to b.
    void Line(glm::vec2 a, glm::vec2 b, float r0, float r1, const glm::vec3& colour)
    {
        const int lx = static_cast<int>(std::floor(std::min(a.x, b.x) - std::max(r0, r1) - 1.0f));
        const int hx = static_cast<int>(std::ceil(std::max(a.x, b.x) + std::max(r0, r1) + 1.0f));
        const int ly = static_cast<int>(std::floor(std::min(a.y, b.y) - std::max(r0, r1) - 1.0f));
        const int hy = static_cast<int>(std::ceil(std::max(a.y, b.y) + std::max(r0, r1) + 1.0f));
        const glm::vec2 d = b - a;
        const float length2 = std::max(glm::dot(d, d), 1.0e-6f);
        for (int y = ly; y <= hy; ++y)
        {
            for (int x = lx; x <= hx; ++x)
            {
                const glm::vec2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                const float t = std::clamp(glm::dot(p - a, d) / length2, 0.0f, 1.0f);
                const float r = r0 + (r1 - r0) * t;
                if (glm::length(p - (a + d * t)) <= r)
                {
                    Put(x, y, colour);
                }
            }
        }
    }
    // A leaf from `base`, `length` long along `angle` (radians, 0 up the picture), `width` across at its widest, pointed at
    // both ends; lighter on one side of its midrib than the other, the midrib darker.
    void Leaf(glm::vec2 base, float angle, float length, float width, const glm::vec3& colour)
    {
        const glm::vec2 along{std::sin(angle), -std::cos(angle)};
        const glm::vec2 across{-along.y, along.x};
        const float reach = length + width;
        for (int y = static_cast<int>(base.y - reach); y <= static_cast<int>(base.y + reach); ++y)
        {
            for (int x = static_cast<int>(base.x - reach); x <= static_cast<int>(base.x + reach); ++x)
            {
                const glm::vec2 p = glm::vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) - base;
                const float u = glm::dot(p, along) / length;
                const float v = glm::dot(p, across);
                if (u < 0.0f || u > 1.0f)
                {
                    continue;
                }
                const float half = width * 0.5f * std::pow(std::sin(kPi * std::pow(u, 0.8f)), 0.75f);
                if (std::abs(v) > half)
                {
                    continue;
                }
                float shade = v > 0.0f ? 1.0f : 0.84f;
                shade *= 0.88f + 0.12f * u;
                if (std::abs(v) < 1.1f)
                {
                    shade *= 0.78f;
                }
                Put(x, y, colour * shade);
            }
        }
    }
};

void BroadSpray(Canvas& canvas, Random& random, int leaves, float size)
{
    // Twigs from the foot of the cell up and out, leaves along them and at their ends.
    const glm::vec3 twig{0.36f, 0.3f, 0.24f};
    const glm::vec2 foot{256.0f, 505.0f};
    const int twigs = 5;
    std::vector<std::pair<glm::vec2, glm::vec2>> spans;
    for (int i = 0; i < twigs; ++i)
    {
        const float angle = -0.9f + 1.8f * (static_cast<float>(i) + random.Range(0.2f, 0.8f)) / static_cast<float>(twigs);
        const float length = random.Range(300.0f, 440.0f);
        const glm::vec2 end = foot + glm::vec2(std::sin(angle), -std::cos(angle)) * length;
        canvas.Line(foot, end, 4.0f, 1.2f, twig);
        spans.push_back({foot, end});
    }
    for (int i = 0; i < leaves; ++i)
    {
        const auto& span = spans[static_cast<size_t>(random.Unit() * static_cast<float>(spans.size()) * 0.999f)];
        const float t = random.Range(0.3f, 1.0f);
        const glm::vec2 at = span.first + (span.second - span.first) * t;
        const glm::vec2 way = glm::normalize(span.second - span.first);
        const float along = std::atan2(way.x, -way.y);
        const float angle = along + (random.Unit() < 0.5f ? -1.0f : 1.0f) * random.Range(0.4f, 1.2f);
        // Greens a little apart: younger leaves yellower and lighter, older darker.
        const float age = random.Unit();
        const glm::vec3 colour = glm::mix(glm::vec3(0.86f, 0.92f, 0.7f), glm::vec3(0.66f, 0.76f, 0.6f), age) * random.Range(0.85f, 1.05f);
        canvas.Leaf(at, angle, size * random.Range(0.8f, 1.2f), size * random.Range(0.38f, 0.5f), colour);
    }
}

void NeedleBranch(Canvas& canvas, Random& random)
{
    // A stem up the middle, twigs off it, needles off everything.
    const glm::vec3 stem{0.4f, 0.34f, 0.27f};
    const glm::vec2 foot{256.0f, 508.0f};
    const glm::vec2 top{256.0f + random.Range(-20.0f, 20.0f), 20.0f};
    canvas.Line(foot, top, 5.0f, 1.5f, stem);
    const auto needles = [&](glm::vec2 a, glm::vec2 b, float reach)
    {
        const glm::vec2 d = b - a;
        const float length = glm::length(d);
        const glm::vec2 way = d / std::max(length, 1.0f);
        const glm::vec2 side{-way.y, way.x};
        for (float s = 0.0f; s < length; s += 5.0f)
        {
            const glm::vec2 at = a + way * s;
            const float fade = 1.0f - 0.6f * s / length;
            for (const float sign : {-1.0f, 1.0f})
            {
                const glm::vec2 out = glm::normalize(side * sign + way * random.Range(0.4f, 0.9f));
                const glm::vec3 colour = glm::vec3(0.62f, 0.72f, 0.6f) * random.Range(0.8f, 1.1f);
                canvas.Line(at, at + out * reach * fade * random.Range(0.8f, 1.1f), 1.6f, 0.8f, colour);
            }
        }
    };
    needles(foot, top, 40.0f);
    for (int i = 0; i < 9; ++i)
    {
        const float t = random.Range(0.15f, 0.85f);
        const glm::vec2 from = foot + (top - foot) * t;
        const float sign = i % 2 == 0 ? -1.0f : 1.0f;
        const glm::vec2 to = from + glm::vec2(sign * random.Range(90.0f, 190.0f) * (1.0f - t * 0.6f), -random.Range(60.0f, 140.0f));
        canvas.Line(from, to, 2.4f, 1.0f, stem);
        needles(from, to, 30.0f);
    }
}

void GrassTuft(Canvas& canvas, Random& random)
{
    // Blades from the foot, tapering, curving over.
    for (int i = 0; i < 70; ++i)
    {
        const float x = 256.0f + random.Range(-150.0f, 150.0f);
        const float high = random.Range(220.0f, 500.0f);
        const float lean = random.Range(-120.0f, 120.0f);
        const float wide = random.Range(4.0f, 9.0f);
        const glm::vec3 colour = glm::mix(glm::vec3(0.84f, 0.9f, 0.66f), glm::vec3(0.62f, 0.72f, 0.56f), random.Unit()) * random.Range(0.85f, 1.05f);
        glm::vec2 at{x, 511.0f};
        const int steps = 12;
        for (int s = 0; s < steps; ++s)
        {
            const float t0 = static_cast<float>(s) / static_cast<float>(steps);
            const float t1 = static_cast<float>(s + 1) / static_cast<float>(steps);
            const glm::vec2 next{x + lean * t1 * t1, 511.0f - high * t1};
            canvas.Line(at, next, wide * (1.0f - t0), wide * (1.0f - t1) + 0.4f, colour * (0.8f + 0.2f * t1));
            at = next;
        }
    }
}

} // namespace

ImageData Atlas()
{
    Canvas canvas;
    canvas.image.width = kSize;
    canvas.image.height = kSize;
    // Clear, its colour a mid green, so where a filtered edge reads into it no dark or bright rim shows.
    canvas.image.pixels.assign(static_cast<size_t>(kSize) * kSize * 4, 0);
    for (size_t i = 0; i < canvas.image.pixels.size(); i += 4)
    {
        canvas.image.pixels[i] = 180;
        canvas.image.pixels[i + 1] = 200;
        canvas.image.pixels[i + 2] = 160;
    }
    Random random(0x1EAF5EEDull);
    for (int cell = 0; cell < 4; ++cell)
    {
        canvas.x0 = (cell % 2) * kCell;
        canvas.y0 = (cell / 2) * kCell;
        switch (cell)
        {
        case kBroad: BroadSpray(canvas, random, 70, 78.0f); break;
        case kBroadDense: BroadSpray(canvas, random, 130, 60.0f); break;
        case kNeedles: NeedleBranch(canvas, random); break;
        default: GrassTuft(canvas, random); break;
        }
    }
    return canvas.image;
}

glm::vec4 Cell(int cell)
{
    const float u = static_cast<float>(cell % 2) * 0.5f;
    const float v = static_cast<float>(cell / 2) * 0.5f;
    // A texel in from each edge, so its neighbour never bleeds in.
    const float in = 1.5f / static_cast<float>(kSize);
    return {u + in, v + in, u + 0.5f - in, v + 0.5f - in};
}

} // namespace Foliage

} // namespace pred
