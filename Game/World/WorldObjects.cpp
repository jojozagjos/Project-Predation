#include "Game/World/WorldObjects.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

const Material kDoorMaterial = Material::Diffuse({0.34f, 0.30f, 0.26f}, 0.80f);
const Material kLockerMaterial = Material::Diffuse({0.26f, 0.29f, 0.32f}, 0.70f);

Transform MakeTransform(const glm::vec3& position, float yaw)
{
    Transform transform;
    transform.position = position;
    transform.rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    return transform;
}

Material MaterialFor(const ItemDefinition& definition)
{
    Material material;
    material.baseColor = definition.color;
    material.roughness = definition.roughness;
    material.metallic = definition.metallic;
    material.emissive = definition.color * definition.emissive;
    return material;
}

MeshData MeshFor(const ItemDefinition& definition)
{
    switch (definition.shape)
    {
    case ItemShape::Cylinder:
        return Primitives::Cylinder(definition.size.x * 0.5f, definition.size.y, 16);
    case ItemShape::Sphere:
        return Primitives::Sphere(definition.size.x * 0.5f, 16, 12);
    case ItemShape::Box:
    default:
        return Primitives::Box(definition.size);
    }
}

} // namespace

bool WorldObjects::Door::IsOpen() const
{
    return std::abs(angle - closedYaw) > std::abs(openYaw - closedYaw) * 0.5f;
}

bool WorldObjects::Door::IsMoving() const
{
    return std::abs(target - angle) > 1e-3f;
}

Transform WorldObjects::DoorPanelTransform(const Door& door) const
{
    // The panel hangs off the hinge, so rotating about the hinge sweeps it rather than spinning it
    // in place. Rotating the offset is the whole trick.
    const glm::quat rotation = glm::angleAxis(door.angle, glm::vec3(0.0f, 1.0f, 0.0f));
    Transform transform;
    transform.rotation = rotation;
    transform.position = door.hinge + rotation * door.panelOffset;
    return transform;
}

int WorldObjects::AddDoor(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                          InteractionSystem& interactions, const glm::vec3& hinge, float closedYaw,
                          float openYaw, const glm::vec3& panelSize, const std::string& name,
                          bool registerInteractable)
{
    Door door;
    door.hinge = hinge;
    door.closedYaw = closedYaw;
    door.openYaw = openYaw;
    door.angle = closedYaw;
    door.target = closedYaw;
    // Hinge at one edge: the panel's centre sits half a width along its own local X.
    door.panelOffset = glm::vec3(panelSize.x * 0.5f, panelSize.y * 0.5f, 0.0f);

    const MeshHandle mesh = meshes.Upload(Primitives::Box(panelSize), name + "_panel");
    const Transform transform = DoorPanelTransform(door);
    door.entity = scene.CreateMeshEntity(name, transform, mesh, kDoorMaterial);
    door.body = physics.CreateBox(panelSize * 0.5f, transform, BodyMotion::Kinematic);

    const auto index = static_cast<int>(m_doors.size());
    m_doors.push_back(door);

    if (registerInteractable)
    {
        Interactable interactable;
        interactable.entity = door.entity;
        interactable.kind = InteractionKind::Door;
        interactable.verb = "Open";
        interactable.name = "Door";
        interactable.payload = index;
        interactable.range = 2.6f;
        interactions.Register(interactable);
    }
    return index;
}

