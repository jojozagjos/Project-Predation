#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/Inventory.h"
#include "Game/Items/ItemDatabase.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace pred;

namespace
{

ItemDatabase MakeDatabase()
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "predation_items_test.json";
    {
        std::ofstream stream(file);
        stream << R"({"items":[
            {"key":"medkit","name":"Medical Kit","max_stack":2},
            {"key":"battery","name":"Battery Cell","max_stack":6},
            {"key":"keycard","name":"Access Keycard","max_stack":4}
        ]})";
    }
    ItemDatabase database;
    REQUIRE(database.LoadFromFile(file));
    std::filesystem::remove(file);
    return database;
}

Entity MakeMarker(Scene& scene, const glm::vec3& position)
{
    const Entity entity = scene.Create("marker");
    scene.GetTransform(entity)->position = position;
    return entity;
}

} // namespace

TEST_CASE("Item database maps stable keys to definitions", "[items]")
{
    const ItemDatabase database = MakeDatabase();
    REQUIRE(database.Count() >= 3);

    const ItemDefinition* medkit = database.Find("medkit");
    REQUIRE(medkit != nullptr);
    REQUIRE(medkit->name == "Medical Kit");
    REQUIRE(medkit->maxStack == 2);
    REQUIRE(database.IdOf("medkit") == medkit->id);

    // Unknown keys and the reserved empty id resolve to nothing rather than to slot zero's contents.
    REQUIRE(database.Find("no_such_item") == nullptr);
    REQUIRE(database.IdOf("no_such_item") == kInvalidItem);
    REQUIRE(database.Get(kInvalidItem) == nullptr);
}

TEST_CASE("Inventory fills existing stacks before opening a new slot", "[items][inventory]")
{
    const ItemDatabase database = MakeDatabase();
    const ItemId battery = database.IdOf("battery"); // stacks to 6

    Inventory inventory(3);
    REQUIRE(inventory.Add(database, battery, 4) == 4);
    REQUIRE(inventory.CountOf(battery) == 4);

    // Four more should top up the first stack to six and start a second, not open two new slots.
    REQUIRE(inventory.Add(database, battery, 4) == 4);
    REQUIRE(inventory.At(0).count == 6);
    REQUIRE(inventory.At(1).count == 2);
    REQUIRE(inventory.At(2).IsEmpty());
    REQUIRE(inventory.CountOf(battery) == 8);
}

TEST_CASE("Inventory reports how much it could actually take", "[items][inventory]")
{
    const ItemDatabase database = MakeDatabase();
    const ItemId medkit = database.IdOf("medkit"); // stacks to 2

    Inventory inventory(2);
    REQUIRE(inventory.CanAdd(database, medkit, 4));
    REQUIRE(inventory.Add(database, medkit, 4) == 4);
    REQUIRE(inventory.IsFull(database));

    // A full bag takes nothing, and says so rather than quietly destroying the surplus.
    REQUIRE_FALSE(inventory.CanAdd(database, medkit, 1));
    REQUIRE(inventory.Add(database, medkit, 1) == 0);
    REQUIRE(inventory.CountOf(medkit) == 4);

    // Partial acceptance is reported honestly too.
    inventory.RemoveFromSlot(0, 1);
    REQUIRE(inventory.Add(database, medkit, 3) == 1);
}

TEST_CASE("Inventory removal empties a slot completely", "[items][inventory]")
{
    const ItemDatabase database = MakeDatabase();
    const ItemId keycard = database.IdOf("keycard");

    Inventory inventory(2);
    inventory.Add(database, keycard, 2);
    REQUIRE(inventory.RemoveFromSlot(0, 1) == 1);
    REQUIRE(inventory.At(0).count == 1);
    REQUIRE(inventory.RemoveFromSlot(0, 5) == 1); // only one was left
    REQUIRE(inventory.At(0).IsEmpty());
    REQUIRE(inventory.At(0).item == kInvalidItem);

    REQUIRE(inventory.RemoveFromSlot(-1, 1) == 0);
    REQUIRE(inventory.RemoveFromSlot(99, 1) == 0);
}

TEST_CASE("Inventory slot selection wraps in both directions", "[items][inventory]")
{
    Inventory inventory(4);
    REQUIRE(inventory.SelectedSlot() == 0);

    inventory.SelectNext(1);
    REQUIRE(inventory.SelectedSlot() == 1);
    inventory.SelectNext(-1);
    inventory.SelectNext(-1);
    REQUIRE(inventory.SelectedSlot() == 3); // wrapped past zero

    inventory.SelectSlot(2);
    REQUIRE(inventory.SelectedSlot() == 2);
    inventory.SelectSlot(99); // out of range is ignored, not clamped into a surprise
    REQUIRE(inventory.SelectedSlot() == 2);
}

