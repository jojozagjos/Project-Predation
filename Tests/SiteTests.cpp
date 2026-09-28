#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/SiteMap.h"
#include "Game/World/SitePlan.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <string>
#include <vector>

using namespace pred;

// A mission site: a few buildings on open ground, closed in by rock, with a pad to land on. Everybody
// arrives at the pad, and every room of every building has to be somewhere they can walk to from it.

namespace
{

bool Overlap(glm::vec2 aMin, glm::vec2 aMax, glm::vec2 bMin, glm::vec2 bMax)
{
    return aMin.x < bMax.x && aMax.x > bMin.x && aMin.y < bMax.y && aMax.y > bMin.y;
}

} // namespace

TEST_CASE("A site's buildings stand apart on its ground, each with a way in, clear of where everybody lands", "[site]")
{
    for (uint32_t seed = 1; seed <= 24; ++seed)
    {
        const SitePlan plan = SitePlan::Generate(seed);
        INFO("seed " << seed);
        REQUIRE(plan.buildings.size() >= 2);
        const glm::vec2 siteMin{plan.origin.x, plan.origin.z};
        const glm::vec2 siteMax = siteMin + glm::vec2(plan.size);
        const glm::vec2 landing{plan.landing.x, plan.landing.z};
        CHECK(landing.x > siteMin.x + 10.0f);
        CHECK(landing.y > siteMin.y + 10.0f);
        CHECK(landing.x < siteMax.x - 10.0f);
        CHECK(landing.y < siteMax.y - 10.0f);
        CHECK_FALSE(plan.InBuilding(landing, 15.0f));
        for (size_t b = 0; b < plan.buildings.size(); ++b)
        {
            const FacilityLayout& building = plan.buildings[b];
            glm::vec2 min;
            glm::vec2 max;
            SitePlan::Footprint(building, 0.0f, min, max);
            CHECK(min.x > siteMin.x + 20.0f);
            CHECK(min.y > siteMin.y + 20.0f);
            CHECK(max.x < siteMax.x - 20.0f);
            CHECK(max.y < siteMax.y - 20.0f);
            CHECK_FALSE(building.exits.empty());
            for (size_t other = b + 1; other < plan.buildings.size(); ++other)
            {
                glm::vec2 oMin;
                glm::vec2 oMax;
                SitePlan::Footprint(plan.buildings[other], 10.0f, oMin, oMax);
                CHECK_FALSE(Overlap(min, max, oMin, oMax));
            }
            // Every way out is on the edge of the building, facing out of it, and into somewhere open.
            for (const FacilityLayout::Exit& exit : building.exits)
            {
                const bool onEdge = (exit.side == 0 && exit.cell.x == 0) || (exit.side == 1 && exit.cell.x == building.width - 1) ||
                                    (exit.side == 2 && exit.cell.y == 0) || (exit.side == 3 && exit.cell.y == building.depth - 1);
                CHECK(onEdge);
                CHECK(building.Open(0, exit.cell.x, exit.cell.y));
            }
            // And the whole of it can be walked from them.
            const std::vector<bool> reach = building.Reachable(true);
            for (const FacilityLayout::Room& room : building.rooms)
            {
                CHECK(reach[building.Index(room.floor, room.min.x, room.min.y)]);
            }
        }
        // Nothing to get behind stands in a building or on the pad.
        for (const SitePlan::Block& block : plan.blocks)
        {
            if (block.kind == SitePlan::BlockKind::Rock || block.kind == SitePlan::BlockKind::Container ||
                block.kind == SitePlan::BlockKind::Tank)
            {
                const glm::vec2 at{block.centre.x, block.centre.z};
                CHECK_FALSE(plan.InBuilding(at, 1.0f));
                CHECK(glm::distance(at, landing) > 8.0f);
            }
        }
    }
}

TEST_CASE("A site is planned the same from the same seed, on every machine", "[site]")
{
    const SitePlan a = SitePlan::Generate(77);
    const SitePlan b = SitePlan::Generate(77);
    REQUIRE(a.buildings.size() == b.buildings.size());
    REQUIRE(a.blocks.size() == b.blocks.size());
    REQUIRE(a.lamps.size() == b.lamps.size());
    CHECK(a.landing == b.landing);
    for (size_t i = 0; i < a.buildings.size(); ++i)
    {
        CHECK(a.buildings[i].origin == b.buildings[i].origin);
        CHECK(a.buildings[i].cells == b.buildings[i].cells);
    }
    for (size_t i = 0; i < a.blocks.size(); ++i)
    {
        CHECK(a.blocks[i].centre == b.blocks[i].centre);
    }
    // And a different seed is a different site.
    const SitePlan c = SitePlan::Generate(78);
    CHECK((c.landing != a.landing || c.buildings.front().origin != a.buildings.front().origin));
}

