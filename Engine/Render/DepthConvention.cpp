#include "Engine/Render/DepthConvention.h"

#include <glm/gtc/matrix_transform.hpp>

namespace pred
{

namespace Depth
{

namespace
{

bool g_homogeneous = false;
bool g_reversed = false;

} // namespace

void Configure(bool homogeneous, bool floatTargets)
{
    g_homogeneous = homogeneous;
    g_reversed = !homogeneous && floatTargets;
}

bool Reversed()
{
    return g_reversed;
}

uint64_t Test()
{
    return g_reversed ? BGFX_STATE_DEPTH_TEST_GREATER : BGFX_STATE_DEPTH_TEST_LESS;
}

float Clear()
{
    return g_reversed ? 0.0f : 1.0f;
}

bgfx::TextureFormat::Enum Format()
{
    return g_reversed ? bgfx::TextureFormat::D32F : bgfx::TextureFormat::D24S8;
}

glm::mat4 Perspective(float verticalFov, float aspect, float nearPlane, float farPlane)
{
    if (g_homogeneous)
    {
        return glm::perspectiveRH_NO(verticalFov, aspect, nearPlane, farPlane);
    }
    // Near and far swapped: the near plane goes to 1 and the far to 0.
    return g_reversed ? glm::perspectiveRH_ZO(verticalFov, aspect, farPlane, nearPlane) : glm::perspectiveRH_ZO(verticalFov, aspect, nearPlane, farPlane);
}

} // namespace Depth

} // namespace pred
