#pragma once

#include "Engine/Core/Math.h"

#include <bgfx/bgfx.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
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
    // Multiplied into the material's colour, in linear space; the alpha scales its roughness. White and
    // opaque unless something says otherwise, which is every mesh but the ones painted per vertex: a
    // creature's skin, bone, gums and eye sockets are one mesh, told apart by this.
    uint32_t color = 0xffffffffu;

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
    // Instead of the one above, for a mesh whose vertices are rewritten every frame: a creature's skin,
    // bent by its skeleton on the processor and sent again. The indices never change.
    bgfx::DynamicVertexBufferHandle dynamicVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle indexBuffer = BGFX_INVALID_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    AABB bounds;
    std::string name;
    // Enough of the geometry to tell whether a second upload under the same name is the same mesh.
    size_t fingerprint = 0;

    bool IsDynamic() const { return bgfx::isValid(dynamicVertexBuffer); }
    bool IsValid() const
    {
        return (bgfx::isValid(vertexBuffer) || bgfx::isValid(dynamicVertexBuffer)) && bgfx::isValid(indexBuffer);
    }
};

struct MeshHandle
{
    static constexpr uint16_t kInvalid = 0xffff;
    uint16_t index = kInvalid;

    bool IsValid() const { return index != kInvalid; }
    bool operator==(const MeshHandle& other) const = default;
};

// Enough of a mesh to tell whether a second upload under the same name is the same mesh. Exposed so
// a test can check the rule without a renderer to upload through.
size_t MeshFingerprintForTesting(const MeshData& data);

// Owns every uploaded mesh for the lifetime of the renderer.
class MeshLibrary
{
public:
    ~MeshLibrary();

    MeshHandle Upload(const MeshData& data, std::string name);
    // No graphics card behind it: meshes are recorded, with their bounds, and nothing is uploaded.
    // For building a level where nothing will be drawn -- a test that needs the whole map to walk a
    // creature across, and one day a host with no screen.
    void SetHeadless(bool headless) { m_headless = headless; }
    // New geometry for a mesh that already exists, always.
    //
    // Upload skips a mesh whose vertex count and bounds match what is already there, which is right
    // for the editor asking for the same part forty times a second and wrong for geometry that is
    // genuinely rebuilt each time: two different bullet holes wrapped round the same pillar can
    // have the same count and near enough the same bounds, and the second would silently keep the
    // first one's shape. This says "it has changed" rather than asking.
    MeshHandle Replace(MeshHandle handle, const MeshData& data);
    // A mesh whose vertices change every frame: the same count and the same triangles, in new places.
    // Made once, with its triangles, and then only its vertices are sent again, by UpdateDynamic.
    MeshHandle CreateDynamic(const MeshData& data, std::string name);
    // New positions, normals and colours for a dynamic mesh, as many as it was made with. False when
    // the handle is not a dynamic mesh or the count is wrong, which is a mistake and is not drawn.
    bool UpdateDynamic(MeshHandle handle, const std::vector<MeshVertex>& vertices, const AABB& bounds);
    // Gives a mesh back: its buffers are destroyed, its name forgotten, and its slot reused by the next
    // new name. For meshes made for one thing that has gone for good, such as a creature that has been
    // removed, which would otherwise each keep a slot for the rest of the session.
    void Release(MeshHandle handle);
    const Mesh* Get(MeshHandle handle) const;
    size_t Count() const { return m_meshes.size(); }
    const std::vector<Mesh>& All() const { return m_meshes; }

    void Shutdown();

private:
    std::vector<Mesh> m_meshes;
    // Uploads are keyed by name, so asking twice gives back the same buffers rather than another
    // pair. Without it the editor exhausted the renderer in a couple of minutes of dragging.
    std::unordered_map<std::string, uint16_t> m_byName;
    // Slots given back by Release, taken again before the list grows.
    std::vector<uint16_t> m_free;
    bool m_headless = false;
};

} // namespace pred
