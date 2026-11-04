#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Waves.h"
#include "Engine/Scene/Scene.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/FacilityOutside.h"
#include "Game/World/Landmark.h"
#include "Game/World/SiteMap.h"
#include "Game/World/SitePlan.h"
#include "Game/World/SiteScatter.h"
#include "Game/World/SiteStructures.h"
#include "Game/World/Signs.h"
#include "Game/World/SiteTerrain.h"
#include "Game/World/Surfaces.h"
#include "Game/World/Weather.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <sstream>
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
                // (On the edge of its shape, whatever that is: the cell past it on the ground floor no part of the building.)
                const glm::ivec2 out = exit.side == 0 ? glm::ivec2(-1, 0) : exit.side == 1 ? glm::ivec2(1, 0) : exit.side == 2 ? glm::ivec2(0, -1) : glm::ivec2(0, 1);
                const bool onEdge = building.Height(exit.cell.x, exit.cell.y) > building.ground && building.Height(exit.cell.x + out.x, exit.cell.y + out.y) == 0;
                CHECK(onEdge);
                CHECK(building.Open(building.ground, exit.cell.x, exit.cell.y));
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

TEST_CASE("Inside a site's building is within its walls and under its roof; the ground and the roof are not", "[site]")
{
    const SitePlan plan = SitePlan::Generate(1);
    int rooms = 0;
    for (const FacilityLayout& building : plan.buildings)
    {
        for (const FacilityLayout::Room& room : building.rooms)
        {
            const glm::vec3 middle = FacilityMap::ToWorld(building, room.floor, glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f);
            CHECK(plan.Indoors(middle + glm::vec3(0.0f, 0.1f, 0.0f)));
            ++rooms;
        }
        // On the roof is outside.
        const glm::vec3 roof = FacilityMap::ToWorld(building, building.floors, glm::vec2(building.width, building.depth) * 0.5f);
        CHECK_FALSE(plan.Indoors(roof + glm::vec3(0.0f, 0.1f, 0.0f)));
    }
    CHECK(rooms > 10);
    CHECK_FALSE(plan.Indoors(plan.landing));
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

// The game checks every level it builds for solids inside one another, and warns of each. A building's own walls
// and floors are made to reach into each other and are one structure, so say nothing of that; but nothing in it
// may be inside them, or inside anything else.
TEST_CASE("Nothing in a site's buildings is inside anything else, as far as the check the game makes on loading goes", "[site][facility]")
{
    for (const uint32_t seed : {1u, 3u})
    {
        INFO("seed " << seed);
        const SitePlan plan = SitePlan::Generate(seed);
        for (size_t b = 0; b < plan.buildings.size(); ++b)
        {
            INFO("building " << b);
            const FacilityMap::Blueprint blueprint = FacilityMap::Draw(plan.buildings[b]);
            PhysicsWorld physics;
            PhysicsWorld::Settings settings;
            settings.workerThreads = 1;
            REQUIRE(physics.Init(settings));
            const uint32_t structure = physics.NewOverlapGroup();
            std::vector<std::pair<BodyHandle, size_t>> bodies;
            for (size_t p = 0; p < blueprint.pieces.size(); ++p)
            {
                const FacilityMap::Piece& piece = blueprint.pieces[p];
                Transform transform;
                transform.position = piece.centre;
                transform.rotation = glm::angleAxis(piece.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                const BodyHandle body = physics.CreateBox(piece.size * 0.5f, transform, BodyMotion::Static);
                physics.SetOverlapGroup(body, piece.Structural() ? structure : 0);
                bodies.emplace_back(body, p);
            }
            const auto describe = [&](BodyHandle body)
            {
                for (const auto& [handle, p] : bodies)
                {
                    if (handle == body)
                    {
                        const FacilityMap::Piece& piece = blueprint.pieces[p];
                        const glm::vec3 lo = piece.centre - piece.size * 0.5f;
                        const glm::vec3 hi = piece.centre + piece.size * 0.5f;
                        std::ostringstream text;
                        text << "kind " << static_cast<int>(piece.kind) << " from " << lo.x << " " << lo.y << " " << lo.z << " to " << hi.x
                             << " " << hi.y << " " << hi.z;
                        return text.str();
                    }
                }
                return std::string("?");
            };
            const std::vector<PhysicsWorld::StaticOverlap> overlaps = physics.FindStaticOverlaps(0.01f);
            for (size_t i = 0; i < overlaps.size() && i < 4; ++i)
            {
                UNSCOPED_INFO(describe(overlaps[i].a) << " into " << describe(overlaps[i].b) << ", " << overlaps[i].penetration * 1000.0f << " mm");
            }
            CHECK(overlaps.empty());
        }
    }
}

TEST_CASE("From where everybody lands, every room of every building on a site can be walked to", "[site][navigation]")
{
    // On the flattest ground and the roughest.
    const SiteTerrain::Shape shape = GENERATE(SiteTerrain::Shape::Flat, SiteTerrain::Shape::Mountainous, SiteTerrain::Shape::Canyons);
    INFO("shape " << static_cast<int>(shape));
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    SiteMap site;
    SiteMap::Look look;
    look.terrain = shape;
    site.SetLook(look);
    site.Build(3, scene, meshes, physics, nullptr);
    NavMesh nav;
    std::string error;
    const bool built = nav.Build(physics.StaticTriangles(), NavSettings{}, &error);
    INFO(error);
    REQUIRE(built);

    const SitePlan& plan = site.Plan();
    int unreachable = 0;
    int rooms = 0;
    std::string lost;
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
                lost += "building " + std::to_string(b) + " room " + std::to_string(r) + " on floor " + std::to_string(room.floor) + "; ";
                ++unreachable;
            }
        }
    }
    INFO(rooms << " rooms; out of reach: " << lost);
    CHECK(rooms > 10);
    CHECK(unreachable == 0);
}


TEST_CASE("A building set into a hillside: the land risen behind it, the bank against its wall to stand on, no way out or window under it", "[site][hillside]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    SiteMap site;
    SiteMap::Look look;
    look.terrain = SiteTerrain::Shape::Rolling;
    look.biome = "temperate";
    look.weather = "rain";
    site.SetLook(look);
    site.Build(3, scene, meshes, physics, nullptr);
    const SitePlan& plan = site.Plan();
    const SiteTerrain& terrain = site.Terrain();
    int banked = 0;
    for (const FacilityLayout& building : plan.buildings)
    {
        const FacilityLayout::Bank& bank = building.bank;
        if (bank.side < 0)
        {
            continue;
        }
        ++banked;
        INFO("building at " << building.origin.x << " " << building.origin.z << ", banked on side " << bank.side);
        for (const FacilityLayout::Exit& exit : building.exits)
        {
            CHECK(exit.side != bank.side);
        }
        for (const FacilityOutside::Window& window : FacilityOutside::Windows(building))
        {
            CHECK_FALSE((window.side == bank.side && window.floor == building.ground));
        }
        glm::vec2 min;
        glm::vec2 max;
        SitePlan::Footprint(building, 0.0f, min, max);
        const glm::vec2 out = bank.side == 0 ? glm::vec2(-1.0f, 0.0f) : bank.side == 1 ? glm::vec2(1.0f, 0.0f) : bank.side == 2 ? glm::vec2(0.0f, -1.0f)
                                                                                                                                  : glm::vec2(0.0f, 1.0f);
        const glm::vec2 along = glm::vec2(out.y, out.x);
        // Along the middle of the wall it is banked against: the land kept down by it, risen further back; the bank on it to stand on.
        const glm::vec2 wall = (min + max) * 0.5f + out * glm::abs(glm::dot(max - min, out)) * 0.5f;
        const float floor = building.GroundLevel();
        for (const float offset : {-6.0f, 0.0f, 6.0f})
        {
            const glm::vec2 at = wall + along * offset;
            CHECK(terrain.Height(at.x + out.x, at.y + out.y) < floor + 0.1f);
            CHECK(terrain.Height(at.x + out.x * 14.0f, at.y + out.y * 14.0f) > floor + bank.height * 0.6f);
            const glm::vec2 on = at + out * 1.5f;
            const RayHit hit = physics.RayCastStatic({on.x, floor + 20.0f, on.y}, {0.0f, -1.0f, 0.0f}, 40.0f);
            REQUIRE(hit.hit);
            CHECK(hit.position.y > floor + bank.height * 0.8f);
        }
        // And nothing of the bank inside it, all of it within reach of the walls.
        const MeshData mesh = terrain.Bank(building, look.ground, look.rock);
        REQUIRE_FALSE(mesh.indices.empty());
        for (const uint32_t index : mesh.indices)
        {
            const glm::vec3& p = mesh.vertices[index].position;
            CHECK_FALSE((p.x > min.x + 0.01f && p.x < max.x - 0.01f && p.z > min.y + 0.01f && p.z < max.y - 0.01f));
        }
    }
    CHECK(banked > 0);
}

