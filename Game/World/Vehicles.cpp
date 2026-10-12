#include "Game/World/Vehicles.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Game/World/Surfaces.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pred
{
namespace
{

// Colours: a worn pale hull and dark metal for the shuttle; for the crawler a faded working orange, grey inside, and
// black rubber tracks.
constexpr glm::vec3 kShuttleHull{0.14f, 0.145f, 0.15f};
constexpr glm::vec3 kShuttleTrim{0.07f, 0.075f, 0.08f};
constexpr glm::vec3 kShuttleDeck{0.44f, 0.45f, 0.43f}; // the floor plate inside, lighter than the paint outside
constexpr glm::vec3 kCrawlerBody{0.52f, 0.22f, 0.12f};
constexpr glm::vec3 kCrawlerInside{0.3f, 0.3f, 0.31f};
constexpr glm::vec3 kCrawlerTrack{0.1f, 0.1f, 0.11f};
constexpr glm::vec3 kGlass{0.08f, 0.1f, 0.12f};

struct Builder
{
    ModelAsset& model;

    ModelPart& Box(const std::string& name, float x0, float x1, float y0, float y1, float z0, float z1, const glm::vec3& colour,
                   float roughness = 0.75f, float metallic = 0.0f, float emissive = 0.0f)
    {
        ModelPart part;
        part.name = name;
        part.shape = PartShape::Box;
        part.position = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f};
        part.size = {x1 - x0, y1 - y0, z1 - z0};
        part.color = colour;
        part.roughness = roughness;
        part.metallic = metallic;
        part.emissive = emissive;
        model.parts.push_back(part);
        return model.parts.back();
    }

    void Socket(const std::string& name, const glm::vec3& position, const glm::vec3& rotation = glm::vec3(0.0f))
    {
        ModelSocket socket;
        socket.name = name;
        socket.position = position;
        socket.rotation = rotation;
        model.sockets.push_back(socket);
    }

};

} // namespace

namespace Vehicles
{

// One cross-section of the shuttle's shell: how far along it, how wide either side, its belly and its roof, and how much of each
// corner is cut off (top and bottom).
struct Section
{
    float z;
    float half;
    float low;
    float high;
    float topCut;
    float bottomCut;

