#include "Engine/Render/Mesh.h"

#include "Engine/Core/Log.h"

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <cstring>

namespace pred
{
namespace
{
bgfx::VertexLayout g_meshLayout;
bool g_meshLayoutInitialized = false;
} // namespace

void MeshVertex::InitLayout()
{
    if (g_meshLayoutInitialized)
    {
        return;
    }
    g_meshLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    g_meshLayoutInitialized = true;
}

const bgfx::VertexLayout& MeshVertex::Layout()
{
    InitLayout();
    return g_meshLayout;
}

void MeshData::Clear()
{
    vertices.clear();
    indices.clear();
}

AABB MeshData::ComputeBounds() const
{
    AABB bounds;
    for (const MeshVertex& vertex : vertices)
    {
        bounds.Expand(vertex.position);
    }
    return bounds;
}

void MeshData::RecalculateNormals()
{
    for (MeshVertex& vertex : vertices)
    {
        vertex.normal = glm::vec3(0.0f);
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const uint32_t i0 = indices[i];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
        {
            continue;
        }
        const glm::vec3& p0 = vertices[i0].position;
        const glm::vec3& p1 = vertices[i1].position;
        const glm::vec3& p2 = vertices[i2].position;
        // Unnormalized cross product weights each face by its area, which is what we want.
        const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
        vertices[i0].normal += faceNormal;
        vertices[i1].normal += faceNormal;
        vertices[i2].normal += faceNormal;
    }
    for (MeshVertex& vertex : vertices)
    {
        const float length = glm::length(vertex.normal);
        vertex.normal = length > 1e-8f ? vertex.normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

void MeshData::Append(const MeshData& other, const glm::mat4& transform)
{
    const auto baseIndex = static_cast<uint32_t>(vertices.size());
    // Normals need the inverse transpose so that non-uniform scale does not skew them.
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

    vertices.reserve(vertices.size() + other.vertices.size());
    for (const MeshVertex& source : other.vertices)
    {
        MeshVertex vertex = source;
        vertex.position = glm::vec3(transform * glm::vec4(source.position, 1.0f));
        vertex.normal = glm::normalize(normalMatrix * source.normal);
        vertices.push_back(vertex);
    }

    indices.reserve(indices.size() + other.indices.size());
    for (const uint32_t index : other.indices)
    {
        indices.push_back(index + baseIndex);
    }
}

MeshLibrary::~MeshLibrary()
{
    Shutdown();
}

MeshHandle MeshLibrary::Upload(const MeshData& data, std::string name)
{
    if (data.vertices.empty() || data.indices.empty())
    {
        PRED_LOG_ERROR(Render, "Mesh '{}' has no geometry ({} vertices, {} indices)", name,
                       data.vertices.size(), data.indices.size());
        return MeshHandle{};
    }
    if (m_meshes.size() >= MeshHandle::kInvalid)
    {
        PRED_LOG_ERROR(Render, "Mesh library is full, cannot upload '{}'", name);
        return MeshHandle{};
    }

    const bgfx::Memory* vertexMemory =
        bgfx::copy(data.vertices.data(), static_cast<uint32_t>(data.vertices.size() * sizeof(MeshVertex)));
    const bgfx::Memory* indexMemory =
        bgfx::copy(data.indices.data(), static_cast<uint32_t>(data.indices.size() * sizeof(uint32_t)));

    Mesh mesh;
    mesh.vertexBuffer = bgfx::createVertexBuffer(vertexMemory, MeshVertex::Layout());
    mesh.indexBuffer = bgfx::createIndexBuffer(indexMemory, BGFX_BUFFER_INDEX32);
    mesh.indexCount = static_cast<uint32_t>(data.indices.size());
    mesh.vertexCount = static_cast<uint32_t>(data.vertices.size());
    mesh.bounds = data.ComputeBounds();
    mesh.name = std::move(name);

    if (!mesh.IsValid())
    {
        PRED_LOG_ERROR(Render, "bgfx rejected buffers for mesh '{}'", mesh.name);
        if (bgfx::isValid(mesh.vertexBuffer))
        {
            bgfx::destroy(mesh.vertexBuffer);
        }
        if (bgfx::isValid(mesh.indexBuffer))
        {
            bgfx::destroy(mesh.indexBuffer);
        }
        return MeshHandle{};
    }

    bgfx::setName(mesh.vertexBuffer, mesh.name.c_str());
    PRED_LOG_DEBUG(Render, "Uploaded mesh '{}': {} vertices, {} triangles", mesh.name, mesh.vertexCount,
                   mesh.indexCount / 3);

    const auto handleIndex = static_cast<uint16_t>(m_meshes.size());
    m_meshes.push_back(std::move(mesh));
    return MeshHandle{handleIndex};
}

const Mesh* MeshLibrary::Get(MeshHandle handle) const
{
    if (!handle.IsValid() || handle.index >= m_meshes.size())
    {
        return nullptr;
    }
    return &m_meshes[handle.index];
}

void MeshLibrary::Shutdown()
{
    for (Mesh& mesh : m_meshes)
    {
        if (bgfx::isValid(mesh.vertexBuffer))
        {
            bgfx::destroy(mesh.vertexBuffer);
            mesh.vertexBuffer = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(mesh.indexBuffer))
        {
            bgfx::destroy(mesh.indexBuffer);
            mesh.indexBuffer = BGFX_INVALID_HANDLE;
        }
    }
    m_meshes.clear();
}

} // namespace pred
