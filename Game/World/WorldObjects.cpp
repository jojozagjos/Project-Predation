#include "Game/World/WorldObjects.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/ItemAppearance.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/Weapons/WeaponDatabase.h"
#include "Game/World/TestMap.h"
#include "Game/World/LabMap.h"

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
// A vent's hatch: the crawlspace's sheet steel.
const Material kHatchMaterial = Material::Metal({0.58f, 0.6f, 0.61f}, 0.55f);
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

namespace
{

// Part of the level, for the lamps' kept shadows (see MeshRenderer::levelGeometry).
void MarkLevel(Scene& scene, Entity entity)
{
    if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
    {
        renderer->levelGeometry = true;
    }
}

} // namespace

namespace
{

// How far a roller shutter has rolled up, 0 to 1.
float Rolled(const WorldObjects::Door& door)
{
    const float span = door.openYaw - door.closedYaw;
    return std::abs(span) > 1.0e-4f ? std::clamp((door.angle - door.closedYaw) / span, 0.0f, 1.0f) : 0.0f;
}

} // namespace

Transform WorldObjects::DoorBodyTransform(const Door& door) const
{
    Transform transform = DoorPanelTransform(door);
    if (door.rollUp)
    {
        const float high = door.panelOffset.y * 2.0f;
        transform.scale = glm::vec3(1.0f);
        transform.position = door.hinge + transform.rotation * glm::vec3(door.panelOffset.x, 0.0f, door.panelOffset.z) +
                             glm::vec3(0.0f, door.panelOffset.y + Rolled(door) * high, 0.0f);
    }
    return transform;
}

Transform WorldObjects::DoorPanelTransform(const Door& door) const
{
    // A roller shutter: its foot rising, its top where it is, what has gone up rolled into the drum (all but a band of it).
    if (door.rollUp)
    {
        const float high = door.panelOffset.y * 2.0f;
        const float shown = 1.0f - 0.94f * Rolled(door);
        Transform transform;
        transform.rotation = glm::angleAxis(door.closedYaw, glm::vec3(0.0f, 1.0f, 0.0f));
        transform.position = door.hinge + transform.rotation * glm::vec3(door.panelOffset.x, 0.0f, door.panelOffset.z) +
                             glm::vec3(0.0f, high - high * shown * 0.5f, 0.0f);
        transform.scale = glm::vec3(1.0f, shown, 1.0f);
        return transform;
    }
    // The panel hangs off the hinge, so rotating about the hinge sweeps it rather than spinning it
    // in place. Rotating the offset is the whole trick. (Lying flat: laid down first, then swung about its hinge, its own y.)
    const glm::quat rotation = door.flat ? door.flatTurn * glm::angleAxis(door.angle, glm::vec3(0.0f, 1.0f, 0.0f))
                                         : glm::angleAxis(door.angle, glm::vec3(0.0f, 1.0f, 0.0f));
    Transform transform;
    transform.rotation = rotation;
    transform.position = door.hinge + rotation * door.panelOffset;
    return transform;
}

int WorldObjects::AddDoor(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                          InteractionSystem& interactions, const glm::vec3& hinge, float closedYaw,
                          float openYaw, const glm::vec3& panelSize, const std::string& name,
                          bool registerInteractable, const MeshData* shape, const Material* material, const char* label)
{
    Door door;
    door.hinge = hinge;
    door.closedYaw = closedYaw;
    door.openYaw = openYaw;
    door.angle = closedYaw;
    door.target = closedYaw;
    // Hinge at one edge: the panel's centre sits half a width along its own local X.
    door.panelOffset = glm::vec3(panelSize.x * 0.5f, panelSize.y * 0.5f, 0.0f);

    // Drawn as the shape given, when there is one (a locker's door, with its slits); it still collides as
    // the plain panel.
    const MeshHandle mesh = meshes.Upload(shape != nullptr ? *shape : Primitives::Box(panelSize), name + "_panel");
    const Transform transform = DoorPanelTransform(door);
    door.entity = scene.CreateMeshEntity(name, transform, mesh, material != nullptr ? *material : kDoorMaterial);
    MarkLevel(scene, door.entity);
    door.body = physics.CreateBox(panelSize * 0.5f, transform, BodyMotion::Kinematic);

    const auto index = static_cast<int>(m_doors.size());
    m_doors.push_back(door);

    if (registerInteractable)
    {
        Interactable interactable;
        interactable.entity = door.entity;
        interactable.kind = InteractionKind::Door;
        interactable.verb = "Open";
        interactable.name = label;
        interactable.payload = index;
        interactable.range = 2.6f;
        interactions.Register(interactable);
    }
    return index;
}

