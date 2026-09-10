#include "Engine/Assets/GltfImport.h"

#include "Engine/Assets/MeshImport.h"
#include "Engine/Core/Log.h"
#include "Engine/Render/TextureLibrary.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <unordered_map>

namespace pred
{
namespace
{

// The container. A twelve byte header, then chunks of [length][type][payload], of which exactly two
// matter: the JSON that describes the model and the binary blob the accessors index into.
constexpr uint32_t kMagic = 0x46546C67u;    // 'glTF'
constexpr uint32_t kChunkJson = 0x4E4F534Au; // 'JSON'
constexpr uint32_t kChunkBin = 0x004E4942u;  // 'BIN\0'

// Accessor component types, from the specification. Only the ones an index or a vertex attribute
// can legally use are handled; anything else is a file this importer has no business reading.
constexpr int kByte = 5120;
constexpr int kUnsignedByte = 5121;
constexpr int kShort = 5122;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedInt = 5125;
constexpr int kFloat = 5126;

uint32_t ReadUint32(const std::vector<uint8_t>& bytes, size_t at)
{
    if (at + 4 > bytes.size())
    {
        return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, 4); // glTF is little endian, and so is everything we run on
    return value;
}

int ComponentCount(const std::string& type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

int ComponentSize(int componentType)
{
    switch (componentType)
    {
    case kByte:
    case kUnsignedByte:
        return 1;
    case kShort:
    case kUnsignedShort:
        return 2;
    case kUnsignedInt:
    case kFloat:
        return 4;
    default:
        return 0;
    }
}

// Everything an accessor needs to be walked: where its data starts, how far apart its elements are,
// and how to turn one element into numbers.
struct Accessor
{
    const uint8_t* data = nullptr;
    size_t count = 0;
    size_t stride = 0;
    int componentType = kFloat;
    int components = 0;
    bool normalized = false;
    bool valid = false;
};

Accessor ResolveAccessor(const nlohmann::json& document, const std::vector<uint8_t>& binary, int index)
{
    Accessor out;
    const auto& accessors = document["accessors"];
    if (index < 0 || static_cast<size_t>(index) >= accessors.size())
    {
        return out;
    }
    const nlohmann::json& accessor = accessors[static_cast<size_t>(index)];

    out.count = accessor.value("count", 0);
    out.componentType = accessor.value("componentType", kFloat);
    out.components = ComponentCount(accessor.value("type", std::string("SCALAR")));
    out.normalized = accessor.value("normalized", false);
    const int elementSize = ComponentSize(out.componentType) * out.components;
    if (out.count == 0 || elementSize == 0)
    {
        return out;
    }

    // A sparse accessor stores a base of zeros with a list of overrides. Rare, and nothing that
    // exports a weapon writes one, so it is refused rather than half-read.
    if (accessor.contains("sparse"))
    {
        return out;
    }

    const auto viewIt = accessor.find("bufferView");
    if (viewIt == accessor.end())
    {
        return out; // legal, and means every element is zero, which is of no use here
    }

    const auto& views = document["bufferViews"];
    const size_t viewIndex = viewIt->get<size_t>();
    if (viewIndex >= views.size())
    {
        return out;
    }
    const nlohmann::json& view = views[viewIndex];

    const size_t viewOffset = view.value("byteOffset", size_t{0});
    const size_t viewLength = view.value("byteLength", size_t{0});
    const size_t accessorOffset = accessor.value("byteOffset", size_t{0});
    out.stride = view.value("byteStride", size_t{0});
    if (out.stride == 0)
    {
        out.stride = static_cast<size_t>(elementSize); // tightly packed
    }

    const size_t start = viewOffset + accessorOffset;
    const size_t needed = out.stride * (out.count - 1) + static_cast<size_t>(elementSize);
    if (start + needed > binary.size() || accessorOffset + needed > viewLength + accessorOffset)
    {
        return out;
    }

    out.data = binary.data() + start;
    out.valid = true;
    return out;
}

float ReadComponent(const Accessor& accessor, size_t element, int component)
{
    const uint8_t* at = accessor.data + accessor.stride * element +
                        static_cast<size_t>(ComponentSize(accessor.componentType) * component);
    switch (accessor.componentType)
    {
    case kFloat:
    {
        float value = 0.0f;
        std::memcpy(&value, at, 4);
        return value;
    }
    case kUnsignedShort:
    {
        uint16_t value = 0;
        std::memcpy(&value, at, 2);
        return accessor.normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
    }
    case kShort:
    {
        int16_t value = 0;
        std::memcpy(&value, at, 2);
        return accessor.normalized ? std::max(static_cast<float>(value) / 32767.0f, -1.0f)
                                   : static_cast<float>(value);
    }
    case kUnsignedByte:
        return accessor.normalized ? static_cast<float>(*at) / 255.0f : static_cast<float>(*at);
    case kByte:
    {
        const auto value = static_cast<int8_t>(*at);
        return accessor.normalized ? std::max(static_cast<float>(value) / 127.0f, -1.0f)
                                   : static_cast<float>(value);
    }
    case kUnsignedInt:
    {
        uint32_t value = 0;
        std::memcpy(&value, at, 4);
        return static_cast<float>(value);
    }
    default:
        return 0.0f;
    }
}

glm::vec3 ReadVec3(const Accessor& accessor, size_t element)
{
    return {ReadComponent(accessor, element, 0), ReadComponent(accessor, element, 1),
            ReadComponent(accessor, element, 2)};
}

glm::mat4 NodeMatrix(const nlohmann::json& node)
{
    // A node carries either a full matrix or a translation, rotation and scale. Both are allowed and
    // exporters differ, so both are read.
    if (const auto it = node.find("matrix"); it != node.end() && it->size() == 16)
    {
        glm::mat4 matrix{1.0f};
        float values[16] = {};
        for (size_t i = 0; i < 16; ++i)
        {
            values[i] = it->at(i).get<float>();
        }
        std::memcpy(glm::value_ptr(matrix), values, sizeof(values)); // both are column major
        return matrix;
    }

    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    if (const auto it = node.find("translation"); it != node.end() && it->size() == 3)
    {
        translation = {it->at(0).get<float>(), it->at(1).get<float>(), it->at(2).get<float>()};
    }
    if (const auto it = node.find("rotation"); it != node.end() && it->size() == 4)
    {
        // glTF stores a quaternion as x, y, z, w; glm's constructor takes w first.
        rotation = glm::quat(it->at(3).get<float>(), it->at(0).get<float>(), it->at(1).get<float>(),
                             it->at(2).get<float>());
    }
    if (const auto it = node.find("scale"); it != node.end() && it->size() == 3)
    {
        scale = {it->at(0).get<float>(), it->at(1).get<float>(), it->at(2).get<float>()};
    }
    return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) *
           glm::scale(glm::mat4(1.0f), scale);
}

std::string PartName(const nlohmann::json& document, const nlohmann::json& node, size_t primitive,
                     int material)
{
    // A name a person can find in a list. The node's name first, because that is what the person who
    // made the model called it; the material's name next, because Sketchfab exports call every node
    // "Object_3" and the materials are the only thing telling the pieces apart.
    std::string name = node.value("name", std::string());
    if (material >= 0 && document.contains("materials"))
    {
        const auto& materials = document["materials"];
        if (static_cast<size_t>(material) < materials.size())
        {
            const std::string materialName = materials[static_cast<size_t>(material)].value("name", "");
            if (!materialName.empty())
            {
                name = name.empty() ? materialName : name + "_" + materialName;
            }
        }
    }
    if (name.empty())
    {
        name = "part";
    }
    if (primitive > 0)
    {
        name += "_" + std::to_string(primitive);
    }
    return name;
}

// Writes the base colour image a material names out beside the model, and returns what to call it.
//
// glTF embeds images in the binary chunk as PNG or JPEG. They are written out rather than carried in
// the model file, because a model file is meant to stay something a person can open and a base
// colour image is two megabytes of it. Decoded and re-encoded rather than copied, so a JPEG arrives
// as a PNG like everything else and the loader has one format to know about.
//
// Returns an empty string when the material has no base colour texture, which is a normal thing for
// a material to be missing and not a fault.
std::string ExtractBaseColorTexture(const nlohmann::json& document, const std::vector<uint8_t>& binary,
                                    int material, const GltfImportOptions& options,
                                    std::unordered_map<int, std::string>& written)
{
    if (material < 0 || options.textureDirectory.empty() || !document.contains("materials"))
    {
        return {};
    }
    const auto& materials = document["materials"];
    if (static_cast<size_t>(material) >= materials.size())
    {
        return {};
    }
    const auto pbr = materials[static_cast<size_t>(material)].find("pbrMetallicRoughness");
    if (pbr == materials[static_cast<size_t>(material)].end())
    {
        return {};
    }
    const auto baseColor = pbr->find("baseColorTexture");
    if (baseColor == pbr->end())
    {
        return {};
    }

    const int textureIndex = baseColor->value("index", -1);
    if (textureIndex < 0 || !document.contains("textures"))
    {
        return {};
    }
    const auto& textures = document["textures"];
    if (static_cast<size_t>(textureIndex) >= textures.size())
    {
        return {};
    }
    const int imageIndex = textures[static_cast<size_t>(textureIndex)].value("source", -1);
    if (imageIndex < 0 || !document.contains("images"))
    {
        return {};
    }

    // The same image is usually named by several materials. Written once.
    if (const auto found = written.find(imageIndex); found != written.end())
    {
        return found->second;
    }

    const auto& images = document["images"];
    if (static_cast<size_t>(imageIndex) >= images.size())
    {
        return {};
    }
    const nlohmann::json& image = images[static_cast<size_t>(imageIndex)];

    // Only images inside the file. A glTF may point at one beside it instead, which this importer
    // does not read for the same reason it only reads .glb: one file in, one model out.
    const auto viewIt = image.find("bufferView");
    if (viewIt == image.end() || !document.contains("bufferViews"))
    {
        return {};
    }
    const auto& views = document["bufferViews"];
    const size_t viewIndex = viewIt->get<size_t>();
    if (viewIndex >= views.size())
    {
        return {};
    }
    const nlohmann::json& view = views[viewIndex];
    const size_t offset = view.value("byteOffset", size_t{0});
    const size_t length = view.value("byteLength", size_t{0});
    if (length == 0 || offset + length > binary.size())
    {
        return {};
    }

    ImageData decoded;
    if (!DecodeImage(binary.data() + offset, length, decoded))
    {
        return {};
    }

    std::error_code ec;
    std::filesystem::create_directories(options.textureDirectory, ec);
    const std::string name =
        (options.texturePrefix.empty() ? std::string("texture") : options.texturePrefix) + "_" +
        std::to_string(imageIndex) + ".png";
    const std::filesystem::path path = options.textureDirectory / name;
    if (!WritePng(path.string(), decoded))
    {
        PRED_LOG_WARN(Asset, "Could not write the texture {}", path.string());
        return {};
    }
    PRED_LOG_INFO(Asset, "Wrote {} ({} by {})", path.string(), decoded.width, decoded.height);

    // Named relative to the assets root, so the model file does not carry this machine's paths.
    const std::string relative = "Models/Textures/" + name;
    written.emplace(imageIndex, relative);
    return relative;
}


void ApplyMaterial(const nlohmann::json& document, int material, ModelPart& part)
{
    // Grey, for a primitive with no material at all. A material that exists but says nothing about
    // its colour is white by the specification, and white is what a textured part needs, because
    // the factor is multiplied into the image.
    part.color = {0.62f, 0.63f, 0.66f};
    part.roughness = 0.55f;
    part.metallic = 0.25f;
    if (material < 0 || !document.contains("materials"))
    {
        return;
    }
    part.color = glm::vec3(1.0f);
    const auto& materials = document["materials"];
    if (static_cast<size_t>(material) >= materials.size())
    {
        return;
    }
    const nlohmann::json& entry = materials[static_cast<size_t>(material)];
    const auto pbr = entry.find("pbrMetallicRoughness");
    if (pbr == entry.end())
    {
        return;
    }
    // The factor only. A base colour texture is ignored, because nothing downstream samples one, and
    // a model whose factor is white then arrives white rather than arriving invisible.
    if (const auto it = pbr->find("baseColorFactor"); it != pbr->end() && it->size() >= 3)
    {
        part.color = {it->at(0).get<float>(), it->at(1).get<float>(), it->at(2).get<float>()};
    }
    part.roughness = pbr->value("roughnessFactor", 0.55f);
    part.metallic = pbr->value("metallicFactor", 0.25f);
}

// Reads one primitive into a part, in the frame the node puts it in.
bool ReadPrimitive(const nlohmann::json& document, const std::vector<uint8_t>& binary,
                   const nlohmann::json& primitive, const glm::mat4& world, MeshData& out)
{
    // Mode 4 is triangles. A file that draws strips or fans is legal and nothing exports one for a
    // prop, so it is skipped rather than guessed at.
    if (primitive.value("mode", 4) != 4)
    {
        return false;
    }
    const auto attributes = primitive.find("attributes");
    if (attributes == primitive.end())
    {
        return false;
    }
    const auto positionIt = attributes->find("POSITION");
    if (positionIt == attributes->end())
    {
        return false;
    }

    const Accessor positions = ResolveAccessor(document, binary, positionIt->get<int>());
    if (!positions.valid || positions.components < 3)
    {
        return false;
    }

    Accessor normals;
    if (const auto it = attributes->find("NORMAL"); it != attributes->end())
    {
        normals = ResolveAccessor(document, binary, it->get<int>());
    }
    Accessor uvs;
    if (const auto it = attributes->find("TEXCOORD_0"); it != attributes->end())
    {
        uvs = ResolveAccessor(document, binary, it->get<int>());
    }

    // Normals do not transform by the same matrix positions do, or a model with a non-uniform scale
    // comes out lit as though it were a different shape.
    const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(world));