    // Its outline, round from the belly's left corner: eight corners.
    std::array<glm::vec3, 8> Corners() const
    {
        return {glm::vec3(-half + bottomCut, low, z), glm::vec3(half - bottomCut, low, z), glm::vec3(half, low + bottomCut, z),
                glm::vec3(half, high - topCut, z),     glm::vec3(half - topCut, high, z),     glm::vec3(-half + topCut, high, z),
                glm::vec3(-half, high - topCut, z),    glm::vec3(-half, low + bottomCut, z)};
    }
    glm::vec3 Middle() const { return {0.0f, (low + high) * 0.5f, z}; }
};

// A flat four-cornered face, turned to face away from `inside`.
void OutwardFace(MeshData& mesh, const glm::vec3& a, glm::vec3 b, const glm::vec3& c, glm::vec3 d, const glm::vec3& inside)
{
    glm::vec3 normal = glm::cross(b - a, c - a);
    if (glm::length(normal) < 1.0e-8f)
    {
        normal = glm::cross(c - a, d - a);
    }
    normal = glm::normalize(normal);
    if (glm::dot(normal, (a + b + c + d) * 0.25f - inside) < 0.0f)
    {
        std::swap(b, d);
        normal = -normal;
    }
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& corner : {a, b, c, d})
    {
        MeshVertex vertex;
        vertex.position = corner;
        vertex.normal = normal;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

// The skin over a run of sections, one piece with no gaps, closed at the front and open at the back.
MeshData Shell(const std::vector<Section>& sections)
{
    MeshData mesh;
    for (size_t i = 0; i + 1 < sections.size(); ++i)
    {
        const auto a = sections[i].Corners();
        const auto b = sections[i + 1].Corners();
        const glm::vec3 inside = (sections[i].Middle() + sections[i + 1].Middle()) * 0.5f;
        for (size_t k = 0; k < 8; ++k)
        {
            OutwardFace(mesh, a[k], a[(k + 1) % 8], b[(k + 1) % 8], b[k], inside);
        }
    }
    // The nose: closed over in a fan from its middle.
    const auto tip = sections.front().Corners();
    const glm::vec3 point = sections.front().Middle() - glm::vec3(0.0f, 0.0f, 0.12f);
    for (size_t k = 0; k < 8; ++k)
    {
        OutwardFace(mesh, tip[k], tip[(k + 1) % 8], point, point, sections[1].Middle());
    }
    return mesh;
}

// A pane over one face of the shell, a few millimetres out from it -- from u0 to u1 of the way along the face's run between
// two sections, and v0 to v1 of the way across it -- facing out, so it is seen from outside and looked out through from in.
MeshData Pane(const Section& from, const Section& to, size_t corner, float u0, float u1, float v0, float v1)
{
    const auto a = from.Corners();
    const auto b = to.Corners();
    const auto at = [&](float u, float v)
    {
        const glm::vec3 edge0 = glm::mix(a[corner], b[corner], u);
        const glm::vec3 edge1 = glm::mix(a[(corner + 1) % 8], b[(corner + 1) % 8], u);
        return glm::mix(edge0, edge1, v);
    };
    const glm::vec3 inside = (from.Middle() + to.Middle()) * 0.5f;
    glm::vec3 out = glm::normalize(glm::cross(at(1.0f, 0.0f) - at(0.0f, 0.0f), at(0.0f, 1.0f) - at(0.0f, 0.0f)));
    if (glm::dot(out, at(0.5f, 0.5f) - inside) < 0.0f)
    {
        out = -out;
    }
    MeshData mesh;
    OutwardFace(mesh, at(u0, v0) + out * 0.012f, at(u1, v0) + out * 0.012f, at(u1, v1) + out * 0.012f, at(u0, v1) + out * 0.012f, inside);
    return mesh;
}

// Where a face of the skin has a window (Pane): between which two sections, on which corner's face, and how much of it --
// u along the run, v across.
struct Window
{
    size_t section;
    size_t corner;
    float u0, u1, v0, v1;
};

// The same skin seen from inside: every face turned in, so looking out from the flight deck or the cabin the shell is
// there -- not the ship through the back of it. Round each window only a frame, so the glass is looked out through.
MeshData ShellInside(const std::vector<Section>& sections, const std::vector<Window>& windows)
{
    MeshData mesh;
    // Each face turned away from a point mirrored to its far side: towards the middle.
    const auto inward = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, const glm::vec3& inside)
    {
        const glm::vec3 centre = (a + b + c + d) * 0.25f;
        OutwardFace(mesh, a, b, c, d, centre * 2.0f - inside);
    };
    for (size_t i = 0; i + 1 < sections.size(); ++i)
    {
        const auto a = sections[i].Corners();
        const auto b = sections[i + 1].Corners();
        const glm::vec3 inside = (sections[i].Middle() + sections[i + 1].Middle()) * 0.5f;
        for (size_t k = 0; k < 8; ++k)
        {
            // A point of the face: u along the run from section i to i + 1, v across from corner k to the next.
            const auto at = [&](float u, float v) { return glm::mix(glm::mix(a[k], b[k], u), glm::mix(a[(k + 1) % 8], b[(k + 1) % 8], u), v); };
            const auto part = [&](float u0, float u1, float v0, float v1)
            {
                if (u1 - u0 > 1.0e-4f && v1 - v0 > 1.0e-4f)
                {
                    inward(at(u0, v0), at(u1, v0), at(u1, v1), at(u0, v1), inside);
                }
            };
            const auto window = std::find_if(windows.begin(), windows.end(), [&](const Window& w) { return w.section == i && w.corner == k; });
            if (window == windows.end())
            {
                part(0.0f, 1.0f, 0.0f, 1.0f);
                continue;
            }
            part(0.0f, window->u0, 0.0f, 1.0f);
            part(window->u1, 1.0f, 0.0f, 1.0f);
            part(window->u0, window->u1, 0.0f, window->v0);
            part(window->u0, window->u1, window->v1, 1.0f);
        }
    }
    const auto tip = sections.front().Corners();
    const glm::vec3 point = sections.front().Middle() - glm::vec3(0.0f, 0.0f, 0.12f);
    for (size_t k = 0; k < 8; ++k)
    {
        inward(tip[k], tip[(k + 1) % 8], point, point, sections[1].Middle());
    }
    return mesh;
}

// A closed solid through a run of rings (each the same number of points, in order round): its sides, and both ends capped --
// a fin, a fairing.
MeshData Loft(const std::vector<std::vector<glm::vec3>>& rings)
{
    MeshData mesh;
    glm::vec3 middle{0.0f};
    size_t count = 0;
    for (const auto& ring : rings)
    {
        for (const glm::vec3& point : ring)
        {
            middle += point;
            ++count;
        }
    }
    middle /= static_cast<float>(std::max<size_t>(count, 1));
    for (size_t i = 0; i + 1 < rings.size(); ++i)
    {
        const auto& a = rings[i];
        const auto& b = rings[i + 1];
        for (size_t k = 0; k < a.size(); ++k)
        {
            const size_t n = (k + 1) % a.size();
            OutwardFace(mesh, a[k], a[n], b[n], b[k], middle);
        }
    }
    for (const auto* end : {&rings.front(), &rings.back()})
    {
        glm::vec3 centre{0.0f};
        for (const glm::vec3& point : *end)
        {
            centre += point;
        }
        centre /= static_cast<float>(end->size());
        for (size_t k = 0; k < end->size(); ++k)
        {
            OutwardFace(mesh, (*end)[k], (*end)[(k + 1) % end->size()], centre, centre, middle);
        }
    }
    return mesh;
}

ModelAsset ShuttleModel()
{
    // A working dropship, nose at -z. Its skin is one piece (Shell), lofted through five sections from the nose back to the
    // open tail -- the windscreen raked up from the chin, the roof's edges cut -- and lined inside (ShellInside), so from in it
    // is a hull and not a hole. Everything inside fits within the skin, a few centimetres in from it, so nothing shows through
    // it and nothing shares a face with it. The engines are pods on pylons up on its flanks, clear of it; it stands on four
    // legs with feet. Inside, a flight deck behind the windscreen (two seats, the console the launch is started from) and,
    // through a wide doorway, the cabin: benches either side, the ramp at the back. The cabin floor is 0.7 m up, with 2.3 m of
    // headroom. Its skin is never solid, so where it could be walked out through, `solid_` boxes that are never drawn stand
    // just inside it.
    ModelAsset model;
    model.name = "shuttle";
    Builder b{model};
    const float deck = 0.7f;
    const float ceiling = 3.0f;
    const float tail = 2.3f;      // the open back, where the ramp hangs
    const float bulkhead = -3.8f; // between the cabin and the flight deck
    const float wall = 0.15f;
    const float inner = 1.9f;     // the cabin's walls, inside
    const float skin = 2.12f;     // the hull's side, outside: clear of the walls' outer faces
    const std::vector<Section> sections = {
        {-6.0f, 0.8f, 1.0f, 1.45f, 0.2f, 0.15f},     // the nose
        {-5.75f, 1.45f, 0.58f, 1.6f, 0.3f, 0.2f},    // the foot of the windscreen
        {-5.05f, 1.9f, 0.55f, 2.75f, 0.45f, 0.25f},  // its top
        {-4.4f, skin, 0.5f, 3.35f, 0.3f, 0.25f},     // the shoulder
        {tail, skin, 0.5f, 3.35f, 0.3f, 0.25f},      // the tail
    };
    const glm::vec3 warning{0.80f, 0.62f, 0.12f};
    const glm::vec3 lining{0.17f, 0.18f, 0.19f};
    // What each part is made of, laid on the shuttle itself, which flies.
    const auto made = [](ModelPart& part, const char* surface, float metres, float keep)
    {
        part.surface = surface;
        part.surfaceScale = metres;
        part.surfaceKeep = keep;
        part.surfaceOnThing = true;
    };
    // The cabin's walls painted plain: a panel set repeating round a room this small reads as tiles.
    const auto panelled = [](ModelPart& part)
    {
        part.color = {0.42f, 0.43f, 0.42f};
        part.roughness = 0.6f;
        part.metallic = 0.3f;
    };
    // The outside's paint, worn: chipped and scratched to the metal, more in some places than others (Material::surfaceWear).
    const auto worn = [](ModelPart& part)
    {
        part.surface = "cargo_crate";
        part.surfaceScale = 1.4f;
        part.surfaceOnThing = true;
        part.surfaceWear = 0.65f;
        part.roughness = 0.6f;
        part.metallic = 0.4f;
    };
    // Bare dark metal: the trim, the gear, the console -- plain, not a texture that reads as stone.
    const auto metal = [](ModelPart& part)
    {
        part.metallic = 0.75f;
        part.roughness = 0.4f;
    };
    const auto mesh = [&](const std::string& name, MeshData data, const glm::vec3& colour, float roughness, float metallic, float emissive) -> ModelPart&
    {
        ModelPart part;
        part.name = name;
        part.shape = PartShape::Mesh;
        part.size = glm::vec3(1.0f);
        part.mesh = std::move(data);
        part.color = colour;
        part.roughness = roughness;
        part.metallic = metallic;
        part.emissive = emissive;
        model.parts.push_back(part);
        return model.parts.back();
    };
    // A cylinder about its middle: along z, or upright.
    const auto cylinder = [&](const std::string& name, const glm::vec3& centre, float diameter, float length, const glm::vec3& colour, bool upright,
                              float metallic = 0.6f, float emissive = 0.0f) -> ModelPart&
    {
        ModelPart& part = b.Box(name, centre.x - diameter * 0.5f, centre.x + diameter * 0.5f, centre.y - length * 0.5f, centre.y + length * 0.5f,
                                centre.z - diameter * 0.5f, centre.z + diameter * 0.5f, colour, 0.45f, metallic, emissive);
        part.position = centre;
        part.size = {diameter, length, diameter};
        part.shape = PartShape::Cylinder;
        part.rotation = upright ? glm::vec3(0.0f) : glm::vec3(90.0f, 0.0f, 0.0f);
        return part;
    };
    // Solid and never drawn.
    const auto solid = [&](const std::string& name, float x0, float x1, float y0, float y1, float z0, float z1) -> ModelPart&
    { return b.Box("solid_" + name, x0, x1, y0, y1, z0, z1, lining); };

    // --- The skin, outside bare pale metal and inside lined; and its windows: the windscreen and the corners either side of
    // it, the flight deck's side windows, and a strip either side of the cabin.
    // The windscreen and the corners either side of it; the flight deck's side windows.
    const std::vector<Window> windows = {{1, 4, 0.04f, 0.96f, 0.04f, 0.96f}, {1, 3, 0.1f, 0.94f, 0.08f, 0.92f}, {1, 5, 0.1f, 0.94f, 0.08f, 0.92f},
                                         {2, 2, 0.12f, 0.88f, 0.42f, 0.86f}, {2, 6, 0.12f, 0.88f, 0.14f, 0.58f}};
    worn(mesh("fx_shell", Shell(sections), kShuttleHull, 0.35f, 0.65f, 0.0f));
    mesh("fx_shell_inside", ShellInside(sections, windows), lining, 0.85f, 0.2f, 0.0f);
    MeshData glass;
    for (const Window& window : windows)
    {
        const MeshData pane = Pane(sections[window.section], sections[window.section + 1], window.corner, window.u0, window.u1, window.v0, window.v1);
        const auto base = static_cast<uint32_t>(glass.vertices.size());
        glass.vertices.insert(glass.vertices.end(), pane.vertices.begin(), pane.vertices.end());
        for (const uint32_t index : pane.indices)
        {
            glass.indices.push_back(base + index);
        }
    }
    mesh("fx_windscreen", std::move(glass), kGlass, 0.08f, 0.4f, 0.05f);
    for (const float side : {-1.0f, 1.0f})
    {
        const std::string name = side < 0.0f ? "left" : "right";
        // Laid on the skin, a centimetre and a bit out from it.
        const float x0 = side * (skin + 0.012f);
        const float x1 = side * (skin + 0.027f);
        b.Box("fx_window_" + name, std::min(x0, x1), std::max(x0, x1), 2.05f, 2.4f, -3.2f, 0.6f, kGlass, 0.15f, 0.3f, 0.06f);
        for (int stripe = 0; stripe < 4; ++stripe)
        {
            const float y0 = 0.85f + static_cast<float>(stripe) * 0.55f;
            b.Box("fx_stripe_" + name + "_" + std::to_string(stripe), std::min(x0, x1), std::max(x0, x1), y0, y0 + 0.28f, tail - 0.4f, tail - 0.04f,
                  warning, 0.7f);
        }
    }

    // --- What it stands on: four legs, each a strut out of the belly in a sleeve, on a broad foot.
    for (const float x : {-1.5f, 1.5f})
    {
        for (const float z : {-4.0f, 1.2f})
        {
            const std::string at = std::string(x < 0.0f ? "left" : "right") + (z < 0.0f ? "_front" : "_back");
            metal(cylinder("leg_sleeve_" + at, {x, 0.44f, z}, 0.22f, 0.24f, kShuttleTrim, true));
            metal(cylinder("leg_strut_" + at, {x, 0.2f, z}, 0.12f, 0.3f, {0.5f, 0.51f, 0.52f}, true, 0.9f));
            metal(cylinder("leg_foot_" + at, {x, 0.04f, z}, 0.46f, 0.08f, kShuttleTrim, true));
        }
    }

    // --- Inside: the floor right through, the cabin's walls and ceiling, the bulkhead with its doorway, the benches.
    // The cabin stops short of the tail, where the door closes inside the hull's end.
    const float aft = tail - 0.12f;
    made(b.Box("deck", -inner, inner, 0.55f, deck, bulkhead, aft, kShuttleDeck), "deck_plate", 1.6f, 0.6f);
    made(b.Box("flight_deck", -1.2f, 1.2f, 0.64f, deck, -5.7f, bulkhead, kShuttleDeck), "deck_plate", 1.6f, 0.6f);
    panelled(b.Box("wall_right", inner, inner + wall, deck, ceiling, bulkhead, aft, kShuttleHull));
    panelled(b.Box("wall_left", -inner - wall, -inner, deck, ceiling, bulkhead, aft, kShuttleHull));
    panelled(b.Box("roof", -inner - wall, inner + wall, ceiling, ceiling + 0.08f, bulkhead, aft, kShuttleHull));
    panelled(b.Box("header", -inner, inner, ceiling - 0.3f, ceiling, aft - wall, aft, kShuttleHull));
    // A doorway a person walks through without ducking or turning: as high as the flight deck is.
    const float flightTop = 2.68f;
    panelled(b.Box("bulkhead_left", -inner, -0.7f, deck, ceiling, bulkhead, bulkhead + wall, kShuttleHull));
    panelled(b.Box("bulkhead_right", 0.7f, inner, deck, ceiling, bulkhead, bulkhead + wall, kShuttleHull));
    panelled(b.Box("bulkhead_over", -0.7f, 0.7f, flightTop, ceiling, bulkhead, bulkhead + wall, kShuttleHull));
    metal(b.Box("bench_right", inner - 0.45f, inner, deck, deck + 0.45f, -3.2f, 1.3f, kShuttleTrim));
    metal(b.Box("bench_left", -inner, -inner + 0.45f, deck, deck + 0.45f, -3.2f, 1.3f, kShuttleTrim));
    b.Box("fx_cabin_light", -0.4f, 0.4f, ceiling - 0.05f, ceiling, -1.4f, -0.6f, {1.0f, 0.95f, 0.85f}, 0.5f, 0.0f, 1.5f);

    // --- The flight deck: under a ceiling below the windscreen's top, low sides under its side windows, the console across
    // the front under the windscreen, and two seats.
    metal(b.Box("flight_ceiling", -1.3f, 1.3f, flightTop, flightTop + 0.06f, -5.05f, bulkhead, kShuttleTrim));
    metal(b.Box("flight_side_left", -1.6f, -1.2f, deck, 1.55f, -5.0f, bulkhead, kShuttleTrim));
    metal(b.Box("flight_side_right", 1.2f, 1.6f, deck, 1.55f, -5.0f, bulkhead, kShuttleTrim));
    metal(b.Box("console", -1.15f, 1.15f, deck, 1.3f, -5.65f, -5.3f, kShuttleTrim));
    b.Box("fx_console_screen", -0.85f, 0.85f, 1.3f, 1.32f, -5.6f, -5.35f, {0.3f, 0.6f, 1.0f}, 0.4f, 0.0f, 0.6f);
    for (const float x : {-0.6f, 0.6f})
    {
        const std::string side = x < 0.0f ? "left" : "right";
        metal(b.Box("seat_" + side, x - 0.25f, x + 0.25f, deck, deck + 0.45f, -4.85f, -4.35f, kShuttleTrim));
        metal(b.Box("seat_back_" + side, x - 0.25f, x + 0.25f, deck + 0.45f, deck + 1.25f, -4.35f, -4.22f, kShuttleTrim));
    }
    // Not out through the windscreen over the console, nor the side windows over the sides.
    {
        ModelPart& screen = solid("windscreen", -1.4f, 1.4f, -0.05f, 0.05f, -0.68f, 0.68f);
        // Along the windscreen, a little inside it: from its foot (1.6 m up at -5.75) to its top (2.75 m at -5.05).
        const glm::vec3 way = glm::normalize(glm::vec3(0.0f, 1.15f, 0.7f));
        const glm::vec3 in{0.0f, -way.z, way.y};
        screen.position = glm::vec3(0.0f, 2.175f, -5.4f) + in * 0.12f;
        screen.rotation = {-glm::degrees(std::atan2(1.15f, 0.7f)), 0.0f, 0.0f};
        solid("flight_window_left", -1.8f, -1.6f, 1.55f, flightTop, -5.0f, bulkhead);
        solid("flight_window_right", 1.6f, 1.8f, 1.55f, flightTop, -5.0f, bulkhead);
    }

    // --- On top: a spine along the roof, the fin at the tail, a mast forward.
    {
        // A fairing along the roof, low and bevelled, rising from the roof at its front and easing down at its back.
        const auto ring = [](float z, float half, float top, float bottom) -> std::vector<glm::vec3>
        { return {{-half, bottom, z}, {half, bottom, z}, {half * 0.7f, top, z}, {-half * 0.7f, top, z}}; };
        worn(mesh("spine", Loft({ring(-3.9f, 0.18f, 3.36f, 3.32f), ring(-3.2f, 0.42f, 3.56f, 3.32f), ring(1.2f, 0.42f, 3.56f, 3.32f),
                            ring(2.25f, 0.28f, 3.47f, 3.32f)}),
             kShuttleHull, 0.35f, 0.65f, 0.0f));
        // The fin on it, swept back, thinning to its top.
        const auto side = [](float x, float thin) -> std::vector<glm::vec3>
        // Its foot sunk into the fairing the whole of its length.
        { return {{x, 3.4f, 0.2f}, {x * thin, 4.45f, 1.45f}, {x * thin, 4.45f, 2.05f}, {x, 3.4f, 2.15f}}; };
        metal(mesh("fin", Loft({side(-0.07f, 0.45f), side(0.07f, 0.45f)}), kShuttleTrim, 0.4f, 0.75f, 0.0f));
        metal(b.Box("mast", -0.035f, 0.035f, 3.5f, 4.05f, -3.0f, -2.93f, kShuttleTrim));
        b.Box("fx_mast_light", -0.06f, 0.06f, 4.05f, 4.15f, -3.02f, -2.91f, {1.0f, 0.15f, 0.1f}, 0.3f, 0.0f, 3.0f);
    }

    // --- The engines: a pod up on either flank on a pylon, clear of the skin, its intake forward and its nozzle aft,
    // glowing when they burn.
    for (const float side : {-1.0f, 1.0f})
    {
        const std::string name = side < 0.0f ? "left" : "right";
        const float x = side * 2.6f;
        const float y = 1.5f; // below the cabin's windows
        const float x0 = side * (skin - 0.08f);
        const float x1 = side * (skin + 0.2f);
        metal(b.Box("pylon_" + name, std::min(x0, x1), std::max(x0, x1), y - 0.18f, y + 0.18f, -1.0f, 0.8f, kShuttleTrim));
        metal(cylinder("engine_" + name, {x, y, -0.2f}, 0.84f, 2.8f, {0.27f, 0.28f, 0.29f}, false));
        metal(cylinder("intake_" + name, {x, y, -1.62f}, 0.92f, 0.12f, kShuttleTrim, false));
        cylinder("fx_intake_" + name, {x, y, -1.69f}, 0.7f, 0.02f, {0.02f, 0.02f, 0.025f}, false, 0.0f);
        metal(cylinder("nozzle_" + name, {x, y, 1.42f}, 0.72f, 0.48f, kShuttleTrim, false));
        cylinder("fx_engine_glow_" + name, {x, y, 1.67f}, 0.6f, 0.03f, {1.0f, 0.55f, 0.25f}, false, 0.0f, 0.25f);
        b.Box("fx_nav_" + name, x + side * 0.42f, x + side * 0.46f, y - 0.06f, y + 0.06f, -0.5f, -0.3f,
              side < 0.0f ? glm::vec3(0.9f, 0.1f, 0.08f) : glm::vec3(0.1f, 0.9f, 0.2f), 0.4f, 0.0f, 3.0f);
    }

    // --- The tail: the hull's end closed round the cabin's opening -- the sides and the top, between the skin and the cabin's
    // walls and ceiling -- so nothing is seen through between them; and the door, the hull's own outline, closing inside it.
    {
        const Section& end = sections.back();
        const auto o = end.Corners();
        const glm::vec3 BL{-inner, deck, tail};
        const glm::vec3 BR{inner, deck, tail};
        const glm::vec3 TL{-inner, ceiling, tail};
        const glm::vec3 TR{inner, ceiling, tail};
        const glm::vec3 inside{0.0f, 1.9f, tail - 1.0f};
        const glm::vec3 behind{0.0f, 1.9f, tail + 1.0f};
        MeshData frame;
        // Both faces: from behind, and from inside the cabin looking back past the door.
        const auto tri = [&](const glm::vec3& a, const glm::vec3& b2, const glm::vec3& c)
        {
            OutwardFace(frame, a, b2, c, c, inside);
            OutwardFace(frame, a, b2, c, c, behind);
        };
        // Right: from the bottom-right corner of the opening round the hull's right side to its top.
        tri(TR, BR, o[1]);
        tri(TR, o[1], o[2]);
        tri(TR, o[2], o[3]);
        tri(TR, o[3], o[4]);
        // Top.
        tri(o[4], o[5], TL);
        tri(o[4], TL, TR);
        // Left.
        tri(TL, o[5], o[6]);
        tri(TL, o[6], o[7]);
        tri(TL, o[7], o[0]);
        tri(TL, o[0], BL);
        metal(mesh("tail_frame", std::move(frame), kShuttleTrim, 0.5f, 0.6f, 0.0f));
        // And the hollow between the cabin's walls and the skin closed at the tail, so nothing is seen through it.
        for (const float sideX : {-1.0f, 1.0f})
        {
            const float x0 = sideX * (inner + wall);
            const float x1 = sideX * (skin - 0.015f);
            metal(b.Box("tail_jamb", std::min(x0, x1), std::max(x0, x1), deck, ceiling + 0.08f, aft - 0.2f, tail - 0.003f, kShuttleTrim));
        }

        // The door: hinged at its foot just inside the hull's end, the hull's outline a few centimetres in, its outside the
        // hull's and its inside tread plate. Drawn open, as it rests (its clips close it); walked on as the box under it.
        const glm::vec3 hinge{0.0f, end.low + 0.06f, tail - 0.04f};
        const float thick = 0.05f;
        const float height = end.high - 0.04f - hinge.y;
        const float slope = std::asin(std::clamp((hinge.y - thick) / height, 0.0f, 1.0f));
        const float swing = 1.5707963f + slope;
        // A point of the door as it is closed (x across, h up from the hinge, d in from its outside face) where it is open.
        const auto open = [&](float x, float h, float d)
        {
            return glm::vec3(x, hinge.y + h * std::cos(swing) + d * std::sin(swing), hinge.z + h * std::sin(swing) - d * std::cos(swing));
        };
        // Inside the cabin's walls, so it closes past them, not into them; the frame round it hides the rest.
        Section shape = end;
        shape.half -= 0.12f;
        shape.low += 0.04f;
        shape.high -= 0.04f;
        const auto outline = shape.Corners();
        MeshData doorSkin;
        MeshData tread;
        const glm::vec3 core = open(0.0f, height * 0.5f, thick * 0.5f);
        const glm::vec3 treadMiddle = open(0.0f, height * 0.5f, thick);
        const glm::vec3 skinMiddle = open(0.0f, height * 0.5f, 0.0f);
        for (size_t k = 0; k < 8; ++k)
        {
            const glm::vec3 a = outline[k];
            const glm::vec3 c = outline[(k + 1) % 8];
            const glm::vec3 outA = open(a.x, a.y - hinge.y, 0.0f);
            const glm::vec3 outC = open(c.x, c.y - hinge.y, 0.0f);
            const glm::vec3 inA = open(a.x, a.y - hinge.y, thick);
            const glm::vec3 inC = open(c.x, c.y - hinge.y, thick);
            OutwardFace(doorSkin, outA, outC, skinMiddle, skinMiddle, core);
            OutwardFace(tread, inA, inC, treadMiddle, treadMiddle, core);
            OutwardFace(doorSkin, outA, outC, inC, inA, core);
        }
        ModelPart& pivot = b.Box("fx_ramp_hinge", hinge.x - 0.005f, hinge.x + 0.005f, hinge.y - 0.005f, hinge.y + 0.005f, hinge.z - 0.005f, hinge.z + 0.005f,
                                 kShuttleHull);
        (void)pivot;
        ModelPart& outside = mesh("ramp_skin", std::move(doorSkin), kShuttleHull, 0.35f, 0.65f, 0.0f);
        worn(outside);
        outside.parent = "fx_ramp_hinge";
        ModelPart& inner_ = mesh("ramp_tread", std::move(tread), kShuttleDeck, 0.6f, 0.4f, 0.0f);
        inner_.parent = "fx_ramp_hinge";
        made(inner_, "deck_plate", 1.6f, 0.6f);
        // What is walked on: a box under the tread, from the hinge down to the ground (never drawn).
        const float length = height;
        const float c = std::cos(slope);
        const float sn = std::sin(slope);
        const glm::vec3 fromHinge{0.0f, thick * 0.5f * c + length * 0.5f * sn, thick * 0.5f * sn - length * 0.5f * c};
        ModelPart& walk = b.Box("solid_ramp", -(inner - 0.05f), inner - 0.05f, -thick * 0.5f, thick * 0.5f, -length * 0.5f, length * 0.5f, kShuttleHull);
        walk.position = hinge + glm::vec3(0.0f, thick, 0.0f) - fromHinge;
        walk.rotation = {glm::degrees(slope), 0.0f, 0.0f};
        walk.parent = "fx_ramp_hinge";
        const float closed = -glm::degrees(swing);
        const auto clip = [&](const std::string& name, float duration, float from, float to)
        {
            AnimationClip animation;
            animation.name = name;
            animation.duration = duration;
            AnimationTrack track;
            track.part = "fx_ramp_hinge";
            AnimationKey first;
            first.time = 0.0f;
            first.rotation = {from, 0.0f, 0.0f};
            AnimationKey last;
            last.time = duration;
            last.rotation = {to, 0.0f, 0.0f};
            track.keys = {first, last};
            animation.tracks.push_back(track);
            model.clips.push_back(animation);
        };
        clip("ramp_closed", 0.1f, closed, closed);
        clip("ramp_opening", 2.5f, closed, 0.0f);
        clip("ramp_closing", 2.5f, 0.0f, closed);
    }
    b.Socket("arrival", {0.0f, deck, -0.5f}, {0.0f, 180.0f, 0.0f});
    b.Socket("console", {0.0f, deck, -5.075f}, {0.0f, 180.0f, 0.0f});
    b.Socket("lamp", {0.0f, ceiling - 0.05f, -1.0f});
    b.Socket("cabin_min", {-inner, deck - 0.5f, -5.7f});
    b.Socket("cabin_max", {inner, ceiling, tail});
    return model;
}

ModelAsset BayDoorsModel()
{
    // Nine metres across and fourteen long, their tops flush with the hangar floor at y = 0: two leaves, each hinged
    // along its outer edge, that swing down and out of the way for the shuttle to drop through.
    ModelAsset model;
    model.name = "hangar_doors";
    Builder b{model};
    const float half = 4.5f;
    const float length = 7.0f;
    const float thickness = 0.3f;
    const glm::vec3 steel{0.24f, 0.25f, 0.26f};
    const glm::vec3 yellow{0.72f, 0.55f, 0.1f};
    const glm::vec3 black{0.06f, 0.06f, 0.06f};
    for (const float side : {-1.0f, 1.0f})
    {
        const std::string name = side < 0.0f ? "left" : "right";
        const std::string hinge = "fx_hinge_" + name;
        b.Box(hinge, side * half - 0.005f, side * half + 0.005f, -thickness * 0.5f - 0.005f, -thickness * 0.5f + 0.005f, -0.005f, 0.005f, steel);
        const float outer = side * (half - 0.01f);
        const float inner = side * 0.01f;
        ModelPart& leaf = b.Box("door_" + name, std::min(outer, inner), std::max(outer, inner), -thickness, 0.0f, -length, length, steel, 0.6f, 0.5f);
        leaf.parent = hinge;
        // Stripes across its inner edge, where the two meet.
        for (int stripe = 0; stripe < 10; ++stripe)
        {
            const float z0 = -length + static_cast<float>(stripe) * 1.4f;
            const float edge = side * 0.01f;
            const float band = side * 0.6f;
            ModelPart& mark = b.Box("fx_stripe_" + name + "_" + std::to_string(stripe), std::min(edge, band), std::max(edge, band), 0.0f, 0.004f,
                                    z0, z0 + 1.4f, stripe % 2 == 0 ? yellow : black, 0.7f);
            mark.parent = hinge;
        }
    }
    const auto clip = [&](const std::string& name, float duration, float from, float to)
    {
        AnimationClip animation;
        animation.name = name;
        animation.duration = duration;
        for (const float side : {-1.0f, 1.0f})
        {
            AnimationTrack track;
            track.part = side < 0.0f ? "fx_hinge_left" : "fx_hinge_right";
            AnimationKey first;
            first.time = 0.0f;
            first.rotation = {0.0f, 0.0f, side * from};
            AnimationKey last;
            last.time = duration;
            last.rotation = {0.0f, 0.0f, side * to};
            track.keys = {first, last};
            animation.tracks.push_back(track);
        }
        model.clips.push_back(animation);
    };
    clip("doors_closed", 0.1f, 0.0f, 0.0f);
    clip("doors_opening", 3.5f, 0.0f, 100.0f);
    clip("doors_open", 0.1f, 100.0f, 100.0f);
    clip("doors_closing", 3.5f, 100.0f, 0.0f);
    return model;
}

std::shared_ptr<ModelAsset> Load(const std::string& name)
{
    auto model = std::make_shared<ModelAsset>();
    const std::filesystem::path file = ModelPath(name);
    if (!file.empty() && model->LoadFromFile(file))
    {
        return model;
    }
    if (name == "shuttle")
    {
        *model = ShuttleModel();
    }
    else if (name == "hangar_doors")
    {
        *model = BayDoorsModel();
    }
    else
    {
        return nullptr;
    }
    // Written the first time, so from now on it is a model like any other -- where the game keeps its models, and
    // nowhere else: a test, run from somewhere with no models folder, makes it and leaves the disk alone.
    if (!std::filesystem::exists(Paths::AssetsRoot() / "Models"))
    {
        return model;
    }
    const std::filesystem::path written = ModelPathFor(name, "Vehicles");
    if (model->SaveToFile(written))
    {
        RescanModels();
        PRED_LOG_INFO(Gameplay, "Wrote the {} to {}, to be edited in the model editor", name, written.string());
    }
    return model;
}

} // namespace Vehicles

