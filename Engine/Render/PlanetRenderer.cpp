#include "Engine/Render/PlanetRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/ShaderLibrary.h"

#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>

namespace pred
{

namespace
{

constexpr int kRings = 48;    // the sphere's rings from pole to pole
constexpr int kSegments = 96; // and its segments round
constexpr int kRingSegments = 160;
constexpr float kTau = 6.28318530718f;

} // namespace

bool PlanetRenderer::Init(ShaderLibrary& shaders)
{
    m_program = shaders.LoadProgram("vs_planet", "fs_planet");
    m_lineProgram = shaders.LoadProgram("vs_debug", "fs_debug");
    if (!bgfx::isValid(m_program))
    {
        PRED_LOG_WARN(Render, "PlanetRenderer: no planet program; the system map will be empty");
        return false;
    }
    m_layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    m_lineLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    // A unit sphere: rings of latitude, segments of longitude, a vertex where each meets.
    std::vector<Vertex> vertices;
    for (int ring = 0; ring <= kRings; ++ring)
    {
        const float v = static_cast<float>(ring) / static_cast<float>(kRings);
        const float polar = v * kTau * 0.5f;
        for (int segment = 0; segment <= kSegments; ++segment)
        {
            const float u = static_cast<float>(segment) / static_cast<float>(kSegments);
            const float around = u * kTau;
            const glm::vec3 p{std::sin(polar) * std::cos(around), std::cos(polar), std::sin(polar) * std::sin(around)};
            vertices.push_back({p.x, p.y, p.z, p.x, p.y, p.z, u, v});
        }
    }
    std::vector<uint16_t> indices;
    for (int ring = 0; ring < kRings; ++ring)
    {
        for (int segment = 0; segment < kSegments; ++segment)
        {
            const auto a = static_cast<uint16_t>(ring * (kSegments + 1) + segment);
            const auto b = static_cast<uint16_t>(a + kSegments + 1);
            indices.insert(indices.end(), {a, static_cast<uint16_t>(a + 1), b, b, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(b + 1)});
        }
    }
    m_sphereVertices = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(Vertex))), m_layout);
    m_sphereIndices = bgfx::createIndexBuffer(bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t))));

    m_uA = bgfx::createUniform("u_planetA", bgfx::UniformType::Vec4);
    m_uB = bgfx::createUniform("u_planetB", bgfx::UniformType::Vec4);
    m_uC = bgfx::createUniform("u_planetC", bgfx::UniformType::Vec4);
    m_uD = bgfx::createUniform("u_planetD", bgfx::UniformType::Vec4);
    m_uE = bgfx::createUniform("u_planetE", bgfx::UniformType::Vec4);
    m_uLight = bgfx::createUniform("u_planetLight", bgfx::UniformType::Vec4);
    m_uLightColor = bgfx::createUniform("u_planetLightColor", bgfx::UniformType::Vec4);
    m_uMode = bgfx::createUniform("u_planetMode", bgfx::UniformType::Vec4);
    m_uEye = bgfx::createUniform("u_planetEye", bgfx::UniformType::Vec4);
    return true;
}

void PlanetRenderer::Shutdown()
{
    for (bgfx::UniformHandle* handle : {&m_uA, &m_uB, &m_uC, &m_uD, &m_uE, &m_uLight, &m_uLightColor, &m_uMode, &m_uEye})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
        }
        *handle = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(m_sphereVertices))
    {
        bgfx::destroy(m_sphereVertices);
    }
    if (bgfx::isValid(m_sphereIndices))
    {
        bgfx::destroy(m_sphereIndices);
    }
    m_sphereVertices = BGFX_INVALID_HANDLE;
    m_sphereIndices = BGFX_INVALID_HANDLE;
    // The programs belong to the shader library.
    m_program = BGFX_INVALID_HANDLE;
    m_lineProgram = BGFX_INVALID_HANDLE;
}

void PlanetRenderer::SetCamera(const glm::vec3& eye, float exposure)
{
    m_eye = eye;
    m_exposure = exposure;
}

void PlanetRenderer::SetLook(const PlanetLook& look, const glm::vec3& towardsLight, const glm::vec3& lightColor, float mode,
                             float highlight, float drift)
{
    const glm::vec4 a{look.groundA, look.oceanAmount};
    const glm::vec4 b{look.groundB, look.ice};
    const glm::vec4 c{look.ocean, look.clouds};
    const glm::vec4 d{look.cloudColor, look.gas ? 1.0f : 0.0f};
    const glm::vec4 e{mode > 2.5f ? look.ringColor : look.airColor, look.air};
    const glm::vec4 light{glm::length(towardsLight) > 0.0f ? glm::normalize(towardsLight) : glm::vec3(0.0f, 1.0f, 0.0f), 1.6f};
    const glm::vec4 colour{lightColor, 1.0f};
    const glm::vec4 modeVec{mode, look.seed, drift, highlight};
    const glm::vec4 eye{m_eye, m_exposure};
    bgfx::setUniform(m_uA, glm::value_ptr(a));
    bgfx::setUniform(m_uB, glm::value_ptr(b));
    bgfx::setUniform(m_uC, glm::value_ptr(c));
    bgfx::setUniform(m_uD, glm::value_ptr(d));
    bgfx::setUniform(m_uE, glm::value_ptr(e));
    bgfx::setUniform(m_uLight, glm::value_ptr(light));
    bgfx::setUniform(m_uLightColor, glm::value_ptr(colour));
    bgfx::setUniform(m_uMode, glm::value_ptr(modeVec));
    bgfx::setUniform(m_uEye, glm::value_ptr(eye));
}