TEST_CASE("Interaction focus follows the aim, not proximity", "[interaction]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));

    Scene scene;
    InteractionSystem interactions;

    // `near` sits closer to the player but off to the side; `far` is further away but dead ahead.
    // Picking by distance would choose the wrong one, which is what makes proximity-based selection
    // feel broken when several things are within reach.
    const Entity nearSide = MakeMarker(scene, {0.9f, 1.5f, -0.4f});
    const Entity farAhead = MakeMarker(scene, {0.0f, 1.5f, -1.8f});

    Interactable a;
    a.entity = nearSide;
    a.name = "Near";
    a.range = 3.0f;
    interactions.Register(a);

    Interactable b;
    b.entity = farAhead;
    b.name = "Far";
    b.range = 3.0f;
    interactions.Register(b);

    const glm::vec3 eye{0.0f, 1.5f, 0.0f};
    const glm::vec3 forward{0.0f, 0.0f, -1.0f};

    const InteractionSystem::Focus& focus = interactions.UpdateFocus(scene, physics, eye, forward);
    REQUIRE(focus.valid);
    REQUIRE(focus.entity == farAhead);
    REQUIRE(focus.prompt == "Use Far");

    physics.Shutdown();
}

TEST_CASE("Interaction respects range, disabling and line of sight", "[interaction]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));

    Scene scene;
    InteractionSystem interactions;

    const Entity target = MakeMarker(scene, {0.0f, 1.5f, -2.0f});
    Interactable interactable;
    interactable.entity = target;
    interactable.name = "Panel";
    interactable.verb = "Use";
    interactable.range = 2.5f;
    interactions.Register(interactable);

    const glm::vec3 eye{0.0f, 1.5f, 0.0f};
    const glm::vec3 forward{0.0f, 0.0f, -1.0f};

    SECTION("in range, in view, unobstructed")
    {
        REQUIRE(interactions.UpdateFocus(scene, physics, eye, forward).valid);
    }

    SECTION("out of range")
    {
        interactions.SetEnabled(target, true);
        const glm::vec3 distantEye{0.0f, 1.5f, 5.0f};
        REQUIRE_FALSE(interactions.UpdateFocus(scene, physics, distantEye, forward).valid);
    }

    SECTION("looking away")
    {
        REQUIRE_FALSE(interactions.UpdateFocus(scene, physics, eye, glm::vec3(0.0f, 0.0f, 1.0f)).valid);
    }

    SECTION("disabled")
    {
        interactions.SetEnabled(target, false);
        REQUIRE_FALSE(interactions.UpdateFocus(scene, physics, eye, forward).valid);
    }

    SECTION("behind a wall")
    {
        // A slab between the eye and the panel must hide it, so nothing can be used through walls.
        Transform wall;
        wall.position = {0.0f, 1.5f, -1.0f};
        physics.CreateBox({2.0f, 2.0f, 0.1f}, wall, BodyMotion::Static);
        physics.OptimizeBroadPhase();
        REQUIRE_FALSE(interactions.UpdateFocus(scene, physics, eye, forward).valid);
    }

    SECTION("unregistering clears an existing focus")
    {
        REQUIRE(interactions.UpdateFocus(scene, physics, eye, forward).valid);
        interactions.Unregister(target);
        REQUIRE_FALSE(interactions.CurrentFocus().valid);
        REQUIRE(interactions.Count() == 0);
    }

    physics.Shutdown();
}

TEST_CASE("A destroyed entity stops being offered", "[interaction]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));

    Scene scene;
    InteractionSystem interactions;

    const Entity target = MakeMarker(scene, {0.0f, 1.5f, -1.5f});
    Interactable interactable;
    interactable.entity = target;
    interactable.name = "Crate";
    interactions.Register(interactable);

    const glm::vec3 eye{0.0f, 1.5f, 0.0f};
    const glm::vec3 forward{0.0f, 0.0f, -1.0f};
    REQUIRE(interactions.UpdateFocus(scene, physics, eye, forward).valid);

    // The registry can outlive the entity for a frame; a stale handle must not produce a prompt.
    scene.Destroy(target);
    REQUIRE_FALSE(interactions.UpdateFocus(scene, physics, eye, forward).valid);

    physics.Shutdown();
}