    const uint32_t base = static_cast<uint32_t>(out.vertices.size());
    out.vertices.reserve(out.vertices.size() + positions.count);
    for (size_t i = 0; i < positions.count; ++i)
    {
        MeshVertex vertex;
        vertex.position = glm::vec3(world * glm::vec4(ReadVec3(positions, i), 1.0f));
        if (normals.valid && normals.components >= 3 && i < normals.count)
        {
            const glm::vec3 normal = normalMatrix * ReadVec3(normals, i);
            vertex.normal = glm::length(normal) > 1e-6f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
        if (uvs.valid && uvs.components >= 2 && i < uvs.count)
        {
            vertex.uv = {ReadComponent(uvs, i, 0), ReadComponent(uvs, i, 1)};
        }
        out.vertices.push_back(vertex);
    }

    if (const auto it = primitive.find("indices"); it != primitive.end())
    {
        const Accessor indices = ResolveAccessor(document, binary, it->get<int>());
        if (!indices.valid)
        {
            out.vertices.resize(base);
            return false;
        }
        out.indices.reserve(out.indices.size() + indices.count);
        for (size_t i = 0; i < indices.count; ++i)
        {
            out.indices.push_back(base + static_cast<uint32_t>(ReadComponent(indices, i, 0)));
        }
    }
    else
    {
        // No index buffer means the vertices are already in triangle order.
        for (size_t i = 0; i < positions.count; ++i)
        {
            out.indices.push_back(base + static_cast<uint32_t>(i));
        }
    }

    if (!normals.valid)
    {
        out.RecalculateNormals();
    }
    return out.indices.size() >= 3;
}

// Walks the node tree, accumulating transforms, and calls `visit` for every node that has a mesh.
void VisitNodes(const nlohmann::json& document, int nodeIndex, const glm::mat4& parent, int depth,
                const std::function<void(const nlohmann::json&, const glm::mat4&)>& visit)
{
    // A malformed file can name itself as its own child. Depth is what stops that being a hang.
    if (depth > 64 || !document.contains("nodes"))
    {
        return;
    }
    const auto& nodes = document["nodes"];
    if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= nodes.size())
    {
        return;
    }
    const nlohmann::json& node = nodes[static_cast<size_t>(nodeIndex)];
    const glm::mat4 world = parent * NodeMatrix(node);

