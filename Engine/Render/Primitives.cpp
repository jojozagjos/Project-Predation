#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pred::Primitives
{
namespace
{

// Adds a quad centred at `center`, spanned by half-axes u and v, facing cross(u, v).
// Emitting corners in the order (-u,-v) (+u,-v) (+u,+v) (-u,+v) gives counter-clockwise winding
// when viewed from the front, which is the glTF convention the renderer expects.
void AddQuad(MeshData& mesh, const glm::vec3& center, const glm::vec3& u, const glm::vec3& v,
             const glm::vec2& uvScale = glm::vec2(1.0f))
{
    const glm::vec3 normal = glm::normalize(glm::cross(u, v));
    const auto base = static_cast<uint32_t>(mesh.vertices.size());

    const glm::vec3 corners[4] = {center - u - v, center + u - v, center + u + v, center - u + v};
    const glm::vec2 uvs[4] = {{0.0f, 0.0f}, {uvScale.x, 0.0f}, {uvScale.x, uvScale.y}, {0.0f, uvScale.y}};

    for (int i = 0; i < 4; ++i)
    {
        mesh.vertices.push_back(MeshVertex{corners[i], normal, uvs[i]});
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void AddTriangle(MeshData& mesh, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(MeshVertex{a, normal, {0.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{b, normal, {1.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{c, normal, {0.5f, 1.0f}});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}

} // namespace

MeshData Box(const glm::vec3& size)
{
    const glm::vec3 h = size * 0.5f;
    MeshData mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);

    AddQuad(mesh, {h.x, 0.0f, 0.0f}, {0.0f, 0.0f, -h.z}, {0.0f, h.y, 0.0f}); // +X
    AddQuad(mesh, {-h.x, 0.0f, 0.0f}, {0.0f, 0.0f, h.z}, {0.0f, h.y, 0.0f}); // -X
    AddQuad(mesh, {0.0f, h.y, 0.0f}, {h.x, 0.0f, 0.0f}, {0.0f, 0.0f, -h.z}); // +Y
    AddQuad(mesh, {0.0f, -h.y, 0.0f}, {h.x, 0.0f, 0.0f}, {0.0f, 0.0f, h.z}); // -Y
    AddQuad(mesh, {0.0f, 0.0f, h.z}, {h.x, 0.0f, 0.0f}, {0.0f, h.y, 0.0f});  // +Z
    AddQuad(mesh, {0.0f, 0.0f, -h.z}, {-h.x, 0.0f, 0.0f}, {0.0f, h.y, 0.0f}); // -Z

    return mesh;
}

MeshData Plane(const glm::vec2& size, int subdivisions)
{
    subdivisions = std::max(1, subdivisions);
    MeshData mesh;

    const glm::vec2 half = size * 0.5f;
    const int verticesPerSide = subdivisions + 1;
    mesh.vertices.reserve(static_cast<size_t>(verticesPerSide) * verticesPerSide);

    for (int z = 0; z < verticesPerSide; ++z)
    {
        const float tz = static_cast<float>(z) / static_cast<float>(subdivisions);
        for (int x = 0; x < verticesPerSide; ++x)
        {
            const float tx = static_cast<float>(x) / static_cast<float>(subdivisions);
            MeshVertex vertex;
            vertex.position = {-half.x + tx * size.x, 0.0f, -half.y + tz * size.y};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            // One UV unit per metre keeps texture density constant regardless of plane size.
            vertex.uv = {tx * size.x, tz * size.y};
            mesh.vertices.push_back(vertex);
        }
    }

    mesh.indices.reserve(static_cast<size_t>(subdivisions) * subdivisions * 6);
    for (int z = 0; z < subdivisions; ++z)
    {
        for (int x = 0; x < subdivisions; ++x)
        {
            const auto i0 = static_cast<uint32_t>(z * verticesPerSide + x);
            const auto i1 = static_cast<uint32_t>(i0 + 1);
            const auto i2 = static_cast<uint32_t>(i0 + verticesPerSide);
            const auto i3 = static_cast<uint32_t>(i2 + 1);
            // Counter-clockwise seen from +Y.
            mesh.indices.insert(mesh.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return mesh;
}

MeshData Sphere(float radius, int segments, int rings)
{
    segments = std::max(3, segments);
    rings = std::max(2, rings);

    MeshData mesh;
    mesh.vertices.reserve(static_cast<size_t>(segments + 1) * (rings + 1));

    for (int ring = 0; ring <= rings; ++ring)
    {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float phi = v * glm::pi<float>(); // 0 at +Y pole
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);

        for (int segment = 0; segment <= segments; ++segment)
        {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float theta = u * glm::two_pi<float>();
            const glm::vec3 normal{sinPhi * std::sin(theta), cosPhi, sinPhi * std::cos(theta)};
            mesh.vertices.push_back(MeshVertex{normal * radius, normal, {u, v}});
        }
    }

    const int stride = segments + 1;
    mesh.indices.reserve(static_cast<size_t>(segments) * rings * 6);
    for (int ring = 0; ring < rings; ++ring)
    {
        for (int segment = 0; segment < segments; ++segment)
        {
            const auto i0 = static_cast<uint32_t>(ring * stride + segment);
            const auto i1 = static_cast<uint32_t>(i0 + stride);
            mesh.indices.insert(mesh.indices.end(), {i0, i1, i0 + 1, i0 + 1, i1, i1 + 1});
        }
    }
    return mesh;
}

MeshData Cylinder(float radius, float height, int segments)
{
    segments = std::max(3, segments);
    const float halfHeight = height * 0.5f;

    MeshData mesh;

    // Side wall: one quad per segment, with duplicated vertices so normals stay faceted-correct.
    for (int segment = 0; segment < segments; ++segment)
    {
        const float t0 = static_cast<float>(segment) / static_cast<float>(segments);
        const float t1 = static_cast<float>(segment + 1) / static_cast<float>(segments);
        const float a0 = t0 * glm::two_pi<float>();
        const float a1 = t1 * glm::two_pi<float>();

        const glm::vec3 n0{std::sin(a0), 0.0f, std::cos(a0)};
        const glm::vec3 n1{std::sin(a1), 0.0f, std::cos(a1)};

        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({n0 * radius - glm::vec3(0.0f, halfHeight, 0.0f), n0, {t0, 0.0f}});
        mesh.vertices.push_back({n1 * radius - glm::vec3(0.0f, halfHeight, 0.0f), n1, {t1, 0.0f}});
        mesh.vertices.push_back({n1 * radius + glm::vec3(0.0f, halfHeight, 0.0f), n1, {t1, 1.0f}});
        mesh.vertices.push_back({n0 * radius + glm::vec3(0.0f, halfHeight, 0.0f), n0, {t0, 1.0f}});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    // Caps as triangle fans.
    for (int cap = 0; cap < 2; ++cap)
    {
        const bool top = cap == 0;
        const glm::vec3 normal{0.0f, top ? 1.0f : -1.0f, 0.0f};
        const float y = top ? halfHeight : -halfHeight;

        const auto center = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({{0.0f, y, 0.0f}, normal, {0.5f, 0.5f}});

        for (int segment = 0; segment <= segments; ++segment)
        {
            const float angle = static_cast<float>(segment) / static_cast<float>(segments) * glm::two_pi<float>();
            const glm::vec3 position{std::sin(angle) * radius, y, std::cos(angle) * radius};
            mesh.vertices.push_back({position, normal, {position.x / radius * 0.5f + 0.5f,
                                                       position.z / radius * 0.5f + 0.5f}});
        }
        for (int segment = 0; segment < segments; ++segment)
        {
            const auto i0 = static_cast<uint32_t>(center + 1 + segment);
            const auto i1 = static_cast<uint32_t>(center + 2 + segment);
            if (top)
            {
                mesh.indices.insert(mesh.indices.end(), {center, i1, i0});
            }
            else
            {
                mesh.indices.insert(mesh.indices.end(), {center, i0, i1});
            }
        }
    }
    return mesh;
}

MeshData Stairs(int stepCount, float stepWidth, float stepRise, float stepRun)
{
    stepCount = std::max(1, stepCount);
    MeshData mesh;

    for (int step = 0; step < stepCount; ++step)
    {
        // Each step is a full-height block from the ground up, so the staircase is solid underneath
        // and can be turned into physics geometry without gaps.
        const float height = stepRise * static_cast<float>(step + 1);
        const MeshData block = Box({stepWidth, height, stepRun});
        const glm::mat4 transform = glm::translate(
            glm::mat4(1.0f),
            glm::vec3(0.0f, height * 0.5f, stepRun * (static_cast<float>(step) + 0.5f)));
        mesh.Append(block, transform);
    }
    return mesh;
}

MeshData Ramp(float width, float length, float height)
{
    MeshData mesh;
    const float hw = width * 0.5f;

    // Corners: base at y=0 spanning z in [0, length], rising to `height` at z = length.
    const glm::vec3 a{-hw, 0.0f, 0.0f};
    const glm::vec3 b{hw, 0.0f, 0.0f};
    const glm::vec3 c{hw, 0.0f, length};
    const glm::vec3 d{-hw, 0.0f, length};
    const glm::vec3 e{hw, height, length};
    const glm::vec3 f{-hw, height, length};

    // Sloped top surface (a, b, e, f) wound counter-clockwise seen from above.
    const glm::vec3 slopeNormal = glm::normalize(glm::cross(b - a, f - a));
    {
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({a, slopeNormal, {0.0f, 0.0f}});
        mesh.vertices.push_back({b, slopeNormal, {1.0f, 0.0f}});
        mesh.vertices.push_back({e, slopeNormal, {1.0f, 1.0f}});
        mesh.vertices.push_back({f, slopeNormal, {0.0f, 1.0f}});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    // Bottom, seen from below: reverse order.
    {
        const glm::vec3 normal{0.0f, -1.0f, 0.0f};
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({a, normal, {0.0f, 0.0f}});
        mesh.vertices.push_back({d, normal, {0.0f, 1.0f}});
        mesh.vertices.push_back({c, normal, {1.0f, 1.0f}});
        mesh.vertices.push_back({b, normal, {1.0f, 0.0f}});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    // Back wall (the tall end at z = length).
    AddQuad(mesh, {0.0f, height * 0.5f, length}, {hw, 0.0f, 0.0f}, {0.0f, height * 0.5f, 0.0f});

    // Triangular sides.
    AddTriangle(mesh, b, c, e); // +X side
    AddTriangle(mesh, a, f, d); // -X side

    return mesh;
}

} // namespace pred::Primitives
