#pragma once

#include "Engine/Scene/Scene.h"

#include <bgfx/bgfx.h>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

class MeshLibrary;

// Shadows for the lamps that stay where they are: every lamp in the level sees the walls round it, in all
// six directions, and a surface it cannot see it does not light.
//
// The lamps used to be kept in their rooms by boxes instead -- a lamp lit what was inside its box and
// nothing outside -- which stopped them shining through walls and could never make light come through a
// doorway and fade: every doorway, every opening between one box and the next, was a hard edge between one
// lamp's light and another's. With a real shadow there are no boxes and no edges.
//
// A lamp and the walls round it do not move, so its six faces are drawn once, into their own tiles of one
// big atlas, and kept. A few lamps are drawn each frame, nearest the camera first; one waiting its turn is
// lit the old way, in its box. Something that does change -- a door swinging -- has the lamps near it
// drawn again. Only what blocks the sky is drawn into them, which is the level and its doors: people and
// creatures have the torch to cast their shadows, and a lamp's shadow of somebody who has moved on would
// be left on the wall. So only what is marked as part of the level is drawn into them.
class LampShadows
{
public:
    // Six square tiles per lamp, this many texels a side, in an atlas this many tiles across.
    static constexpr uint16_t kTileSize = 256;
    static constexpr uint16_t kTilesAcross = 16;
    static constexpr uint16_t kAtlasSize = kTileSize * kTilesAcross;
    static constexpr int kSlots = (kTilesAcross * kTilesAcross) / 6;
    // How many lamps are drawn in a frame at most, and so how many views that takes.
    static constexpr int kLampsPerFrame = 2;
    static constexpr int kViewsNeeded = kLampsPerFrame * 6;

    bool Init(bgfx::ProgramHandle depthProgram);
    void Shutdown();
    bool Ready() const { return bgfx::isValid(m_frameBuffer); }
    bgfx::TextureHandle Texture() const { return m_texture; }

    // Chooses which lamps have shadows, from the lights in the scene with a shadow key, nearest `focus`
    // first; draws the next few that need it into views starting at `firstView`. A lamp drawn this frame
    // is used from the next, once its views have run.
    void Update(const std::vector<PunctualLight>& lights, const glm::vec3& focus, bgfx::ViewId firstView,
                const Scene& scene, const MeshLibrary& meshes);

    // The slot a lamp's shadow is in, or -1 while it has none ready.
    int SlotFor(uint32_t key) const;
    // Draw again every lamp that could see anything within `radius` of `at`: a door has moved there.
    void Invalidate(const glm::vec3& at, float radius);
    void InvalidateAll();

    // x = one tile's width in texture coordinates, y = tiles across, z = 1 when textures start at the
    // bottom (the texture origin), w = one texel of a tile, in the tile's own coordinates.
    glm::vec4 Params() const;

private:
    struct Slot
    {
        uint32_t key = 0;
        glm::vec3 position{0.0f};
        float range = 0.0f;
        uint64_t drawnFrame = 0;
        uint64_t usedFrame = 0;
        bool drawn = false;
        // Wants drawing again -- a door near it has moved -- but the old one is still used till then:
        // dropped at once, the lamp fell back to its box for a moment every time a door moved.
        bool dirty = false;
    };
    void Draw(int slot, bgfx::ViewId firstView, const Scene& scene, const MeshLibrary& meshes);

    std::vector<Slot> m_slots;
    bgfx::FrameBufferHandle m_frameBuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_texture = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uRange = BGFX_INVALID_HANDLE;
    uint64_t m_frame = 1;
};

// The six faces, in the order their tiles are laid out: which way each looks, and which way is up in it.
// The shader has the same table (fs_mesh.sc), and the two must agree.
glm::vec3 LampFaceForward(int face);
glm::vec3 LampFaceUp(int face);

} // namespace pred
