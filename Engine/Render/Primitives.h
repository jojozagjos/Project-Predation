#pragma once

#include "Engine/Render/Mesh.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace pred
{

// Procedural geometry generators.
//
// Everything is built at its true world size rather than as a unit shape that gets scaled, because
// non-uniform scale skews normals and complicates physics shape matching. Ask for the size you want.
namespace Primitives
{

// Axis-aligned box centred on the origin.
MeshData Box(const glm::vec3& size);

// Flat quad in the XZ plane facing +Y, centred on the origin, subdivided into a grid.
MeshData Plane(const glm::vec2& size, int subdivisions = 1);

MeshData Sphere(float radius, int segments = 24, int rings = 16);

// Y-axis aligned cylinder centred on the origin.
MeshData Cylinder(float radius, float height, int segments = 24);

// A sphere stretched to different radii along X, Y and Z, centred on the origin. Heads, body segments,
// anything rounder than a box and not the same size every way.
MeshData Ellipsoid(const glm::vec3& radii, int segments = 16, int rings = 10);

// A Y-axis aligned tube with rounded ends, `length` from tip to tip, centred on the origin. Limbs.
MeshData Capsule(float radius, float length, int segments = 12, int rings = 8);

// A Y-axis aligned tube narrowing from `bottomRadius` to `topRadius`, centred on the origin; a cone
// when the top radius is zero. Tails, horns, claws and spines.
MeshData Frustum(float bottomRadius, float topRadius, float height, int segments = 12);

// A staircase climbing along +Z, with its bottom step's front face at z = 0 and its base at y = 0.
MeshData Stairs(int stepCount, float stepWidth, float stepRise, float stepRun);

// A wedge ramp: base rectangle at y = 0, rising along +Z from 0 to height. Centred on X.
MeshData Ramp(float width, float length, float height);

} // namespace Primitives
} // namespace pred
