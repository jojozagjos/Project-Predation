#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace pred
{

// How a planet looks from space: what its ground is mixed from, its seas, its ice, its cloud and its air. One set of
// numbers, drawn the same way wherever the planet is seen -- on the system map, and hung in the sky out of the ship's
// windows -- so a world looks like itself from both (Shaders/planet/planet_surface.sh).
struct PlanetLook
{
    glm::vec3 groundA{0.6f, 0.62f, 0.66f};
    glm::vec3 groundB{0.45f, 0.47f, 0.5f};
    glm::vec3 ocean{0.1f, 0.2f, 0.35f};
    float oceanAmount = 0.0f; // 0 to 1, of its surface
    float ice = 0.0f;         // 0 none, 1 covered
    float clouds = 0.0f;      // 0 none, 1 overcast
    glm::vec3 cloudColor{0.95f, 0.96f, 1.0f};
    glm::vec3 airColor{0.35f, 0.55f, 0.95f};
    float air = 0.0f; // the glow of its air at the edge, 0 none
    bool gas = false; // banded, a gas giant
    float seed = 0.0f; // moves the pattern, so no two look alike
    glm::vec2 rings{0.0f}; // inside and outside, in its radii; 0 none
    glm::vec3 ringColor{0.7f, 0.66f, 0.6f};

    // One plain colour, for a planet nothing more is known of.
    static PlanetLook Plain(const glm::vec3& color, float air)
    {
        PlanetLook look;
        look.groundA = color;
        look.groundB = color * 0.8f;
        look.air = air;
        look.clouds = 0.35f;
        return look;
    }
};

} // namespace pred
