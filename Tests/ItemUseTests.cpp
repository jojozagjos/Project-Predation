#include "Engine/Core/JsonText.h"
#include "Engine/Net/BitStream.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/Net/Protocol.h"
#include "Game/World/LabMap.h"
#include "Game/World/TestMap.h"
#include "Game/World/WorldObjects.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <filesystem>
#include <fstream>
#include <map>

using namespace pred;

// Using items: what each one does, read from items.json, how the hand moves through it, how it
// travels, and that every one of them is on both benches.

namespace
{

std::filesystem::path ItemsFile()
{
    return std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "items.json";
}

} // namespace

TEST_CASE("Every item that is not a weapon has a use, and the use says what it does", "[items][use]")
{
    ItemDatabase items;
    REQUIRE(items.LoadFromFile(ItemsFile()));
    const std::map<std::string, ItemUseKind> expected{{"medkit", ItemUseKind::Heal},
                                                      {"battery", ItemUseKind::Recharge},
                                                      {"keycard", ItemUseKind::Unlock},
                                                      {"flare", ItemUseKind::Flare}};
    for (const auto& [key, kind] : expected)
    {
        const ItemDefinition* item = items.Find(key);
        REQUIRE(item != nullptr);
        INFO(key);
        CHECK(item->use.kind == kind);
        CHECK(item->use.seconds > 0.2f);
        // A hand that moves, and comes back to where it started.
        REQUIRE(item->use.motion.size() >= 3);
        CHECK(item->use.motion.front().t == 0.0f);
        CHECK(item->use.motion.back().t == 1.0f);
        CHECK(glm::length(item->use.motion.front().offset) < 0.001f);
        CHECK(item->benchCount >= 1);
    }
    CHECK(items.Find("medkit")->use.consumed);
    CHECK(items.Find("medkit")->use.amount > 20.0f);
    CHECK_FALSE(items.Find("keycard")->use.consumed);
    // A flare is struck and then thrown: two halves, and the second starts where the first ends.
    const ItemUse& flare = items.Find("flare")->use;
    REQUIRE_FALSE(flare.secondMotion.empty());
    CHECK(glm::distance(flare.motion.back().offset, flare.secondMotion.front().offset) < 0.001f);
    CHECK(flare.amount >= 30.0f);
}

