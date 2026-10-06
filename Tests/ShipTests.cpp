#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/Outpost.h"
#include "Game/World/ShipMap.h"
#include "Game/World/Vehicles.h"
#include "Game/World/WorldObjects.h"

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <filesystem>

using namespace pred;

namespace
{

struct Aboard
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;
    InteractionSystem interactions;
    ItemDatabase items;
    ShipMap ship;
    WorldObjects world;

    Aboard()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 2;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        REQUIRE(items.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "items.json"));
        ship.Build(scene, meshes, physics, nullptr);
    }
};

} // namespace

TEST_CASE("The ship, and the lockers and crates put aboard it, are inside nothing", "[ship]")
{
    Aboard aboard;
    REQUIRE(aboard.ship.Built());
    aboard.world.AddFacility(aboard.scene, aboard.meshes, aboard.physics, aboard.interactions, aboard.items, aboard.ship.Placements());
    const std::vector<PhysicsWorld::StaticOverlap> overlaps = aboard.physics.FindStaticOverlaps(0.01f);
    CHECK(overlaps.empty());
    CHECK(aboard.ship.Placements().lockers.size() == 3);
    CHECK(aboard.ship.Placements().ammoCrates.size() == 2);
    // Nothing is laid out on the benches: the kit is drawn at the loadout locker.
    CHECK(aboard.ship.Placements().items.empty());
}

TEST_CASE("Everybody arrives aboard standing on the ops room's floor", "[ship]")
{
    Aboard aboard;
    for (uint8_t player = 0; player < 8; ++player)
    {
        INFO("player " << static_cast<int>(player));
        const glm::vec3 spawn = aboard.ship.Spawn(player);
        const RayHit floor = aboard.physics.RayCastStatic(spawn + glm::vec3(0.0f, 1.0f, 0.0f), {0.0f, -1.0f, 0.0f}, 3.0f);
        REQUIRE(floor.hit);
        CHECK(std::abs(floor.position.y - (ShipSpec::kOrigin.y + ShipSpec::kDeck)) < 0.05f);
        // Nothing overhead but the ceiling, well above a head.
        const RayHit above = aboard.physics.RayCastStatic(spawn + glm::vec3(0.0f, 0.2f, 0.0f), {0.0f, 1.0f, 0.0f}, 10.0f);
        REQUIRE(above.hit);
        CHECK(above.distance > 2.3f);
        CHECK(aboard.ship.Contains(spawn));
    }
    // And the ship is well away from everywhere else.
    CHECK_FALSE(aboard.ship.Contains({0.0f, 0.0f, 0.0f}));
    CHECK_FALSE(aboard.ship.Contains({250.0f, 0.0f, 40.0f}));
}

TEST_CASE("The loadout locker is the first thing seen coming into the gear room, its screen on its face", "[ship]")
{
    Aboard aboard;
    const CinePose locker = aboard.ship.LoadoutLocker();
    // In the gear room, to starboard of the corridor, at the height of a face.
    const glm::vec3 local = locker.position - ShipSpec::kOrigin;
    CHECK(local.x > 1.0f);
    CHECK(local.z > -4.0f);
    CHECK(local.z < 3.5f);
    CHECK(local.y > 1.2f);
    CHECK(local.y < 1.8f);
    // Facing into the room, towards the door.
    const glm::vec3 facing = locker.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    CHECK(facing.x < -0.99f);
    // And nothing between the doorway and it: a look straight in from the corridor lands on its face.
    const glm::vec3 doorway = ShipSpec::kOrigin + glm::vec3(0.5f, local.y, local.z);
    const RayHit look = aboard.physics.RayCastStatic(doorway, {1.0f, 0.0f, 0.0f}, 20.0f);
    REQUIRE(look.hit);
    CHECK(std::abs(look.position.x - locker.position.x) < 0.05f);
}

