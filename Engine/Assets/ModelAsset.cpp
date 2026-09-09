#include "Engine/Assets/ModelAsset.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Primitives.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <fstream>

namespace pred
{
namespace
{

glm::vec3 ReadVec3(const nlohmann::json& node, const glm::vec3& fallback)
{
    if (!node.is_array() || node.size() != 3)
    {
        return fallback;
    }
    return glm::vec3(node[0].get<float>(), node[1].get<float>(), node[2].get<float>());
}

nlohmann::json WriteVec3(const glm::vec3& value)
{
    return nlohmann::json::array({value.x, value.y, value.z});
}

template <typename T>
void ReadField(const nlohmann::json& node, const char* key, T& target)
{
    if (const auto it = node.find(key); it != node.end() && !it->is_null())
    {
        target = it->get<T>();
    }
}

glm::mat4 EulerMatrix(const glm::vec3& degrees)
{
    // X then Y then Z, which is the order the editor's three fields read in.
    return glm::rotate(glm::mat4(1.0f), glm::radians(degrees.z), glm::vec3(0.0f, 0.0f, 1.0f)) *
           glm::rotate(glm::mat4(1.0f), glm::radians(degrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
           glm::rotate(glm::mat4(1.0f), glm::radians(degrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
}

} // namespace

const char* PartShapeName(PartShape shape)
{
    switch (shape)
    {
    case PartShape::Cylinder:
        return "cylinder";
    case PartShape::Sphere:
        return "sphere";
    case PartShape::Mesh:
        return "mesh";
    case PartShape::Box:
    default:
        return "box";
    }
}

PartShape PartShapeFromString(const std::string& name)
{
    if (name == "cylinder")
    {
        return PartShape::Cylinder;
    }
    if (name == "sphere")
    {
        return PartShape::Sphere;
    }
    if (name == "mesh")
    {
        return PartShape::Mesh;
    }
    return PartShape::Box;
}

glm::mat4 ModelPart::LocalMatrix() const
{
    return glm::translate(glm::mat4(1.0f), position) * EulerMatrix(rotation);
}

MeshData ModelAsset::BuildPartMesh(const ModelPart& part) const
{
    switch (part.shape)
    {
    case PartShape::Cylinder:
        return Primitives::Cylinder(part.size.x * 0.5f, part.size.y, 16);
    case PartShape::Sphere:
        return Primitives::Sphere(part.size.x * 0.5f, 16, 12);
    case PartShape::Mesh:
    {
        // An imported mesh carries its own vertices; size scales it, so a download that came in at
        // the wrong scale can be fixed without touching the file it came from.
        MeshData scaled = part.mesh;
        for (MeshVertex& vertex : scaled.vertices)
        {
            vertex.position *= part.size;
        }
        return scaled;
    }
    case PartShape::Box:
    default:
        return Primitives::Box(part.size);
    }
}

MeshData ModelAsset::BuildMesh() const
{
    MeshData combined;
    for (const ModelPart& part : parts)
    {
        if (!part.visible)
        {
            continue;
        }
        combined.Append(BuildPartMesh(part), part.LocalMatrix());
    }
    return combined;
}

const ModelSocket* ModelAsset::FindSocket(const std::string& socketName) const
{
    const auto it = std::find_if(sockets.begin(), sockets.end(),
                                 [&](const ModelSocket& socket) { return socket.name == socketName; });
    return it == sockets.end() ? nullptr : &*it;
}

const AnimationClip* ModelAsset::FindClip(const std::string& clipName) const
{
    const auto it = std::find_if(clips.begin(), clips.end(),
                                 [&](const AnimationClip& clip) { return clip.name == clipName; });
    return it == clips.end() ? nullptr : &*it;
}

ModelPart* ModelAsset::FindPart(const std::string& partName)
{
    const auto it = std::find_if(parts.begin(), parts.end(),
                                 [&](const ModelPart& part) { return part.name == partName; });
    return it == parts.end() ? nullptr : &*it;
}

glm::mat4 ModelAsset::PartMatrixAt(const ModelPart& part, const AnimationClip* clip, float time,
                                   float* visibility) const
{
    if (visibility != nullptr)
    {
        *visibility = part.visible ? 1.0f : 0.0f;
    }
    if (clip == nullptr || clip->tracks.empty())
    {
        return part.LocalMatrix();
    }

    const auto track = std::find_if(clip->tracks.begin(), clip->tracks.end(),
                                    [&](const AnimationTrack& candidate)
                                    { return candidate.part == part.name; });
    if (track == clip->tracks.end() || track->keys.empty())
    {
        return part.LocalMatrix();
    }

    // Linear between the two keys either side of `time`, holding the ends. Offsets are relative to
    // the rest pose, so editing the model does not invalidate a clip that was authored against it.
    const std::vector<AnimationKey>& keys = track->keys;
    AnimationKey blended = keys.front();
    if (time >= keys.back().time)
    {
        blended = keys.back();
    }
    else
    {
        for (size_t i = 1; i < keys.size(); ++i)
        {
            if (time <= keys[i].time)
            {
                const AnimationKey& a = keys[i - 1];
                const AnimationKey& b = keys[i];
                const float span = std::max(b.time - a.time, 1e-5f);
                const float t = std::clamp((time - a.time) / span, 0.0f, 1.0f);
                blended.position = glm::mix(a.position, b.position, t);
                blended.rotation = glm::mix(a.rotation, b.rotation, t);
                blended.visible = glm::mix(a.visible, b.visible, t);
                break;
            }
        }
    }

    if (visibility != nullptr)
    {
        *visibility = part.visible ? blended.visible : 0.0f;
    }
    return glm::translate(glm::mat4(1.0f), part.position + blended.position) *
           EulerMatrix(part.rotation + blended.rotation);
}

bool ModelAsset::LoadFromFile(const std::filesystem::path& file)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return false;
    }

    nlohmann::json json;
    try
    {
        stream >> json;
    }
    catch (const std::exception& error)
    {
        PRED_LOG_ERROR(Asset, "Model {} is not valid JSON: {}", file.string(), error.what());
        return false;
    }

    parts.clear();
    sockets.clear();
    clips.clear();
    name = file.stem().string();
    ReadField(json, "name", name);

    if (const auto it = json.find("parts"); it != json.end() && it->is_array())
    {
        for (const auto& node : *it)
        {
            ModelPart part;
            ReadField(node, "name", part.name);
            std::string shape = PartShapeName(part.shape);
            ReadField(node, "shape", shape);
            part.shape = PartShapeFromString(shape);

            if (const auto field = node.find("position"); field != node.end())
            {
                part.position = ReadVec3(*field, part.position);
            }
            if (const auto field = node.find("rotation"); field != node.end())
            {
                part.rotation = ReadVec3(*field, part.rotation);
            }
            if (const auto field = node.find("size"); field != node.end())
            {
                part.size = ReadVec3(*field, part.size);
            }
            if (const auto field = node.find("color"); field != node.end())
            {
                part.color = ReadVec3(*field, part.color);
            }
            ReadField(node, "roughness", part.roughness);
            ReadField(node, "metallic", part.metallic);
            ReadField(node, "emissive", part.emissive);
            ReadField(node, "visible", part.visible);
            ReadField(node, "source", part.sourceFile);

            // An imported mesh is stored inline, as flat arrays, so the model stays one file.
            if (const auto field = node.find("mesh"); field != node.end() && field->is_object())
            {
                const auto positions = field->find("positions");
                const auto normals = field->find("normals");
                const auto indices = field->find("indices");
                if (positions != field->end() && indices != field->end())
                {
                    const auto& p = *positions;
                    for (size_t i = 0; i + 2 < p.size(); i += 3)
                    {
                        MeshVertex vertex;
                        vertex.position = {p[i].get<float>(), p[i + 1].get<float>(), p[i + 2].get<float>()};
                        if (normals != field->end() && i + 2 < normals->size())
                        {
                            vertex.normal = {(*normals)[i].get<float>(), (*normals)[i + 1].get<float>(),
                                             (*normals)[i + 2].get<float>()};
                        }
                        part.mesh.vertices.push_back(vertex);
                    }
                    for (const auto& index : *indices)
                    {
                        part.mesh.indices.push_back(index.get<uint32_t>());
                    }
                }
            }
            parts.push_back(std::move(part));
        }
    }

    if (const auto it = json.find("sockets"); it != json.end() && it->is_array())
    {
        for (const auto& node : *it)
        {
            ModelSocket socket;
            ReadField(node, "name", socket.name);
            if (const auto field = node.find("position"); field != node.end())
            {
                socket.position = ReadVec3(*field, socket.position);
            }
            if (const auto field = node.find("rotation"); field != node.end())
            {
                socket.rotation = ReadVec3(*field, socket.rotation);
            }
            sockets.push_back(std::move(socket));
        }
    }

    if (const auto it = json.find("clips"); it != json.end() && it->is_array())
    {
        for (const auto& node : *it)
        {
            AnimationClip clip;
            ReadField(node, "name", clip.name);
            ReadField(node, "duration", clip.duration);
            ReadField(node, "loop", clip.loop);
            if (const auto tracks = node.find("tracks"); tracks != node.end() && tracks->is_array())
            {
                for (const auto& trackNode : *tracks)
                {
                    AnimationTrack track;
                    ReadField(trackNode, "part", track.part);
                    if (const auto keys = trackNode.find("keys"); keys != trackNode.end() && keys->is_array())
                    {
                        for (const auto& keyNode : *keys)
                        {
                            AnimationKey key;
                            ReadField(keyNode, "time", key.time);
                            if (const auto field = keyNode.find("position"); field != keyNode.end())
                            {
                                key.position = ReadVec3(*field, key.position);
                            }
                            if (const auto field = keyNode.find("rotation"); field != keyNode.end())
                            {
                                key.rotation = ReadVec3(*field, key.rotation);
                            }
                            ReadField(keyNode, "visible", key.visible);
                            track.keys.push_back(key);
                        }
                        std::sort(track.keys.begin(), track.keys.end(),
                                  [](const AnimationKey& a, const AnimationKey& b)
                                  { return a.time < b.time; });
                    }
                    clip.tracks.push_back(std::move(track));
                }
            }
            clips.push_back(std::move(clip));
        }
    }

    PRED_LOG_INFO(Asset, "Loaded model '{}': {} parts, {} sockets, {} clips", name, parts.size(),
                  sockets.size(), clips.size());
    return true;
}

bool ModelAsset::SaveToFile(const std::filesystem::path& file) const
{
    nlohmann::json json;
    json["name"] = name;

    nlohmann::json partArray = nlohmann::json::array();
    for (const ModelPart& part : parts)
    {
        nlohmann::json node;
        node["name"] = part.name;
        node["shape"] = PartShapeName(part.shape);
        node["position"] = WriteVec3(part.position);
        node["rotation"] = WriteVec3(part.rotation);
        node["size"] = WriteVec3(part.size);
        node["color"] = WriteVec3(part.color);
        node["roughness"] = part.roughness;
        node["metallic"] = part.metallic;
        node["emissive"] = part.emissive;
        node["visible"] = part.visible;
        if (part.shape == PartShape::Mesh && !part.mesh.vertices.empty())
        {
            node["source"] = part.sourceFile;
            nlohmann::json positions = nlohmann::json::array();
            nlohmann::json normals = nlohmann::json::array();
            for (const MeshVertex& vertex : part.mesh.vertices)
            {
                positions.push_back(vertex.position.x);
                positions.push_back(vertex.position.y);
                positions.push_back(vertex.position.z);
                normals.push_back(vertex.normal.x);
                normals.push_back(vertex.normal.y);
                normals.push_back(vertex.normal.z);
            }
            node["mesh"] = {{"positions", positions}, {"normals", normals}, {"indices", part.mesh.indices}};
        }
        partArray.push_back(std::move(node));
    }
    json["parts"] = std::move(partArray);

    nlohmann::json socketArray = nlohmann::json::array();
    for (const ModelSocket& socket : sockets)
    {
        socketArray.push_back({{"name", socket.name},
                               {"position", WriteVec3(socket.position)},
                               {"rotation", WriteVec3(socket.rotation)}});
    }
    json["sockets"] = std::move(socketArray);

    nlohmann::json clipArray = nlohmann::json::array();
    for (const AnimationClip& clip : clips)
    {
        nlohmann::json tracks = nlohmann::json::array();
        for (const AnimationTrack& track : clip.tracks)
        {
            nlohmann::json keys = nlohmann::json::array();
            for (const AnimationKey& key : track.keys)
            {
                keys.push_back({{"time", key.time},
                                {"position", WriteVec3(key.position)},
                                {"rotation", WriteVec3(key.rotation)},
                                {"visible", key.visible}});
            }
            tracks.push_back({{"part", track.part}, {"keys", std::move(keys)}});
        }
        clipArray.push_back({{"name", clip.name},
                             {"duration", clip.duration},
                             {"loop", clip.loop},
                             {"tracks", std::move(tracks)}});
    }
    json["clips"] = std::move(clipArray);

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_ERROR(Asset, "Could not write model to {}", file.string());
        return false;
    }
    stream << json.dump(2) << '\n';
    PRED_LOG_INFO(Asset, "Saved model '{}' to {}", name, file.string());
    return true;
}

std::filesystem::path ModelDirectory()
{
    return Paths::AssetsRoot() / "Models";
}

std::vector<std::string> ListModels()
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(ModelDirectory(), ec))
    {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json")
        {
            names.push_back(entry.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace pred