TEST_CASE("A building left to the cold on a frozen world is snowed under: deep drifts up its walls to stand on, a way down to each door", "[site][snow]")
{
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    SiteMap site;
    SiteMap::Look look;
    look.biome = "frozen";
    site.SetLook(look);
    site.Build(3, scene, meshes, physics, nullptr);
    const SitePlan& plan = site.Plan();
    int under = 0;
    for (const FacilityLayout& building : plan.buildings)
    {
        const std::array<float, 4>& drifted = building.outside.drifted;
        if (drifted[0] <= 0.0f)
        {
            continue;
        }
        ++under;
        CHECK(building.frozen);
        INFO("building at " << building.origin.x << " " << building.origin.z);
        // Against the side it is deepest on, along the middle of every stretch of wall there well away from a door: standing on snow
        // a good way up it.
        const int side = static_cast<int>(std::max_element(drifted.begin(), drifted.end()) - drifted.begin());
        const float floor = building.GroundLevel();
        int tried = 0;
        int deep = 0;
        for (const FacilityLayout::OuterWall& wall : building.OuterWalls())
        {
            if (wall.side != side || wall.low != 0 || wall.to - wall.from < 2)
            {
                continue;
            }
            const float line = static_cast<float>(wall.line) * FacilityLayout::kCell;
            const float mid = (static_cast<float>(wall.from + wall.to) * 0.5f) * FacilityLayout::kCell;
            const glm::vec2 out = side == 0 ? glm::vec2(-1.0f, 0.0f) : side == 1 ? glm::vec2(1.0f, 0.0f) : side == 2 ? glm::vec2(0.0f, -1.0f) : glm::vec2(0.0f, 1.0f);
            const glm::vec2 on = glm::vec2(building.origin.x, building.origin.z) + (side < 2 ? glm::vec2(line, mid) : glm::vec2(mid, line)) + out * 0.8f;
            bool byDoor = false;
            for (const FacilityLayout::Exit& exit : building.exits)
            {
                const glm::vec3 door = FacilityMap::ExitOutside(building, exit, 0.5f);
                byDoor = byDoor || glm::distance(glm::vec2(door.x, door.z), on) < 6.0f;
            }
            if (byDoor)
            {
                continue;
            }
            ++tried;
            const RayHit hit = physics.RayCastStatic({on.x, floor + 10.0f, on.y}, {0.0f, -1.0f, 0.0f}, 20.0f);
            REQUIRE(hit.hit);
            deep += hit.position.y > floor + 1.0f ? 1 : 0;
        }
        CHECK(deep * 2 >= tried);
        // And at every door, down at the ground: the snow kept out of it.
        for (const FacilityLayout::Exit& exit : building.exits)
        {
            const glm::vec3 door = FacilityMap::ExitOutside(building, exit, 0.5f);
            const RayHit hit = physics.RayCastStatic(door + glm::vec3(0.0f, 1.5f, 0.0f), {0.0f, -1.0f, 0.0f}, 4.0f);
            REQUIRE(hit.hit);
            CHECK(hit.position.y < floor + 0.2f);
        }
    }
    CHECK(under > 0);
}

TEST_CASE("A building left long ago or burnt is now and then half fallen in: a corner of it a ruin, the rest standing whole", "[site][ruin]")
{
    int ruins = 0;
    for (uint16_t seed = 1; seed <= 12; ++seed)
    {
        const SitePlan plan = SitePlan::Generate(seed);
        for (size_t b = 0; b < plan.buildings.size(); ++b)
        {
            const FacilityLayout& building = plan.buildings[b];
            const FacilityLayout::Ruin& ruin = building.ruin;
            if (!ruin.Has())
            {
                continue;
            }
            ++ruins;
            INFO("seed " << seed << " building " << b);
            CHECK(b > 0);
            CHECK((building.condition == FacilityLayout::Condition::Abandoned || building.condition == FacilityLayout::Condition::Burnt));
            // What fell is no part of what stands, and most of it stands.
            int fell = 0;
            int stands = 0;
            for (int z = 0; z < building.depth; ++z)
            {
                for (int x = 0; x < building.width; ++x)
                {
                    fell += ruin.Height(x, z) > 0 ? 1 : 0;
                    stands += building.Height(x, z) > 0 ? 1 : 0;
                    CHECK_FALSE((ruin.Height(x, z) > 0 && building.Height(x, z) > 0));
                }
            }
            CHECK(fell >= 6);
            CHECK(stands >= fell * 2);
            // Every room of what stands reached from the way in.
            const std::vector<bool> reach = building.Reachable(true);
            for (const FacilityLayout::Room& room : building.rooms)
            {
                CHECK(reach[building.Index(room.floor, room.min.x, room.min.y)]);
            }
            // No window in the wall left standing where it broke.
            for (const FacilityOutside::Window& window : FacilityOutside::Windows(building))
            {
                const int past = window.side == 0 || window.side == 2 ? window.line - 1 : window.line;
                CHECK((window.side < 2 ? ruin.Height(past, window.bay) : ruin.Height(window.bay, past)) == 0);
            }
            // Its ruin drawn (FacilityRuin), as broken concrete is, in shapes: a floor over every cell of it, felt; the stumps of its walls
            // standing on its outline, felt, their tops at heights all along -- not one height, not steps of one; rubble heaped, felt.
            const FacilityMap::Blueprint blueprint = FacilityMap::Draw(building);
            float floorArea = 0.0f;
            std::vector<float> tops;
            bool heaped = false;
            for (const FacilityMap::Blueprint::Shape& shape : blueprint.shapes)
            {
                const MeshData& mesh = shape.mesh;
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                {
                    const glm::vec3& a = mesh.vertices[mesh.indices[i]].position;
                    const glm::vec3& q = mesh.vertices[mesh.indices[i + 1]].position;
                    const glm::vec3& r = mesh.vertices[mesh.indices[i + 2]].position;
                    // (Within the building's ground and a little: the ruin, and the damage carried into what stands.)
                    CHECK(a.x > building.origin.x - 4.0f);
                    CHECK(a.x < building.origin.x + static_cast<float>(building.width) * FacilityLayout::kCell + 4.0f);
                    CHECK(a.z > building.origin.z - 4.0f);
                    CHECK(a.z < building.origin.z + static_cast<float>(building.depth) * FacilityLayout::kCell + 4.0f);
                    if (shape.kind == FacilityMap::Piece::Kind::Floor && shape.solid && std::abs(a.y - building.GroundLevel()) < 0.01f &&
                        std::abs(q.y - a.y) < 0.01f && std::abs(r.y - a.y) < 0.01f)
                    {
                        floorArea += glm::length(glm::cross(q - a, r - a)) * 0.5f;
                    }
                    if (shape.kind == FacilityMap::Piece::Kind::Wall)
                    {
                        CHECK(shape.solid);
                        tops.push_back(std::max({a.y, q.y, r.y}));
                    }
                    heaped = heaped || (shape.kind == FacilityMap::Piece::Kind::RockFloor && shape.solid);
                }
            }
            CHECK(floorArea >= static_cast<float>(fell) * FacilityLayout::kCell * FacilityLayout::kCell - 0.1f);
            REQUIRE(tops.size() >= 20);
            std::sort(tops.begin(), tops.end());
            tops.erase(std::unique(tops.begin(), tops.end(), [](float p, float q) { return std::abs(p - q) < 0.02f; }), tops.end());
            CHECK(tops.size() >= 20);
            CHECK(heaped);
        }
    }
    CHECK(ruins > 0);
}