TEST_CASE("The navigation console stands in the ops room, the screens beyond it, facing where people come in", "[ship]")
{
    Aboard aboard;
    const CinePose console = aboard.ship.BriefingConsole();
    const glm::vec3 local = console.position - ShipSpec::kOrigin;
    // Aft of the screens, so they are seen over it.
    float width = 0.0f;
    CHECK(aboard.ship.BriefingScreen(0, width).position.z < console.position.z);
    CHECK(width > 1.5f);
    CHECK(std::abs(local.x) < 1.0f);
    // Its front (-z of its own) looks into the room, and there is floor to stand on there with nothing in the way.
    const glm::vec3 front = console.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    CHECK(front.z > 0.99f);
    const glm::vec3 standing = console.position + front * 1.2f + glm::vec3(0.0f, 1.0f, 0.0f);
    const RayHit floor = aboard.physics.RayCastStatic(standing, {0.0f, -1.0f, 0.0f}, 2.0f);
    REQUIRE(floor.hit);
    CHECK(std::abs(floor.position.y - console.position.y) < 0.05f);
    const RayHit up = aboard.physics.RayCastStatic(standing, {0.0f, 1.0f, 0.0f}, 3.0f);
    CHECK((!up.hit || up.distance > 1.5f));
}

TEST_CASE("The shuttle stands on the closed bay doors, which swing down out of its way", "[ship]")
{
    Aboard aboard;
    // Standing in the hangar, on its floor.
    const glm::vec3 home = aboard.ship.Shuttle().Home().position;
    CHECK(aboard.ship.InHangar(home + glm::vec3(0.0f, 1.0f, 0.0f)));
    const RayHit under = aboard.physics.RayCastStatic(home + glm::vec3(1.3f, 0.2f, -2.0f), {0.0f, -1.0f, 0.0f}, 2.0f);
    REQUIRE(under.hit);
    CHECK(std::abs(under.position.y - home.y) < 0.05f);

    // Open, each leaf hangs down under the floor: its inner edge metres below where it lay.
    const ModelAsset doors = Vehicles::BayDoorsModel();
    const AnimationClip* opening = doors.FindClip("doors_opening");
    REQUIRE(opening != nullptr);
    for (const char* leaf : {"door_left", "door_right"})
    {
        INFO(leaf);
        const auto part = std::find_if(doors.parts.begin(), doors.parts.end(), [&](const ModelPart& p) { return p.name == leaf; });
        REQUIRE(part != doors.parts.end());
        const float inner = part->position.x < 0.0f ? part->size.x * 0.5f : -part->size.x * 0.5f;
        const glm::vec4 closed = doors.PartMatrixAt(*part, nullptr, 0.0f) * glm::vec4(inner, 0.0f, 0.0f, 1.0f);
        const glm::vec4 open = doors.PartMatrixAt(*part, opening, opening->duration) * glm::vec4(inner, 0.0f, 0.0f, 1.0f);
        CHECK(std::abs(closed.y + 0.15f) < 0.05f);
        CHECK(open.y < -3.5f);
        // And out of the middle, where the shuttle falls through.
        CHECK(std::abs(open.x) > 3.5f);
    }
}

