#pragma once

#include <bgfx/bgfx.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace pred
{

class ShaderLibrary;
struct Environment;

// The sky, drawn as one triangle behind everything else.
//
// Not a box and not a sphere. Either would need geometry, a size chosen against the far plane, and
// care that the player never reaches the edge of it; a triangle at the far plane needs none of that,
// because the sky is not a thing in the world -- it is what is left where nothing in the world was
// drawn. The colours come from the same Environment the lighting uses, so the sky a player sees and
// the sky the surfaces reflect are the same sky rather than two things that have to be kept in step.
class SkyRenderer
{
public:
    bool Init(ShaderLibrary& shaders);
    void Shutdown();
    bool IsValid() const { return bgfx::isValid(m_program); }

    // `view` and `projection` are the camera's. The translation is taken out of the view: the sky is
    // infinitely far away, so walking must not move it.
    void Draw(bgfx::ViewId view, const Environment& environment, const glm::mat4& viewMatrix,
              const glm::mat4& projection);

private:
    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle m_triangle = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout m_layout;

    bgfx::UniformHandle m_uRays = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uZenith = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uHorizon = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uGround = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSun = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uSunColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uGrade = BGFX_INVALID_HANDLE;
};

} // namespace pred