TEST_CASE("Print which seeds' sites have which structures round them, and where", "[.sitestructures]")
{
    std::string text;
    std::vector<uint16_t> seeds;
    for (uint16_t base = 1; base <= 12; ++base)
    {
        for (const SitePlan::SiteKind kind : {SitePlan::SiteKind::Facility, SitePlan::SiteKind::Station, SitePlan::SiteKind::Survey, SitePlan::SiteKind::Signal})
        {
            seeds.push_back(SitePlan::SeedFor(base, kind));
        }
    }
    for (const uint16_t seed : seeds)
    {
        const SitePlan plan = SitePlan::Generate(seed);
        const bool bunker = !plan.buildings.empty() && plan.buildings[0].ground > 0 &&
                            std::count(plan.buildings[0].heights.begin(), plan.buildings[0].heights.end(), plan.buildings[0].ground) > 0;
        text += "seed " + std::to_string(seed) + " is a " + SitePlan::TypeName(plan.type) + (bunker ? " (bunker)" : "") + "\n";
        for (const SitePlan::Block& block : plan.blocks)
        {
            if (SiteStructures::Is(block.kind))
            {
                text += "seed " + std::to_string(seed) + " kind " + std::to_string(static_cast<int>(block.kind)) + " at " + std::to_string(static_cast<int>(block.centre.x)) + " " +
                        std::to_string(static_cast<int>(block.centre.y)) + " " + std::to_string(static_cast<int>(block.centre.z)) + " yaw " + std::to_string(block.yaw) + "\n";
            }
        }
    }
    WARN(text);
}
TEST_CASE("Print what is drawn near a point of a site's building", "[.near]")
{
    const char* seedText = std::getenv("PRED_NEAR_SEED");
    const char* atText = std::getenv("PRED_NEAR_AT");
    const SitePlan plan = SitePlan::Generate(seedText != nullptr ? static_cast<uint32_t>(std::atoi(seedText)) : 1u);
    glm::vec3 at{0.0f};
    if (atText != nullptr)
    {
        std::sscanf(atText, "%f %f %f", &at.x, &at.y, &at.z);
    }
    std::string text;
    for (size_t b = 0; b < plan.buildings.size(); ++b)
    {
        FacilityLayout building = plan.buildings[b];
        building.frozen = std::getenv("PRED_NEAR_FROZEN") != nullptr;
        const FacilityMap::Blueprint blueprint = FacilityMap::Draw(building);
        const auto list = [&](const std::vector<FacilityMap::Dressing>& all, const char* what)
        {
            for (const FacilityMap::Dressing& d : all)
            {
                if (glm::distance(d.centre, at) < 1.2f)
                {
                    text += std::string(what) + " b" + std::to_string(b) + " form " + std::to_string(static_cast<int>(d.form)) + " kind " + std::to_string(static_cast<int>(d.kind)) +
                            " at " + std::to_string(d.centre.x) + " " + std::to_string(d.centre.y) + " " + std::to_string(d.centre.z) + " size " + std::to_string(d.size.x) + " " +
                            std::to_string(d.size.y) + " " + std::to_string(d.size.z) + "\n";
                }
            }
        };
        list(blueprint.dressing, "dressing");
        list(blueprint.furnishing, "furnishing");
        for (const FacilityMap::Lamp& lamp : blueprint.lamps)
        {
            if (glm::distance(glm::vec2(lamp.position.x, lamp.position.z), glm::vec2(at.x, at.z)) < 2.0f)
            {
                text += "lamp b" + std::to_string(b) + " kind " + std::to_string(static_cast<int>(lamp.kind)) + " at " + std::to_string(lamp.position.x) + " " + std::to_string(lamp.position.y) + " " + std::to_string(lamp.position.z) + " spill " + std::to_string(lamp.spillOf) + "\n";
            }
        }
        for (const FacilityMap::Piece& p : blueprint.pieces)
        {
            if (glm::distance(p.centre, at) < 1.2f)
            {
                text += "piece b" + std::to_string(b) + " kind " + std::to_string(static_cast<int>(p.kind)) + " at " + std::to_string(p.centre.x) + " " +
                        std::to_string(p.centre.y) + " " + std::to_string(p.centre.z) + "\n";
            }
        }
    }
    WARN(text);
}
TEST_CASE("Each kind of place plans what that kind has, and keeps its kind through its seed", "[site]")
{
    using SiteKind = SitePlan::SiteKind;
    for (const SiteKind kind : {SiteKind::Facility, SiteKind::Station, SiteKind::Survey, SiteKind::Wreck, SiteKind::Signal})
    {
        for (uint16_t base = 1; base <= 12; ++base)
        {
            const uint16_t seed = SitePlan::SeedFor(base, kind);
            INFO("kind " << static_cast<int>(kind) << " seed " << seed);
            CHECK(SitePlan::KindOf(seed) == kind);
            const SitePlan plan = SitePlan::Generate(seed);
            CHECK(plan.kind == kind);
            // Always somewhere to go in -- a mission's business is in the first building.
            REQUIRE(!plan.buildings.empty());
            const auto count = [&](SitePlan::BlockKind block)
            {
                return std::count_if(plan.blocks.begin(), plan.blocks.end(), [&](const SitePlan::Block& b) { return b.kind == block; });
            };
            switch (kind)
            {
            case SiteKind::Facility: CHECK(plan.buildings.size() >= 2); break;
            case SiteKind::Survey: CHECK(plan.buildings.size() == 1); break;
            case SiteKind::Wreck: CHECK(count(SitePlan::BlockKind::Debris) >= 8); break;
            case SiteKind::Station:
            case SiteKind::Signal:
                // (Its own, as well as the relay mast out in a valley.)
                CHECK(std::count_if(plan.blocks.begin(), plan.blocks.end(), [](const SitePlan::Block& b) { return b.kind == SitePlan::BlockKind::Mast && !b.outlying; }) == 1);
                break;
            }
            CHECK((kind == SiteKind::Wreck) == (count(SitePlan::BlockKind::Debris) > 0));
        }
    }
    // A seed of an old campaign, from before kinds, is a facility.
    CHECK(SitePlan::KindOf(1) == SiteKind::Facility);
}

TEST_CASE("A stream or a river runs in from far off, downhill across the world's own ground, and leaves what is levelled level", "[site][terrain][stream]")
{
    using Shape = SiteTerrain::Shape;
    int streams = 0;
    for (const Shape shape : {Shape::Rolling, Shape::Flat, Shape::Mountainous})
    {
        for (uint16_t base = 1; base <= 4; ++base)
        {
            const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(base, SitePlan::SiteKind::Facility));
            INFO("shape " << static_cast<int>(shape) << " seed " << plan.seed);
            SiteTerrain terrain;
            terrain.Plan(plan, shape, false, SiteTerrain::Water::None, true);
            // Its ground all there, wherever it is.
            for (float z = plan.origin.z - 60.0f; z < plan.origin.z + plan.size + 60.0f; z += 5.0f)
            {
                for (float x = plan.origin.x - 60.0f; x < plan.origin.x + plan.size + 60.0f; x += 5.0f)
                {
                    REQUIRE(std::isfinite(terrain.Height(x, z)));
                }
            }
            // The buildings' ground still level.
            for (const FacilityLayout& building : plan.buildings)
            {
                glm::vec2 min;
                glm::vec2 max;
                SitePlan::Footprint(building, 2.0f, min, max);
                // (Over a bunker's floors, where nothing of it stands, its earth over it.)
                const glm::vec2 middle = (min + max) * 0.5f;
                const glm::vec2 cell = (middle - glm::vec2(building.origin.x, building.origin.z)) / FacilityLayout::kCell;
                const bool earth = building.ground > 0 && building.Height(static_cast<int>(std::floor(cell.x)), static_cast<int>(std::floor(cell.y))) == building.ground;
                CHECK(std::abs(terrain.Height(middle.x, middle.y) - terrain.Base()) < (earth ? 0.31f : 0.01f));
            }
            for (const SiteTerrain::Stream& stream : terrain.Streams())
            {
                ++streams;
                REQUIRE(stream.line.size() >= 2);
                for (size_t k = 0; k < stream.line.size(); ++k)
                {
                    // Falling as it goes, never rising.
                    if (k > 0)
                    {
                        CHECK(stream.surface[k] <= stream.surface[k - 1] + 1.0e-4f);
                    }
                    // Water in its middle, as deep as it is (knee-deep, or a river to swim) and no deeper, where it runs
                    // over the ground, not where it falls (a cliff's face is not followed so closely by the ground's grid).
                    const glm::vec2 at = stream.line[k];
                    const float depth = terrain.WaterDepth(at.x, at.y);
                    const float before = stream.surface[k > 1 ? k - 2 : 0];
                    const float after = stream.surface[std::min(k + 2, stream.line.size() - 1)];
                    if (before - after < 1.0f && terrain.PastRoam(at.x, at.y) < 0.0f)
                    {
                        INFO("point " << k << " of " << stream.line.size());
                        CHECK(depth > 0.05f);
                        CHECK(depth < stream.water + 0.7f);
                    }
                    CHECK(std::isfinite(stream.surface[k]));
                }
            }
            // Far from it, dry.
            CHECK(terrain.WaterDepth(plan.origin.x + plan.size * 0.5f, plan.origin.z + plan.size * 0.5f) < 0.0f);
        }
    }
    CHECK(streams > 3);
}

