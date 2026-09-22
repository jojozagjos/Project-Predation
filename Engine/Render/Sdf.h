#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Signed distances to simple shapes, the ways of blending them, and a little noise: what sculpting in
// distance fields is made of. Negative inside a shape, positive outside. Used with SampleDistance and
// SurfaceNets to turn a description of a shape into a mesh.
namespace pred::Sdf
{


inline float SignOf(float value)
{
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

// A cone with rounded ends: a sphere of radius r1 at a and one of r2 at b, and the smooth taper between
// them. Inigo Quilez's exact distance.
inline float RoundCone(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float r1, float r2)
{
    const glm::vec3 ba = b - a;
    const float l2 = glm::dot(ba, ba);
    if (l2 < 1e-10f)
    {
        return glm::length(p - a) - std::max(r1, r2);
    }
    const float rr = r1 - r2;
    const float a2 = l2 - rr * rr;
    const float il2 = 1.0f / l2;
    const glm::vec3 pa = p - a;
    const float y = glm::dot(pa, ba);
    const float z = y - l2;
    const glm::vec3 xv = pa * l2 - ba * y;
    const float x2 = glm::dot(xv, xv);
    const float y2 = y * y * l2;
    const float z2 = z * z * l2;
    const float k = SignOf(rr) * rr * rr * x2;
    if (SignOf(z) * a2 * z2 > k)
    {
        return std::sqrt(x2 + z2) * il2 - r2;
    }
    if (SignOf(y) * a2 * y2 < k)
    {
        return std::sqrt(x2 + y2) * il2 - r1;
    }
    return (std::sqrt(std::max(x2 * a2 * il2, 0.0f)) + y * rr) * il2 - r1;
}

// An ellipsoid's distance, near enough: exact on the surface and a good bound away from it.
inline float Ellipsoid(const glm::vec3& local, const glm::vec3& radii)
{
    const float k0 = glm::length(local / radii);
    const float k1 = glm::length(local / (radii * radii));
    if (k1 < 1e-8f)
    {
        return -std::min({radii.x, radii.y, radii.z});
    }
    return k0 * (k0 - 1.0f) / k1;
}

// Blends two shapes into one over a distance k, instead of the crease where two shapes meet.
inline float SmoothUnion(float a, float b, float k)
{
    if (k <= 0.0f)
    {
        return std::min(a, b);
    }
    const float h = std::max(k - std::abs(a - b), 0.0f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}

inline float SmoothCut(float body, float cut, float k)
{
    return -SmoothUnion(-body, cut, k);
}

// --- Noise --------------------------------------------------------------------------------------

inline float Hash(int x, int y, int z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^
                 static_cast<uint32_t>(z) * 83492791u ^ seed * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / 16777215.0f;
}

inline float ValueNoise(const glm::vec3& p, uint32_t seed)
{
    const glm::vec3 f = glm::floor(p);
    const glm::vec3 t = p - f;
    const glm::vec3 u = t * t * (3.0f - 2.0f * t);
    const int x = static_cast<int>(f.x);
    const int y = static_cast<int>(f.y);
    const int z = static_cast<int>(f.z);
    const auto lerp = [](float a, float b, float s) { return a + (b - a) * s; };
    const float x00 = lerp(Hash(x, y, z, seed), Hash(x + 1, y, z, seed), u.x);
    const float x10 = lerp(Hash(x, y + 1, z, seed), Hash(x + 1, y + 1, z, seed), u.x);
    const float x01 = lerp(Hash(x, y, z + 1, seed), Hash(x + 1, y, z + 1, seed), u.x);
    const float x11 = lerp(Hash(x, y + 1, z + 1, seed), Hash(x + 1, y + 1, z + 1, seed), u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

inline float Fbm(const glm::vec3& p, uint32_t seed, int octaves)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    glm::vec3 q = p;
    for (int i = 0; i < octaves; ++i)
    {
        sum += ValueNoise(q, seed + static_cast<uint32_t>(i) * 101u) * amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        q *= 2.07f;
    }
    return sum / total;
}

} // namespace pred::Sdf
