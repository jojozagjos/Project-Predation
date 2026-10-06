#pragma once

#include "Engine/Render/Material.h"

#include <string>

namespace pred
{

class TextureLibrary;

// Surface texture sets: a folder under Assets/Textures for each, named for the set, with <set>_BaseColor,
// <set>_Normal (OpenGL style) and <set>_Roughness in it (.png or .jpg). Laid on a level's materials by where things
// are rather than by their coordinates (Material::surfaceScale), so a set is chosen by name and how big it repeats.
namespace Surfaces
{

// Where the sets are uploaded: the game's texture library. Without one (a test, a headless run) nothing is laid on.
void SetLibrary(TextureLibrary* textures);

// `material` with the named set laid on it, repeating every `metres`; as it was when there is no such set. Each set is
// read once, the first time it is asked for.
// `keep`: how much of the set's own colour shows (Material::surfaceKeep); `natural`: ground, its repeat hidden.
Material Apply(Material material, const std::string& name, float metres, float keep = 1.0f, bool natural = false);
// And a second set where it is steep, on a material with a set laid on it already: tinted on average to `colour` (the
// steep parts' own colour, against the material's), repeating every `metres`.
Material ApplySteep(Material material, const std::string& name, float metres);

} // namespace Surfaces

} // namespace pred
