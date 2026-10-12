#include "Game/World/Cargo.h"

#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace pred
{

namespace Cargo
{

namespace
{

ModelPart& Box(ModelAsset& model, const std::string& name, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour, float roughness,
               float metallic = 0.0f)
{
    ModelPart part;
    part.name = name;
    part.shape = PartShape::Box;
    part.position = (lo + hi) * 0.5f;
    part.size = glm::abs(hi - lo);
    part.color = colour;
    part.roughness = roughness;
    part.metallic = metallic;
    model.parts.push_back(part);
    return model.parts.back();
}

// A cylinder about `centre`, upright, or lying along z or along x.
enum class Lying
{
    Upright,
    AlongZ,
    AlongX
};

ModelPart& Cylinder(ModelAsset& model, const std::string& name, const glm::vec3& centre, float diameter, float length, Lying lying,
                    const glm::vec3& colour, float roughness, float metallic = 0.0f)
{
    ModelPart part;
    part.name = name;
    part.shape = PartShape::Cylinder;
    part.position = centre;
    part.size = {diameter, length, diameter};
    // Only about x and then y, so a prop turned about y stays right (Place).
    part.rotation = lying == Lying::Upright ? glm::vec3(0.0f) : lying == Lying::AlongZ ? glm::vec3(90.0f, 0.0f, 0.0f) : glm::vec3(90.0f, 90.0f, 0.0f);
    part.color = colour;
    part.roughness = roughness;
    part.metallic = metallic;
    model.parts.push_back(part);
    return model.parts.back();
}

// Painted metal, worn (Material::surfaceWear): the scratched paint set, laid on the thing itself.
void Worn(ModelPart& part, float wear)
{
    part.surface = "painted_metal";
    part.surfaceScale = 0.8f;
    part.surfaceOnThing = true;
    part.surfaceWear = wear;
}

} // namespace

ModelAsset PlasticCrate(const glm::vec3& size, const glm::vec3& colour)
{
    ModelAsset model;
    model.name = "plastic_crate";
    const float w = size.x * 0.5f;
    const float d = size.z * 0.5f;
    const float lid = 0.05f;
    const float rim = size.y - lid;
    const float rough = 0.72f;
    // The tub, a little in from its corners and ribs, which stand proud of it.
    Box(model, "crate_body", {-w + 0.02f, 0.01f, -d + 0.02f}, {w - 0.02f, rim, d - 0.02f}, colour, rough);
    for (const float sx : {-1.0f, 1.0f})
    {
        for (const float sz : {-1.0f, 1.0f})
        {
            const glm::vec3 lo{sx < 0.0f ? -w : w - 0.07f, 0.0f, sz < 0.0f ? -d : d - 0.07f};
            Box(model, "crate_post", lo, lo + glm::vec3(0.07f, rim, 0.07f), colour * 0.92f, rough);
        }
    }
    // Ribs down its long sides, clear of the middle where the label goes; a stacking rim round its top.
    const int ribs = std::max(2, static_cast<int>(size.x / 0.26f));
    for (int i = 0; i < ribs; ++i)
    {
        const float x = -w + 0.07f + (size.x - 0.14f) * (static_cast<float>(i) + 0.5f) / static_cast<float>(ribs);
        if (std::abs(x) < 0.16f)
        {
            continue;
        }
        for (const float sz : {-1.0f, 1.0f})
        {
            const float z0 = sz < 0.0f ? -d : d - 0.02f;
            Box(model, "crate_rib", {x - 0.016f, 0.05f, z0}, {x + 0.016f, rim - 0.09f, z0 + 0.02f}, colour * 0.95f, rough);
        }
    }
    for (const float sz : {-1.0f, 1.0f})
    {
        const float z0 = sz < 0.0f ? -d - 0.004f : d - 0.02f;
        Box(model, "crate_band", {-w + 0.07f, rim - 0.075f, z0}, {w - 0.07f, rim - 0.03f, z0 + 0.024f}, colour * 0.9f, rough);
    }
    // The lid, a little over the tub all round, two ribs across it.
    Box(model, "crate_lid", {-w - 0.006f, rim, -d - 0.006f}, {w + 0.006f, size.y, d + 0.006f}, colour * 0.85f, 0.65f);
    for (const float sz : {-0.5f, 0.5f})
    {
        Box(model, "crate_lid_rib", {-w + 0.08f, size.y, sz * d - 0.03f}, {w - 0.08f, size.y + 0.012f, sz * d + 0.03f}, colour * 0.8f, 0.65f);
    }
    // Hand holds at the ends; a label on the front.
    const glm::vec3 dark = colour * 0.35f;
    for (const float sx : {-1.0f, 1.0f})
    {
        const float x0 = sx < 0.0f ? -w + 0.008f : w - 0.02f;
        Box(model, "crate_handle", {x0, rim - 0.22f, -0.13f}, {x0 + 0.012f, rim - 0.11f, 0.13f}, dark, 0.8f);
    }
    Box(model, "fx_crate_label", {-0.13f, size.y * 0.42f, d - 0.02f}, {0.13f, size.y * 0.42f + 0.11f, d - 0.011f}, {0.82f, 0.82f, 0.78f}, 0.9f);
    return model;
}

ModelAsset HardCase(const glm::vec3& size, const glm::vec3& colour)
{
    ModelAsset model;
    model.name = "hard_case";
    const float w = size.x * 0.5f;
    const float d = size.z * 0.5f;
    const float h = size.y;
    const glm::vec3 alloy{0.62f, 0.63f, 0.65f};
    const glm::vec3 steel{0.2f, 0.2f, 0.21f};
    const glm::vec3 rubber{0.05f, 0.05f, 0.055f};
    Worn(Box(model, "case_shell", {-w + 0.015f, 0.03f, -d + 0.015f}, {w - 0.015f, h - 0.015f, d - 0.015f}, colour, 0.55f, 0.3f), 0.35f);
    // Where the lid closes.
    const float seam = h * 0.64f;
    Box(model, "case_seam", {-w + 0.035f, seam - 0.012f, -d + 0.006f}, {w - 0.035f, seam + 0.012f, d - 0.006f}, steel, 0.5f, 0.7f);
    Box(model, "case_seam", {-w + 0.006f, seam - 0.012f, -d + 0.035f}, {w - 0.006f, seam + 0.012f, d - 0.035f}, steel, 0.5f, 0.7f);
    // Aluminium up its corners and along its top and bottom edges; steel corner guards over the ends of them.
    for (const float sx : {-1.0f, 1.0f})
    {
        for (const float sz : {-1.0f, 1.0f})
        {
            const glm::vec3 lo{sx < 0.0f ? -w : w - 0.035f, 0.03f, sz < 0.0f ? -d : d - 0.035f};
            Box(model, "case_edge", lo, lo + glm::vec3(0.035f, h - 0.045f, 0.035f), alloy, 0.35f, 0.9f);
            for (const float y : {0.024f, h - 0.06f})
            {
                const glm::vec3 g{sx < 0.0f ? -w - 0.006f : w - 0.054f, y, sz < 0.0f ? -d - 0.006f : d - 0.054f};
                Box(model, "case_corner", g, g + glm::vec3(0.06f, 0.066f, 0.06f), steel, 0.4f, 0.85f);
            }
        }
    }
    for (const float y : {0.02f, h - 0.035f})
    {
        for (const float sz : {-1.0f, 1.0f})
        {
            const float z0 = sz < 0.0f ? -d : d - 0.03f;
            Box(model, "case_rail", {-w + 0.054f, y, z0}, {w - 0.054f, y + 0.03f, z0 + 0.03f}, alloy, 0.35f, 0.9f);
        }
        for (const float sx : {-1.0f, 1.0f})
        {
            const float x0 = sx < 0.0f ? -w : w - 0.03f;
            Box(model, "case_rail", {x0, y, -d + 0.054f}, {x0 + 0.03f, y + 0.03f, d - 0.054f}, alloy, 0.35f, 0.9f);
        }
    }
    // Two latches across the seam on the front, a handle on top, feet.
    for (const float x : {-w * 0.55f, w * 0.55f})
    {
        Box(model, "case_latch", {x - 0.04f, seam - 0.05f, d - 0.015f}, {x + 0.04f, seam + 0.03f, d + 0.012f}, alloy, 0.3f, 0.9f);
    }
    for (const float x : {-0.1f, 0.1f})
    {
        Box(model, "case_handle_post", {x - 0.015f, h - 0.015f, -0.015f}, {x + 0.015f, h + 0.035f, 0.015f}, steel, 0.4f, 0.85f);
    }
    Box(model, "case_handle", {-0.12f, h + 0.035f, -0.02f}, {0.12f, h + 0.06f, 0.02f}, rubber, 0.85f);
    for (const float sx : {-1.0f, 1.0f})
    {
        for (const float sz : {-1.0f, 1.0f})
        {
            const glm::vec3 c{sx * (w - 0.11f), 0.0f, sz * (d - 0.11f)};
            Box(model, "case_foot", c - glm::vec3(0.03f, 0.0f, 0.03f), c + glm::vec3(0.03f, 0.03f, 0.03f), rubber, 0.9f);
        }
    }
    return model;
}

ModelAsset Drum(const glm::vec3& colour)
{
    ModelAsset model;
    model.name = "drum";
    const float d = 0.572f;
    const float h = 0.88f;
    Worn(Cylinder(model, "drum_body", {0.0f, h * 0.5f, 0.0f}, d, h - 0.02f, Lying::Upright, colour, 0.5f, 0.35f), 0.6f);
    // The chimes, top and bottom: the lid is the top one's face.
    Worn(Cylinder(model, "drum_chime", {0.0f, 0.0125f, 0.0f}, d + 0.014f, 0.025f, Lying::Upright, colour * 0.9f, 0.5f, 0.35f), 0.8f);
    Worn(Cylinder(model, "drum_chime", {0.0f, h - 0.0125f, 0.0f}, d + 0.014f, 0.025f, Lying::Upright, colour * 0.9f, 0.5f, 0.35f), 0.8f);
    for (const float y : {h / 3.0f, h * 2.0f / 3.0f})
    {
        Worn(Cylinder(model, "drum_hoop", {0.0f, y, 0.0f}, d + 0.026f, 0.03f, Lying::Upright, colour * 0.9f, 0.5f, 0.35f), 0.9f);
    }
    const glm::vec3 bung{0.25f, 0.25f, 0.26f};
    Cylinder(model, "drum_bung", {0.17f, h + 0.006f, 0.0f}, 0.07f, 0.012f, Lying::Upright, bung, 0.5f, 0.7f);
    Cylinder(model, "drum_bung", {-0.17f, h + 0.005f, 0.06f}, 0.045f, 0.01f, Lying::Upright, bung, 0.5f, 0.7f);
    return model;
}

ModelAsset GasRack(int count, const glm::vec3& colour)
{
    ModelAsset model;
    model.name = "gas_rack";
    count = std::max(count, 1);
    const float pitch = 0.26f;
    const float w = static_cast<float>(count) * pitch * 0.5f + 0.06f;
    const glm::vec3 frame{0.16f, 0.17f, 0.18f};
    const glm::vec3 body{0.22f, 0.23f, 0.25f};
    const glm::vec3 brass{0.62f, 0.46f, 0.2f};
    Box(model, "rack_base", {-w, 0.0f, -0.2f}, {w, 0.03f, 0.16f}, frame, 0.6f, 0.6f);
    for (const float x : {-w, w - 0.04f})
    {
        Box(model, "rack_upright", {x, 0.03f, -0.2f}, {x + 0.04f, 1.25f, -0.16f}, frame, 0.6f, 0.6f);
    }
    for (const float y : {0.5f, 1.1f})
    {
        Box(model, "rack_rail", {-w + 0.04f, y - 0.02f, -0.2f}, {w - 0.04f, y + 0.02f, -0.16f}, frame, 0.6f, 0.6f);
    }
    for (int i = 0; i < count; ++i)
    {
        const float x = -w + 0.06f + pitch * (static_cast<float>(i) + 0.5f);
        const float z = -0.02f;
        Worn(Cylinder(model, "bottle_body", {x, 0.03f + 0.62f, z}, 0.23f, 1.24f, Lying::Upright, body, 0.5f, 0.3f), 0.5f);
        ModelPart& shoulder = model.parts.emplace_back();
        shoulder.name = "bottle_shoulder";
        shoulder.shape = PartShape::Sphere;
        shoulder.position = {x, 1.27f, z};
        shoulder.size = glm::vec3(0.23f);
        shoulder.color = colour;
        shoulder.roughness = 0.5f;
        shoulder.metallic = 0.3f;
        Cylinder(model, "bottle_valve", {x, 1.42f, z}, 0.05f, 0.1f, Lying::Upright, brass, 0.35f, 0.9f);
        Cylinder(model, "bottle_wheel", {x, 1.475f, z}, 0.08f, 0.015f, Lying::Upright, frame, 0.6f, 0.5f);
    }
    Box(model, "rack_chain", {-w + 0.04f, 0.94f, 0.096f}, {w - 0.04f, 0.956f, 0.11f}, frame * 0.8f, 0.5f, 0.8f);
    return model;
}

ModelAsset WrappedPallet(float high)
{
    ModelAsset model;
    model.name = "pallet";
    const glm::vec3 wood{0.46f, 0.35f, 0.22f};
    const auto plank = [&](int i) { return wood * (0.85f + 0.06f * static_cast<float>((i * 7) % 4)); };
    int n = 0;
    for (const float z : {-0.45f, 0.0f, 0.45f})
    {
        Box(model, "pallet_board", {-0.6f, 0.0f, z - 0.05f}, {0.6f, 0.022f, z + 0.05f}, plank(n++), 0.88f);
        for (const float x : {-0.55f, 0.0f, 0.55f})
        {
            Box(model, "pallet_block", {x - 0.05f, 0.022f, z - 0.05f}, {x + 0.05f, 0.1f, z + 0.05f}, plank(n++) * 0.9f, 0.9f);
        }
    }
    for (const float x : {-0.55f, 0.0f, 0.55f})
    {
        Box(model, "pallet_stringer", {x - 0.05f, 0.1f, -0.5f}, {x + 0.05f, 0.122f, 0.5f}, plank(n++), 0.88f);
    }
    for (int i = 0; i < 7; ++i)
    {
        const float z = -0.45f + static_cast<float>(i) * 0.15f;
        Box(model, "pallet_deck", {-0.6f, 0.122f, z - 0.05f}, {0.6f, 0.144f, z + 0.05f}, plank(n++), 0.85f);
    }
    // The load: cartons two by two in layers, each a little apart, and two straps over it all.
    const float base = 0.144f;
    const int layers = std::max(1, static_cast<int>(std::round(high / 0.35f)));
    const float layer = high / static_cast<float>(layers);
    const glm::vec3 card{0.56f, 0.45f, 0.31f};
    for (int l = 0; l < layers; ++l)
    {
        const float y0 = base + layer * static_cast<float>(l);
        int c = 0;
        for (const float sx : {-1.0f, 1.0f})
        {
            for (const float sz : {-1.0f, 1.0f})
            {
                const glm::vec3 lo{sx < 0.0f ? -0.56f : 0.004f, y0, sz < 0.0f ? -0.47f : 0.004f};
                const glm::vec3 hi{sx < 0.0f ? -0.004f : 0.56f, y0 + layer - 0.004f, sz < 0.0f ? -0.004f : 0.47f};
                Box(model, "pallet_carton", lo, hi, card * (0.9f + 0.05f * static_cast<float>((l + c++) % 3)), 0.92f);
            }
        }
    }
    const float top = base + high - 0.004f;
    const glm::vec3 strap{0.1f, 0.16f, 0.3f};
    for (const float x : {-0.3f, 0.3f})
    {
        Box(model, "pallet_strap", {x - 0.025f, top, -0.476f}, {x + 0.025f, top + 0.006f, 0.476f}, strap, 0.6f);
        for (const float sz : {-1.0f, 1.0f})
        {
            const float z0 = sz < 0.0f ? -0.476f : 0.47f;
            Box(model, "pallet_strap", {x - 0.025f, base, z0}, {x + 0.025f, top, z0 + 0.006f}, strap, 0.6f);
        }
    }
    return model;
}

ModelAsset Duffel(const glm::vec3& colour)
{
    ModelAsset model;
    model.name = "duffel";
    // A capsule laid along x and flattened under its own weight.
    const float radius = 0.17f;
    const float squash = 0.72f;
    MeshData bag = Primitives::Capsule(radius, 0.42f, 12, 8);
    for (MeshVertex& vertex : bag.vertices)
    {
        const glm::vec3 p = vertex.position;
        vertex.position = {p.y, -p.x * squash + radius * squash, p.z};
        const glm::vec3 n = vertex.normal;
        vertex.normal = glm::normalize(glm::vec3(n.y, -n.x / squash, n.z));
    }
    ModelPart& part = model.parts.emplace_back();
    part.name = "duffel_bag";
    part.shape = PartShape::Mesh;
    part.size = glm::vec3(1.0f);
    part.mesh = std::move(bag);
    part.color = colour;
    part.roughness = 0.95f;
    const float top = radius * squash * 2.0f;
    const glm::vec3 webbing = colour * 0.4f;
    for (const float x : {-0.12f, 0.12f})
    {
        Box(model, "duffel_strap", {x - 0.02f, top - 0.03f, -0.1f}, {x + 0.02f, top + 0.025f, 0.1f}, webbing, 0.9f);
    }
    return model;
}

void Place(ModelAsset& into, const ModelAsset& prop, const glm::vec3& at, float yawDegrees, const std::string& prefix)
{
    const glm::quat turn = glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
    for (ModelPart part : prop.parts)
    {
        part.name = prefix + part.name;
        part.position = at + turn * part.position;
        part.rotation.y += yawDegrees;
        into.parts.push_back(std::move(part));
    }
}

void Bounds(const ModelAsset& prop, glm::vec3& lo, glm::vec3& hi)
{
    lo = glm::vec3(std::numeric_limits<float>::max());
    hi = glm::vec3(std::numeric_limits<float>::lowest());
    for (const ModelPart& part : prop.parts)
    {
        const glm::mat4 local = part.LocalMatrix();
        for (const MeshVertex& vertex : prop.BuildPartMesh(part).vertices)
        {
            const glm::vec3 p = glm::vec3(local * glm::vec4(vertex.position, 1.0f));
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    if (prop.parts.empty())
    {
        lo = hi = glm::vec3(0.0f);
    }
}

} // namespace Cargo

} // namespace pred
