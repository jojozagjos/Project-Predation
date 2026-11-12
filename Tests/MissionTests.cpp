#include "Engine/Net/BitStream.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Mission/Mission.h"
#include "Game/Mission/Objectives.h"
#include "Engine/Core/Paths.h"
#include "Game/Mission/MissionProps.h"
#include "Game/Mission/SiteNames.h"
#include "Game/Mission/SiteSystems.h"
#include "Game/Net/Protocol.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/Vehicles.h"
#include "Game/World/SiteMap.h"
#include "Game/World/SitePlan.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace pred;
namespace
{

// The breaker board worked as somebody at it would: its door opened (if it is not) and the main switch thrown, then each breaker found off
// switched on in turn -- one that trips the main switched off again and left off, and the main thrown again. True when the
// power is back.
bool SolveBoard(MissionState& state, const MissionPlan& plan)
{
    using MissionRules::Worked;
    if (!state.boardOpen && MissionRules::WorkBoard(state, plan, MissionRules::kBoardDoor) != Worked::Opened)
    {
        return false;
    }
    if (!state.boardMain && MissionRules::WorkBoard(state, plan, MissionRules::kBoardMain) == Worked::Done)
    {
        return true;
    }
    for (int b = 0; b < MissionSpec::kBoardBreakers; ++b)
    {
        if (((state.boardOn >> b) & 1u) != 0u)
        {
            continue;
        }
        const Worked worked = MissionRules::WorkBoard(state, plan, MissionRules::kBoardBreaker + b);
        if (worked == Worked::Done)
        {
            return true;
        }
        if (worked == Worked::Tripped)
        {
            MissionRules::WorkBoard(state, plan, MissionRules::kBoardBreaker + b);
            if (MissionRules::WorkBoard(state, plan, MissionRules::kBoardMain) == Worked::Done)
            {
                return true;
            }
        }
    }
    return state.powered;
}

// A station's console worked in its order, each system started once the one before is up to pressure, somebody at it
// throughout. True when the station is restarted.
bool RunConsole(MissionState& state, const MissionPlan& plan)
{
    for (int k = 0; k < MissionRules::kConsoleSystems; ++k)
    {
        if (MissionRules::WorkConsole(state, plan, k) != MissionRules::Worked::Switched)
        {
            return false;
        }
        for (int t = 0; t < 4000; ++t)
        {
            if (MissionRules::Tick(state, plan, 0.25f, true).downloaded)
            {
                return true;
            }
            if (state.progress >= MissionRules::ConsoleCap(state))
            {
                break;
            }
        }
    }
    return false;
}

} // namespace


// The mission: the data on a terminal in one of a site's buildings, downloaded onto a drive and carried to the shuttle.
// Sometimes the terminal's building has no power until its breaker is reset, and sometimes there is no map.

TEST_CASE("A mission's terminal stands on a bench in a room anybody can walk to, the same on every machine", "[mission]")
{
    for (uint32_t seed = 1; seed <= 16; ++seed)
    {
        INFO("seed " << seed);
        const SitePlan site = SitePlan::Generate(seed);
        const MissionPlan plan = MissionPlan::Generate(site, seed);
        REQUIRE(plan.Valid());
        REQUIRE(plan.building < static_cast<int>(site.buildings.size()));
        const FacilityLayout& building = site.buildings[static_cast<size_t>(plan.building)];

        // In the building, in a room of it that can be reached without the keycard, and not the nest.
        glm::vec2 min;
        glm::vec2 max;
        SitePlan::Footprint(building, 0.0f, min, max);
        CHECK(plan.terminal.x > min.x);
        CHECK(plan.terminal.x < max.x);
        CHECK(plan.terminal.z > min.y);
        CHECK(plan.terminal.z < max.y);
        CHECK(plan.room >= 0);
        CHECK(plan.room != building.nestRoom);
        const FacilityLayout::Room& room = building.rooms[static_cast<size_t>(plan.room)];
        CHECK(room.floor == plan.floor);
        CHECK(building.Reachable(false)[building.Index(room.floor, room.min.x, room.min.y)]);
        // Standing on its bench: its foot at a bench's height over its floor.
        const float floor = FacilityMap::ToWorld(building, plan.floor, glm::vec2(0.0f)).y;
        CHECK(std::abs(plan.terminal.y - MissionSpec::kTerminalSize.y * 0.5f - (floor + 0.92f)) < 0.01f);
        // The drive comes out in front of it, on the bench, not in the air or in the wall.
        CHECK(plan.driveAt.y > floor + 0.92f);
        CHECK(plan.driveAt.y < floor + 1.1f);
        CHECK(glm::distance(glm::vec2(plan.driveAt.x, plan.driveAt.z), glm::vec2(plan.terminal.x, plan.terminal.z)) > 0.2f);
        // (A data download takes most of a minute; starting a facility's plant at its console a little less.)
        CHECK(plan.downloadSeconds >= (plan.kind == MissionPlan::Kind::Download ? 35.0f : 20.0f));
        CHECK(plan.downloadSeconds <= 60.0f);
        // With the power out there is a breaker to reset, and lamps that are out with it.
        if (plan.powerOut)
        {
            glm::vec3 panel;
            float yaw = 0.0f;
            CHECK(BreakerPanel(building, panel, yaw));
            CHECK_FALSE(plan.circuits.empty());
        }

        const MissionPlan again = MissionPlan::Generate(site, seed);
        CHECK(again.building == plan.building);
        CHECK(again.terminal == plan.terminal);
        CHECK(again.powerOut == plan.powerOut);
        CHECK(again.mapGiven == plan.mapGiven);
        CHECK(again.downloadSeconds == plan.downloadSeconds);
    }
}

TEST_CASE("Some missions have the power out, some come without a map, and most have neither", "[mission]")
{
    int powerOut = 0;
    int noMap = 0;
    constexpr int kSites = 40;
    for (uint32_t seed = 1; seed <= kSites; ++seed)
    {
        const MissionPlan plan = MissionPlan::Generate(SitePlan::Generate(seed), seed);
        powerOut += plan.powerOut ? 1 : 0;
        noMap += plan.mapGiven ? 0 : 1;
    }
    INFO(powerOut << " with the power out, " << noMap << " without a map, of " << kSites);
    CHECK(powerOut >= kSites / 6);
    CHECK(powerOut <= kSites * 2 / 3);
    CHECK(noMap >= kSites / 10);
    CHECK(noMap <= kSites / 2);
}

