#include "Game/World/FacilityFurniture.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace pred
{

namespace FacilityFurniture
{

namespace
{

using Kind = FacilityMap::Piece::Kind;
using Form = FacilityMap::Dressing::Form;
using Fitting = FacilityLayout::Fitting;

// One piece of furnishing, drawn in its own frame: its foot's middle at the origin, across along x, its back (against the
// wall, if it stands against one) towards +z and its front towards -z, up along y.
struct Maker
{
    FacilityMap::Blueprint& out;
    glm::vec3 foot{0.0f};
    glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};

    void Add(Form form, Kind kind, const glm::vec3& centre, const glm::vec3& size, const glm::quat& local = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        FacilityMap::Dressing dressing;
        dressing.form = form;
        dressing.kind = kind;
        dressing.centre = foot + turn * centre;
        dressing.size = size;
        dressing.rotation = turn * local;
        out.furnishing.push_back(dressing);
    }
    // A box between two corners, in its own frame.
    void Box(Kind kind, const glm::vec3& lo, const glm::vec3& hi) { Add(Form::Box, kind, (lo + hi) * 0.5f, hi - lo); }
    // An upright cylinder: its foot's middle, how wide and how tall.
    void Post(Kind kind, const glm::vec3& at, float across, float high) { Add(Form::Cylinder, kind, at + glm::vec3(0.0f, high * 0.5f, 0.0f), {across, high, across}); }
    // One lying along x or along z.
    void Rod(Kind kind, const glm::vec3& middle, float across, float length, bool alongX)
    {
        const glm::quat lie = glm::angleAxis(glm::half_pi<float>(), alongX ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
        Add(Form::Cylinder, kind, middle, {across, length, across}, lie);
    }
    void Ball(Kind kind, const glm::vec3& middle, const glm::vec3& size) { Add(Form::Ball, kind, middle, size); }

    // Four legs under a top `w` by `d`, `high` tall.
    void Legs(Kind kind, float w, float d, float high, float inset = 0.05f, float thick = 0.05f)
    {
        for (const float sx : {-1.0f, 1.0f})
        {
            for (const float sz : {-1.0f, 1.0f})
            {
                const glm::vec3 at{sx * (w * 0.5f - inset), 0.0f, sz * (d * 0.5f - inset)};
                Box(kind, at + glm::vec3(-thick * 0.5f, 0.0f, -thick * 0.5f), at + glm::vec3(thick * 0.5f, high, thick * 0.5f));
            }
        }
    }
    // A chair facing `facing` (+1: towards +z) at `at`: seat, back, its post and its foot.
    void Chair(const glm::vec3& at, float facing)
    {
        Box(Kind::Fabric, at + glm::vec3(-0.24f, 0.44f, -0.24f), at + glm::vec3(0.24f, 0.52f, 0.24f));
        Box(Kind::Fabric, at + glm::vec3(-0.23f, 0.56f, -facing * 0.27f - 0.03f), at + glm::vec3(0.23f, 1.0f, -facing * 0.27f + 0.03f));
        Box(Kind::DarkSteel, at + glm::vec3(-0.02f, 0.52f, -facing * 0.25f - 0.02f), at + glm::vec3(0.02f, 0.57f, -facing * 0.2f + 0.02f));
        Post(Kind::DarkSteel, at + glm::vec3(0.0f, 0.06f, 0.0f), 0.05f, 0.38f);
        Box(Kind::DarkSteel, at + glm::vec3(-0.28f, 0.03f, -0.03f), at + glm::vec3(0.28f, 0.07f, 0.03f));
        Box(Kind::DarkSteel, at + glm::vec3(-0.03f, 0.031f, -0.28f), at + glm::vec3(0.03f, 0.069f, 0.28f));
    }
    // A screen on a stand, facing -z, its foot's middle at `at`.
    // A screen on its stand, facing its front (-z) -- or, `facing` +1, its back (+z), for whoever sits on that side of it.
    void Monitor(const glm::vec3& at, float w, float h, float facing = -1.0f)
    {
        const float s = -facing;
        const auto box = [&](Kind kind, glm::vec3 lo, glm::vec3 hi)
        {
            lo.z *= s;
            hi.z *= s;
            Box(kind, at + glm::min(lo, hi), at + glm::max(lo, hi));
        };
        box(Kind::DarkPlastic, {-0.12f, 0.0f, -0.08f}, {0.12f, 0.015f, 0.08f});
        box(Kind::DarkPlastic, {-0.025f, 0.015f, 0.0f}, {0.025f, 0.12f, 0.03f});
        box(Kind::DarkPlastic, {-w * 0.5f, 0.1f, -0.02f}, {w * 0.5f, 0.1f + h, 0.02f});
        box(Kind::Screen, {-w * 0.5f + 0.015f, 0.115f, -0.024f}, {w * 0.5f - 0.015f, 0.085f + h, -0.019f});
    }
    // Boxes on a shelf from `x0` to `x1` at `y`, `deep` deep.
    void Boxes(float x0, float x1, float y, float deep, uint32_t roll)
    {
        float x = x0 + 0.04f;
        int i = 0;
        while (x < x1 - 0.25f)
        {
            const uint32_t r = roll * 2654435761u + static_cast<uint32_t>(i++) * 40503u;
            const float w = 0.25f + static_cast<float>(r % 23u) * 0.012f;
            const float h = 0.18f + static_cast<float>((r >> 5) % 17u) * 0.012f;
            const float d = deep * (0.6f + static_cast<float>((r >> 9) % 7u) * 0.05f);
            if (x + w > x1 - 0.02f)
            {
                break;
            }
            if ((r >> 13) % 5u != 0u)
            {
                Box(Kind::Cardboard, {x, y, deep * 0.5f - d}, {x + w, y + h, deep * 0.5f - 0.02f});
            }
            x += w + 0.04f;
        }
    }
};

void Build(Maker& m, Fitting kind, uint32_t roll)
{
    const glm::vec3 size = FacilityLayout::FittingSize(kind);
    const float w = size.x;
    const float d = size.y;
    const float back = d * 0.5f;
    switch (kind)
    {
    case Fitting::Reception:
    {
        // A counter facing the room, its returns either side; behind it a worktop with a screen facing whoever sits at it, and
        // their chair, between the counter and the wall.
        const float counter = -back + 0.85f;
        m.Box(Kind::Wood, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, 1.04f, -back + 0.12f});
        m.Box(Kind::Wood, {-w * 0.5f, 0.0f, -back + 0.12f}, {-w * 0.5f + 0.1f, 1.04f, counter});
        m.Box(Kind::Wood, {w * 0.5f - 0.1f, 0.0f, -back + 0.12f}, {w * 0.5f, 1.04f, counter});
        m.Box(Kind::Plastic, {-w * 0.5f - 0.03f, 1.04f, -back - 0.05f}, {w * 0.5f + 0.03f, 1.08f, -back + 0.3f});
        m.Box(Kind::Plastic, {-w * 0.5f + 0.1f, 0.72f, -back + 0.12f}, {w * 0.5f - 0.1f, 0.76f, counter});
        m.Monitor({0.35f, 0.76f, -back + 0.4f}, 0.5f, 0.3f, 1.0f);
        m.Box(Kind::DarkPlastic, {0.15f, 0.76f, -back + 0.62f}, {0.55f, 0.785f, -back + 0.77f});
        m.Chair({0.35f, 0.0f, counter + 0.4f}, -1.0f);
        break;
    }
    case Fitting::Seat:
    {
        m.Box(Kind::Fabric, {-w * 0.5f, 0.4f, -back}, {w * 0.5f, 0.48f, back - 0.06f});
        m.Box(Kind::Fabric, {-w * 0.5f, 0.48f, back - 0.12f}, {w * 0.5f, 0.88f, back - 0.04f});
        m.Legs(Kind::DarkSteel, w, d - 0.06f, 0.4f, 0.08f, 0.04f);
        break;
    }
    case Fitting::PlantPot:
    {
        m.Add(Form::Cone, Kind::DarkPlastic, {0.0f, 0.22f, 0.0f}, {0.42f, 0.44f, 0.52f});
        m.Post(Kind::Cardboard, {0.0f, 0.38f, 0.0f}, 0.44f, 0.04f);
        m.Post(Kind::Wood, {0.0f, 0.4f, 0.0f}, 0.04f, 0.35f);
        m.Ball(Kind::Leaf, {0.0f, 0.82f, 0.0f}, {0.6f, 0.55f, 0.6f});
        m.Ball(Kind::Leaf, {0.12f, 0.98f, -0.05f}, {0.36f, 0.32f, 0.36f});
        break;
    }
    case Fitting::Noticeboard:
    {
        m.Box(Kind::DarkSteel, {-0.72f, 1.05f, back - 0.04f}, {0.72f, 1.97f, back});
        m.Box(Kind::Cardboard, {-0.68f, 1.09f, back - 0.05f}, {0.68f, 1.93f, back - 0.035f});
        for (int i = 0; i < 6; ++i)
        {
            const float x = -0.58f + static_cast<float>(i % 3) * 0.42f + static_cast<float>((roll >> i) % 3u) * 0.04f;
            const float y = 1.2f + static_cast<float>(i / 3) * 0.36f;
            m.Box(Kind::Bedding, {x, y, back - 0.054f}, {x + 0.21f, y + 0.29f, back - 0.05f});
        }
        break;
    }
    case Fitting::Desk:
    {
        const float top = 0.74f;
        const float front = back - 0.75f;
        m.Box(Kind::Wood, {-w * 0.5f, top, front}, {w * 0.5f, top + 0.04f, back});
        m.Box(Kind::Steel, {-w * 0.5f + 0.04f, 0.0f, front + 0.05f}, {-w * 0.5f + 0.08f, top, back - 0.05f});
        m.Box(Kind::Steel, {w * 0.5f - 0.08f, 0.0f, front + 0.05f}, {w * 0.5f - 0.04f, top, back - 0.05f});
        m.Box(Kind::Wood, {-w * 0.5f + 0.08f, 0.3f, back - 0.08f}, {w * 0.5f - 0.08f, top - 0.001f, back - 0.05f});
        m.Box(Kind::Steel, {w * 0.5f - 0.5f, 0.05f, front + 0.06f}, {w * 0.5f - 0.08f, 0.68f, back - 0.1f});
        m.Monitor({-0.1f, top + 0.04f, back - 0.22f}, 0.58f, 0.34f);
        m.Box(Kind::DarkPlastic, {-0.35f, top + 0.04f, front + 0.12f}, {0.1f, top + 0.06f, front + 0.28f});
        m.Box(Kind::DarkPlastic, {0.2f, top + 0.04f, front + 0.15f}, {0.28f, top + 0.065f, front + 0.27f});
        m.Chair({-0.1f, 0.0f, -back + 0.3f}, 1.0f);
        break;
    }
    case Fitting::Cabinet:
    {
        m.Box(Kind::Steel, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, size.z, back});
        for (int i = 0; i < 4; ++i)
        {
            const float y = 0.08f + static_cast<float>(i) * 0.46f;
            m.Box(Kind::DarkSteel, {-w * 0.5f + 0.04f, y + 0.42f, -back - 0.004f}, {w * 0.5f - 0.04f, y + 0.435f, -back});
            m.Box(Kind::DarkSteel, {-0.12f, y + 0.3f, -back - 0.03f}, {0.12f, y + 0.33f, -back});
        }
        break;
    }
    case Fitting::Console:
    {
        const float front = back - 0.85f;
        m.Box(Kind::DarkPlastic, {-w * 0.5f, 0.0f, front}, {w * 0.5f, 0.72f, back});
        // Its desk, sloping up away from whoever sits at it, and its row of screens on a riser behind.
        m.Add(Form::Box, Kind::Plastic, {0.0f, 0.8f, front + 0.32f}, {w, 0.05f, 0.62f}, glm::angleAxis(glm::radians(-12.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
        m.Box(Kind::DarkPlastic, {-w * 0.5f, 0.72f, back - 0.22f}, {w * 0.5f, 1.15f, back});
        for (int i = 0; i < 3; ++i)
        {
            const float x = -w * 0.5f + 0.1f + static_cast<float>(i) * (w - 0.2f) / 3.0f;
            m.Box(Kind::Screen, {x + 0.02f, 0.8f, back - 0.225f}, {x + (w - 0.2f) / 3.0f - 0.02f, 1.12f, back - 0.22f});
        }
        for (int i = 0; i < 8; ++i)
        {
            const float x = -w * 0.5f + 0.2f + static_cast<float>(i) * 0.18f;
            m.Box((roll >> i) & 1u ? Kind::Screen : Kind::DarkSteel, {x, 0.84f, front + 0.4f}, {x + 0.08f, 0.865f, front + 0.46f});
        }
        m.Chair({0.0f, 0.0f, -back + 0.28f}, 1.0f);
        break;
    }
    case Fitting::Wallscreen:
    {
        m.Box(Kind::DarkPlastic, {-w * 0.5f, 1.0f, back - 0.07f}, {w * 0.5f, 2.3f, back});
        m.Box(Kind::Screen, {-w * 0.5f + 0.05f, 1.05f, back - 0.075f}, {w * 0.5f - 0.05f, 2.25f, back - 0.07f});
        break;
    }
    case Fitting::LabBench:
    {
        const float top = 0.9f;
        m.Box(Kind::Plastic, {-w * 0.5f + 0.02f, 0.0f, -back + 0.06f}, {w * 0.5f - 0.02f, top - 0.04f, back});
        for (int i = 0; i < 3; ++i)
        {
            const float x = -w * 0.5f + 0.06f + static_cast<float>(i) * (w - 0.12f) / 3.0f;
            m.Box(Kind::Steel, {x + 0.02f, 0.1f, -back + 0.055f}, {x + (w - 0.12f) / 3.0f - 0.02f, top - 0.08f, -back + 0.06f});
            m.Box(Kind::DarkSteel, {x + 0.25f, top - 0.18f, -back + 0.03f}, {x + 0.45f, top - 0.16f, -back + 0.055f});
        }
        m.Box(Kind::DarkPlastic, {-w * 0.5f, top - 0.04f, -back}, {w * 0.5f, top, back});
        // What is being worked on: a microscope, a rack of tubes, an instrument with its screen, bottles.
        m.Box(Kind::Plastic, {-0.85f, top, -0.1f}, {-0.55f, top + 0.03f, 0.15f});
        m.Post(Kind::DarkSteel, {-0.7f, top + 0.03f, 0.1f}, 0.06f, 0.35f);
        m.Add(Form::Cylinder, Kind::DarkSteel, {-0.7f, top + 0.33f, 0.0f}, {0.07f, 0.22f, 0.07f}, glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
        m.Box(Kind::Steel, {-0.3f, top, -0.1f}, {0.05f, top + 0.06f, 0.05f});
        for (int i = 0; i < 6; ++i)
        {
            m.Post(Kind::Porcelain, {-0.27f + static_cast<float>(i) * 0.055f, top + 0.06f, -0.025f}, 0.025f, 0.12f);
        }
        m.Box(Kind::Plastic, {0.25f, top, -0.05f}, {0.7f, top + 0.3f, back - 0.05f});
        m.Box(Kind::Screen, {0.3f, top + 0.12f, -0.055f}, {0.5f, top + 0.25f, -0.05f});
        m.Post(Kind::Porcelain, {0.85f, top, -0.1f}, 0.1f, 0.2f);
        m.Post(Kind::Porcelain, {0.97f, top, 0.05f}, 0.08f, 0.15f);
        break;
    }
    case Fitting::FumeHood:
    {
        m.Box(Kind::Plastic, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, 0.9f, back});
        m.Box(Kind::Plastic, {-w * 0.5f, 0.9f, -back + 0.1f}, {-w * 0.5f + 0.08f, 2.3f, back});
        m.Box(Kind::Plastic, {w * 0.5f - 0.08f, 0.9f, -back + 0.1f}, {w * 0.5f, 2.3f, back});
        m.Box(Kind::Plastic, {-w * 0.5f + 0.08f, 1.95f, -back + 0.1f}, {w * 0.5f - 0.08f, 2.3f, back});
        m.Box(Kind::DarkPlastic, {-w * 0.5f + 0.08f, 0.9f, back - 0.04f}, {w * 0.5f - 0.08f, 1.95f, back});
        m.Box(Kind::Steel, {-w * 0.5f + 0.08f, 0.9f, -back + 0.1f}, {w * 0.5f - 0.08f, 0.93f, back - 0.04f});
        // Its sash, half down, framed.
        m.Box(Kind::DarkSteel, {-w * 0.5f + 0.08f, 1.45f, -back + 0.1f}, {w * 0.5f - 0.08f, 1.5f, -back + 0.13f});
        m.Box(Kind::Glazing, {-w * 0.5f + 0.1f, 1.5f, -back + 0.11f}, {w * 0.5f - 0.1f, 1.94f, -back + 0.12f});
        m.Post(Kind::DarkSteel, {0.0f, 2.3f, back - 0.3f}, 0.25f, 0.6f);
        m.Post(Kind::Porcelain, {-0.3f, 0.93f, 0.1f}, 0.1f, 0.18f);
        break;
    }
    case Fitting::Bed:
    case Fitting::Bunk:
    {
        // Head against the wall: a frame, a mattress, a pillow at the head, a blanket over the rest -- twice over, on posts,
        // for a bunk, a ladder up its side.
        const int beds = kind == Fitting::Bunk ? 2 : 1;
        for (int b = 0; b < beds; ++b)
        {
            const float y = b == 0 ? 0.0f : 1.05f;
            m.Box(Kind::Steel, {-w * 0.5f, y + 0.28f, -back}, {w * 0.5f, y + 0.34f, back});
            m.Box(Kind::Bedding, {-w * 0.5f + 0.03f, y + 0.34f, -back + 0.03f}, {w * 0.5f - 0.03f, y + 0.5f, back - 0.03f});
            m.Box(Kind::Bedding, {-w * 0.5f + 0.12f, y + 0.5f, back - 0.45f}, {w * 0.5f - 0.12f, y + 0.6f, back - 0.08f});
            m.Box(Kind::Fabric, {-w * 0.5f + 0.02f, y + 0.48f, -back + 0.02f}, {w * 0.5f - 0.02f, y + 0.53f, back - 0.55f});
        }
        if (kind == Fitting::Bunk)
        {
            for (const float sx : {-1.0f, 1.0f})
            {
                for (const float sz : {-1.0f, 1.0f})
                {
                    m.Box(Kind::Steel, {sx * w * 0.5f - 0.025f, 0.0f, sz * back - 0.025f}, {sx * w * 0.5f + 0.025f, 1.8f, sz * back + 0.025f});
                }
            }
            for (int r = 0; r < 4; ++r)
            {
                m.Box(Kind::Steel, {w * 0.5f, 0.35f + static_cast<float>(r) * 0.28f, -back + 0.2f}, {w * 0.5f + 0.03f, 0.38f + static_cast<float>(r) * 0.28f, -back + 0.6f});
            }
        }
        else
        {
            m.Legs(Kind::Steel, w, d, 0.28f, 0.04f, 0.05f);
            m.Box(Kind::Steel, {-w * 0.5f, 0.28f, back - 0.05f}, {w * 0.5f, 0.95f, back});
        }
        break;
    }
    case Fitting::Table:
    {
        // A table out in the room, a bench either side of it.
        m.Box(Kind::Wood, {-w * 0.5f, 0.72f, -0.4f}, {w * 0.5f, 0.76f, 0.4f});
        m.Legs(Kind::Steel, w, 0.8f, 0.72f, 0.08f, 0.05f);
        for (const float s : {-1.0f, 1.0f})
        {
            m.Box(Kind::Wood, {-w * 0.5f + 0.05f, 0.43f, s * 0.85f - 0.16f}, {w * 0.5f - 0.05f, 0.47f, s * 0.85f + 0.16f});
            m.Box(Kind::Steel, {-w * 0.5f + 0.15f, 0.0f, s * 0.85f - 0.03f}, {-w * 0.5f + 0.2f, 0.43f, s * 0.85f + 0.03f});
            m.Box(Kind::Steel, {w * 0.5f - 0.2f, 0.0f, s * 0.85f - 0.03f}, {w * 0.5f - 0.15f, 0.43f, s * 0.85f + 0.03f});
        }
        if (roll % 3u == 0u)
        {
            m.Box(Kind::DarkPlastic, {-0.4f, 0.76f, -0.2f}, {-0.1f, 0.78f, 0.05f});
            m.Post(Kind::Porcelain, {0.3f, 0.76f, 0.1f}, 0.08f, 0.1f);
        }
        break;
    }
    case Fitting::Counter:
    case Fitting::Cooker:
    {
        m.Box(Kind::Plastic, {-w * 0.5f, 0.08f, -back + 0.04f}, {w * 0.5f, 0.88f, back});
        m.Box(Kind::DarkPlastic, {-w * 0.5f + 0.04f, 0.0f, -back + 0.1f}, {w * 0.5f - 0.04f, 0.08f, back});
        if (kind == Fitting::Cooker)
        {
            m.Box(Kind::DarkSteel, {-w * 0.5f, 0.88f, -back}, {w * 0.5f, 0.92f, back});
            for (int i = 0; i < 4; ++i)
            {
                m.Post(Kind::Steel, {(i % 2 == 0 ? -0.2f : 0.2f), 0.92f, (i < 2 ? -0.13f : 0.13f)}, 0.2f, 0.012f);
            }
            m.Box(Kind::Glazing, {-w * 0.5f + 0.1f, 0.2f, -back + 0.035f}, {w * 0.5f - 0.1f, 0.6f, -back + 0.04f});
            m.Box(Kind::Steel, {-w * 0.5f + 0.12f, 0.66f, -back + 0.0f}, {w * 0.5f - 0.12f, 0.68f, -back + 0.04f});
        }
        else
        {
            m.Box(Kind::Porcelain, {-w * 0.5f, 0.88f, -back}, {w * 0.5f, 0.92f, back});
            // Its cupboard doors, and now and then a sink set in it.
            for (int i = 0; i < 4; ++i)
            {
                const float x = -w * 0.5f + static_cast<float>(i) * w / 4.0f;
                m.Box(Kind::DarkSteel, {x + 0.02f, 0.78f, -back + 0.035f}, {x + 0.12f, 0.8f, -back + 0.04f});
                m.Box(Kind::DarkPlastic, {x + w / 4.0f - 0.006f, 0.1f, -back + 0.035f}, {x + w / 4.0f + 0.006f, 0.86f, -back + 0.04f});
            }
            if (roll % 2u == 0u)
            {
                m.Box(Kind::Steel, {-0.35f, 0.885f, -0.2f}, {0.15f, 0.925f, 0.2f});
                m.Box(Kind::DarkSteel, {-0.32f, 0.86f, -0.17f}, {0.12f, 0.93f, 0.17f});
                m.Post(Kind::Steel, {-0.1f, 0.92f, 0.24f}, 0.04f, 0.25f);
            }
        }
        break;
    }
    case Fitting::Fridge:
    {
        m.Box(Kind::Plastic, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, size.z, back});
        m.Box(Kind::DarkPlastic, {-w * 0.5f + 0.01f, 1.25f, -back - 0.004f}, {w * 0.5f - 0.01f, 1.265f, -back});
        m.Box(Kind::Steel, {w * 0.5f - 0.12f, 0.8f, -back - 0.05f}, {w * 0.5f - 0.09f, 1.15f, -back});
        m.Box(Kind::Steel, {w * 0.5f - 0.12f, 1.35f, -back - 0.05f}, {w * 0.5f - 0.09f, 1.7f, -back});
        break;
    }
    case Fitting::Basin:
    {
        m.Box(Kind::Porcelain, {-0.3f, 0.78f, -back + 0.02f}, {0.3f, 0.9f, back});
        m.Box(Kind::DarkSteel, {-0.22f, 0.86f, -back + 0.08f}, {0.22f, 0.905f, back - 0.08f});
        m.Post(Kind::Porcelain, {0.0f, 0.0f, back - 0.18f}, 0.16f, 0.78f);
        m.Post(Kind::Steel, {0.0f, 0.9f, back - 0.06f}, 0.03f, 0.18f);
        m.Box(Kind::Steel, {-0.015f, 1.06f, back - 0.18f}, {0.015f, 1.08f, back - 0.05f});
        // The mirror over it.
        m.Box(Kind::DarkSteel, {-0.3f, 1.2f, back - 0.025f}, {0.3f, 1.95f, back});
        m.Box(Kind::Mirror, {-0.28f, 1.22f, back - 0.03f}, {0.28f, 1.93f, back - 0.025f});
        break;
    }
    case Fitting::Toilet:
    {
        // In its stall: a panel either side, its door, the pan at the back.
        for (const float s : {-1.0f, 1.0f})
        {
            m.Box(Kind::Plastic, {s * w * 0.5f - 0.02f, 0.15f, -back}, {s * w * 0.5f + 0.02f, 2.0f, back});
            m.Box(Kind::Steel, {s * w * 0.5f - 0.02f, 0.0f, -back + 0.05f}, {s * w * 0.5f + 0.02f, 0.15f, -back + 0.1f});
        }
        m.Box(Kind::Plastic, {-w * 0.5f + 0.02f, 0.2f, -back}, {w * 0.5f - 0.02f, 1.95f, -back + 0.03f});
        m.Box(Kind::DarkSteel, {w * 0.5f - 0.2f, 1.0f, -back - 0.03f}, {w * 0.5f - 0.1f, 1.04f, -back});
        m.Post(Kind::Porcelain, {0.0f, 0.0f, back - 0.45f}, 0.38f, 0.42f);
        m.Box(Kind::Porcelain, {-0.2f, 0.4f, back - 0.2f}, {0.2f, 0.85f, back});
        break;
    }
    case Fitting::Shower:
    {
        m.Box(Kind::Porcelain, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, 0.08f, back});
        m.Box(Kind::DarkSteel, {-0.05f, 0.075f, -0.05f}, {0.05f, 0.085f, 0.05f});
        for (const float s : {-1.0f, 1.0f})
        {
            m.Box(Kind::Plastic, {s * w * 0.5f - 0.02f, 0.08f, -back + 0.02f}, {s * w * 0.5f + 0.02f, 2.05f, back});
        }
        m.Post(Kind::Steel, {0.0f, 1.0f, back - 0.05f}, 0.03f, 1.0f);
        m.Add(Form::Cylinder, Kind::Steel, {0.0f, 2.0f, back - 0.18f}, {0.18f, 0.03f, 0.18f});
        m.Rod(Kind::Steel, {0.0f, 2.02f, -back + 0.02f}, 0.025f, w, true);
        m.Box(Kind::Bedding, {-w * 0.5f + 0.02f, 0.4f, -back + 0.02f}, {-w * 0.5f + 0.3f, 2.0f, -back + 0.04f});
        break;
    }
    case Fitting::MedBed:
    {
        // On a frame on wheels, its back raised, a drip on a stand beside it.
        m.Box(Kind::Steel, {-w * 0.5f, 0.45f, -back}, {w * 0.5f, 0.52f, back});
        m.Box(Kind::Bedding, {-w * 0.5f + 0.03f, 0.52f, -back + 0.03f}, {w * 0.5f - 0.03f, 0.64f, back - 0.75f});
        m.Add(Form::Box, Kind::Bedding, {0.0f, 0.82f, back - 0.4f}, {w - 0.06f, 0.12f, 0.75f}, glm::angleAxis(glm::radians(35.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
        for (const float sx : {-1.0f, 1.0f})
        {
            for (const float sz : {-1.0f, 1.0f})
            {
                m.Box(Kind::Steel, {sx * (w * 0.5f - 0.06f) - 0.025f, 0.1f, sz * (back - 0.1f) - 0.025f}, {sx * (w * 0.5f - 0.06f) + 0.025f, 0.45f, sz * (back - 0.1f) + 0.025f});
                m.Add(Form::Cylinder, Kind::DarkPlastic, {sx * (w * 0.5f - 0.06f), 0.05f, sz * (back - 0.1f)}, {0.1f, 0.04f, 0.1f},
                      glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)));
            }
            m.Box(Kind::Steel, {sx * w * 0.5f - 0.015f, 0.52f, -back + 0.3f}, {sx * w * 0.5f + 0.015f, 0.8f, -back + 0.9f});
        }
        m.Post(Kind::Steel, {w * 0.5f + 0.15f, 0.0f, back - 0.3f}, 0.03f, 1.9f);
        m.Box(Kind::Bedding, {w * 0.5f + 0.1f, 1.6f, back - 0.33f}, {w * 0.5f + 0.2f, 1.82f, back - 0.27f});
        break;
    }
    case Fitting::Shelving:
    {
        for (const float sx : {-1.0f, 1.0f})
        {
            for (const float sz : {-1.0f, 1.0f})
            {
                m.Box(Kind::Steel, {sx * (w * 0.5f - 0.025f) - 0.025f, 0.0f, sz * (back - 0.025f) - 0.025f}, {sx * (w * 0.5f - 0.025f) + 0.025f, size.z, sz * (back - 0.025f) + 0.025f});
            }
        }
        for (int i = 0; i < 4; ++i)
        {
            const float y = 0.12f + static_cast<float>(i) * 0.65f;
            m.Box(Kind::Steel, {-w * 0.5f + 0.05f, y, -back + 0.01f}, {w * 0.5f - 0.05f, y + 0.03f, back - 0.01f});
            m.Boxes(-w * 0.5f + 0.05f, w * 0.5f - 0.05f, y + 0.03f, d, roll + static_cast<uint32_t>(i) * 97u);
        }
        break;
    }
    case Fitting::Pallet:
    {
        m.Box(Kind::Wood, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, 0.12f, back});
        m.Box(Kind::Wood, {-w * 0.5f, 0.12f, -back}, {w * 0.5f, 0.15f, back});
        const int layers = 1 + static_cast<int>(roll % 2u);
        for (int l = 0; l < layers; ++l)
        {
            const float y = 0.15f + static_cast<float>(l) * 0.46f;
            for (int i = 0; i < 4; ++i)
            {
                const float x0 = -w * 0.5f + 0.03f + static_cast<float>(i % 2) * (w * 0.5f - 0.02f);
                const float z0 = -back + 0.03f + static_cast<float>(i / 2) * (back - 0.02f);
                if ((roll >> (l * 4 + i)) % 7u != 0u)
                {
                    m.Box(Kind::Cardboard, {x0, y, z0}, {x0 + w * 0.5f - 0.05f, y + 0.44f, z0 + back - 0.05f});
                }
            }
        }
        break;
    }
    case Fitting::Workbench:
    {
        const float top = 0.9f;
        m.Box(Kind::Wood, {-w * 0.5f, top - 0.06f, -back}, {w * 0.5f, top, back});
        m.Legs(Kind::DarkSteel, w, d, top - 0.06f, 0.05f, 0.06f);
        m.Box(Kind::Steel, {-w * 0.5f + 0.06f, 0.15f, -back + 0.06f}, {w * 0.5f - 0.06f, 0.18f, back - 0.06f});
        m.Box(Kind::Cardboard, {-w * 0.5f + 0.15f, 0.18f, -back + 0.1f}, {-w * 0.5f + 0.6f, 0.45f, back - 0.1f});
        // A vice at its front corner, and over it a board of tools.
        m.Box(Kind::Paint, {w * 0.5f - 0.35f, top, -back + 0.02f}, {w * 0.5f - 0.1f, top + 0.14f, -back + 0.22f});
        m.Box(Kind::DarkSteel, {w * 0.5f - 0.33f, top + 0.14f, -back}, {w * 0.5f - 0.12f, top + 0.2f, -back + 0.06f});
        m.Box(Kind::Cardboard, {-w * 0.5f + 0.1f, 1.2f, back - 0.03f}, {w * 0.5f - 0.1f, 1.95f, back});
        for (int i = 0; i < 9; ++i)
        {
            const float x = -w * 0.5f + 0.25f + static_cast<float>(i) * 0.18f;
            const float h = 0.15f + static_cast<float>((roll >> i) % 3u) * 0.08f;
            m.Box(i % 3 == 0 ? Kind::Paint : Kind::DarkSteel, {x, 1.5f - h, back - 0.06f}, {x + 0.04f, 1.6f, back - 0.03f});
        }
        break;
    }
    case Fitting::Drum:
    {
        m.Post(Kind::Paint, {0.0f, 0.0f, 0.0f}, 0.58f, 0.88f);
        for (const float y : {0.02f, 0.3f, 0.58f, 0.86f})
        {
            m.Post(Kind::DarkSteel, {0.0f, y, 0.0f}, 0.6f, 0.025f);
        }
        m.Post(Kind::DarkSteel, {0.12f, 0.88f, 0.08f}, 0.06f, 0.02f);
        break;
    }
    case Fitting::Generator:
    {
        // On its skid: the engine, its radiator at one end and the alternator at the other, an exhaust up out of it, its panel.
        m.Box(Kind::DarkSteel, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, 0.14f, back});
        m.Box(Kind::Paint, {-0.6f, 0.14f, -0.42f}, {0.4f, 1.0f, 0.42f});
        m.Box(Kind::DarkSteel, {-0.95f, 0.14f, -0.5f}, {-0.6f, 1.2f, 0.5f});
        m.Box(Kind::Steel, {-0.98f, 0.24f, -0.42f}, {-0.95f, 1.1f, 0.42f});
        m.Rod(Kind::Paint, {0.7f, 0.58f, 0.0f}, 0.62f, 0.6f, true);
        m.Post(Kind::DarkSteel, {-0.2f, 1.0f, 0.2f}, 0.14f, 0.6f);
        m.Box(Kind::Plastic, {0.45f, 0.14f, -back}, {0.95f, 0.6f, -back + 0.12f});
        m.Box(Kind::Screen, {0.55f, 0.35f, -back - 0.005f}, {0.85f, 0.5f, -back});
        for (int i = 0; i < 4; ++i)
        {
            m.Box(Kind::DarkSteel, {-0.55f + static_cast<float>(i) * 0.25f, 1.0f, -0.35f}, {-0.45f + static_cast<float>(i) * 0.25f, 1.06f, 0.35f});
        }
        break;
    }
    case Fitting::Pump:
    {
        m.Box(Kind::DarkSteel, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, 0.1f, back});
        m.Rod(Kind::Paint, {-0.2f, 0.42f, 0.0f}, 0.5f, 0.7f, true);
        m.Rod(Kind::DarkSteel, {0.2f, 0.42f, 0.0f}, 0.2f, 0.2f, true);
        m.Add(Form::Cylinder, Kind::Paint, {0.4f, 0.42f, 0.0f}, {0.56f, 0.28f, 0.56f}, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)));
        m.Post(Kind::Steel, {0.4f, 0.65f, 0.0f}, 0.16f, 0.35f);
        m.Rod(Kind::Steel, {0.4f, 0.3f, -0.25f}, 0.16f, 0.3f, false);
        break;
    }
    case Fitting::Rack:
    {
        // A server rack: its cabinet, its door's mesh dark, the lights of what is in it showing in rows.
        m.Box(Kind::DarkPlastic, {-w * 0.5f, 0.0f, -back}, {w * 0.5f, size.z, back});
        for (int i = 0; i < 18; ++i)
        {
            const float y = 0.15f + static_cast<float>(i) * 0.1f;
            for (const float face : {-1.0f, 1.0f})
            {
                const float z = face * back;
                m.Box(Kind::DarkSteel, {-w * 0.5f + 0.05f, y, z - 0.006f}, {w * 0.5f - 0.05f, y + 0.085f, z + 0.006f});
                if ((roll >> (i % 16)) & 1u)
                {
                    const float x = -w * 0.5f + 0.1f + static_cast<float>((roll >> i) % 4u) * 0.08f;
                    m.Box(Kind::Screen, {x, y + 0.03f, z - 0.01f}, {x + 0.05f, y + 0.05f, z + 0.01f});
                }
            }
        }
        break;
    }
    case Fitting::Terminal:
    {
        // A plain bench: a steel cabinet under a top, nothing on it -- what goes there is the mission's terminal.
        const float top = size.z;
        m.Box(Kind::Steel, {-w * 0.5f + 0.03f, 0.0f, -back + 0.05f}, {w * 0.5f - 0.03f, top - 0.04f, back});
        m.Box(Kind::DarkPlastic, {-w * 0.5f, top - 0.04f, -back}, {w * 0.5f, top, back});
        for (const float x : {-0.4f, 0.4f})
        {
            m.Box(Kind::DarkSteel, {x - 0.12f, top - 0.15f, -back + 0.03f}, {x + 0.12f, top - 0.13f, -back + 0.05f});
        }
        break;
    }
    case Fitting::Mirror:
    {
        // At face height, framed.
        m.Box(Kind::DarkSteel, {-w * 0.5f, 1.15f, back - 0.03f}, {w * 0.5f, 1.85f, back});
        m.Box(Kind::Mirror, {-w * 0.5f + 0.03f, 1.18f, back - 0.035f}, {w * 0.5f - 0.03f, 1.82f, back - 0.03f});
        break;
    }
    case Fitting::Racking:
    {
        // A bay of pallet racking: its frames at each end, beams at three levels, loaded pallets on two.
        for (const float sx : {-1.0f, 1.0f})
        {
            for (const float sz : {-1.0f, 1.0f})
            {
                m.Box(Kind::Paint, {sx * (w * 0.5f - 0.04f) - 0.04f, 0.0f, sz * (back - 0.04f) - 0.04f}, {sx * (w * 0.5f - 0.04f) + 0.04f, size.z, sz * (back - 0.04f) + 0.04f});
            }
            for (int k = 0; k < 4; ++k)
            {
                m.Box(Kind::Steel, {sx * (w * 0.5f - 0.04f) - 0.02f, 0.3f + static_cast<float>(k) * 0.6f, -back + 0.04f}, {sx * (w * 0.5f - 0.04f) + 0.02f, 0.33f + static_cast<float>(k) * 0.6f, back - 0.04f});
            }
        }
        for (const float y : {0.1f, 1.25f, 2.4f})
        {
            for (const float sz : {-1.0f, 1.0f})
            {
                m.Box(Kind::Paint, {-w * 0.5f + 0.08f, y, sz * (back - 0.04f) - 0.035f}, {w * 0.5f - 0.08f, y + 0.1f, sz * (back - 0.04f) + 0.035f});
            }
        }
        for (const float y : {0.2f, 1.35f})
        {
            for (int p = 0; p < 2; ++p)
            {
                if ((roll >> p) % 4u == 0u)
                {
                    continue;
                }
                const float x = -w * 0.5f + 0.15f + static_cast<float>(p) * (w * 0.5f - 0.05f);
                m.Box(Kind::Wood, {x, y, -back + 0.1f}, {x + w * 0.5f - 0.2f, y + 0.12f, back - 0.1f});
                m.Box(Kind::Cardboard, {x + 0.04f, y + 0.12f, -back + 0.14f}, {x + w * 0.5f - 0.24f, y + 0.95f, back - 0.14f});
            }
        }
        break;
    }
    }
}

