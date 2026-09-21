#include "Game/Weapons/BulletHole.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <cstdint>

namespace pred
{

MeshData BuildBulletHoleShape()
{
    // A crater, not a disc.
    //
    // It was one flat fan of triangles all facing straight out of the wall, so every hole was a
    // black circle of exactly the same shade as every other, whatever the light was doing. What
    // makes a real one read as a hole is that it is not flat: the rim catches the light and the pit
    // does not, and which part is which changes as you move.
    //
    // So there are three rings. A lip that stands slightly proud and faces a little outwards, a wall
    // that turns down into the pit, and a floor at the bottom facing back up the way the round came
    // in. One material and one colour, because the shape is doing the work.
    //
    // The wall's normals face inwards, towards the pit. They faced outwards at first, which is the
    // normal of a bump rather than of a bowl, and up close every hole read as a small black dome
    // stuck to the wall with a highlight on the wrong side. Facing inwards, the side of the pit
    // towards a light is the dark one and the far side catches it, which is how the eye reads a
    // hole rather than a bump.
    //
    // Shallow on purpose. The depth is barely visible at any distance -- the normals are what show
    // it -- and every millimetre of it is a millimetre the whole mark has to stand off the surface so
    // that the floor is not inside the wall.
    constexpr int kSides = 20;
    constexpr float kLip = 0.024f;    // the outer edge, on the surface
    constexpr float kMouth = 0.015f;  // the steepest part of the wall
    constexpr float kFloor = 0.007f;  // the pit
    constexpr float kProud = 0.0008f; // how far the lip stands out
    constexpr float kDeep = 0.003f;   // and how far the floor is sunk

    MeshData shape;
    const auto ring = [&](float radius, float height, const glm::vec3& normal)
    {
        for (int i = 0; i < kSides; ++i)
        {
            const float angle = glm::two_pi<float>() * static_cast<float>(i) / kSides;
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            MeshVertex vertex;
            vertex.position = {c * radius, height, s * radius};
            // Turned outwards with the ring, so the lip lights up on the side facing a lamp and
            // stays dark on the other -- which is the whole point of it being raised.
            vertex.normal = glm::normalize(glm::vec3(c * normal.x, normal.y, s * normal.x));
            vertex.uv = {0.5f + 0.5f * c, 0.5f + 0.5f * s};
            shape.vertices.push_back(vertex);
        }
    };

    MeshVertex pit;
    pit.position = {0.0f, -kDeep, 0.0f};
    pit.normal = {0.0f, 1.0f, 0.0f};
    pit.uv = {0.5f, 0.5f};
    shape.vertices.push_back(pit); // 0

    // Normals as (outwards, up). Negative outwards is inwards, towards the pit.
    ring(kFloor, -kDeep, {-0.35f, 0.94f, 0.0f});        // 1: the floor starting to turn up
    ring(kMouth, -kDeep * 0.35f, {-0.70f, 0.71f, 0.0f}); // 1 + kSides: the wall, facing the pit
    ring(kLip, kProud, {0.25f, 0.97f, 0.0f});           // 1 + 2 * kSides: the crest of the lip

    // Wound so every face points along +Y, which is the way placement expects: it turns +Y onto the
    // surface normal.
    for (int i = 0; i < kSides; ++i)
    {
        const auto next = static_cast<uint32_t>((i + 1) % kSides);
        const auto here = static_cast<uint32_t>(i);
        shape.indices.push_back(0);
        shape.indices.push_back(1 + next);
        shape.indices.push_back(1 + here);
        for (uint32_t band = 0; band < 2; ++band)
        {
            const uint32_t inner = 1 + band * kSides;
            const uint32_t outer = 1 + (band + 1) * kSides;
            shape.indices.push_back(inner + here);
            shape.indices.push_back(inner + next);
            shape.indices.push_back(outer + next);
            shape.indices.push_back(inner + here);
            shape.indices.push_back(outer + next);
            shape.indices.push_back(outer + here);
        }
    }
    return shape;
}

BulletHolePlacement ConformBulletHole(const MeshData& shape, const PhysicsWorld& physics,
                                      const glm::vec3& at, const glm::vec3& normal, float spin,
                                      float size)
{
    BulletHolePlacement placed;
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const glm::vec3 out =
        glm::length(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);

    // The shortest turn from the shape's +Y onto the surface normal. Straight down has no shortest
    // turn, and a half turn about any horizontal axis will do there.
    const glm::quat face = glm::dot(up, out) < -0.9999f
                               ? glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f))
                               : glm::rotation(up, out);
    placed.rotation = face * glm::angleAxis(spin, up);
    placed.position = at;
    const glm::quat toLocal = glm::inverse(placed.rotation);

    // How far either side of the tangent plane to look for the surface. Comfortably more than a
    // five-centimetre mark sags on the tightest curve in the level -- a pillar a little over half a
    // metre across sags under two millimetres -- and short enough not to find the wall behind.
    constexpr float kProbe = 0.04f;
    // What a vertex with nothing under it tries next, as a fraction of how far out it was. The last
    // is the centre, which always has the surface under it because that is where the round landed.
    constexpr float kPulls[] = {1.0f, 0.75f, 0.5f, 0.25f, 0.0f};

    placed.mesh = shape;
    for (MeshVertex& vertex : placed.mesh.vertices)
    {
        const glm::vec3 footprint{vertex.position.x * size, 0.0f, vertex.position.z * size};
        const float height = vertex.position.y * size;
        const glm::vec3 shapeNormal = placed.rotation * vertex.normal;

        glm::vec3 surface = at + placed.rotation * footprint;
        glm::vec3 surfaceNormal = out;
        bool found = false;
        for (const float pull : kPulls)
        {
            const glm::vec3 above = at + placed.rotation * (footprint * pull) + out * kProbe;
            const RayHit hit = physics.RayCast(above, -out, kProbe * 2.0f);
            // A hit at the very start of the ray is a ray that began inside something -- the wall
            // beside a hole in the floor, say -- and says nothing about the surface under the mark.
            // A hit facing away from the mark is the far side of something thin.
            if (hit && hit.distance > 1e-4f && glm::dot(hit.normal, out) > 0.2f)
            {
                surface = hit.position;
                surfaceNormal = glm::normalize(hit.normal);
                found = true;
                if (pull < 1.0f)
                {
                    ++placed.pulledIn;
                }
                else
                {
                    ++placed.conformed;
                }
                break;
            }
        }
        // Nothing anywhere under it, which only happens when the round's own point cannot be found
        // again: the flat placement is the best that is left, and is what every hole used to be.

        // Standing off the surface along the surface's own normal at that point rather than the
        // normal at the centre, which is what makes the mark wrap rather than lie on the curve.
        const glm::quat bend = found ? glm::rotation(out, surfaceNormal) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::vec3 world = surface + surfaceNormal * (kBulletHoleStandOff + height);
        vertex.position = toLocal * (world - at);
        vertex.normal = glm::normalize(toLocal * (bend * shapeNormal));
    }
    return placed;
}

} // namespace pred
