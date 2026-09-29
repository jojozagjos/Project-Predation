#pragma once

// A screen's picture, drawn in by hand on the CPU a few times a second and put on something in the world as a texture:
// the devices carried in the hand, the briefing room's screens. Lines, boxes, turned boxes, rings, dots and a five by
// seven font; scan lines over it all for the look of an old tube.

#include "Engine/Render/TextureLibrary.h"

#include <glm/common.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pred
{

struct Rgb
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// Five by seven letters, a row a byte, the high five bits the columns.
inline const std::array<uint8_t, 7>* Glyph(char c)
{
    static const std::array<std::array<uint8_t, 7>, 43> kFont = {{
        {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70}, // 0
        {0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70}, // 1
        {0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8}, // 2
        {0xF8, 0x10, 0x20, 0x10, 0x08, 0x88, 0x70}, // 3
        {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10}, // 4
        {0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70}, // 5
        {0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70}, // 6
        {0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40}, // 7
        {0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70}, // 8
        {0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60}, // 9
        {0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88}, // A
        {0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0}, // B
        {0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70}, // C
        {0xE0, 0x90, 0x88, 0x88, 0x88, 0x90, 0xE0}, // D
        {0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8}, // E
        {0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0x80}, // F
        {0x70, 0x88, 0x80, 0xB8, 0x88, 0x88, 0x78}, // G
        {0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88}, // H
        {0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70}, // I
        {0x38, 0x10, 0x10, 0x10, 0x10, 0x90, 0x60}, // J
        {0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x88}, // K
        {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8}, // L
        {0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x88}, // M
        {0x88, 0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88}, // N
        {0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70}, // O
        {0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80}, // P
        {0x70, 0x88, 0x88, 0x88, 0xA8, 0x90, 0x68}, // Q
        {0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88}, // R
        {0x78, 0x80, 0x80, 0x70, 0x08, 0x08, 0xF0}, // S
        {0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}, // T
        {0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70}, // U
        {0x88, 0x88, 0x88, 0x88, 0x88, 0x50, 0x20}, // V
        {0x88, 0x88, 0x88, 0xA8, 0xA8, 0xA8, 0x50}, // W
        {0x88, 0x88, 0x50, 0x20, 0x50, 0x88, 0x88}, // X
        {0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x20}, // Y
        {0xF8, 0x08, 0x10, 0x20, 0x40, 0x80, 0xF8}, // Z
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x60}, // .
        {0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00}, // -
        {0x00, 0x60, 0x60, 0x00, 0x60, 0x60, 0x00}, // :
        {0x00, 0x00, 0x00, 0x00, 0x60, 0x20, 0x40}, // ,
        {0x08, 0x08, 0x10, 0x20, 0x40, 0x80, 0x80}, // /
        {0xC8, 0xC8, 0x10, 0x20, 0x40, 0x98, 0x98}, // %
        {0x00, 0x20, 0x20, 0xF8, 0x20, 0x20, 0x00}, // +
    }};
    if (c >= '0' && c <= '9')
    {
        return &kFont[static_cast<size_t>(c - '0')];
    }
    if (c >= 'A' && c <= 'Z')
    {
        return &kFont[static_cast<size_t>(10 + c - 'A')];
    }
    if (c == '.')
    {
        return &kFont[36];
    }
    if (c == '-')
    {
        return &kFont[37];
    }
    if (c == ':')
    {
        return &kFont[38];
    }
    if (c == ',')
    {
        return &kFont[39];
    }
    if (c == '/')
    {
        return &kFont[40];
    }
    if (c == '%')
    {
        return &kFont[41];
    }
    if (c == '+')
    {
        return &kFont[42];
    }
    return nullptr;
}

// A screen's picture, drawn in by hand: lines, boxes, rings, dots and letters.
struct ScreenCanvas
{
    ImageData image;
    int width = 0;
    int height = 0;