void PlanetRenderer::Body(bgfx::ViewId view, const glm::mat4& model, const PlanetLook& look, const glm::vec3& towardsLight,
                          const glm::vec3& lightColor, float highlight, float drift)
{
    if (!IsValid())
    {
        return;
    }
    SetLook(look, towardsLight, lightColor, 0.0f, highlight, drift);
    bgfx::setTransform(glm::value_ptr(model));
    bgfx::setVertexBuffer(0, m_sphereVertices);
    bgfx::setIndexBuffer(m_sphereIndices);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                   BGFX_STATE_MSAA);
    bgfx::submit(view, m_program);
}

void PlanetRenderer::Star(bgfx::ViewId view, bgfx::ViewId glowView, const glm::vec3& at, float radius, const glm::vec3& color,
                          const glm::vec3& cameraRight, const glm::vec3& cameraUp, float time)
{
    if (!IsValid())
    {
        return;
    }
    PlanetLook none;
    glm::mat4 model(radius);
    model[3] = glm::vec4(at, 1.0f);
    SetLook(none, glm::vec3(0.0f, 1.0f, 0.0f), color, 1.0f, 0.0f, time);
    bgfx::setTransform(glm::value_ptr(model));
    bgfx::setVertexBuffer(0, m_sphereVertices);
    bgfx::setIndexBuffer(m_sphereIndices);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                   BGFX_STATE_MSAA);
    bgfx::submit(view, m_program);

    // The glow: a square facing the camera, several times the star's size, added on.
    if (bgfx::getAvailTransientVertexBuffer(6, m_layout) < 6)
    {
        return;
    }
    bgfx::TransientVertexBuffer quad;
    bgfx::allocTransientVertexBuffer(&quad, 6, m_layout);
    const float size = radius * 3.6f;
    const glm::vec3 corners[4] = {at - cameraRight * size - cameraUp * size, at + cameraRight * size - cameraUp * size,
                                  at + cameraRight * size + cameraUp * size, at - cameraRight * size + cameraUp * size};
    const glm::vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const int order[6] = {0, 1, 2, 0, 2, 3};
    auto* out = reinterpret_cast<Vertex*>(quad.data);
    for (int i = 0; i < 6; ++i)
    {
        const glm::vec3& p = corners[order[i]];
        out[i] = {p.x, p.y, p.z, 0.0f, 0.0f, 1.0f, uvs[order[i]].x, uvs[order[i]].y};
    }
    SetLook(none, glm::vec3(0.0f, 1.0f, 0.0f), color, 2.0f, 0.0f, time);
    const glm::mat4 identity(1.0f);
    bgfx::setTransform(glm::value_ptr(identity));
    bgfx::setVertexBuffer(0, &quad);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ADD | BGFX_STATE_MSAA);
    bgfx::submit(glowView, m_program);
}

void PlanetRenderer::Rings(bgfx::ViewId view, const glm::mat4& model, const PlanetLook& look, const glm::vec3& towardsLight,
                           const glm::vec3& lightColor)
{
    if (!IsValid() || look.rings.y <= look.rings.x || look.rings.x <= 0.0f)
    {
        return;
    }
    const uint32_t count = kRingSegments * 6;
    if (bgfx::getAvailTransientVertexBuffer(count, m_layout) < count)
    {
        return;
    }
    bgfx::TransientVertexBuffer strip;
    bgfx::allocTransientVertexBuffer(&strip, count, m_layout);
    auto* out = reinterpret_cast<Vertex*>(strip.data);
    for (int i = 0; i < kRingSegments; ++i)
    {
        const float a0 = static_cast<float>(i) / kRingSegments * kTau;
        const float a1 = static_cast<float>(i + 1) / kRingSegments * kTau;
        const glm::vec3 d0{std::cos(a0), 0.0f, std::sin(a0)};
        const glm::vec3 d1{std::cos(a1), 0.0f, std::sin(a1)};
        const glm::vec3 p[4] = {d0 * look.rings.x, d0 * look.rings.y, d1 * look.rings.y, d1 * look.rings.x};
        const float t[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; ++k)
        {
            const glm::vec3& q = p[order[k]];
            out[i * 6 + k] = {q.x, q.y, q.z, 0.0f, 1.0f, 0.0f, t[order[k]], 0.0f};
        }
    }
    SetLook(look, towardsLight, lightColor, 3.0f, 0.0f, 0.0f);
    bgfx::setTransform(glm::value_ptr(model));
    bgfx::setVertexBuffer(0, &strip);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    bgfx::submit(view, m_program);
}

void PlanetRenderer::Line(const glm::vec3& a, const glm::vec3& b, uint32_t abgr)
{
    m_lines.push_back({a.x, a.y, a.z, abgr});
    m_lines.push_back({b.x, b.y, b.z, abgr});
}

void PlanetRenderer::FlushLines(bgfx::ViewId view)
{
    if (m_lines.empty() || !bgfx::isValid(m_lineProgram))
    {
        m_lines.clear();
        return;
    }
    uint32_t count = static_cast<uint32_t>(m_lines.size());
    count = std::min(count, bgfx::getAvailTransientVertexBuffer(count, m_lineLayout));
    count -= count % 2;
    if (count > 0)
    {
        bgfx::TransientVertexBuffer buffer;
        bgfx::allocTransientVertexBuffer(&buffer, count, m_lineLayout);
        std::memcpy(buffer.data, m_lines.data(), static_cast<size_t>(count) * sizeof(LineVertex));
        const glm::mat4 identity(1.0f);
        bgfx::setTransform(glm::value_ptr(identity));
        bgfx::setVertexBuffer(0, &buffer, 0, count);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA |
                       BGFX_STATE_LINEAA | BGFX_STATE_MSAA);
        bgfx::submit(view, m_lineProgram);
    }
    m_lines.clear();
}

} // namespace pred