TEST_CASE("A site's ground is level where people built, the world's own shape elsewhere, and open land round it to the horizon", "[site][terrain]")
{
    using Shape = SiteTerrain::Shape;
    using SiteKind = SitePlan::SiteKind;
    for (const Shape shape : {Shape::Flat, Shape::Rolling, Shape::Mountainous, Shape::Canyons, Shape::Cratered, Shape::Mesas, Shape::Pillars, Shape::Crystals})
    {
        for (const SiteKind kind : {SiteKind::Facility, SiteKind::Station, SiteKind::Survey, SiteKind::Wreck, SiteKind::Signal})
        {
            for (uint16_t base = 1; base <= 3; ++base)
            {
                const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(base, kind));
                INFO("shape " << static_cast<int>(shape) << " kind " << static_cast<int>(kind) << " seed " << plan.seed);
                SiteTerrain terrain;
                terrain.Plan(plan, shape, shape == Shape::Flat);
                const auto level = [&](float x, float z) { return std::abs(terrain.Height(x, z) - terrain.Base()) < 0.01f; };

                // Under every building and round it, and at every way in.
                for (const FacilityLayout& building : plan.buildings)
                {
                    glm::vec2 min;
                    glm::vec2 max;
                    SitePlan::Footprint(building, 2.0f, min, max);
                    for (float u = 0.0f; u <= 1.0f; u += 0.25f)
                    {
                        CHECK(level(glm::mix(min.x, max.x, u), min.y));
                        CHECK(level(glm::mix(min.x, max.x, u), max.y));
                        CHECK(level(min.x, glm::mix(min.y, max.y, u)));
                        CHECK(level(max.x, glm::mix(min.y, max.y, u)));
                    }
                    for (const FacilityLayout::Exit& exit : building.exits)
                    {
                        const glm::vec3 door = FacilityMap::ExitOutside(building, exit, 3.0f);
                        CHECK(level(door.x, door.z));
                    }
                }
                // The pad, all of it, and where everybody stands off the ramp.
                for (const glm::vec2 corner : {glm::vec2(0.0f), glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f)})
                {
                    CHECK(level(plan.landing.x + corner.x * 8.0f, plan.landing.z + corner.y * 8.0f));
                }
                CHECK(level(plan.rampFoot.position.x, plan.rampFoot.position.z));

                // The edge of where anybody may go is the land itself: going straight out from the site, most ways meet ground
                // too steep to climb (a cliff, or a ravine's side) by about kRoam -- but not every way, no ring of rock -- and
                // the ground short of it open.
                {
                    int blocked = 0;
                    int ways = 0;
                    int openNear = 0;
                    int near = 0;
                    for (float t = 5.0f; t < plan.size; t += 10.0f)
                    {
                        const glm::vec2 starts[4] = {{plan.origin.x, plan.origin.z + t},
                                                     {plan.origin.x + plan.size, plan.origin.z + t},
                                                     {plan.origin.x + t, plan.origin.z},
                                                     {plan.origin.x + t, plan.origin.z + plan.size}};
                        const glm::vec2 outs[4] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}};
                        for (int side = 0; side < 4; ++side)
                        {
                            bool stopped = false;
                            // (Out past where the edge wanders in and out, sixteen metres either way.)
                            for (float d = 12.0f; d <= SiteTerrain::kRoam + 24.0f; d += 1.0f)
                            {
                                const glm::vec2 at = starts[side] + outs[side] * d;
                                stopped = stopped || terrain.Normal(at.x, at.y).y < 0.6f;
                            }
                            blocked += stopped ? 1 : 0;
                            ++ways;
                            const glm::vec2 inside = starts[side] + outs[side] * 8.0f;
                            openNear += terrain.Normal(inside.x, inside.y).y > SiteTerrain::kSteepCosine ? 1 : 0;
                            ++near;
                        }
                    }
                    CHECK(blocked * 20 > ways * 11);
                    CHECK(blocked < ways);
                    CHECK(openNear * 10 > near * 8);
                }
                // And the land round it out to the horizon, every way.
                {
                    const MeshData far = terrain.FarMesh(glm::vec3(0.5f), glm::vec3(0.3f));
                    REQUIRE_FALSE(far.indices.empty());
                    glm::vec2 reach{0.0f};
                    const glm::vec2 middle{plan.origin.x + plan.size * 0.5f, plan.origin.z + plan.size * 0.5f};
                    for (const MeshVertex& vertex : far.vertices)
                    {
                        reach = glm::max(reach, glm::abs(glm::vec2(vertex.position.x, vertex.position.z) - middle));
                    }
                    CHECK(reach.x >= SiteTerrain::kFar - 1.0f);
                    CHECK(reach.y >= SiteTerrain::kFar - 1.0f);
                }

                // Every cell drawn, once -- but those left out under buildings with basements.
                size_t triangles = 0;
                for (int cz = 0; cz < terrain.Chunks(); ++cz)
                {
                    for (int cx = 0; cx < terrain.Chunks(); ++cx)
                    {
                        triangles += terrain.Mesh(cx, cz, glm::vec3(0.5f), glm::vec3(0.3f)).indices.size() / 3;
                    }
                }
                const size_t cells = static_cast<size_t>(std::lround(terrain.Extent() / SiteTerrain::kCell));
                size_t leftOut = 0;
                for (int j = 0; j < static_cast<int>(cells); ++j)
                {
                    for (int i = 0; i < static_cast<int>(cells); ++i)
                    {
                        leftOut += terrain.LeftOut(i, j) ? 1 : 0;
                    }
                }
                CHECK(triangles == (cells * cells - leftOut) * 2);

                // And the same everywhere from the same seed.
                SiteTerrain again;
                again.Plan(plan, shape, shape == Shape::Flat);
                for (float x = 0.0f; x < plan.size; x += 37.0f)
                {
                    CHECK(again.Height(plan.origin.x + x, plan.origin.z + x * 0.7f) == terrain.Height(plan.origin.x + x, plan.origin.z + x * 0.7f));
                }
            }
        }
    }
}

TEST_CASE("Not every world's ground is flat: each shape rises and falls as it should", "[site][terrain]")
{
    using Shape = SiteTerrain::Shape;
    const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(5, SitePlan::SiteKind::Survey));
    const auto range = [&](Shape shape, bool dunes)
    {
        SiteTerrain terrain;
        terrain.Plan(plan, shape, dunes);
        float low = 1.0e9f;
        float high = -1.0e9f;
        // The open ground, clear of the rock rising round it (and its rounded corners).
        for (float z = 40.0f; z < plan.size - 40.0f; z += 4.0f)
        {
            for (float x = 40.0f; x < plan.size - 40.0f; x += 4.0f)
            {
                const float h = terrain.Height(plan.origin.x + x, plan.origin.z + z);
                low = std::min(low, h);
                high = std::max(high, h);
            }
        }
        return high - low;
    };
    CHECK(range(Shape::Flat, false) < 3.0f);
    CHECK(range(Shape::Flat, true) > 3.0f);
    CHECK(range(Shape::Rolling, false) > 5.0f);
    CHECK(range(Shape::Mountainous, false) > 12.0f);
    CHECK(range(Shape::Canyons, false) > 7.0f);
    CHECK(range(Shape::Cratered, false) > 2.5f);
    CHECK(range(Shape::Mesas, false) > 3.0f);
    CHECK(SiteTerrain::ShapeOf("mesas") == Shape::Mesas);
    CHECK(SiteTerrain::ShapeOf("pillars") == Shape::Pillars);
    CHECK(SiteTerrain::ShapeOf("crystalfields") == Shape::Crystals);
    CHECK(SiteTerrain::ShapeOf("mountainous") == Shape::Mountainous);
    CHECK(SiteTerrain::ShapeOf("broken") == Shape::Mountainous);
    CHECK(SiteTerrain::ShapeOf("dunes") == Shape::Flat);
    CHECK(SiteTerrain::ShapeOf("anything") == Shape::Flat);
}

