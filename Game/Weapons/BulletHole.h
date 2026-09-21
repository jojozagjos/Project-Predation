#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace pred
{

// The shape of a bullet hole, and the laying of one onto whatever it landed on.
//
// Separate from the game so it can be tested against a real collider without a renderer: the thing
// that goes wrong with a hole is where its vertices end up relative to the surface, and that is
// arithmetic plus a ray cast.

// How far the whole mark stands off the surface. More than the pit is deep, or the floor of the
// crater would be inside the wall and the depth test would hide it; enough that the two do not fight
// for the same pixels as the angle changes.
inline constexpr float kBulletHoleStandOff = 0.0045f;

// The crater, lying in XZ and facing +Y, about five centimetres across: a lip that stands slightly
// proud and faces outwards, a wall that turns down, and a floor sunk below the lip. The shading
// comes from the normals rather than from a texture, which is what makes it read as a hole rather
// than as a black sticker.
MeshData BuildBulletHoleShape();

struct BulletHolePlacement
{
    // Where the entity goes. Its mesh is in this frame.
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    MeshData mesh;
    // How many vertices found the surface under them, and how many had to be pulled in because they
    // were past an edge. Reported for tests and for the debug view.
    int conformed = 0;
    int pulledIn = 0;
};

// Lays the shape onto the surface around `at`.
//
// It used to be laid flat: one disc on the tangent plane at the point of impact. On a wall that is
// exact. On anything round it is a flat plate stuck to a curve, and on the level's cylinders and
// sphere -- which are triangle meshes -- it was also a plate lying on whichever facet the round
// struck, so near the top of the sphere it lay flat as a table while the surface around it fell
// away.
//
// So every vertex is dropped onto the real surface under it, along the normal, and its height
// above the surface is measured from there along that surface's own normal. The mark wraps a curve
// and follows facets exactly as they are drawn. A vertex with nothing under it -- the mark has
// landed near an edge -- is pulled in to where the ring inside it landed, so the mark shrinks at a
// corner rather than hanging off it in the air.
//
// `spin` turns the mark about its normal and `size` scales it, so a wall with a magazine in it
// does not read as one stamp repeated.
BulletHolePlacement ConformBulletHole(const MeshData& shape, const PhysicsWorld& physics,
                                      const glm::vec3& at, const glm::vec3& normal, float spin,
                                      float size);

} // namespace pred
