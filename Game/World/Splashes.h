#pragma once

#include "Engine/Render/SpriteRenderer.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

// Water thrown up where something breaks its surface -- a foot coming down in the shallows, a swimmer's stroke: soft drops
// flying up and falling back, streaked as they go, and a quick puff of white where it broke, drawn as the weather is
// (soft blots, Sprite). Each drop gone as it falls back through the surface it came from.
class Splashes
{
public:
    // At `at` on the surface (its y the water's level there), as hard as `strength` (0 to 1: a step, a kick), the water's
    // own colour `tint` (white-grey for water, sickly for acid).
    void Emit(const glm::vec3& at, float strength, const glm::vec3& tint);
    void Update(float dt);
    // What to draw now, lit by `light` (what the sky and the sun give a white thing here) and fogged towards `fog` from
    // `fogStart` to `fogEnd` metres from `eye`.
    void Sprites(std::vector<Sprite>& out, const glm::vec3& light, const glm::vec3& fog, float fogStart, float fogEnd, const glm::vec3& eye) const;
    void Clear() { m_drops.clear(); }

private:
    struct Drop
    {
        glm::vec3 at{0.0f};
        glm::vec3 velocity{0.0f};
        glm::vec3 tint{1.0f};
        float level = 0.0f; // the surface it came out of
        float age = 0.0f;
        float life = 1.0f;
        float size = 0.03f;
        bool foam = false;  // the puff where it broke, not a drop
    };
    float Random();

    std::vector<Drop> m_drops;
    uint32_t m_state = 0x51A5Bu;
};

} // namespace pred