uint32_t Mix(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

} // namespace

// What a fitting is felt as, in its own frame (its foot's middle at the origin, its front -z): the boxes round what is there,
// not round the floor about it -- a desk and its chair apart, with the floor between them to walk on; a table and its benches;
// a reception counter, and its chair behind it, not the room behind it. Anything else the box it fills. None of them touching.
std::vector<std::pair<glm::vec3, glm::vec3>> Solids(Fitting kind)
{
    const glm::vec3 size = FacilityLayout::FittingSize(kind);
    const float w = size.x;
    const float back = size.y * 0.5f;
    switch (kind)
    {
    case Fitting::Desk:
        return {{{-w * 0.5f, 0.0f, back - 0.75f}, {w * 0.5f, 0.78f, back}}, {{-0.37f, 0.0f, -back + 0.02f}, {0.17f, 1.0f, back - 0.8f}}};
    case Fitting::Console:
        return {{{-w * 0.5f, 0.0f, back - 0.85f}, {w * 0.5f, 1.15f, back}}, {{-0.27f, 0.0f, -back + 0.0f}, {0.27f, 1.0f, back - 0.9f}}};
    case Fitting::Table:
        return {{{-w * 0.5f, 0.0f, -0.4f}, {w * 0.5f, 0.78f, 0.4f}},
                {{-w * 0.5f + 0.05f, 0.0f, -1.01f}, {w * 0.5f - 0.05f, 0.47f, -0.69f}},
                {{-w * 0.5f + 0.05f, 0.0f, 0.69f}, {w * 0.5f - 0.05f, 0.47f, 1.01f}}};
    case Fitting::Reception:
        return {{{-w * 0.5f - 0.03f, 0.0f, -back - 0.05f}, {w * 0.5f + 0.03f, 1.08f, -back + 0.85f}}, {{0.08f, 0.0f, -back + 0.97f}, {0.62f, 1.0f, -back + 1.53f}}};
    default:
        return {{{-w * 0.5f, 0.0f, -back}, {w * 0.5f, size.z, back}}};
    }
}