TEST_CASE("No two parts of a vehicle share a surface facing the same way, which would flicker", "[ship][vehicle]")
{
    // Two faces in one plane, facing the same way and overlapping, are drawn in whichever order the depth test happens to
    // pick from pixel to pixel: the flickering seen on the back of the ship. Faces touching back to back are fine.
    ShipHullLook upgraded;
    upgraded.drive = 4;
    upgraded.sensors = 4;
    for (const ModelAsset& model : {ShipMap::HullModel(ShipHullLook{}), ShipMap::HullModel(upgraded), ShipMap::HullModel(upgraded, true), Vehicles::ShuttleModel(),
                                    Vehicles::BayDoorsModel()})
    {
        INFO(model.name);
        for (size_t i = 0; i < model.parts.size(); ++i)
        {
            const ModelPart& a = model.parts[i];
            if (a.shape != PartShape::Box || a.rotation != glm::vec3(0.0f) || !a.parent.empty())
            {
                continue;
            }
            for (size_t j = i + 1; j < model.parts.size(); ++j)
            {
                const ModelPart& b = model.parts[j];
                if (b.shape != PartShape::Box || b.rotation != glm::vec3(0.0f) || !b.parent.empty())
                {
                    continue;
                }
                for (int axis = 0; axis < 3; ++axis)
                {
                    const int u = (axis + 1) % 3;
                    const int v = (axis + 2) % 3;
                    const float overlapU = std::min(a.position[u] + a.size[u] * 0.5f, b.position[u] + b.size[u] * 0.5f) -
                                           std::max(a.position[u] - a.size[u] * 0.5f, b.position[u] - b.size[u] * 0.5f);
                    const float overlapV = std::min(a.position[v] + a.size[v] * 0.5f, b.position[v] + b.size[v] * 0.5f) -
                                           std::max(a.position[v] - a.size[v] * 0.5f, b.position[v] - b.size[v] * 0.5f);
                    if (overlapU < 0.01f || overlapV < 0.01f)
                    {
                        continue;
                    }
                    for (const float side : {-0.5f, 0.5f})
                    {
                        const float faceA = a.position[axis] + a.size[axis] * side;
                        const float faceB = b.position[axis] + b.size[axis] * side;
                        if (std::abs(faceA - faceB) > 0.001f)
                        {
                            continue;
                        }
                        // Buried: just outside the shared face is inside some other part, so neither is ever seen.
                        glm::vec3 outside;
                        outside[axis] = faceA + (side > 0.0f ? 0.002f : -0.002f);
                        outside[u] = (std::max(a.position[u] - a.size[u] * 0.5f, b.position[u] - b.size[u] * 0.5f) +
                                      std::min(a.position[u] + a.size[u] * 0.5f, b.position[u] + b.size[u] * 0.5f)) * 0.5f;
                        outside[v] = (std::max(a.position[v] - a.size[v] * 0.5f, b.position[v] - b.size[v] * 0.5f) +
                                      std::min(a.position[v] + a.size[v] * 0.5f, b.position[v] + b.size[v] * 0.5f)) * 0.5f;
                        const bool buried = std::any_of(model.parts.begin(), model.parts.end(), [&](const ModelPart& c)
                        {
                            return c.shape == PartShape::Box && c.rotation == glm::vec3(0.0f) && c.parent.empty() &&
                                   glm::all(glm::lessThan(glm::abs(outside - c.position), c.size * 0.5f));
                        });
                        INFO(a.name << " and " << b.name << " on axis " << axis);
                        CHECK(buried);
                    }
                }
            }
        }
    }
}

namespace
{

// Every pair of parts, across the models given, with a face within a few millimetres of one of the other's, facing the same way
// and overlapping, that nothing else covers: what flickers. Turned and non-box parts are left out (none of them lies on another).
std::vector<std::string> SharedFaces(const std::vector<const ModelAsset*>& models)
{
    std::vector<const ModelPart*> parts;
    for (const ModelAsset* model : models)
    {
        for (const ModelPart& part : model->parts)
        {
            if (part.shape == PartShape::Box && part.rotation == glm::vec3(0.0f) && part.parent.empty())
            {
                parts.push_back(&part);
            }
        }
    }
    std::vector<std::string> found;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const ModelPart& a = *parts[i];
        for (size_t j = i + 1; j < parts.size(); ++j)
        {
            const ModelPart& b = *parts[j];
            for (int axis = 0; axis < 3; ++axis)
            {
                const int u = (axis + 1) % 3;
                const int v = (axis + 2) % 3;
                const float lowU = std::max(a.position[u] - a.size[u] * 0.5f, b.position[u] - b.size[u] * 0.5f);
                const float highU = std::min(a.position[u] + a.size[u] * 0.5f, b.position[u] + b.size[u] * 0.5f);
                const float lowV = std::max(a.position[v] - a.size[v] * 0.5f, b.position[v] - b.size[v] * 0.5f);
                const float highV = std::min(a.position[v] + a.size[v] * 0.5f, b.position[v] + b.size[v] * 0.5f);
                if (highU - lowU < 0.01f || highV - lowV < 0.01f)
                {
                    continue;
                }
                for (const float side : {-0.5f, 0.5f})
                {
                    const float faceA = a.position[axis] + a.size[axis] * side;
                    const float faceB = b.position[axis] + b.size[axis] * side;
                    if (std::abs(faceA - faceB) > 0.005f)
                    {
                        continue;
                    }
                    // Buried: just outside the shared face is inside some other part, so neither is ever seen. Looked at in a
                    // few places over the overlap, not only its middle.
                    bool seen = false;
                    for (const glm::vec2 at : {glm::vec2{0.5f, 0.5f}, glm::vec2{0.1f, 0.1f}, glm::vec2{0.9f, 0.1f}, glm::vec2{0.1f, 0.9f}, glm::vec2{0.9f, 0.9f}})
                    {
                        glm::vec3 outside;
                        outside[axis] = std::max(faceA, faceB) * (side > 0.0f ? 1.0f : 0.0f) + std::min(faceA, faceB) * (side > 0.0f ? 0.0f : 1.0f) +
                                        (side > 0.0f ? 0.004f : -0.004f);
                        outside[u] = lowU + (highU - lowU) * at.x;
                        outside[v] = lowV + (highV - lowV) * at.y;
                        const bool buried = std::any_of(parts.begin(), parts.end(), [&](const ModelPart* c)
                                                        { return glm::all(glm::lessThanEqual(glm::abs(outside - c->position), c->size * 0.5f + glm::vec3(1.0e-4f))); });
                        seen = seen || !buried;
                    }
                    if (seen)
                    {
                        found.push_back(a.name + " and " + b.name + " on axis " + std::to_string(axis));
                    }
                }
            }
        }
    }
    return found;
}

} // namespace