namespace
{

// A door's paint at a corner, multiplying its material's colour.
uint32_t DoorPaint(const glm::vec3& colour)
{
    const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(colour.r) | (byte(colour.g) << 8) | (byte(colour.b) << 16) | 0xFF000000u;
}

// Some of a door: a box of `size` at `centre` in the panel's frame (its middle at the origin), painted `colour`.
void DoorBox(MeshData& mesh, const glm::vec3& centre, const glm::vec3& size, const glm::vec3& colour)
{
    const MeshData box = Primitives::Box(size);
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (MeshVertex vertex : box.vertices)
    {
        vertex.position += centre;
        vertex.color = DoorPaint(colour);
        mesh.vertices.push_back(vertex);
    }
    for (const uint32_t index : box.indices)
    {
        mesh.indices.push_back(base + index);
    }
}

// A round of it through its thickness (along z): a disc `radius` across, `thick` through, at `centre`.
void DoorDisc(MeshData& mesh, const glm::vec3& centre, float radius, float thick, const glm::vec3& colour)
{
    const MeshData disc = Primitives::Cylinder(radius, thick, 20);
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (MeshVertex vertex : disc.vertices)
    {
        // (Its axis turned from up to through the door: y to z.)
        vertex.position = centre + glm::vec3(vertex.position.x, -vertex.position.z, vertex.position.y);
        vertex.normal = glm::vec3(vertex.normal.x, -vertex.normal.z, vertex.normal.y);
        vertex.color = DoorPaint(colour);
        mesh.vertices.push_back(vertex);
    }
    for (const uint32_t index : disc.indices)
    {
        mesh.indices.push_back(base + index);
    }
}

// A door of its kind (PlacedDoor::Style), `size` across, high and thick, centred on itself as the plain panel it is felt as.
// What is on its faces is on both: a door is seen from either side.
MeshData DoorPanel(WorldObjects::PlacedDoor::Style style, const glm::vec3& size)
{
    using Style = WorldObjects::PlacedDoor::Style;
    MeshData mesh;
    const float w = size.x;
    const float h = size.y;
    const float t = size.z;
    const float foot = -h * 0.5f;
    const glm::vec3 steel{0.62f, 0.63f, 0.64f};
    const glm::vec3 dark{0.1f, 0.11f, 0.12f};
    const glm::vec3 glass{0.08f, 0.1f, 0.12f};
    const glm::vec3 yellow{0.86f, 0.64f, 0.1f};
    // Its body, in its own colour: a room's painted, the rest steel -- a way out's painted green, as a way out is.
    const glm::vec3 body = style == Style::Room ? glm::vec3(0.66f, 0.63f, 0.57f) : style == Style::Exit ? glm::vec3(0.3f, 0.44f, 0.36f)
                         : style == Style::Heavy ? glm::vec3(0.4f, 0.42f, 0.44f) : style == Style::Airlock ? glm::vec3(0.6f, 0.61f, 0.6f) : glm::vec3(0.5f, 0.53f, 0.56f);
    DoorBox(mesh, glm::vec3(0.0f), size, body);
    const float latch = w * 0.5f - 0.1f; // along it from its middle, away from the hinge
    for (const float side : {-1.0f, 1.0f})
    {
        const float face = side * t * 0.5f;
        const auto on = [&](float x, float y, float sx, float sy, float proud, const glm::vec3& colour)
        { DoorBox(mesh, {x, foot + y, face + side * proud * 0.5f}, {sx, sy, proud}, colour); };
        // A lever handle on its rose, at a hand's height.
        const auto lever = [&]
        {
            on(latch, 1.0f, 0.055f, 0.08f, 0.012f, steel);
            DoorBox(mesh, {latch - 0.05f, foot + 1.0f, face + side * 0.035f}, {0.13f, 0.022f, 0.022f}, steel);
            DoorBox(mesh, {latch, foot + 1.0f, face + side * 0.02f}, {0.02f, 0.02f, 0.03f}, steel);
        };
        // Hazard stripes in a band, alternately yellow and black.
        const auto stripes = [&](float x0, float x1, float y0, float y1, bool across)
        {
            const float length = across ? x1 - x0 : y1 - y0;
            const int count = std::max(2, static_cast<int>(length / 0.1f));
            for (int i = 0; i < count; ++i)
            {
                const float a = static_cast<float>(i) / static_cast<float>(count);
                const float b = static_cast<float>(i + 1) / static_cast<float>(count);
                const glm::vec3& colour = i % 2 == 0 ? yellow : dark;
                if (across)
                {
                    on(x0 + (x1 - x0) * (a + b) * 0.5f, (y0 + y1) * 0.5f, (x1 - x0) / static_cast<float>(count), y1 - y0, 0.004f, colour);
                }
                else
                {
                    on((x0 + x1) * 0.5f, y0 + (y1 - y0) * (a + b) * 0.5f, x1 - x0, (y1 - y0) / static_cast<float>(count), 0.004f, colour);
                }
            }
        };
        switch (style)
        {
        case Style::Room:
            // A strip of glass to see through, in its frame; a kick plate along its foot.
            on(0.12f, 1.42f, 0.2f, 0.82f, 0.006f, steel);
            on(0.12f, 1.42f, 0.14f, 0.74f, 0.009f, glass);
            on(0.0f, 0.12f, w - 0.06f, 0.22f, 0.004f, steel);
            lever();
            break;
        case Style::Security:
            // Plain steel, stiffened across; a card reader by its handle, its light lit.
            for (const float y : {0.45f, 1.05f, 1.65f})
            {
                on(0.0f, y, w - 0.12f, 0.035f, 0.007f, body * 0.9f);
            }
            on(latch, 1.28f, 0.075f, 0.12f, 0.02f, dark);
            on(latch, 1.31f, 0.02f, 0.02f, 0.024f, glm::vec3(0.25f, 0.9f, 0.35f));
            lever();
            break;
        case Style::Heavy:
            // Thick steel on two uprights, hazard-striped along its foot, a pull bar to haul it by.
            stripes(-w * 0.5f + 0.02f, w * 0.5f - 0.02f, 0.03f, 0.29f, true);
            for (const float x : {-w * 0.25f, w * 0.25f})
            {
                on(x, h * 0.55f, 0.05f, h * 0.8f, 0.012f, body * 0.85f);
            }
            on(latch, 1.05f, 0.04f, 0.42f, 0.055f, steel);
            on(latch, 1.24f, 0.06f, 0.04f, 0.03f, steel);
            on(latch, 0.86f, 0.06f, 0.04f, 0.03f, steel);
            break;
        case Style::Exit:
            // A push bar across it at a hand's height on its mounts, a square of glass over it.
            on(0.0f, 1.0f, w - 0.22f, 0.05f, 0.06f, steel);
            on(-w * 0.5f + 0.13f, 1.0f, 0.06f, 0.1f, 0.03f, steel * 0.7f);
            on(w * 0.5f - 0.13f, 1.0f, 0.06f, 0.1f, 0.03f, steel * 0.7f);
            on(0.0f, 1.55f, 0.32f, 0.32f, 0.006f, steel);
            on(0.0f, 1.55f, 0.26f, 0.26f, 0.009f, glass);
            on(0.0f, 0.12f, w - 0.06f, 0.22f, 0.004f, steel);
            break;
        case Style::Airlock:
            // A round port in a ring at the eye, hazard stripes up both edges, a lever to dog it shut.
            DoorDisc(mesh, {0.0f, foot + 1.5f, face + side * 0.01f}, 0.19f, 0.02f, steel * 0.8f);
            DoorDisc(mesh, {0.0f, foot + 1.5f, face + side * 0.014f}, 0.14f, 0.02f, glass);
            stripes(-w * 0.5f + 0.01f, -w * 0.5f + 0.09f, 0.05f, h - 0.05f, false);
            stripes(w * 0.5f - 0.09f, w * 0.5f - 0.01f, 0.05f, h - 0.05f, false);
            lever();
            break;
        }
    }
    return mesh;
}

// A locker's door, centred on itself like the plain panel it stands in for: solid, but for a band of
// narrow slits across it at the height of the eyes of whoever is standing inside, to look out through.
MeshData LouvredPanel(const glm::vec3& size)
{
    constexpr float kBandLow = 1.46f;  // up from the bottom of the door
    constexpr float kBandHigh = 1.84f;
    constexpr float kBandWidth = 0.56f;
    constexpr int kSlits = 8;
    constexpr float kSlit = 0.024f;
    MeshData mesh;
    const float bottom = -size.y * 0.5f;
    const auto piece = [&](float x0, float x1, float y0, float y1, float depth)
    {
        mesh.Append(Primitives::Box({x1 - x0, y1 - y0, depth}),
                    glm::translate(glm::mat4(1.0f), glm::vec3((x0 + x1) * 0.5f, bottom + (y0 + y1) * 0.5f, 0.0f)));
    };
    const float half = size.x * 0.5f;
    const float band = kBandWidth * 0.5f;
    piece(-half, half, 0.0f, kBandLow, size.z);
    piece(-half, half, kBandHigh, size.y, size.z);
    piece(-half, -band, kBandLow, kBandHigh, size.z);
    piece(band, half, kBandLow, kBandHigh, size.z);
    // The slats between the slits: thinner than the door, as pressed louvres are.
    const float slat = ((kBandHigh - kBandLow) - kSlits * kSlit) / static_cast<float>(kSlits + 1);
    for (int i = 0; i <= kSlits; ++i)
    {
        const float y0 = kBandLow + static_cast<float>(i) * (slat + kSlit);
        piece(-band, band, y0, y0 + slat, size.z * 0.4f);
    }
    return mesh;
}

// A roller shutter (PlacedDoor::rollUp): its curtain of steel slats, a ridge along each both sides; a heavy bar along its foot, a
// pull handle in its middle either side; painted as its kind is (an airlock's steel, anything else a works' grey-green).
MeshData ShutterPanel(WorldObjects::PlacedDoor::Style style, const glm::vec3& size)
{
    MeshData mesh;
    const float w = size.x;
    const float h = size.y;
    const float t = size.z;
    const float foot = -h * 0.5f;
    const glm::vec3 body = style == WorldObjects::PlacedDoor::Style::Airlock ? glm::vec3(0.6f, 0.61f, 0.6f) : glm::vec3(0.42f, 0.46f, 0.42f);
    const glm::vec3 ridge = body * 0.82f;
    const glm::vec3 dark{0.1f, 0.11f, 0.12f};
    DoorBox(mesh, glm::vec3(0.0f), size, body);
    for (float y = 0.16f; y < h - 0.04f; y += 0.1f)
    {
        DoorBox(mesh, {0.0f, foot + y, 0.0f}, {w - 0.04f, 0.022f, t + 0.016f}, ridge);
    }
    DoorBox(mesh, {0.0f, foot + 0.04f, 0.0f}, {w, 0.08f, t + 0.04f}, dark);
    for (const float side : {-1.0f, 1.0f})
    {
        DoorBox(mesh, {0.0f, foot + 0.16f, side * (t * 0.5f + 0.04f)}, {0.32f, 0.03f, 0.03f}, dark);
        for (const float x : {-0.15f, 0.15f})
        {
            DoorBox(mesh, {x, foot + 0.12f, side * (t * 0.5f + 0.025f)}, {0.03f, 0.08f, 0.03f}, dark);
        }
    }
    return mesh;
}

// A vent's hatch, centred on itself like the plain panel it stands in for: a frame round a grille of slats across it, air
// between them, the slats thinner than the frame and turned a little as a vent's louvres are.
MeshData VentGrille(const glm::vec3& size)
{
    constexpr float kFrame = 0.05f;
    constexpr int kSlats = 9;
    MeshData mesh;
    const float bottom = -size.y * 0.5f;
    const auto piece = [&](float x0, float x1, float y0, float y1, float depth, float tilt)
    {
        mesh.Append(Primitives::Box({x1 - x0, y1 - y0, depth}),
                    glm::translate(glm::mat4(1.0f), glm::vec3((x0 + x1) * 0.5f, bottom + (y0 + y1) * 0.5f, 0.0f)) *
                        glm::rotate(glm::mat4(1.0f), tilt, glm::vec3(1.0f, 0.0f, 0.0f)));
    };
    const float half = size.x * 0.5f;
    piece(-half, half, 0.0f, kFrame, size.z, 0.0f);
    piece(-half, half, size.y - kFrame, size.y, size.z, 0.0f);
    piece(-half, -half + kFrame, kFrame, size.y - kFrame, size.z, 0.0f);
    piece(half - kFrame, half, kFrame, size.y - kFrame, size.z, 0.0f);
    const float pitch = (size.y - 2.0f * kFrame) / static_cast<float>(kSlats);
    for (int i = 0; i < kSlats; ++i)
    {
        const float y = kFrame + pitch * (static_cast<float>(i) + 0.5f);
        piece(-half + kFrame, half - kFrame, y - pitch * 0.3f, y + pitch * 0.3f, size.z * 0.3f, glm::radians(-30.0f));
    }
    return mesh;
}

} // namespace