// Not run with the rest: writes each shape of ground, shaded from above, to terrain_<shape>.ppm for looking at.
TEST_CASE("Draw each shape of a site's ground from above", "[.terrain_pictures]")
{
    using Shape = SiteTerrain::Shape;
    const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(3, SitePlan::SiteKind::Facility));
    const char* names[] = {"flat", "rolling", "mountainous", "canyons", "cratered", "dunes"};
    for (int s = 0; s < 6; ++s)
    {
        SiteTerrain terrain;
        terrain.Plan(plan, s == 5 ? Shape::Flat : static_cast<Shape>(s), s == 5);
        const int size = 480;
        std::ofstream out(std::string("terrain_") + names[s] + ".ppm", std::ios::binary);
        out << "P6\n" << size << " " << size << "\n255\n";
        const glm::vec3 light = glm::normalize(glm::vec3(-0.5f, 0.8f, -0.3f));
        for (int j = 0; j < size; ++j)
        {
            for (int i = 0; i < size; ++i)
            {
                const float x = terrain.Min().x + terrain.Extent() * (static_cast<float>(i) + 0.5f) / static_cast<float>(size);
                const float z = terrain.Min().y + terrain.Extent() * (static_cast<float>(j) + 0.5f) / static_cast<float>(size);
                const glm::vec3 n = terrain.Normal(x, z);
                const float shade = 0.25f + 0.75f * std::max(glm::dot(n, light), 0.0f);
                const float h = std::clamp((terrain.Height(x, z) - terrain.Base() + 15.0f) / 75.0f, 0.0f, 1.0f);
                const float rock = SiteTerrain::Rockiness(n);
                const bool built = terrain.Wildness(x, z) < 0.01f;
                glm::vec3 c = glm::mix(glm::mix(glm::vec3(0.3f, 0.55f, 0.35f), glm::vec3(0.95f), h), glm::vec3(0.55f, 0.45f, 0.4f), rock);
                if (built)
                {
                    c = glm::vec3(0.6f, 0.6f, 0.75f);
                }
                c *= shade;
                const char px[3] = {static_cast<char>(c.r * 255.0f), static_cast<char>(c.g * 255.0f), static_cast<char>(c.b * 255.0f)};
                out.write(px, 3);
            }
        }
    }
}

TEST_CASE("What a step sounds like comes from what the ground is laid with", "[site][sound]")
{
    const auto step = [](const std::string& set, bool metal) { return std::string(Surfaces::StepName(Surfaces::StepOfSet(set, metal))); };
    CHECK(step("snow_02", false) == "snow");
    CHECK(step("regolith", false) == "gravel");
    CHECK(step("rock_cliff", false) == "stone");
    CHECK(step("deck_plate", false) == "metal");
    CHECK(step("concrete_slab", false) == "concrete");
    CHECK(step("grass_dirt", false) == "grass");
    CHECK(step("building_cladding", false) == "metal");
    // Nothing laid on it: metal if it is metal, concrete if not.
    CHECK(step("", true) == "metal");
    CHECK(step("", false) == "concrete");
    CHECK(Surfaces::StepName(0) == nullptr);
}

TEST_CASE("A site's weather comes and goes in spells, smoothly, the same for everyone", "[site][weather]")
{
    for (const uint32_t seed : {1u, 77u, 4096u, 123456u})
    {
        float least = 10.0f;
        float most = 0.0f;
        float last = Weather::SpellAt(seed, 0.0);
        for (double clock = 1.0; clock < 4.0 * 3600.0; clock += 1.0)
        {
            const float now = Weather::SpellAt(seed, clock);
            INFO("seed " << seed << " at " << clock);
            // Never switched on or off: a second's change is small.
            CHECK(std::abs(now - last) < 0.02f);
            // The same moment, the same weather (every player's clock is the campaign's).
            CHECK(now == Weather::SpellAt(seed, clock));
            least = std::min(least, now);
            most = std::max(most, now);
            last = now;
        }
        // Over a few hours: a lull with nothing coming down, and a spell past the usual.
        CHECK(least < 0.02f);
        CHECK(most > 1.0f);
        CHECK(most <= 1.31f);
    }
}

TEST_CASE("The ground's rock outcrops are few, on rises, never on the dead flat", "[site][surfaces]")
{
    int rises = 0;
    int samples = 0;
    for (float z = -500.0f; z < 500.0f; z += 2.5f)
    {
        for (float x = -500.0f; x < 500.0f; x += 2.5f)
        {
            CHECK_FALSE(Surfaces::OutcropAt(x, z, 1.0f));
            rises += Surfaces::OutcropAt(x, z, 0.97f) ? 1 : 0;
            ++samples;
        }
    }
    const float share = static_cast<float>(rises) / static_cast<float>(samples);
    INFO("share of a gentle rise that is rock: " << share);
    CHECK(share > 0.005f);
    CHECK(share < 0.15f);
}

TEST_CASE("A coast or an island has the sea past it, and nothing built is ever in the water", "[site][terrain][water]")
{
    using Water = SiteTerrain::Water;
    for (const Water water : {Water::Coast, Water::Island, Water::Lakes})
    {
        for (uint16_t base = 1; base <= 4; ++base)
        {
            const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(base, SitePlan::SiteKind::Facility));
            INFO("water " << static_cast<int>(water) << " seed " << plan.seed);
            SiteTerrain terrain;
            terrain.Plan(plan, SiteTerrain::Shape::Rolling, false, water);
            // (Lakes lie only at some places.)
            if (water == Water::Lakes && !terrain.HasWater())
            {
                continue;
            }
            REQUIRE(terrain.HasWater());
            // Dry where people built: the buildings, the pad.
            for (const FacilityLayout& building : plan.buildings)
            {
                glm::vec2 min;
                glm::vec2 max;
                SitePlan::Footprint(building, 2.0f, min, max);
                CHECK(terrain.WaterDepth((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f) < 0.0f);
            }
            CHECK(terrain.WaterDepth(plan.landing.x, plan.landing.z) < 0.0f);
            // And at a coast or round an island, the sea out there: water in its mesh, deep somewhere.
            if (water != Water::Lakes)
            {
                const MeshData surface = terrain.WaterMesh();
                REQUIRE_FALSE(surface.indices.empty());
                float deepest = 0.0f;
                for (const MeshVertex& vertex : surface.vertices)
                {
                    deepest = std::max(deepest, vertex.uv.x);
                }
                CHECK(deepest > 10.0f);
            }
        }
    }
    // No water, no surface.
    SiteTerrain dry;
    dry.Plan(SitePlan::Generate(1), SiteTerrain::Shape::Flat, false);
    CHECK_FALSE(dry.HasWater());
    CHECK(dry.WaterMesh().indices.empty());
}

TEST_CASE("The ground is two surfaces in patches, and sand along the water", "[site][surfaces]")
{
    // None asked for, none anywhere.
    CHECK_FALSE(Surfaces::BlendAt(10.0f, 10.0f, 0.0f, 0.0f, -1.0e9f, 0.0f));
    // About as much of the ground as asked for, in patches (not none, not all).
    int in = 0;
    int samples = 0;
    for (float z = -400.0f; z < 400.0f; z += 4.0f)
    {
        for (float x = -400.0f; x < 400.0f; x += 4.0f)
        {
            in += Surfaces::BlendAt(x, z, 0.0f, 0.3f, -1.0e9f, 0.0f) ? 1 : 0;
            ++samples;
        }
    }
    const float share = static_cast<float>(in) / static_cast<float>(samples);
    CHECK(share > 0.1f);
    CHECK(share < 0.5f);
    // A beach: at the water's edge always, well up from it never.
    CHECK(Surfaces::BlendAt(5.0f, 5.0f, 0.2f, 0.0f, 0.0f, 1.4f));
    CHECK_FALSE(Surfaces::BlendAt(5.0f, 5.0f, 6.0f, 0.0f, 0.0f, 1.4f));
}

TEST_CASE("Trees grow in woods with clearings, round the site and out to the horizon, never in the water", "[site][scatter]")
{
    const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(3, SitePlan::SiteKind::Facility));
    SiteTerrain terrain;
    terrain.Plan(plan, SiteTerrain::Shape::Rolling, false, SiteTerrain::Water::Coast);
    // Nothing grows on ice; a temperate world has both woods and open ground.
    CHECK(SiteScatter::Woodland(plan.seed, "frozen", 0.0f, 0.0f) == 0.0f);
    int wooded = 0;
    int open = 0;
    for (float z = -1500.0f; z < 1500.0f; z += 20.0f)
    {
        for (float x = -1500.0f; x < 1500.0f; x += 20.0f)
        {
            const float wood = SiteScatter::Woodland(plan.seed, "temperate", x, z);
            wooded += wood > 0.9f ? 1 : 0;
            open += wood < 0.1f ? 1 : 0;
        }
    }
    CHECK(wooded > 500);
    CHECK(open > 500);
    // Trees past the site's own square, not only round the buildings, and none of them standing in the water.
    int outside = 0;
    for (const SiteScatter::Item& item : SiteScatter::Plan(plan.seed, "temperate", plan, terrain))
    {
        if (item.kind != SiteScatter::Kind::Tree)
        {
            continue;
        }
        CHECK(terrain.WaterDepth(item.at.x, item.at.z) < 0.0f);
        const glm::vec3 local = item.at - plan.origin;
        outside += local.x < 0.0f || local.z < 0.0f || local.x > plan.size || local.z > plan.size ? 1 : 0;
    }
    CHECK(outside > 50);
    CHECK_FALSE(SiteScatter::FarTrees(plan.seed, "temperate", plan, terrain, glm::vec3(0.2f, 0.3f, 0.1f)).indices.empty());
    CHECK(SiteScatter::FarTrees(plan.seed, "desert", plan, terrain, glm::vec3(0.2f, 0.3f, 0.1f)).indices.empty());
    // The ground as drawn: the field's on it, and no step where the field gives way to the land round it.
    const glm::vec2 corner = terrain.Min();
    CHECK(terrain.GroundHeight(corner.x + 50.0f, corner.y + 50.0f) == terrain.Height(corner.x + 50.0f, corner.y + 50.0f));
    const float z = corner.y + terrain.Extent() * 0.5f;
    CHECK(std::abs(terrain.GroundHeight(corner.x + 0.5f, z) - terrain.GroundHeight(corner.x - 0.5f, z)) < 1.5f);
}

