#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

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

    // The base colour image, named relative to the assets root: "Models/Textures/m4_0.png". A path
    // rather than the bytes, unlike the mesh, because a texture is two megabytes and a model file
    // is meant to stay something a person can open. Empty means the part is drawn in `color` alone.
    std::string texture;

    glm::mat4 LocalMatrix() const;
};

// A named point on the model. The game asks for these by name rather than knowing the layout, so
// moving a grip in the editor moves the hand that holds it.
struct ModelSocket
{
    std::string name;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // euler degrees
    // The same three numbers as a turn, in the order the editor's fields read: X, then Y, then Z.
    // What it means depends on the socket: on `grip` it is how the weapon is turned in the hand.
    glm::quat Rotation() const;
};

// How a key reaches the next one.
//
// Every key used to be Linear, which is why hand-authored animation looked mechanical: a part moved
// at a constant speed and changed direction with a corner. Real movement starts and stops.
enum class KeyEase : uint8_t
{
    // Holds this key's value until the next one. For anything that does not slide: a magazine that
    // is either in the weapon or gone, a bolt that is forward or back.
    Step,
    // Constant speed to the next key. What was here before, kept because a straight run at a
    // steady rate is sometimes exactly right -- a belt feeding, a barrel spinning up.
    Linear,
    // Eases out of this key and into the next. The default, because most of what a weapon does is
    // a hand starting and stopping, and because a chain of these is the cheapest thing that stops
    // an animation looking like a machine.
    Smooth
};

struct AnimationKey
{
    float time = 0.0f; // seconds from the start of the clip
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // euler degrees
    float visible = 1.0f;     // 0 hides the part, for a magazine that has left the weapon
    KeyEase ease = KeyEase::Smooth;
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
    const ModelPart* FindPart(const std::string& partName) const;

    // Where a part sits at a moment in a clip, as a matrix in the model's frame. Falls back to the
    // rest pose when the clip does not touch that part.
    glm::mat4 PartMatrixAt(const ModelPart& part, const AnimationClip* clip, float time,
                           float* visibility = nullptr) const;
};

// Where authored models live, so nothing has to spell the path out.
std::filesystem::path ModelDirectory();

// Every model under it, by name, whatever folder it is in.
//
// The folders are for people, not for the game. A model is named by its file and found wherever it
// sits, so Models/Weapons/m4_carbine.json is still "m4_carbine" and moving it into a folder
// tomorrow does not rewrite weapons.json, the editor's list, or anybody's saved data. Adding an
// asset is then genuinely dropping a file in.
//
// The price is that two models cannot share a name in different folders. That is worth paying and
// it is not silent: ModelPath says which one it picked and reports the clash.
std::vector<std::string> ListModels();

// The file a model name refers to, or an empty path when there is none. Reported rather than
// guessed: a name that matches nothing is a mistake, and so is a name that matches twice.
std::filesystem::path ModelPath(const std::string& name);

// Where a model of a given kind should be written. Used when something new is created -- an import,
// an export from the editor -- so new files land in the right folder by themselves rather than
// piling up at the top and being tidied later.
std::filesystem::path ModelPathFor(const std::string& name, const std::string& folder);

// Forgets where the models are, so a file added while the game is running is found. Called after
// anything writes one.
void RescanModels();

} // namespace pred
