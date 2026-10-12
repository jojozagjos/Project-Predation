#pragma once

#include <bgfx/bgfx.h>
#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

class ShaderLibrary;

// One soft blot of colour in the air: a snowflake, a raindrop's streak, a grain of sand, a puff of dust or mist.
struct Sprite
{
    glm::vec3 at{0.0f};
    glm::vec3 colour{1.0f}; // linear light, lit and fogged already
    float alpha = 1.0f;
    float size = 0.05f;     // across, metres
    float length = 0.0f;    // along `along`, metres, for a streak (0: round, facing the eye)
    glm::vec3 along{0.0f, -1.0f, 0.0f};
};

// Sprites drawn over the picture already there, blended, farthest first: what the world's opaque drawing cannot show -- the
// air with something in it. Each faces the eye (or, streaked, turns about the way it streaks to face it as nearly as it
// can); tested against the world's depth, so walls hide them, but hiding nothing itself.
class SpriteRenderer
{
public:
    bool Init(ShaderLibrary& shaders);
    void Shutdown();

    // Into `view` (one drawn after the sky, into the world's picture, with the world's camera), in `depthTest`'s
    // convention. `sprites` is sorted in place.
    void Draw(bgfx::ViewId view, uint64_t depthTest, const glm::vec3& eye, std::vector<Sprite>& sprites);

private:
    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout m_layout;
};

} // namespace pred
