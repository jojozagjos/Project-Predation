#pragma once

#include "Engine/Core/Math.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"

#include <string>
#include <utility>

namespace pred
{

// Builds visual and collision geometry together so the two can never drift apart.
class MapBuilder
{
public:
    // `prefix` goes on the front of every mesh name, so two maps built into one world do not share each
    // other's numbered meshes.
    MapBuilder(Scene& scene, MeshLibrary& meshes, PhysicsWorld* physics, std::string prefix = {})
        : m_scene(scene), m_meshes(meshes), m_physics(physics), m_prefix(std::move(prefix))
    {
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
        const std::string meshName = m_prefix + name + "#" + std::to_string(m_boxCount++);
        const MeshHandle mesh = m_meshes.Upload(Primitives::Box(size), meshName);
        m_scene.CreateMeshEntity(name, transform, mesh, material);
        if (m_physics != nullptr)
        {
            m_physics->CreateBox(size * 0.5f, transform, BodyMotion::Static);
        }
    }

    // Reuses an already-uploaded mesh, for shapes placed many times.
    void AddBoxInstance(const std::string& name, const Transform& transform, MeshHandle mesh,
                        const glm::vec3& size, const Material& material)
    {
        m_scene.CreateMeshEntity(name, transform, mesh, material);
        if (m_physics != nullptr)
        {
            m_physics->CreateBox(size * 0.5f, transform, BodyMotion::Static);
        }
    }

    // Arbitrary geometry: collided as a static triangle mesh so stairs and ramps are exact.
    void AddMesh(const std::string& name, const Transform& transform, const MeshData& data,
                 const Material& material, bool collide = true)
    {
        // Numbered like the boxes above, and for the same reason: two stairs of different sizes
        // both called "stairs" would share one mesh and the second would draw as the first.
        const MeshHandle mesh = m_meshes.Upload(data, m_prefix + name + "#" + std::to_string(m_boxCount++));
        m_scene.CreateMeshEntity(name, transform, mesh, material);
        if (collide && m_physics != nullptr)
        {
            m_physics->CreateMeshBody(data, transform);
        }
    }

    void AddMeshInstance(const std::string& name, const Transform& transform, MeshHandle mesh,
                         const MeshData& data, const Material& material, bool collide = true)
    {
        m_scene.CreateMeshEntity(name, transform, mesh, material);
        if (collide && m_physics != nullptr)
        {
            m_physics->CreateMeshBody(data, transform);
        }
    }

    // Visual only: markers and other things the player should pass through.
    void AddDecoration(const std::string& name, const Transform& transform, MeshHandle mesh,
                       const Material& material)
    {
        m_scene.CreateMeshEntity(name, transform, mesh, material);
    }

private:
    Scene& m_scene;
    MeshLibrary& m_meshes;
    PhysicsWorld* m_physics;
    // How many boxes have been added, so each gets a mesh name of its own in a stable order.
    size_t m_boxCount = 0;
    std::string m_prefix;
};

} // namespace pred