TEST_CASE("Every building on a site has one breaker panel on a wall of a room reachable without the keycard, and circuits of its own",
          "[mission][site]")
{
    for (uint32_t seed = 1; seed <= 8; ++seed)
    {
        INFO("seed " << seed);
        const SitePlan site = SitePlan::Generate(seed);
        for (size_t b = 0; b < site.buildings.size(); ++b)
        {
            INFO("building " << b);
            const FacilityLayout& building = site.buildings[b];
            int panels = 0;
            for (const FacilityLayout::Placed& thing : building.things)
            {
                if (thing.thing != FacilityLayout::Thing::Breaker)
                {
                    continue;
                }
                ++panels;
                const glm::ivec2 cell{static_cast<int>(std::floor(thing.at.x)), static_cast<int>(std::floor(thing.at.y))};
                CHECK(building.RoomAt(thing.floor, cell.x, cell.y) >= 0);
                CHECK(building.Reachable(false)[building.Index(thing.floor, cell.x, cell.y)]);
            }
            CHECK(panels == 1);
            glm::vec3 panel;
            float yaw = 0.0f;
            REQUIRE(BreakerPanel(building, panel, yaw));
            // At the height of a hand, on the wall.
            const float floor = FacilityMap::ToWorld(building, 0, glm::vec2(0.0f)).y;
            CHECK(panel.y > floor + 1.0f);
            // Its lamps on circuits nobody else's are on.
            for (const FacilityLayout::Lamp& lamp : building.lamps)
            {
                CHECK(lamp.circuit / 100 == static_cast<int>(b) + 1);
            }
        }
    }
}

TEST_CASE("The download needs power and somebody at the terminal, and when it is done the drive is ready", "[mission]")
{
    MissionPlan plan;
    plan.building = 0;
    plan.powerOut = true;
    plan.downloadSeconds = 40.0f;
    MissionState state = MissionRules::Start(plan);
    CHECK(state.stage == MissionState::Stage::Find);
    CHECK_FALSE(state.powered);
    CHECK_FALSE(MissionRules::StartDownload(state));
    CHECK(MissionRules::WorkBoard(state, plan, MissionRules::kBoardMain) == MissionRules::Worked::Nothing); // (its door shut)
    CHECK(SolveBoard(state, plan));
    CHECK(state.powered);
    CHECK(MissionRules::WorkBoard(state, plan, MissionRules::kBoardMain) == MissionRules::Worked::Nothing);
    REQUIRE(MissionRules::StartDownload(state));
    CHECK(state.stage == MissionState::Stage::Downloading);

    // Left alone, it waits.
    for (int i = 0; i < 60; ++i)
    {
        CHECK_FALSE(MissionRules::Tick(state, plan, 0.5f, false).downloaded);
    }
    CHECK(state.progress == 0.0f);
    CHECK_FALSE(state.attended);
    // Attended, it goes on, and finishes in the time it takes.
    bool downloaded = false;
    float took = 0.0f;
    while (!downloaded && took < 100.0f)
    {
        downloaded = MissionRules::Tick(state, plan, 0.1f, true).downloaded;
        took += 0.1f;
    }
    CHECK(downloaded);
    CHECK(std::abs(took - plan.downloadSeconds) < 0.25f);
    CHECK(state.stage == MissionState::Stage::Carry);
    CHECK_FALSE(MissionRules::StartDownload(state));
}

TEST_CASE("A launch counts down and can be held; when the shuttle goes, it is over with whoever is aboard", "[mission]")
{
    MissionPlan plan;
    plan.building = 0;
    MissionState state = MissionRules::Start(plan);
    REQUIRE(MissionRules::ToggleLaunch(state));
    CHECK(state.Launching());
    CHECK(state.launchIn == MissionSpec::kLaunchSeconds);
    CHECK_FALSE(MissionRules::Tick(state, plan, 10.0f, false).launched);
    CHECK(std::abs(state.launchIn - (MissionSpec::kLaunchSeconds - 10.0f)) < 1.0e-4f);
    // Held: the countdown stops, and starting again starts it from the top.
    REQUIRE(MissionRules::ToggleLaunch(state));
    CHECK_FALSE(state.Launching());
    CHECK_FALSE(MissionRules::Tick(state, plan, 30.0f, false).launched);
    REQUIRE(MissionRules::ToggleLaunch(state));
    CHECK(state.launchIn == MissionSpec::kLaunchSeconds);
    bool launched = false;
    for (int i = 0; i < 25 && !launched; ++i)
    {
        launched = MissionRules::Tick(state, plan, 1.0f, false).launched;
    }
    REQUIRE(launched);
    MissionRules::Finish(state, true, 0b0101);
    CHECK(state.stage == MissionState::Stage::Over);
    CHECK(state.recovered);
    CHECK(state.aboard == 0b0101);
    CHECK_FALSE(MissionRules::ToggleLaunch(state));
}

TEST_CASE("How the mission stands crosses the wire unchanged", "[mission][net][protocol]")
{
    WorldEventMessage sent;
    sent.kind = WorldEventKind::Mission;
    sent.index = static_cast<uint8_t>(MissionState::Stage::Downloading);
    sent.flag = true;
    sent.flag2 = true;
    sent.amount = 0.37f;
    sent.item = 143;
    sent.rounds = 1;
    sent.other = 0b1010;
    sent.direction.z = 1234.0f; // (the board's switches and the device's, packed)
    sent.reserve = 1;           // (the terminal found, where there was no map)
    BitWriter writer;
    WriteWorldEvent(writer, sent);
    const std::vector<uint8_t>& bytes = writer.Finish();
    BitReader reader(bytes.data(), bytes.size());
    WorldEventMessage received;
    REQUIRE(ReadWorldEvent(reader, received));
    CHECK(received.kind == WorldEventKind::Mission);
    CHECK(received.index == sent.index);
    CHECK(received.flag);
    CHECK(received.flag2);
    CHECK(std::abs(received.amount - 0.37f) < 0.002f);
    CHECK(received.item == 143);
    CHECK(received.rounds == 1);
    CHECK(received.other == 0b1010);
    CHECK(received.direction.z == 1234.0f);
    CHECK(received.reserve == 1);
}

TEST_CASE("The shuttle stands on its legs with its ramp down to the pad", "[mission][vehicle]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    const CinePose home{{0.0f, 0.1f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}};
    VehicleProp shuttle;
    REQUIRE(shuttle.Build(scene, meshes, &physics, std::make_shared<ModelAsset>(Vehicles::ShuttleModel()), home, physics.NewOverlapGroup(), "test_"));
    CinePose arrival;
    REQUIRE(shuttle.Socket("arrival", arrival));
    const RayHit deck = physics.RayCast(arrival.position + glm::vec3(0.0f, 1.0f, 0.0f), {0.0f, -1.0f, 0.0f}, 3.0f);
    REQUIRE(deck.hit);
    CHECK(std::abs(deck.position.y - 0.8f) < 0.02f);
    // Halfway down the ramp, somewhere between the cabin floor and the pad.
    const RayHit ramp = physics.RayCast({0.0f, 3.0f, 3.75f}, {0.0f, -1.0f, 0.0f}, 4.0f);
    REQUIRE(ramp.hit);
    CHECK(ramp.position.y > 0.1f + 0.25f);
    CHECK(ramp.position.y < 0.8f - 0.2f);
    // Its launch console at the front, on the flight deck, facing back down the shuttle, with room to stand at it.
    CinePose console;
    REQUIRE(shuttle.Socket("console", console));
    CHECK(shuttle.Part("console").IsValid());
    CHECK(shuttle.Aboard(console.position + glm::vec3(0.0f, 0.1f, 0.0f)));
    const glm::vec3 faces = console.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    CHECK(faces.z > 0.99f);
    CHECK_FALSE(physics.RayCast(console.position + faces * 0.3f + glm::vec3(0.0f, 1.6f, 0.0f), faces, 0.8f).hit);
}

