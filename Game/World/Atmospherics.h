#pragma once

#include "Engine/Render/SpriteRenderer.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

struct Environment;
struct SitePlan;
class SiteTerrain;

// What the air of a world does that its weather does not: dust devils wandering over a desert, forming and dying away; mist
// lying in the low ground of a jungle or a sea coast. Planned from the site's seed and moving by the campaign's clock, so
// every machine sees the same.
namespace Atmospherics
{

struct Plan
{
    // Where each dust devil wanders about, and how far.
    struct Devil
    {
        glm::vec3 home{0.0f};
        float roam = 60.0f;
        float size = 1.0f;
    };
    std::vector<Devil> devils;
    // Low ground where mist lies.
    std::vector<glm::vec3> mist;
};

Plan Make(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain);

// As they are at `seconds`, lit by the light there is and fogged as far as they are from `eye`, added to `out`.
void Sprites(const Plan& plan, const std::string& biome, double seconds, const glm::vec3& wind, const Environment& light, const glm::vec3& eye,
             const SiteTerrain& terrain, std::vector<Sprite>& out);

} // namespace Atmospherics

} // namespace pred
