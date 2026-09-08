#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace pred
{

class Input;

// Free-flying developer camera. Right-handed, +Y up, looks down -Z at yaw 0.
// The player camera in Phase 2 is a separate class; this one is for the
// test map and tools.
class FlyCamera
{
public:
    glm::vec3 position{0.0f, 1.7f, 6.0f};
    float yaw = 0.0f;   // radians, positive turns right
    float pitch = 0.0f; // radians, positive looks up
    float fovDegrees = 90.0f;
    float nearPlane = 0.05f;
    float farPlane = 500.0f;
    float moveSpeed = 4.0f;
    float fastMultiplier = 4.0f;
    float slowMultiplier = 0.25f;
    float mouseSensitivity = 0.12f; // degrees per pixel

    // Applies look (when looking is true) and movement actions from the input state.
    void Update(const Input& input, float dt, bool looking);

    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::mat4 View() const;
    glm::mat4 Projection(float aspect, bool homogeneousDepth) const;
};

} // namespace pred
