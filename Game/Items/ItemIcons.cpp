#include "Game/Items/ItemIcons.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Renderer.h"
#include "Engine/Render/SceneRenderer.h"
#include "Engine/Scene/Scene.h"
#include "Game/Weapons/WeaponDatabase.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

// A three-quarter view from above and to one side. A straight-on shot of a box is a rectangle and
// tells you nothing, whereas this shows three faces and reads as an object.
const glm::vec3 kViewDirection = glm::normalize(glm::vec3(0.62f, 0.48f, 1.0f));
constexpr float kVerticalFov = 0.5236f; // 30 degrees

// Lit for legibility rather than for atmosphere: the world's night lighting would leave every icon
// a dark silhouette. No fog, because there is no distance to fall off over.
Environment IconEnvironment()
{
    Environment environment;
    environment.sunDirection = glm::normalize(glm::vec3(-0.4f, -0.7f, -0.55f));
    environment.sunColor = {1.0f, 0.98f, 0.94f};
    environment.sunIntensity = 2.6f;
    environment.ambientSky = {0.34f, 0.36f, 0.42f};
    environment.ambientGround = {0.14f, 0.14f, 0.16f};
    environment.fogStart = 1.0e6f;
    environment.fogEnd = 1.0e7f;
    return environment;
}

} // namespace

ItemIcons::~ItemIcons()
{
    Shutdown();
}

bool ItemIcons::Build(const ItemDatabase& items, MeshLibrary& meshes, const Renderer& renderer,
                      const WeaponDatabase* weapons, TextureLibrary* textures, int cellPixels)
{
    Shutdown();

    m_cellPixels = std::clamp(cellPixels, 32, 256);
    m_homogeneousDepth = renderer.HomogeneousDepth();

    for (const ItemDefinition& definition : items.All())
    {
        if (definition.id == kInvalidItem)
        {
            continue; // index 0 is the placeholder for "no item"
        }

        std::vector<ItemPart> parts = ItemParts(definition, weapons, textures);
        Entry entry;
        entry.item = definition.id;
        MeshData whole;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (parts[i].mesh.indices.empty())
            {
                continue;
            }
            whole.Append(parts[i].mesh, parts[i].transform);
            Entry::Part part;
            part.mesh = meshes.Upload(parts[i].mesh, "icon_" + definition.key + "_" + std::to_string(i));
            part.material = parts[i].material;
            part.transform = parts[i].transform;
            entry.parts.push_back(part);
        }
        if (entry.parts.empty())
        {
            continue;
        }

        // Seen from the side when it is a weapon or anything long, the way a weapon is always shown:
        // from three-quarters a rifle is foreshortened into something a third of its length, a small
        // dark smudge in the corner of its cell. Barrel to the right, a little from above -- every
        // weapon model has its barrel down +Z, so one view suits them all. Anything else keeps the
        // three-quarter view, which is what makes a box read as a box.
        const AABB bounds = whole.ComputeBounds();
        const glm::vec3 size = bounds.max - bounds.min;
        const bool weapon = weapons != nullptr && weapons->Get(weapons->ForItem(definition.key)) != nullptr;
        const bool longways = weapon || size.z > 2.0f * std::max(size.x, size.y);
        const glm::vec3 direction =
            longways ? glm::normalize(glm::vec3(-1.0f, 0.28f, 0.18f)) : kViewDirection;

        // Framed to what the camera actually sees rather than to a sphere round the item. The
        // sphere of a long thin thing is mostly empty, and fitting that left every weapon at a third
        // of its cell. The box's corners are projected onto the view and the widest of them fills
        // the frame.
        const glm::vec3 centre = (bounds.min + bounds.max) * 0.5f;
        const glm::vec3 right = glm::normalize(glm::cross(-direction, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 up = glm::cross(right, -direction);
        float halfWidth = 1e-3f;
        float halfDepth = 1e-3f;
        for (int corner = 0; corner < 8; ++corner)
        {
            const glm::vec3 point{(corner & 1) ? bounds.max.x : bounds.min.x,
                                  (corner & 2) ? bounds.max.y : bounds.min.y,
                                  (corner & 4) ? bounds.max.z : bounds.min.z};
            const glm::vec3 offset = point - centre;
            halfWidth = std::max({halfWidth, std::abs(glm::dot(offset, right)),
                                 std::abs(glm::dot(offset, up))});
            halfDepth = std::max(halfDepth, std::abs(glm::dot(offset, direction)));
        }
        const float radius = std::max(glm::length(size) * 0.5f, 1e-3f);
        const float distance = halfWidth / std::tan(kVerticalFov * 0.5f) * 1.12f + halfDepth;

        entry.focus = centre;
        entry.eye = centre + direction * distance;
        entry.nearPlane = std::max(distance - radius * 2.0f, radius * 0.01f);
        entry.farPlane = distance + radius * 2.0f;
        m_entries.push_back(entry);
    }

    if (m_entries.empty())
    {
        return true; // nothing to draw is not a failure
    }

    if (m_entries.size() > Renderer::kViewOffscreenCount)
    {
        // Each cell needs its own view, because a view carries one viewport. Rather than silently
        // dropping icons, say so: the fix is to widen the reserved block.
        PRED_LOG_WARN(Render, "Item icons: {} items but only {} offscreen views; the rest have no icon",
                      m_entries.size(), Renderer::kViewOffscreenCount);
        m_entries.resize(Renderer::kViewOffscreenCount);
    }

    m_columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(m_entries.size()))));
    const int rows = static_cast<int>((m_entries.size() + m_columns - 1) / static_cast<size_t>(m_columns));
    m_size = m_cellPixels * std::max(m_columns, rows);

    const auto dimension = static_cast<uint16_t>(m_size);
    constexpr uint64_t kColorFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    bgfx::TextureHandle attachments[2];
    attachments[0] =
        bgfx::createTexture2D(dimension, dimension, false, 1, bgfx::TextureFormat::BGRA8, kColorFlags);
    // Without a depth attachment the near faces of a box would not reliably cover the far ones.
    attachments[1] = bgfx::createTexture2D(dimension, dimension, false, 1, bgfx::TextureFormat::D24S8,
                                           BGFX_TEXTURE_RT_WRITE_ONLY);
    if (!bgfx::isValid(attachments[0]) || !bgfx::isValid(attachments[1]))
    {
        PRED_LOG_ERROR(Render, "Item icons: could not create a {}x{} render target", m_size, m_size);
        Shutdown();
        return false;
    }

    m_framebuffer = bgfx::createFrameBuffer(2, attachments, true);
    if (!bgfx::isValid(m_framebuffer))
    {
        PRED_LOG_ERROR(Render, "Item icons: could not create the render target framebuffer");
        Shutdown();
        return false;
    }
    m_texture = attachments[0];

    // UVs are worked out once, here, rather than at every draw.
    const float cell = static_cast<float>(m_cellPixels) / static_cast<float>(m_size);
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const auto column = static_cast<float>(i % static_cast<size_t>(m_columns));
        const auto row = static_cast<float>(i / static_cast<size_t>(m_columns));
        m_entries[i].icon.uv0 = {column * cell, row * cell};
        m_entries[i].icon.uv1 = {(column + 1.0f) * cell, (row + 1.0f) * cell};
        m_entries[i].icon.valid = true;
    }

    PRED_LOG_INFO(Render, "Item icons: {} items in a {}x{} atlas", m_entries.size(), m_size, m_size);
    return true;
}