void WorldObjects::Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                         InteractionSystem& interactions, const ItemDatabase& items)
{
    // --- Doors, standing in the open where they are easy to walk up to and try.
    AddDoor(scene, meshes, physics, interactions, {4.0f, 0.0f, 2.0f}, 0.0f, glm::radians(-100.0f),
            {0.95f, 2.05f, 0.09f}, "door_a", true);
    AddDoor(scene, meshes, physics, interactions, {-4.0f, 0.0f, 2.0f}, glm::radians(180.0f),
            glm::radians(80.0f), {0.95f, 2.05f, 0.09f}, "door_b", true);

    // --- Lockers to hide in. The panel is a door in its own right so it swings when used.
    const glm::vec3 lockerSize{0.85f, 2.0f, 0.75f};
    const MeshHandle lockerMesh = meshes.Upload(Primitives::Box(lockerSize), "locker_shell");

    const glm::vec3 lockerPositions[] = {{-7.0f, 0.0f, 0.5f}, {7.5f, 0.0f, 0.5f}};
    for (const glm::vec3& position : lockerPositions)
    {
        HidingSpot spot;
        // The shell is a visual and a collider; the player stands inside it while hidden.
        spot.entity = scene.CreateMeshEntity("locker", MakeTransform(position + glm::vec3(0.0f, lockerSize.y * 0.5f, 0.0f), 0.0f),
                                             lockerMesh, kLockerMaterial);
        physics.CreateBox({lockerSize.x * 0.5f, lockerSize.y * 0.5f, 0.06f},
                          MakeTransform(position + glm::vec3(0.0f, lockerSize.y * 0.5f, lockerSize.z * 0.5f), 0.0f),
                          BodyMotion::Static);
        physics.CreateBox({0.06f, lockerSize.y * 0.5f, lockerSize.z * 0.5f},
                          MakeTransform(position + glm::vec3(-lockerSize.x * 0.5f, lockerSize.y * 0.5f, 0.0f), 0.0f),
                          BodyMotion::Static);
        physics.CreateBox({0.06f, lockerSize.y * 0.5f, lockerSize.z * 0.5f},
                          MakeTransform(position + glm::vec3(lockerSize.x * 0.5f, lockerSize.y * 0.5f, 0.0f), 0.0f),
                          BodyMotion::Static);

        spot.insidePosition = position + glm::vec3(0.0f, 0.0f, 0.05f);
        spot.exitPosition = position - glm::vec3(0.0f, 0.0f, 1.15f);
        spot.insideYaw = glm::radians(180.0f); // facing out through the door
        spot.doorIndex = AddDoor(scene, meshes, physics, interactions,
                                 position + glm::vec3(-lockerSize.x * 0.5f, 0.0f, -lockerSize.z * 0.5f),
                                 0.0f, glm::radians(-105.0f), {lockerSize.x, lockerSize.y, 0.06f},
                                 "locker_door", false);
        spot.doorEntity = m_doors[static_cast<size_t>(spot.doorIndex)].entity;

        const auto index = static_cast<int>(m_hidingSpots.size());
        m_hidingSpots.push_back(spot);

        Interactable interactable;
        interactable.entity = spot.entity;
        interactable.kind = InteractionKind::HidingSpot;
        interactable.verb = "Hide in";
        interactable.name = "Locker";
        interactable.payload = index;
        interactable.range = 2.4f;
        interactable.focusOffset = {0.0f, -0.4f, -0.45f};
        interactions.Register(interactable);
    }

    // --- Loose items.
    struct Placement
    {
        const char* key;
        glm::vec3 position;
        int count;
    };
    const Placement placements[] = {
        {"sample_container", {1.6f, 0.0f, 3.4f}, 1},
        {"medkit", {-1.8f, 0.0f, 3.8f}, 1},
        {"battery", {0.4f, 0.0f, 4.6f}, 3},
        {"keycard", {5.2f, 0.0f, 5.0f}, 1},
    };

    for (const Placement& placement : placements)
    {
        const ItemId id = items.IdOf(placement.key);
        if (id == kInvalidItem)
        {
            PRED_LOG_WARN(Gameplay, "Test map references unknown item '{}'", placement.key);
            continue;
        }
        SpawnPickup(scene, meshes, physics, interactions, items, id, placement.count,
                    placement.position + glm::vec3(0.0f, 0.4f, 0.0f), glm::vec3(0.0f));
    }

    PRED_LOG_INFO(Gameplay, "World objects: {} doors, {} pickups, {} hiding spots", m_doors.size(),
                  m_pickups.size(), m_hidingSpots.size());
}