TEST_CASE("Kestrel Station and the ship standing in it share no surface, which would flicker", "[ship][kestrel]")
{
    const ModelAsset hull = ShipMap::HullModel(ShipHullLook{});
    const ModelAsset solid = KestrelStation::Solid({0.4f, 0.4f, 0.4f}, {0.3f, 0.3f, 0.3f});
    const ModelAsset dressing = KestrelStation::Dressing({0.4f, 0.4f, 0.4f});
    // And the ship's own stair, out at its door.
    const ModelAsset landing = ShipMap::StairLandingModel();
    const std::vector<std::string> found = SharedFaces({&hull, &solid, &dressing, &landing});
    for (const std::string& pair : found)
    {
        UNSCOPED_INFO(pair);
    }
    CHECK(found.empty());
}

namespace
{

// Whether a point is inside any part of a model that is not turned (the hills and dishes are, and are nowhere near), grown by
// `margin`.
const ModelPart* PartAt(const ModelAsset& model, const glm::vec3& point, float margin = 0.0f)
{
    for (const ModelPart& part : model.parts)
    {
        if (part.rotation == glm::vec3(0.0f) && part.parent.empty() && glm::all(glm::lessThanEqual(glm::abs(point - part.position), part.size * 0.5f + margin)))
        {
            return &part;
        }
    }
    return nullptr;
}

uint32_t OutpostSeed(int i)
{
    return (static_cast<uint32_t>(i) * 2654435761u + 0x51u) | 1u;
}

} // namespace

TEST_CASE("Every outpost but Kestrel is its own, planned the same from the same seed", "[ship][outpost]")
{
    const glm::vec3 ground{0.4f};
    const glm::vec3 rock{0.3f};
    const Outpost::Layout a = Outpost::Generate(OutpostSeed(3), ground, rock);
    const Outpost::Layout b = Outpost::Generate(OutpostSeed(3), ground, rock);
    REQUIRE(a.solid.parts.size() == b.solid.parts.size());
    REQUIRE(a.dressing.parts.size() == b.dressing.parts.size());
    for (size_t i = 0; i < a.solid.parts.size(); ++i)
    {
        REQUIRE(a.solid.parts[i].position == b.solid.parts[i].position);
        REQUIRE(a.solid.parts[i].size == b.solid.parts[i].size);
    }
    REQUIRE(a.lamps.size() == b.lamps.size());

    // And they differ from one another.
    std::vector<size_t> counts;
    for (int i = 0; i < 30; ++i)
    {
        const size_t count = Outpost::Generate(OutpostSeed(i), ground, rock).solid.parts.size();
        if (std::find(counts.begin(), counts.end(), count) == counts.end())
        {
            counts.push_back(count);
        }
    }
    CHECK(counts.size() >= 20);
}

