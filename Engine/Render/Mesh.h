#pragma once

#include "Engine/Core/Math.h"

#include <bgfx/bgfx.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// The single static-mesh vertex format. Skinned meshes get their own format when the animation
// system lands; keeping one layout until then avoids speculative machinery.
struct MeshVertex
{
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};

    static const bgfx::VertexLayout& Layout();
    static void InitLayout();
};

// CPU-side geometry, before upload. Primitive builders and (later) the glTF importer produce these.
struct MeshData
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;

    void Clear();
    AABB ComputeBounds() const;
    // Averages face normals into vertices. Only needed for generators that do not emit normals.
    void RecalculateNormals();
    // Appends another mesh, offsetting its indices. Used to bake static geometry into one buffer.
    void Append(const MeshData& other, const glm::mat4& transform);

    size_t TriangleCount() const { return indices.size() / 3; }
};

// GPU geometry. Owned by MeshLibrary.
struct Mesh
{
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle indexBuffer = BGFX_INVALID_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    AABB bounds;
    std::string name;

    bool IsValid() const { return bgfx::isValid(vertexBuffer) && bgfx::isValid(indexBuffer); }
};

struct MeshHandle
{
    static constexpr uint16_t kInvalid = 0xffff;
    uint16_t index = kInvalid;

    bool IsValid() const { return index != kInvalid; }
    bool operator==(const MeshHandle& other) const = default;
};

// Owns every uploaded mesh for the lifetime of the renderer.
class MeshLibrary
{
public:
    ~MeshLibrary();

    MeshHandle Upload(const MeshData& data, std::string name);
    const Mesh* Get(MeshHandle handle) const;
    size_t Count() const { return m_meshes.size(); }
    const std::vector<Mesh>& All() const { return m_meshes; }

    void Shutdown();

private:
    std::vector<Mesh> m_meshes;
};

} // namespace pred
