#include "Engine/Render/LampShadows.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

glm::vec3 LampFaceForward(int face)
{
    switch (face)
    {
    case 0: return {1.0f, 0.0f, 0.0f};
    case 1: return {-1.0f, 0.0f, 0.0f};
    case 2: return {0.0f, 1.0f, 0.0f};
    case 3: return {0.0f, -1.0f, 0.0f};
    case 4: return {0.0f, 0.0f, 1.0f};
    default: return {0.0f, 0.0f, -1.0f};
    }
}

glm::vec3 LampFaceUp(int face)
{
    // Anything not along the face itself; straight up for the four sideways faces.
    switch (face)
    {
    case 2: return {0.0f, 0.0f, 1.0f};
    case 3: return {0.0f, 0.0f, -1.0f};
    default: return {0.0f, 1.0f, 0.0f};
    }
}

bool LampShadows::Init(bgfx::ProgramHandle depthProgram)
{
    if (!bgfx::isValid(depthProgram))
    {
        return false;
    }
    // Half floats when the card can draw into them: a lamp reaches a few metres, which sixteen bits hold to a
    // few millimetres, at half the memory of a full float.
    const auto renderable = [](bgfx::TextureFormat::Enum format)
    { return (bgfx::getCaps()->formats[format] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) != 0; };
    const bgfx::TextureFormat::Enum format = renderable(bgfx::TextureFormat::R16F) ? bgfx::TextureFormat::R16F : bgfx::TextureFormat::R32F;
    if (!renderable(format))
    {
        PRED_LOG_WARN(Render, "LampShadows: this GPU cannot render to a float target; lamps are kept in their rooms by boxes");
        return false;
    }
    m_program = depthProgram;
    m_uRange = bgfx::createUniform("u_shadowRange", bgfx::UniformType::Vec4);
    // Point sampled, clamped: see ShadowMap. The lookup keeps itself inside its own tile.
    m_texture = bgfx::createTexture2D(kAtlasSize, kAtlasSize, false, 1, format,
                                      BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                          BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(m_texture))
    {
        return false;
    }
    const bgfx::TextureHandle depth =
        bgfx::createTexture2D(kAtlasSize, kAtlasSize, false, 1, bgfx::TextureFormat::D16, BGFX_TEXTURE_RT_WRITE_ONLY);
    const bgfx::TextureHandle attachments[] = {m_texture, depth};
    m_frameBuffer = bgfx::createFrameBuffer(2, attachments, true);
    if (!bgfx::isValid(m_frameBuffer))
    {
        m_texture = BGFX_INVALID_HANDLE;
        return false;
    }
    m_slots.assign(kSlots, Slot{});
    PRED_LOG_INFO(Render, "Lamp shadows ready: {} lamps of six {}-texel faces", kSlots, kTileSize);
    return true;
}

void LampShadows::Shutdown()
{
    if (bgfx::isValid(m_frameBuffer))
    {
        bgfx::destroy(m_frameBuffer);
    }
    m_frameBuffer = BGFX_INVALID_HANDLE;
    m_texture = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(m_uRange))
    {
        bgfx::destroy(m_uRange);
    }
    m_uRange = BGFX_INVALID_HANDLE;
    m_slots.clear();
}