TEST_CASE("A building planned with ways out has an outer wall all round, with a door in it at each", "[site][facility]")
{
    FacilityLayout::Options options;
    options.width = 16;
    options.depth = 14;
    options.minFloors = 1;
    options.maxFloors = 2;
    options.exits = 2;
    options.origin = {300.0f, 0.0f, 300.0f};
    const FacilityLayout layout = FacilityLayout::Generate(5, options);
    REQUIRE(layout.exits.size() == 2);
    const FacilityMap::Blueprint blueprint = FacilityMap::Draw(layout);
    // Drawn where it was put.
    for (const FacilityMap::Piece& piece : blueprint.pieces)
    {
        CHECK(piece.centre.x > 299.0f);
        CHECK(piece.centre.z > 299.0f);
    }
    // A door hung in each way out, on the building's edge.
    int onEdge = 0;
    for (const WorldObjects::PlacedDoor& door : blueprint.placements.doors)
    {
        const float x = door.hinge.x - 300.0f;
        const float z = door.hinge.z - 300.0f;
        const float w = static_cast<float>(layout.width) * FacilityLayout::kCell;
        const float d = static_cast<float>(layout.depth) * FacilityLayout::kCell;
        onEdge += (std::abs(x) < 0.01f || std::abs(x - w) < 0.01f || std::abs(z) < 0.01f || std::abs(z - d) < 0.01f) ? 1 : 0;
    }
    CHECK(onEdge == 2);
    // Just outside each, and just inside it, a ray along the ground meets no wall between them.
    // (Only the door itself stands there, and that is not part of the building.)
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 1;
    REQUIRE(physics.Init(settings));
    for (const FacilityMap::Piece& piece : blueprint.pieces)
    {
        Transform transform;
        transform.position = piece.centre;
        physics.CreateBox(piece.size * 0.5f, transform, BodyMotion::Static);
    }
    for (const FacilityLayout::Exit& exit : layout.exits)
    {
        const glm::vec3 outside = FacilityMap::ExitOutside(layout, exit, 1.5f) + glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 inside = FacilityMap::ExitOutside(layout, exit, -3.0f) + glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 along = inside - outside;
        CHECK_FALSE(physics.RayCast(outside, along, glm::length(along)).hit);
        // And beside the doorway, the wall.
        const glm::vec3 sideways = exit.side < 2 ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 beside = outside + sideways * 1.6f;
        CHECK(physics.RayCast(beside, along, glm::length(along)).hit);
    }
}

TEST_CASE("From where everybody lands, every room of every building on a site can be walked to", "[site][navigation]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    SiteMap site;
    site.Build(3, scene, meshes, physics, nullptr);
    NavMesh nav;
    std::string error;
    REQUIRE(nav.Build(physics.StaticTriangles(), NavSettings{}, &error));

    const SitePlan& plan = site.Plan();
    int unreachable = 0;
    int rooms = 0;
    for (size_t b = 0; b < plan.buildings.size(); ++b)
    {
        const FacilityLayout& building = plan.buildings[b];
        for (size_t r = 0; r < building.rooms.size(); ++r)
        {
            const FacilityLayout::Room& room = building.rooms[r];
            ++rooms;
            const glm::vec3 target = FacilityMap::ToWorld(building, room.floor, glm::vec2(room.min) + glm::vec2(0.5f)) + glm::vec3(0.0f, 0.1f, 0.0f);
            std::vector<glm::vec3> corners;
            bool reached = false;
            nav.FindPath(site.Spawn(), target, corners, &reached, nullptr, 0);
            if (!reached || corners.empty() || glm::distance(corners.back(), target) > 1.5f)
            {
                UNSCOPED_INFO("building " << b << " room " << r << " on floor " << room.floor);
                ++unreachable;
            }
        }
    }
    INFO(rooms << " rooms");
    CHECK(rooms > 10);
    CHECK(unreachable == 0);
}