TEST_CASE("Landmarks stand out on the land, never on the site, each its own shape", "[site][landmark]")
{
    const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(5, SitePlan::SiteKind::Facility));
    SiteTerrain terrain;
    terrain.Plan(plan, SiteTerrain::Shape::Flat, false);
    const std::vector<Landmark::Placed> desert = Landmark::Scatter(plan.seed, "desert", SiteTerrain::Shape::Flat, plan, terrain);
    REQUIRE_FALSE(desert.empty());
    const glm::vec2 middle{plan.origin.x + plan.size * 0.5f, plan.origin.z + plan.size * 0.5f};
    for (const Landmark::Placed& landmark : desert)
    {
        // Out on the land, well past where anybody may go.
        CHECK(glm::distance(glm::vec2(landmark.at.x, landmark.at.z), middle) > plan.size * 0.5f + SiteTerrain::kRoam + 200.0f);
        CHECK(landmark.size > 20.0f);
    }
    // The same from the same seed; no two of a kind the same shape.
    const std::vector<Landmark::Placed> again = Landmark::Scatter(plan.seed, "desert", SiteTerrain::Shape::Flat, plan, terrain);
    REQUIRE(again.size() == desert.size());
    for (size_t i = 0; i < desert.size(); ++i)
    {
        CHECK(again[i].at == desert[i].at);
        for (size_t j = i + 1; j < desert.size(); ++j)
        {
            CHECK(desert[i].seed != desert[j].seed);
        }
    }
    const MeshData shape = Landmark::Shape(desert.front());
    CHECK_FALSE(shape.indices.empty());
}

TEST_CASE("The organism's signs: none where there are none, plainer further out, one rhythm everywhere", "[site][signs]")
{
    const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(7, SitePlan::SiteKind::Facility));
    SiteTerrain terrain;
    terrain.Plan(plan, SiteTerrain::Shape::Rolling, false);
    CHECK(Signs::Make(plan.seed, 0.0f, plan, terrain).empty());
    const std::vector<Signs::Formation> faint = Signs::Make(plan.seed, 0.15f, plan, terrain);
    const std::vector<Signs::Formation> plain = Signs::Make(plan.seed, 1.0f, plan, terrain);
    REQUIRE_FALSE(faint.empty());
    REQUIRE_FALSE(plain.empty());
    CHECK(plain.size() >= faint.size());
    CHECK(plain.front().size > faint.front().size);
    CHECK_FALSE(Signs::Rock(plain.front(), 1.0f, terrain).indices.empty());
    // The rhythm: between nought and one, at its height once in each, and the same at the same moment wherever it is asked.
    float most = 0.0f;
    for (double t = 0.0; t < 47.0; t += 0.05)
    {
        const float pulse = Signs::Pulse(t, 47.0f);
        CHECK(pulse >= 0.0f);
        CHECK(pulse <= 1.0f);
        most = std::max(most, pulse);
    }
    CHECK(most > 0.95f);
    CHECK(Signs::Pulse(12345.6, 47.0f) == Signs::Pulse(12345.6, 47.0f));
}

TEST_CASE("A crater field has craters on the ground anybody walks on, not only far off", "[site][terrain]")
{
    // (Over a few sites: how many craters land on any one site's walkable ground goes up and down.)
    int wild = 0;
    int bowls = 0;
    for (const uint16_t seed : {uint16_t{11}, uint16_t{12}, uint16_t{13}, uint16_t{14}})
    {
        const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(seed, SitePlan::SiteKind::Facility));
        SiteTerrain terrain;
        terrain.Plan(plan, SiteTerrain::Shape::Cratered, false);
        // Over the wild ground of the site (not what was levelled for it): how many places lie a metre or more below the ground
        // five metres round them every way -- the floor of a crater.
        for (float z = plan.origin.z - 30.0f; z < plan.origin.z + plan.size + 30.0f; z += 4.0f)
        {
            for (float x = plan.origin.x - 30.0f; x < plan.origin.x + plan.size + 30.0f; x += 4.0f)
            {
                if (!terrain.OnField(x, z) || terrain.Wildness(x, z) < 0.99f)
                {
                    continue;
                }
                ++wild;
                const float here = terrain.Height(x, z);
                float round = 0.0f;
                for (int k = 0; k < 8; ++k)
                {
                    const float angle = static_cast<float>(k) * 0.785398f;
                    round += terrain.Height(x + std::cos(angle) * 5.0f, z + std::sin(angle) * 5.0f) / 8.0f;
                }
                bowls += round - here > 1.0f ? 1 : 0;
            }
        }
    }
    CAPTURE(wild, bowls);
    REQUIRE(wild > 200);
    CHECK(bowls > wild / 40);
}

TEST_CASE("The sea's waves rise and fall with time, higher on the open sea and in a storm, calm in the shallows", "[site][water]")
{
    const Waves sea = Waves::Raised(9.0f, 0.4f, 0.0f, true, 7u);
    const Waves storm = Waves::Raised(9.0f, 0.4f, 1.0f, true, 7u);
    const Waves lake = Waves::Raised(9.0f, 0.4f, 0.0f, false, 7u);
    CHECK(storm.Highest() > sea.Highest() * 1.5f);
    CHECK(lake.Highest() < 0.5f);
    // Even in a storm, short and steep off a coast, not an ocean's swell: none of its waves over sixty metres long (one that
    // long goes by at near ten metres a second).
    for (int i = 0; i < storm.count; ++i)
    {
        CHECK(storm.waves[static_cast<size_t>(i)].z < 60.0f);
    }
    // Over a minute at one place out at sea, as drawn three metres apart: up and down by a metre or more.
    float lowest = 1.0e9f;
    float highest = -1.0e9f;
    for (int i = 0; i < 120; ++i)
    {
        const float rise = sea.Rise({30.0f, -12.0f}, static_cast<float>(i) * 0.5f, 50.0f, 3.0f);
        lowest = std::min(lowest, rise);
        highest = std::max(highest, rise);
    }
    CAPTURE(lowest, highest, sea.Highest());
    CHECK(highest - lowest > 1.0f);
    // The rise over a place is the moved water's height there: the water moved to it from where it rests.
    const glm::vec2 rest{4.0f, 9.0f};
    const glm::vec3 moved = sea.Moved(rest, 3.0f, 50.0f, 3.0f);
    CHECK(sea.Rise(rest + glm::vec2(moved.x, moved.z), 3.0f, 50.0f, 3.0f) == Catch::Approx(moved.y).margin(0.05f));
    // In water half a metre deep, hardly anything.
    CHECK(std::abs(sea.Rise({30.0f, -12.0f}, 3.0f, 0.5f, 3.0f)) < 0.3f);
}