TEST_CASE("A use's motion is eased between its keys", "[items][use]")
{
    std::vector<ItemMotionKey> keys{{0.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
                                    {0.5f, {0.2f, 0.4f, 0.0f}, {90.0f, 0.0f, 0.0f}},
                                    {1.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}};
    CHECK(SampleMotion(keys, 0.5f).offset.y == Catch::Approx(0.4f));
    CHECK(SampleMotion(keys, 0.25f).offset.y == Catch::Approx(0.2f));
    // Eased: a quarter of the way into a span is less than a quarter of the way there.
    CHECK(SampleMotion(keys, 0.125f).offset.y < 0.1f);
    CHECK(SampleMotion(keys, 2.0f).offset.y == Catch::Approx(0.0f));
    CHECK(glm::length(SampleMotion({}, 0.5f).offset) == 0.0f);
}

TEST_CASE("Data files are written the way a person would write them", "[json]")
{
    const nlohmann::json value = {
        {"offset", {-0.10599999874830246, 0.0, 1.5}},
        {"name", "flare"},
        {"keys", nlohmann::json::array({nlohmann::json::array({0.0, 1.0}), nlohmann::json::array({0.5, 2.25})})}};
    const std::string text = JsonText(value);
    INFO(text);
    CHECK(text.find("[-0.106, 0.0, 1.5]") != std::string::npos);
    CHECK(text.find("0.10599999") == std::string::npos);
    CHECK(text.find("[0.5, 2.25]") != std::string::npos);
    // And it reads back as the same thing, to the places it was rounded to.
    const nlohmann::json back = nlohmann::json::parse(text);
    CHECK(back["offset"][0].get<double>() == Catch::Approx(-0.106));
    CHECK(back["name"] == "flare");
}

TEST_CASE("Using an item, a flare thrown and a door unlocked travel intact", "[items][net][protocol]")
{
    ItemUseMessage sent;
    sent.phase = ItemUseMessage::Throw;
    sent.item = 5;
    sent.target = 2;
    sent.door = 0xFF;
    sent.position = {3.0f, 1.2f, -4.0f};
    sent.velocity = {0.0f, 2.5f, 11.0f};
    BitWriter writer;
    WriteMessageHeader(writer, MessageType::ItemUse);
    WriteItemUse(writer, sent);
    const std::vector<uint8_t>& bytes = writer.Finish();
    BitReader reader(bytes.data(), bytes.size());
    MessageType type = MessageType::Count;
    REQUIRE(ReadMessageHeader(reader, type));
    CHECK(type == MessageType::ItemUse);
    ItemUseMessage received;
    REQUIRE(ReadItemUse(reader, received));
    CHECK(received.phase == ItemUseMessage::Throw);
    CHECK(received.item == 5);
    CHECK(received.target == 2);
    CHECK(glm::distance(received.position, sent.position) < 0.02f);
    CHECK(glm::distance(received.velocity, sent.velocity) < 0.2f);

    for (const WorldEventKind kind : {WorldEventKind::ItemUsed, WorldEventKind::FlareThrown, WorldEventKind::DoorUnlocked})
    {
        WorldEventMessage event;
        event.kind = kind;
        event.player = 3;
        event.item = 4;
        event.index = kind == WorldEventKind::DoorUnlocked ? 12 : 1;
        event.other = 2;
        event.amount = 45.0f;
        event.position = {1.0f, 2.0f, 3.0f};
        event.direction = {0.0f, 3.0f, 8.0f};
        BitWriter eventWriter;
        WriteMessageHeader(eventWriter, MessageType::WorldEvent);
        WriteWorldEvent(eventWriter, event);
        const std::vector<uint8_t>& eventBytes = eventWriter.Finish();
        BitReader eventReader(eventBytes.data(), eventBytes.size());
        MessageType eventType = MessageType::Count;
        REQUIRE(ReadMessageHeader(eventReader, eventType));
        WorldEventMessage back;
        REQUIRE(ReadWorldEvent(eventReader, back));
        CHECK(back.kind == kind);
        CHECK(back.player == 3);
        if (kind == WorldEventKind::DoorUnlocked)
        {
            CHECK(back.index == 12);
        }
        else
        {
            CHECK(back.amount == Catch::Approx(45.0f).margin(0.2f));
        }
        if (kind == WorldEventKind::ItemUsed)
        {
            CHECK(back.item == 4);
            CHECK(back.index == 1);
            CHECK(back.other == 2);
        }
        if (kind == WorldEventKind::FlareThrown)
        {
            CHECK(glm::distance(back.position, event.position) < 0.02f);
        }
    }
}

TEST_CASE("Every item is on both equipment benches", "[items][world]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    InteractionSystem interactions;
    ItemDatabase items;
    REQUIRE(items.LoadFromFile(ItemsFile()));
    WorldObjects world;
    world.Build(scene, meshes, physics, interactions, items, nullptr);

    const auto onBench = [&](const glm::vec3& centre, float halfLength)
    {
        std::map<ItemId, int> found;
        for (const WorldObjects::Pickup& pickup : world.Pickups())
        {
            const Transform* transform = scene.GetTransform(pickup.entity);
            if (pickup.alive && transform != nullptr && std::abs(transform->position.x - centre.x) < halfLength &&
                std::abs(transform->position.z - centre.z) < 1.0f)
            {
                found[pickup.item] += pickup.count;
            }
        }
        return found;
    };
    const std::map<ItemId, int> test = onBench({TestMapSpec::kEquipmentBayX, 0.0f, TestMapSpec::kBayZ + 1.3f}, 2.6f);
    const std::map<ItemId, int> lab = onBench(LabSpec::kBench, 2.6f);
    for (const ItemDefinition& item : items.All())
    {
        if (item.id == kInvalidItem)
        {
            continue;
        }
        INFO(item.key);
        CHECK(test.count(item.id) == 1);
        CHECK(lab.count(item.id) == 1);
        if (lab.count(item.id) == 1)
        {
            CHECK(lab.at(item.id) == item.benchCount);
        }
    }
}
