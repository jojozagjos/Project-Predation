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
// Drawn as soft sprites (SpriteRenderer): flakes, streaks of rain, grains. And, as it thickens, the air itself full of it --
// clouds of it rolling past, thick and thin, through the air round the eye (AirNow, drawn through the depth of the picture: not
// flat cards turned to face the eye, which is what they looked like) -- with the fog it brings (Visibility), what stops
// anybody seeing far in a storm.
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
    // How much of a world's weather is coming down at a site just now, as a share of its usual (0 to about 1.3): spells of it
    // building up and easing off over minutes, lulls between them, and now and then a peak past the usual -- a downpour that
    // turns to a thunderstorm. From the campaign's clock, so every player has the same weather at the same moment.
    static float SpellAt(uint32_t seed, double clock);

    // The light it is seen in: what lights it (the sky's and the sun's, together), and the fog it fades into.
    struct Light
    {
        glm::vec3 light{1.0f};
        glm::vec3 fogColour{0.0f};
        float fogStart = 1.0e4f;
        float fogEnd = 2.0e4f;
    };

    // `intensity` 0 to 1, how much of it there is; `tint` the colour of dust (the ground it was lifted from); `wind`
    // metres a second of drift, sideways; `ground` how high the ground is under the eye, for what blows along it; `detail`
    // how much of it is drawn (the r.weather setting, 0.3 to 1: fewer flakes, not less weather). Then Sprites() and AirNow()
    // are this frame's to draw. `indoors` says whether anything within a distance of a point is under a roof:
    // nothing is drawn that would reach in.
    void Update(const glm::vec3& eye, float ground, float dt, Kind kind, float intensity, const glm::vec3& tint, const glm::vec3& wind,
                const Light& light, float detail, const std::function<bool(const glm::vec3&, float)>& indoors);
    std::vector<Sprite>& Sprites() { return m_sprites; }

    // The air thick with it, as the picture is finished (PostProcess::Settings::air): how thick at the thickest (a share of
    // the light it takes away each metre; 0 clear), the colour it is lit, and how fast and which way its clouds roll past
    // (metres a second).
    struct Air
    {
        float thickness = 0.0f;
        glm::vec3 colour{0.0f};
        glm::vec3 drift{0.0f};
        glm::vec3 moved{0.0f}; // how far its clouds have rolled so far
        float overGround = 1.7f; // how high the eye is over the ground (it is thickest low down)
    };
    const Air& AirNow() const { return m_air; }

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
    };
    void Make(Kind kind, const glm::vec3& eye);

    Kind m_kind = Kind::None;
    std::vector<Piece> m_pieces;
    std::vector<Sprite> m_sprites;
    Air m_air;
    glm::vec3 m_airMoved{0.0f};
    float m_clock = 0.0f;
    uint32_t m_seed = 0x5A0Fu;
};

} // namespace pred