    if (node.contains("mesh"))
    {
        visit(node, world);
    }
    if (const auto it = node.find("children"); it != node.end())
    {
        for (const auto& child : *it)
        {
            VisitNodes(document, child.get<int>(), world, depth + 1, visit);
        }
    }
}

} // namespace

bool IsGltfFile(const std::filesystem::path& file)
{
    std::string extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".glb" || extension == ".gltf";
}

bool LoadGlbModel(const std::filesystem::path& file, const GltfImportOptions& options, ModelAsset& out,
                  std::string* error)
{
    const auto fail = [&](std::string message)
    {
        PRED_LOG_ERROR(Asset, "glTF import failed for {}: {}", file.string(), message);
        if (error != nullptr)
        {
            *error = std::move(message);
        }
        return false;
    };

    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        return fail("cannot open the file");
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < 20)
    {
        return fail("file is too short to be a glb");
    }
    if (ReadUint32(bytes, 0) != kMagic)
    {
        return fail("not a glb: this importer reads the binary form, not .gltf with separate files");
    }

    // Chunks, in order, until the file runs out. The specification says JSON comes first and BIN
    // second, but reading whatever is actually there costs nothing and is one fewer assumption.
    std::string json;
    std::vector<uint8_t> binary;
    size_t at = 12;
    while (at + 8 <= bytes.size())
    {
        const uint32_t length = ReadUint32(bytes, at);
        const uint32_t type = ReadUint32(bytes, at + 4);
        const size_t payload = at + 8;
        if (payload + length > bytes.size())
        {
            break;
        }
        if (type == kChunkJson)
        {
            json.assign(reinterpret_cast<const char*>(bytes.data() + payload), length);
        }
        else if (type == kChunkBin)
        {
            binary.assign(bytes.begin() + static_cast<ptrdiff_t>(payload),
                          bytes.begin() + static_cast<ptrdiff_t>(payload + length));
        }
        at = payload + length;
        at += (4 - (at % 4)) % 4; // chunks are four byte aligned
    }

    if (json.empty())
    {
        return fail("no JSON chunk");
    }

    nlohmann::json document;
    try
    {
        document = nlohmann::json::parse(json);
    }
    catch (const nlohmann::json::exception& ex)
    {
        return fail(std::string("malformed JSON: ") + ex.what());
    }

    if (!document.contains("meshes") || !document.contains("accessors") ||
        !document.contains("bufferViews"))
    {
        return fail("no geometry in the file");
    }

    // Everything is turned once at import rather than left for the editor to do part by part. glTF
    // has no idea which way a weapon points and a model arrives however its author left it.
    const glm::vec3 radians = glm::radians(options.rotationDegrees);
    const glm::mat4 correction = glm::eulerAngleXYZ(radians.x, radians.y, radians.z);

    ModelAsset built;
    built.name = file.stem().string();

    // Which images have already been written out, keyed by their index in the file. The same image
    // is usually named by several materials and there is no reason to decode it more than once.
    std::unordered_map<int, std::string> writtenTextures;

    const auto addNode = [&](const nlohmann::json& node, const glm::mat4& world)
    {
        const auto& meshes = document["meshes"];
        const size_t meshIndex = node["mesh"].get<size_t>();
        if (meshIndex >= meshes.size())
        {
            return;
        }
        const nlohmann::json& mesh = meshes[meshIndex];
        const auto primitives = mesh.find("primitives");
        if (primitives == mesh.end())
        {
            return;
        }
        for (size_t i = 0; i < primitives->size(); ++i)
        {
            const nlohmann::json& primitive = primitives->at(i);
            const int material = primitive.value("material", -1);

            ModelPart part;
            part.shape = PartShape::Mesh;
            part.sourceFile = file.filename().string();
            part.size = glm::vec3(1.0f);
            if (!ReadPrimitive(document, binary, primitive, correction * world, part.mesh))
            {
                continue;
            }
            part.name = PartName(document, node, i, material);
            ApplyMaterial(document, material, part);
            part.texture =
                ExtractBaseColorTexture(document, binary, material, options, writtenTextures);
            built.parts.push_back(std::move(part));
        }
    };

    const auto& scenes = document.contains("scenes") ? document["scenes"] : nlohmann::json::array();
    const size_t sceneIndex = document.value("scene", size_t{0});
    if (sceneIndex < scenes.size() && scenes[sceneIndex].contains("nodes"))
    {
        for (const auto& root : scenes[sceneIndex]["nodes"])
        {
            VisitNodes(document, root.get<int>(), glm::mat4(1.0f), 0, addNode);
        }
    }
    else if (document.contains("nodes"))
    {
        // No scene, which is legal. Every node is treated as a root, which is what a viewer does.
        for (size_t i = 0; i < document["nodes"].size(); ++i)
        {
            VisitNodes(document, static_cast<int>(i), glm::mat4(1.0f), 0, addNode);
        }
    }

    if (built.parts.empty())
    {
        return fail("no triangles: the file may use strips, skinning or a compression extension");
    }

    // Scaled and centred as one thing, not part by part, or the pieces come apart.
    AABB bounds;
    bool first = true;
    for (const ModelPart& part : built.parts)
    {
        const AABB partBounds = part.mesh.ComputeBounds();
        if (first)
        {
            bounds = partBounds;
            first = false;
        }
        else
        {
            bounds.min = glm::min(bounds.min, partBounds.min);
            bounds.max = glm::max(bounds.max, partBounds.max);
        }
    }

    const glm::vec3 extent = bounds.max - bounds.min;
    const float longest = std::max({extent.x, extent.y, extent.z});
    const float scale = longest > 1e-5f && options.targetSize > 0.0f ? options.targetSize / longest : 1.0f;
    const glm::vec3 centre = options.centre ? (bounds.min + bounds.max) * 0.5f : glm::vec3(0.0f);
    for (ModelPart& part : built.parts)
    {
        for (MeshVertex& vertex : part.mesh.vertices)
        {
            vertex.position = (vertex.position - centre) * scale;
        }
    }

    // A first guess at where the hands and the muzzle go, from the shape itself.
    //
    // Without sockets a model is held by its origin, which for anything downloaded is the middle of
    // the receiver: the hand ends up inside the weapon and the barrel points out of the wrist. These
    // are guesses and they are meant to be moved in the editor, but a guess taken from the geometry
    // is close enough to see what needs changing, which nothing at the origin ever is.
    {
        glm::vec3 low{1e9f};
        glm::vec3 high{-1e9f};
        for (const ModelPart& part : built.parts)
        {
            for (const MeshVertex& vertex : part.mesh.vertices)
            {
                low = glm::min(low, vertex.position);
                high = glm::max(high, vertex.position);
            }
        }

        // Where the barrel sits vertically, taken from the front of the model rather than assumed:
        // on a rifle the bore is near the top, on a pistol it is above the slide, and on a shotgun
        // it is somewhere else again.
        const float front = glm::mix(high.z, low.z, 0.15f);
        float barrelY = (low.y + high.y) * 0.5f;
        {
            double total = 0.0;
            size_t counted = 0;
            for (const ModelPart& part : built.parts)
            {
                for (const MeshVertex& vertex : part.mesh.vertices)
                {
                    if (vertex.position.z >= front)
                    {
                        total += vertex.position.y;
                        ++counted;
                    }
                }
            }
            if (counted > 0)
            {
                barrelY = static_cast<float>(total / static_cast<double>(counted));
            }
        }

        const float height = high.y - low.y;
        const float length = high.z - low.z;
        const bool longArm = length > height * 1.9f; // a rifle is long for its height; a pistol is not

        const auto socket = [&](const char* socketName, const glm::vec3& position)
        {
            ModelSocket entry;
            entry.name = socketName;
            entry.position = position;
            built.sockets.push_back(entry);
        };

        // A rifle's pistol grip sits just behind the magazine well, about a fifth of the way along
        // from the back; a pistol's is most of the way back and most of the way down.
        socket("grip", {0.0f, low.y + height * (longArm ? 0.30f : 0.42f),
                        low.z + length * (longArm ? 0.22f : 0.30f)});
        socket("support", {0.0f, barrelY - height * 0.14f,
                           low.z + length * (longArm ? 0.66f : 0.58f)});
        socket("muzzle", {0.0f, barrelY, high.z});
        socket("sight", {0.0f, high.y, low.z + length * (longArm ? 0.45f : 0.6f)});
        socket("magazine", {0.0f, low.y + height * 0.2f,
                            low.z + length * (longArm ? 0.34f : 0.28f)});

        // A rifle carries its magazine and its grip behind the middle, and a pistol carries its own
        // grip further back still, so the lowest part of a weapon is behind its centre. When it is
        // not, the model is almost certainly facing the wrong way, and saying so beats leaving
        // someone to work out why the thing is held by its muzzle.
        double lowestAlong = 0.0;
        size_t lowest = 0;
        for (const ModelPart& part : built.parts)
        {
            for (const MeshVertex& vertex : part.mesh.vertices)
            {
                if (vertex.position.y < low.y + height * 0.06f)
                {
                    lowestAlong += vertex.position.z;
                    ++lowest;
                }
            }
        }
        if (lowest > 0 && lowestAlong / static_cast<double>(lowest) > length * 0.02)
        {
            PRED_LOG_WARN(Asset,
                          "{} looks like it is facing backwards: its lowest parts, which on a "
                          "weapon are the grip and the magazine, are in front of its middle. Turn "
                          "it around in the editor, or import it with the opposite turn.",
                          file.filename().string());
        }
    }

    size_t triangles = 0;
    for (const ModelPart& part : built.parts)
    {
        triangles += part.mesh.TriangleCount();
    }
    PRED_LOG_INFO(Asset, "Imported {}: {} parts, {} triangles, scaled by {:.4f}", file.filename().string(),
                  built.parts.size(), triangles, scale);

    out = std::move(built);
    return true;
}

} // namespace pred
