#pragma once

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

class ShaderLibrary;

// Packed ABGR color helpers matching bgfx's Uint8x4 normalized Color0 attribute.
namespace Color
{
constexpr uint32_t RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(r);
}
constexpr uint32_t kWhite = RGBA(255, 255, 255);
constexpr uint32_t kRed = RGBA(230, 60, 60);
constexpr uint32_t kGreen = RGBA(70, 220, 90);
constexpr uint32_t kBlue = RGBA(70, 120, 255);
constexpr uint32_t kYellow = RGBA(240, 220, 70);
constexpr uint32_t kCyan = RGBA(70, 220, 230);
constexpr uint32_t kMagenta = RGBA(230, 70, 230);
constexpr uint32_t kGrey = RGBA(120, 120, 130);
constexpr uint32_t kDarkGrey = RGBA(60, 60, 70);
} // namespace Color

// Immediate-mode line drawing for debug overlays. Lines are accumulated during
// the frame and submitted in one draw call by Flush().
class DebugDraw
{
public:
    bool Init(ShaderLibrary& shaders);
    void Shutdown();

    void Line(const glm::vec3& a, const glm::vec3& b, uint32_t color);
    void Box(const glm::vec3& min, const glm::vec3& max, uint32_t color);
    void Axes(const glm::mat4& transform, float size = 1.0f);
    // `height` lifts the grid off y = 0 so it does not z-fight with a floor at the same level.
    void Grid(float halfExtent, float step, float height = 0.0f, uint32_t minorColor = Color::kDarkGrey,
              uint32_t majorColor = Color::kGrey, int majorEvery = 5);
    void Sphere(const glm::vec3& center, float radius, uint32_t color, int segments = 16);

    void Flush(bgfx::ViewId view);
    size_t PendingLineCount() const { return m_vertices.size() / 2; }

private:
    struct Vertex
    {
        float x, y, z;
        uint32_t abgr;
    };

    std::vector<Vertex> m_vertices;
    bgfx::VertexLayout m_layout;
    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE;
    bool m_warnedOverflow = false;
};

} // namespace pred
