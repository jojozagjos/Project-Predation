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

namespace
{

// Enough of a mesh to tell whether it is the same one as last time. Counts and bounds, because the
// things that ask for the same name twice are re-uploading a part that has either not changed at
// all or has been resized, and both of those show up here. Hashing every vertex would be exact and
// would cost more than the upload it is trying to avoid.
size_t MeshFingerprintImpl(const MeshData& data)
{
    const AABB bounds = data.ComputeBounds();
    size_t hash = data.vertices.size() * 1000003ull + data.indices.size();
    const auto mix = [&hash](float value)
    {
        hash ^= static_cast<size_t>(static_cast<long long>(value * 100000.0f)) + 0x9e3779b97f4a7c15ull +
                (hash << 6) + (hash >> 2);
    };
    for (int axis = 0; axis < 3; ++axis)
    {
        mix(bounds.min[axis]);
        mix(bounds.max[axis]);
    }
    return hash;
}

} // namespace

size_t MeshFingerprintForTesting(const MeshData& data)
{
    return MeshFingerprintImpl(data);
}

MeshHandle MeshLibrary::Upload(const MeshData& data, std::string name)
{
    if (data.vertices.empty() || data.indices.empty())
    {
        PRED_LOG_ERROR(Render, "Mesh '{}' has no geometry ({} vertices, {} indices)", name,
                       data.vertices.size(), data.indices.size());
        return MeshHandle{};
    }

    // The same name twice is the same mesh, and it is asked for constantly: the editor rebuilds its
    // preview on every change, and every rebuild used to take four more buffer handles and never
    // give any back. Four thousand of them is a couple of minutes of dragging a socket, and after
    // that nothing can be uploaded at all and the model quietly disappears.
    const size_t fingerprint = MeshFingerprintImpl(data);
    if (const auto found = m_byName.find(name); found != m_byName.end())
    {
        Mesh& existing = m_meshes[found->second];
        if (existing.fingerprint == fingerprint && existing.IsValid())
        {
            return MeshHandle{found->second};
        }
        // Same name, different geometry: a part that has been resized. The buffers are replaced
        // where they are, so everything already holding the handle keeps working.
        if (bgfx::isValid(existing.vertexBuffer))
        {
            bgfx::destroy(existing.vertexBuffer);
        }
        if (bgfx::isValid(existing.indexBuffer))
        {
            bgfx::destroy(existing.indexBuffer);
        }
        existing = Mesh{};
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
    mesh.fingerprint = fingerprint;
    PRED_LOG_DEBUG(Render, "Uploaded mesh '{}': {} vertices, {} triangles", mesh.name, mesh.vertexCount,
                   mesh.indexCount / 3);

    // Back into the slot the name already had, when it had one, so every handle handed out before
    // still points at the right thing.
    if (const auto found = m_byName.find(mesh.name); found != m_byName.end())
    {
        const uint16_t index = found->second;
        m_meshes[index] = std::move(mesh);
        return MeshHandle{index};
    }

    const auto handleIndex = static_cast<uint16_t>(m_meshes.size());
    m_byName.emplace(mesh.name, handleIndex);
    m_meshes.push_back(std::move(mesh));
    return MeshHandle{handleIndex};
}

MeshHandle MeshLibrary::Replace(MeshHandle handle, const MeshData& data)
{
    if (!handle.IsValid() || handle.index >= m_meshes.size())
    {
        return MeshHandle{};
    }
    // A fingerprint that cannot match, so Upload replaces the buffers rather than deciding this is
    // the mesh it already has. Same slot, same name, so every handle already out keeps working.
    Mesh& existing = m_meshes[handle.index];
    existing.fingerprint = MeshFingerprintImpl(data) ^ 1u;
    return Upload(data, existing.name);
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
    m_byName.clear();
}

} // namespace pred