    ScreenCanvas(int w, int h) : width(w), height(h)
    {
        image.width = w;
        image.height = h;
        image.pixels.assign(static_cast<size_t>(w * h * 4), 255);
    }
    void Clear(Rgb colour)
    {
        for (size_t i = 0; i < image.pixels.size(); i += 4)
        {
            image.pixels[i] = colour.r;
            image.pixels[i + 1] = colour.g;
            image.pixels[i + 2] = colour.b;
        }
    }
    void Put(int x, int y, Rgb colour)
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
        {
            return;
        }
        uint8_t* p = &image.pixels[static_cast<size_t>((y * width + x) * 4)];
        p[0] = colour.r;
        p[1] = colour.g;
        p[2] = colour.b;
    }
    void Fill(float x0, float y0, float x1, float y1, Rgb colour)
    {
        const int ax = static_cast<int>(std::floor(std::min(x0, x1)));
        const int bx = static_cast<int>(std::ceil(std::max(x0, x1)));
        const int ay = static_cast<int>(std::floor(std::min(y0, y1)));
        const int by = static_cast<int>(std::ceil(std::max(y0, y1)));
        for (int y = std::max(ay, 0); y < std::min(by, height); ++y)
        {
            for (int x = std::max(ax, 0); x < std::min(bx, width); ++x)
            {
                Put(x, y, colour);
            }
        }
    }
    void Line(float x0, float y0, float x1, float y1, Rgb colour, int thick = 1)
    {
        const float length = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        const int steps = std::max(1, static_cast<int>(length));
        for (int i = 0; i <= steps; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const int x = static_cast<int>(x0 + (x1 - x0) * t);
            const int y = static_cast<int>(y0 + (y1 - y0) * t);
            for (int dy = 0; dy < thick; ++dy)
            {
                for (int dx = 0; dx < thick; ++dx)
                {
                    Put(x + dx - thick / 2, y + dy - thick / 2, colour);
                }
            }
        }
    }
    // Four corners in order round, filled: a room turned with the map.
    void FillQuad(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d, Rgb colour)
    {
        const glm::vec2 lo = glm::min(glm::min(a, b), glm::min(c, d));
        const glm::vec2 hi = glm::max(glm::max(a, b), glm::max(c, d));
        const std::array<glm::vec2, 4> corners{a, b, c, d};
        const auto cross = [](const glm::vec2& u, const glm::vec2& v) { return u.x * v.y - u.y * v.x; };
        for (int y = std::max(static_cast<int>(lo.y), 0); y <= std::min(static_cast<int>(hi.y), height - 1); ++y)
        {
            for (int x = std::max(static_cast<int>(lo.x), 0); x <= std::min(static_cast<int>(hi.x), width - 1); ++x)
            {
                const glm::vec2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                bool positive = false;
                bool negative = false;
                for (size_t i = 0; i < 4; ++i)
                {
                    const float side = cross(corners[(i + 1) % 4] - corners[i], p - corners[i]);
                    positive = positive || side > 0.0f;
                    negative = negative || side < 0.0f;
                }
                if (!(positive && negative))
                {
                    Put(x, y, colour);
                }
            }
        }
    }
    void Box(float x0, float y0, float x1, float y1, Rgb colour)
    {
        Line(x0, y0, x1, y0, colour);
        Line(x1, y0, x1, y1, colour);
        Line(x1, y1, x0, y1, colour);
        Line(x0, y1, x0, y0, colour);
    }
    void Dot(float cx, float cy, float radius, Rgb colour)
    {
        for (int y = static_cast<int>(cy - radius); y <= static_cast<int>(cy + radius); ++y)
        {
            for (int x = static_cast<int>(cx - radius); x <= static_cast<int>(cx + radius); ++x)
            {
                if ((static_cast<float>(x) - cx) * (static_cast<float>(x) - cx) + (static_cast<float>(y) - cy) * (static_cast<float>(y) - cy) <= radius * radius)
                {
                    Put(x, y, colour);
                }
            }
        }
    }
    void Arc(float cx, float cy, float radius, float from, float to, Rgb colour)
    {
        const int steps = std::max(8, static_cast<int>(radius * (to - from)));
        for (int i = 0; i <= steps; ++i)
        {
            const float a = from + (to - from) * static_cast<float>(i) / static_cast<float>(steps);
            Put(static_cast<int>(cx + std::sin(a) * radius), static_cast<int>(cy - std::cos(a) * radius), colour);
        }
    }
    void Text(int x, int y, const char* text, Rgb colour, int scale = 2)
    {
        for (const char* c = text; *c != '\0'; ++c, x += 6 * scale)
        {
            const std::array<uint8_t, 7>* glyph = Glyph(*c);
            if (glyph == nullptr)
            {
                continue;
            }
            for (int row = 0; row < 7; ++row)
            {
                for (int column = 0; column < 5; ++column)
                {
                    if (((*glyph)[static_cast<size_t>(row)] >> (7 - column)) & 1u)
                    {
                        Fill(static_cast<float>(x + column * scale), static_cast<float>(y + row * scale), static_cast<float>(x + (column + 1) * scale),
                             static_cast<float>(y + (row + 1) * scale), colour);
                    }
                }
            }
        }
    }
    int TextWidth(const char* text, int scale = 2) const { return static_cast<int>(std::strlen(text)) * 6 * scale - scale; }
    // The corner a device's case is mapped to: its colour, and alpha nought so that it does not glow as the screen does.
    void PaintCase(Rgb colour)
    {
        Fill(0.0f, 0.0f, 4.0f, 4.0f, colour);
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                image.pixels[static_cast<size_t>((y * width + x) * 4 + 3)] = 0;
            }
        }
    }
    // Scan lines, for the look of an old tube.
    void Lines()
    {
        for (int y = 0; y < height; y += 3)
        {
            for (int x = 0; x < width; ++x)
            {
                uint8_t* p = &image.pixels[static_cast<size_t>((y * width + x) * 4)];
                p[0] = static_cast<uint8_t>(p[0] * 0.7f);
                p[1] = static_cast<uint8_t>(p[1] * 0.7f);
                p[2] = static_cast<uint8_t>(p[2] * 0.7f);
            }
        }
    }
};

} // namespace pred
