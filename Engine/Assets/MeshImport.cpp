#include "Engine/Assets/MeshImport.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred
{
namespace
{

// One corner of an OBJ face: indices into the position, texture and normal arrays. OBJ writes these
// as "v", "v/vt", "v//vn" or "v/vt/vn", and every exporter picks a different one.
struct FaceCorner
{
    int position = 0;
    int uv = 0;
    int normal = 0;

    bool operator==(const FaceCorner& other) const = default;
};

struct FaceCornerHash
{
    size_t operator()(const FaceCorner& corner) const
    {
        return (static_cast<size_t>(corner.position) * 73856093u) ^
               (static_cast<size_t>(corner.uv) * 19349663u) ^ (static_cast<size_t>(corner.normal) * 83492791u);
    }
};

// OBJ indices are one-based and may be negative, which counts back from the end of the list so far.
int ResolveIndex(int value, size_t count)
{
    if (value > 0)
    {
        return value - 1;
    }
    if (value < 0)
    {
        return static_cast<int>(count) + value;
    }
    return -1;
}

FaceCorner ParseCorner(const std::string& token)
{
    FaceCorner corner;
    int field = 0;
    size_t start = 0;
    while (start <= token.size() && field < 3)
    {
        const size_t slash = token.find('/', start);
        const size_t end = slash == std::string::npos ? token.size() : slash;
        if (end > start)
        {
            int value = 0;
            const char* first = token.data() + start;
            std::from_chars(first, token.data() + end, value);
            (field == 0 ? corner.position : field == 1 ? corner.uv : corner.normal) = value;
        }
        if (slash == std::string::npos)
        {
            break;
        }
        start = slash + 1;
        ++field;
    }
    return corner;
}

} // namespace

bool LoadObjMesh(const std::filesystem::path& file, MeshData& out)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_ERROR(Asset, "Could not open mesh {}", file.string());
        return false;
    }

    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    MeshData mesh;
    std::unordered_map<FaceCorner, uint32_t, FaceCornerHash> lookup;

    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream parts(line);
        std::string keyword;
        parts >> keyword;

        if (keyword == "v")
        {
            glm::vec3 value{0.0f};
            parts >> value.x >> value.y >> value.z;
            positions.push_back(value);
        }
        else if (keyword == "vn")
        {
            glm::vec3 value{0.0f, 1.0f, 0.0f};
            parts >> value.x >> value.y >> value.z;
            normals.push_back(value);
        }
        else if (keyword == "f")
        {
            // Fanned from the first corner, which is correct for any convex face and is what every
            // other importer does with the concave ones too.
            std::vector<uint32_t> corners;
            std::string token;
            while (parts >> token)
            {
                const FaceCorner corner = ParseCorner(token);
                const auto existing = lookup.find(corner);
                if (existing != lookup.end())
                {
                    corners.push_back(existing->second);
                    continue;
                }

                MeshVertex vertex;
                const int positionIndex = ResolveIndex(corner.position, positions.size());
                if (positionIndex < 0 || positionIndex >= static_cast<int>(positions.size()))
                {
                    continue;
                }
                vertex.position = positions[static_cast<size_t>(positionIndex)];
                const int normalIndex = ResolveIndex(corner.normal, normals.size());
                if (normalIndex >= 0 && normalIndex < static_cast<int>(normals.size()))
                {
                    vertex.normal = normals[static_cast<size_t>(normalIndex)];
                }

                const auto index = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
                lookup.emplace(corner, index);
                corners.push_back(index);
            }

            for (size_t i = 2; i < corners.size(); ++i)
            {
                mesh.indices.push_back(corners[0]);
                mesh.indices.push_back(corners[i - 1]);
                mesh.indices.push_back(corners[i]);
            }
        }
    }

    if (mesh.indices.empty())
    {
        PRED_LOG_ERROR(Asset, "Mesh {} contained no triangles", file.string());
        return false;
    }
    if (normals.empty())
    {
        mesh.RecalculateNormals();
    }

    PRED_LOG_INFO(Asset, "Imported {}: {} vertices, {} triangles", file.string(), mesh.vertices.size(),
                  mesh.TriangleCount());
    out = std::move(mesh);
    return true;
}

float NormalizeMeshScale(MeshData& mesh, float targetSize)
{
    if (mesh.vertices.empty() || targetSize <= 0.0f)
    {
        return 1.0f;
    }
    const AABB bounds = mesh.ComputeBounds();
    const glm::vec3 extent = bounds.max - bounds.min;
    const float longest = std::max({extent.x, extent.y, extent.z});
    if (longest < 1e-6f)
    {
        return 1.0f;
    }

    const float scale = targetSize / longest;
    const glm::vec3 centre = (bounds.min + bounds.max) * 0.5f;
    for (MeshVertex& vertex : mesh.vertices)
    {
        vertex.position = (vertex.position - centre) * scale;
    }
    return scale;
}

} // namespace pred
