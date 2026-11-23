#pragma once

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

// How brightly a point on the front of a creature's eye glows (0 to 1), its lamp's colour times this -- the same for every creature,
// giant and leviathan (you, 2026-11-23: more of the eye glowing). `across` and `high` are where the point is over the eye's front
// (each -1 to 1 across the disc that shows): the iris lit all over, as an animal's eyes shine back at a light, streaked with fibres
// out from the middle and hottest in a ring round the pupil; darker towards its rim; and the pupil a dark upright slit down its middle.
inline float EyeGlowLevel(float across, float high)
{
    const float r = std::min(std::sqrt(across * across + high * high), 1.0f);
    const float angle = std::atan2(high, across);
    const float fibres = 0.2f * std::sin(angle * 17.0f + r * 9.0f) * std::sin(angle * 7.0f - 3.0f) + 0.08f * std::sin(angle * 29.0f);
    const float ring = 0.5f * std::exp(-((r - 0.27f) / 0.12f) * ((r - 0.27f) / 0.12f));
    float level = (0.42f + fibres * glm::smoothstep(0.15f, 0.4f, r) + ring) * (1.0f - 0.85f * glm::smoothstep(0.68f, 1.0f, r));
    const float pupil = std::exp(-(across * across) / (0.13f * 0.13f)) * glm::smoothstep(0.98f, 0.5f, std::abs(high));
    level *= 1.0f - 0.95f * pupil;
    return std::clamp(level, 0.0f, 1.0f);
}

} // namespace pred
