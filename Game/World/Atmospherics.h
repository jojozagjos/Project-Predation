#pragma once

#include "Engine/Render/SpriteRenderer.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <functional>

#include <string>
#include <vector>

namespace pred
{

struct Environment;
class SiteTerrain;

// What the air of a world does that its weather does not: snow lifted off the ground and blown along by a strong wind.
namespace Atmospherics
{

// As it is now, lit by the light there is round `eye`, added to `out`. `travel` is how far the wind has carried the air,
// metres, built up a frame at a time: what blown snow moves by. `sheltered` says where something stands that it does not blow
// through -- a building, within so far of it.
void Sprites(const std::string& biome, const glm::vec3& wind, const Environment& light, const glm::vec3& eye, const glm::vec2& travel,
             const SiteTerrain& terrain, const std::function<bool(const glm::vec3& at, float margin)>& sheltered, std::vector<Sprite>& out);

} // namespace Atmospherics

} // namespace pred
