#pragma once

#include "Game/Items/ItemAppearance.h"
#include "Game/Items/ItemDatabase.h"

#include <bgfx/bgfx.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

class MeshLibrary;
class Renderer;
class SceneRenderer;

// Inventory icons, drawn as the items themselves.
//
// Every item is rendered once into a cell of one offscreen texture, using the same mesh and the
// same shader the world uses. The inventory then draws a sub-rectangle of that texture. The point
// is that adding an item to items.json gives it an icon immediately: nobody has to draw one, and an
// icon can never disagree with the thing it stands for.
//
// The atlas is rendered once and kept. Items do not change at runtime, so redrawing it every frame
// would be pure cost; Invalidate() forces a redraw if the item data is ever reloaded.
class ItemIcons
{
public:
    struct Icon
    {
        glm::vec2 uv0{0.0f, 0.0f};
        glm::vec2 uv1{1.0f, 1.0f};
        bool valid = false;
    };

    ~ItemIcons();

    // Uploads a mesh per item and creates the render target. Safe to call again after a reload.
    bool Build(const ItemDatabase& items, MeshLibrary& meshes, const Renderer& renderer,
               int cellPixels = 128);
    void Shutdown();
    void Invalidate() { m_rendered = false; }

    // Must be called inside a frame, before the UI is submitted. Does nothing once the atlas holds
    // a rendered image.
    void Render(SceneRenderer& sceneRenderer, const MeshLibrary& meshes);

    bool IsReady() const { return m_rendered; }
    bgfx::TextureHandle Texture() const { return m_texture; }
    // Returns nullptr for an item with no icon, so callers fall back rather than drawing garbage.
    const Icon* Find(ItemId item) const;

private:
    struct Entry
    {
        ItemId item = kInvalidItem;
        MeshHandle mesh;
        Material material;
        // Where the camera sits and what it looks at, worked out from the mesh bounds so items of
        // very different sizes all fill their cell.
        glm::vec3 eye{0.0f};
        glm::vec3 focus{0.0f};
        float nearPlane = 0.01f;
        float farPlane = 10.0f;
        Icon icon;
    };

    std::vector<Entry> m_entries;
    bgfx::FrameBufferHandle m_framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_texture = BGFX_INVALID_HANDLE;
    int m_cellPixels = 128;
    int m_columns = 1;
    int m_size = 128;
    bool m_homogeneousDepth = false;
    bool m_rendered = false;
};

} // namespace pred
