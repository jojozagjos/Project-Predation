#pragma once

#include "Engine/Assets/ModelAsset.h"

#include <glm/common.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace pred
{

// Builds a model out of boxes and cylinders, each part named by what it is and a count -- for the places made of them (Kestrel
// Station, the outposts generated for other worlds). A part named starting "fx_" is never solid (VehicleProp).
struct PartBuilder
{
    ModelAsset& model;
    int count = 0;

    std::string Name(const char* stem) { return std::string(stem) + "_" + std::to_string(count++); }

    // A box between two corners.
    ModelPart& Box(const char* stem, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour, float rough = 0.8f, float metal = 0.0f,
                   float emissive = 0.0f)
    {
        ModelPart part;
        part.name = Name(stem);
        part.shape = PartShape::Box;
        part.position = (lo + hi) * 0.5f;
        part.size = glm::abs(hi - lo);
        part.color = colour;
        part.roughness = rough;
        part.metallic = metal;
        part.emissive = emissive;
        model.parts.push_back(part);
        return model.parts.back();
    }
    // A box about its middle, turned about up by `yaw` degrees and tipped about its own x by `tip`.
    ModelPart& Turned(const char* stem, const glm::vec3& centre, const glm::vec3& size, float yaw, const glm::vec3& colour, float tip = 0.0f,
                      float rough = 0.8f, float metal = 0.0f, float emissive = 0.0f)
    {
        ModelPart& part = Box(stem, centre - size * 0.5f, centre + size * 0.5f, colour, rough, metal, emissive);
        part.rotation = {tip, yaw, 0.0f};
        return part;
    }
    // A cylinder about its middle, standing along y unless turned (degrees).
    ModelPart& Cylinder(const char* stem, const glm::vec3& centre, float diameter, float length, const glm::vec3& rotation, const glm::vec3& colour,
                        float rough = 0.6f, float metal = 0.0f, float emissive = 0.0f)
    {
        ModelPart& part = Box(stem, centre - glm::vec3(diameter * 0.5f, length * 0.5f, diameter * 0.5f),
                              centre + glm::vec3(diameter * 0.5f, length * 0.5f, diameter * 0.5f), colour, rough, metal, emissive);
        part.shape = PartShape::Cylinder;
        part.rotation = rotation;
        return part;
    }
    // A ring of four along the top of a block, the long sides whole and the short ones between them, so no two meet face
    // to face on top.
    void Parapet(const char* stem, const glm::vec3& lo, const glm::vec3& hi, float thick, const glm::vec3& colour)
    {
        Box(stem, {lo.x, lo.y, lo.z}, {hi.x, hi.y, lo.z + thick}, colour, 0.85f);
        Box(stem, {lo.x, lo.y, hi.z - thick}, {hi.x, hi.y, hi.z}, colour, 0.85f);
        Box(stem, {lo.x, lo.y, lo.z + thick}, {lo.x + thick, hi.y, hi.z - thick}, colour, 0.85f);
        Box(stem, {hi.x - thick, lo.y, lo.z + thick}, {hi.x, hi.y, hi.z - thick}, colour, 0.85f);
    }
};

// A rectangle on the ground (x0, z0, x1, z1), less the holes in it: the pieces left, as rectangles. For pouring one surface
// into another (a pad into a slab) rather than laying it on top, where the two would flicker.
inline std::vector<glm::vec4> SubtractRects(const glm::vec4& rect, const std::vector<glm::vec4>& holes)
{
    std::vector<glm::vec4> pieces{rect};
    for (const glm::vec4& hole : holes)
    {
        std::vector<glm::vec4> next;
        for (const glm::vec4& r : pieces)
        {
            const float x0 = std::max(r.x, hole.x);
            const float z0 = std::max(r.y, hole.y);
            const float x1 = std::min(r.z, hole.z);
            const float z1 = std::min(r.w, hole.w);
            if (x1 <= x0 || z1 <= z0)
            {
                next.push_back(r);
                continue;
            }
            // Either side of the hole the full depth; before and after it between those.
            if (x0 > r.x)
            {
                next.push_back({r.x, r.y, x0, r.w});
            }
            if (x1 < r.z)
            {
                next.push_back({x1, r.y, r.z, r.w});
            }
            if (z0 > r.y)
            {
                next.push_back({x0, r.y, x1, z0});
            }
            if (z1 < r.w)
            {
                next.push_back({x0, z1, x1, r.w});
            }
        }
        pieces = std::move(next);
    }
    return pieces;
}

} // namespace pred
