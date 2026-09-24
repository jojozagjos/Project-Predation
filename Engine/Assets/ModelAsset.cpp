#include "Engine/Assets/ModelAsset.h"
#include "Engine/Core/JsonText.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Primitives.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <map>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
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

glm::mat4 EulerDegreesMatrix(const glm::vec3& degrees)
{
    return EulerMatrix(degrees);
}

glm::vec3 EulerDegreesFromMatrix(const glm::mat4& matrix)
{
    // The matrix above is Z times Y times X, which is exactly what this takes apart.
    float z = 0.0f;
    float y = 0.0f;
    float x = 0.0f;
    glm::extractEulerAngleZYX(matrix, z, y, x);
    return glm::degrees(glm::vec3(x, y, z));
}

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

glm::quat ModelSocket::Rotation() const
{
    return glm::quat_cast(EulerMatrix(rotation));
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

const ModelPart* ModelAsset::FindPart(const std::string& partName) const
{
    const auto it = std::find_if(parts.begin(), parts.end(),
                                 [&](const ModelPart& part) { return part.name == partName; });
    return it == parts.end() ? nullptr : &*it;
}

TrackSample SampleTrack(const AnimationTrack& track, float time)
{
    TrackSample sample;
    if (track.keys.empty())
    {
        return sample;
    }
    const std::vector<AnimationKey>& keys = track.keys;
    const auto take = [&](const AnimationKey& key)
    {
        sample.position = key.position;
        sample.euler = key.rotation;
        sample.rotation = glm::quat(glm::radians(key.rotation));
        sample.visible = key.visible;
        sample.holder = key.holder;
    };
    if (time <= keys.front().time)
    {
        take(keys.front());
        return sample;
    }
    if (time >= keys.back().time)
    {
        take(keys.back());
        return sample;
    }
    for (size_t i = 1; i < keys.size(); ++i)
    {
        // The first key after `time`, strictly. Exactly on a key, that key is the one being left, so
        // a key that hands a part to something else takes effect at its own moment and not a hair
        // after it -- which is what lets the editor ask where a part is at a key and get that key.
        if (time >= keys[i].time)
        {
            continue;
        }
        const AnimationKey& a = keys[i - 1];
        const AnimationKey& b = keys[i];
        take(a);
        // A part that changes hands does so at the key, all at once. Its two keys are in different
        // frames -- one an offset in the weapon, the other a place in a hand -- and a blend between
        // them would be a blend between two different questions.
        if (a.holder != b.holder)
        {
            return sample;
        }
        const float span = std::max(b.time - a.time, 1e-5f);
        const float raw = std::clamp((time - a.time) / span, 0.0f, 1.0f);
        // How the gap is crossed is a property of the key being left, not the one being arrived at:
        // a key says how it hands over.
        const float t = a.ease == KeyEase::Step      ? 0.0f
                        : a.ease == KeyEase::Linear ? raw
                                                    : raw * raw * (3.0f - 2.0f * raw);
        sample.position = glm::mix(a.position, b.position, t);

        // Rotation goes the short way round, as a rotation rather than as three numbers.
        //
        // Mixing Euler triples is not interpolating a rotation, it is interpolating the notation. Two
        // turns a hundred and eighty degrees apart average to something that is neither; a part
        // crossing the wrap from 179 to -179 degrees takes the long way round the whole circle rather
        // than the two degrees it actually moved; and near the poles the three numbers stop being
        // independent at all. Converting each key to a rotation and taking the shortest arc between
        // them does none of that, and for the small turns most keys hold it agrees with the old way to
        // within a rounding error.
        const glm::quat from = glm::quat(glm::radians(a.rotation));
        const glm::quat to = glm::quat(glm::radians(b.rotation));
        sample.rotation = glm::slerp(from, to, t);
        sample.euler = glm::degrees(glm::eulerAngles(sample.rotation));

        // Visibility does not fade: a magazine is in the weapon or it is not. Mixed, the value only
        // crosses the threshold the renderer tests against right at the far key, so a part keyed away
        // at one moment and back at another stayed on screen for essentially the whole gap.
        sample.visible = a.visible;
        return sample;
    }
    take(keys.back());
    return sample;
}

glm::vec3 ModelAsset::HandRest(int side) const
{
    const ModelSocket* socket = FindSocket(side == 0 ? "support" : "grip");
    return socket != nullptr ? socket->position : glm::vec3(0.0f);
}

bool ModelAsset::HandAt(const AnimationClip* clip, int side, float time, glm::vec3& offset, glm::quat& turn) const
{
    offset = glm::vec3(0.0f);
    turn = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (clip == nullptr)
    {
        return false;
    }
    const std::string trackName = side == 0 ? kLeftHandTrack : kRightHandTrack;
    for (const AnimationTrack& track : clip->tracks)
    {
        if (track.part == trackName && !track.keys.empty())
        {
            const TrackSample sample = SampleTrack(track, time);
            offset = sample.position;
            // The same order of turns the editor's fields and the parts use, so a hand turned by the
            // numbers in the editor turns the way a part given the same numbers would.
            turn = glm::quat_cast(EulerMatrix(sample.euler));
            return true;
        }
    }
    return false;
}

glm::mat4 ModelAsset::HandFrameAt(const AnimationClip* clip, int side, float time, const glm::vec3& rest) const
{
    glm::vec3 offset{0.0f};
    glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
    HandAt(clip, side, time, offset, turn);
    return glm::translate(glm::mat4(1.0f), rest + offset) * glm::mat4_cast(turn);
}

glm::mat4 ModelAsset::PartMatrixAt(const ModelPart& part, const AnimationClip* clip, float time,
                                   float* visibility, const glm::vec3* handRest, int depth) const
{
    if (visibility != nullptr)
    {
        *visibility = part.visible ? 1.0f : 0.0f;
    }

    // Its own movement first: where its own track puts it, or where it rests.
    glm::mat4 own = part.LocalMatrix();
    bool ownHand = false;
    if (clip != nullptr && !clip->tracks.empty())
    {
        const auto track = std::find_if(clip->tracks.begin(), clip->tracks.end(),
                                        [&](const AnimationTrack& candidate) { return candidate.part == part.name; });
        if (track != clip->tracks.end() && !track->keys.empty())
        {
            const TrackSample sample = SampleTrack(*track, time);
            if (visibility != nullptr)
            {
                *visibility = part.visible ? sample.visible : 0.0f;
            }
            if (sample.holder != PartHolder::Weapon)
            {
                // In a hand: wherever the hand is, and where the part sits in it.
                const int side = sample.holder == PartHolder::LeftHand ? 0 : 1;
                const glm::vec3 rest = handRest != nullptr ? handRest[side] : HandRest(side);
                own = HandFrameAt(clip, side, time, rest) * glm::translate(glm::mat4(1.0f), sample.position) *
                      EulerMatrix(sample.euler);
                ownHand = true;
            }
            else
            {
                // In the weapon: offsets from the rest pose, so editing the model does not invalidate a
                // clip that was authored against it.
                own = glm::translate(glm::mat4(1.0f), part.position + sample.position) *
                      EulerMatrix(part.rotation + sample.euler);
            }
        }
    }

    // Then the part it moves with, if any: carried by however far that one has moved from where it
    // rests. At rest that is no movement at all, which is why grouping never shifts anything. A part a
    // hand is holding answers to the hand, not to its group. Bounded, so a loop of parts that each move
    // with the next cannot hang the game.
    if (!part.parent.empty() && !ownHand && depth < 8)
    {
        if (const ModelPart* parent = FindPart(part.parent); parent != nullptr && parent != &part)
        {
            float parentVisible = 1.0f;
            const glm::mat4 moved = PartMatrixAt(*parent, clip, time, &parentVisible, handRest, depth + 1);
            own = moved * glm::inverse(parent->LocalMatrix()) * own;
            // A magazine gone into a pouch takes its pieces with it.
            if (visibility != nullptr)
            {
                *visibility = std::min(*visibility, parentVisible);
            }
        }
    }
    return own;
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
            ReadField(node, "texture", part.texture);
            ReadField(node, "parent", part.parent);

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
                            // Absent in anything authored before easing existed, which is every
                            // clip currently on disk. Those were all linear, so that is what they
                            // are read back as: a file that has never been opened in the editor
                            // should not quietly start moving differently.
                            std::string ease = "linear";
                            ReadField(keyNode, "ease", ease);
                            key.ease = ease == "step"     ? KeyEase::Step
                                       : ease == "smooth" ? KeyEase::Smooth
                                                          : KeyEase::Linear;
                            // Absent means the weapon, which is every key written before a hand
                            // could hold anything.
                            std::string held = "weapon";
                            ReadField(keyNode, "held", held);
                            key.holder = held == "left"    ? PartHolder::LeftHand
                                         : held == "right" ? PartHolder::RightHand
                                                           : PartHolder::Weapon;
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
        if (!part.parent.empty())
        {
            node["parent"] = part.parent;
        }
        // Any part can wear an image, not only an imported one. This used to be written inside the
        // imported-mesh block below, so a texture put on a box or a cylinder in the editor was simply
        // not saved, and came back as a plain grey part the next time the model was opened.
        if (!part.texture.empty())
        {
            node["texture"] = part.texture;
        }
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
                const char* ease = key.ease == KeyEase::Step     ? "step"
                                   : key.ease == KeyEase::Smooth ? "smooth"
                                                                 : "linear";
                keys.push_back({{"time", key.time},
                                {"position", WriteVec3(key.position)},
                                {"rotation", WriteVec3(key.rotation)},
                                {"visible", key.visible},
                                {"ease", ease}});
                if (key.holder != PartHolder::Weapon)
                {
                    keys.back()["held"] = key.holder == PartHolder::LeftHand ? "left" : "right";
                }
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
    stream << JsonText(json);
    PRED_LOG_INFO(Asset, "Saved model '{}' to {}", name, file.string());
    // The name-to-file index is now out of date by exactly this one file. Done here rather than by
    // every caller, because "remember to rescan after you save" is a rule that gets forgotten and
    // whose symptom is a model that exists on disk and cannot be found by name.
    RescanModels();
    return true;
}

std::filesystem::path ModelDirectory()
{
    return Paths::AssetsRoot() / "Models";
}

namespace
{

// Name to file, for every model under the models folder whatever folder that is.
//
// The folders are for people, not for the game. A model is named by its file and found wherever it
// sits, so moving one into a subfolder does not rewrite weapons.json, the editor's list or anybody
// else's reference to it, and adding an asset is genuinely dropping a file in.
//
// Built once and thrown away when something writes a model, rather than kept live: the alternative
// is walking the asset tree every time a weapon is drawn, and models are read far more often than
// they are added.
struct Index
{
    std::map<std::string, std::filesystem::path> byName;
    bool built = false;
    // Which folder it was built for.
    //
    // The index holds whole paths, so it belongs to one assets root. Anything that moves the root
    // underneath it -- a tool pointed at a different tree, and every test that calls Paths::Init --
    // leaves it holding paths into the old one, and the symptom is a model that exists and cannot
    // be found, depending on what ran first.
    std::filesystem::path root;
};

Index& ModelIndex()
{
    static Index index;
    const std::filesystem::path root = ModelDirectory();
    if (index.built && index.root == root)
    {
        return index;
    }
    index.built = true;
    index.root = root;
    index.byName.clear();

    std::error_code ec;
    for (auto entry = std::filesystem::recursive_directory_iterator(root, ec);
         entry != std::filesystem::recursive_directory_iterator(); entry.increment(ec))
    {
        if (ec)
        {
            break;
        }
        if (!entry->is_regular_file(ec) || entry->path().extension() != ".json")
        {
            continue;
        }
        // Source art and textures are not models. They live under here so everything about a model
        // is in one place, and a .json among them belongs to an exporter or a texture pack.
        const std::filesystem::path relative = std::filesystem::relative(entry->path(), root, ec);
        if (!relative.empty())
        {
            const std::string top = relative.begin()->string();
            if (top == "Source" || top == "Textures")
            {
                continue;
            }
        }

        const std::string name = entry->path().stem().string();
        const auto [existing, added] = index.byName.emplace(name, entry->path());
        if (!added)
        {
            // Said out loud rather than settled by whichever the directory walk reached first. Two
            // models with one name is a mistake somebody made a minute ago and would otherwise
            // spend an hour on, because the symptom is that edits to one of them do nothing.
            PRED_LOG_ERROR(Asset, "Two models are called '{}': {} and {}. Using the first.", name,
                           existing->second.string(), entry->path().string());
        }
    }
    return index;
}

} // namespace

void RescanModels()
{
    ModelIndex().built = false;
}

std::filesystem::path ModelPath(const std::string& name)
{
    const Index& index = ModelIndex();
    const auto found = index.byName.find(name);
    return found == index.byName.end() ? std::filesystem::path{} : found->second;
}

std::filesystem::path ModelPathFor(const std::string& name, const std::string& folder)
{
    // An existing model is rewritten where it already is. Moving somebody's file because they
    // saved it is the kind of helpfulness nobody asked for.
    if (const std::filesystem::path existing = ModelPath(name); !existing.empty())
    {
        return existing;
    }
    std::filesystem::path directory = ModelDirectory();
    if (!folder.empty())
    {
        directory /= folder;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return directory / (name + ".json");
}


std::vector<std::string> ListModels()
{
    std::vector<std::string> names;
    for (const auto& [name, file] : ModelIndex().byName)
    {
        names.push_back(name);
    }
    // Already in order -- a std::map is sorted by key -- but said rather than relied on, because
    // the container is an implementation detail and this list is drawn on a screen.
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace pred