// --- One standing in the world ---------------------------------------------------------------------------

namespace
{

bool SameMaterial(const Material& a, const Material& b)
{
    return a.baseColor == b.baseColor && a.metallic == b.metallic && a.roughness == b.roughness && a.emissive == b.emissive && a.cutout == b.cutout &&
           a.emissiveTextured == b.emissiveTextured && a.reflectivity == b.reflectivity && a.baseColorTexture == b.baseColorTexture &&
           a.normalTexture == b.normalTexture && a.roughnessTexture == b.roughnessTexture && a.surfaceScale == b.surfaceScale &&
           a.surfaceKeep == b.surfaceKeep && a.surfaceNatural == b.surfaceNatural && a.surfaceOnThing == b.surfaceOnThing && a.surfaceWear == b.surfaceWear &&
           a.steepColorTexture == b.steepColorTexture && a.steepScale == b.steepScale && a.organic == b.organic;
}

glm::mat4 Matrix(const CinePose& pose)
{
    return glm::translate(glm::mat4(1.0f), pose.position) * glm::mat4_cast(pose.rotation);
}

} // namespace

bool VehicleProp::Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics, std::shared_ptr<ModelAsset> model, const CinePose& home,
                        uint32_t overlapGroup, const std::string& prefix)
{
    Clear(scene, physics);
    if (model == nullptr)
    {
        return false;
    }
    m_model = std::move(model);
    m_home = home;
    // Merged: every part's mesh in the model's own frame, gathered by material and by stretch of it, each gathering one mesh.
    struct Gathered
    {
        Material material;
        bool shadow = true;
        glm::ivec3 cell{0};
        MeshData mesh;
    };
    std::vector<Gathered> gathered;
    for (const ModelPart& part : m_model->parts)
    {
        Material material = Material::Diffuse(part.color, part.roughness);
        material.metallic = part.metallic;
        material.emissive = part.color * part.emissive;
        if (!part.surface.empty())
        {
            material = part.surfaceWear > 0.0f ? Surfaces::ApplyWorn(material, part.surface, part.surfaceScale, part.surfaceWear)
                                               : Surfaces::Apply(material, part.surface, part.surfaceScale, part.surfaceKeep);
            material.surfaceOnThing = part.surfaceOnThing;
        }
        // A solid_ part is only there to be walked into: never drawn.
        const bool collider = part.name.rfind("solid_", 0) == 0;
        if (m_merged)
        {
            if (part.visible && !collider)
            {
                const glm::mat4 local = m_model->PartMatrixAt(part, nullptr, 0.0f);
                const glm::mat3 turn(local);
                const glm::ivec3 cell = glm::ivec3(glm::floor(glm::vec3(local[3]) / 32.0f));
                const bool shadow = part.emissive <= 0.0f;
                auto group = std::find_if(gathered.begin(), gathered.end(), [&](const Gathered& g)
                                          { return g.cell == cell && g.shadow == shadow && SameMaterial(g.material, material); });
                if (group == gathered.end())
                {
                    gathered.push_back({material, shadow, cell, MeshData{}});
                    group = gathered.end() - 1;
                }
                const MeshData piece = m_model->BuildPartMesh(part);
                const auto base = static_cast<uint32_t>(group->mesh.vertices.size());
                for (MeshVertex vertex : piece.vertices)
                {
                    vertex.position = glm::vec3(local * glm::vec4(vertex.position, 1.0f));
                    vertex.normal = glm::normalize(turn * vertex.normal);
                    group->mesh.vertices.push_back(vertex);
                }
                for (const uint32_t index : piece.indices)
                {
                    group->mesh.indices.push_back(base + index);
                }
            }
        }
        else
        {
            const MeshHandle mesh = meshes.Upload(m_model->BuildPartMesh(part), "vehicle_" + m_model->name + "_" + part.name);
            m_parts.push_back(scene.CreateMeshEntity(prefix + part.name, Transform{}, mesh, material));
            if (MeshRenderer* renderer = scene.GetMeshRenderer(m_parts.back()))
            {
                renderer->castsShadow = part.emissive <= 0.0f;
            }
        }
        // Solid where it rests, all but its glass, its lamps and its glow.
        if (physics == nullptr || part.name.rfind("fx_", 0) == 0 || !part.visible)
        {
            continue;
        }
        const glm::mat4 world = Matrix(home) * m_model->PartMatrixAt(part, nullptr, 0.0f);
        Transform transform;
        transform.position = glm::vec3(world[3]);
        transform.rotation = glm::normalize(glm::quat_cast(glm::mat3(world)));
        const glm::vec3 size = part.shape == PartShape::Box ? part.size
                               : part.shape == PartShape::Sphere ? glm::vec3(part.size.x)
                                                                 : glm::vec3(part.size.x, part.size.y, part.size.x);
        const BodyHandle body = physics->CreateBox(glm::max(size * 0.5f, glm::vec3(0.005f)), transform, BodyMotion::Static);
        physics->SetTag(body, Surfaces::StepOfSet(part.surface, part.metallic > 0.3f));
        if (overlapGroup != 0)
        {
            physics->SetOverlapGroup(body, overlapGroup);
        }
        m_bodies.push_back(body);
    }
    for (size_t i = 0; i < gathered.size(); ++i)
    {
        const MeshHandle mesh = meshes.Upload(gathered[i].mesh, "vehicle_" + m_model->name + "_merged#" + std::to_string(i));
        m_parts.push_back(scene.CreateMeshEntity(prefix + "merged", Transform{}, mesh, gathered[i].material));
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_parts.back()))
        {
            renderer->castsShadow = gathered[i].shadow;
        }
    }
    GoHome(scene);
    return true;
}

