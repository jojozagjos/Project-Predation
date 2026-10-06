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
        SurfaceOf(stem, part);
        model.parts.push_back(part);
        return model.parts.back();
    }
    // What a part is made of, by what it is: the surface texture set it is laid with, and how big it repeats.
    static void SurfaceOf(const char* stem, ModelPart& part)
    {
        struct Made
        {
            const char* stem;
            const char* surface;
            float metres;
            float keep; // how much of the set's own colour shows: little, the part's colour is its paint
        };
        static constexpr Made kMade[] = {
            {"slab", "concrete_slab", 6.0f, 0.3f},         {"pad", "concrete_slab", 6.0f, 0.3f},
            {"pavement", "pavement", 3.0f, 0.3f},          {"road", "asphalt", 6.0f, 0.3f},
            {"tank", "painted_metal", 3.0f, 0.2f},         {"crane_leg", "painted_metal", 2.0f, 0.2f},
            {"shipworks_crane", "painted_metal", 2.0f, 0.2f}, {"shipworks_crane_mast", "painted_metal", 2.0f, 0.2f},
            {"trestle_leg", "pipe_steel", 1.0f, 0.3f},     {"trestle_beam", "pipe_steel", 1.0f, 0.3f},
            {"pipe", "pipe_steel", 1.0f, 0.3f},            {"street_pipe", "pipe_steel", 1.0f, 0.3f},
            {"conduit", "pipe_steel", 1.0f, 0.3f},         {"street_post", "pipe_steel", 1.0f, 0.3f},
            {"street_arm", "pipe_steel", 1.0f, 0.3f},      {"relay_leg", "pipe_steel", 1.0f, 0.3f},
            {"relay_brace", "pipe_steel", 1.0f, 0.3f},     {"stack", "pipe_steel", 1.5f, 0.3f},
            {"shipworks_roof", "shed_roof", 3.0f, 0.2f},   {"roof_unit", "shed_roof", 2.0f, 0.2f},
            {"bay_buttress", "blast_wall", 4.0f, 0.3f},    {"coping", "blast_wall", 4.0f, 0.3f},
            {"walkway", "facility_stairs", 1.0f, 0.3f},    {"salvage_dock", "blast_wall", 4.0f, 0.3f},
            {"building", "building_cladding", 4.0f, 0.2f}, {"building_parapet", "building_cladding", 4.0f, 0.2f},
            {"shipworks", "building_cladding", 4.0f, 0.2f}, {"bay_wall", "blast_wall", 4.0f, 0.3f},
            {"annex_module", "habitat_shell", 3.0f, 0.2f},  {"ops_front", "building_cladding", 4.0f, 0.2f},
            {"ops_plinth", "blast_wall", 4.0f, 0.3f},
        };
        for (const Made& made : kMade)
        {
            if (std::string(stem) == made.stem)
            {
                part.surface = made.surface;
                part.surfaceScale = made.metres;
                part.surfaceKeep = made.keep;
                return;
            }
        }
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
