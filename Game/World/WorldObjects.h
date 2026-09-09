#pragma once

#include "Engine/Core/Math.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Entity.h"
#include "Game/Items/ItemDatabase.h"

#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

class Scene;
class MeshLibrary;
class InteractionSystem;

// The usable things in a level: doors that swing, items lying about, and places to hide.
//
// Each kind keeps its own list and the interactable's payload is an index into it. That is enough
// while the counts are small and keeps the lookup trivial; a general entity-component store is the
// answer if this grows to a dozen kinds.
class WorldObjects
{
public:
    struct Door
    {
        Entity entity;
        BodyHandle body;
        glm::vec3 hinge{0.0f};      // world position of the hinge axis
        glm::vec3 panelOffset{0.0f}; // hinge to panel centre, in the door's closed frame
        float closedYaw = 0.0f;
        float openYaw = 0.0f;
        float angle = 0.0f;  // current, radians
        float target = 0.0f; // where it is heading
        float speed = 4.5f;  // radians per second
        bool locked = false;

        bool IsOpen() const;
        bool IsMoving() const;
    };

    struct Pickup
    {
        Entity entity;
        BodyHandle body;
        ItemId item = kInvalidItem;
        int count = 1;
        bool alive = true;
    };

    struct HidingSpot
    {
        Entity entity;
        Entity doorEntity;         // the panel that swings open when it is used
        glm::vec3 insidePosition{0.0f};
        glm::vec3 exitPosition{0.0f};
        float insideYaw = 0.0f;
        int doorIndex = -1;
        bool occupied = false;
    };

    void Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions,
               const ItemDatabase& items);
    void Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions);

    // Advances door swings. Called from the fixed update, before physics steps.
    void Update(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions, float dt);

    bool ToggleDoor(int index, InteractionSystem& interactions);
    void SetDoorOpen(int index, bool open, InteractionSystem& interactions);

    Door* GetDoor(int index);
    Pickup* GetPickup(int index);
    HidingSpot* GetHidingSpot(int index);

    // Removes a pickup from the world, for when it goes into a bag.
    bool ConsumePickup(int index, Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions);

    // Creates a new pickup, for when something is dropped back out.
    int SpawnPickup(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                    InteractionSystem& interactions, const ItemDatabase& items, ItemId item, int count,
                    const glm::vec3& position, const glm::vec3& velocity);

    const std::vector<Door>& Doors() const { return m_doors; }
    const std::vector<Pickup>& Pickups() const { return m_pickups; }
    const std::vector<HidingSpot>& HidingSpots() const { return m_hidingSpots; }

private:
    Transform DoorPanelTransform(const Door& door) const;
    int AddDoor(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions,
                const glm::vec3& hinge, float closedYaw, float openYaw, const glm::vec3& panelSize,
                const std::string& name, bool registerInteractable);
    // Removes a pickup from the world, whether it was taken or fell out of the level.
    void Despawn(Pickup& pickup, Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions);

    std::vector<Door> m_doors;
    std::vector<Pickup> m_pickups;
    std::vector<HidingSpot> m_hidingSpots;

    // Meshes reused by pickups spawned at runtime.
    std::vector<MeshHandle> m_itemMeshes;
};

} // namespace pred