int WorldObjects::SpawnPickup(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                              InteractionSystem& interactions, const ItemDatabase& items, ItemId item,
                              int count, const glm::vec3& position, const glm::vec3& velocity)
{
    const ItemDefinition* definition = items.Get(item);
    if (definition == nullptr || count <= 0)
    {
        return -1;
    }

    Pickup pickup;
    pickup.item = item;
    pickup.count = count;

    const MeshHandle mesh = meshes.Upload(MeshFor(*definition), "item_" + definition->key);
    Transform transform;
    transform.position = position;
    pickup.entity = scene.CreateMeshEntity("pickup_" + definition->key, transform, mesh,
                                           MaterialFor(*definition));
    // Dropped items are dynamic so they settle naturally rather than floating where they were let go.
    pickup.body = physics.CreateBox(definition->size * 0.5f, transform, BodyMotion::Dynamic,
                                    std::max(definition->mass, 0.01f) * 400.0f);
    if (pickup.body.IsValid() && glm::length(velocity) > 0.0f)
    {
        physics.SetLinearVelocity(pickup.body, velocity);
    }

    // Reuse an existing free record if one is available, so indices stay small.
    int index = -1;
    for (size_t i = 0; i < m_pickups.size(); ++i)
    {
        if (!m_pickups[i].alive)
        {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0)
    {
        index = static_cast<int>(m_pickups.size());
        m_pickups.push_back(pickup);
    }
    else
    {
        m_pickups[static_cast<size_t>(index)] = pickup;
    }

    Interactable interactable;
    interactable.entity = pickup.entity;
    interactable.kind = InteractionKind::Pickup;
    interactable.verb = "Take";
    interactable.name = count > 1 ? definition->name + " x" + std::to_string(count) : definition->name;
    interactable.payload = index;
    interactable.range = 2.2f;
    interactions.Register(interactable);

    return index;
}

bool WorldObjects::ConsumePickup(int index, Scene& scene, PhysicsWorld& physics,
                                 InteractionSystem& interactions)
{
    Pickup* pickup = GetPickup(index);
    if (pickup == nullptr)
    {
        return false;
    }
    interactions.Unregister(pickup->entity);
    physics.DestroyBody(pickup->body);
    scene.Destroy(pickup->entity);
    pickup->alive = false;
    pickup->item = kInvalidItem;
    pickup->count = 0;
    return true;
}

void WorldObjects::Update(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions,
                          float dt)
{
    for (Door& door : m_doors)
    {
        if (!door.IsMoving())
        {
            continue;
        }

        const float step = door.speed * dt;
        const float delta = door.target - door.angle;
        door.angle += std::clamp(delta, -step, step);
        if (std::abs(door.target - door.angle) < 1e-3f)
        {
            door.angle = door.target;
        }

        const Transform transform = DoorPanelTransform(door);
        if (Transform* sceneTransform = scene.GetTransform(door.entity))
        {
            *sceneTransform = transform;
        }
        // Kinematic movement rather than teleporting, so the panel shoves what is in its way.
        physics.MoveKinematic(door.body, transform, dt);
    }

    // Keep the prompt honest about what using it will do.
    for (size_t i = 0; i < m_doors.size(); ++i)
    {
        const Door& door = m_doors[i];
        if (Interactable* interactable = interactions.Find(door.entity))
        {
            interactable->verb = door.locked ? "Locked" : (door.IsOpen() ? "Close" : "Open");
        }
    }
}

bool WorldObjects::ToggleDoor(int index, InteractionSystem& interactions)
{
    Door* door = GetDoor(index);
    if (door == nullptr || door->locked)
    {
        return false;
    }
    SetDoorOpen(index, !door->IsOpen(), interactions);
    return true;
}

void WorldObjects::SetDoorOpen(int index, bool open, InteractionSystem& interactions)
{
    Door* door = GetDoor(index);
    if (door == nullptr)
    {
        return;
    }
    door->target = open ? door->openYaw : door->closedYaw;
    if (Interactable* interactable = interactions.Find(door->entity))
    {
        interactable->verb = open ? "Close" : "Open";
    }
}

WorldObjects::Door* WorldObjects::GetDoor(int index)
{
    if (index < 0 || index >= static_cast<int>(m_doors.size()))
    {
        return nullptr;
    }
    return &m_doors[static_cast<size_t>(index)];
}

WorldObjects::Pickup* WorldObjects::GetPickup(int index)
{
    if (index < 0 || index >= static_cast<int>(m_pickups.size()))
    {
        return nullptr;
    }
    Pickup* pickup = &m_pickups[static_cast<size_t>(index)];
    return pickup->alive ? pickup : nullptr;
}

WorldObjects::HidingSpot* WorldObjects::GetHidingSpot(int index)
{
    if (index < 0 || index >= static_cast<int>(m_hidingSpots.size()))
    {
        return nullptr;
    }
    return &m_hidingSpots[static_cast<size_t>(index)];
}

void WorldObjects::Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions)
{
    for (Door& door : m_doors)
    {
        interactions.Unregister(door.entity);
        physics.DestroyBody(door.body);
        scene.Destroy(door.entity);
    }
    for (Pickup& pickup : m_pickups)
    {
        if (pickup.alive)
        {
            interactions.Unregister(pickup.entity);
            physics.DestroyBody(pickup.body);
            scene.Destroy(pickup.entity);
        }
    }
    for (HidingSpot& spot : m_hidingSpots)
    {
        interactions.Unregister(spot.entity);
        scene.Destroy(spot.entity);
    }
    m_doors.clear();
    m_pickups.clear();
    m_hidingSpots.clear();
    m_itemMeshes.clear();
}

} // namespace pred
