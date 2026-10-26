#pragma once

#include "Engine/Render/Mesh.h"
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

// Vents in the ground where a world is still alive under it: steam from fumaroles on a volcanic world, yellow-green gas on a
// toxic one -- each a low mound crusted with what it breathes out (white minerals, yellow sulphur), and its plume rising and
// drifting off with the wind. Where they are comes from the site's seed and the plume from the campaign's clock, so every
// machine sees the same ones breathing the same way.
namespace Vents
{

struct Vent
{
    glm::vec3 at{0.0f}; // its mouth, on the ground
    float strength = 1.0f;
};

// Whether a world has them, and what they breathe.
bool Has(const std::string& biome);

// Where they are on a site of that world: on its own ground and round it, never on what was built on, nor in water.
std::vector<Vent> Plan(uint32_t seed, const std::string& biome, const SitePlan& plan, const SiteTerrain& terrain);

// The mound of one, in the world (no transform), painted for the world's rock material: crusted near its mouth.
MeshData Mound(const Vent& vent, const std::string& biome, uint32_t seed);

// Their plumes as they are at `seconds`, lit by the light there is and fogged as far as they are from `eye`, blown along
// `wind` (Environment::wind's strength), added to `out`.
void Plumes(const std::vector<Vent>& vents, const std::string& biome, double seconds, const glm::vec3& wind, const Environment& light,
            const glm::vec3& eye, std::vector<Sprite>& out);
// The same for any plumes, their own colour and how thick (0 to 1, a vent's steam about 0.16): an outpost's stack smoke and the
// steam off its roofs.
void Plumes(const std::vector<Vent>& vents, const glm::vec3& own, float thick, double seconds, const glm::vec3& wind, const Environment& light,
            const glm::vec3& eye, std::vector<Sprite>& out);

} // namespace Vents

} // namespace pred