int LampShadows::SlotFor(uint32_t key) const
{
    if (key == 0)
    {
        return -1;
    }
    for (size_t i = 0; i < m_slots.size(); ++i)
    {
        const Slot& slot = m_slots[i];
        // Drawn in an earlier frame: this frame's views run after the picture has been drawn.
        if (slot.key == key && slot.drawn && slot.drawnFrame < m_frame)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void LampShadows::Invalidate(const glm::vec3& at, float radius)
{
    for (Slot& slot : m_slots)
    {
        if (slot.key != 0 && glm::distance(slot.position, at) < slot.range + radius)
        {
            slot.dirty = true;
        }
    }
}

void LampShadows::InvalidateAll()
{
    for (Slot& slot : m_slots)
    {
        slot = Slot{};
    }
}

glm::vec4 LampShadows::Params() const
{
    const bgfx::Caps* caps = bgfx::getCaps();
    return {1.0f / static_cast<float>(kTilesAcross), static_cast<float>(kTilesAcross), caps->originBottomLeft ? 1.0f : 0.0f,
            1.0f / static_cast<float>(kTileSize)};
}

void LampShadows::Update(const std::vector<PunctualLight>& lights, const glm::vec3& focus, bgfx::ViewId firstView,
                         const Scene& scene, const MeshLibrary& meshes)
{
    ++m_frame;
    if (!Ready())
    {
        return;
    }
    // The lamps that could matter, nearest first.
    std::vector<std::pair<float, const PunctualLight*>> wanted;
    for (const PunctualLight& light : lights)
    {
        if (light.shadowKey == 0 || light.range <= 0.0f)
        {
            continue;
        }
        const float away = glm::distance(light.position, focus) - light.range;
        if (away < 30.0f)
        {
            wanted.emplace_back(away, &light);
        }
    }
    std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (wanted.size() > static_cast<size_t>(kSlots))
    {
        wanted.resize(static_cast<size_t>(kSlots));
    }

    int drawnThisFrame = 0;
    for (const auto& [away, light] : wanted)
    {
        int slot = -1;
        for (size_t i = 0; i < m_slots.size(); ++i)
        {
            if (m_slots[i].key == light->shadowKey)
            {
                slot = static_cast<int>(i);
            }
        }
        if (slot < 0)
        {
            // An empty slot, or else the one used longest ago that is not wanted this frame.
            uint64_t oldest = UINT64_MAX;
            for (size_t i = 0; i < m_slots.size(); ++i)
            {
                if (m_slots[i].key == 0)
                {
                    slot = static_cast<int>(i);
                    break;
                }
                if (m_slots[i].usedFrame < m_frame && m_slots[i].usedFrame < oldest)
                {
                    oldest = m_slots[i].usedFrame;
                    slot = static_cast<int>(i);
                }
            }
            if (slot < 0)
            {
                continue;
            }
            m_slots[static_cast<size_t>(slot)] = Slot{};
            m_slots[static_cast<size_t>(slot)].key = light->shadowKey;
        }
        Slot& chosen = m_slots[static_cast<size_t>(slot)];
        chosen.usedFrame = m_frame;
        // A lamp that has moved or changed its reach is a different lamp.
        if (chosen.drawn && (glm::distance(chosen.position, light->position) > 0.01f || std::abs(chosen.range - light->range) > 0.01f))
        {
            chosen.drawn = false;
        }
        if ((!chosen.drawn || chosen.dirty) && drawnThisFrame < kLampsPerFrame)
        {
            chosen.position = light->position;
            chosen.range = light->range;
            Draw(slot, static_cast<bgfx::ViewId>(firstView + drawnThisFrame * 6), scene, meshes);
            // A lamp drawn for the first time is used from the next frame; one drawn again keeps being used,
            // the new drawing replacing the old as it is made.
            if (!chosen.drawn)
            {
                chosen.drawnFrame = m_frame;
            }
            chosen.drawn = true;
            chosen.dirty = false;
            ++drawnThisFrame;
        }
    }
}

void LampShadows::Draw(int slotIndex, bgfx::ViewId firstView, const Scene& scene, const MeshLibrary& meshes)
{
    const Slot& slot = m_slots[static_cast<size_t>(slotIndex)];
    const bgfx::Caps* caps = bgfx::getCaps();
    constexpr float kNear = 0.05f;
    const float farPlane = std::max(slot.range, kNear + 0.5f);
    const glm::mat4 projection = caps->homogeneousDepth
                                     ? glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, kNear, farPlane)
                                     : glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, kNear, farPlane);
    for (int face = 0; face < 6; ++face)
    {
        const bgfx::ViewId view = static_cast<bgfx::ViewId>(firstView + face);
        const int tile = slotIndex * 6 + face;
        const uint16_t x = static_cast<uint16_t>((tile % kTilesAcross) * kTileSize);
        const uint16_t y = static_cast<uint16_t>((tile / kTilesAcross) * kTileSize);
        const glm::mat4 lookAt =
            glm::lookAtRH(slot.position, slot.position + LampFaceForward(face), LampFaceUp(face));
        bgfx::setViewFrameBuffer(view, m_frameBuffer);
        bgfx::setViewRect(view, x, y, kTileSize, kTileSize);
        bgfx::setViewScissor(view, x, y, kTileSize, kTileSize);
        // Zero is "as far as this lamp reaches": see fs_shadow.sc.
        bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
        bgfx::setViewTransform(view, glm::value_ptr(lookAt), glm::value_ptr(projection));
        bgfx::touch(view);
    }
    const float range[4] = {farPlane, 0.0f, 0.0f, 0.0f};
    scene.ForEachShadowCaster(
        [&](Entity, const Transform& transform, const MeshRenderer& renderer)
        {
            // The level only: what does not walk about or get picked up. And not the lamp's own fitting, which
            // casts nothing.
            if (!renderer.castsShadow || !renderer.levelGeometry)
            {
                return;
            }
            const Mesh* mesh = meshes.Get(renderer.mesh);
            if (mesh == nullptr || !mesh->IsValid())
            {
                return;
            }
            const glm::mat4 model = transform.Matrix();
            // Only what is within its reach.
            if (mesh->bounds.IsValid())
            {
                const glm::vec3 centre = glm::vec3(model * glm::vec4((mesh->bounds.min + mesh->bounds.max) * 0.5f, 1.0f));
                const float scale = std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                                              glm::length(glm::vec3(model[2]))});
                const float radius = glm::length(mesh->bounds.max - mesh->bounds.min) * 0.5f * scale;
                if (glm::distance(centre, slot.position) > farPlane + radius)
                {
                    return;
                }
            }
            for (int face = 0; face < 6; ++face)
            {
                bgfx::setUniform(m_uRange, range);
                bgfx::setTransform(glm::value_ptr(model));
                if (mesh->IsDynamic())
                {
                    bgfx::setVertexBuffer(0, mesh->dynamicVertexBuffer);
                }
                else
                {
                    bgfx::setVertexBuffer(0, mesh->vertexBuffer);
                }
                bgfx::setIndexBuffer(mesh->indexBuffer);
                // Both sides: a lamp in the middle of a room sees the insides of its walls, and which way
                // round each wall's triangles are wound is a question it has no business asking.
                bgfx::setState(BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
                bgfx::submit(static_cast<bgfx::ViewId>(firstView + face), m_program);
            }
        });
}

} // namespace pred
