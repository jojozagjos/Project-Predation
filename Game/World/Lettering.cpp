#include "Game/World/Lettering.h"

#include <glm/vec3.hpp>

#include <array>
#include <cctype>
#include <cstdint>

namespace pred
{

namespace Lettering
{

namespace
{

// Each glyph seven rows of five dots, the top row first, the leftmost dot the highest bit.
struct Glyph
{
    char c;
    std::array<uint8_t, 7> rows;
};

constexpr Glyph kGlyphs[] = {
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {'/', {0b00001, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b10000}},
    {'<', {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010}},
    {'>', {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000}},
    {'&', {0b01100, 0b10010, 0b10100, 0b01000, 0b10101, 0b10010, 0b01101}},
    // Up and down arrows (a lift's, going), for ^ and ~.
    {'^', {0b00100, 0b01110, 0b10101, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'~', {0b00100, 0b00100, 0b00100, 0b00100, 0b10101, 0b01110, 0b00100}},
};

const Glyph* Find(char c)
{
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const Glyph& glyph : kGlyphs)
    {
        if (glyph.c == upper)
        {
            return &glyph;
        }
    }
    return nullptr;
}

// Five dots and one between letters.
constexpr int kAdvance = 6;

} // namespace

float Width(const std::string& text, float height)
{
    const float dot = height / 7.0f;
    return text.empty() ? 0.0f : (static_cast<float>(text.size() * kAdvance) - 1.0f) * dot;
}

MeshData Mesh(const std::string& text, float height)
{
    MeshData mesh;
    const float dot = height / 7.0f;
    // Read by somebody in front of it, facing its +z: their right is its -x, so the letters go that way from its left edge.
    const float left = Width(text, height) * 0.5f;
    const glm::vec3 normal{0.0f, 0.0f, -1.0f};
    for (size_t i = 0; i < text.size(); ++i)
    {
        const Glyph* glyph = Find(text[i]);
        if (glyph == nullptr)
        {
            continue;
        }
        const float start = left - static_cast<float>(i * kAdvance) * dot;
        for (int row = 0; row < 7; ++row)
        {
            const uint8_t bits = glyph->rows[static_cast<size_t>(row)];
            const float top = height * 0.5f - static_cast<float>(row) * dot;
            // Each run of dots along the row one piece.
            for (int col = 0; col < 5;)
            {
                if ((bits & (1u << (4 - col))) == 0)
                {
                    ++col;
                    continue;
                }
                int end = col;
                while (end < 5 && (bits & (1u << (4 - end))) != 0)
                {
                    ++end;
                }
                const float x0 = start - static_cast<float>(col) * dot;
                const float x1 = start - static_cast<float>(end) * dot;
                const auto base = static_cast<uint32_t>(mesh.vertices.size());
                // Wound to face -z (as Primitives' quads are: the normal is the cross of the first two edges).
                mesh.vertices.push_back(MeshVertex{{x0, top - dot, 0.0f}, normal, {0.0f, 0.0f}});
                mesh.vertices.push_back(MeshVertex{{x1, top - dot, 0.0f}, normal, {1.0f, 0.0f}});
                mesh.vertices.push_back(MeshVertex{{x1, top, 0.0f}, normal, {1.0f, 1.0f}});
                mesh.vertices.push_back(MeshVertex{{x0, top, 0.0f}, normal, {0.0f, 1.0f}});
                mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                col = end;
            }
        }
    }
    return mesh;
}

} // namespace Lettering

} // namespace pred
