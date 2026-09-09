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
class WeaponDatabase;

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

    // A place to refill magazines. The developer map leaves it unlimited so weapons can be
    // exercised without walking back and forth; a real map will give each one a fixed number of
    // refills, because ammunition you can always get more of is not a decision.
    struct AmmoCrate
    {
        Entity entity;
        Entity lidEntity;
        glm::vec3 lidRest{0.0f}; // where the lid sits closed
        int refillsLeft = -1;    // -1 is unlimited
        float lidAngle = 0.0f;
        float lidTarget = 0.0f;
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

    // `weapons` is optional and only affects appearance: with it, a dropped rifle looks like the
    // rifle rather than like a block.
    void Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions,
               const ItemDatabase& items, const WeaponDatabase* weapons = nullptr);
    void Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions);

    // Advances door swings. Called from the fixed update, before physics steps.
    void Update(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions, float dt);

    bool ToggleDoor(int index, InteractionSystem& interactions);
    void SetDoorOpen(int index, bool open, InteractionSystem& interactions);

    Door* GetDoor(int index);
    Pickup* GetPickup(int index);
    HidingSpot* GetHidingSpot(int index);
    AmmoCrate* GetAmmoCrate(int index);
    // Takes one refill. False when the crate has nothing left, so the caller can say so rather than
    // silently doing nothing.
    bool DrawFromAmmoCrate(int index, InteractionSystem& interactions);

    // Removes a pickup from the world, for when it goes into a bag.
    bool ConsumePickup(int index, Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions);

    // Creates a new pickup, for when something is dropped back out.
    int SpawnPickup(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                    InteractionSystem& interactions, const ItemDatabase& items, ItemId item, int count,
                    const glm::vec3& position, const glm::vec3& velocity);

    const std::vector<Door>& Doors() const { return m_doors; }
    const std::vector<Pickup>& Pickups() const { return m_pickups; }
    const std::vector<HidingSpot>& HidingSpots() const { return m_hidingSpots; }
    const std::vector<AmmoCrate>& AmmoCrates() const { return m_ammoCrates; }

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
    std::vector<AmmoCrate> m_ammoCrates;
    // Held only so pickups can be built with the real weapon models. Appearance, never behaviour.
    const WeaponDatabase* m_weapons = nullptr;
    // Meshes reused by pickups spawned at runtime.
    std::vector<MeshHandle> m_itemMeshes;
};

} // namespace pred
