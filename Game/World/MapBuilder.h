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
    ~MapBuilder() { FlushBatches(); }
    MapBuilder(const MapBuilder&) = delete;
    MapBuilder& operator=(const MapBuilder&) = delete;

    // Draws what is added from here on in batches: everything of one material within one cell of the world
    // this big goes into one mesh, drawn as one thing, instead of every box and every four-metre piece of
    // one being drawn on its own. What collides is not batched -- each box is still its own body.
    //
    // Lamps are found per pixel now (SceneRenderer::SetClusterCamera), so how a level is cut up no longer
    // decides how it is lit, and a building drawn as a few hundred things rather than several thousand is
    // most of the difference between thirty frames a second and a hundred.
    void BeginBatching(float cellSize = 16.0f) { m_batchCell = cellSize; }
    // Uploads what has been batched and puts it in the scene. Called by the destructor if not before.
    void FlushBatches()
    {
        for (Batch& batch : m_batches)
        {
            if (batch.mesh.vertices.empty())
            {
                continue;
            }
            const std::string meshName = m_prefix + batch.name + "_batch#" + std::to_string(m_boxCount++);
            const MeshHandle mesh = m_meshes.Upload(batch.mesh, meshName);
            const Entity entity = m_scene.CreateMeshEntity(batch.name, Transform{}, mesh, batch.material);
            Keep(entity);
            if (MeshRenderer* renderer = m_scene.GetMeshRenderer(entity))
            {
                renderer->castsShadow = batch.shadows;
                renderer->levelGeometry = batch.shadows;
            }
        }
        m_batches.clear();
    }

    // Keeps every entity and body made from here on in these lists, so a map that is rebuilt (a
    // generated facility, from a new seed) can take everything it made away again.
    void Track(std::vector<Entity>* entities, std::vector<BodyHandle>* bodies)
    {
        m_trackedEntities = entities;
        m_trackedBodies = bodies;
    }

    // What is added from here on is part of one structure, whose pieces are built to reach into one another
    // (PhysicsWorld::SetOverlapGroup): a group from PhysicsWorld::NewOverlapGroup, or 0 for things that
    // have to keep clear of everything.
    void SetStructure(uint32_t group) { m_overlapGroup = group; }

    // Whether what is added from here on casts shadows and counts as the level's own geometry. Off for thin things laid
    // on a surface -- a screen on a wall, paint on a floor: a few millimetres off it, in a lamp's shadow map they are
    // the surface, and the two fight over which is in shadow as the lamps with shadow maps change with the view.
    void SetShadows(bool cast) { m_shadows = cast; }

    // Box: rendered as a box mesh, collided as a box shape. Cheaper and more robust than a
    // triangle mesh, and exact for this shape.
    //
    // `tile` is how big a drawn piece may be: kTile indoors, where every room has lamps of its own, and more
    // out on open ground, where the lamps are few and a ground cut into four-metre squares is thousands of
    // things to draw.
    void AddBox(const std::string& name, const Transform& transform, const glm::vec3& size,
                const Material& material, float tile = kTile)
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
        if (m_batchCell > 0.0f)
        {
            AddToBatch(name, Primitives::Box(size), transform.Matrix(), transform.position, material);
            if (m_physics != nullptr)
            {
                Keep(m_physics->CreateBox(size * 0.5f, transform, BodyMotion::Static));
            }
            return;
        }
        const int across = std::max(1, static_cast<int>(std::ceil(size.x / tile)));
        const int deep = std::max(1, static_cast<int>(std::ceil(size.z / tile)));
        const glm::vec3 piece{size.x / static_cast<float>(across), size.y, size.z / static_cast<float>(deep)};
        const std::string meshName = m_prefix + name + "#" + std::to_string(m_boxCount++);
        const MeshHandle mesh = m_meshes.Upload(Primitives::Box(piece), meshName);
        for (int i = 0; i < across; ++i)
        {
            for (int k = 0; k < deep; ++k)
            {
                const glm::vec3 offset{(static_cast<float>(i) + 0.5f) * piece.x - size.x * 0.5f, 0.0f,
                                       (static_cast<float>(k) + 0.5f) * piece.z - size.z * 0.5f};
                Transform placed = transform;
                placed.position = transform.position + transform.rotation * offset;
                Keep(m_scene.CreateMeshEntity(name, placed, mesh, material));
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
        if (m_batchCell > 0.0f)
        {
            AddToBatch(name, data, transform.Matrix(), transform.position, material);
        }
        else
        {
            // Numbered like the boxes above, and for the same reason: two stairs of different sizes
            // both called "stairs" would share one mesh and the second would draw as the first.
            const MeshHandle mesh = m_meshes.Upload(data, m_prefix + name + "#" + std::to_string(m_boxCount++));
            Keep(m_scene.CreateMeshEntity(name, transform, mesh, material));
        }
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
    struct Batch
    {
        std::string name;
        Material material;
        glm::ivec3 cell{0};
        MeshData mesh;
        bool shadows = true;
    };

    static bool SameMaterial(const Material& a, const Material& b)
    {
        return a.baseColor == b.baseColor && a.metallic == b.metallic && a.roughness == b.roughness && a.emissive == b.emissive &&
               a.emissiveTextured == b.emissiveTextured &&
               a.reflectivity == b.reflectivity && a.baseColorTexture == b.baseColorTexture && a.organic == b.organic &&
               a.organicBeat == b.organicBeat;
    }

    void AddToBatch(const std::string& name, const MeshData& data, const glm::mat4& transform, const glm::vec3& at,
                    const Material& material)
    {
        const glm::ivec3 cell{static_cast<int>(std::floor(at.x / m_batchCell)), static_cast<int>(std::floor(at.y / m_batchCell)),
                              static_cast<int>(std::floor(at.z / m_batchCell))};
        for (Batch& batch : m_batches)
        {
            if (batch.cell == cell && batch.shadows == m_shadows && SameMaterial(batch.material, material))
            {
                batch.mesh.Append(data, transform);
                return;
            }
        }
        Batch batch;
        batch.name = name;
        batch.material = material;
        batch.cell = cell;
        batch.shadows = m_shadows;
        batch.mesh.Append(data, transform);
        m_batches.push_back(std::move(batch));
    }

    void Keep(Entity entity)
    {
        // Everything a map builds is the level, and is what the lamps' kept shadows are drawn from.
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(entity))
        {
            renderer->levelGeometry = true;
        }
        if (m_trackedEntities != nullptr)
        {
            m_trackedEntities->push_back(entity);
        }
    }
    void Keep(BodyHandle body)
    {
        if (m_overlapGroup != 0 && m_physics != nullptr)
        {
            m_physics->SetOverlapGroup(body, m_overlapGroup);
        }
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
    float m_batchCell = 0.0f;
    uint32_t m_overlapGroup = 0;
    bool m_shadows = true;
    std::vector<Batch> m_batches;
};

} // namespace pred
