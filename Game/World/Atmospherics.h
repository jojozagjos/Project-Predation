#pragma once

#include "Engine/Render/SpriteRenderer.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

struct Environment;
class SiteTerrain;

// What the air of a world does that its weather does not: snow lifted off the ground and blown along by a strong wind; mist
// lying in the low ground of a jungle or a sea coast. The mist is planned from the site's seed and moves by the campaign's clock, so
// every machine sees the same.
namespace Atmospherics
{

struct Plan
{
    // Low ground where mist lies.
    std::vector<glm::vec3> mist;
};

Plan Make(uint32_t seed, const std::string& biome, const SiteTerrain& terrain);

// As they are at `seconds`, lit by the light there is and fogged as far as they are from `eye`, added to `out`. `travel` is
// how far the wind has carried the air, metres, built up a frame at a time: what blown snow moves by.
void Sprites(const Plan& plan, const std::string& biome, double seconds, const glm::vec3& wind, const Environment& light, const glm::vec3& eye,
             const glm::vec2& travel, const SiteTerrain& terrain, std::vector<Sprite>& out);

} // namespace Atmospherics

} // namespace pred