TEST_CASE("What the mission puts in the world -- the terminal, the boards, the console -- is inside nothing", "[mission][site]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    InteractionSystem interactions;
    for (const uint16_t seed : {uint16_t{1}, uint16_t{3}})
    {
        INFO("seed " << seed);
        SiteMap site;
        site.Build(seed, scene, meshes, physics, nullptr);
        const MissionPlan plan = MissionPlan::Generate(site.Plan(), seed);
        MissionProps props;
        props.Build(scene, meshes, nullptr, physics, interactions, site.Plan(), plan, site.Shuttle());
        const std::vector<PhysicsWorld::StaticOverlap> overlaps = physics.FindStaticOverlaps(0.01f);
        for (size_t i = 0; i < overlaps.size() && i < 4; ++i)
        {
            UNSCOPED_INFO("overlap " << overlaps[i].penetration * 1000.0f << " mm at " << overlaps[i].position.x << " "
                                     << overlaps[i].position.y << " " << overlaps[i].position.z);
        }
        CHECK(overlaps.empty());
        // Something to press at the terminal (a station's: its switches), on the board whose power is out, and at the console.
        CHECK(interactions.FindByPayload(InteractionKind::Terminal, plan.AtConsole() ? 1 : 0) != nullptr);
        CHECK(interactions.FindByPayload(InteractionKind::Launch, 0) != nullptr);
        CHECK((interactions.FindByPayload(InteractionKind::Breaker, MissionRules::kBoardMain) != nullptr) == plan.powerOut);
        props.Clear(scene, physics, interactions);
        site.Clear(scene, physics, nullptr);
    }
}

TEST_CASE("A site is called the same on every machine, in the style of the agreed examples", "[mission]")
{
    SiteNames names; // the examples built in
    const SiteTitle a = names.For(91);
    const SiteTitle b = names.For(91);
    CHECK(a.planet == b.planet);
    CHECK(a.site == b.site);
    CHECK(a.planet.rfind("KEPLER-", 0) == 0);
    CHECK(a.site.rfind("POLAR RESEARCH FACILITY ", 0) == 0);
    CHECK(a.site.find(", NORTH CRYOSPHERE") != std::string::npos);
    // Different sites, different numbers, mostly.
    int different = 0;
    for (uint32_t seed = 1; seed <= 20; ++seed)
    {
        different += names.For(seed).planet != a.planet ? 1 : 0;
    }
    CHECK(different >= 15);
    // And the file the game reads is readable.
    SiteNames fromFile;
    std::string error;
    CHECK(fromFile.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "sites.json", &error));
    INFO(error);
    CHECK_FALSE(fromFile.catalogues.empty());
    CHECK_FALSE(fromFile.kinds.empty());
}

TEST_CASE("The intercom says one of the lines written for a moment, the same one on every machine, and nothing without any", "[mission]")
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "predation_intercom_test.json";
    {
        std::ofstream out(file);
        out << R"({"lines": {"arrival": [{"sound": "Intercom/a", "subtitle": "One."}, {"sound": "Intercom/b", "subtitle": "Two.", "seconds": 4}],
                              "launch": []}})";
    }
    IntercomLines lines;
    REQUIRE(lines.LoadFromFile(file));
    CHECK(lines.Count() == 2);
    const IntercomLine* first = lines.Pick("arrival", 7);
    REQUIRE(first != nullptr);
    CHECK(lines.Pick("arrival", 7) == first);
    CHECK(lines.Pick("launch", 7) == nullptr);
    CHECK(lines.Pick("nothing", 7) == nullptr);
    // Across missions, both get said.
    bool one = false;
    bool two = false;
    for (uint32_t seed = 0; seed < 40; ++seed)
    {
        const IntercomLine* line = lines.Pick("arrival", seed);
        one = one || line->subtitle == "One.";
        two = two || line->subtitle == "Two.";
    }
    CHECK(one);
    CHECK(two);
    CHECK(IntercomLines::SecondsFor(*lines.Pick("arrival", 0)) >= 2.5f);
    std::filesystem::remove(file);

    // The file the game reads is readable, and knows every moment.
    IntercomLines game;
    CHECK(game.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "intercom.json"));
    CHECK(IntercomLines::Moments().size() == 13);
}

