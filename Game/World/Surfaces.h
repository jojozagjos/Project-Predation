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
// The foliage atlas (Foliage::Atlas), made and uploaded the first time it is wanted, mipmapped for what it cuts out.
TextureHandle FoliageAtlas();

// Where the sets are uploaded: the game's texture library. Without one (a test, a headless run) nothing is laid on.
void SetLibrary(TextureLibrary* textures);

// `material` with the named set laid on it, repeating every `metres`; as it was when there is no such set. Each set is
// read once, the first time it is asked for.
// `keep`: how much of the set's own colour shows (Material::surfaceKeep); `natural`: ground, its repeat hidden.
Material Apply(Material material, const std::string& name, float metres, float keep = 1.0f, bool natural = false);
// And a second set where it is steep, on a material with a set laid on it already: tinted on average to `colour` (the
// steep parts' own colour, against the material's), repeating every `metres`.
Material ApplySteep(Material material, const std::string& name, float metres);
// `material` painted and worn: the named set read as a photograph of painted metal (Material::surfaceWear), repeating every
// `metres`, `wear` as much worn as it shows; the material's colour is the paint's, as it is.
Material ApplyWorn(Material material, const std::string& name, float metres, float wear);

// What a step on something sounds like: a footstep surface (Assets/Data/footsteps.json), by the set it is laid with, or by
// what it is (metal or not) when it has none. As a number for PhysicsWorld::SetTag (StepName turns it back).
uint32_t StepOf(const Material& material);
uint32_t StepOfSet(const std::string& name, bool metal);
const char* StepName(uint32_t tag);
// Whether the ground at (x, z) on a site is one of the outcrops of its rock showing through it (the ground shader's, fs_mesh:
// small patches on gentle rises), its slope given as its normal's up: so it sounds like rock underfoot where it looks like it.
bool OutcropAt(float x, float z, float normalUp);

} // namespace Surfaces

} // namespace pred
