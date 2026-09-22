#include "Engine/Render/MeshSimplify.h"

#include <meshoptimizer.h>

#include <algorithm>

namespace pred
{

MeshData SimplifyMesh(const MeshData& mesh, size_t targetTriangles, float maxError, std::vector<uint32_t>& kept)
{
    kept.clear();
    const size_t vertexCount = mesh.vertices.size();
    if (vertexCount == 0 || mesh.indices.size() / 3 <= targetTriangles)
    {
        kept.resize(vertexCount);
        for (size_t v = 0; v < vertexCount; ++v)
        {
            kept[v] = static_cast<uint32_t>(v);
        }
        return mesh;
    }

    // What else should hold its shape besides the positions: which way the surface faces, and its paint.
    constexpr size_t kAttributes = 6;
    std::vector<float> attributes(vertexCount * kAttributes);
    for (size_t v = 0; v < vertexCount; ++v)
    {
        const MeshVertex& vertex = mesh.vertices[v];
        float* out = &attributes[v * kAttributes];
        out[0] = vertex.normal.x;
        out[1] = vertex.normal.y;
        out[2] = vertex.normal.z;
        out[3] = static_cast<float>(vertex.color & 0xFFu) / 255.0f;
        out[4] = static_cast<float>((vertex.color >> 8) & 0xFFu) / 255.0f;
        out[5] = static_cast<float>((vertex.color >> 16) & 0xFFu) / 255.0f;
    }
    const float weights[kAttributes] = {0.4f, 0.4f, 0.4f, 1.5f, 1.5f, 1.5f};

    std::vector<uint32_t> indices(mesh.indices.size());
    float error = 0.0f;
    const size_t count = meshopt_simplifyWithAttributes(
        indices.data(), mesh.indices.data(), mesh.indices.size(), &mesh.vertices[0].position.x, vertexCount,
        sizeof(MeshVertex), attributes.data(), kAttributes * sizeof(float), weights, kAttributes, nullptr,
        targetTriangles * 3, maxError, 0, &error);
    indices.resize(count);

    // Only the vertices still used, in the order they are first used, which is also the order the card
    // reads them fastest in.
    std::vector<uint32_t> remap(vertexCount);
    const size_t unique = meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(), indices.size(), vertexCount);
    MeshData out;
    out.vertices.resize(unique);
    kept.resize(unique);
    for (size_t v = 0; v < vertexCount; ++v)
    {
        if (remap[v] != ~0u)
        {
            out.vertices[remap[v]] = mesh.vertices[v];
            kept[remap[v]] = static_cast<uint32_t>(v);
        }
    }
    out.indices.resize(indices.size());
    meshopt_remapIndexBuffer(out.indices.data(), indices.data(), indices.size(), remap.data());
    meshopt_optimizeVertexCache(out.indices.data(), out.indices.data(), out.indices.size(), unique);
    return out;
}

} // namespace pred