TEST_CASE("Standing in front of the terminal or the breaker board and looking at a thing on it, it is offered", "[mission][site][interaction]")
{
    // Its focus was turned twice -- once where it was set, once where it was used -- so a terminal or a panel facing
    // any way but one had its focus behind it, hidden by its own front: no prompt, and nothing to be done with it.
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    InteractionSystem interactions;
    int boards = 0;
    for (const uint16_t seed : {uint16_t{1}, uint16_t{3}, uint16_t{5}, uint16_t{8}, uint16_t{11}, uint16_t{14}})
    {
        INFO("seed " << seed);
        SiteMap site;
        site.Build(seed, scene, meshes, physics, nullptr);
        const MissionPlan plan = MissionPlan::Generate(site.Plan(), seed);
        MissionProps props;
        props.Build(scene, meshes, nullptr, physics, interactions, site.Plan(), plan, site.Shuttle());
        physics.OptimizeBroadPhase();
        MissionState state = MissionRules::Start(plan);
        float time = 0.0f;
        const auto show = [&]
        {
            // (Long enough for the door to have swung open.)
            for (int i = 0; i < 30; ++i)
            {
                time += 0.1f;
                props.Show(scene, interactions, nullptr, state, time);
            }
        };
        show();
        // From where the camera is brought up close, each of a device's controls is the one under the pointer when it is
        // pointed at: in reach, not behind anything, and not taken for its neighbour.
        const auto pickable = [&](MissionProps::Device device, InteractionKind kind, int payload)
        {
            glm::vec3 eye;
            glm::vec3 target;
            REQUIRE(props.View(device, eye, target));
            const Interactable* thing = interactions.FindByPayload(kind, payload);
            REQUIRE(thing != nullptr);
            const glm::vec3 focus = interactions.FocusPoint(scene, *thing);
            const InteractionSystem::Focus& found = interactions.UpdateFocus(scene, physics, eye, focus - eye, 5.0f, true);
            return found.valid && found.entity == thing->entity && props.DeviceOf(found.entity) == device;
        };
        const auto offered = [&](InteractionKind kind, int payload)
        {
            const Interactable* thing = interactions.FindByPayload(kind, payload);
            REQUIRE(thing != nullptr);
            const Transform* at = scene.GetTransform(thing->entity);
            REQUIRE(at != nullptr);
            const glm::vec3 front = at->rotation * glm::vec3(0.0f, 0.0f, -1.0f);
            const glm::vec3 focus = interactions.FocusPoint(scene, *thing);
            const glm::vec3 eye = focus + front * 0.9f + glm::vec3(0.0f, 0.5f, 0.0f);
            const InteractionSystem::Focus& found = interactions.UpdateFocus(scene, physics, eye, focus - eye, 28.0f, thing->closeUp);
            return found.valid && found.entity == thing->entity;
        };
        if (plan.powerOut)
        {
            // The board: brought up close; its door, then once it is open each of its switches -- offered only up close.
            ++boards;
            CHECK(offered(InteractionKind::Device, static_cast<int>(MissionProps::Device::Board)));
            CHECK(interactions.FindByPayload(InteractionKind::Breaker, MissionRules::kBoardDoor)->closeUp);
            CHECK(offered(InteractionKind::Breaker, MissionRules::kBoardDoor));
            CHECK(pickable(MissionProps::Device::Board, InteractionKind::Breaker, MissionRules::kBoardDoor));
            REQUIRE(MissionRules::WorkBoard(state, plan, MissionRules::kBoardDoor) == MissionRules::Worked::Opened);
            show();
            for (int control = MissionRules::kBoardMain; control < MissionRules::kBoardBreaker + MissionSpec::kBoardBreakers; ++control)
            {
                INFO("control " << control);
                CHECK(offered(InteractionKind::Breaker, control));
                CHECK(pickable(MissionProps::Device::Board, InteractionKind::Breaker, control));
            }
            REQUIRE(SolveBoard(state, plan));
            show();
        }
        if (plan.AtConsole())
        {
            CHECK(offered(InteractionKind::Device, static_cast<int>(MissionProps::Device::Console)));
            for (int k = 1; k <= MissionRules::kConsoleSystems; ++k)
            {
                INFO("switch " << k);
                CHECK(offered(InteractionKind::Terminal, k));
                CHECK(pickable(MissionProps::Device::Console, InteractionKind::Terminal, k));
            }
        }
        else if (plan.building >= 0)
        {
            CHECK(offered(InteractionKind::Terminal, 0));
        }
        props.Clear(scene, physics, interactions);
        site.Clear(scene, physics, nullptr);
    }
    CHECK(boards > 0);
}
TEST_CASE("The shuttle's sides are solid at its skin, so nobody puts their head through into it", "[mission][vehicle]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    const CinePose home{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}};
    VehicleProp shuttle;
    REQUIRE(shuttle.Build(scene, meshes, &physics, std::make_shared<ModelAsset>(Vehicles::ShuttleModel()), home, physics.NewOverlapGroup(), "test_"));
    // The skin's half width and the height of its straight sides along its length (Vehicles::ShuttleModel's sections).
    struct Section
    {
        float z, half, low, high;
    };
    const Section sections[] = {{-5.75f, 1.45f, 0.78f, 1.3f}, {-5.05f, 1.9f, 0.8f, 2.3f}, {-4.4f, 2.12f, 0.75f, 3.05f}, {2.2f, 2.12f, 0.75f, 3.05f}};
    int tested = 0;
    for (float z = -5.6f; z < 2.2f; z += 0.25f)
    {
        size_t i = 0;
        while (sections[i + 1].z < z)
        {
            ++i;
        }
        const float t = (z - sections[i].z) / (sections[i + 1].z - sections[i].z);
        const float half = sections[i].half + (sections[i + 1].half - sections[i].half) * t;
        const float low = std::max(sections[i].low, sections[i + 1].low);
        const float high = std::min(sections[i].high, sections[i + 1].high);
        for (float y = low + 0.1f; y < high - 0.1f; y += 0.3f)
        {
            for (const float s : {-1.0f, 1.0f})
            {
                INFO("z " << z << " y " << y << " side " << s);
                const RayHit hit = physics.RayCast({s * 4.0f, y, z}, {-s, 0.0f, 0.0f}, 4.0f);
                REQUIRE(hit.hit);
                // Stopped within a few centimetres of the skin (or on something outside it, an engine).
                CHECK(std::abs(hit.position.x) > half - 0.15f);
                ++tested;
            }
        }
    }
    CHECK(tested > 40);
}