void VehicleProp::SetFarVisible(Scene& scene)
{
    for (const Entity entity : m_parts)
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
        {
            renderer->farVisible = true;
        }
    }
}

void VehicleProp::Clear(Scene& scene, PhysicsWorld* physics)
{
    for (const Entity part : m_parts)
    {
        scene.Destroy(part);
    }
    m_parts.clear();
    if (physics != nullptr)
    {
        for (const BodyHandle body : m_bodies)
        {
            physics->DestroyBody(body);
        }
    }
    m_bodies.clear();
    m_model = nullptr;
}

void VehicleProp::Show(Scene& scene, const CinePose& pose, const std::string& clip, float clipTime)
{
    if (m_model == nullptr)
    {
        return;
    }
    m_shown = pose;
    if (m_merged)
    {
        // All of it in one frame, the model's own.
        for (const Entity entity : m_parts)
        {
            if (Transform* transform = scene.GetTransform(entity))
            {
                transform->position = pose.position;
                transform->rotation = pose.rotation;
            }
            if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
            {
                renderer->visible = !m_hidden;
            }
        }
        return;
    }
    const AnimationClip* animation = clip.empty() ? nullptr : m_model->FindClip(clip);
    const glm::mat4 root = Matrix(pose);
    for (size_t i = 0; i < m_parts.size() && i < m_model->parts.size(); ++i)
    {
        float visible = 1.0f;
        const glm::mat4 world = root * m_model->PartMatrixAt(m_model->parts[i], animation, clipTime, &visible);
        if (Transform* transform = scene.GetTransform(m_parts[i]))
        {
            transform->position = glm::vec3(world[3]);
            transform->rotation = glm::normalize(glm::quat_cast(glm::mat3(world)));
        }
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_parts[i]))
        {
            bool put = false;
            for (const std::string& prefix : m_hiddenPrefixes)
            {
                put = put || m_model->parts[i].name.rfind(prefix, 0) == 0;
            }
            put = put || m_model->parts[i].name.rfind("solid_", 0) == 0;
            renderer->visible = visible > 0.5f && !m_hidden && !put;
        }
    }
}

