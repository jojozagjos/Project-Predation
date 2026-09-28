#include "Engine/Net/BitStream.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Mission/Mission.h"
#include "Game/Mission/MissionProps.h"
#include "Game/Net/Protocol.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/Shuttle.h"
#include "Game/World/SiteMap.h"
#include "Game/World/SitePlan.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <cmath>

using namespace pred;

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
        CHECK(plan.downloadSeconds >= 35.0f);
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
    CHECK(MissionRules::RestorePower(state));
    CHECK(state.powered);
    CHECK_FALSE(MissionRules::RestorePower(state));
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
}

TEST_CASE("The shuttle stands on its pad with its ramp down to it, and everybody arrives in its cabin", "[mission][site]")
{
    const SitePlan site = SitePlan::Generate(3);
    const glm::vec3 base = site.ShuttleBase();
    const float yaw = site.landingYaw;
    const glm::vec3 arrival = Shuttle::Arrival(base, yaw);
    CHECK(Shuttle::Aboard(base, yaw, arrival));
    CHECK_FALSE(Shuttle::Aboard(base, yaw, base + glm::vec3(10.0f, 0.7f, 0.0f)));
    CHECK_FALSE(Shuttle::Aboard(base, yaw, base));
    // Down the ramp is the way into the site.
    const glm::vec3 down{std::sin(yaw), 0.0f, -std::cos(yaw)};
    CHECK_FALSE(Shuttle::Aboard(base, yaw, arrival + down * 5.0f));

    // As solid as it is drawn: the deck under the arrival, the ramp surface halfway down it, and all of it on the pad.
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    for (const Shuttle::Piece& piece : Shuttle::Pieces(base, yaw))
    {
        Transform transform;
        transform.position = piece.centre;
        transform.rotation = piece.rotation;
        physics.CreateBox(piece.size * 0.5f, transform, BodyMotion::Static);
        CHECK(glm::distance(glm::vec2(piece.centre.x, piece.centre.z), glm::vec2(base.x, base.z)) < 7.0f);
    }
    const RayHit deck = physics.RayCast(arrival, {0.0f, -1.0f, 0.0f}, 2.0f);
    REQUIRE(deck.hit);
    CHECK(std::abs(deck.position.y - (base.y + 0.7f)) < 0.01f);
    const glm::vec3 halfway = base + down * 3.75f + glm::vec3(0.0f, 2.0f, 0.0f);
    const RayHit ramp = physics.RayCast(halfway, {0.0f, -1.0f, 0.0f}, 3.0f);
    REQUIRE(ramp.hit);
    CHECK(std::abs(ramp.position.y - (base.y + 0.35f)) < 0.02f);
    // And a way out of the cabin: nothing across the open back at head height.
    const glm::vec3 head = arrival + glm::vec3(0.0f, 1.0f, 0.0f);
    CHECK_FALSE(physics.RayCast(head, down, 4.0f).hit);
}

TEST_CASE("What the mission puts in the world -- the terminal, the panels, the console -- is inside nothing", "[mission][site]")
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
        props.Build(scene, meshes, physics, interactions, site.Plan(), plan);
        const std::vector<PhysicsWorld::StaticOverlap> overlaps = physics.FindStaticOverlaps(0.01f);
        for (size_t i = 0; i < overlaps.size() && i < 4; ++i)
        {
            UNSCOPED_INFO("overlap " << overlaps[i].penetration * 1000.0f << " mm at " << overlaps[i].position.x << " "
                                     << overlaps[i].position.y << " " << overlaps[i].position.z);
        }
        CHECK(overlaps.empty());
        // Something to press at the terminal, at every building's panel, and at the console.
        CHECK(interactions.FindByPayload(InteractionKind::Terminal, 0) != nullptr);
        CHECK(interactions.FindByPayload(InteractionKind::Launch, 0) != nullptr);
        for (size_t b = 0; b < site.Plan().buildings.size(); ++b)
        {
            CHECK(interactions.FindByPayload(InteractionKind::Breaker, static_cast<int>(b)) != nullptr);
        }
        props.Clear(scene, physics, interactions);
        site.Clear(scene, physics, nullptr);
    }
}
