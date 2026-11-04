#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/LevelLights.h"
#include "Game/World/Outpost.h"
#include "Game/World/WorldObjects.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pred
{

// The buildings at an outpost to be gone into (Outpost::Layout::buildings). From when the outpost is there, each is stood in for by a
// box as big as it is, in the cladding it will have, its door shut; and one after another, in the order they come -- the same on every
// machine, so their doors are numbered alike in WorldObjects -- each is built inside, as a facility's building is (FacilityMap), in its
// stand-in's place: its rooms, stairs, doors and lamps. All taken away together, when the outpost goes or the world is made again.
class OutpostInteriors
{
public:
    // Its buildings, planned in the ship's frame, put in the world at `origin` (ShipSpec::kOrigin): their stand-ins made at once, part of
    // the outpost's structure (ShipMap::FieldGroup). Whatever was there before is taken away first.
    void Set(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights, std::vector<Outpost::Layout::Building> buildings,
             const glm::vec3& origin, uint32_t structure);
    // The next not yet built built, its stand-in gone: its doors given to be added to WorldObjects. False when every one already is.
    bool BuildNext(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights, WorldObjects::Placements& doors);
    void Clear(Scene& scene, PhysicsWorld& physics, LevelLights* lights);

    bool Planned() const { return m_planned; }
    bool Done() const { return m_next >= m_entries.size(); }
    size_t Count() const { return m_entries.size(); }
    size_t BuiltCount() const { return m_next; }
    // Its buildings as planned, in the world (for the tests, and the log).
    const FacilityLayout& Layout(size_t index) const { return m_entries[index].plan.layout; }

private:
    struct Entry
    {
        Outpost::Layout::Building plan;
        Entity standIn;
        Entity door;
        BodyHandle body;
        std::unique_ptr<FacilityMap> map;
    };
    void RemoveStandIn(Scene& scene, PhysicsWorld& physics, Entry& entry);

    std::vector<Entry> m_entries;
    size_t m_next = 0;
    uint32_t m_structure = 0;
    MeshLibrary* m_meshes = nullptr;
    std::vector<MeshHandle> m_meshHandles;
    bool m_planned = false;
};

} // namespace pred
