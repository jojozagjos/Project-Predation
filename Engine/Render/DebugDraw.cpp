#include "Engine/Render/DebugDraw.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/ShaderLibrary.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstring>

namespace pred
{

bool DebugDraw::Init(ShaderLibrary& shaders)
{
    m_layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    m_program = shaders.LoadProgram("vs_debug", "fs_debug");
    if (!bgfx::isValid(m_program))
    {
        PRED_LOG_ERROR(Render, "DebugDraw: failed to load debug line program");
        return false;
    }
    m_vertices.reserve(4096);
    return true;
}

void DebugDraw::Shutdown()
{
    // Programs are owned by the ShaderLibrary.
    m_program = BGFX_INVALID_HANDLE;
    m_vertices.clear();
}

void DebugDraw::Line(const glm::vec3& a, const glm::vec3& b, uint32_t color)
{
    m_vertices.push_back({a.x, a.y, a.z, color});
    m_vertices.push_back({b.x, b.y, b.z, color});
}

void DebugDraw::Box(const glm::vec3& min, const glm::vec3& max, uint32_t color)
{
    const glm::vec3 c[8] = {
        {min.x, min.y, min.z}, {max.x, min.y, min.z}, {max.x, max.y, min.z}, {min.x, max.y, min.z},
        {min.x, min.y, max.z}, {max.x, min.y, max.z}, {max.x, max.y, max.z}, {min.x, max.y, max.z},
    };
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                     {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : edges)
    {
        Line(c[edge[0]], c[edge[1]], color);
    }
}

void DebugDraw::BoxOriented(const glm::mat4& transform, const glm::vec3& halfExtents, uint32_t color)
{
    glm::vec3 corners[8];
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec3 local{(i & 1) ? halfExtents.x : -halfExtents.x, (i & 2) ? halfExtents.y : -halfExtents.y,
                              (i & 4) ? halfExtents.z : -halfExtents.z};
        corners[i] = glm::vec3(transform * glm::vec4(local, 1.0f));
    }
    // Pairs of corner indices that differ in exactly one bit are the box edges.
    for (int i = 0; i < 8; ++i)
    {
        for (int bit = 1; bit < 8; bit <<= 1)
        {
            const int j = i | bit;
            if (j != i)
            {
                Line(corners[i], corners[j], color);
            }
        }
    }
}

void DebugDraw::Axes(const glm::mat4& transform, float size)
{
    const glm::vec3 origin = glm::vec3(transform[3]);
    const glm::vec3 x = glm::vec3(transform[0]) * size;
    const glm::vec3 y = glm::vec3(transform[1]) * size;
    const glm::vec3 z = glm::vec3(transform[2]) * size;
    Line(origin, origin + x, Color::kRed);
    Line(origin, origin + y, Color::kGreen);
    Line(origin, origin + z, Color::kBlue);
}

void DebugDraw::Grid(float halfExtent, float step, float height, uint32_t minorColor, uint32_t majorColor,
                     int majorEvery)
{
    if (step <= 0.0f)
    {
        return;
    }
    const int count = static_cast<int>(std::floor(halfExtent / step));
    for (int i = -count; i <= count; ++i)
    {
        const float offset = static_cast<float>(i) * step;
        const uint32_t color = (majorEvery > 0 && i % majorEvery == 0) ? majorColor : minorColor;
        Line({offset, height, -halfExtent}, {offset, height, halfExtent}, color);
        Line({-halfExtent, height, offset}, {halfExtent, height, offset}, color);
    }
}

void DebugDraw::Sphere(const glm::vec3& center, float radius, uint32_t color, int segments)
{
    segments = segments < 4 ? 4 : segments;
    const float twoPi = glm::two_pi<float>();
    for (int i = 0; i < segments; ++i)
    {
        const float a0 = twoPi * static_cast<float>(i) / static_cast<float>(segments);
        const float a1 = twoPi * static_cast<float>(i + 1) / static_cast<float>(segments);
        const float c0 = std::cos(a0) * radius;
        const float s0 = std::sin(a0) * radius;
        const float c1 = std::cos(a1) * radius;
        const float s1 = std::sin(a1) * radius;
        Line(center + glm::vec3(c0, s0, 0.0f), center + glm::vec3(c1, s1, 0.0f), color);
        Line(center + glm::vec3(c0, 0.0f, s0), center + glm::vec3(c1, 0.0f, s1), color);
        Line(center + glm::vec3(0.0f, c0, s0), center + glm::vec3(0.0f, c1, s1), color);
    }
}

void DebugDraw::Flush(bgfx::ViewId view)
{
    if (m_vertices.empty() || !bgfx::isValid(m_program))
    {
        m_vertices.clear();
        return;
    }

    uint32_t count = static_cast<uint32_t>(m_vertices.size());
    const uint32_t available = bgfx::getAvailTransientVertexBuffer(count, m_layout);
    if (available < count)
    {
        if (!m_warnedOverflow)
        {
            PRED_LOG_WARN(Render, "DebugDraw: transient buffer full ({} of {} vertices drawn)", available, count);
            m_warnedOverflow = true;
        }
        count = available - (available % 2);
    }
    if (count == 0)
    {
        m_vertices.clear();
        return;
    }

    bgfx::TransientVertexBuffer tvb;
    bgfx::allocTransientVertexBuffer(&tvb, count, m_layout);
    std::memcpy(tvb.data, m_vertices.data(), static_cast<size_t>(count) * sizeof(Vertex));

    bgfx::setVertexBuffer(0, &tvb, 0, count);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                   BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA);
    bgfx::submit(view, m_program);

    m_vertices.clear();
}

} // namespace pred