TEST_CASE("Valleys reach out into the land from every site, with a field generator and a relay mast out in them, in reach", "[site][terrain]")
{
    for (const uint16_t seed : {uint16_t{1}, uint16_t{2}, uint16_t{3}, uint16_t{4}})
    {
        INFO("seed " << seed);
        const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(seed, SitePlan::SiteKind::Facility));
        REQUIRE(plan.valleys.size() >= 2);
        REQUIRE(plan.generator >= 0);
        REQUIRE(plan.relay >= 0);
        CHECK(plan.blocks[static_cast<size_t>(plan.generator)].outlying);
        CHECK(plan.blocks[static_cast<size_t>(plan.relay)].kind == SitePlan::BlockKind::Mast);
        SiteTerrain terrain;
        terrain.Plan(plan, SiteTerrain::Shape::Rolling, false);
        const glm::vec2 middle{plan.origin.x + plan.size * 0.5f, plan.origin.z + plan.size * 0.5f};
        for (const SitePlan::Valley& valley : plan.valleys)
        {
            // Its end well out past the open ground, and everywhere along it in reach.
            CHECK(glm::distance(valley.end, middle) > plan.size * 0.5f + SiteSpec::kRoam + 40.0f);
            for (size_t i = 0; i + 1 < valley.line.size(); ++i)
            {
                for (float t = 0.0f; t <= 1.0f; t += 0.1f)
                {
                    const glm::vec2 at = glm::mix(valley.line[i], valley.line[i + 1], t);
                    CHECK(terrain.PastRoam(at.x, at.y) < 0.0f);
                }
            }
            CHECK(terrain.PastRoam(valley.end.x, valley.end.y) < 0.0f);
        }
        // And out there the ground goes on: much more of it in reach than the band round the square alone.
        int inReach = 0;
        int inBand = 0;
        for (float z = plan.origin.z - 250.0f; z < plan.origin.z + plan.size + 250.0f; z += 5.0f)
        {
            for (float x = plan.origin.x - 250.0f; x < plan.origin.x + plan.size + 250.0f; x += 5.0f)
            {
                const glm::vec2 out = glm::max(glm::abs(glm::vec2(x, z) - middle) - glm::vec2(plan.size * 0.5f), glm::vec2(0.0f));
                inReach += terrain.PastRoam(x, z) < 0.0f && glm::length(out) > SiteSpec::kRoam + 20.0f ? 1 : 0;
                inBand += glm::length(out) <= SiteSpec::kRoam ? 1 : 0;
            }
        }
        CHECK(inReach > 100);
        CHECK(inBand > 0);
    }
}

TEST_CASE("Everything scattered on the ground reaches down to its foot: nothing stands on air", "[site][scatter]")
{
    // Each kind's shapes are about a metre, their foot at nought, Plan sinking them into the ground from there: the lowest of
    // each shape must be at its foot or under it (a boulder's cut-flat underside was a quarter of its height up).
    for (int k = 0; k < static_cast<int>(SiteScatter::Kind::Count); ++k)
    {
        for (int variant = 0; variant < SiteScatter::kVariants; ++variant)
        {
            for (const uint32_t seed : {3u, 77u, 1234u})
            {
                const MeshData mesh = SiteScatter::Shape(static_cast<SiteScatter::Kind>(k), variant, seed);
                if (mesh.vertices.empty())
                {
                    continue;
                }
                float lowest = 1.0e9f;
                for (const MeshVertex& vertex : mesh.vertices)
                {
                    lowest = std::min(lowest, vertex.position.y);
                }
                CAPTURE(k, variant, seed);
                CHECK(lowest <= 0.01f);
            }
        }
    }
}

TEST_CASE("Now and then two of a site's buildings are joined under the ground, by a passage that fits", "[site]")
{
    int joined = 0;
    int sites = 0;
    for (uint16_t seed = 1; seed <= 120; ++seed)
    {
        const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(seed, SitePlan::SiteKind::Facility));
        ++sites;
        for (const SitePlan::Tunnel& tunnel : plan.tunnels)
        {
            INFO("seed " << seed);
            ++joined;
            REQUIRE(tunnel.from >= 0);
            REQUIRE(tunnel.to >= 0);
            REQUIRE(tunnel.cells.size() >= 2);
            const glm::vec2 origin{plan.origin.x, plan.origin.z};
            const auto local = [&](const FacilityLayout& building, glm::ivec2 cell)
            { return cell - glm::ivec2(glm::round((glm::vec2(building.origin.x, building.origin.z) - origin) / FacilityLayout::kCell)); };
            // Each end beside its building's doorway down there; every step to the next cell, side by side.
            for (const auto& [index, end] : {std::pair{tunnel.from, tunnel.cells.front()}, std::pair{tunnel.to, tunnel.cells.back()}})
            {
                const FacilityLayout& building = plan.buildings[static_cast<size_t>(index)];
                REQUIRE(building.ground >= 1);
                REQUIRE(building.tunnels.size() == 1);
                const glm::ivec2 door = local(building, end) - building.tunnels.front().cell;
                CHECK(std::abs(door.x) + std::abs(door.y) == 1);
                CHECK(tunnel.floor == Catch::Approx(building.GroundLevel() - FacilityLayout::kStorey));
            }
            for (size_t i = 1; i < tunnel.cells.size(); ++i)
            {
                const glm::ivec2 step = tunnel.cells[i] - tunnel.cells[i - 1];
                CHECK(std::abs(step.x) + std::abs(step.y) == 1);
            }
            // Under no building, anywhere along it.
            for (const glm::ivec2 cell : tunnel.cells)
            {
                for (const FacilityLayout& building : plan.buildings)
                {
                    const glm::ivec2 at = local(building, cell);
                    CHECK(building.Height(at.x, at.y) == 0);
                }
            }
            // And the ground over it levelled.
            const glm::vec2 middle = origin + (glm::vec2(tunnel.cells[tunnel.cells.size() / 2]) + glm::vec2(0.5f)) * FacilityLayout::kCell;
            CHECK(plan.InBuilding(middle, 0.0f));
        }
    }
    // Sometimes, not often.
    CHECK(joined > 0);
    CHECK(joined < sites / 3);
}

TEST_CASE("A mine is broken into a cave: an adit out of its workings into passages and chambers, open, under nothing built, indoors", "[site]")
{
    int caves = 0;
    int flooded = 0;
    for (uint16_t base = 1; base <= 120 && (caves < 4 || flooded == 0); ++base)
    {
        const uint16_t seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Facility);
        const SitePlan plan = SitePlan::Generate(seed);
        for (const SitePlan::Cave& cave : plan.caves)
        {
            INFO("seed " << seed);
            ++caves;
            // Out of a mine's workings, through its doorway there.
            REQUIRE(cave.building >= 0);
            const FacilityLayout& building = plan.buildings[static_cast<size_t>(cave.building)];
            CHECK(building.workings == 0);
            CHECK_FALSE(building.caves.empty());
            CHECK(cave.passages.size() >= 4);
            // Open along its passages and in its chambers, the way on from the adit's end open; and that counts as indoors.
            for (const SitePlan::Cave::Passage& passage : cave.passages)
            {
                const glm::vec3 middle = (passage.a + passage.b) * 0.5f;
                CHECK(cave.Distance(middle) < 0.0f);
                CHECK(plan.Indoors(middle));
            }
            CHECK(plan.Indoors(cave.mouth + cave.out * (cave.adit * 0.5f) + glm::vec3(0.0f, 1.0f, 0.0f)));
            // Flooded, now and then: in its low ground, never up to the adit's floor.
            if (cave.water > cave.bottom)
            {
                ++flooded;
                CHECK(cave.water < cave.mouth.y - 0.5f);
            }
            // Nowhere near under another building, nor back into its own.
            for (const FacilityLayout& other : plan.buildings)
            {
                glm::vec2 min;
                glm::vec2 max;
                SitePlan::Footprint(other, 0.0f, min, max);
                for (const SitePlan::Cave::Passage& passage : cave.passages)
                {
                    for (const glm::vec3& end : {passage.a, passage.b})
                    {
                        // (Far enough under its lowest floor, a passage may go under it.)
                        CHECK_FALSE((end.x > min.x && end.x < max.x && end.z > min.y && end.z < max.y && end.y + passage.radius > other.origin.y - 1.0f));
                    }
                }
            }
        }
    }
    CHECK(caves >= 2);
    CHECK(flooded >= 1);
}

TEST_CASE("Other buildings are joined to a cave now and then, each by an adit of its own and a way open all along into it", "[site]")
{
    int joins = 0;
    for (uint16_t base = 1; base <= 200 && joins < 4; ++base)
    {
        const uint16_t seed = SitePlan::SeedFor(base, SitePlan::SiteKind::Facility);
        const SitePlan plan = SitePlan::Generate(seed);
        for (const SitePlan::Cave& cave : plan.caves)
        {
            for (const SitePlan::Cave::Adit& adit : cave.joins)
            {
                INFO("seed " << seed << " building " << adit.building);
                ++joins;
                REQUIRE(adit.building >= 0);
                REQUIRE(adit.building != cave.building);
                const FacilityLayout& building = plan.buildings[static_cast<size_t>(adit.building)];
                CHECK_FALSE(building.caves.empty());
                CHECK(building.ground >= 2);
                // Its adit open, and on from its end to the cave: walked along the way it was laid, every step open and indoors.
                for (float a = 0.3f; a < adit.length; a += 0.5f)
                {
                    const glm::vec3 at = adit.mouth + adit.out * a + glm::vec3(0.0f, 1.2f, 0.0f);
                    CHECK(cave.Distance(at) < 0.0f);
                    CHECK(plan.Indoors(at));
                }
            }
            // Every passage open along its middle, a metre over its floor.
            for (const SitePlan::Cave::Passage& passage : cave.passages)
            {
                for (float t = 0.0f; t <= 1.0f; t += 0.1f)
                {
                    const glm::vec3 middle = passage.a + (passage.b - passage.a) * t;
                    CHECK(cave.Distance(middle - glm::vec3(0.0f, passage.radius * 0.55f - 1.0f, 0.0f)) < 0.0f);
                }
            }
        }
    }
    CHECK(joins >= 2);
}

