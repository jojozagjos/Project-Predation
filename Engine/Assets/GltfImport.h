#pragma once

#include "Engine/Assets/ModelAsset.h"

#include <filesystem>
#include <string>
#include <vector>

namespace pred
{

// Reads a glTF 2.0 binary file into an editable model.
//
// Written here rather than pulled in as a library because the part of glTF a static prop needs is
// small and completely specified: a JSON chunk, a binary chunk, and accessors that say how to read
// one out of the other. The JSON parser is already a dependency. What is deliberately not read is
// skinning, morph targets, cameras, lights and textures, none of which this renderer has anything
// to do with; a textured download arrives as flat-shaded parts in the colours of its materials,
// which is the look the game has anyway.
//
// Every primitive becomes one part, baked into world space by its node's transform, so what comes
// out is a list of pieces that can be coloured, hidden and animated separately in the editor. That
// matters more than fidelity for a weapon: a magazine has to be its own part or a reload cannot
// move it.
struct GltfImportOptions
{
    // The longest side the model is scaled to, in metres. Downloads arrive in wildly different
    // units and a model a hundred times too large is indistinguishable from one that failed.
    float targetSize = 0.6f;
    // Turned about each axis, in degrees, before anything else. glTF has no idea which way a weapon
    // points, and the two conventions in the wild differ by a quarter turn; this is how the barrel
    // is made to run down +Z where the game expects it.
    glm::vec3 rotationDegrees{0.0f};
    // Slides the origin to the middle of the model's footprint first. Off by default because a
    // weapon's origin is usually meaningful; on, it makes an arbitrary download easier to place.
    bool centre = true;
    // Where the images embedded in the file are written, and what they are named relative to the
    // assets root. Textures are written out beside the model rather than carried inside it: a
    // model file is meant to stay something a person can open, and a base colour image is two
    // megabytes. Leave the directory empty to skip textures altogether.
    std::filesystem::path textureDirectory;
    std::string texturePrefix;
    // Parts to leave out, by name, before the model is scaled and centred.
    //
    // Downloads are often display pieces: a gun with a spare magazine lying beside it and a couple of
    // cartridges stood on end. Left in, they float next to the weapon in the player's hands, and
    // because the whole file is fitted to one size they also shrink the weapon itself to make room
    // for them. Removing them afterwards fixes the first and not the second.
    std::vector<std::string> skipParts;
};

// Returns false and leaves `out` untouched when the file cannot be read or holds no triangles.
// `error`, when given, is filled with something that says why.
bool LoadGlbModel(const std::filesystem::path& file, const GltfImportOptions& options, ModelAsset& out,
                  std::string* error = nullptr);

// True when the extension is one LoadGlbModel will take.
bool IsGltfFile(const std::filesystem::path& file);

} // namespace pred
