#include "Engine/Render/SpriteRenderer.h"

#include "Engine/Render/ShaderLibrary.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdint>

namespace pred
{

namespace
{

struct Corner
{
    float x, y, z;
    float u, v;
    float r, g, b, a;
};

} // namespace

bool SpriteRenderer::Init(ShaderLibrary& shaders)
{
    m_program = shaders.LoadProgram("vs_sprite", "fs_sprite");
    m_layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .end();
    return bgfx::isValid(m_program);
}

void SpriteRenderer::Shutdown()
{
    if (bgfx::isValid(m_program))
    {
        bgfx::destroy(m_program);
        m_program = BGFX_INVALID_HANDLE;
    }
}

void SpriteRenderer::Draw(bgfx::ViewId view, uint64_t depthTest, const glm::vec3& eye, std::vector<Sprite>& sprites)
{
    if (!bgfx::isValid(m_program) || sprites.empty())
    {
        return;
    }
    // Farthest first, so each is blended over what is behind it.
    std::sort(sprites.begin(), sprites.end(), [&](const Sprite& a, const Sprite& b)
              { return glm::dot(a.at - eye, a.at - eye) > glm::dot(b.at - eye, b.at - eye); });
    const uint32_t wanted = static_cast<uint32_t>(std::min<size_t>(sprites.size(), 16000));
    uint32_t count = wanted;
    while (count > 0 && (bgfx::getAvailTransientVertexBuffer(count * 4, m_layout) < count * 4 || bgfx::getAvailTransientIndexBuffer(count * 6) < count * 6))
    {
        count /= 2;
    }
    if (count == 0)
    {
        return;
    }
    bgfx::TransientVertexBuffer vertices;
    bgfx::TransientIndexBuffer indices;
    bgfx::allocTransientVertexBuffer(&vertices, count * 4, m_layout);
    bgfx::allocTransientIndexBuffer(&indices, count * 6);
    auto* corner = reinterpret_cast<Corner*>(vertices.data);
    auto* index = reinterpret_cast<uint16_t*>(indices.data);
    // When fewer fit than were asked for, the nearest are the ones kept.
    const size_t first = sprites.size() - count;
    for (uint32_t i = 0; i < count; ++i)
    {
        const Sprite& sprite = sprites[first + i];
        const glm::vec3 toEye = eye - sprite.at;
        glm::vec3 across;
        glm::vec3 up;
        if (sprite.length > 0.0f)
        {
            // A streak: along its way, as wide as it is across, turned about its way to face the eye.
            up = sprite.along * (sprite.length * 0.5f);
            const glm::vec3 side = glm::cross(sprite.along, toEye);
            const float sideLength = glm::length(side);
            across = (sideLength > 1.0e-5f ? side / sideLength : glm::vec3(1.0f, 0.0f, 0.0f)) * (sprite.size * 0.5f);
        }
        else
        {
            const glm::vec3 forward = glm::length(toEye) > 1.0e-5f ? glm::normalize(-toEye) : glm::vec3(0.0f, 0.0f, -1.0f);
            const glm::vec3 right = std::abs(forward.y) < 0.99f ? glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)))
                                                                : glm::vec3(1.0f, 0.0f, 0.0f);
            across = right * (sprite.size * 0.5f);
            up = glm::cross(right, forward) * (sprite.size * 0.5f);
        }
        const glm::vec3 points[4] = {sprite.at - across - up, sprite.at + across - up, sprite.at + across + up, sprite.at - across + up};
        const float uv[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
        for (int c = 0; c < 4; ++c)
        {
            corner[i * 4 + c] = {points[c].x, points[c].y, points[c].z, uv[c][0], uv[c][1], sprite.colour.r, sprite.colour.g, sprite.colour.b, sprite.alpha};
        }
        const auto base = static_cast<uint16_t>(i * 4);
        const uint16_t quad[6] = {base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                                  base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)};
        std::copy(quad, quad + 6, index + i * 6);
    }
    bgfx::setVertexBuffer(0, &vertices);
    bgfx::setIndexBuffer(&indices);
    bgfx::setState(BGFX_STATE_WRITE_RGB | depthTest | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    bgfx::submit(view, m_program);
}

} // namespace pred
