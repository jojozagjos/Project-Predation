#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

// A nest, as creatures grow them: not a heap on the floor but a living thing rooted in a wall. A heart
// the size of a person's chest hangs from the wall, its vessels spread out across it, and growth
// spreads from it over the floor, the walls and the ceiling round about as the nest gets older.
// Sculpted the way a creature is -- blended shapes turned into one surface -- and painted per vertex,
// with how wet each part is in the alpha of its colour.

// The heart, and the flesh that roots it into the wall, in the wall's own frame: +Z comes out of the
// wall, +Y is up, and the wall itself is z = 0. Two meshes because only one of them beats.
struct NestHeartMeshes
{
    MeshData heart;
    MeshData roots;
};
NestHeartMeshes BuildNestHeart(uint32_t seed);

// Where the middle of the heart is, in that frame: what is shot at, lit and heard.
inline constexpr float kNestHeartOut = 0.34f;
inline constexpr float kNestHeartUp = 0.05f;

// The rest of a nest: one living skin over every surface it has grown on, as a few meshes of a few metres
// each. Built from the plan of where it grows -- the patches the heart spreads to over the surfaces, and
// the roots between them -- as one blended shape: a lumpy membrane of flesh coating the floor, walls and
// ceiling, raised roots over it, egg sacs clustered near the heart, strands hanging from the ceiling.
//
// Every vertex carries, in its texture coordinates, how far it is from the heart along the way the nest
// grows (x) and how far it stands off the surface under it (y). The mesh shader uses those to grow the
// skin out from the heart, to run each heartbeat out across it as a swell, and to kill it from the heart
// outwards -- all on the one surface, with nothing in the scene per patch.
struct NestPad
{
    glm::vec3 at{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float size = 1.0f;
    float fromHeart = 0.0f;
    float stretch = 1.0f;
    float spin = 0.0f;
};
struct NestRoot
{
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float radiusA = 0.05f;
    float radiusB = 0.03f;
    float fromHeartA = 0.0f;
    float fromHeartB = 0.0f;
};
struct NestSkinPlan
{
    uint32_t seed = 0;
    std::vector<NestPad> pads;
    std::vector<NestRoot> roots;
};
std::vector<MeshData> BuildNestSkin(const NestSkinPlan& plan);

} // namespace pred