TEST_CASE("An outpost has its name to put up, lamps enough to light it, and nothing where people and cameras stand", "[ship][outpost]")
{
    // Where the cinematics of setting down and taking off stand their cameras (ship_land, ship_takeoff), in the ship's frame.
    const glm::vec3 cameras[] = {{-40.0f, 3.0f, -22.0f}, {-38.0f, 3.4f, -18.0f}, {-19.0f, 1.6f, -18.0f}, {-18.5f, 1.8f, -14.0f},
                                 {-42.0f, 2.2f, -78.0f}, {-44.0f, 2.0f, -74.0f}, {18.0f, 10.0f, 42.0f}, {16.0f, 16.0f, 30.0f}};
    const float s = KestrelStation::kSlabTop;
    const float cross = (KestrelStation::kCrossFrom + KestrelStation::kCrossTo) * 0.5f;
    CHECK(KestrelStation::Lamps().size() <= Outpost::kMaxLamps);
    for (int i = 0; i < 40; ++i)
    {
        const Outpost::Layout layout = Outpost::Generate(OutpostSeed(i), glm::vec3(0.4f), glm::vec3(0.3f));
        INFO("outpost " << i);
        CHECK(layout.lamps.size() >= 12);
        CHECK(layout.lamps.size() <= Outpost::kMaxLamps);
        CHECK(std::count_if(layout.signs.begin(), layout.signs.end(), [](const KestrelStation::Sign& sign) { return sign.id == "outpost_name"; }) == 1);
        for (size_t a = 0; a < layout.signs.size(); ++a)
        {
            for (size_t b = a + 1; b < layout.signs.size(); ++b)
            {
                CHECK(layout.signs[a].id != layout.signs[b].id);
            }
        }
        for (const glm::vec3& camera : cameras)
        {
            const ModelPart* solid = PartAt(layout.solid, camera, 1.0f);
            const ModelPart* dressing = PartAt(layout.dressing, camera, 1.0f);
            INFO("camera at " << camera.x << " " << camera.y << " " << camera.z << ": " << (solid ? solid->name : dressing ? dressing->name : ""));
            CHECK(solid == nullptr);
            CHECK(dressing == nullptr);
        }
        // Where the crew stand when they come off the ship, and the walk from the stair across the street to the operations
        // block's door: clear, above the kerbs.
        for (int player = 0; player < 8; ++player)
        {
            const glm::vec3 at = KestrelStation::Spawn(player) - ShipSpec::kOrigin;
            for (float y = 0.3f; y < 1.9f; y += 0.4f)
            {
                const ModelPart* part = PartAt(layout.solid, {at.x, s + y, at.z});
                INFO("player " << player << " in " << (part ? part->name : ""));
                CHECK(part == nullptr);
            }
        }
        for (float x = -9.6f; x > -48.8f; x -= 0.5f)
        {
            const ModelPart* part = PartAt(layout.solid, {x, s + 1.0f, cross});
            INFO("the walk at " << x << " blocked by " << (part ? part->name : ""));
            CHECK(part == nullptr);
        }
        // The way the ship comes in, low over the pad's front along its middle: nothing standing in it.
        for (const ModelAsset* model : {&layout.solid, &layout.dressing})
        {
            for (const ModelPart& part : model->parts)
            {
                if (std::abs(part.position.x) < 22.0f && part.position.z + part.size.z * 0.5f < -40.0f && part.rotation == glm::vec3(0.0f))
                {
                    INFO(part.name);
                    CHECK(part.position.y + part.size.y * 0.5f < s + 4.0f);
                }
            }
        }
    }
}

TEST_CASE("Outposts and the ship standing in them share no surface, which would flicker", "[ship][outpost]")
{
    const ModelAsset hull = ShipMap::HullModel(ShipHullLook{});
    for (int i = 0; i < 10; ++i)
    {
        const Outpost::Layout layout = Outpost::Generate(OutpostSeed(i), glm::vec3(0.4f), glm::vec3(0.3f));
        const std::vector<std::string> found = SharedFaces({&hull, &layout.solid, &layout.dressing});
        INFO("outpost " << i);
        for (const std::string& pair : found)
        {
            UNSCOPED_INFO(pair);
        }
        CHECK(found.empty());
    }
}