TEST_CASE("What lies in a cave lies on its floor", "[site]")
{
    int checked = 0;
    for (uint16_t base = 1; base <= 120 && checked < 6; ++base)
    {
        const SitePlan plan = SitePlan::Generate(SitePlan::SeedFor(base, SitePlan::SiteKind::Facility));
        for (const SitePlan::Cave& cave : plan.caves)
        {
            for (const glm::vec4& chamber : cave.chambers)
            {
                ++checked;
                const glm::vec3 middle{chamber.x, chamber.y - chamber.w * 0.45f, chamber.z};
                const float floor = cave.FloorUnder(middle);
                // Just over it open, just under it rock.
                CHECK(cave.Distance({middle.x, floor + 0.03f, middle.z}) < 0.0f);
                CHECK(cave.Distance({middle.x, floor - 0.03f, middle.z}) > 0.0f);
                // And flat there: the chamber's own floor, not dipped where the passage into it runs in.
                CHECK(std::abs(floor - middle.y) < 0.12f);
            }
        }
    }
    CHECK(checked >= 6);
}

TEST_CASE("Water standing in a flooded building never lies in one plane with a step's top", "[site][flood]")
{
    // At a metre over its lowest floor -- five steps up exactly -- as a test may ask (site_flood on 1), and with the depths it picks.
    for (const float level : {1.0f, -1.0f})
    {
        INFO("level " << level);
        PhysicsWorld physics;
        PhysicsWorld::Settings settings;
        settings.workerThreads = 2;
        REQUIRE(physics.Init(settings));
        Scene scene;
        MeshLibrary meshes;
        meshes.SetHeadless(true);
        SiteMap site;
        SiteMap::Look look;
        look.biome = "temperate";
        look.weather = "rain";
        look.liquid = SiteMap::Look::Liquid::Water;
        look.water = SiteTerrain::Water::Lakes;
        look.flood = true;
        look.floodLevel = level;
        site.SetLook(look);
        site.Build(3, scene, meshes, physics, nullptr);
        REQUIRE(site.Flooded());
        const float rise = FacilityLayout::kStorey / static_cast<float>(FacilityMap::kSteps);
        bool found = false;
        for (const FacilityLayout& building : site.Plan().buildings)
        {
            const glm::vec3 middle = FacilityMap::ToWorld(building, 0, glm::vec2(static_cast<float>(building.width), static_cast<float>(building.depth)) * 0.5f);
            float surface = 0.0f;
            if (site.Flood(middle.x, middle.z, surface) <= 0.0f)
            {
                continue;
            }
            found = true;
            const float over = surface - building.origin.y;
            CHECK(std::abs(over - std::round(over / rise) * rise) >= 0.02f);
        }
        CHECK(found);
    }
}

TEST_CASE("A building half fallen in is got into through where it broke, and the damage goes on inside it", "[site][ruin]")
{
    // A site with one: its ground floor broken through somewhere, into a room or a corridor -- and a floor fallen through, near it.
    uint16_t seed = 0;
    for (uint16_t s = 1; s <= 30 && seed == 0; ++s)
    {
        for (const FacilityLayout& building : SitePlan::Generate(s).buildings)
        {
            seed = building.ruin.Has() && !building.ruin.fallen.empty() ? s : seed;
        }
    }
    REQUIRE(seed != 0);
    INFO("seed " << seed);
    PhysicsWorld physics;
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    REQUIRE(physics.Init(settings));
    Scene scene;
    MeshLibrary meshes;
    meshes.SetHeadless(true);
    SiteMap site;
    SiteMap::Look look;
    look.biome = "temperate";
    look.weather = "rain";
    site.SetLook(look);
    site.Build(seed, scene, meshes, physics, nullptr);
    NavMesh nav;
    std::string error;
    REQUIRE(nav.Build(physics.StaticTriangles(), NavSettings{}, &error));
    int ways = 0;
    for (const FacilityLayout& building : site.Plan().buildings)
    {
        const FacilityLayout::Ruin& ruin = building.ruin;
        if (!ruin.Has())
        {
            continue;
        }
        const glm::ivec2 steps[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const FacilityLayout::Ruin::Breach& breach : ruin.breaches)
        {
            // Every one into a room or a corridor of what stands, from a cell of the ruin.
            const glm::ivec2 past = breach.cell + steps[breach.side];
            CHECK(ruin.Height(past.x, past.y) > 0);
            const FacilityLayout::Cell behind = building.At(breach.floor, breach.cell.x, breach.cell.y);
            CHECK((behind == FacilityLayout::Cell::Room || behind == FacilityLayout::Cell::Corridor));
            if (breach.floor != building.ground)
            {
                continue;
            }
            // Walked through, from out in the ruin to the middle of the cell behind it.
            const glm::vec3 from = FacilityMap::ToWorld(building, building.ground, glm::vec2(past) + glm::vec2(0.5f) + glm::vec2(steps[breach.side]) * 0.2f) + glm::vec3(0.0f, 0.1f, 0.0f);
            const glm::vec3 to = FacilityMap::ToWorld(building, building.ground, glm::vec2(breach.cell) + glm::vec2(0.5f) - glm::vec2(steps[breach.side]) * 0.15f) + glm::vec3(0.0f, 0.1f, 0.0f);
            std::vector<glm::vec3> corners;
            bool reached = false;
            nav.FindPath(from, to, corners, &reached, nullptr, 0);
            CAPTURE(from.x, from.z, to.x, to.z);
            CHECK(reached);
            if (!corners.empty())
            {
                // Straight through, not round by a door: no further than the two are apart and a little.
                float length = 0.0f;
                glm::vec3 at = from;
                for (const glm::vec3& corner : corners)
                {
                    length += glm::distance(at, corner);
                    at = corner;
                }
                CHECK(length < glm::distance(from, to) + 3.0f);
            }
            ++ways;
        }
        // And everything in it still reached from its way in, the fallen floors and all.
        const std::vector<bool> reach = building.Reachable(true);
        for (const FacilityLayout::Room& room : building.rooms)
        {
            CHECK(reach[building.Index(room.floor, room.min.x, room.min.y)]);
        }
        // Through each wall inside broken through, from the middle of the one cell to the middle of the other, at chest height: open.
        for (const FacilityLayout::Ruin::Broken& wall : ruin.broken)
        {
            const glm::ivec2 other = wall.cell + (wall.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
            const glm::vec3 a = FacilityMap::ToWorld(building, wall.floor, glm::vec2(wall.cell) + glm::vec2(0.5f)) + glm::vec3(0.0f, 1.2f, 0.0f);
            const glm::vec3 b = FacilityMap::ToWorld(building, wall.floor, glm::vec2(other) + glm::vec2(0.5f)) + glm::vec3(0.0f, 1.2f, 0.0f);
            CAPTURE(wall.floor, wall.cell.x, wall.cell.y, wall.side);
            CHECK_FALSE(physics.RayCastStatic(a, glm::normalize(b - a), glm::distance(a, b) * 0.8f).hit);
        }
        // Down through a fallen floor: nothing there but what came down, on the floor under it -- a storey down, a heap's height over it.
        for (const glm::ivec3& hole : ruin.fallen)
        {
            CHECK(building.At(hole.y, hole.x, hole.z) == FacilityLayout::Cell::Void);
            const glm::vec3 over = FacilityMap::ToWorld(building, hole.y, glm::vec2(hole.x, hole.z) + glm::vec2(0.5f)) + glm::vec3(0.0f, 1.5f, 0.0f);
            const RayHit down = physics.RayCastStatic(over, {0.0f, -1.0f, 0.0f}, 6.0f);
            REQUIRE(down.hit);
            const float floor = building.origin.y + static_cast<float>(hole.y) * FacilityLayout::kStorey;
            CHECK(down.position.y < floor - FacilityLayout::kStorey + 1.3f);
            CHECK(down.position.y > floor - FacilityLayout::kStorey - 0.1f);
        }
    }
    CHECK(ways > 0);
}