TEST_CASE("The power to put back is always the building's own: its board in it, worked by hand, and nothing else answering", "[mission][site]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    InteractionSystem interactions;
    // A site whose power is out.
    uint16_t seed = 1;
    while (seed < 200 && !MissionPlan::Generate(SitePlan::Generate(seed), seed).powerOut)
    {
        ++seed;
    }
    INFO("seed " << seed);
    SiteMap site;
    site.Build(seed, scene, meshes, physics, nullptr);
    const MissionPlan plan = MissionPlan::Generate(site.Plan(), seed);
    REQUIRE(plan.powerOut);
    MissionProps props;
    props.Build(scene, meshes, nullptr, physics, interactions, site.Plan(), plan, site.Shuttle());
    // Its door, its main switch and its six breakers, all on the board on the terminal's building's wall; nothing more.
    glm::vec3 panel;
    float yaw = 0.0f;
    REQUIRE(BreakerPanel(site.Plan().buildings[static_cast<size_t>(plan.building)], panel, yaw));
    for (int control = 0; control < MissionRules::kBoardBreaker + MissionSpec::kBoardBreakers; ++control)
    {
        INFO("control " << control);
        const Interactable* thing = interactions.FindByPayload(InteractionKind::Breaker, control);
        REQUIRE(thing != nullptr);
        CHECK(glm::distance(interactions.FocusPoint(scene, *thing), panel) < 0.8f);
    }
    CHECK(interactions.FindByPayload(InteractionKind::Breaker, MissionRules::kBoardBreaker + MissionSpec::kBoardBreakers) == nullptr);
    props.Clear(scene, physics, interactions);
    site.Clear(scene, physics, nullptr);
}
TEST_CASE("The download mission's steps, as data: power when it is out and found so, then the download, then the drive to the shuttle", "[mission][objectives]")
{
    Objectives::Library library;
    std::string error;
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");
    const bool loaded = library.LoadFromFile(Paths::AssetsRoot() / "Data" / "missions.json", &error);
    INFO(error);
    REQUIRE(loaded);
    const Objectives::Template* source = library.Find("download_data");
    REQUIRE(source != nullptr);
    using Stage = MissionState::Stage;
    const auto open = [](const Objectives::Graph& graph)
    {
        std::vector<Objectives::Kind> kinds;
        for (const int index : graph.Open())
        {
            kinds.push_back(graph.steps[static_cast<size_t>(index)].def->kind);
        }
        return kinds;
    };

    // With the power on: the download, then the drive; no step for the power at all.
    MissionPlan plan;
    plan.building = 0;
    plan.powerOut = false;
    Objectives::Graph graph = Objectives::Lay(*source, plan);
    CHECK(graph.steps.size() == 2);
    MissionState state = MissionRules::Start(plan);
    Objectives::Update(graph, state, false);
    CHECK(open(graph) == std::vector<Objectives::Kind>{Objectives::Kind::Download});
    state.stage = Stage::Carry;
    Objectives::Update(graph, state, false);
    CHECK(open(graph) == std::vector<Objectives::Kind>{Objectives::Kind::Carry});
    state.stage = Stage::Over;
    state.recovered = true;
    Objectives::Update(graph, state, false);
    CHECK(open(graph).empty());
    CHECK(graph.Complete());

    // With it out: the download shown until somebody finds the terminal dead; then the breaker until it is reset; then on.
    plan.powerOut = true;
    graph = Objectives::Lay(*source, plan);
    CHECK(graph.steps.size() == 3);
    state = MissionRules::Start(plan);
    REQUIRE_FALSE(state.powered);
    Objectives::Update(graph, state, false);
    CHECK(open(graph) == std::vector<Objectives::Kind>{Objectives::Kind::Download});
    Objectives::Update(graph, state, true);
    REQUIRE(open(graph) == std::vector<Objectives::Kind>{Objectives::Kind::RestorePower});
    // (Telling the crew to reset it.)
    CHECK_FALSE(Objectives::TextOf(graph.steps[static_cast<size_t>(graph.Open().front())]).empty());
    state.powered = true;
    Objectives::Update(graph, state, true);
    CHECK(open(graph) == std::vector<Objectives::Kind>{Objectives::Kind::Download});

    // A briefing without a map says so under the download.
    plan.mapGiven = false;
    CHECK(Objectives::HintOf(graph.steps[static_cast<size_t>(graph.Open().front())], plan).find("map") != std::string::npos);

    // And what is not a template is told why.
    Objectives::Library broken;
    CHECK_FALSE(broken.LoadFromString(R"({"templates": [{"id": "x", "steps": [{"id": "a", "kind": "fly"}]}]})", &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("A site says what it has for a mission: its terminals, its power and breakers, its locks and keycard, its ways in, its rooms", "[mission][site]")
{
    for (uint16_t base = 1; base <= 8; ++base)
    {
        const uint16_t seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Facility);
        INFO("seed " << seed);
        const SitePlan site = SitePlan::Generate(seed);
        const SiteSystems systems = SiteSystems::Survey(site);
        // Somewhere a mission can put its business, and every building's power, each with its breaker.
        CHECK_FALSE(systems.OpenTerminals().empty());
        REQUIRE(systems.power.size() == site.buildings.size());
        for (size_t b = 0; b < site.buildings.size(); ++b)
        {
            const SiteSystems::Power* power = systems.PowerOf(static_cast<int>(b));
            REQUIRE(power != nullptr);
            CHECK_FALSE(power->circuits.empty());
            CHECK(power->breaker);
        }
        // Where anything is locked, the keycard is somewhere to find.
        if (!systems.locks.empty())
        {
            CHECK_FALSE(systems.keycards.empty());
        }
        // Every way in, and every room, accounted for.
        size_t exits = 0;
        size_t rooms = 0;
        for (const FacilityLayout& building : site.buildings)
        {
            exits += building.exits.size();
            rooms += building.rooms.size();
        }
        CHECK(systems.ways.size() == exits);
        CHECK(systems.rooms.size() == rooms);
        // And the mission's terminal is one of the open ones.
        const MissionPlan plan = MissionPlan::Generate(site, seed);
        REQUIRE(plan.Valid());
        CHECK((plan.cave >= 0 || std::any_of(systems.terminals.begin(), systems.terminals.end(), [&](const SiteSystems::Terminal& terminal)
                          { return terminal.open && terminal.spot.building == plan.building && terminal.spot.room == plan.room; })));
        // (Or down its cave, on a station in a chamber out of the water.)
        if (plan.cave >= 0)
        {
            REQUIRE(static_cast<size_t>(plan.cave) < systems.caves.size());
            CHECK(std::any_of(systems.caves[static_cast<size_t>(plan.cave)].chambers.begin(), systems.caves[static_cast<size_t>(plan.cave)].chambers.end(),
                              [&](const glm::vec3& floor) { return std::abs(floor.x - plan.terminal.x) < 0.01f && std::abs(floor.z - plan.terminal.z) < 0.01f; }));
        }
    }
}

TEST_CASE("A survey site's mission is a survey: readings at markers across the site, one at a time while somebody stays, then back", "[mission]")
{
    int surveys = 0;
    for (uint16_t base = 1; base <= 20; ++base)
    {
        const uint16_t seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Survey);
        const SitePlan site = SitePlan::Generate(seed);
        const MissionPlan plan = MissionPlan::Generate(site, seed);
        if (plan.kind != MissionPlan::Kind::Survey)
        {
            continue;
        }
        ++surveys;
        INFO("seed " << seed);
        REQUIRE(plan.Valid());
        CHECK(plan.markers.size() >= 3);
        CHECK(plan.markers.size() <= 5);
        for (const glm::vec3& marker : plan.markers)
        {
            CHECK_FALSE(site.InBuilding({marker.x, marker.z}, 4.0f));
        }
        MissionState state = MissionRules::Start(plan);
        CHECK(state.stage == MissionState::Stage::Find);
        CHECK_FALSE(MissionRules::StartReading(state, plan, static_cast<int>(plan.markers.size())));
        for (int m = 0; m < static_cast<int>(plan.markers.size()); ++m)
        {
            REQUIRE(MissionRules::StartReading(state, plan, m));
            // (Not a second at once.)
            CHECK_FALSE(MissionRules::StartReading(state, plan, (m + 1) % static_cast<int>(plan.markers.size())));
            // Nobody there: it waits.
            MissionRules::Tick(state, plan, 2.0f, false);
            CHECK(state.progress == 0.0f);
            bool read = false;
            for (int t = 0; t < 100 && !read; ++t)
            {
                read = MissionRules::Tick(state, plan, 0.25f, true).read;
            }
            REQUIRE(read);
            CHECK(((state.readings >> m) & 1u) != 0u);
            // (Taken once: not again.)
            CHECK_FALSE(MissionRules::StartReading(state, plan, m));
        }
        CHECK(MissionRules::AllReadings(state, plan));
        CHECK(state.stage == MissionState::Stage::Carry);
    }
    CHECK(surveys > 0);
}

TEST_CASE("A station's mission is to restore it: its power out at its breaker, life support restarted at the console, nothing to carry", "[mission]")
{
    Objectives::Library library;
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");
    REQUIRE(library.LoadFromFile(Paths::AssetsRoot() / "Data" / "missions.json"));
    const Objectives::Template* source = library.Find(Objectives::TemplateFor(MissionPlan::Kind::Restore));
    REQUIRE(source != nullptr);
    CHECK(source->brief.size() == 3);
    CHECK_FALSE(source->done.empty());
    CHECK_FALSE(source->failed.empty());
    int restores = 0;
    for (uint16_t base = 1; base <= 20; ++base)
    {
        const uint16_t seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Station);
        const SitePlan site = SitePlan::Generate(seed);
        const MissionPlan plan = MissionPlan::Generate(site, seed);
        if (plan.kind != MissionPlan::Kind::Restore)
        {
            // (Down a cave instead, now and then: a download from the station there, as anywhere.)
            CHECK(plan.cave >= 0);
            continue;
        }
        ++restores;
        INFO("seed " << seed);
        REQUIRE(plan.Valid());
        CHECK(plan.markers.empty());
        // The power is out wherever the building has a breaker to reset -- and every building has one.
        glm::vec3 panel;
        float yaw = 0.0f;
        REQUIRE(BreakerPanel(site.buildings[static_cast<size_t>(plan.building)], panel, yaw));
        CHECK(plan.powerOut);
        // Known from the start, not on finding the console dead: the breaker first, then the restart, then back.
        Objectives::Graph graph = Objectives::Lay(*source, plan);
        REQUIRE(graph.steps.size() == 3);
        MissionState state = MissionRules::Start(plan);
        Objectives::Update(graph, state, false);
        REQUIRE(graph.Open().size() == 1);
        CHECK(graph.steps[static_cast<size_t>(graph.Open().front())].def->kind == Objectives::Kind::RestorePower);
        CHECK_FALSE(MissionRules::StartDownload(state));
        CHECK(MissionRules::WorkConsole(state, plan, 0) == MissionRules::Worked::Nothing); // (no power to it)
        REQUIRE(SolveBoard(state, plan));
        Objectives::Update(graph, state, false);
        REQUIRE(graph.Open().size() == 1);
        const Objectives::Step& restart = graph.steps[static_cast<size_t>(graph.Open().front())];
        CHECK(restart.def->kind == Objectives::Kind::Download);
        CHECK_FALSE(restart.def->working.empty());
        CHECK_FALSE(restart.def->stay.empty());
        CHECK_FALSE(restart.def->paused.empty());
        REQUIRE(RunConsole(state, plan));
        CHECK(state.stage == MissionState::Stage::Carry);
        Objectives::Update(graph, state, false);
        REQUIRE(graph.Open().size() == 1);
        CHECK(graph.steps[static_cast<size_t>(graph.Open().front())].def->kind == Objectives::Kind::Carry);
    }
    CHECK(restores >= 10);
}

