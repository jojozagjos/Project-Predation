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
// buildings' floor, blending out into the natural ground over the next twenty metres or so. Round it the land goes on to
// the horizon: the world's landforms at their own size coming in with distance (a range of mountains, mesas, great
// craters), drawn out to a few kilometres (FarMesh); how far anybody may go is kRoam past the open ground, no wall of rock.
// Everything outside the levelled ground -- boulders, debris, lamp poles -- stands on it where it is.
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
    // What water lies on the ground: none; lakes in its low ground (a level a few metres under the site's); the sea along one
    // side, the land going down into it past the site (a coast); or all round it (an island).
    enum class Water : uint8_t
    {
        None,
        Lakes,
        Coast,
        Island
    };

    // A world's terrain id ("mountainous", ...); flat for anything not known ("dunes" is flat ground drifted over: see Plan).
    static Shape ShapeOf(const std::string& terrain);

    // `dunes`: the ground drifted into long ridges across the wind, over whatever shape it has.
    void Plan(const SitePlan& plan, Shape shape, bool dunes, Water water = Water::None);

    // Its water: whether it has any, the level of it (world y), and how deep it is at a place (nought or less: dry ground).
    bool HasWater() const { return m_water != Water::None; }
    float WaterLevel() const { return m_waterLevel; }
    float WaterDepth(float x, float z) const;
    // The surface of it over the site and the land round it, to the horizon: a grid, finer near, its corners' uv.x how deep
    // the water is under each (for its colour and its shore), only where there is water under some of a square.
    MeshData WaterMesh() const;

    // How high the ground is at a place (world x, z), and which way it faces.
    float Height(float x, float z) const;
    glm::vec3 Normal(float x, float z) const;
    // The levelled ground's height: the buildings' floors, a hand under them.
    float Base() const { return m_base; }
    // And as drawn anywhere out to the horizon: the field's where it reaches, the land round it (FarMesh) past that.
    float GroundHeight(float x, float z) const;
    glm::vec3 GroundNormal(float x, float z) const;
    // Whether a place is on the height field (what can be walked on and a margin round it).
    bool OnField(float x, float z) const;
    // How far a place is from being levelled ground: 0 where it is, 1 where the ground is the world's own.
    float Wildness(float x, float z) const;

    // The ground drawn and stood on, in pieces of so many cells a side: the world's ground on the gentle, rock showing through
    // as it steepens (painted per vertex, as a share of Paint's colour, which is the material's).
    static constexpr int kChunkCells = 30;
    int Chunks() const { return (m_cells + kChunkCells - 1) / kChunkCells; }
    // `grown`, when given: the colour of the grass over the world's own ground, laid over it there (and thinning out towards
    // what was levelled) so the ground reads as grown over far off, past where tufts of it are drawn.
    MeshData Mesh(int chunkX, int chunkZ, const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown = nullptr) const;
    // Whether anybody could ever stand on a piece: the pieces wholly up the rock round the edge are drawn but need nothing
    // to stand on.
    bool InReach(int chunkX, int chunkZ) const;
    // The land round the site out to the horizon, drawn only: coarser the further it is, from just inside the edge of the
    // height field (under it there, so no gap shows where the two meet) out to kFar from the site's middle every way.
    MeshData FarMesh(const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown = nullptr) const;
    static constexpr float kFar = 3000.0f;
    // How far past the site's open ground anybody may go (the shuttle's comms): the game turns them back there.
    static constexpr float kRoam = 40.0f;
    static glm::vec3 Paint(const glm::vec3& ground, const glm::vec3& rock) { return glm::max(glm::max(ground, rock), glm::vec3(1.0e-3f)); }
    // The colour painted on the ground at a point and facing (Mesh's, over Paint's): its own, grown over where it is wild
    // when `grown` is given, the rock's where it is steep.
    glm::vec3 Colour(float x, float z, const glm::vec3& normal, const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown) const;
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
    // The far land's lines out from the middle each way (offsets, metres), and its height at a place.
    std::vector<float> FarLines() const;
    float FarHeight(float x, float z) const;
    // The world's landforms at their own size (a range, mesas, great craters, swells), for the land round the site.
    float Macro(float x, float z) const;

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
    std::vector<float> m_farLines; // FarLines, kept for GroundHeight
    std::vector<Crater> m_craters;
    float m_duneAngle = 0.0f;
    float m_macroHere = 0.0f; // Macro at the site's middle: the land round it is measured from it
    Water m_water = Water::None;
    float m_waterLevel = -1.0e9f;
    glm::vec2 m_coastWay{1.0f, 0.0f}; // which way the sea is, at a coast
};

} // namespace pred