int WorldObjects::AddLocker(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions,
                            const glm::vec3& position, float yaw)
{
    // Its door on its own -Z face; turned by `yaw`, everything about it is turned with it.
    const glm::vec3 lockerSize{1.06f, 2.05f, 0.88f};
    constexpr float panelHalfThickness = 0.06f;
    const float lockerInnerHalfWidth = lockerSize.x * 0.5f - panelHalfThickness * 2.0f;
    const glm::quat turn = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    const auto at = [&](const glm::vec3& local) { return position + turn * local; };

    HidingSpot spot;
    // The shell is a visual and a collider; the player stands inside it while hidden.
    spot.entity = scene.CreateMeshEntity("locker", MakeTransform(at({0.0f, lockerSize.y * 0.5f, 0.0f}), yaw), m_lockerMesh,
                                         kLockerMaterial);
    MarkLevel(scene, spot.entity);
    // Back, fitted between the two sides. Every collider below sits exactly where its panel is drawn.
    spot.bodies.push_back(physics.CreateBox({lockerInnerHalfWidth, lockerSize.y * 0.5f, panelHalfThickness},
                      MakeTransform(at({0.0f, lockerSize.y * 0.5f, lockerSize.z * 0.5f - panelHalfThickness}), yaw),
                      BodyMotion::Static));
    spot.bodies.push_back(physics.CreateBox({panelHalfThickness, lockerSize.y * 0.5f, lockerSize.z * 0.5f},
                      MakeTransform(at({-lockerSize.x * 0.5f + panelHalfThickness, lockerSize.y * 0.5f, 0.0f}), yaw),
                      BodyMotion::Static));
    spot.bodies.push_back(physics.CreateBox({panelHalfThickness, lockerSize.y * 0.5f, lockerSize.z * 0.5f},
                      MakeTransform(at({lockerSize.x * 0.5f - panelHalfThickness, lockerSize.y * 0.5f, 0.0f}), yaw),
                      BodyMotion::Static));
    // The roof, between the sides, from behind the door to in front of the back: without it anything
    // that climbed up there fell straight in.
    {
        const float front = -lockerSize.z * 0.5f + 0.07f;
        const float back = lockerSize.z * 0.5f - panelHalfThickness * 2.0f - 0.005f;
        spot.bodies.push_back(physics.CreateBox({lockerInnerHalfWidth - 0.005f, panelHalfThickness, (back - front) * 0.5f},
                          MakeTransform(at({0.0f, lockerSize.y - panelHalfThickness, (front + back) * 0.5f}), yaw),
                          BodyMotion::Static));
    }

    // Centred in the clear space, which sits slightly forward of the shell's middle because the back
    // panel takes up depth that the door does not; looking out of the door.
    spot.insidePosition = at({0.0f, 0.0f, -panelHalfThickness * 0.5f});
    spot.exitPosition = at({0.0f, 0.0f, -1.25f});
    // As a look, which turns the other way to a thing: a thing turned by yaw faces (-sin, -cos), a look of yaw (sin, -cos).
    spot.insideYaw = -yaw;
    // The door fills the clear opening between the sides, and swings out of the locker, not back
    // through its own side.
    const glm::vec3 doorSize{lockerInnerHalfWidth * 2.0f, lockerSize.y, 0.06f};
    const MeshData louvred = LouvredPanel(doorSize);
    spot.doorIndex = AddDoor(scene, meshes, physics, interactions, at({-lockerInnerHalfWidth, 0.0f, -lockerSize.z * 0.5f}), yaw,
                             yaw + glm::radians(105.0f), doorSize, "locker_door", false, &louvred);
    spot.doorEntity = m_doors[static_cast<size_t>(spot.doorIndex)].entity;
    // Standing open to begin with: you have to be able to see a hiding place to decide to run for it.
    SetDoorOpen(spot.doorIndex, true, interactions);

    const auto index = static_cast<int>(m_hidingSpots.size());
    m_hidingSpots.push_back(spot);

    Interactable interactable;
    interactable.entity = spot.entity;
    interactable.kind = InteractionKind::HidingSpot;
    interactable.verb = "Hide";
    interactable.name = "";
    interactable.payload = index;
    interactable.range = 2.4f;
    // In the middle of its open doorway, at the height of a look into it: at its foot, as it was, the
    // prompt only came up for somebody looking at the floor in front of it.
    // In its own frame: it is turned with it (turned here too, a locker facing any way but one had its focus to the side
    // or behind it).
    interactable.focusOffset = glm::vec3(0.0f, 0.1f, -0.4f);
    interactions.Register(interactable);
    return index;
}