TEST_CASE("A signal site's mission is its transmitter: one, out on the site away from the pad, shut down by hand", "[mission]")
{
    Objectives::Library library;
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");
    REQUIRE(library.LoadFromFile(Paths::AssetsRoot() / "Data" / "missions.json"));
    const Objectives::Template* source = library.Find(Objectives::TemplateFor(MissionPlan::Kind::Signal));
    REQUIRE(source != nullptr);
    CHECK(source->brief.size() == 3);
    int signals = 0;
    for (uint16_t base = 1; base <= 20; ++base)
    {
        const uint16_t seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Signal);
        if (MissionPlan::KindFor(seed) != MissionPlan::Kind::Signal)
        {
            continue;
        }
        const SitePlan site = SitePlan::Generate(seed);
        const MissionPlan plan = MissionPlan::Generate(site, seed);
        INFO("seed " << seed);
        REQUIRE(plan.kind == MissionPlan::Kind::Signal);
        ++signals;
        REQUIRE(plan.markers.size() == 1);
        const glm::vec2 at{plan.markers.front().x, plan.markers.front().z};
        CHECK_FALSE(site.InBuilding(at, 4.0f));
        CHECK(glm::distance(at, glm::vec2(site.landing.x, site.landing.z)) > 40.0f);
        // (On the site.)
        CHECK(at.x > site.origin.x);
        CHECK(at.y > site.origin.z);
        CHECK(at.x < site.origin.x + site.size);
        CHECK(at.y < site.origin.z + site.size);
        // The same on every machine.
        const MissionPlan again = MissionPlan::Generate(site, seed);
        CHECK(again.markers.front() == plan.markers.front());

        Objectives::Graph graph = Objectives::Lay(*source, plan);
        REQUIRE(graph.steps.size() == 2);
        MissionState state = MissionRules::Start(plan);
        Objectives::Update(graph, state, false);
        REQUIRE(graph.Open().size() == 1);
        CHECK(graph.steps[static_cast<size_t>(graph.Open().front())].def->kind == Objectives::Kind::Readings);
        // Shut down by hand: its lid, its transmitter off, then its power -- not while it is still sending.
        using MissionRules::Worked;
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCasePower) == Worked::Nothing);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseLid) == Worked::Opened);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCasePower) == Worked::Tripped);
        CHECK(state.stage == MissionState::Stage::Find);
        CHECK((state.device & MissionState::kPowerOff) == 0);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseSender) == Worked::Switched);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseSender) == Worked::Switched);
        CHECK((state.device & MissionState::kSenderOff) == 0);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseSender) == Worked::Switched);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCasePower) == Worked::Done);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseSender) == Worked::Nothing);
        CHECK(state.stage == MissionState::Stage::Carry);
        Objectives::Update(graph, state, false);
        REQUIRE(graph.Open().size() == 1);
        CHECK(graph.steps[static_cast<size_t>(graph.Open().front())].def->kind == Objectives::Kind::Carry);
    }
    // (About half of them: the rest are brought back.)
    CHECK(signals >= 5);
}

TEST_CASE("A breaker board is found with its main switch off and two to four breakers tripped, one of them now and then on a fault", "[mission]")
{
    int boards = 0;
    int faulted = 0;
    for (uint32_t seed = 1; seed <= 200; ++seed)
    {
        const SitePlan site = SitePlan::Generate(seed);
        const MissionPlan plan = MissionPlan::Generate(site, seed);
        if (!plan.powerOut)
        {
            continue;
        }
        INFO("seed " << seed);
        ++boards;
        const int tripped = std::popcount(static_cast<unsigned>(plan.boardTripped));
        CHECK(tripped >= 2);
        CHECK(tripped <= 4);
        CHECK((plan.boardTripped & ~MissionSpec::kBoardAll) == 0);
        if (plan.boardFault >= 0)
        {
            ++faulted;
            CHECK(((plan.boardTripped >> plan.boardFault) & 1u) != 0u);
        }
        // The same on every machine.
        const MissionPlan again = MissionPlan::Generate(site, seed);
        CHECK(again.boardTripped == plan.boardTripped);
        CHECK(again.boardFault == plan.boardFault);
        const MissionState state = MissionRules::Start(plan);
        CHECK_FALSE(state.boardOpen);
        CHECK_FALSE(state.boardMain);
        CHECK(state.boardOn == (MissionSpec::kBoardAll & ~plan.boardTripped));
        // Every lamp through a sound breaker: none left dark by the one to leave off.
        for (size_t c = 0; c < plan.circuits.size(); ++c)
        {
            CHECK(MissionRules::BreakerOf(plan, c) != plan.boardFault);
        }
    }
    INFO(boards << " boards, " << faulted << " with a fault");
    CHECK(boards >= 20);
    CHECK(faulted > 0);
    CHECK(faulted < boards);
}

