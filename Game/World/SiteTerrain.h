#pragma once

#include "Engine/Render/Mesh.h"
#include "Game/World/SitePlan.h"

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// The ground of a site: not a flat plain but its world's own -- rolling, mountainous, cut by canyons, cratered, rippled
// into dunes -- planned from the site's seed and the kind of ground its world has (universe.json, a body's "terrain").
//
// Where people built (the buildings, the pad, the yards, and the ways between them) it is levelled: cut and filled to the
// buildings' floor, blending out into the natural ground over the next twenty metres or so. Round the edge of the site it
// rises into steep walls of rock, too steep to climb, so the site is a basin of its own. Everything outside the levelled
// ground -- boulders, debris, lamp poles -- stands on it where it is.
//
// A height field, two metres a cell, over the site and well past its edge. Every machine plans the same ground from the
// same seed and world, so only the seed is ever sent.
class SiteTerrain
{
public:
    enum class Shape : uint8_t
    {
        Flat,
        Rolling,
        Mountainous,
        Canyons,
        Cratered
    };
    // A world's terrain id ("mountainous", ...); flat for anything not known ("dunes" is flat ground drifted over: see Plan).
    static Shape ShapeOf(const std::string& terrain);

    // `dunes`: the ground drifted into long ridges across the wind, over whatever shape it has.
    void Plan(const SitePlan& plan, Shape shape, bool dunes);

    // How high the ground is at a place (world x, z), and which way it faces.
    float Height(float x, float z) const;
    glm::vec3 Normal(float x, float z) const;
    // The levelled ground's height: the buildings' floors, a hand under them.
    float Base() const { return m_base; }
    // How far a place is from being levelled ground: 0 where it is, 1 where the ground is the world's own.
    float Wildness(float x, float z) const;

    // The ground drawn and stood on, in pieces of so many cells a side: the world's ground on the gentle, rock showing through
    // as it steepens (painted per vertex, as a share of Paint's colour, which is the material's).
    static constexpr int kChunkCells = 30;
    int Chunks() const { return (m_cells + kChunkCells - 1) / kChunkCells; }
    MeshData Mesh(int chunkX, int chunkZ, const glm::vec3& ground, const glm::vec3& rock) const;
    static glm::vec3 Paint(const glm::vec3& ground, const glm::vec3& rock) { return glm::max(glm::max(ground, rock), glm::vec3(1.0e-3f)); }
    // How much of a place facing this way is rock, 0 to 1.
    static float Rockiness(const glm::vec3& normal);
    // How steep a slope is steep: about as steep as anybody can walk up (all rock from here on).
    static constexpr float kSteepCosine = 0.78f;

    bool Planned() const { return !m_heights.empty(); }
    // The corner the field starts at and how far it reaches (world x, z), for the tests.
    glm::vec2 Min() const { return m_min; }
    float Extent() const { return static_cast<float>(m_cells) * kCell; }
    static constexpr float kCell = 2.0f;
    // How far past the site's open ground the field reaches, every way.
    static constexpr float kMargin = 90.0f;

private:
    float At(int i, int j) const { return m_heights[static_cast<size_t>(j) * static_cast<size_t>(m_cells + 1) + static_cast<size_t>(i)]; }
    float Natural(float x, float z) const;

    struct Crater
    {
        glm::vec2 at;
        float radius;
    };

    Shape m_shape = Shape::Flat;
    bool m_dunes = false;
    uint32_t m_seed = 0;
    float m_base = 0.0f;
    glm::vec2 m_origin{0.0f};
    float m_size = 300.0f;
    glm::vec2 m_min{0.0f};
    int m_cells = 0;
    std::vector<float> m_heights;
    std::vector<float> m_wild;
    std::vector<Crater> m_craters;
    float m_duneAngle = 0.0f;
};

} // namespace pred