void VehicleProp::SetPartsHidden(Scene& scene, const std::string& prefix, bool hidden)
{
    const auto found = std::find(m_hiddenPrefixes.begin(), m_hiddenPrefixes.end(), prefix);
    if (hidden == (found != m_hiddenPrefixes.end()))
    {
        return;
    }
    if (hidden)
    {
        m_hiddenPrefixes.push_back(prefix);
    }
    else
    {
        m_hiddenPrefixes.erase(found);
    }
    if (m_model != nullptr)
    {
        Show(scene, m_shown);
    }
}

void VehicleProp::SetHidden(Scene& scene, bool hidden)
{
    if (hidden == m_hidden)
    {
        return;
    }
    m_hidden = hidden;
    Show(scene, m_shown);
}

Entity VehicleProp::Part(const std::string& name) const
{
    if (m_model == nullptr || m_merged)
    {
        return Entity{};
    }
    for (size_t i = 0; i < m_model->parts.size() && i < m_parts.size(); ++i)
    {
        if (m_model->parts[i].name == name)
        {
            return m_parts[i];
        }
    }
    return Entity{};
}

bool VehicleProp::Socket(const std::string& name, CinePose& out) const
{
    if (m_model == nullptr)
    {
        return false;
    }
    const ModelSocket* socket = m_model->FindSocket(name);
    if (socket == nullptr)
    {
        return false;
    }
    out.position = m_home.Apply(socket->position);
    out.rotation = m_home.rotation * socket->Rotation();
    return true;
}

bool VehicleProp::SocketShown(const std::string& name, CinePose& out) const
{
    const ModelSocket* socket = m_model != nullptr ? m_model->FindSocket(name) : nullptr;
    if (socket == nullptr)
    {
        return false;
    }
    out.position = m_shown.Apply(socket->position);
    out.rotation = m_shown.rotation * socket->Rotation();
    return true;
}

bool VehicleProp::Aboard(const glm::vec3& point) const
{
    if (m_model == nullptr)
    {
        return false;
    }
    const ModelSocket* low = m_model->FindSocket("cabin_min");
    const ModelSocket* high = m_model->FindSocket("cabin_max");
    if (low == nullptr || high == nullptr)
    {
        return false;
    }
    const glm::vec3 local = glm::inverse(m_home.rotation) * (point - m_home.position);
    const glm::vec3 lo = glm::min(low->position, high->position);
    const glm::vec3 hi = glm::max(low->position, high->position);
    return local.x > lo.x && local.x < hi.x && local.y > lo.y && local.y < hi.y && local.z > lo.z && local.z < hi.z;
}

} // namespace pred
