#include "Game/World/Shuttle.h"

#include <cmath>

namespace pred::Shuttle
{

namespace
{

// Measured along it (+ towards the ramp, and the site), across it (+ to the right looking down the ramp) and up from
// the pad, in metres.
constexpr float kBack = -5.5f;   // the front wall's outside, where the nose begins
constexpr float kDoor = 2.5f;    // the open back of the cabin, where the ramp starts
constexpr float kHalfWidth = 2.0f;
constexpr float kWall = 0.2f;
constexpr float kDeck = 0.7f;    // the cabin floor
constexpr float kCeiling = 3.2f; // two and a half metres of headroom
constexpr float kRampRun = 2.5f;
constexpr float kRampWidth = 3.2f;
constexpr float kRampThickness = 0.12f;

struct Frame
{
    glm::vec3 base;
    glm::vec3 along;  // towards the ramp
    glm::vec3 across; // to the right, looking down the ramp
    glm::quat turn;

    glm::vec3 At(float a, float c, float y) const { return base + along * a + across * c + glm::vec3(0.0f, y, 0.0f); }
};

Frame FrameOf(const glm::vec3& base, float yaw)
{
    return {base, {std::sin(yaw), 0.0f, -std::cos(yaw)}, {std::cos(yaw), 0.0f, std::sin(yaw)}, Turn(yaw)};
}

} // namespace

glm::quat Turn(float yaw)
{
    // Its own -z down the ramp and +x to the right: a turn of -yaw about y takes (0, 0, -1) to (sin, 0, -cos).
    return glm::angleAxis(-yaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

std::vector<Piece> Pieces(const glm::vec3& base, float yaw)
{
    const Frame frame = FrameOf(base, yaw);
    std::vector<Piece> pieces;
    const auto box = [&](float a0, float a1, float c0, float c1, float y0, float y1, bool trim = false)
    {
        Piece piece;
        piece.centre = frame.At((a0 + a1) * 0.5f, (c0 + c1) * 0.5f, (y0 + y1) * 0.5f);
        piece.size = {c1 - c0, y1 - y0, a1 - a0};
        piece.rotation = frame.turn;
        piece.trim = trim;
        pieces.push_back(piece);
    };

    // Four legs, and the belly between them: it stands on the pad rather than lying on it.
    for (const float a : {kBack + 0.7f, kDoor - 0.7f})
    {
        for (const float c : {-1.3f, 1.3f})
        {
            box(a - 0.15f, a + 0.15f, c - 0.15f, c + 0.15f, 0.0f, 0.25f, true);
        }
    }
    box(kBack, kDoor, -1.6f, 1.6f, 0.25f, 0.55f, true);
    // The cabin: its floor, its sides and front, its roof, and a header over the open back.
    box(kBack, kDoor, -kHalfWidth, kHalfWidth, 0.55f, kDeck);
    box(kBack, kDoor, kHalfWidth - kWall, kHalfWidth, kDeck, kCeiling);
    box(kBack, kDoor, -kHalfWidth, -kHalfWidth + kWall, kDeck, kCeiling);
    box(kBack, kBack + kWall, -kHalfWidth + kWall, kHalfWidth - kWall, kDeck, kCeiling);
    box(kBack, kDoor, -kHalfWidth, kHalfWidth, kCeiling, kCeiling + 0.25f);
    box(kDoor - kWall, kDoor, -kHalfWidth + kWall, kHalfWidth - kWall, kCeiling - 0.3f, kCeiling);
    // Benches down both sides.
    box(-3.6f, 0.8f, kHalfWidth - kWall - 0.45f, kHalfWidth - kWall, kDeck, kDeck + 0.45f);
    box(-3.6f, 0.8f, -kHalfWidth + kWall, -kHalfWidth + kWall + 0.45f, kDeck, kDeck + 0.45f);
    // Outside: the nose ahead of the front wall, an engine either side, a fin on the roof.
    box(kBack - 1.2f, kBack, -1.5f, 1.5f, 1.1f, 3.0f, true);
    box(-5.0f, -1.2f, kHalfWidth, kHalfWidth + 0.8f, 1.1f, 2.3f, true);
    box(-5.0f, -1.2f, -kHalfWidth - 0.8f, -kHalfWidth, 1.1f, 2.3f, true);
    box(-5.2f, -3.2f, -0.08f, 0.08f, kCeiling + 0.25f, kCeiling + 1.2f, true);

    // The ramp, from the cabin floor down to the pad: its top surface runs from the lip of the floor to the pad's
    // surface, and the plank is that surface less half its thickness along the way it faces.
    const float rise = kDeck;
    const float length = std::sqrt(kRampRun * kRampRun + rise * rise);
    const float middleA = kDoor + kRampRun * 0.5f;
    const float middleY = rise * 0.5f;
    const float normalA = rise / length;
    const float normalY = kRampRun / length;
    Piece ramp;
    ramp.centre = frame.At(middleA - normalA * kRampThickness * 0.5f, 0.0f, middleY - normalY * kRampThickness * 0.5f);
    ramp.size = {kRampWidth, kRampThickness, length};
    // Tipped about its own x so the end towards the cabin (its +z) is the high one.
    ramp.rotation = frame.turn * glm::angleAxis(-std::atan2(rise, kRampRun), glm::vec3(1.0f, 0.0f, 0.0f));
    pieces.push_back(ramp);
    return pieces;
}

bool Aboard(const glm::vec3& base, float yaw, const glm::vec3& at)
{
    const Frame frame = FrameOf(base, yaw);
    const glm::vec3 offset = at - base;
    const float a = glm::dot(offset, frame.along);
    const float c = glm::dot(offset, frame.across);
    return a > kBack + kWall && a < kDoor && std::abs(c) < kHalfWidth - kWall && offset.y > kDeck - 0.5f && offset.y < kCeiling;
}

glm::vec3 Arrival(const glm::vec3& base, float yaw)
{
    return FrameOf(base, yaw).At(-1.0f, 0.0f, kDeck + 0.5f);
}

glm::vec3 ConsoleSize()
{
    return {1.4f, 1.0f, 0.55f};
}

glm::vec3 Console(const glm::vec3& base, float yaw)
{
    const glm::vec3 size = ConsoleSize();
    return FrameOf(base, yaw).At(kBack + kWall + size.z * 0.5f + 0.01f, 0.0f, kDeck + size.y * 0.5f);
}

glm::vec3 CabinLamp(const glm::vec3& base, float yaw)
{
    return FrameOf(base, yaw).At(-1.5f, 0.0f, kCeiling - 0.05f);
}

} // namespace pred::Shuttle
