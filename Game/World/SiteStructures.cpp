#include "Game/World/SiteStructures.h"

#include "Engine/Render/Primitives.h"
#include "Game/World/FacilityOutside.h"
#include "Game/World/MapBuilder.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pred
{
namespace SiteStructures
{

namespace
{

using Kind = SitePlan::BlockKind;

const char* NameOf(Kind kind)
{
    switch (kind)
    {
    case Kind::Headframe: return "site_headframe";
    case Kind::SpoilHeap: return "site_spoil";
    case Kind::PipeRack: return "site_pipe_rack";
    case Kind::FlareStack: return "site_flare_stack";
    case Kind::Bund: return "site_bund";
    case Kind::Gantry: return "site_gantry";
    case Kind::SolarArray: return "site_solar";
    case Kind::Dish: return "site_dish";
    case Kind::Fence: return "site_fence";
    case Kind::WeatherStation: return "site_weather_station";
    default: return "site_structure";
    }
}

// Drawing in the structure's own frame: x across, y up from its foot, z deep, turned by its yaw.
struct Drawer
{
    MapBuilder& builder;
    const std::string name;
    glm::vec3 foot;
    glm::quat turn;

    glm::vec3 World(const glm::vec3& local) const { return foot + turn * local; }

    // A box at `centre`, `size`, turned a further `local` -- felt, or only drawn.
    void Box(const glm::vec3& centre, const glm::vec3& size, const Material& material, bool solid,
             const glm::quat& local = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        Transform where;
        where.position = World(centre);
        where.rotation = turn * local;
        if (solid && local == glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
        {
            builder.AddBox(name, where, size, material);
            return;
        }
        builder.AddMesh(name, where, Primitives::Box(size), material, solid);
    }
    // A round bar from one point to another (in its own frame), `radius` thick.
    void Rod(const glm::vec3& from, const glm::vec3& to, float radius, const Material& material, bool solid, int sides = 8)
    {
        const glm::vec3 a = World(from);
        const glm::vec3 b = World(to);
        const glm::vec3 run = b - a;
        if (glm::length(run) < 1.0e-3f)
        {
            return;
        }
        Transform where;
        where.position = (a + b) * 0.5f;
        where.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(run));
        builder.AddMesh(name, where, Primitives::Cylinder(radius, glm::length(run), sides), material, solid);
    }
    // A square bar from one point to another, `thick` across.
    void Beam(const glm::vec3& from, const glm::vec3& to, float thick, const Material& material, bool solid)
    {
        const glm::vec3 a = World(from);
        const glm::vec3 b = World(to);
        const glm::vec3 run = b - a;
        if (glm::length(run) < 1.0e-3f)
        {
            return;
        }
        Transform where;
        where.position = (a + b) * 0.5f;
        where.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(run));
        builder.AddMesh(name, where, Primitives::Box({thick, glm::length(run), thick}), material, solid);
    }
    // Any mesh, at `centre` turned a further `local`.
    void Mesh(const MeshData& mesh, const glm::vec3& centre, const glm::quat& local, const Material& material, bool solid)
    {
        Transform where;
        where.position = World(centre);
        where.rotation = turn * local;
        builder.AddMesh(name, where, mesh, material, solid);
    }
};

// A mine's headframe over its shaft: four lattice legs narrowing as they rise to a deck at its top, braced across and up each
// face; two wheels on the deck the cables run over, back and down to the winder house behind, its backstays leaning to it; a
// concrete collar round the shaft, dark down the middle.
void Headframe(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float high = size.y;
    const float base = std::min(size.x, size.z) * 0.5f;
    const float top = base * 0.45f;
    const auto leg = [&](int k, float y)
    {
        const float sx = (k & 1) ? 1.0f : -1.0f;
        const float sz = (k & 2) ? 1.0f : -1.0f;
        const float r = glm::mix(base, top, y / high);
        return glm::vec3(sx * r, y, sz * r);
    };
    // The collar and the shaft.
    d.Box({0.0f, 0.15f, 0.0f}, {base * 1.4f, 0.3f, base * 1.4f}, m.concrete, true);
    d.Box({0.0f, 0.302f, 0.0f}, {base * 0.9f, 0.004f, base * 0.9f}, m.dark, false);
    const int panels = std::max(4, static_cast<int>(high / 3.0f));
    for (int k = 0; k < 4; ++k)
    {
        const int next = k == 0 ? 1 : k == 1 ? 3 : k == 3 ? 2 : 0; // round the four corners
        for (int p = 0; p < panels; ++p)
        {
            const float y0 = 0.3f + (high - 0.3f) * static_cast<float>(p) / static_cast<float>(panels);
            const float y1 = 0.3f + (high - 0.3f) * static_cast<float>(p + 1) / static_cast<float>(panels);
            d.Beam(leg(k, y0), leg(k, y1), 0.3f, m.steel, true);
            d.Beam(leg(k, y1), leg(next, y1), 0.14f, m.steel, false);
            d.Beam(p % 2 == 0 ? leg(k, y0) : leg(next, y0), p % 2 == 0 ? leg(next, y1) : leg(k, y1), 0.08f, m.steel, false);
        }
        d.Box(leg(k, 0.3f) + glm::vec3(0.0f, -0.13f, 0.0f), {0.8f, 0.34f, 0.8f}, m.concrete, true); // (proud of the collar it meets)
    }
    // The deck at its top, a rail round it, and the wheels on it.
    d.Box({0.0f, high + 0.1f, 0.0f}, {top * 2.4f, 0.2f, top * 2.6f}, m.steel, true);
    for (const float s : {-1.0f, 1.0f})
    {
        d.Beam({-top * 1.2f, high + 1.1f, s * top * 1.3f}, {top * 1.2f, high + 1.1f, s * top * 1.3f}, 0.05f, m.steel, false);
        d.Beam({s * top * 1.2f, high + 1.1f, -top * 1.3f}, {s * top * 1.2f, high + 1.1f, top * 1.3f}, 0.05f, m.steel, false);
    }
    const float wheel = std::max(1.2f, top * 0.8f);
    const glm::quat edgeOn = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)); // its axle along x
    // The two side by side on the one axle (each in its own plane: one behind the other in the same one, they fought for it),
    // the axle in bearings on three posts off the deck -- between them and either side.
    const glm::vec3 axle{0.0f, high + 0.2f + wheel, 0.0f};
    constexpr float kApart = 0.45f;
    for (const float x : {-kApart, kApart})
    {
        const glm::vec3 hub = axle + glm::vec3(x, 0.0f, 0.0f);
        d.Mesh(Primitives::Cylinder(wheel, 0.18f, 28), hub, edgeOn, m.hazard, false);
        d.Mesh(Primitives::Cylinder(wheel * 0.8f, 0.2f, 28), hub, edgeOn, m.dark, false);
        for (int s = 0; s < 6; ++s)
        {
            const float a = glm::two_pi<float>() * static_cast<float>(s) / 6.0f;
            d.Beam(hub, hub + glm::vec3(0.0f, std::cos(a) * wheel * 0.85f, std::sin(a) * wheel * 0.85f), 0.07f, m.hazard, false);
        }
    }
    d.Mesh(Primitives::Cylinder(0.12f, 2.0f * kApart + 0.9f, 10), axle, edgeOn, m.steel, false);
    for (const float x : {-kApart - 0.4f, 0.0f, kApart + 0.4f})
    {
        d.Beam({x, high + 0.2f, 0.0f}, axle + glm::vec3(x, 0.0f, 0.0f), 0.18f, m.steel, false);
    }
    // The winder house behind it, and the backstays leaning from the deck down towards it; the cables from the wheels into it.
    const float back = high * 0.75f + 4.0f;
    const glm::vec3 house{0.0f, 0.0f, -back};
    d.Box(house + glm::vec3(0.0f, 2.6f, 0.0f), {7.0f, 5.2f, 6.0f}, m.paint, true);
    d.Box(house + glm::vec3(0.0f, 5.3f, 0.0f), {7.4f, 0.2f, 6.4f}, m.steel, true);
    d.Box(house + glm::vec3(0.0f, 1.1f, 3.02f), {1.3f, 2.2f, 0.06f}, m.dark, false);
    for (const float x : {-2.0f, 2.0f})
    {
        d.Box(house + glm::vec3(x, 3.4f, 3.02f), {1.2f, 0.9f, 0.05f}, m.dark, false);
    }
    for (const float s : {-1.0f, 1.0f})
    {
        const glm::vec3 from{s * top, high, -top};
        const glm::vec3 to{s * base * 1.2f, 0.3f, -high * 0.55f};
        d.Beam(from, to, 0.3f, m.steel, true);
        d.Box(to + glm::vec3(0.0f, -0.15f, 0.0f), {0.9f, 0.3f, 0.9f}, m.concrete, true);
        // (Each wheel's cable leaving it where it runs off its rim towards the house, as a rope off a pulley.)
        const glm::vec3 hub = axle + glm::vec3(s * kApart, 0.0f, 0.0f);
        const glm::vec3 winder = house + glm::vec3(s * kApart, 4.6f, 3.0f);
        const glm::vec3 run = glm::normalize(winder - hub);
        d.Rod(hub + glm::normalize(glm::vec3(0.0f, -run.z, run.y)) * wheel, winder, 0.025f, m.dark, false, 6);
    }
}

