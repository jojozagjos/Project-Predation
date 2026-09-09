#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pred
{

// An authored model: a list of parts and a list of named points on it.
//
// This is the format the in-engine editor reads and writes, and the format the game loads instead
// of building geometry in C++. A part is either a primitive or an imported mesh, so simple things
// stay editable as numbers and anything downloaded can be dropped straight in beside them.
//
// Everything is in the model's own frame. For a weapon that means the origin is the pistol grip,
// +Z runs down the barrel and +Y is up; sockets are what tell the game where the hands, the
// magazine and the muzzle are, so nothing outside this file has to guess.
enum class PartShape : uint8_t
{
    Box,
    Cylinder,
    Sphere,
    Mesh // vertices carried in the part itself, from an imported file
};

const char* PartShapeName(PartShape shape);
PartShape PartShapeFromString(const std::string& name);

struct ModelPart
{
    std::string name = "part";
    PartShape shape = PartShape::Box;

    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // euler degrees, applied X then Y then Z
    glm::vec3 size{0.1f, 0.1f, 0.1f};

    glm::vec3 color{0.5f, 0.5f, 0.55f};
    float roughness = 0.6f;
    float metallic = 0.0f;
    float emissive = 0.0f;

    bool visible = true;

    // Only for PartShape::Mesh. Kept with the part rather than referenced by path so a model is one
    // self-contained file, which matters when the thing it came from was a download.
    MeshData mesh;
    std::string sourceFile; // where it was imported from, for reference only

    glm::mat4 LocalMatrix() const;
};

// A named point on the model. The game asks for these by name rather than knowing the layout, so
// moving a grip in the editor moves the hand that holds it.
struct ModelSocket
{
    std::string name;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // euler degrees
};

// One keyframe of one part, at a moment in a clip.
struct AnimationKey
{
    float time = 0.0f; // seconds from the start of the clip
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // euler degrees
    float visible = 1.0f;     // 0 hides the part, for a magazine that has left the weapon
};

// The animation of one part through a clip.
struct AnimationTrack
{
    std::string part;
    std::vector<AnimationKey> keys;
};

// A named animation: reload, equip, fire and so on. Offsets are relative to the part's rest pose,
// so a clip keeps working when the model is edited underneath it.
struct AnimationClip
{
    std::string name;
    float duration = 1.0f;
    bool loop = false;
    std::vector<AnimationTrack> tracks;
};

class ModelAsset
{
public:
    std::string name;
    std::vector<ModelPart> parts;
    std::vector<ModelSocket> sockets;
    std::vector<AnimationClip> clips;

    bool LoadFromFile(const std::filesystem::path& file);
    bool SaveToFile(const std::filesystem::path& file) const;

    // Bakes every visible part into one mesh, for drawing a model that is not being animated.
    MeshData BuildMesh() const;
    // The mesh of one part, in the part's own frame.
    MeshData BuildPartMesh(const ModelPart& part) const;

    const ModelSocket* FindSocket(const std::string& socketName) const;
    const AnimationClip* FindClip(const std::string& clipName) const;
    ModelPart* FindPart(const std::string& partName);

    // Where a part sits at a moment in a clip, as a matrix in the model's frame. Falls back to the
    // rest pose when the clip does not touch that part.
    glm::mat4 PartMatrixAt(const ModelPart& part, const AnimationClip* clip, float time,
                           float* visibility = nullptr) const;
};

// Where authored models live, so nothing has to spell the path out.
std::filesystem::path ModelDirectory();
std::vector<std::string> ListModels();

} // namespace pred
