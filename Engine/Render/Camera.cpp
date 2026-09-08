#include "Engine/Render/Camera.h"

#include "Engine/Platform/Input.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

void FlyCamera::Update(const Input& input, float dt, bool looking)
{
    if (looking)
    {
        const glm::vec2 delta = input.MouseDelta();
        const float sensitivityRad = glm::radians(mouseSensitivity);
        yaw += delta.x * sensitivityRad;
        pitch -= delta.y * sensitivityRad;
        const float limit = glm::radians(89.0f);
        pitch = std::clamp(pitch, -limit, limit);
        if (yaw > glm::pi<float>())
        {
            yaw -= glm::two_pi<float>();
        }
        else if (yaw < -glm::pi<float>())
        {
            yaw += glm::two_pi<float>();
        }
    }

    glm::vec3 move(0.0f);
    if (input.IsActionDown("move_forward"))
    {
        move += Forward();
    }
    if (input.IsActionDown("move_back"))
    {
        move -= Forward();
    }
    if (input.IsActionDown("move_right"))
    {
        move += Right();
    }
    if (input.IsActionDown("move_left"))
    {
        move -= Right();
    }
    if (input.IsActionDown("move_up"))
    {
        move += glm::vec3(0.0f, 1.0f, 0.0f);
    }
    if (input.IsActionDown("move_down"))
    {
        move -= glm::vec3(0.0f, 1.0f, 0.0f);
    }

    const float length = glm::length(move);
    if (length > 0.0f)
    {
        float speed = moveSpeed;
        if (input.IsActionDown("sprint"))
        {
            speed *= fastMultiplier;
        }
        if (input.IsActionDown("walk"))
        {
            speed *= slowMultiplier;
        }
        position += (move / length) * speed * dt;
    }
}

glm::vec3 FlyCamera::Forward() const
{
    const float cp = std::cos(pitch);
    return glm::vec3(std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp);
}

glm::vec3 FlyCamera::Right() const
{
    return glm::normalize(glm::cross(Forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::mat4 FlyCamera::View() const
{
    return glm::lookAtRH(position, position + Forward(), glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 FlyCamera::Projection(float aspect, bool homogeneousDepth) const
{
    const float fov = glm::radians(fovDegrees);
    return homogeneousDepth ? glm::perspectiveRH_NO(fov, aspect, nearPlane, farPlane)
                            : glm::perspectiveRH_ZO(fov, aspect, nearPlane, farPlane);
}

} // namespace pred