// Broken rock heaped where it was tipped: one great heap and two smaller against it, in the world's own rock.
void SpoilHeap(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float r = size.x * 0.5f;
    d.Mesh(Primitives::Frustum(r, r * 0.18f, size.y, 14), {0.0f, size.y * 0.5f - 0.2f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m.rock, true);
    d.Mesh(Primitives::Frustum(r * 0.6f, r * 0.1f, size.y * 0.6f, 12), {r * 0.75f, size.y * 0.3f - 0.2f, r * 0.3f},
           glm::angleAxis(0.7f, glm::vec3(0.0f, 1.0f, 0.0f)), m.rock, true);
    d.Mesh(Primitives::Frustum(r * 0.5f, r * 0.12f, size.y * 0.45f, 12), {-r * 0.6f, size.y * 0.22f - 0.2f, -r * 0.55f},
           glm::angleAxis(1.9f, glm::vec3(0.0f, 1.0f, 0.0f)), m.rock, true);
}

// A pipe rack along x: bents every six metres -- two posts, a beam across at two heights -- tied along the top; pipes on both levels,
// each its own size, lying in their saddles.
void PipeRack(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float half = size.x * 0.5f;
    const float wide = size.z * 0.5f;
    const float lower = size.y * 0.55f;
    const float upper = size.y;
    const int bents = std::max(2, static_cast<int>(size.x / 6.0f) + 1);
    for (int i = 0; i < bents; ++i)
    {
        const float x = -half + size.x * static_cast<float>(i) / static_cast<float>(bents - 1);
        for (const float z : {-wide, wide})
        {
            d.Box({x, upper * 0.5f, z}, {0.3f, upper, 0.3f}, m.steel, true);
            d.Box({x, 0.15f, z}, {0.7f, 0.3f, 0.7f}, m.concrete, true);
        }
        for (const float y : {lower, upper})
        {
            d.Box({x, y, 0.0f}, {0.25f, 0.3f, size.z + 0.3f}, m.steel, false);
        }
    }
    for (const float z : {-wide, wide})
    {
        d.Box({0.0f, upper + 0.2f, z}, {size.x, 0.22f, 0.22f}, m.steel, false);
    }
    const float radii[] = {0.32f, 0.18f, 0.25f, 0.12f, 0.22f, 0.15f, 0.28f};
    const glm::quat alongX = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    for (int level = 0; level < 2; ++level)
    {
        const float y = level == 0 ? lower + 0.15f : upper + 0.15f;
        float z = -wide + 0.35f;
        for (int p = 0; p < 4 && z < wide - 0.2f; ++p)
        {
            const float r = radii[(level * 3 + p) % 7];
            d.Mesh(Primitives::Cylinder(r, size.x + 1.0f, 14), {0.0f, y + r, z + r}, alongX, level == 0 ? m.pipe : m.paint, false);
            z += r * 2.0f + 0.3f;
        }
    }
}

// A refinery's flare stack: a tall steel pipe on a concrete base, a platform and its rail near the top, guyed three ways; at its top
// the flame, burning off what is not wanted.
void FlareStack(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float high = size.y;
    const float r = size.x * 0.5f;
    d.Box({0.0f, 0.4f, 0.0f}, {r * 5.0f, 0.8f, r * 5.0f}, m.concrete, true);
    d.Mesh(Primitives::Cylinder(r, high - 0.8f, 16), {0.0f, 0.8f + (high - 0.8f) * 0.5f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m.paint, true);
    for (float y = 4.0f; y < high - 2.0f; y += 4.0f)
    {
        d.Mesh(Primitives::Cylinder(r + 0.04f, 0.12f, 16), {0.0f, y, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m.hazard, false);
    }
    d.Mesh(Primitives::Cylinder(r + 0.9f, 0.12f, 16), {0.0f, high - 2.5f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m.steel, false);
    for (int k = 0; k < 8; ++k)
    {
        const float a = glm::two_pi<float>() * static_cast<float>(k) / 8.0f;
        d.Beam({std::cos(a) * (r + 0.85f), high - 2.5f, std::sin(a) * (r + 0.85f)}, {std::cos(a) * (r + 0.85f), high - 1.4f, std::sin(a) * (r + 0.85f)},
               0.04f, m.steel, false);
    }
    // The tip and its flame.
    d.Mesh(Primitives::Frustum(r * 1.3f, r, 0.6f, 16), {0.0f, high + 0.3f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m.steel, false);
    d.Mesh(Primitives::Frustum(r * 1.1f, 0.05f, r * 6.0f, 12), {0.0f, high + 0.6f + r * 3.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m.glow, false);
    for (int k = 0; k < 3; ++k)
    {
        const float a = glm::two_pi<float>() * static_cast<float>(k) / 3.0f + 0.4f;
        const glm::vec3 anchor{std::cos(a) * high * 0.5f, 0.0f, std::sin(a) * high * 0.5f};
        d.Box(anchor + glm::vec3(0.0f, 0.2f, 0.0f), {0.8f, 0.4f, 0.8f}, m.concrete, true);
        d.Rod({0.0f, high * 0.7f, 0.0f}, anchor + glm::vec3(0.0f, 0.4f, 0.0f), 0.015f, m.dark, false, 5);
    }
}

// A low concrete wall round a tank farm, to hold what spills -- concrete steps over it on its -z side.
void Bund(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float hx = size.x * 0.5f;
    const float hz = size.z * 0.5f;
    const float high = size.y;
    constexpr float kWall = 0.3f;
    d.Box({0.0f, high * 0.5f, -hz}, {size.x + kWall, high, kWall}, m.concrete, true);
    d.Box({0.0f, high * 0.5f, hz}, {size.x + kWall, high, kWall}, m.concrete, true);
    d.Box({-hx, high * 0.5f, 0.0f}, {kWall, high, size.z - kWall}, m.concrete, true);
    d.Box({hx, high * 0.5f, 0.0f}, {kWall, high, size.z - kWall}, m.concrete, true);
    for (int s = 0; s < 3; ++s)
    {
        const float h = high * static_cast<float>(s + 1) / 3.0f;
        d.Box({0.0f, h * 0.5f, -hz - kWall * 0.5f - 0.3f * static_cast<float>(3 - s)}, {1.2f, h, 0.3f}, m.concrete, true);
        d.Box({0.0f, h * 0.5f, -hz + kWall * 0.5f + 0.3f * static_cast<float>(3 - s)}, {1.2f, h, 0.3f}, m.concrete, true);
    }
}

// A gantry crane over the containers: at either end along x a pair of legs braced together, a girder along each side over them,
// the trolley on the girders in the middle, its hook block down on its cables; painted yellow, hazard-striped at the legs' feet.
void Gantry(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float hx = size.x * 0.5f;
    const float hz = size.z * 0.5f;
    const float high = size.y;
    for (const float x : {-hx, hx})
    {
        for (const float z : {-hz, hz})
        {
            d.Box({x, high * 0.5f, z}, {0.6f, high, 0.6f}, m.hazard, true);
            d.Box({x, 0.4f, z}, {1.2f, 0.8f, 1.6f}, m.steel, true);
        }
        d.Box({x, high * 0.35f, 0.0f}, {0.4f, 0.4f, size.z}, m.hazard, false);
        d.Beam({x, 1.0f, -hz}, {x, high * 0.35f, 0.0f}, 0.2f, m.hazard, false);
        d.Beam({x, 1.0f, hz}, {x, high * 0.35f, 0.0f}, 0.2f, m.hazard, false);
    }
    for (const float z : {-hz, hz})
    {
        d.Box({0.0f, high + 0.5f, z}, {size.x + 1.0f, 1.0f, 0.7f}, m.hazard, true);
    }
    d.Box({0.0f, high + 1.3f, 0.0f}, {2.8f, 1.2f, size.z + 0.4f}, m.paint, false);
    d.Box({0.0f, high + 1.3f, -hz + 1.0f}, {1.8f, 1.6f, 1.6f}, m.paint, false);
    const float hook = high * 0.45f;
    for (const float x : {-0.25f, 0.25f})
    {
        d.Rod({x, high + 0.7f, 0.0f}, {x, hook + 0.6f, 0.0f}, 0.02f, m.dark, false, 5);
    }
    d.Box({0.0f, hook + 0.3f, 0.0f}, {0.8f, 0.6f, 0.4f}, m.hazard, false);
    d.Box({0.0f, hook - 0.2f, 0.0f}, {0.12f, 0.5f, 0.12f}, m.steel, false);
}

// Solar panels in two rows along x, tilted up towards -z on their posts, each a dark cell face in its frame.
void SolarArray(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float hx = size.x * 0.5f;
    const int panels = std::max(2, static_cast<int>(size.x / 2.1f));
    const float width = size.x / static_cast<float>(panels);
    constexpr float kLean = 0.55f;
    const glm::quat tilt = glm::angleAxis(-kLean, glm::vec3(1.0f, 0.0f, 0.0f));
    // (Each post up to the underside of the frame where it meets it: the frame's middle 1.25 m up, rising a metre in every
    // tan(lean) back, 2.5 cm thick.)
    const auto under = [&](float back) { return 1.25f + back * std::tan(kLean) - 0.025f / std::cos(kLean); };
    for (const float z : {-size.z * 0.25f, size.z * 0.25f})
    {
        for (int i = 0; i <= panels; ++i)
        {
            const float x = -hx + width * static_cast<float>(i);
            for (const float back : {-0.4f, 0.4f})
            {
                d.Box({x, (under(back) - 0.06f) * 0.5f, z + back}, {0.08f, under(back) - 0.06f, 0.08f}, m.steel, true);
            }
        }
        // A rail along the row on each line of posts, the frames resting on them.
        for (const float back : {-0.4f, 0.4f})
        {
            d.Box({0.0f, under(back) - 0.03f, z + back}, {size.x + 0.06f, 0.06f, 0.07f}, m.steel, false);
        }
        for (int i = 0; i < panels; ++i)
        {
            const float x = -hx + width * (static_cast<float>(i) + 0.5f);
            d.Box({x, 1.25f, z}, {width - 0.08f, 0.05f, 1.9f}, m.steel, false, tilt);
            d.Box({x, 1.28f, z}, {width - 0.16f, 0.02f, 1.8f}, m.dark, false, tilt);
        }
    }
}

// A dish antenna on its pedestal: a concrete pad, the pedestal, a turntable on it and its yoke's two arms up to the elevation axle;
// on the axle the hub, and on the hub the dish's back, turned up towards the sky along its yaw (-z); the feed at the dish's focus,
// held there on three struts from its rim.
void Dish(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float radius = size.x * 0.5f;
    const float depth = radius * 0.3f;
    const float pedestal = size.y * 0.45f;
    const glm::quat upright(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat alongX = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    d.Box({0.0f, 0.2f, 0.0f}, {radius * 1.2f, 0.4f, radius * 1.2f}, m.concrete, true);
    const float top = 0.4f + pedestal;
    d.Mesh(Primitives::Cylinder(0.7f, pedestal, 16), {0.0f, 0.4f + pedestal * 0.5f, 0.0f}, upright, m.paint, true);
    d.Box({0.0f, top + 0.25f, 0.0f}, {1.9f, 0.5f, 1.4f}, m.paint, false);
    const glm::vec3 axle{0.0f, top + 1.5f, 0.0f};
    for (const float x : {-0.8f, 0.8f})
    {
        d.Box({x, (top + 0.5f + axle.y + 0.25f) * 0.5f, 0.0f}, {0.24f, axle.y + 0.25f - top - 0.5f, 0.6f}, m.paint, false);
    }
    d.Mesh(Primitives::Cylinder(0.15f, 1.9f, 12), axle, alongX, m.steel, false);
    // The dish opens along its +y (FacilityOutside::Dish, its rim at its origin, its middle `depth` under that): turned from straight
    // up to forty-odd degrees off the sky towards -z, its back on the hub, the hub's face a hand proud inside it as a cap.
    const glm::quat raise = glm::angleAxis(-0.75f, glm::vec3(1.0f, 0.0f, 0.0f));
    const auto along = [&](float out) { return axle + raise * glm::vec3(0.0f, out, 0.0f); };
    constexpr float kBack = 0.55f; // the dish's middle, out along it from the axle
    d.Mesh(Primitives::Cylinder(0.45f, kBack + 0.06f + 0.25f, 16), along((kBack + 0.06f - 0.25f) * 0.5f), raise, m.paint, false);
    d.Mesh(FacilityOutside::Dish(radius, depth, 32), along(kBack + depth), raise, m.paint, false);
    // The feed at the focus (a parabola's: a quarter of the rim's radius squared over its depth, out from its middle).
    const float focus = kBack + radius * radius / (4.0f * depth);
    d.Mesh(Primitives::Cylinder(0.2f, 0.5f, 10), along(focus), raise, m.steel, false);
    for (int k = 0; k < 3; ++k)
    {
        const float a = glm::two_pi<float>() * static_cast<float>(k) / 3.0f;
        const float r = radius * 0.92f;
        const glm::vec3 rim = axle + raise * glm::vec3(std::cos(a) * r, kBack + depth * (r * r) / (radius * radius), std::sin(a) * r);
        d.Rod(rim, along(focus - 0.2f), 0.04f, m.steel, false, 6);
    }
}

// A fence round a rectangle: posts every two and a half metres, a rail at the top and the foot, the wire between drawn as its
// strands -- and felt as a wall -- three strands of barbed wire over it, on the posts; a gate's gap in the middle of its -z side, its two posts
// heavier.
void Fence(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float hx = size.x * 0.5f;
    const float hz = size.z * 0.5f;
    const float high = size.y;
    constexpr float kGate = 3.0f;
    constexpr float kBarbed = 0.45f; // (each post running on up past the mesh, to carry the barbed wire)
    // (A post where two runs meet stands once: two in one place, their faces were drawn twice. In its own frame.)
    std::vector<glm::vec2> posted;
    const auto post = [&](const glm::vec3& at)
    {
        for (const glm::vec2& done : posted)
        {
            if (glm::distance(done, glm::vec2(at.x, at.z)) < 0.05f)
            {
                return;
            }
        }
        posted.push_back({at.x, at.z});
        d.Box(at + glm::vec3(0.0f, (high + kBarbed) * 0.5f, 0.0f), {0.07f, high + kBarbed, 0.07f}, m.steel, true);
    };
    const auto run = [&](glm::vec3 a, glm::vec3 b)
    {
        const float length = glm::length(b - a);
        if (length < 0.3f)
        {
            return;
        }
        const glm::vec3 way = (b - a) / length;
        const int posts = std::max(1, static_cast<int>(length / 2.5f));
        for (int i = 0; i <= posts; ++i)
        {
            const glm::vec3 at = a + way * (length * static_cast<float>(i) / static_cast<float>(posts));
            post(at);
        }
        for (const float y : {0.08f, high - 0.04f})
        {
            d.Beam(a + glm::vec3(0.0f, y, 0.0f), b + glm::vec3(0.0f, y, 0.0f), 0.04f, m.steel, false);
        }
        // The wire: a strand every fifteen centimetres up it and along it, crossing.
        for (float y = 0.25f; y < high - 0.1f; y += 0.3f)
        {
            d.Beam(a + glm::vec3(0.0f, y, 0.0f), b + glm::vec3(0.0f, y, 0.0f), 0.006f, m.dark, false);
        }
        for (float t = 0.15f; t < length; t += 0.3f)
        {
            const glm::vec3 at = a + way * t;
            d.Beam(at + glm::vec3(0.0f, 0.1f, 0.0f), at + glm::vec3(0.0f, high - 0.06f, 0.0f), 0.006f, m.dark, false);
        }
        for (int s = 0; s < 3; ++s)
        {
            const float y = high + 0.12f + 0.12f * static_cast<float>(s);
            d.Beam(a + glm::vec3(0.0f, y, 0.0f), b + glm::vec3(0.0f, y, 0.0f), 0.01f, m.dark, false);
        }
        // Felt as the wall it is.
        Transform solid;
        solid.position = d.World((a + b) * 0.5f + glm::vec3(0.0f, high * 0.5f, 0.0f));
        solid.rotation = d.turn * glm::angleAxis(std::atan2(way.x, way.z), glm::vec3(0.0f, 1.0f, 0.0f));
        d.builder.AddSolid(solid, {0.06f, high, length});
    };
    // (The gate's posts its own, heavier, where the runs either side of it end.)
    posted.push_back({-kGate * 0.5f, -hz});
    posted.push_back({kGate * 0.5f, -hz});
    run({-hx, 0.0f, hz}, {hx, 0.0f, hz});
    run({-hx, 0.0f, -hz}, {-hx, 0.0f, hz});
    run({hx, 0.0f, -hz}, {hx, 0.0f, hz});
    run({-hx, 0.0f, -hz}, {-kGate * 0.5f, 0.0f, -hz});
    run({kGate * 0.5f, 0.0f, -hz}, {hx, 0.0f, -hz});
    for (const float x : {-kGate * 0.5f, kGate * 0.5f})
    {
        d.Box({x, (high + kBarbed + 0.1f) * 0.5f, -hz}, {0.14f, high + kBarbed + 0.1f, 0.14f}, m.steel, true);
    }
}

// A weather station: a mast on a footing, a wind vane and three cups spinning at its top, a box of instruments and a small panel
// to power it part way up.
void WeatherStation(Drawer& d, const glm::vec3& size, const SiteStructures::Materials& m)
{
    const float high = size.y;
    d.Box({0.0f, 0.15f, 0.0f}, {0.8f, 0.3f, 0.8f}, m.concrete, true);
    d.Mesh(Primitives::Cylinder(0.05f, high, 10), {0.0f, high * 0.5f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m.steel, true);
    // (The box strapped to the mast, its back against it; the panel out on a bracket.)
    d.Box({0.0f, high * 0.45f, 0.149f}, {0.3f, 0.4f, 0.2f}, m.paint, false);
    d.Box({0.0f, high * 0.65f, -0.25f}, {0.6f, 0.03f, 0.45f}, m.dark, false, glm::angleAxis(0.6f, glm::vec3(1.0f, 0.0f, 0.0f)));
    d.Beam({0.0f, high * 0.65f, 0.0f}, {0.0f, high * 0.65f, -0.25f}, 0.03f, m.steel, false);
    d.Beam({-0.45f, high, 0.0f}, {0.45f, high, 0.0f}, 0.03f, m.steel, false);
    // The vane on one end, the cups on the other, each on its spindle up off the arm.
    d.Beam({-0.45f, high, 0.0f}, {-0.45f, high + 0.12f, 0.0f}, 0.012f, m.steel, false);
    d.Beam({0.45f, high, 0.0f}, {0.45f, high + 0.15f, 0.0f}, 0.012f, m.steel, false);
    d.Box({-0.45f, high + 0.12f, 0.0f}, {0.03f, 0.03f, 0.5f}, m.steel, false);
    d.Box({-0.45f, high + 0.12f, -0.25f}, {0.02f, 0.15f, 0.15f}, m.steel, false);
    for (int k = 0; k < 3; ++k)
    {
        const float a = glm::two_pi<float>() * static_cast<float>(k) / 3.0f;
        const glm::vec3 cup{0.45f + std::cos(a) * 0.15f, high + 0.15f, std::sin(a) * 0.15f};
        d.Beam({0.45f, high + 0.15f, 0.0f}, cup, 0.01f, m.steel, false);
        d.Mesh(Primitives::Sphere(0.04f, 8, 6), cup, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m.paint, false);
    }
}

} // namespace

bool Is(SitePlan::BlockKind kind)
{
    switch (kind)
    {
    case Kind::Headframe:
    case Kind::SpoilHeap:
    case Kind::PipeRack:
    case Kind::FlareStack:
    case Kind::Bund:
    case Kind::Gantry:
    case Kind::SolarArray:
    case Kind::Dish:
    case Kind::Fence:
    case Kind::WeatherStation: return true;
    default: return false;
    }
}

void Build(const SitePlan::Block& block, const glm::vec3& foot, MapBuilder& builder, const Materials& made)
{
    Drawer d{builder, NameOf(block.kind), foot, glm::angleAxis(block.yaw, glm::vec3(0.0f, 1.0f, 0.0f))};
    switch (block.kind)
    {
    case Kind::Headframe: Headframe(d, block.size, made); break;
    case Kind::SpoilHeap: SpoilHeap(d, block.size, made); break;
    case Kind::PipeRack: PipeRack(d, block.size, made); break;
    case Kind::FlareStack: FlareStack(d, block.size, made); break;
    case Kind::Bund: Bund(d, block.size, made); break;
    case Kind::Gantry: Gantry(d, block.size, made); break;
    case Kind::SolarArray: SolarArray(d, block.size, made); break;
    case Kind::Dish: Dish(d, block.size, made); break;
    case Kind::Fence: Fence(d, block.size, made); break;
    case Kind::WeatherStation: WeatherStation(d, block.size, made); break;
    default: break;
    }
}

} // namespace SiteStructures
} // namespace pred