int WorldObjects::AddAmmoCrate(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions, const glm::vec3& position,
                               float yaw)
{
    constexpr glm::vec3 crateSize{0.72f, 0.44f, 0.46f};
    constexpr float lidThickness = 0.05f;
    AmmoCrate crate;
    const float bodyHeight = crateSize.y - lidThickness;
    crate.entity = scene.CreateMeshEntity("ammo_crate", MakeTransform(position + glm::vec3(0.0f, bodyHeight * 0.5f, 0.0f), yaw),
                                          m_crateMesh, kAmmoCrateMaterial);
    crate.lidRest = position + glm::vec3(0.0f, bodyHeight + lidThickness * 0.5f, 0.0f);
    MarkLevel(scene, crate.entity);
    crate.lidEntity = scene.CreateMeshEntity("ammo_crate_lid", MakeTransform(crate.lidRest, yaw), m_lidMesh, kAmmoLidMaterial);
    MarkLevel(scene, crate.lidEntity);
    crate.yaw = yaw;
    crate.body = physics.CreateBox({crateSize.x * 0.5f, crateSize.y * 0.5f, crateSize.z * 0.5f},
                      MakeTransform(position + glm::vec3(0.0f, crateSize.y * 0.5f, 0.0f), yaw), BodyMotion::Static);

    const auto index = static_cast<int>(m_ammoCrates.size());
    m_ammoCrates.push_back(crate);

    Interactable interactable;
    interactable.entity = crate.entity;
    interactable.kind = InteractionKind::AmmoCrate;
    interactable.verb = "Take ammunition";
    interactable.name = "";
    interactable.payload = index;
    interactable.range = 2.2f;
    interactable.focusOffset = {0.0f, crateSize.y * 0.4f, 0.0f};
    interactions.Register(interactable);
    return index;
}

