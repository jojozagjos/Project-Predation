#include "Game/World/Vehicles.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

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

ModelAsset ShuttleModel()
{
    // Along it: the nose at -z, the cabin's front wall at z = -5.5, its open back at 2.5 and the ramp down to the pad
    // behind that. Across it: 4 m. Up: the cabin floor 0.7 m off the pad, two and a half metres of headroom.
    ModelAsset model;
    model.name = "shuttle";
    Builder b{model};
    const float back = -5.5f;
    const float door = 2.5f;
    const float half = 2.0f;
    const float wall = 0.2f;
    const float deck = 0.7f;
    const float ceiling = 3.2f;
    int leg = 0;
    for (const float z : {back + 0.7f, door - 0.7f})
    {
        for (const float x : {-1.3f, 1.3f})
        {
            b.Box("leg_" + std::to_string(leg++), x - 0.15f, x + 0.15f, 0.0f, 0.25f, z - 0.15f, z + 0.15f, kShuttleTrim, 0.55f, 0.6f);
        }
    }
    b.Box("belly", -1.6f, 1.6f, 0.25f, 0.55f, back, door, kShuttleTrim, 0.55f, 0.6f);
    b.Box("deck", -half, half, 0.55f, deck, back, door, kShuttleHull);
    b.Box("wall_right", half - wall, half, deck, ceiling, back, door, kShuttleHull);
    b.Box("wall_left", -half, -half + wall, deck, ceiling, back, door, kShuttleHull);
    b.Box("front_wall", -half + wall, half - wall, deck, ceiling, back, back + wall, kShuttleHull);
    b.Box("roof", -half, half, ceiling, ceiling + 0.25f, back, door, kShuttleHull);
    b.Box("header", -half + wall, half - wall, ceiling - 0.3f, ceiling, door - wall, door, kShuttleHull);
    b.Box("bench_right", half - wall - 0.45f, half - wall, deck, deck + 0.45f, -3.6f, 0.8f, kShuttleHull);
    b.Box("bench_left", -half + wall, -half + wall + 0.45f, deck, deck + 0.45f, -3.6f, 0.8f, kShuttleHull);
    b.Box("nose", -1.5f, 1.5f, 1.1f, 3.0f, back - 1.2f, back, kShuttleTrim, 0.55f, 0.6f);
    b.Box("engine_right", half, half + 0.8f, 1.1f, 2.3f, -5.0f, -1.2f, kShuttleTrim, 0.55f, 0.6f);
    b.Box("engine_left", -half - 0.8f, -half, 1.1f, 2.3f, -5.0f, -1.2f, kShuttleTrim, 0.55f, 0.6f);
    b.Box("fx_engine_glow_right", half + 0.1f, half + 0.7f, 1.2f, 2.2f, -1.2f, -1.15f, {1.0f, 0.55f, 0.25f}, 0.5f, 0.0f, 0.6f);
    b.Box("fx_engine_glow_left", -half - 0.7f, -half - 0.1f, 1.2f, 2.2f, -1.2f, -1.15f, {1.0f, 0.55f, 0.25f}, 0.5f, 0.0f, 0.6f);
    b.Box("fin", -0.08f, 0.08f, ceiling + 0.25f, ceiling + 1.2f, -5.2f, -3.2f, kShuttleTrim, 0.55f, 0.6f);
    b.Box("fx_cabin_light", -0.4f, 0.4f, ceiling - 0.05f, ceiling, -1.9f, -1.1f, {1.0f, 0.95f, 0.85f}, 0.5f, 0.0f, 1.5f);
    // The launch console, against the front wall, its screen on top towards the cabin.
    b.Box("console", -0.5f, 0.5f, deck, deck + 0.95f, back + wall, back + wall + 0.45f, kShuttleTrim, 0.55f, 0.6f);
    b.Box("fx_console_screen", -0.4f, 0.4f, deck + 0.95f, deck + 0.97f, back + wall + 0.06f, back + wall + 0.4f, {0.3f, 0.6f, 1.0f}, 0.4f, 0.0f, 0.6f);
    // As wide as the hull, not just the opening: closed, it covers the whole back, walls' edges and all.
    b.Ramp({0.0f, deck, door}, 2.596f, 2.0f * half - 0.04f, 0.04f, kShuttleHull);
    b.Socket("arrival", {0.0f, deck, -1.0f}, {0.0f, 180.0f, 0.0f});
    b.Socket("console", {0.0f, deck, back + wall + 0.225f}, {0.0f, 180.0f, 0.0f});
    b.Socket("lamp", {0.0f, ceiling - 0.05f, -1.5f});
    // A landing light under its belly, straight down.
    b.Socket("light_landing", {0.0f, 0.2f, -2.0f}, {-90.0f, 0.0f, 0.0f});
    b.Socket("cabin_min", {-half + wall, deck - 0.5f, back + wall});
    b.Socket("cabin_max", {half - wall, ceiling, door});
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

namespace
{

// A quad facing out from a centre, and a block narrowing from one upright rectangle across x to another.
void OutwardQuad(MeshData& mesh, const glm::vec3& a, glm::vec3 b, const glm::vec3& c, glm::vec3 d, const glm::vec3& centre)
{
    glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
    if (glm::dot(normal, (a + b + c + d) * 0.25f - centre) < 0.0f)
    {
        std::swap(b, d);
        normal = -normal;
    }
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    const glm::vec3 corners[4] = {a, b, c, d};
    const glm::vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    for (int i = 0; i < 4; ++i)
    {
        mesh.vertices.push_back(MeshVertex{corners[i], normal, uvs[i]});
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

MeshData Tapered(float backZ, float backHalf, float backLow, float backHigh, float frontZ, float frontHalf, float frontLow, float frontHigh)
{
    const glm::vec3 b0{-backHalf, backLow, backZ}, b1{backHalf, backLow, backZ}, b2{backHalf, backHigh, backZ}, b3{-backHalf, backHigh, backZ};
    const glm::vec3 f0{-frontHalf, frontLow, frontZ}, f1{frontHalf, frontLow, frontZ}, f2{frontHalf, frontHigh, frontZ},
        f3{-frontHalf, frontHigh, frontZ};
    const glm::vec3 centre = (b0 + b1 + b2 + b3 + f0 + f1 + f2 + f3) / 8.0f;
    MeshData mesh;
    OutwardQuad(mesh, b0, b1, b2, b3, centre);
    OutwardQuad(mesh, f0, f1, f2, f3, centre);
    OutwardQuad(mesh, b0, b1, f1, f0, centre);
    OutwardQuad(mesh, b3, b2, f2, f3, centre);
    OutwardQuad(mesh, b0, b3, f3, f0, centre);
    OutwardQuad(mesh, b1, b2, f2, f1, centre);
    return mesh;
}

} // namespace

ModelAsset CarrierModel()
{
    // In the ship's frame, round its rooms (ShipMap): the lower deck's floor at y = 0, the upper's at 3.6, the hangar
    // aft to z = 34.3 with its bay in the floor between z = 13 and 27, the cockpit forward at z = -40.
    ModelAsset model;
    model.name = "carrier";
    Builder b{model};
    int count = 0;
    const auto name = [&](const char* stem) { return std::string(stem) + "_" + std::to_string(count++); };
    const glm::vec3 hull{0.40f, 0.41f, 0.41f};
    const glm::vec3 panel{0.31f, 0.32f, 0.33f};
    const glm::vec3 dark{0.17f, 0.18f, 0.19f};
    const glm::vec3 glass{0.95f, 0.78f, 0.5f};
    const auto skin = [&](const char* stem, float x0, float x1, float y0, float y1, float z0, float z1, const glm::vec3& colour)
    {
        const bool metal = colour == dark;
        b.Box(name(stem), x0, x1, y0, y1, z0, z1, colour, metal ? 0.55f : 0.72f, metal ? 1.0f : 0.0f);
    };
    // A wall of skin with a window through it: the pieces round the gap.
    const auto pierced = [&](float x0, float x1, float y0, float y1, float z0, float z1, bool alongZ, float from, float to, float bottom, float top)
    {
        if (alongZ)
        {
            skin("skin", x0, x1, y0, y1, z0, from, hull);
            skin("skin", x0, x1, y0, y1, to, z1, hull);
            skin("skin", x0, x1, y0, bottom, from, to, hull);
            skin("skin", x0, x1, top, y1, from, to, hull);
        }
        else
        {
            skin("skin", x0, from, y0, y1, z0, z1, hull);
            skin("skin", to, x1, y0, y1, z0, z1, hull);
            skin("skin", from, to, y0, bottom, z0, z1, hull);
            skin("skin", from, to, top, y1, z0, z1, hull);
        }
    };
    const float out = 13.0f;
    const float upper = 3.6f;
    for (const float side : {-1.0f, 1.0f})
    {
        const float x0 = side < 0.0f ? -out : 12.3f;
        const float x1 = side < 0.0f ? -12.3f : out;
        const float o0 = side < 0.0f ? -out - 0.05f : out;
        const float o1 = side < 0.0f ? -out : out + 0.05f;
        skin("skin", x0, x1, -1.4f, 7.6f, -22.3f, 5.55f, hull);
        skin("skin", x0, x1, -1.4f, 9.0f, 5.55f, 34.6f, hull);
        // The hangar's sides stand out from the rest: sponsons, dark, with the bay's machinery in them.
        skin("sponson", side < 0.0f ? -out - 1.2f : out, side < 0.0f ? -out : out + 1.2f, -1.0f, 6.5f, 8.0f, 32.0f, dark);
        skin("sponson_cap", side < 0.0f ? -out - 1.3f : out, side < 0.0f ? -out : out + 1.3f, 6.5f, 6.9f, 7.5f, 32.5f, panel);
        // A darker belt along it at the deck line, and panels proud of it here and there.
        skin("belt", o0, o1, 2.8f, 3.4f, -22.3f, 34.6f, dark);
        for (float z = -19.5f; z < 33.0f; z += 7.0f)
        {
            skin("panel", side < 0.0f ? -out - 0.04f : out, side < 0.0f ? -out : out + 0.04f, 4.2f, 6.8f, z, z + 5.5f, panel);
            skin("panel", side < 0.0f ? -out - 0.04f : out, side < 0.0f ? -out : out + 0.04f, -0.8f, 2.2f, z + 1.0f, z + 4.0f, panel);
        }
        // Lit windows along it: people aboard.
        for (float z = -19.0f; z < 4.0f; z += 3.2f)
        {
            b.Box(name("fx_porthole"), side < 0.0f ? -out - 0.06f : out, side < 0.0f ? -out : out + 0.06f, 5.0f, 5.5f, z, z + 0.9f, glass, 0.4f, 0.0f, 1.4f);
        }
        // Radiators up off the hangar roof.
        for (const float x : {7.0f, 10.0f})
        {
            skin("radiator", side * x - 0.06f, side * x + 0.06f, 9.0f, 10.8f, 10.0f, 30.0f, dark);
        }
        // The step down from the main body to the cockpit block.
        skin("skin", side < 0.0f ? -out : 8.3f, side < 0.0f ? -8.3f : out, 3.0f, 7.6f, -22.6f, -22.3f, hull);
    }
    // Roof, belly and back fitted between the sides rather than over them: two faces in one plane fight over which is drawn.
    const float in = 12.3f;
    skin("skin", -in, in, 7.2f, 7.6f, -22.3f, 5.85f, hull);
    // A spine down the middle of the roof, and plating either side of it.
    skin("spine", -2.2f, 2.2f, 7.6f, 8.4f, -30.0f, 5.55f, panel);
    skin("spine", -2.2f, 2.2f, 9.0f, 9.6f, 5.85f, 33.0f, panel);
    for (float z = -20.0f; z < 4.0f; z += 6.0f)
    {
        skin("roof_plate", -11.5f, -3.0f, 7.6f, 7.68f, z, z + 4.8f, panel);
        skin("roof_plate", 3.0f, 11.5f, 7.6f, 7.68f, z + 1.0f, z + 5.8f, panel);
    }
    skin("skin", -in, in, 8.6f, 9.0f, 5.55f, 34.6f, hull);
    skin("skin", -in, in, 7.6f, 8.6f, 5.55f, 5.85f, hull);
    skin("skin", -in, in, -1.4f, 8.6f, 34.3f, 34.6f, dark);
    // The belly, open under the bay.
    skin("belly", -in, in, -1.4f, -0.6f, -22.3f, 13.0f, hull);
    skin("belly", -in, in, -1.4f, -0.6f, 27.0f, 34.3f, hull);
    skin("belly", -in, -4.55f, -1.4f, -0.6f, 13.0f, 27.0f, hull);
    skin("belly", 4.55f, in, -1.4f, -0.6f, 13.0f, 27.0f, hull);
    // The cockpit block, its windows through it, and a visor over the ones ahead.
    const float cockpit = 9.0f;
    pierced(-cockpit, -8.3f, 3.0f, 7.6f, -40.6f, -22.6f, true, -38.5f, -31.5f, upper + 0.9f, upper + 2.5f);
    pierced(8.3f, cockpit, 3.0f, 7.6f, -40.6f, -22.6f, true, -38.5f, -31.5f, upper + 0.9f, upper + 2.5f);
    skin("skin", -8.3f, 8.3f, 7.2f, 7.6f, -40.3f, -22.6f, hull);
    pierced(-8.3f, 8.3f, 3.0f, 7.6f, -40.6f, -40.3f, false, -6.8f, 6.8f, upper + 0.7f, upper + 2.7f);
    skin("visor", -7.4f, 7.4f, upper + 2.72f, upper + 3.02f, -40.9f, -40.35f, dark);
    // The bow under it, narrowing to a blunt nose ahead of the windows.
    const auto meshPart = [&](const char* stem, MeshData mesh, const glm::vec3& colour)
    {
        ModelPart part;
        part.name = name(stem);
        part.shape = PartShape::Mesh;
        part.size = glm::vec3(1.0f);
        part.mesh = std::move(mesh);
        part.color = colour;
        part.roughness = 0.72f;
        model.parts.push_back(part);
    };
    meshPart("bow", Tapered(-22.3f, out, -1.4f, 3.0f, -48.0f, 3.5f, 1.2f, 3.0f), hull);
    meshPart("bow_deck", Tapered(-22.3f, 8.3f, 3.0f, 3.2f, -46.0f, 3.2f, 3.0f, 3.1f), panel);
    // A mast and a dish on the roof.
    skin("mast", -0.12f, 0.12f, 7.6f, 11.8f, -12.12f, -11.88f, dark);
    const auto cylinder = [&](const std::string& partName, const glm::vec3& centre, float diameter, float height, const glm::vec3& rotation,
                              const glm::vec3& colour, float metallic, float emissive)
    {
        ModelPart& part = b.Box(partName, centre.x - diameter * 0.5f, centre.x + diameter * 0.5f, centre.y - height * 0.5f, centre.y + height * 0.5f,
                                centre.z - diameter * 0.5f, centre.z + diameter * 0.5f, colour, 0.45f, metallic, emissive);
        part.shape = PartShape::Cylinder;
        part.rotation = rotation;
    };
    cylinder(name("dish"), {0.0f, 11.0f, -12.0f}, 2.8f, 0.18f, {60.0f, 0.0f, 0.0f}, panel, 0.0f, 0.0f);
    // The engines aft: three, their nozzles dark rings round a glow a burn turns up.
    struct Engine
    {
        float x;
        float y;
        float half;
        float length;
    };
    int engineIndex = 0;
    for (const Engine& engine : {Engine{0.0f, 3.8f, 2.8f, 10.0f}, Engine{-9.2f, 3.0f, 2.2f, 8.0f}, Engine{9.2f, 3.0f, 2.2f, 8.0f}})
    {
        const float z0 = 34.6f;
        const float z1 = z0 + engine.length;
        skin("engine", engine.x - engine.half, engine.x + engine.half, engine.y - engine.half, engine.y + engine.half, z0, z1, panel);
        skin("engine_band", engine.x - engine.half - 0.05f, engine.x + engine.half + 0.05f, engine.y - engine.half - 0.05f,
             engine.y + engine.half + 0.05f, z0 + 2.0f, z0 + 2.6f, dark);
        cylinder(name("nozzle"), {engine.x, engine.y, z1 + 0.8f}, engine.half * 1.9f, 1.6f, {90.0f, 0.0f, 0.0f}, {0.12f, 0.12f, 0.13f}, 1.0f, 0.0f);
        // On the nozzle's mouth, just proud of it, where it can be seen.
        cylinder("fx_engine_glow_" + std::to_string(engineIndex++), {engine.x, engine.y, z1 + 1.625f}, engine.half * 1.6f, 0.02f, {90.0f, 0.0f, 0.0f},
                 {0.55f, 0.75f, 1.0f}, 0.0f, 0.3f);
    }
    // Its lights: red to port, green to starboard, white at the tail.
    b.Box("fx_nav_port", -out - 0.2f, -out, 3.0f, 3.2f, -22.0f, -21.8f, {1.0f, 0.1f, 0.08f}, 0.4f, 0.0f, 4.0f);
    b.Box("fx_nav_starboard", out, out + 0.2f, 3.0f, 3.2f, -22.0f, -21.8f, {0.1f, 1.0f, 0.2f}, 0.4f, 0.0f, 4.0f);
    b.Box("fx_nav_tail", -0.1f, 0.1f, 9.0f, 9.2f, 34.2f, 34.4f, {1.0f, 1.0f, 1.0f}, 0.4f, 0.0f, 4.0f);
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
    else if (name == "carrier")
    {
        *model = CarrierModel();
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
            renderer->visible = visible > 0.5f && !m_hidden;
        }
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