void ItemIcons::Shutdown()
{
    if (bgfx::isValid(m_framebuffer))
    {
        // The framebuffer owns its attachments, so destroying it destroys the texture too.
        bgfx::destroy(m_framebuffer);
        m_framebuffer = BGFX_INVALID_HANDLE;
    }
    m_texture = BGFX_INVALID_HANDLE;
    m_entries.clear();
    m_rendered = false;
}

void ItemIcons::Render(SceneRenderer& sceneRenderer, const MeshLibrary& meshes)
{
    if (m_rendered || !bgfx::isValid(m_framebuffer) || m_entries.empty())
    {
        return;
    }

    const Environment environment = IconEnvironment();
    const auto cell = static_cast<uint16_t>(m_cellPixels);

    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const Entry& entry = m_entries[i];

        const auto view = static_cast<bgfx::ViewId>(Renderer::kViewOffscreenFirst + i);
        const auto column = static_cast<uint16_t>(i % static_cast<size_t>(m_columns));
        const auto row = static_cast<uint16_t>(i / static_cast<size_t>(m_columns));

        bgfx::setViewFrameBuffer(view, m_framebuffer);
        bgfx::setViewRect(view, static_cast<uint16_t>(column * cell), static_cast<uint16_t>(row * cell),
                          cell, cell);
        // Cleared to fully transparent so the panel behind shows through around the item.
        bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);

        const glm::mat4 viewMatrix = glm::lookAtRH(entry.eye, entry.focus, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection =
            m_homogeneousDepth
                ? glm::perspectiveRH_NO(kVerticalFov, 1.0f, entry.nearPlane, entry.farPlane)
                : glm::perspectiveRH_ZO(kVerticalFov, 1.0f, entry.nearPlane, entry.farPlane);
        bgfx::setViewTransform(view, glm::value_ptr(viewMatrix), glm::value_ptr(projection));

        for (const Entry::Part& part : entry.parts)
        {
            const Mesh* mesh = meshes.Get(part.mesh);
            if (mesh != nullptr && mesh->IsValid())
            {
                sceneRenderer.DrawOne(view, *mesh, part.material, part.transform, environment,
                                      entry.eye);
            }
        }
    }

    m_rendered = true;
}

const ItemIcons::Icon* ItemIcons::Find(ItemId item) const
{
    for (const Entry& entry : m_entries)
    {
        if (entry.item == item && entry.icon.valid)
        {
            return &entry.icon;
        }
    }
    return nullptr;
}

} // namespace pred
