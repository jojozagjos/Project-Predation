#pragma once

#include "Engine/Render/TextureLibrary.h"

#include <glm/vec3.hpp>

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

    // Multiplied into the base colour. An invalid handle draws the library's single white pixel, so
    // an untextured material is a textured one whose texture happens to be white and the shader
    // needs no branch.
    TextureHandle baseColorTexture;

    static Material Diffuse(const glm::vec3& color, float roughness = 0.85f)
    {
        Material material;
        material.baseColor = color;
        material.roughness = roughness;
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
