#include "Engine/Render/SceneRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/ShaderLibrary.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/type_ptr.hpp>

namespace pred
{

bool SceneRenderer::Init(ShaderLibrary& shaders)
{
    m_program = shaders.LoadProgram("vs_mesh", "fs_mesh");
    if (!bgfx::isValid(m_program))
    {
        PRED_LOG_ERROR(Render, "SceneRenderer: failed to load the mesh program");
        return false;
    }

    m_uBaseColor = bgfx::createUniform("u_baseColor", bgfx::UniformType::Vec4);
    m_uMaterialParams = bgfx::createUniform("u_materialParams", bgfx::UniformType::Vec4);
    m_uEmissive = bgfx::createUniform("u_emissive", bgfx::UniformType::Vec4);
    m_uLightDirection = bgfx::createUniform("u_lightDirection", bgfx::UniformType::Vec4);
    m_uLightColor = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
    m_uAmbientSky = bgfx::createUniform("u_ambientSky", bgfx::UniformType::Vec4);
    m_uAmbientGround = bgfx::createUniform("u_ambientGround", bgfx::UniformType::Vec4);
    m_uFogColor = bgfx::createUniform("u_fogColor", bgfx::UniformType::Vec4);
    m_uFogParams = bgfx::createUniform("u_fogParams", bgfx::UniformType::Vec4);
    m_uCameraPosition = bgfx::createUniform("u_cameraPosition", bgfx::UniformType::Vec4);

    PRED_LOG_INFO(Render, "SceneRenderer initialized");
    return true;
}

void SceneRenderer::Shutdown()
{
    const bgfx::UniformHandle uniforms[] = {m_uBaseColor,     m_uMaterialParams, m_uEmissive,
                                            m_uLightDirection, m_uLightColor,    m_uAmbientSky,
                                            m_uAmbientGround, m_uFogColor,       m_uFogParams,
                                            m_uCameraPosition};
    for (const bgfx::UniformHandle handle : uniforms)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    m_uBaseColor = m_uMaterialParams = m_uEmissive = BGFX_INVALID_HANDLE;
    m_uLightDirection = m_uLightColor = m_uAmbientSky = m_uAmbientGround = BGFX_INVALID_HANDLE;
    m_uFogColor = m_uFogParams = m_uCameraPosition = BGFX_INVALID_HANDLE;
    // The program itself is owned by the ShaderLibrary.
    m_program = BGFX_INVALID_HANDLE;
}

// Uniforms that are the same for every mesh in a pass. Split out so the item icon atlas can draw
// single meshes through the same shader the world uses, rather than approximating them in 2D.
void SceneRenderer::SetEnvironmentUniforms(const Environment& environment, const glm::vec3& cameraPosition)
{
    // Shader wants the direction *towards* the light, which is the opposite of travel direction.
    const glm::vec3 towardsLight = glm::normalize(-environment.sunDirection);
    const float lightDirection[4] = {towardsLight.x, towardsLight.y, towardsLight.z, 0.0f};
    const float lightColor[4] = {environment.sunColor.r, environment.sunColor.g, environment.sunColor.b,
                                 environment.sunIntensity};
    const float ambientSky[4] = {environment.ambientSky.r, environment.ambientSky.g, environment.ambientSky.b,
                                 0.0f};
    const float ambientGround[4] = {environment.ambientGround.r, environment.ambientGround.g,
                                    environment.ambientGround.b, 0.0f};
    const float fogColor[4] = {environment.fogColor.r, environment.fogColor.g, environment.fogColor.b, 0.0f};
    const float fogParams[4] = {environment.fogStart, environment.fogEnd, 0.0f, 0.0f};
    const float cameraPos[4] = {cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f};

    bgfx::setUniform(m_uLightDirection, lightDirection);
    bgfx::setUniform(m_uLightColor, lightColor);
    bgfx::setUniform(m_uAmbientSky, ambientSky);
    bgfx::setUniform(m_uAmbientGround, ambientGround);
    bgfx::setUniform(m_uFogColor, fogColor);
    bgfx::setUniform(m_uFogParams, fogParams);
    bgfx::setUniform(m_uCameraPosition, cameraPos);
}

uint64_t SceneRenderer::DrawState() const
{
    // Primitives are wound counter-clockwise when seen from outside (the glTF convention). A
    // right-handed projection flips that to clockwise in screen space, so the winding to discard is
    // CW. Getting this backwards renders the *insides* of objects: surfaces lit by ambient only and
    // single-sided geometry such as the ground plane disappearing entirely.
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                     BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
    if (m_wireframe)
    {
        state |= BGFX_STATE_PT_LINES;
    }
    return state;
}

void SceneRenderer::SubmitMesh(bgfx::ViewId view, const Mesh& mesh, const Material& material,
                               const glm::mat4& model, uint64_t state)
{
    const float baseColor[4] = {material.baseColor.r, material.baseColor.g, material.baseColor.b, 1.0f};
    const float materialParams[4] = {material.metallic, material.roughness, 0.0f, 0.0f};
    const float emissive[4] = {material.emissive.r, material.emissive.g, material.emissive.b, 0.0f};

    bgfx::setUniform(m_uBaseColor, baseColor);
    bgfx::setUniform(m_uMaterialParams, materialParams);
    bgfx::setUniform(m_uEmissive, emissive);

    bgfx::setTransform(glm::value_ptr(model));
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(mesh.indexBuffer);
    bgfx::setState(state);
    bgfx::submit(view, m_program);

    ++m_stats.meshesSubmitted;
    m_stats.trianglesSubmitted += mesh.indexCount / 3;
}

void SceneRenderer::Draw(bgfx::ViewId view, const Scene& scene, const MeshLibrary& meshes,
                         const glm::vec3& cameraPosition)
{
    m_stats = Stats{};
    if (!bgfx::isValid(m_program))
    {
        return;
    }

    SetEnvironmentUniforms(scene.GetEnvironment(), cameraPosition);
    const uint64_t state = DrawState();

    scene.ForEachMeshRenderer(
        [&](Entity, const Transform& transform, const MeshRenderer& renderer)
        {
            const Mesh* mesh = meshes.Get(renderer.mesh);
            if (mesh == nullptr || !mesh->IsValid())
            {
                return;
            }
            SubmitMesh(view, *mesh, renderer.material, transform.Matrix(), state);
        });
}

void SceneRenderer::DrawOne(bgfx::ViewId view, const Mesh& mesh, const Material& material,
                            const glm::mat4& model, const Environment& environment,
                            const glm::vec3& cameraPosition)
{
    if (!bgfx::isValid(m_program) || !mesh.IsValid())
    {
        return;
    }
    SetEnvironmentUniforms(environment, cameraPosition);
    SubmitMesh(view, mesh, material, model, DrawState());
}

} // namespace pred
