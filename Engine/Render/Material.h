#pragma once

#include "Engine/Render/TextureLibrary.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace pred
{

// Metallic-roughness surface parameters, matching the glTF convention so imported assets map
// directly onto it.
struct Material
{
    glm::vec3 baseColor{0.75f, 0.75f, 0.78f};
    float metallic = 0.0f;
    float roughness = 0.85f;
    glm::vec3 emissive{0.0f};
    // How much of a flat mirror.s reflection this surface shows, 0 to 1.
    //
    // Only meaningful on a surface lying in the mirror plane the renderer was given: this is a
    // planar reflection, which is one flat surface rendered from a camera reflected across it, not
    // a property any surface can have. Anything else with this set shows somebody else.s reflection
    // smeared across it, so it is set by the map on the panels that are mirrors and nowhere else.
    float reflectivity = 0.0f;

    // Multiplied into the base colour. An invalid handle draws the library's single white pixel, so
    // an untextured material is a textured one whose texture happens to be white and the shader
    // needs no branch.
    TextureHandle baseColorTexture;
    // The glow painted by the texture: emissive times what the texture shows there, and times its alpha, a screen lit
    // where its picture is and dark between, and not at all where the alpha is nought. Off, the glow is the same all over.
    bool emissiveTextured = false;

    // A surface texture set laid on by where things are rather than by their own coordinates: a normal map and a
    // roughness map beside the base colour, all repeating every `surfaceScale` metres across whatever the material
    // is on, from whichever side it faces (0: off, and the mesh's own coordinates are used as ever). For level
    // geometry, which is boxes whose coordinates only stretch one copy over each face.
    TextureHandle normalTexture;
    TextureHandle roughnessTexture;
    float surfaceScale = 0.0f;
    // How much of the set's own colour shows (1 all; 0 none, only its light and dark): a photograph's paint and rust
    // fighting the colour asked of the surface is let go of.
    float surfaceKeep = 1.0f;
    // Whether the set is natural ground, whose repeat is hidden by blending it with a larger, turned copy of itself --
    // not for anything laid in a grid (paving, plate), which would come out in diamonds.
    bool surfaceNatural = false;
    // And a second set where the surface is steep -- a world's rock showing through its ground on the slopes -- blended in
    // as the slope passes what anybody can stand on, repeating every `steepScale` metres (0: none), its colour times
    // `steepTint` (the first set's average colour over its own, so each is tinted to the colour asked of it).
    TextureHandle steepColorTexture;
    TextureHandle steepNormalTexture;
    TextureHandle steepRoughnessTexture;
    float steepScale = 0.0f;
    glm::vec3 steepTint{1.0f};

    // Living tissue that grows, beats and dies across its surface -- a nest's skin -- driven from here and
    // from each vertex's texture coordinates: x how far along the growth from its source, y how far it
    // stands off the surface under it. x = on (0 for everything else), y = how far it has grown, z = how
    // far death has come out (negative: alive), w = how far behind the death it has rotted down.
    glm::vec4 organic{0.0f};
    // x = where the beat is (0..1), y = how hard it beats, z = how long the beat takes to cross a metre
    // (in beats), w unused.
    glm::vec4 organicBeat{0.0f};

    static Material Diffuse(const glm::vec3& color, float roughness = 0.85f)
    {
        Material material;
        material.baseColor = color;
        material.roughness = roughness;
        return material;
    }

    // A mirror: polished metal that also shows the planar reflection.
    static Material Mirror(const glm::vec3& color, float reflectivity = 0.85f)
    {
        Material material;
        material.baseColor = color;
        material.metallic = 1.0f;
        material.roughness = 0.04f;
        material.reflectivity = reflectivity;
        return material;
    }

    static Material Metal(const glm::vec3& color, float roughness = 0.35f)
    {
        Material material;
        material.baseColor = color;
        material.metallic = 1.0f;
        material.roughness = roughness;
        return material;
    }

    static Material Emissive(const glm::vec3& color, float strength = 1.0f)
    {
        Material material;
        material.baseColor = color;
        material.emissive = color * strength;
        return material;
    }
};

} // namespace pred