void WorldObjects::AddFacility(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, InteractionSystem& interactions,
                               const ItemDatabase& items, const Placements& placements)
{
    // In the order given, so every machine building the same facility numbers its doors, lockers,
    // crates and items the same.
    for (const PlacedDoor& door : placements.doors)
    {
        int index = -1;
        if (door.hatch)
        {
            const glm::vec3 size{door.width, door.height, 0.03f};
            const MeshData grille = VentGrille(size);
            index = AddDoor(scene, meshes, physics, interactions, door.hinge, door.closedYaw, door.openYaw, size, "vent_hatch", true, &grille,
                            &kHatchMaterial, "");
            // A ceiling vent's: laid flat in the ceiling, and put where it lies.
            if (door.flat)
            {
                Door& lying = m_doors[static_cast<size_t>(index)];
                lying.flat = true;
                lying.flatTurn = glm::angleAxis(door.flatYaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
                const Transform placed = DoorPanelTransform(lying);
                if (Transform* transform = scene.GetTransform(lying.entity))
                {
                    *transform = placed;
                }
                physics.SetTransform(lying.body, placed);
            }
        }
        else
        {
            // Drawn as its kind (DoorPanel), felt as the plain panel; a room's painted, the rest steel, a heavy one thicker.
            using Style = PlacedDoor::Style;
            static const Material kPainted = Material::Diffuse(glm::vec3(1.0f), 0.6f);
            static const Material kSteel = []
            {
                Material steel = Material::Diffuse(glm::vec3(1.0f), 0.45f);
                steel.metallic = 0.55f;
                return steel;
            }();
            const glm::vec3 size{door.width, door.height, door.rollUp ? 0.06f : door.style == Style::Heavy ? 0.13f : 0.09f};
            const MeshData panel = door.rollUp ? ShutterPanel(door.style, size) : DoorPanel(door.style, size);
            // (Its prompt the verb alone -- Open, Close, Locked: what it is plain to see.)
            const char* label = "";
            // (Named by its kind: the library keeps one mesh to a name.)
            const std::string name = std::string(door.rollUp ? "facility_shutter_" : "facility_door_") + std::to_string(static_cast<int>(door.style));
            index = AddDoor(scene, meshes, physics, interactions, door.hinge, door.closedYaw, door.openYaw, size, name, true, &panel,
                            door.style == Style::Room || door.style == Style::Exit ? &kPainted : &kSteel, label);
        }
        m_doors[static_cast<size_t>(index)].locked = door.locked;
        m_doors[static_cast<size_t>(index)].speed = door.style == PlacedDoor::Style::Airlock ? 1.5f : door.style == PlacedDoor::Style::Heavy ? 2.0f : 4.5f;
        // A roller shutter: rolling up over a couple of seconds, its handle reached from a little further off (it rises out of reach
        // of where a door's would be).
        if (door.rollUp)
        {
            Door& shutter = m_doors[static_cast<size_t>(index)];
            shutter.rollUp = true;
            shutter.speed = 0.5f * std::abs(shutter.openYaw - shutter.closedYaw);
            if (Interactable* interactable = interactions.Find(shutter.entity))
            {
                interactable->range = 3.4f;
            }
        }
    }
    // An airlock's two doors, each told of the other (PlacedDoor::partner: by its place in this list, which is where it was added
    // here, from the first).
    const size_t first = m_doors.size() - placements.doors.size();
    for (size_t i = 0; i < placements.doors.size(); ++i)
    {
        const int partner = placements.doors[i].partner;
        if (partner >= 0 && static_cast<size_t>(partner) < placements.doors.size())
        {
            m_doors[first + i].partner = static_cast<int>(first) + partner;
        }
    }
    // And by each airlock door, on each side of the wall it is in, its warning light: a lens on a steel box, lit by Update. Put on
    // the wall's face either side -- found by looking back at the wall from out in front of it.
    for (size_t i = first; i < m_doors.size(); ++i)
    {
        Door& door = m_doors[i];
        if (door.partner < 0)
        {
            continue;
        }
        const PlacedDoor& placed = placements.doors[i - first];
        const glm::vec3 along{std::cos(door.closedYaw), 0.0f, -std::sin(door.closedYaw)};
        const glm::vec3 normal{along.z, 0.0f, -along.x};
        // (Beside its frame at its top corner, away from its hinge: over it is the way out's sign.)
        const glm::vec3 middle = door.hinge + along * (placed.width + 0.32f) + glm::vec3(0.0f, placed.height - 0.1f, 0.0f);
        static const MeshData kHousing = Primitives::Box({0.16f, 0.09f, 0.06f});
        static const MeshData kLens = Primitives::Ellipsoid({0.055f, 0.035f, 0.035f}, 12, 8);
        const MeshHandle housing = meshes.Upload(kHousing, "airlock_lamp_housing");
        const MeshHandle lens = meshes.Upload(kLens, "airlock_lamp_lens");
        for (int s = 0; s < 2; ++s)
        {
            const glm::vec3 out = normal * (s == 0 ? 1.0f : -1.0f);
            const RayHit wall = physics.RayCastStatic(middle + out * 0.9f, -out, 0.9f);
            const glm::vec3 face = wall ? wall.position : middle + out * 0.15f;
            Transform transform;
            transform.position = face + out * 0.03f;
            transform.rotation = glm::angleAxis(std::atan2(out.x, out.z), glm::vec3(0.0f, 1.0f, 0.0f));
            door.lampHousings[s] = scene.CreateMeshEntity("airlock_lamp_housing", transform, housing, kHatchMaterial);
            transform.position = face + out * 0.065f;
            Material glow = Material::Diffuse(glm::vec3(0.9f), 0.3f);
            door.lamps[s] = scene.CreateMeshEntity("airlock_lamp", transform, lens, glow);
            door.lampAt[s] = transform.position;
            MarkLevel(scene, door.lamps[s]);
            MarkLevel(scene, door.lampHousings[s]);
        }
    }
    for (const PlacedThing& locker : placements.lockers)
    {
        AddLocker(scene, meshes, physics, interactions, locker.position, locker.yaw);
    }
    for (const PlacedThing& crate : placements.ammoCrates)
    {
        AddAmmoCrate(scene, physics, interactions, crate.position, crate.yaw);
    }
    for (const PlacedItem& item : placements.items)
    {
        const ItemId id = items.IdOf(item.key);
        const ItemDefinition* definition = items.Get(id);
        if (definition == nullptr)
        {
            continue;
        }
        SpawnPickup(scene, meshes, physics, interactions, items, id, item.count,
                    item.position + glm::vec3(0.0f, definition->size.y * 0.5f + 0.005f, 0.0f), glm::vec3(0.0f));
    }
    PRED_LOG_INFO(Gameplay, "Facility objects: {} doors, {} lockers, {} ammunition crates, {} items", placements.doors.size(),
                  placements.lockers.size(), placements.ammoCrates.size(), placements.items.size());
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
            {kInteractionBayX - 1.55f - 0.565f, 0.0f, doorFrameZ}, 0.0f, glm::radians(-100.0f),
            {1.13f, 2.08f, 0.09f}, "door_a", true);
    AddDoor(scene, meshes, physics, interactions,
            {kInteractionBayX + 1.55f + 0.565f, 0.0f, doorFrameZ}, glm::radians(180.0f),
            glm::radians(80.0f), {1.13f, 2.08f, 0.09f}, "door_b", true);

    // --- The creature lab's doors: the corridor's own, swinging in, and one into each room off it -- the
    // store's locked, for a creature to break down.
    {
        using namespace LabSpec;
        AddDoor(scene, meshes, physics, interactions, {kCorridorDoorX - kDoorWidth * 0.5f + 0.01f, 0.0f, kCorridorSouth + 0.15f},
                0.0f, glm::radians(100.0f), {kDoorWidth - 0.02f, LabSpec::kDoorHeight - 0.02f, 0.09f}, "lab_door_corridor", true);
        AddDoor(scene, meshes, physics, interactions, {kCorridorEast + 0.15f, 0.0f, kLockerDoorZ + kDoorWidth * 0.5f - 0.01f},
                glm::radians(90.0f), glm::radians(-10.0f), {kDoorWidth - 0.02f, LabSpec::kDoorHeight - 0.02f, 0.09f}, "lab_door_lockers", true);
        const int store = AddDoor(scene, meshes, physics, interactions,
                                  {kCorridorEast + 0.15f, 0.0f, kStoreDoorZ + kDoorWidth * 0.5f - 0.01f}, glm::radians(90.0f),
                                  glm::radians(-10.0f), {kDoorWidth - 0.02f, LabSpec::kDoorHeight - 0.02f, 0.09f}, "lab_door_store", true);
        m_doors[static_cast<size_t>(store)].locked = true;
    }

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
                                         {kInteractionBayX + 1.3f, 0.0f, kBayZ + 0.9f},
                                         LabSpec::kLockers[0], LabSpec::kLockers[1], LabSpec::kLockers[2],
                                         LabSpec::kLockers[3]};
    m_lockerMesh = lockerMesh;
    for (const glm::vec3& position : lockerPositions)
    {
        AddLocker(scene, meshes, physics, interactions, position, 0.0f);
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
                                            {kEquipmentBayX + 1.5f, 0.0f, kBayZ + 0.3f}, LabSpec::kAmmo};

        m_crateMesh = crateMesh;
        m_lidMesh = lidMesh;
        for (const glm::vec3& position : cratePositions)
        {
            AddAmmoCrate(scene, physics, interactions, position, 0.0f);
        }
    }

    // --- Loose items, laid out along both equipment benches in one row each.
    //
    // Every item the game knows about appears on both -- the test map's and the creature lab's -- in
    // the order items.json defines them, as many of each as its bench_count says. Listing them here by
    // hand meant an item added to items.json was on neither, and the lab's bench had three of seven.
    const auto layOut = [&](float centreX, float top, float z)
    {
        int laid = 0;
        for (const ItemDefinition& definition : items.All())
        {
            laid += definition.id != kInvalidItem && definition.benchCount > 0 ? 1 : 0;
        }
        // (Closer together as there are more of them, to stay on the bench: 4.8 m of it at most.)
        const float kSpacing = std::min(0.62f, 4.8f / static_cast<float>(std::max(laid - 1, 1)));
        float x = centreX - kSpacing * static_cast<float>(std::max(laid - 1, 0)) * 0.5f;
        for (const ItemDefinition& definition : items.All())
        {
            if (definition.id == kInvalidItem || definition.benchCount <= 0)
            {
                continue;
            }
            // Standing on the bench, not dropped onto it. A pickup's position is the centre of its box,
            // so a fixed clearance buried the tall ones in the surface, and the separation impulse
            // flipped them onto their ends.
            SpawnPickup(scene, meshes, physics, interactions, items, definition.id, definition.benchCount,
                        {x, top + definition.size.y * 0.5f + 0.005f, z}, glm::vec3(0.0f));
            x += kSpacing;
        }
    };
    layOut(kEquipmentBayX, kBenchTop, kBayZ + 1.3f);
    layOut(LabSpec::kBench.x, LabSpec::kBenchTop, LabSpec::kBench.z);

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
    // Dropped roughly facing the way it was thrown, and turned a different amount each time, rather
    // than all of them lying in the same direction like stock on a shelf.
    //
    // The seed used to be the pickup's index and the item alone. Indices are reused -- a slot that
    // has been picked up is the first one the next drop takes -- so picking something up and
    // dropping it again put it in the same slot with the same seed, and it landed exactly the same
    // way every time. A count of every drop goes in as well now. Two machines need not agree on it:
    // the host sends every loose object's position and rotation to everybody, so whichever way a
    // client guesses, the host's answer replaces it within a frame.
    {
        ++m_dropSerial;
        const uint32_t seed = static_cast<uint32_t>(atIndex + 1) * 2654435761u +
                              static_cast<uint32_t>(item) * 40503u + m_dropSerial * 0x85EBCA6Bu;
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
        // The heading it left the hand on, when it was thrown rather than set down, with up to a
        // quarter turn either way on top: the direction a dropped thing points says which way it
        // was going, and never pointing exactly that way says nobody placed it.
        const glm::vec2 flat{velocity.x, velocity.z};
        const float heading = glm::length(flat) > 0.1f ? std::atan2(flat.x, flat.y)
                                                        : unit(3) * glm::two_pi<float>();
        const float spin = heading + (unit(0) - 0.5f) * glm::pi<float>();
        transform.rotation =
            glm::angleAxis(spin, glm::vec3(0.0f, 1.0f, 0.0f)) *
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
    // An airlock's warning lights, over both its doors: red and pulsing while either cycles, green over a door that is open, a low
    // amber over one shut and ready.
    m_clock += dt;
    for (Door& door : m_doors)
    {
        if (door.partner < 0 || static_cast<size_t>(door.partner) >= m_doors.size())
        {
            continue;
        }
        const Door& other = m_doors[static_cast<size_t>(door.partner)];
        const bool cycling = door.openIn >= 0.0f || other.openIn >= 0.0f;
        const glm::vec3 colour = cycling ? glm::vec3(3.2f, 0.18f, 0.08f) * (0.25f + 0.75f * (0.5f + 0.5f * std::sin(m_clock * 9.0f)))
                                 : door.angle != door.closedYaw ? glm::vec3(0.2f, 2.4f, 0.5f)
                                                                : glm::vec3(0.9f, 0.5f, 0.06f);
        for (const Entity lamp : door.lamps)
        {
            if (MeshRenderer* renderer = scene.GetMeshRenderer(lamp))
            {
                renderer->material.emissive = colour;
            }
        }
    }
    for (Door& door : m_doors)
    {
        // Waiting to open, an airlock's door: once its other is shut and the air has cycled.
        if (door.openIn >= 0.0f)
        {
            const Door* other = door.partner >= 0 && static_cast<size_t>(door.partner) < m_doors.size() ? &m_doors[static_cast<size_t>(door.partner)] : nullptr;
            door.openIn = other != nullptr && other->angle != other->closedYaw ? std::max(door.openIn, kAirlockCycle * 0.5f) : door.openIn - dt;
            if (door.openIn < 0.0f)
            {
                door.target = door.openYaw;
            }
        }
        if (door.IsMoving())
        {
            // (A heavy door -- an airlock's, a blast door -- slow, and easing into where it stops: its weight.)
            const float ease = door.speed < 3.0f ? std::clamp(std::abs(door.target - door.angle) / 0.4f, 0.2f, 1.0f) : 1.0f;
            const float step = door.speed * ease * dt;
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
        // (In the lamps' shadows as it swings too: they are drawn before the picture now, as it is this frame -- Renderer's view order.)

        // Kinematic movement rather than teleporting, so the panel shoves what is in its way. It is
        // driven every tick, not only while the angle is changing: MoveKinematic works by setting
        // the velocity needed to reach the target within dt, and that velocity persists until it is
        // set again. Skipping still doors left every one of them carrying the velocity from its
        // last moving tick, so the collider slowly sailed away from the panel it belonged to.
        // Driving a door that is already where it should be asks for zero velocity, which both
        // stops it and pulls back anything that has drifted.
        physics.MoveKinematic(door.body, DoorBodyTransform(door), dt);
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
            // Hinged along the back edge, so it swings up and back rather than turning about its own
            // middle and sinking half of itself into the crate.
            //
            // The panel turns about the hinge, so its middle travels round it: up by the sine and back
            // by the cosine. The two used to disagree in sign -- it turned one way and slid the other --
            // which is what made the lid look broken and left it standing where it was.
            constexpr float kHalfDepth = 0.23f;
            const float lift = std::sin(crate.lidAngle) * kHalfDepth;
            const float back = (1.0f - std::cos(crate.lidAngle)) * kHalfDepth;
            const glm::quat turn = glm::angleAxis(crate.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            transform->rotation = turn * glm::angleAxis(-crate.lidAngle, glm::vec3(1.0f, 0.0f, 0.0f));
            transform->position = crate.lidRest + turn * glm::vec3(0.0f, lift, -back);
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
            interactable->verb = door.locked ? (m_haveKeycard ? "Unlock" : "Locked") : (door.IsOpen() ? "Close" : "Open");
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
    // An airlock's door: never open with the other -- that one shut first, and this one opening once it is and the air has cycled
    // (Update), as everybody's copy of it does the same.
    door->openIn = -1.0f;
    if (open && door->partner >= 0)
    {
        if (Door* other = GetDoor(door->partner))
        {
            other->target = other->closedYaw;
            other->openIn = -1.0f;
            door->target = door->closedYaw;
            door->openIn = kAirlockCycle;
            door->locked = false;
            return;
        }
    }
    door->target = open ? door->openYaw : door->closedYaw;
    // A door that is open is not locked, whoever opened it. The host unlocks one a creature breaks down;
    // everybody else only hears that it opened, and a client promoted to host later must not find it
    // still locked in its copy of the world.
    if (open)
    {
        door->locked = false;
    }
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

void WorldObjects::RemoveDoorsFrom(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions, size_t first)
{
    for (size_t i = first; i < m_doors.size(); ++i)
    {
        Door& door = m_doors[i];
        interactions.Unregister(door.entity);
        for (int s = 0; s < 2; ++s)
        {
            scene.Destroy(door.lamps[s]);
            scene.Destroy(door.lampHousings[s]);
        }
        physics.DestroyBody(door.body);
        scene.Destroy(door.entity);
    }
    m_doors.resize(std::min(first, m_doors.size()));
}

void WorldObjects::Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions)
{
    RemoveDoorsFrom(scene, physics, interactions, 0);
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
    // With their colliders: these used to stay behind, so every new game stacked another set of lockers
    // and crates, invisible, on top of the last.
    for (HidingSpot& spot : m_hidingSpots)
    {
        interactions.Unregister(spot.entity);
        scene.Destroy(spot.entity);
        for (const BodyHandle body : spot.bodies)
        {
            physics.DestroyBody(body);
        }
    }
    for (AmmoCrate& crate : m_ammoCrates)
    {
        interactions.Unregister(crate.entity);
        physics.DestroyBody(crate.body);
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
