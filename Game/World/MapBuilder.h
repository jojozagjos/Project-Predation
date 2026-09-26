#pragma once

#include "Engine/Core/Math.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace pred
{

// Builds visual and collision geometry together so the two can never drift apart.
class MapBuilder
{
public:
    // How big a drawn piece of a box may be across, in metres. See AddBox.
    static constexpr float kTile = 4.0f;

    // `prefix` goes on the front of every mesh name, so two maps built into one world do not share each
    // other's numbered meshes.
    MapBuilder(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics, std::string prefix = {})
        : m_scene(scene), m_meshes(meshes), m_physics(physics), m_prefix(std::move(prefix))
    {
    }

    // Keeps every entity and body made from here on in these lists, so a map that is rebuilt (a
    // generated facility, from a new seed) can take everything it made away again.
    void Track(std::vector<Entity>* entities, std::vector<BodyHandle>* bodies)
    {
        m_trackedEntities = entities;
        m_trackedBodies = bodies;
    }

    // Box: rendered as a box mesh, collided as a box shape. Cheaper and more robust than a
    // triangle mesh, and exact for this shape.
    void AddBox(const std::string& name, const Transform& transform, const glm::vec3& size,
                const Material& material)
    {
        // The mesh gets a name of its own, numbered in the order the map is built.
        //
        // Meshes are shared by name, so several boxes that call themselves the same thing and are
        // not the same size all end up drawing whichever of them was uploaded last. The corridor
        // builds a row of doorways that narrow as they go and calls every lintel "gap_lintel", so
        // they all came out the width of the last one and the tops of the wide ones stopped
        // reaching their walls. Numbered by build order rather than made unique some other way,
        // because the map is rebuilt for every new game and a name that changes each time would
        // upload the whole level again.
        //
        // Drawn in tiles of at most four metres across when it is bigger than that, and collided as one.
        // Each drawn piece is lit by the lamps nearest it, up to a handful; a wall the length of the
        // building as one piece would be lit by the handful nearest its middle and dark at both ends.
        const int across = std::max(1, static_cast<int>(std::ceil(size.x / kTile)));
        const int deep = std::max(1, static_cast<int>(std::ceil(size.z / kTile)));
        const glm::vec3 tile{size.x / static_cast<float>(across), size.y, size.z / static_cast<float>(deep)};
        const std::string meshName = m_prefix + name + "#" + std::to_string(m_boxCount++);
        const MeshHandle mesh = m_meshes.Upload(Primitives::Box(tile), meshName);
        for (int i = 0; i < across; ++i)
        {
            for (int k = 0; k < deep; ++k)
            {
                const glm::vec3 offset{(static_cast<float>(i) + 0.5f) * tile.x - size.x * 0.5f, 0.0f,
                                       (static_cast<float>(k) + 0.5f) * tile.z - size.z * 0.5f};
                Transform piece = transform;
                piece.position = transform.position + transform.rotation * offset;
                Keep(m_scene.CreateMeshEntity(name, piece, mesh, material));
            }
        }
        if (m_physics != nullptr)
        {
            Keep(m_physics->CreateBox(size * 0.5f, transform, BodyMotion::Static));
        }
    }

    // Reuses an already-uploaded mesh, for shapes placed many times.
    void AddBoxInstance(const std::string& name, const Transform& transform, MeshHandle mesh,
                        const glm::vec3& size, const Material& material)
    {
        Keep(m_scene.CreateMeshEntity(name, transform, mesh, material));
        if (m_physics != nullptr)
        {
            Keep(m_physics->CreateBox(size * 0.5f, transform, BodyMotion::Static));
        }
    }

    // Arbitrary geometry: collided as a static triangle mesh so stairs and ramps are exact.
    void AddMesh(const std::string& name, const Transform& transform, const MeshData& data,
                 const Material& material, bool collide = true)
    {
        // Numbered like the boxes above, and for the same reason: two stairs of different sizes
        // both called "stairs" would share one mesh and the second would draw as the first.
        const MeshHandle mesh = m_meshes.Upload(data, m_prefix + name + "#" + std::to_string(m_boxCount++));
        Keep(m_scene.CreateMeshEntity(name, transform, mesh, material));
        if (collide && m_physics != nullptr)
        {
            Keep(m_physics->CreateMeshBody(data, transform));
        }
    }

    void AddMeshInstance(const std::string& name, const Transform& transform, MeshHandle mesh,
                         const MeshData& data, const Material& material, bool collide = true)
    {
        Keep(m_scene.CreateMeshEntity(name, transform, mesh, material));
        if (collide && m_physics != nullptr)
        {
            Keep(m_physics->CreateMeshBody(data, transform));
        }
    }

    // Visual only: markers and other things the player should pass through.
    void AddDecoration(const std::string& name, const Transform& transform, MeshHandle mesh,
                       const Material& material)
    {
        Keep(m_scene.CreateMeshEntity(name, transform, mesh, material));
    }

private:
    void Keep(Entity entity)
    {
        if (m_trackedEntities != nullptr)
        {
            m_trackedEntities->push_back(entity);
        }
    }
    void Keep(BodyHandle body)
    {
        if (m_trackedBodies != nullptr)
        {
            m_trackedBodies->push_back(body);
        }
    }

    std::vector<Entity>* m_trackedEntities = nullptr;
    std::vector<BodyHandle>* m_trackedBodies = nullptr;
    Scene& m_scene;
    MeshLibrary& m_meshes;
    PhysicsWorld* m_physics;
    // How many boxes have been added, so each gets a mesh name of its own in a stable order.
    size_t m_boxCount = 0;
    std::string m_prefix;
};

} // namespace pred