TEST_CASE("The cinematics of setting down and taking off see the ship, at Kestrel and at every outpost", "[ship][outpost][kestrel]")
{
    // Each camera's places and the middle of the ship as it looks at it, in the ship's frame: setting down it is watched coming in
    // and touching down; taking off, lifting.
    struct Sight
    {
        glm::vec3 eye;
        glm::vec3 ship;
    };
    const Sight sights[] = {{{-40.0f, 3.0f, -22.0f}, {0.0f, 16.0f, 0.0f}}, {{-38.0f, 3.4f, -18.0f}, {0.0f, 16.0f, 0.0f}},
                            {{-19.0f, 1.6f, -18.0f}, {0.0f, 1.0f, 0.0f}},  {{-18.5f, 1.8f, -14.0f}, {0.0f, 1.0f, 0.0f}},
                            {{-42.0f, 2.2f, -78.0f}, {0.0f, 4.0f, 0.0f}},  {{-44.0f, 2.0f, -74.0f}, {0.0f, 4.0f, 0.0f}},
                            {{18.0f, 10.0f, 42.0f}, {0.0f, 4.0f, -6.0f}}, {{16.0f, 16.0f, 30.0f}, {0.0f, 8.0f, -6.0f}}};
    const auto check = [&](const ModelAsset& solid, const ModelAsset& dressing, const std::string& where)
    {
        for (const Sight& sight : sights)
        {
            // Up to the ship's side: the last stretch is the ship itself.
            for (float t = 0.0f; t < 0.75f; t += 0.01f)
            {
                const glm::vec3 at = sight.eye + (sight.ship - sight.eye) * t;
                const ModelPart* part = PartAt(solid, at);
                if (part == nullptr)
                {
                    part = PartAt(dressing, at);
                }
                if (part != nullptr && part->name.rfind("fx_", 0) == 0)
                {
                    part = nullptr;
                }
                INFO(where << ": from " << sight.eye.x << " " << sight.eye.y << " " << sight.eye.z << " blocked by " << (part ? part->name : ""));
                CHECK(part == nullptr);
                if (part != nullptr)
                {
                    break;
                }
            }
        }
    };
    check(KestrelStation::Solid(glm::vec3(0.4f), glm::vec3(0.3f)), KestrelStation::Dressing(glm::vec3(0.4f)), "Kestrel");
    for (int i = 0; i < 20; ++i)
    {
        const Outpost::Layout layout = Outpost::Generate(OutpostSeed(i), glm::vec3(0.4f), glm::vec3(0.3f));
        check(layout.solid, layout.dressing, "outpost " + std::to_string(i));
    }
}

TEST_CASE("An outpost looks as its owner has it: CIRRA's marked and walled, one nobody keeps dark", "[ship][outpost]")
{
    OutpostStyle cirra;
    cirra.mark = true;
    cirra.standard = true;
    cirra.operations = "OPERATIONS HALL";
    OutpostStyle abandoned;
    abandoned.abandoned = true;
    abandoned.lit = 0.12f;
    const ModelAsset hull = ShipMap::HullModel(ShipHullLook{});
    for (int i = 0; i < 8; ++i)
    {
        INFO("outpost " << i);
        const Outpost::Layout plain = Outpost::Generate(OutpostSeed(i), glm::vec3(0.4f), glm::vec3(0.3f));
        const Outpost::Layout marked = Outpost::Generate(OutpostSeed(i), glm::vec3(0.4f), glm::vec3(0.3f), cirra);
        const Outpost::Layout dark = Outpost::Generate(OutpostSeed(i), glm::vec3(0.4f), glm::vec3(0.3f), abandoned);
        const auto has = [](const Outpost::Layout& layout, const std::string& id)
        { return std::any_of(layout.signs.begin(), layout.signs.end(), [&](const KestrelStation::Sign& sign) { return sign.id == id; }); };
        // CIRRA's: its mark over the operations block's door and on the wall behind the pad, which it always has, and a fence.
        CHECK(has(marked, "logo_ops"));
        CHECK(has(marked, "bay_logo"));
        CHECK_FALSE(has(plain, "logo_ops"));
        CHECK(std::any_of(marked.solid.parts.begin(), marked.solid.parts.end(), [](const ModelPart& part) { return part.name.rfind("fence", 0) == 0; }));
        CHECK(std::any_of(marked.signs.begin(), marked.signs.end(), [](const KestrelStation::Sign& sign) { return !sign.lines.empty() && sign.lines[0] == "OPERATIONS HALL"; }));
        // Nobody there: few of its lamps working, its signs dark.
        CHECK(dark.lamps.size() * 3 < plain.lamps.size());
        CHECK(std::all_of(dark.signs.begin(), dark.signs.end(), [](const KestrelStation::Sign& sign) { return sign.dark; }));
        // And both still share no flickering surface with the ship, and keep the crew's way clear.
        for (const Outpost::Layout* layout : {&marked, &dark})
        {
            CHECK(SharedFaces({&hull, &layout->solid, &layout->dressing}).empty());
            const glm::vec3 at = KestrelStation::Spawn(0) - ShipSpec::kOrigin;
            CHECK(PartAt(layout->solid, {at.x, KestrelStation::kSlabTop + 1.0f, at.z}) == nullptr);
        }
    }
}
