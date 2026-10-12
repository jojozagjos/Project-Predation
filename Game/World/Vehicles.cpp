#include "Game/World/Vehicles.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Game/World/Surfaces.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace pred
{
namespace
{

// Colours: a worn pale hull and dark metal for the shuttle; for the crawler a faded working orange, grey inside, and
// black rubber tracks.
constexpr glm::vec3 kShuttleHull{0.44f, 0.45f, 0.43f};
constexpr glm::vec3 kShuttleTrim{0.2f, 0.21f, 0.22f};
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

    // A ramp hinged along its top edge at `hinge`, running back (+z) and down to the ground, and the three clips that
    // close, open and hold it closed: the hinge turns about x, and the ramp is carried round with it.
    void Ramp(const glm::vec3& hinge, float length, float width, float thickness, const glm::vec3& colour)
    {
        ModelPart& pivot = Box("fx_ramp_hinge", hinge.x - 0.005f, hinge.x + 0.005f, hinge.y - 0.005f, hinge.y + 0.005f, hinge.z - 0.005f,
                               hinge.z + 0.005f, colour);
        (void)pivot;
        // Steep enough that the underside of its far edge rests on the ground, rather than its top surface -- which would put
        // the rest of its thickness into the ground.
        const float slope = std::asin(std::clamp((hinge.y - thickness) / length, 0.0f, 1.0f));
        const float c = std::cos(slope);
        const float s = std::sin(slope);
        // Its top surface runs from the hinge down to the ground: the middle of it is back from the top edge by half its
        // length, and under it by half its thickness.
        const glm::vec3 fromHinge{0.0f, thickness * 0.5f * c + length * 0.5f * s, thickness * 0.5f * s - length * 0.5f * c};
        ModelPart& ramp = Box("ramp", -width * 0.5f, width * 0.5f, -thickness * 0.5f, thickness * 0.5f, -length * 0.5f, length * 0.5f, colour);
        ramp.position = hinge - fromHinge;
        ramp.rotation = {glm::degrees(slope), 0.0f, 0.0f};
        ramp.parent = "fx_ramp_hinge";

        const float closed = -(90.0f + glm::degrees(slope));
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

ModelAsset ShuttleModel()
{
    // A working dropship, nose at -z. Its skin is one piece (Shell), lofted through five sections from the nose back to the
    // open tail -- the windscreen raked up from the chin, the shoulders cut along the roof -- with the engines in nacelles
    // set into its flanks, a spine and a fin on top, two skids under it. Inside, a flight deck behind the windscreen (two
    // seats, the console the launch is started from) and, through a doorway, the cabin: benches either side, the ramp at
    // the back. The cabin floor is 0.7 m up, with two and a bit metres of headroom.
    ModelAsset model;
    model.name = "shuttle";
    Builder b{model};
    const float deck = 0.7f;
    const float ceiling = 3.0f;
    const float tail = 2.3f;     // the open back, where the ramp hangs
    const float bulkhead = -3.8f; // between the cabin and the flight deck
    const float wall = 0.15f;
    const float inner = 1.9f;    // the cabin's walls, inside
    const std::vector<Section> sections = {
        {-6.0f, 0.8f, 1.0f, 1.45f, 0.2f, 0.15f},  // the nose
        {-5.75f, 1.4f, 0.58f, 1.6f, 0.3f, 0.2f},  // the foot of the windscreen
        {-5.05f, 1.85f, 0.55f, 2.75f, 0.45f, 0.25f}, // its top
        {-4.4f, 2.05f, 0.5f, 3.2f, 0.5f, 0.25f},  // the shoulder
        {tail, 2.05f, 0.5f, 3.2f, 0.5f, 0.25f},   // the tail
    };
    const glm::vec3 warning{0.80f, 0.62f, 0.12f};
    // What each part is made of, laid on the shuttle itself, which flies.
    const auto made = [](ModelPart& part, const char* surface, float metres, float keep)
    {
        part.surface = surface;
        part.surfaceScale = metres;
        part.surfaceKeep = keep;
        part.surfaceOnThing = true;
    };
    const auto hull = [&](ModelPart& part) { made(part, "shuttle_hull", 4.0f, 0.15f); };
    const auto trim = [&](ModelPart& part) { made(part, "hull_dark", 2.0f, 0.3f); };
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
    // A cylinder about its middle, running along z.
    const auto barrel = [&](const std::string& name, const glm::vec3& centre, float diameter, float length, const glm::vec3& colour, float metallic = 0.6f,
                            float emissive = 0.0f) -> ModelPart&
    {
        ModelPart& part = b.Box(name, centre.x - diameter * 0.5f, centre.x + diameter * 0.5f, centre.y - length * 0.5f, centre.y + length * 0.5f,
                                centre.z - diameter * 0.5f, centre.z + diameter * 0.5f, colour, 0.5f, metallic, emissive);
        part.position = centre;
        part.size = {diameter, length, diameter};
        part.shape = PartShape::Cylinder;
        part.rotation = {90.0f, 0.0f, 0.0f};
        return part;
    };

    // --- The skin, and its windows: the windscreen and the corners either side of it, the flight deck's side windows, and a
    // strip either side of the cabin. (Never solid: what is walked into is the walls and the rest inside it.)
    hull(mesh("fx_shell", Shell(sections), kShuttleHull, 0.6f, 0.2f, 0.0f));
    MeshData glass;
    for (const MeshData& pane : {Pane(sections[1], sections[2], 4, 0.04f, 0.96f, 0.04f, 0.96f), Pane(sections[1], sections[2], 3, 0.1f, 0.94f, 0.08f, 0.92f),
                                 Pane(sections[1], sections[2], 5, 0.1f, 0.94f, 0.08f, 0.92f), Pane(sections[2], sections[3], 2, 0.12f, 0.88f, 0.42f, 0.86f),
                                 Pane(sections[2], sections[3], 6, 0.12f, 0.88f, 0.14f, 0.58f)})
    {
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
        const float x = side * 2.05f;
        b.Box("fx_window_" + name, std::min(x, x + side * 0.015f), std::max(x, x + side * 0.015f), 2.05f, 2.4f, -3.2f, 0.6f, kGlass, 0.15f, 0.3f, 0.06f);
        for (int stripe = 0; stripe < 4; ++stripe)
        {
            const float y0 = 0.75f + static_cast<float>(stripe) * 0.6f;
            b.Box("fx_stripe_" + name + "_" + std::to_string(stripe), std::min(x, x + side * 0.015f), std::max(x, x + side * 0.015f), y0, y0 + 0.3f,
                  tail - 0.4f, tail - 0.04f, warning, 0.7f);
        }
    }

    // --- What it stands on: two skids, each on two struts up to the belly.
    for (const float x : {-1.55f, 1.55f})
    {
        const std::string side = x < 0.0f ? "left" : "right";
        trim(b.Box("skid_" + side, x - 0.1f, x + 0.1f, 0.0f, 0.12f, -5.2f, 1.7f, kShuttleTrim, 0.55f, 0.6f));
        trim(b.Box("skid_toe_" + side, x - 0.1f, x + 0.1f, 0.06f, 0.2f, -5.45f, -5.2f, kShuttleTrim, 0.55f, 0.6f));
        for (const float z : {-4.3f, 0.9f})
        {
            trim(b.Box("skid_strut_" + side, x - 0.07f, x + 0.07f, 0.12f, 0.52f, z - 0.09f, z + 0.09f, kShuttleTrim, 0.5f, 0.7f));
        }
    }

    // --- Inside: the floor right through, the cabin's walls and ceiling, the bulkhead with its doorway, the benches.
    made(b.Box("deck", -inner - wall, inner + wall, 0.5f, deck, bulkhead, tail, kShuttleHull), "deck_plate", 1.6f, 0.6f);
    made(b.Box("flight_deck", -1.25f, 1.25f, 0.62f, deck, -5.7f, bulkhead, kShuttleHull), "deck_plate", 1.6f, 0.6f);
    hull(b.Box("wall_right", inner, inner + wall, deck, ceiling, bulkhead, tail, kShuttleHull));
    hull(b.Box("wall_left", -inner - wall, -inner, deck, ceiling, bulkhead, tail, kShuttleHull));
    hull(b.Box("roof", -inner - wall, inner + wall, ceiling, ceiling + 0.15f, bulkhead, tail, kShuttleHull));
    hull(b.Box("header", -inner, inner, ceiling - 0.3f, ceiling, tail - wall, tail, kShuttleHull));
    hull(b.Box("bulkhead_left", -inner, -0.5f, deck, ceiling, bulkhead, bulkhead + wall, kShuttleHull));
    hull(b.Box("bulkhead_right", 0.5f, inner, deck, ceiling, bulkhead, bulkhead + wall, kShuttleHull));
    hull(b.Box("bulkhead_over", -0.5f, 0.5f, 2.15f, ceiling, bulkhead, bulkhead + wall, kShuttleHull));
    trim(b.Box("bench_right", inner - 0.45f, inner, deck, deck + 0.45f, -3.2f, 1.3f, kShuttleHull));
    trim(b.Box("bench_left", -inner, -inner + 0.45f, deck, deck + 0.45f, -3.2f, 1.3f, kShuttleHull));
    b.Box("fx_cabin_light", -0.4f, 0.4f, ceiling - 0.05f, ceiling, -1.4f, -0.6f, {1.0f, 0.95f, 0.85f}, 0.5f, 0.0f, 1.5f);

    // --- The flight deck: under a ceiling below the windscreen's top, low walls either side to below its side windows, the
    // console across the front under the windscreen, and two seats.
    trim(b.Box("flight_ceiling", -1.75f, 1.75f, 2.72f, 2.82f, -5.05f, bulkhead, kShuttleTrim));
    trim(b.Box("flight_side_left", -1.8f, -1.3f, deck, 1.55f, -5.3f, bulkhead, kShuttleTrim));
    trim(b.Box("flight_side_right", 1.3f, 1.8f, deck, 1.55f, -5.3f, bulkhead, kShuttleTrim));
    trim(b.Box("console", -1.3f, 1.3f, deck, 1.3f, -5.7f, -5.3f, kShuttleTrim, 0.55f, 0.6f));
    b.Box("fx_console_screen", -0.9f, 0.9f, 1.3f, 1.32f, -5.65f, -5.35f, {0.3f, 0.6f, 1.0f}, 0.4f, 0.0f, 0.6f);
    for (const float x : {-0.75f, 0.75f})
    {
        const std::string side = x < 0.0f ? "left" : "right";
        trim(b.Box("seat_" + side, x - 0.25f, x + 0.25f, deck, deck + 0.45f, -4.85f, -4.35f, kShuttleTrim));
        trim(b.Box("seat_back_" + side, x - 0.25f, x + 0.25f, deck + 0.45f, deck + 1.25f, -4.35f, -4.22f, kShuttleTrim));
    }

    // --- On top: a spine along the roof, the fin at the tail, a mast forward.
    hull(b.Box("spine", -0.4f, 0.4f, 3.18f, 3.4f, -3.9f, 1.6f, kShuttleHull));
    trim(b.Box("fin", -0.07f, 0.07f, 3.38f, 4.05f, 0.5f, 2.0f, kShuttleTrim, 0.55f, 0.6f));
    trim(b.Box("mast", -0.04f, 0.04f, 3.38f, 3.95f, -3.3f, -3.22f, kShuttleTrim, 0.5f, 0.7f));

    // --- The engines: a nacelle set into either flank, its intake forward and its nozzle aft, glowing when they burn.
    for (const float side : {-1.0f, 1.0f})
    {
        const std::string name = side < 0.0f ? "left" : "right";
        const float x = side * 2.3f;
        const float y = 1.45f;
        hull(barrel("engine_" + name, {x, y, -0.6f}, 0.95f, 4.0f, kShuttleHull, 0.3f));
        trim(barrel("intake_" + name, {x, y, -2.66f}, 1.02f, 0.14f, kShuttleTrim));
        barrel("fx_intake_" + name, {x, y, -2.74f}, 0.78f, 0.02f, {0.02f, 0.02f, 0.025f}, 0.0f);
        trim(barrel("nozzle_" + name, {x, y, 1.6f}, 0.8f, 0.45f, kShuttleTrim, 0.8f));
        barrel("fx_engine_glow_" + name, {x, y, 1.84f}, 0.66f, 0.03f, {1.0f, 0.55f, 0.25f}, 0.0f, 0.6f);
        b.Box("fx_nav_" + name, x + side * 0.45f, x + side * 0.5f, y - 0.06f, y + 0.06f, -0.9f, -0.7f,
              side < 0.0f ? glm::vec3(0.9f, 0.1f, 0.08f) : glm::vec3(0.1f, 0.9f, 0.2f), 0.4f, 0.0f, 3.0f);
    }

    // As wide as the hull, not just the opening: closed, it covers the whole back, walls' edges and all.
    b.Ramp({0.0f, deck, tail}, 2.596f, 2.0f * (inner + wall) - 0.04f, 0.04f, kShuttleHull);
    for (ModelPart& part : model.parts)
    {
        if (part.name == "ramp")
        {
            made(part, "deck_plate", 1.6f, 0.6f);
        }
    }
    b.Socket("arrival", {0.0f, deck, -0.5f}, {0.0f, 180.0f, 0.0f});
    b.Socket("console", {0.0f, deck, -5.075f}, {0.0f, 180.0f, 0.0f});
    b.Socket("lamp", {0.0f, ceiling - 0.05f, -1.0f});
    // A landing light under its belly, straight down.
    b.Socket("light_landing", {0.0f, 0.2f, -2.0f}, {-90.0f, 0.0f, 0.0f});
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
    for (const ModelPart& part : m_model->parts)
    {
        Material material = Material::Diffuse(part.color, part.roughness);
        material.metallic = part.metallic;
        material.emissive = part.color * part.emissive;
        if (!part.surface.empty())
        {
            material = Surfaces::Apply(material, part.surface, part.surfaceScale, part.surfaceKeep);
            material.surfaceOnThing = part.surfaceOnThing;
        }
        const MeshHandle mesh = meshes.Upload(m_model->BuildPartMesh(part), "vehicle_" + m_model->name + "_" + part.name);
        m_parts.push_back(scene.CreateMeshEntity(prefix + part.name, Transform{}, mesh, material));
        if (MeshRenderer* renderer = scene.GetMeshRenderer(m_parts.back()))
        {
            renderer->castsShadow = part.emissive <= 0.0f;
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
    GoHome(scene);
    return true;
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
    if (m_model == nullptr)
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