TEST_CASE("A faulted breaker trips the main switch with a bang, and the power is back once it is found and left off", "[mission]")
{
    using MissionRules::Worked;
    MissionPlan plan;
    plan.building = 0;
    plan.powerOut = true;
    plan.boardTripped = 0b011010; // breakers 1, 3 and 4
    plan.boardFault = 3;
    MissionState state = MissionRules::Start(plan);
    const auto work = [&](int control) { return MissionRules::WorkBoard(state, plan, control); };
    const int breaker = MissionRules::kBoardBreaker;

    // Nothing through its shut door.
    CHECK(work(MissionRules::kBoardMain) == Worked::Nothing);
    CHECK(work(breaker + 1) == Worked::Nothing);
    CHECK(work(MissionRules::kBoardDoor) == Worked::Opened);
    CHECK(work(MissionRules::kBoardDoor) == Worked::Nothing);
    // The main on: the breakers that did not trip are live.
    CHECK(work(MissionRules::kBoardMain) == Worked::Switched);
    CHECK(MissionRules::BreakerLive(state, 0));
    CHECK_FALSE(MissionRules::BreakerLive(state, 1));
    CHECK(work(breaker + 1) == Worked::Switched);
    CHECK(MissionRules::BreakerLive(state, 1));
    // The faulted one on: the main goes, everything is dead again.
    CHECK(work(breaker + 3) == Worked::Tripped);
    CHECK_FALSE(state.boardMain);
    CHECK_FALSE(MissionRules::BreakerLive(state, 0));
    // Nor will the main stay on while it is.
    CHECK(work(MissionRules::kBoardMain) == Worked::Tripped);
    CHECK_FALSE(state.boardMain);
    CHECK_FALSE(state.powered);
    // Off, and the main back on; the last sound one on, and the power is back.
    CHECK(work(breaker + 3) == Worked::Switched);
    CHECK(work(MissionRules::kBoardMain) == Worked::Switched);
    CHECK_FALSE(state.powered);
    CHECK(work(breaker + 4) == Worked::Done);
    CHECK(state.powered);
    CHECK_FALSE(MissionRules::BreakerLive(state, 3));
    // Then it is left as it is.
    CHECK(work(breaker + 3) == Worked::Nothing);
    CHECK(work(MissionRules::kBoardMain) == Worked::Nothing);
}

TEST_CASE("A station's life support is started a system at a time, in turn and each at pressure, or it all trips", "[mission]")
{
    using MissionRules::Worked;
    MissionPlan plan;
    plan.kind = MissionPlan::Kind::Restore;
    plan.building = 0;
    plan.powerOut = true;
    plan.downloadSeconds = 30.0f;
    MissionState state = MissionRules::Start(plan);
    // Not without power.
    CHECK(MissionRules::WorkConsole(state, plan, 0) == Worked::Nothing);
    REQUIRE(SolveBoard(state, plan));

    // Out of turn: it trips, and says so until a system is next started.
    CHECK(MissionRules::WorkConsole(state, plan, 1) == Worked::Tripped);
    CHECK(state.device == MissionState::kConsoleTripped);
    CHECK(state.stage == MissionState::Stage::Find);
    CHECK(MissionRules::WorkConsole(state, plan, 0) == Worked::Switched);
    CHECK(state.device == 1);
    CHECK(state.stage == MissionState::Stage::Downloading);
    // The next before the first is up to pressure: tripped.
    CHECK(MissionRules::WorkConsole(state, plan, 1) == Worked::Tripped);
    CHECK(state.progress == 0.0f);
    CHECK(MissionRules::WorkConsole(state, plan, 0) == Worked::Switched);
    // Nobody at it: nothing. Somebody: up to the first's pressure, and no further on it alone.
    MissionRules::Tick(state, plan, 5.0f, false);
    CHECK(state.progress == 0.0f);
    for (int t = 0; t < 200; ++t)
    {
        CHECK_FALSE(MissionRules::Tick(state, plan, 0.25f, true).downloaded);
    }
    CHECK(std::abs(state.progress - 1.0f / 3.0f) < 1e-5f);
    CHECK(MissionRules::WorkConsole(state, plan, 1) == Worked::Switched);
    // Its own switch off: those after it stop, and its pressure is lost.
    CHECK(MissionRules::WorkConsole(state, plan, 1) == Worked::Switched);
    CHECK(state.device == 1);
    CHECK(std::abs(state.progress - 1.0f / 3.0f) < 1e-5f);
    CHECK(MissionRules::WorkConsole(state, plan, 0) == Worked::Switched);
    CHECK(state.device == 0);
    CHECK(state.progress == 0.0f);
    CHECK(state.stage == MissionState::Stage::Find);
    // Done right, it comes up and the station is restarted.
    REQUIRE(RunConsole(state, plan));
    CHECK(state.stage == MissionState::Stage::Carry);
    CHECK(MissionRules::WorkConsole(state, plan, 0) == Worked::Nothing);
}