void Furnish(const FacilityLayout& layout, FacilityMap::Blueprint& out)
{
    for (size_t i = 0; i < layout.fittings.size(); ++i)
    {
        const FacilityLayout::Fitted& fitted = layout.fittings[i];
        const glm::vec3 size = FacilityLayout::FittingSize(fitted.kind);
        Maker maker{out};
        // Against a wall, its back a hair off it (as what the plan puts against a wall stands); out in a room, on its cell.
        if (fitted.wall)
        {
            FacilityLayout::Placed placed;
            placed.floor = fitted.floor;
            placed.at = fitted.at;
            placed.yaw = fitted.yaw;
            maker.foot = FacilityMap::PlacedAgainstWall(layout, placed, size.y);
        }
        else
        {
            maker.foot = FacilityMap::ToWorld(layout, fitted.floor, fitted.at);
        }
        maker.turn = glm::angleAxis(fitted.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        Build(maker, fitted.kind, Mix(layout.seed, static_cast<uint32_t>(i)));
        // A mirror in a building nobody looks after, cracked from where something hit it -- and in a burnt one, sooted over.
        if ((fitted.kind == Fitting::Basin || fitted.kind == Fitting::Mirror) && layout.condition != FacilityLayout::Condition::Kept)
        {
            const bool basin = fitted.kind == Fitting::Basin;
            const float front = basin ? size.y * 0.5f - 0.031f : size.y * 0.5f - 0.036f;
            const glm::vec2 lo = basin ? glm::vec2(-0.28f, 1.22f) : glm::vec2(-0.22f, 1.18f);
            const glm::vec2 hi = basin ? glm::vec2(0.28f, 1.93f) : glm::vec2(0.22f, 1.82f);
            uint32_t roll = Mix(layout.seed, static_cast<uint32_t>(i) ^ 0xC2ACu);
            const auto unit = [&roll]
            {
                roll = Mix(roll, 0x9E37u);
                return static_cast<float>(roll & 0xFFFFu) / 65535.0f;
            };
            if (unit() < 0.7f)
            {
                const glm::vec2 hit = glm::mix(lo, hi, glm::vec2(0.3f + 0.4f * unit(), 0.3f + 0.4f * unit()));
                for (int c = 3 + static_cast<int>(unit() * 4.0f); c > 0; --c)
                {
                    const float a = unit() * glm::two_pi<float>();
                    const glm::vec2 way{std::cos(a), std::sin(a)};
                    // As far as it goes before the frame.
                    float reach = 0.4f;
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        if (std::abs(way[axis]) > 1e-3f)
                        {
                            reach = std::min(reach, ((way[axis] > 0.0f ? hi[axis] : lo[axis]) - hit[axis]) / way[axis]);
                        }
                    }
                    reach *= 0.4f + 0.6f * unit();
                    const glm::vec2 middle = hit + way * (reach * 0.5f);
                    maker.Add(Form::Box, Kind::Paper, {middle.x, middle.y, front}, {reach, 0.004f, 0.001f}, glm::angleAxis(a, glm::vec3(0.0f, 0.0f, 1.0f)));
                }
            }
            if (layout.condition == FacilityLayout::Condition::Burnt)
            {
                maker.Add(Form::Box, Kind::Soot, {(lo.x + hi.x) * 0.5f, hi.y - (hi.y - lo.y) * 0.3f, front - 0.001f}, {hi.x - lo.x, (hi.y - lo.y) * 0.6f, 0.001f});
            }
        }
        // Felt as what it is made of (Solids), but what hangs on a wall, thin, which is not in anybody's way.
        if (size.y > 0.15f)
        {
            for (const auto& [lo, hi] : Solids(fitted.kind))
            {
                FacilityMap::Piece solid;
                solid.kind = Kind::Furniture;
                solid.centre = maker.foot + maker.turn * ((lo + hi) * 0.5f);
                solid.size = hi - lo;
                solid.yaw = fitted.yaw;
                out.pieces.push_back(solid);
            }
        }
    }
}

} // namespace FacilityFurniture

} // namespace pred
