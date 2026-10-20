#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>

namespace pred
{

// The waves on open water: a few long travelling waves (Gerstner's -- the crests sharpen and the water goes round in
// circles, not only up and down), sized by the wind as a sea is, the same ones moving the drawn surface (vs_water) and
// worked out here for whatever is in the water, so a swimmer rides the waves that are drawn. By a shared clock, so they
// are the same for everybody.
struct Waves
{
    static constexpr int kMost = 6;
    // Each: the way it goes (xy, unit, along x and z), how long it is (z, metres) and how high (w, metres from its middle to
    // a crest).
    std::array<glm::vec4, kMost> waves{};
    int count = 0;
    // How sharp the crests are, 0 (smooth swells) to 1 (as sharp as they go without folding over).
    float steepness = 0.0f;

    // The sea a wind raises: `wind` metres a second blowing towards `angle` (radians, from +x towards +z), and `storm` (0 to
    // 1) how much heavier a storm makes it. On open sea, as high as a sea grows -- a ripple in a breeze, a couple of metres
    // in a strong wind, ten and more in a gale; on a lake, never more than a short chop. `seed` turns each wave its own way.
    static Waves Raised(float wind, float angle, float storm, bool openSea, uint32_t seed);

    // How far the water that is at rest at `at` (x, z) is moved at `seconds`: along x, up, along z. `depth` how deep it is
    // there (they die down in the shallows) and `spacing` how far apart the drawn surface's lines are (a wave too short for
    // them is not drawn): as vs_water has them, so what is felt is what is seen.
    glm::vec3 Moved(const glm::vec2& at, float seconds, float depth = 1.0e9f, float spacing = 0.0f) const;
    // How high the surface is over its level right above `at` (x, z) at `seconds` -- the water there having come from
    // somewhere else, as the waves carry it round.
    float Rise(const glm::vec2& at, float seconds, float depth = 1.0e9f, float spacing = 0.0f) const;
    // The highest any crest gets (metres over the level), for how much the surface can stand above it.
    float Highest() const;
};

} // namespace pred
