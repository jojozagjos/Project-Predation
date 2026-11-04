#include "Game/World/OutpostInteriors.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"

#include <glm/gtc/quaternion.hpp>

#include <string>

namespace pred
{

void OutpostInteriors::Set(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights, std::vector<Outpost::Layout::Building> buildings,
                           const glm::vec3& origin, uint32_t structure)
{
    Clear(scene, physics, lights);
    m_meshes = &meshes;
    m_structure = structure;
    m_planned = true;
    // The door shut in each, steel painted dark.
    Material shut = Material::Diffuse({0.2f, 0.21f, 0.22f}, 0.45f);
    shut.metallic = 0.5f;
    for (size_t i = 0; i < buildings.size(); ++i)
    {
        Entry entry;
        entry.plan = std::move(buildings[i]);
        FacilityLayout& layout = entry.plan.layout;
        layout.origin += origin;
        entry.plan.box += glm::vec4(origin.x, origin.z, origin.x, origin.z);
        entry.plan.floor += origin.y;
        entry.plan.roof += origin.y;
        // (Its lamps on circuits of their own: none of them the ship's, or another building's.)
        for (FacilityLayout::Lamp& lamp : layout.lamps)
        {
            lamp.circuit += 5000 + 100 * static_cast<int>(i);
        }
        // Its stand-in: the box it fills, in its cladding; felt, part of the outpost.
        const glm::vec4& box = entry.plan.box;
        const glm::vec3 size{box.z - box.x, entry.plan.roof - entry.plan.floor, box.w - box.y};
        Transform transform;
        transform.position = {(box.x + box.z) * 0.5f, (entry.plan.floor + entry.plan.roof) * 0.5f, (box.y + box.w) * 0.5f};
        const MeshHandle mesh = meshes.Upload(Primitives::Box(size), "outpost_standin_" + std::to_string(i));
        m_meshHandles.push_back(mesh);
        entry.standIn = scene.CreateMeshEntity("outpost_standin", transform, mesh, FacilityMap::OutsideOf(layout.outside, FacilityMap::Piece::Kind::Cladding));
        if (MeshRenderer* renderer = scene.GetMeshRenderer(entry.standIn))
        {
            renderer->levelGeometry = true;
            renderer->farVisible = true;
        }
        entry.body = physics.CreateBox(size * 0.5f, transform, BodyMotion::Static);
        physics.SetOverlapGroup(entry.body, structure);
        // And its door, shut, where its way out will be: a panel a hair proud of the box's face.
        if (!layout.exits.empty())
        {
            const FacilityLayout::Exit& exit = layout.exits.front();
            const float wide = FacilitySpec::ExitWidth(exit);
            const float high = FacilitySpec::ExitHeight(exit);
            Transform door;
            door.position = FacilityMap::ExitOutside(layout, exit, 0.03f) + glm::vec3(0.0f, high * 0.5f, 0.0f);
            door.rotation = glm::angleAxis(exit.side < 2 ? glm::half_pi<float>() : 0.0f, glm::vec3(0.0f, 1.0f, 0.0f));
            const MeshHandle panel = meshes.Upload(Primitives::Box({wide, high, 0.04f}), "outpost_standin_door_" + std::to_string(i));
            m_meshHandles.push_back(panel);
            entry.door = scene.CreateMeshEntity("outpost_standin_door", door, panel, shut);
        }
        m_entries.push_back(std::move(entry));
    }
    PRED_LOG_INFO(Gameplay, "Outpost: {} buildings to go into", m_entries.size());
    for (const Entry& entry : m_entries)
    {
        const glm::vec3 door = entry.plan.layout.exits.empty() ? glm::vec3(0.0f) : FacilityMap::ExitOutside(entry.plan.layout, entry.plan.layout.exits.front(), 1.0f);
        PRED_LOG_INFO(Gameplay, "Outpost building at {:.0f} {:.0f} to {:.0f} {:.0f}, {} storeys, its way in at {:.1f} {:.1f} {:.1f} facing {}", entry.plan.box.x, entry.plan.box.y, entry.plan.box.z,
                      entry.plan.box.w, entry.plan.layout.floors, door.x, door.y, door.z, entry.plan.layout.exits.empty() ? -1 : entry.plan.layout.exits.front().side);
    }
}

bool OutpostInteriors::BuildNext(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights, WorldObjects::Placements& doors)
{
    if (Done())
    {
        return false;
    }
    const size_t index = m_next++;
    Entry& entry = m_entries[index];
    RemoveStandIn(scene, physics, entry);
    entry.map = std::make_unique<FacilityMap>();
    entry.map->Build(entry.plan.layout, entry.plan.layout.seed, scene, meshes, physics, lights, "outpost_b" + std::to_string(index) + "_", m_structure);
    // Only its doors: an outpost's buildings have nothing in them to take (Outpost: Enterable).
    doors = WorldObjects::Placements{};
    doors.doors = entry.map->Placements().doors;
    return true;
}

void OutpostInteriors::RemoveStandIn(Scene& scene, PhysicsWorld& physics, Entry& entry)
{
    scene.Destroy(entry.standIn);
    scene.Destroy(entry.door);
    if (entry.body.IsValid())
    {
        physics.DestroyBody(entry.body);
    }
    entry.standIn = Entity{};
    entry.door = Entity{};
    entry.body = BodyHandle{};
}

void OutpostInteriors::Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights)
{
    // The last built first: each takes its lamps away from where its own began (LevelLights::RemoveFrom).
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
    {
        if (it->map != nullptr)
        {
            it->map->Clear(scene, physics, lights);
        }
        RemoveStandIn(scene, physics, *it);
    }
    m_entries.clear();
    if (m_meshes != nullptr)
    {
        for (const MeshHandle mesh : m_meshHandles)
        {
            m_meshes->Release(mesh);
        }
    }
    m_meshHandles.clear();
    m_next = 0;
    m_planned = false;
}

} // namespace pred
