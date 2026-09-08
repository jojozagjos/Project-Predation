#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <limits>

namespace pred
{

// Axis-aligned bounding box. Default-constructed boxes are "empty": min > max, so the
// first Expand() sets both ends. Used for mesh bounds, culling, and physics shape fitting.
struct AABB
{
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    bool IsValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

    void Expand(const glm::vec3& point)
    {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    void Expand(const AABB& other)
    {
        if (other.IsValid())
        {
            min = glm::min(min, other.min);
            max = glm::max(max, other.max);
        }
    }

    glm::vec3 Center() const { return IsValid() ? (min + max) * 0.5f : glm::vec3(0.0f); }
    glm::vec3 Size() const { return IsValid() ? (max - min) : glm::vec3(0.0f); }
    glm::vec3 HalfExtents() const { return Size() * 0.5f; }

    float BoundingRadius() const { return IsValid() ? glm::length(HalfExtents()) : 0.0f; }
};

// Position, rotation, scale. The engine is right-handed with +Y up.
struct Transform
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity (w, x, y, z)
    glm::vec3 scale{1.0f};

    glm::mat4 Matrix() const
    {
        glm::mat4 m = glm::mat4_cast(rotation);
        m[0] *= scale.x;
        m[1] *= scale.y;
        m[2] *= scale.z;
        m[3] = glm::vec4(position, 1.0f);
        return m;
    }

    static Transform At(const glm::vec3& position) { return Transform{position}; }
};

} // namespace pred
