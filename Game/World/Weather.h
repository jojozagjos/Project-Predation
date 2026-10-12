#pragma once

#include "Engine/Render/SpriteRenderer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace pred
{

// What falls from a world's sky round whoever is looking -- snow, rain, dust or ash -- in a box that goes with the eye,
// each piece coming round again at the other side as it leaves, so it seems to fill the sky wherever anybody goes; blown
// by the wind, gusting, and not indoors.
//
// Drawn as soft sprites (SpriteRenderer): flakes, streaks of rain, grains; and, as it thickens, big soft puffs of the stuff
// drifting past, the air itself full of it -- with the fog it brings (Visibility), what stops anybody seeing far in a storm.
class Weather
{
public:
    enum class Kind : uint8_t
    {
        None,
        Snow,
        Rain,
        Dust,
        Ash
    };
    // From a weather's particles ("snow", "rain", "dust", "ash"; anything else none).
    static Kind KindOf(const std::string& particles);
    // How hard the wind is blowing just now, as a share of its usual (about 0.4 to 1.6), `seconds` into anything: the same
    // gusting moves what falls and thickens the fog with it.
    static float GustAt(float seconds);

    // The light it is seen in: what lights it (the sky's and the sun's, together), and the fog it fades into.
    struct Light
    {
        glm::vec3 light{1.0f};
        glm::vec3 fogColour{0.0f};
        float fogStart = 1.0e4f;
        float fogEnd = 2.0e4f;
    };

    // `intensity` 0 to 1, how much of it there is; `tint` the colour of dust (the ground it was lifted from); `wind`
    // metres a second of drift, sideways; `ground` how high the ground is under the eye, for what blows along it. Then
    // Sprites() is this frame's to draw.
    void Update(const glm::vec3& eye, float ground, float dt, Kind kind, float intensity, const glm::vec3& tint, const glm::vec3& wind,
                const Light& light, const std::function<bool(const glm::vec3&)>& indoors);
    std::vector<Sprite>& Sprites() { return m_sprites; }

private:
    struct Piece
    {
        glm::vec3 at{0.0f};
        float fall = 1.0f;
        float sway = 0.0f;
        float size = 0.03f;
        float height = 0.2f; // blown along the ground: how far over it
        float alpha = 1.0f;
        bool low = false;    // blown along the ground rather than falling
        bool glow = false;   // an ember
        bool puff = false;   // a big soft puff of it: the air thick with it
    };
    void Make(Kind kind, const glm::vec3& eye);

    Kind m_kind = Kind::None;
    std::vector<Piece> m_pieces;
    std::vector<Sprite> m_sprites;
    float m_clock = 0.0f;
    uint32_t m_seed = 0x5A0Fu;
};

} // namespace pred
