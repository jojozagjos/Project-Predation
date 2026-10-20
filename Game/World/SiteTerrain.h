#pragma once

#include "Engine/Render/Mesh.h"
#include "Game/World/SitePlan.h"

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
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
        Cratered,
        IceSheet,  // flat ice: pressure ridges, shallow crevasses, the odd peak standing out of it far off
        Glacial,   // the floor of a broad U-shaped valley, mountain walls either side
        LavaPlain, // old flows: low hummocks and blisters, a volcano's cone far off
        Karst,     // rolling ground pitted with sinkholes, limestone towers standing out of the land round it
        Highlands  // a high plateau of rolling moor and rock tors, the land falling away past its edge
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
    // Where that grid is finest (its middle, x and z) and how far out from it every way its lines stay a few metres apart; and
    // how far apart they are at a place -- for the waves on it, which let go of what is too short for the lines (Waves).
    glm::vec2 WaterMiddle() const { return m_origin + glm::vec2(m_size * 0.5f); }
    float WaterFineReach() const { return Extent() * 0.5f; }
    float WaterLineSpacing(float x, float z) const;

    // How high the ground is at a place (world x, z), and which way it faces.
    float Height(float x, float z) const;
    glm::vec3 Normal(float x, float z) const;
    // As the drawn ground has it, between its corners: the corners' own normals and colours blended across each of the two
    // triangles a cell is drawn as. What is laid flat on the ground and painted to match it -- a footprint -- takes these,
    // not Normal's and Colour's at the place itself, or it is shaded and coloured a little differently and shows as a patch.
    glm::vec3 DrawnNormal(float x, float z) const;
    glm::vec3 DrawnColour(float x, float z, const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown) const;
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
    // Whether a cell of the ground (column, row) is left out: under a building with basements (see m_holes).
    bool LeftOut(int i, int j) const;
    // The land round the site out to the horizon, drawn only: coarser the further it is, from just inside the edge of the
    // height field (under it there, so no gap shows where the two meet) out to kFar from the site's middle every way.
    MeshData FarMesh(const glm::vec3& ground, const glm::vec3& rock, const glm::vec3* grown = nullptr) const;
    static constexpr float kFar = 3000.0f;
    // How far anybody may go (the shuttle's comms; the game turns them back there): a shape round the site's open ground --
    // its square widened by kRoam every way, the outline wandering in bays and headlands -- and valleys reaching out from it
    // into the land (Arms), each ending in a wider bowl, somewhere to go to. How far past that shape a place is, metres
    // (less than nought: inside it).
    float PastRoam(float x, float z) const;
    // A valley out from the site's open ground: its line from the side of the square out (a few bends), how wide it is either
    // side of that, and the bowl at its end (its middle, how far round) -- where something out in the land can stand.
    struct Arm
    {
        std::vector<glm::vec2> line;
        float halfWidth = 20.0f;
        glm::vec2 end{0.0f};
        float bowl = 26.0f;
    };
    const std::vector<Arm>& Arms() const { return m_arms; }
    // (The band round the square, at its narrowest a little less: see PastRoam.)
    static constexpr float kRoam = 60.0f;
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
    // (Past the band and the valleys out of it, with the cliffs round them.)
    static constexpr float kMargin = 260.0f;

private:
    float At(int i, int j) const { return m_heights[static_cast<size_t>(j) * static_cast<size_t>(m_cells + 1) + static_cast<size_t>(i)]; }
    // The normal the drawn ground has at a corner of its grid; and the corners of the triangle a place is drawn in, how much of
    // each (DrawnNormal, DrawnColour).
    glm::vec3 VertexNormal(int i, int j) const;
    void DrawnCorners(float x, float z, int (&i)[3], int (&j)[3], float (&weight)[3]) const;
    float Natural(float x, float z) const;
    // The far land's lines out from the middle each way (offsets, metres), and its height at a place.
    std::vector<float> FarLines() const;
    float FarHeight(float x, float z) const;
    // The world's landforms at their own size (a range, mesas, great craters, swells), for the land round the site.
    float Macro(float x, float z) const;
    // Where the land stops anybody going further, past the site's own ground: cliffs too steep to climb, ravines on canyon and
    // mountain worlds, nothing on the sea's side (the deep water is the edge there). Part of Natural.
    float Edge(float x, float z) const;

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
    // Where the ground is left out, under buildings with basements (their own floors cover it, and stairs go down through
    // where it would be): each a building's footprint, a little in from its walls.
    std::vector<std::pair<glm::vec2, glm::vec2>> m_holes;
    float m_duneAngle = 0.0f;
    float m_macroHere = 0.0f; // Macro at the site's middle: the land round it is measured from it
    Water m_water = Water::None;
    float m_waterLevel = -1.0e9f;
    glm::vec2 m_coastWay{1.0f, 0.0f}; // which way the sea is, at a coast
    std::array<glm::vec2, 3> m_edgeWaves{}; // how Edge goes round the site: each wave's weight and phase
    std::vector<Arm> m_arms;
};

} // namespace pred
