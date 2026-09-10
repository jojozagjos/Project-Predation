#include "Game/World/WorldObjects.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/ItemAppearance.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/Weapons/WeaponDatabase.h"
#include "Game/World/TestMap.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace pred
{
namespace
{

const Material kDoorMaterial = Material::Diffuse({0.34f, 0.30f, 0.26f}, 0.80f);
const Material kLockerMaterial = Material::Diffuse({0.26f, 0.29f, 0.32f}, 0.70f);
// Olive, because an ammunition crate should be findable at a glance in a corridor full of grey.
const Material kAmmoCrateMaterial = Material::Diffuse({0.28f, 0.31f, 0.20f}, 0.75f);
const Material kAmmoLidMaterial = Material::Diffuse({0.34f, 0.37f, 0.24f}, 0.70f);

Transform MakeTransform(const glm::vec3& position, float yaw)
{
    Transform transform;
    transform.position = position;
    transform.rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    return transform;
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
                         InteractionSystem& interactions, const ItemDatabase& items,
                         const WeaponDatabase* weapons)
{
    using namespace TestMapSpec;
    m_weapons = weapons;

    // Everything in this function lives in one of the two bays the map builder lays out, so doors,
    // lockers and loose items are each together with their own kind rather than dotted about.

    // --- Doors, hung in the two frames at the front of the interaction bay.
    const float doorFrameZ = kBayZ - 2.1f;
    AddDoor(scene, meshes, physics, interactions,
            {kInteractionBayX - 1.55f - 0.55f, 0.0f, doorFrameZ}, 0.0f, glm::radians(-100.0f),
            {1.10f, 2.05f, 0.09f}, "door_a", true);
    AddDoor(scene, meshes, physics, interactions,
            {kInteractionBayX + 1.55f + 0.55f, 0.0f, doorFrameZ}, glm::radians(180.0f),
            glm::radians(80.0f), {1.10f, 2.05f, 0.09f}, "door_b", true);

    // --- Lockers to hide in. The panel is a door in its own right so it swings when used.
    //
    // Wide enough that a person fits. The old shell had a 0.61 m interior and the player capsule is
    // 0.64 m across, so hiding wedged them into the walls and Jolt pushed them straight back out.
    // The controller also pins them in place now, but the model still has to fit the box.
    const glm::vec3 lockerSize{1.06f, 2.05f, 0.88f};

    // The shell is three slabs, and they meet edge to edge rather than lapping over one another.
    // Overlapping panels are invisible but they make the level's own geometry check report an
    // intersection on every start, and a warning that always fires is a warning nobody reads.
    constexpr float panelHalfThickness = 0.06f;
    const float lockerInnerHalfWidth = lockerSize.x * 0.5f - panelHalfThickness * 2.0f;

    // Drawn as the same three panels the colliders use, plus a floor and a roof. It used to be one
    // solid box, which is why hiding put the player inside what looked like a solid cabinet: the
    // colliders were a shell but the model was not.
    MeshData lockerShell;
    {
        const float halfHeight = lockerSize.y * 0.5f;
        const auto panel = [&](const glm::vec3& size, const glm::vec3& centre)
        { lockerShell.Append(Primitives::Box(size), glm::translate(glm::mat4(1.0f), centre)); };

        panel({lockerInnerHalfWidth * 2.0f, lockerSize.y, panelHalfThickness * 2.0f},
              {0.0f, 0.0f, lockerSize.z * 0.5f - panelHalfThickness});
        panel({panelHalfThickness * 2.0f, lockerSize.y, lockerSize.z},
              {-lockerSize.x * 0.5f + panelHalfThickness, 0.0f, 0.0f});
        panel({panelHalfThickness * 2.0f, lockerSize.y, lockerSize.z},
              {lockerSize.x * 0.5f - panelHalfThickness, 0.0f, 0.0f});
        panel({lockerSize.x, panelHalfThickness * 2.0f, lockerSize.z},
              {0.0f, halfHeight - panelHalfThickness, 0.0f});
        panel({lockerSize.x, panelHalfThickness * 2.0f, lockerSize.z},
              {0.0f, -halfHeight + panelHalfThickness, 0.0f});
    }
    const MeshHandle lockerMesh = meshes.Upload(lockerShell, "locker_shell");

    // Against the back of the interaction bay, opening towards the plaza.
    const glm::vec3 lockerPositions[] = {{kInteractionBayX - 1.3f, 0.0f, kBayZ + 0.9f},
                                         {kInteractionBayX + 1.3f, 0.0f, kBayZ + 0.9f}};
    for (const glm::vec3& position : lockerPositions)
    {
        HidingSpot spot;
        // The shell is a visual and a collider; the player stands inside it while hidden.
        spot.entity = scene.CreateMeshEntity("locker", MakeTransform(position + glm::vec3(0.0f, lockerSize.y * 0.5f, 0.0f), 0.0f),
                                             lockerMesh, kLockerMaterial);
        // Back, fitted between the two sides. Every collider below sits exactly where its panel is
        // drawn: the shell used to stick a collider out past the model it belonged to.
        physics.CreateBox({lockerInnerHalfWidth, lockerSize.y * 0.5f, panelHalfThickness},
                          MakeTransform(position + glm::vec3(0.0f, lockerSize.y * 0.5f,
                                                             lockerSize.z * 0.5f - panelHalfThickness),
                                        0.0f),
                          BodyMotion::Static);
        physics.CreateBox({panelHalfThickness, lockerSize.y * 0.5f, lockerSize.z * 0.5f},
                          MakeTransform(position + glm::vec3(-lockerSize.x * 0.5f + panelHalfThickness,
                                                             lockerSize.y * 0.5f, 0.0f),
                                        0.0f),
                          BodyMotion::Static);
        physics.CreateBox({panelHalfThickness, lockerSize.y * 0.5f, lockerSize.z * 0.5f},
                          MakeTransform(position + glm::vec3(lockerSize.x * 0.5f - panelHalfThickness,
                                                             lockerSize.y * 0.5f, 0.0f),
                                        0.0f),
                          BodyMotion::Static);

        // Centred in the clear space, which sits slightly forward of the shell's middle because the
        // back panel takes up depth that the door does not.
        spot.insidePosition = position - glm::vec3(0.0f, 0.0f, panelHalfThickness * 0.5f);
        spot.exitPosition = position - glm::vec3(0.0f, 0.0f, 1.25f);
        // The door is on the -Z face, and yaw 0 faces -Z, so this is looking out of it. It used to
        // be 180 degrees, which turned the player round to face the back of the locker.
        spot.insideYaw = 0.0f;
        // The door fills the clear opening between the sides, so closing it does not drive the panel
        // into them.
        //
        // The open angle is positive, which swings the panel out of the locker. A negative one
        // swings it the other way, which for a door in a frame is a choice and for a cabinet is the
        // panel folding back through its own side: that is what left the locker looking broken
        // after it had been used once.
        spot.doorIndex = AddDoor(scene, meshes, physics, interactions,
                                 position + glm::vec3(-lockerInnerHalfWidth, 0.0f, -lockerSize.z * 0.5f),
                                 0.0f, glm::radians(105.0f),
                                 {lockerInnerHalfWidth * 2.0f, lockerSize.y, 0.06f}, "locker_door", false);
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

    // --- Ammunition crates, at either end of the equipment bench.
    //
    // Two of them, so there is one within reach of wherever you happen to be shooting from. The lid
    // tips open when you take from it, which is the only feedback there is that anything happened:
    // a resupply with no visible result reads as a broken interaction.
    {
        constexpr glm::vec3 crateSize{0.72f, 0.44f, 0.46f};
        constexpr float lidThickness = 0.05f;
        const MeshHandle crateMesh = meshes.Upload(
            Primitives::Box({crateSize.x, crateSize.y - lidThickness, crateSize.z}), "ammo_crate");
        const MeshHandle lidMesh =
            meshes.Upload(Primitives::Box({crateSize.x, lidThickness, crateSize.z}), "ammo_crate_lid");

        // On the floor in front of the bench, not on it: the bench is already full of loose items,
        // and a crate standing inside it is exactly what the level's own overlap check exists to
        // catch.
        const glm::vec3 cratePositions[] = {{kEquipmentBayX - 1.5f, 0.0f, kBayZ + 0.3f},
                                            {kEquipmentBayX + 1.5f, 0.0f, kBayZ + 0.3f}};

        for (const glm::vec3& position : cratePositions)
        {
            AmmoCrate crate;
            const float bodyHeight = crateSize.y - lidThickness;
            const glm::vec3 bodyCentre = position + glm::vec3(0.0f, bodyHeight * 0.5f, 0.0f);
            crate.entity = scene.CreateMeshEntity("ammo_crate", MakeTransform(bodyCentre, 0.0f),
                                                  crateMesh, kAmmoCrateMaterial);
            crate.lidRest = position + glm::vec3(0.0f, bodyHeight + lidThickness * 0.5f, 0.0f);
            crate.lidEntity = scene.CreateMeshEntity(
                "ammo_crate_lid", MakeTransform(crate.lidRest, 0.0f), lidMesh, kAmmoLidMaterial);
            physics.CreateBox({crateSize.x * 0.5f, crateSize.y * 0.5f, crateSize.z * 0.5f},
                              MakeTransform(position + glm::vec3(0.0f, crateSize.y * 0.5f, 0.0f), 0.0f),
                              BodyMotion::Static);

            const auto index = static_cast<int>(m_ammoCrates.size());
            m_ammoCrates.push_back(crate);

            Interactable interactable;
            interactable.entity = crate.entity;
            interactable.kind = InteractionKind::AmmoCrate;
            interactable.verb = "Take ammunition from";
            interactable.name = "Ammunition Crate";
            interactable.payload = index;
            interactable.range = 2.2f;
            interactable.focusOffset = {0.0f, crateSize.y * 0.4f, 0.0f};
            interactions.Register(interactable);
        }
    }

    // --- Loose items, laid out along the equipment bench in one row.
    //
    // Every item the game knows about appears here, weapons included, in the order they are defined.
    // Scattering a few of them across the map meant a weapon could not be found without knowing
    // where to look, and adding an item to items.json left it nowhere in the world at all.
    struct Placement
    {
        const char* key;
        int count;
    };
    const Placement placements[] = {
        {"sidearm", 1},  {"carbine", 1},  {"medkit", 2}, {"battery", 4},
        {"keycard", 1},  {"flare", 2},    {"sample_container", 1},
    };

    constexpr float kSpacing = 0.62f;
    const float rowWidth = kSpacing * static_cast<float>(std::size(placements) - 1);
    float x = kEquipmentBayX - rowWidth * 0.5f;
    for (const Placement& placement : placements)
    {
        const ItemId id = items.IdOf(placement.key);
        const ItemDefinition* definition = items.Get(id);
        if (id == kInvalidItem || definition == nullptr)
        {
            PRED_LOG_WARN(Gameplay, "Test map references unknown item '{}'", placement.key);
            continue;
        }
        // Standing on the bench, not dropped onto it. A pickup's position is the centre of its box,
        // so a fixed clearance buried the tall ones in the surface, and the separation impulse
        // flipped them onto their ends.
        SpawnPickup(scene, meshes, physics, interactions, items, id, placement.count,
                    {x, kBenchTop + definition->size.y * 0.5f + 0.005f, kBayZ + 1.3f}, glm::vec3(0.0f));
        x += kSpacing;
    }

    PRED_LOG_INFO(Gameplay, "World objects: {} doors, {} pickups, {} hiding spots", m_doors.size(),
                  m_pickups.size(), m_hidingSpots.size());
}

int WorldObjects::ChooseSlot(const std::vector<Pickup>& existing, int requested)
{
    if (requested >= 0)
    {
        return requested;
    }
    // Reuse a free record if there is one, so indices stay small enough to fit the wire.
    for (size_t i = 0; i < existing.size(); ++i)
    {
        if (!existing[i].alive)
        {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(existing.size());
}

int WorldObjects::SpawnPickup(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                              InteractionSystem& interactions, const ItemDatabase& items, ItemId item,
                              int count, const glm::vec3& position, const glm::vec3& velocity,
                              int rounds, int reserve, int atIndex)
{
    const ItemDefinition* definition = items.Get(item);
    if (definition == nullptr || count <= 0)
    {
        return -1;
    }

    Pickup pickup;
    pickup.item = item;
    pickup.count = count;
    pickup.rounds = rounds;
    pickup.reserve = reserve;

    Transform transform;
    transform.position = position;
    // Dropped facing whichever way it left the hand, and tumbling, rather than all of them lying in
    // the same direction like stock on a shelf. The seed is the index and the item, so the two
    // machines that both spawn this pickup agree about how it landed.
    {
        const uint32_t seed =
            static_cast<uint32_t>(atIndex + 1) * 2654435761u + static_cast<uint32_t>(item) * 40503u;
        const auto unit = [seed](int which)
        {
            const uint32_t mixed = (seed + static_cast<uint32_t>(which) * 0x9E3779B9u) * 1103515245u;
            return static_cast<float>((mixed >> 8) & 0xFFFFu) / 65535.0f;
        };
        // Turned about the up axis and tipped a little, rather than tumbled about a random one.
        // A free axis puts a rifle on its muzzle and a keycard on one corner as often as not, which
        // is not what dropping something looks like: things land flat and face whichever way they
        // happened to be going. The tip is what keeps it from reading as stock on a shelf.
        constexpr float kMaxTiltDegrees = 14.0f;
        transform.rotation =
            glm::angleAxis(unit(0) * glm::two_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::angleAxis(glm::radians((unit(1) - 0.5f) * 2.0f * kMaxTiltDegrees),
                           glm::vec3(1.0f, 0.0f, 0.0f)) *
            glm::angleAxis(glm::radians((unit(2) - 0.5f) * 2.0f * kMaxTiltDegrees),
                           glm::vec3(0.0f, 0.0f, 1.0f));
    }

    // A weapon is drawn as the parts it is made of rather than as one merged lump, so a dropped
    // rifle looks like the rifle: every piece keeps its own material and its own texture. Anything
    // that is not a weapon is one shape and one colour, and one entity is all it needs.
    const WeaponDefinition* weapon =
        m_weapons != nullptr ? m_weapons->Get(m_weapons->ForItem(definition->key)) : nullptr;
    if (weapon != nullptr)
    {
        const WeaponVisual visual = BuildWeaponVisual(*weapon, m_textures);
        for (size_t part = 0; part < visual.parts.size(); ++part)
        {
            const MeshHandle partMesh = meshes.Upload(
                visual.parts[part].mesh, "pickup_" + definition->key + "_" + visual.parts[part].name);
            pickup.parts.push_back(scene.CreateMeshEntity("pickup_" + definition->key, transform,
                                                          partMesh, visual.parts[part].material));
            pickup.partRest.push_back(visual.parts[part].rest);
        }
    }

    if (pickup.parts.empty())
    {
        const MeshHandle mesh =
            meshes.Upload(ItemMesh(*definition, m_weapons), "item_" + definition->key);
        pickup.parts.push_back(scene.CreateMeshEntity("pickup_" + definition->key, transform, mesh,
                                                      ItemMaterial(*definition)));
        pickup.partRest.push_back(glm::mat4(1.0f));
    }
    pickup.entity = pickup.parts.front();
    // Dropped items are dynamic so they settle naturally rather than floating where they were let
    // go, and they are debris so the player walks through them rather than kicking them about. An
    // item wedged against a capsule is one that cannot be picked up.
    pickup.body = physics.CreateBox(definition->size * 0.5f, transform, BodyMotion::Dynamic,
                                    std::max(definition->mass, 0.01f) * 400.0f, PhysicsLayer::Debris);
    if (pickup.body.IsValid() && glm::length(velocity) > 0.0f)
    {
        physics.SetLinearVelocity(pickup.body, velocity);
    }

    // The index is the name everyone uses for this thing afterwards: a pickup is taken by index and
    // removed by index on every machine at once. So when the host says which one it made, that is
    // the one that gets made here, even if this machine would have chosen differently.
    //
    // It used to choose for itself on both sides. Two machines with different holes in their pickup
    // lists then disagreed about which item was which, and from that moment taking one removed a
    // different one somewhere else: an item that could not be picked up on one screen and a second
    // copy of it on another.
    const int index = ChooseSlot(m_pickups, atIndex);
    if (static_cast<size_t>(index) >= m_pickups.size())
    {
        // Told to use an index past the end, which happens whenever this machine has had fewer
        // drops than the host. The gap is filled with dead records so the numbering still lines up.
        Pickup empty;
        empty.alive = false;
        m_pickups.resize(static_cast<size_t>(index) + 1, empty);
    }
    else if (m_pickups[static_cast<size_t>(index)].alive)
    {
        // Something is already there. The host is the authority on what that index means, so what
        // was there is wrong and goes.
        ConsumePickup(index, scene, physics, interactions);
    }
    m_pickups[static_cast<size_t>(index)] = pickup;

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

void WorldObjects::Despawn(Pickup& pickup, Scene& scene, PhysicsWorld& physics,
                           InteractionSystem& interactions)
{
    interactions.Unregister(pickup.entity);
    physics.DestroyBody(pickup.body);
    for (const Entity part : pickup.parts)
    {
        scene.Destroy(part);
    }
    pickup.parts.clear();
    pickup.partRest.clear();
    pickup.alive = false;
    pickup.item = kInvalidItem;
    pickup.count = 0;
    pickup.body = BodyHandle{};
}

bool WorldObjects::ConsumePickup(int index, Scene& scene, PhysicsWorld& physics,
                                 InteractionSystem& interactions)
{
    Pickup* pickup = GetPickup(index);
    if (pickup == nullptr)
    {
        return false;
    }
    Despawn(*pickup, scene, physics, interactions);
    return true;
}

void WorldObjects::SetNetworkState(int index, const glm::vec3& position, const glm::quat& rotation)
{
    if (index < 0 || static_cast<size_t>(index) >= m_pickups.size())
    {
        return;
    }
    Pickup& pickup = m_pickups[static_cast<size_t>(index)];
    pickup.netPosition = position;
    pickup.netRotation = rotation;
    pickup.netValid = true;
}

void WorldObjects::FollowNetworkState(PhysicsWorld& physics, float dt)
{
    if (dt <= 0.0f)
    {
        return;
    }

    // Eased towards the host's answer rather than set to it. The host speaks thirty times a second
    // and the screen draws at least twice that, so setting the transform on each new answer draws a
    // falling item in visible steps. It also fought the client's own solver, which went on
    // integrating between updates and was snapped back, and which could push a thin item through
    // the floor on the frame it was teleported into it.
    const float blend = 1.0f - std::exp(-18.0f * dt);
    for (Pickup& pickup : m_pickups)
    {
        if (!pickup.alive || !pickup.netValid || !physics.IsValid(pickup.body))
        {
            continue;
        }

        // Nothing of this machine's own is allowed to move it, or the ease is a tug of war.
        physics.SetLinearVelocity(pickup.body, glm::vec3(0.0f));
        physics.SetAngularVelocity(pickup.body, glm::vec3(0.0f));

        const Transform current = physics.GetTransform(pickup.body);
        Transform next;
        // A long way out means it was just spawned or teleported, and easing across a room looks
        // worse than arriving.
        const bool far = glm::distance(current.position, pickup.netPosition) > 1.5f;
        next.position = far ? pickup.netPosition : glm::mix(current.position, pickup.netPosition, blend);
        next.rotation = far ? pickup.netRotation : glm::slerp(current.rotation, pickup.netRotation, blend);
        next.scale = current.scale;
        physics.SetTransform(pickup.body, next);
    }
}

void WorldObjects::Update(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions,
                          float dt)
{
    for (Door& door : m_doors)
    {
        if (door.IsMoving())
        {
            const float step = door.speed * dt;
            const float delta = door.target - door.angle;
            door.angle += std::clamp(delta, -step, step);
            if (std::abs(door.target - door.angle) < 1e-3f)
            {
                door.angle = door.target;
            }
        }

        const Transform transform = DoorPanelTransform(door);
        if (Transform* sceneTransform = scene.GetTransform(door.entity))
        {
            *sceneTransform = transform;
        }

        // Kinematic movement rather than teleporting, so the panel shoves what is in its way. It is
        // driven every tick, not only while the angle is changing: MoveKinematic works by setting
        // the velocity needed to reach the target within dt, and that velocity persists until it is
        // set again. Skipping still doors left every one of them carrying the velocity from its
        // last moving tick, so the collider slowly sailed away from the panel it belonged to.
        // Driving a door that is already where it should be asks for zero velocity, which both
        // stops it and pulls back anything that has drifted.
        physics.MoveKinematic(door.body, transform, dt);
    }

    // Crate lids: they tip open when something is taken and fall shut again on their own. There is
    // no collider on the lid, because it exists purely so that taking from a crate is visible.
    for (AmmoCrate& crate : m_ammoCrates)
    {
        constexpr float kOpenSpeed = 7.0f;
        constexpr float kCloseSpeed = 2.4f;
        const bool opening = crate.lidTarget > crate.lidAngle;
        const float step = (opening ? kOpenSpeed : kCloseSpeed) * dt;
        crate.lidAngle += std::clamp(crate.lidTarget - crate.lidAngle, -step, step);
        if (opening && crate.lidTarget - crate.lidAngle < 1e-3f)
        {
            crate.lidTarget = 0.0f;
        }

        if (Transform* transform = scene.GetTransform(crate.lidEntity))
        {
            // Hinged along the back edge, so it swings up and back rather than turning about its
            // own middle and sinking half of itself into the crate.
            constexpr float kHalfDepth = 0.23f;
            const float lift = std::sin(crate.lidAngle) * kHalfDepth;
            const float pull = (1.0f - std::cos(crate.lidAngle)) * kHalfDepth;
            transform->rotation = glm::angleAxis(-crate.lidAngle, glm::vec3(1.0f, 0.0f, 0.0f));
            transform->position = crate.lidRest + glm::vec3(0.0f, lift, pull);
        }
    }

    // Dropped items are dynamic bodies, so their meshes have to follow the simulation. Without this
    // a dropped item's collider tumbled away while the thing you could see stayed hanging in the
    // air where you let go of it.
    //
    // This is also the single place a client would instead apply transforms sent by the host, so
    // keep it as the only route from a pickup's body to its visible transform.
    for (Pickup& pickup : m_pickups)
    {
        if (!pickup.alive || !pickup.body.IsValid() || !physics.IsActive(pickup.body))
        {
            continue;
        }
        const Transform body = physics.GetTransform(pickup.body);
        // Every piece rides the body's frame, each keeping where it sits on the weapon.
        for (size_t part = 0; part < pickup.parts.size() && part < pickup.partRest.size(); ++part)
        {
            Transform* transform = scene.GetTransform(pickup.parts[part]);
            if (transform == nullptr)
            {
                continue;
            }
            const glm::mat4 world = glm::translate(glm::mat4(1.0f), body.position) *
                                    glm::mat4_cast(body.rotation) * pickup.partRest[part];
            transform->position = glm::vec3(world[3]);
            transform->rotation = glm::quat_cast(glm::mat3(world));
            transform->scale = glm::vec3(1.0f);
        }

        // Anything that has fallen out of the level is gone; leaving it costs a body and an
        // interactable for something nobody can ever reach.
        if (body.position.y < -25.0f)
        {
            Despawn(pickup, scene, physics, interactions);
        }
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

WorldObjects::AmmoCrate* WorldObjects::GetAmmoCrate(int index)
{
    if (index < 0 || index >= static_cast<int>(m_ammoCrates.size()))
    {
        return nullptr;
    }
    return &m_ammoCrates[static_cast<size_t>(index)];
}

bool WorldObjects::DrawFromAmmoCrate(int index, InteractionSystem& interactions)
{
    AmmoCrate* crate = GetAmmoCrate(index);
    if (crate == nullptr || crate->refillsLeft == 0)
    {
        return false;
    }
    if (crate->refillsLeft > 0)
    {
        --crate->refillsLeft;
    }

    // The lid tips open and falls shut again on its own, so taking from it looks like taking from it.
    crate->lidTarget = glm::radians(62.0f);

    if (Interactable* interactable = interactions.Find(crate->entity))
    {
        if (crate->refillsLeft == 0)
        {
            interactable->name = "Ammunition Crate (empty)";
            interactable->enabled = false;
        }
        else if (crate->refillsLeft > 0)
        {
            interactable->name = "Ammunition Crate x" + std::to_string(crate->refillsLeft);
        }
    }
    return true;
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
    // Through the same teardown a single pickup goes through, rather than a second copy of it. The
    // copy that used to be here destroyed only the entity a pickup calls its own and not the rest
    // of the parts it is drawn as, so starting a new game left most of every dropped rifle lying on
    // the floor of a world that no longer existed. Two teardowns for one thing is how that happened
    // and there is now one.
    for (Pickup& pickup : m_pickups)
    {
        if (pickup.alive)
        {
            Despawn(pickup, scene, physics, interactions);
        }
    }
    for (HidingSpot& spot : m_hidingSpots)
    {
        interactions.Unregister(spot.entity);
        scene.Destroy(spot.entity);
    }
    for (AmmoCrate& crate : m_ammoCrates)
    {
        interactions.Unregister(crate.entity);
        scene.Destroy(crate.lidEntity);
        scene.Destroy(crate.entity);
    }
    m_doors.clear();
    m_pickups.clear();
    m_hidingSpots.clear();
    m_ammoCrates.clear();
    m_itemMeshes.clear();
}

} // namespace pred
