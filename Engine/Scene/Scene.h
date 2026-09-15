#pragma once

#include "Engine/Core/Math.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Entity.h"

#include <glm/vec3.hpp>

#include <array>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pred
{

// Drawn geometry attached to an entity.
struct MeshRenderer
{
    MeshHandle mesh;
    Material material;
    bool visible = true;
    bool castsShadow = true;
    // In the world, but not to be drawn from the camera's own point of view.
    //
    // This is the player's head in first person. It is not absent -- it is over the eye, and its
    // shadow on the ground in front of you is the only part of your own head you can ever see. Made
    // invisible instead, the head vanishes from that shadow and the character is decapitated in the
    // one view where anybody would notice.
    bool hiddenFromCamera = false;
    // Whether this blocks the sky, as opposed to the sun.
    //
    // The sky map answers "is there a roof over this", which is a question about the building. A
    // person standing on open ground does block the sky above the patch they are standing on, and
    // putting them in the map is therefore correct and looks wrong: they get a soft round shadow
    // underneath them as well as the sharp one the sun casts, and two shadows from one body reads
    // as a fault however defensible it is. Static level geometry blocks the sky; things that walk
    // about do not.
    bool blocksSky = true;
};

// A light that has a place, as opposed to the sun, which only has a direction.
//
// One shape covers both a bulb and a torch: a point light is a spot whose cone is the whole sphere.
// Two kinds would mean two loops in the shader and two lists here, to say the same thing twice.
struct PunctualLight
{
    glm::vec3 position{0.0f};
    // Where it points. Ignored when the cone is wide open.
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    // Brightness at the source. Falls off with the square of the distance, cut off at `range`.
    float intensity = 0.0f;
    float range = 10.0f;
    // The cone, in degrees from the axis. Full brightness within the inner angle, fading to nothing
    // at the outer one. 180 makes it a bulb.
    float innerAngle = 18.0f;
    float outerAngle = 30.0f;
    // How big the source is, in metres.
    //
    // Nothing is a mathematical point, and treating a light as one is what blows out everything near
    // it: the inverse square of a tenth of a metre is a hundred. Inside this radius the brightness
    // stops climbing, which is what a source of a real size does. It is the difference between a
    // torch and a flashbulb pressed against whatever is in front of it -- and with the torch on the
    // eye, what is in front of it is the player's own weapon.
    float sourceRadius = 0.6f;
};

inline constexpr size_t kMaxPunctualLights = 4;

struct Environment
{
    glm::vec3 sunDirection{-0.35f, -0.85f, -0.4f}; // direction the light travels
    glm::vec3 sunColor{1.0f, 0.96f, 0.9f};
    float sunIntensity = 2.2f;

    glm::vec3 ambientSky{0.16f, 0.19f, 0.26f};
    glm::vec3 ambientGround{0.05f, 0.05f, 0.06f};

    glm::vec3 fogColor{0.06f, 0.07f, 0.09f};
    float fogStart = 12.0f;
    float fogEnd = 90.0f;

    std::array<PunctualLight, kMaxPunctualLights> lights{};

    // How the picture is developed, rather than what is in it.
    //
    // Both belong to the viewer rather than the scene: a monitor that crushes its low end makes a
    // dark game unplayable, and the answer to that is not to light the game brighter for everybody.
    float exposure = 1.0f;
    float contrast = 1.0f;
};

// Entity storage plus the components the renderer needs.
//
// Components live in arrays indexed by the entity's slot, using std::optional for presence. That is
// deliberately simple rather than a packed archetype store: at the scale this game runs (hundreds of
// entities, one creature) it is fast enough, trivially debuggable, and easy to replace later without
// changing this interface.
class Scene
{
public:
    Entity Create(std::string name = {});
    void Destroy(Entity entity);
    void Clear();

    bool IsAlive(Entity entity) const;
    size_t EntityCount() const { return m_aliveCount; }
    size_t Capacity() const { return m_slots.size(); }

    const std::string& Name(Entity entity) const;
    void SetName(Entity entity, std::string name);

    Transform* GetTransform(Entity entity);
    const Transform* GetTransform(Entity entity) const;

    void SetMeshRenderer(Entity entity, const MeshRenderer& renderer);
    MeshRenderer* GetMeshRenderer(Entity entity);
    const MeshRenderer* GetMeshRenderer(Entity entity) const;
    void RemoveMeshRenderer(Entity entity);

    // Convenience: create an entity with a transform and a mesh in one call.
    Entity CreateMeshEntity(std::string name, const Transform& transform, MeshHandle mesh,
                            const Material& material);

    // Visits every alive entity the camera should draw.
    template <typename Fn>
    void ForEachMeshRenderer(Fn&& fn) const
    {
        ForEachRenderable(std::forward<Fn>(fn), false);
    }

    // And every one that should be in a shadow map, which is not the same set: a thing can be kept
    // out of the camera's view and still block light. See MeshRenderer::hiddenFromCamera.
    template <typename Fn>
    void ForEachShadowCaster(Fn&& fn) const
    {
        ForEachRenderable(std::forward<Fn>(fn), true);
    }

    Environment& GetEnvironment() { return m_environment; }
    const Environment& GetEnvironment() const { return m_environment; }

private:
    template <typename Fn>
    void ForEachRenderable(Fn&& fn, bool forShadows) const
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_slots.size()); ++i)
        {
            if (!m_slots[i].alive || !m_meshRenderers[i].has_value())
            {
                continue;
            }
            const MeshRenderer& renderer = *m_meshRenderers[i];
            if (!renderer.visible)
            {
                continue;
            }
            if (forShadows ? !renderer.castsShadow : renderer.hiddenFromCamera)
            {
                continue;
            }
            fn(Entity{i, m_slots[i].generation}, m_transforms[i], renderer);
        }
    }

    struct Slot
    {
        uint32_t generation = 0;
        bool alive = false;
    };

    bool ResolveIndex(Entity entity, uint32_t& outIndex) const;

    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_freeList;
    std::vector<Transform> m_transforms;
    std::vector<std::optional<MeshRenderer>> m_meshRenderers;
    std::vector<std::string> m_names;
    size_t m_aliveCount = 0;

    Environment m_environment;
};

} // namespace pred