TEST_CASE("A signal's transmitter is worked up close: its lid, then its two switches, each picked out from where the camera comes to", "[mission][site][interaction]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    InteractionSystem interactions;
    uint16_t seed = 0;
    for (uint16_t base = 1; base < 64 && seed == 0; ++base)
    {
        if (MissionPlan::KindFor(SitePlan::SeedFor(base, SitePlan::SiteKind::Signal)) == MissionPlan::Kind::Signal)
        {
            seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Signal);
        }
    }
    REQUIRE(seed != 0);
    SiteMap site;
    site.Build(seed, scene, meshes, physics, nullptr);
    MissionPlan plan = MissionPlan::Generate(site.Plan(), seed);
    REQUIRE(plan.kind == MissionPlan::Kind::Signal);
    plan.markers.front().y = site.Terrain().Height(plan.markers.front().x, plan.markers.front().z);
    MissionProps props;
    props.Build(scene, meshes, nullptr, physics, interactions, site.Plan(), plan, site.Shuttle());
    physics.OptimizeBroadPhase();
    MissionState state = MissionRules::Start(plan);
    float time = 0.0f;
    const auto show = [&]
    {
        for (int i = 0; i < 30; ++i)
        {
            time += 0.1f;
            props.Show(scene, interactions, nullptr, state, time);
        }
    };
    const auto pickable = [&](int control)
    {
        glm::vec3 eye;
        glm::vec3 target;
        REQUIRE(props.View(MissionProps::Device::Transmitter, eye, target));
        const Interactable* thing = interactions.FindByPayload(InteractionKind::Terminal, control);
        REQUIRE(thing != nullptr);
        CHECK(thing->closeUp);
        const glm::vec3 focus = interactions.FocusPoint(scene, *thing);
        const InteractionSystem::Focus& found = interactions.UpdateFocus(scene, physics, eye, focus - eye, 5.0f, true);
        return found.valid && found.entity == thing->entity;
    };
    show();
    // From outside: only the case, to come up to.
    REQUIRE(interactions.FindByPayload(InteractionKind::Device, static_cast<int>(MissionProps::Device::Transmitter)) != nullptr);
    CHECK(pickable(MissionRules::kCaseLid));
    REQUIRE(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseLid) == MissionRules::Worked::Opened);
    show();
    CHECK(pickable(MissionRules::kCaseSender));
    CHECK(pickable(MissionRules::kCasePower));
    props.Clear(scene, physics, interactions);
    site.Clear(scene, physics, nullptr);
}

TEST_CASE("Facilities and signals each go both ways: a facility's data recovered or its plant started, a signal shut down or brought back", "[mission]")
{
    // From the site's own seed, so a contract offered for it before it is made says what its mission will be.
    int kinds[6] = {};
    for (uint16_t base = 1; base <= 60; ++base)
    {
        const MissionPlan::Kind facility = MissionPlan::KindFor(SitePlan::SeedFor(base, SitePlan::SiteKind::Facility));
        CHECK((facility == MissionPlan::Kind::Download || facility == MissionPlan::Kind::Activate));
        ++kinds[static_cast<int>(facility)];
        const MissionPlan::Kind signal = MissionPlan::KindFor(SitePlan::SeedFor(base, SitePlan::SiteKind::Signal));
        CHECK((signal == MissionPlan::Kind::Signal || signal == MissionPlan::Kind::Revive));
        ++kinds[static_cast<int>(signal)];
        CHECK(MissionPlan::KindFor(SitePlan::SeedFor(base, SitePlan::SiteKind::Station)) == MissionPlan::Kind::Restore);
        CHECK(MissionPlan::KindFor(SitePlan::SeedFor(base, SitePlan::SiteKind::Survey)) == MissionPlan::Kind::Survey);
    }
    // Each about half the time.
    for (const MissionPlan::Kind kind : {MissionPlan::Kind::Download, MissionPlan::Kind::Activate, MissionPlan::Kind::Signal, MissionPlan::Kind::Revive})
    {
        INFO("kind " << static_cast<int>(kind));
        CHECK(kinds[static_cast<int>(kind)] >= 15);
    }
    // And each has its wording.
    Objectives::Library library;
    Paths::Init(nullptr, std::filesystem::path(PRED_SOURCE_DIR) / "Assets");
    REQUIRE(library.LoadFromFile(Paths::AssetsRoot() / "Data" / "missions.json"));
    CHECK(library.Find(Objectives::TemplateFor(MissionPlan::Kind::Activate)) != nullptr);
    CHECK(library.Find(Objectives::TemplateFor(MissionPlan::Kind::Revive)) != nullptr);
}

TEST_CASE("A facility's plant is started at its control console, a system at a time, once its power is back", "[mission]")
{
    int checked = 0;
    for (uint16_t base = 1; base <= 40 && checked < 4; ++base)
    {
        const uint16_t seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Facility);
        if (MissionPlan::KindFor(seed) != MissionPlan::Kind::Activate)
        {
            continue;
        }
        const SitePlan site = SitePlan::Generate(seed);
        MissionPlan plan = MissionPlan::Generate(site, seed);
        INFO("seed " << seed);
        if (plan.cave >= 0)
        {
            CHECK(plan.kind == MissionPlan::Kind::Download); // (down a cave it is the survey station's data)
            continue;
        }
        REQUIRE(plan.kind == MissionPlan::Kind::Activate);
        REQUIRE(plan.Valid());
        ++checked;
        MissionState state = MissionRules::Start(plan);
        if (!state.powered)
        {
            REQUIRE(SolveBoard(state, plan));
        }
        // The systems in turn, each run up before the next: then it is running, and it is back to the shuttle with nothing to carry.
        for (int system = 0; system < MissionRules::kConsoleSystems; ++system)
        {
            REQUIRE(MissionRules::WorkConsole(state, plan, system) == MissionRules::Worked::Switched);
            for (int i = 0; i < 400 && state.stage == MissionState::Stage::Downloading && state.progress < MissionRules::ConsoleCap(state) - 1e-4f; ++i)
            {
                MissionRules::Tick(state, plan, 0.25f, true);
            }
        }
        for (int i = 0; i < 400 && state.stage == MissionState::Stage::Downloading; ++i)
        {
            MissionRules::Tick(state, plan, 0.25f, true);
        }
        CHECK(state.stage == MissionState::Stage::Carry);
    }
    CHECK(checked > 0);
}

TEST_CASE("A signal gone silent is brought back by hand: its lid, its power, then its sender -- not its sender with no power under it", "[mission]")
{
    int checked = 0;
    for (uint16_t base = 1; base <= 40 && checked < 3; ++base)
    {
        const uint16_t seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Signal);
        if (MissionPlan::KindFor(seed) != MissionPlan::Kind::Revive)
        {
            continue;
        }
        const SitePlan site = SitePlan::Generate(seed);
        const MissionPlan plan = MissionPlan::Generate(site, seed);
        INFO("seed " << seed);
        REQUIRE(plan.kind == MissionPlan::Kind::Revive);
        REQUIRE(plan.markers.size() == 1);
        ++checked;
        using MissionRules::Worked;
        MissionState state = MissionRules::Start(plan);
        // Found dead: its power off and its sender off.
        CHECK((state.device & MissionState::kPowerOff) != 0);
        CHECK((state.device & MissionState::kSenderOff) != 0);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseSender) == Worked::Nothing); // (the lid shut)
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseLid) == Worked::Opened);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseSender) == Worked::Nothing); // (no power)
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCasePower) == Worked::Switched);
        CHECK((state.device & MissionState::kPowerOff) == 0);
        CHECK(state.stage == MissionState::Stage::Find);
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCaseSender) == Worked::Done);
        CHECK((state.device & MissionState::kSenderOff) == 0);
        CHECK(state.stage == MissionState::Stage::Carry);
        // And worked again it does nothing more: it is on the air.
        CHECK(MissionRules::WorkTransmitter(state, plan, MissionRules::kCasePower) == Worked::Nothing);
    }
    CHECK(checked > 0);
}
