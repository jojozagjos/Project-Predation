#include "Game/Campaign/Campaign.h"
#include "Game/Campaign/CampaignStore.h"
#include "Game/Campaign/Universe.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <filesystem>
#include <fstream>
#include <set>

using namespace pred;

// The campaign: a universe made from one seed, the same every time, and a save that holds only what the crew has
// done, read back exactly as it was written.

namespace
{

const UniverseData& ShippedData()
{
    static UniverseData data = []
    {
        UniverseData loaded;
        std::string error;
        const bool ok = loaded.LoadFromFile(std::filesystem::path(PRED_SOURCE_DIR) / "Assets" / "Data" / "universe.json", &error);
        INFO(error);
        REQUIRE(ok);
        return loaded;
    }();
    return data;
}

std::filesystem::path ScratchFolder(const char* name)
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "predation_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);
    return folder;
}

} // namespace

TEST_CASE("The universe's data file reads, and every table has something in it", "[campaign]")
{
    const UniverseData& data = ShippedData();
    CHECK_FALSE(data.biomes.empty());
    CHECK_FALSE(data.atmospheres.empty());
    CHECK_FALSE(data.terrains.empty());
    CHECK_FALSE(data.weathers.empty());
    CHECK_FALSE(data.civilizations.empty());
    CHECK_FALSE(data.stars.empty());
    CHECK_FALSE(data.regionKinds.empty());
    // Every biome's lists name entries that exist.
    for (const BiomeDef& biome : data.biomes)
    {
        INFO(biome.id);
        for (const auto& [id, weight] : biome.atmospheres)
        {
            CHECK(data.Atmosphere(id) != nullptr);
        }
        for (const auto& [id, weight] : biome.terrains)
        {
            CHECK(data.Terrain(id) != nullptr);
        }
        for (const auto& [id, weight] : biome.weathers)
        {
            CHECK(data.Weather(id) != nullptr);
        }
    }
    for (const CivilizationDef& civ : data.civilizations)
    {
        for (const auto& [id, weight] : civ.regions)
        {
            INFO(civ.id << " -> " << id);
            CHECK(data.RegionKind(id) != nullptr);
        }
    }
}

TEST_CASE("A system comes out the same from the same seed, and differently from another", "[campaign]")
{
    const UniverseData& data = ShippedData();
    std::set<std::string> names;
    for (uint64_t seed = 1; seed <= 20; ++seed)
    {
        const StarSystem a = Universe::Generate(seed, SystemId{}, data);
        const StarSystem b = Universe::Generate(seed, SystemId{}, data);
        REQUIRE(a.name == b.name);
        REQUIRE(a.bodies.size() == b.bodies.size());
        for (size_t i = 0; i < a.bodies.size(); ++i)
        {
            CHECK(a.bodies[i].name == b.bodies[i].name);
            CHECK(a.bodies[i].biome == b.bodies[i].biome);
            CHECK(a.bodies[i].seed == b.bodies[i].seed);
            CHECK(a.bodies[i].regions.size() == b.bodies[i].regions.size());
        }
        names.insert(a.name);
    }
    // Twenty seeds, nearly twenty names.
    CHECK(names.size() >= 15);
}

TEST_CASE("Home always has a settled world with a shipyard on the charts", "[campaign]")
{
    const UniverseData& data = ShippedData();
    for (uint64_t seed = 1; seed <= 40; ++seed)
    {
        INFO("seed " << seed);
        const StarSystem home = Universe::Generate(seed, SystemId{}, data);
        REQUIRE(home.hub >= 0);
        const Body& world = home.bodies[static_cast<size_t>(home.hub)];
        CHECK(world.kind == BodyKind::Planet);
        CHECK(world.Landable());
        REQUIRE_FALSE(world.regions.empty());
        CHECK(world.regions.front().kind == "service_hub");
        CHECK(world.regions.front().charted);
        CHECK(home.bodies.size() >= 5);
    }
}

TEST_CASE("Every body is something, moons designated as moons, and only solid ones have places to land", "[campaign]")
{
    const UniverseData& data = ShippedData();
    for (uint64_t seed = 1; seed <= 20; ++seed)
    {
        const StarSystem system = Universe::Generate(seed, SystemId{3, 0, -2, 0}, data);
        for (const Body& body : system.bodies)
        {
            INFO(body.name);
            CHECK_FALSE(body.biome.empty());
            CHECK(data.Biome(body.biome) != nullptr);
            if (body.gas)
            {
                CHECK(body.regions.empty());
            }
            if (body.kind == BodyKind::Moon)
            {
                REQUIRE(body.parent >= 0);
                const Body& planet = system.bodies[static_cast<size_t>(body.parent)];
                // A designation of its own ("LV-426"), not its planet's name.
                CHECK(body.name.rfind("LV-", 0) == 0);
                CHECK(planet.kind == BodyKind::Planet);
            }
            for (const LandingRegion& region : body.regions)
            {
                CHECK(region.seed != 0);
                CHECK_FALSE(region.designation.empty());
            }
        }
        // Planets go outwards.
        float last = 0.0f;
        for (const Body& body : system.bodies)
        {
            if (body.kind == BodyKind::Planet)
            {
                CHECK(body.orbit > last);
                last = body.orbit;
            }
        }
    }
}

TEST_CASE("Planets move along their orbits with the clock, and come back round", "[campaign]")
{
    const StarSystem system = Universe::Generate(7, SystemId{}, ShippedData());
    const Body& planet = system.bodies.front();
    const glm::vec3 now = system.Position(planet.index, 0.0);
    const glm::vec3 later = system.Position(planet.index, planet.period * 0.25);
    const glm::vec3 round = system.Position(planet.index, planet.period);
    CHECK(glm::length(now - later) > planet.orbit * 0.5f);
    CHECK(glm::length(now - round) < 1.0e-3f);
    CHECK(std::abs(glm::length(now) - planet.orbit) < planet.orbit * 0.1f);
}

TEST_CASE("The galaxy has systems around home, the same ones every time, home first", "[campaign]")
{
    Universe universe;
    universe.Reset(99, &ShippedData());
    const std::vector<SystemId> near = universe.Near(glm::vec3(0.0f), 40.0f);
    REQUIRE_FALSE(near.empty());
    CHECK(near.front() == SystemId{});
    CHECK(near.size() > 3);
    Universe again;
    again.Reset(99, &ShippedData());
    CHECK(again.Near(glm::vec3(0.0f), 40.0f) == near);
    for (const SystemId id : near)
    {
        CHECK(SystemId::Unpack(id.Packed()) == id);
        CHECK(universe.System(id) != nullptr);
    }
    // A cell beyond its systems has none to give.
    CHECK(universe.System(SystemId{0, 0, 0, 5}) == nullptr);
}

TEST_CASE("A campaign begins at the shipyard with home's records known", "[campaign]")
{
    Universe universe;
    universe.Reset(1234, &ShippedData());
    const CampaignState state = CampaignState::Begin("First", 1234, universe);
    const StarSystem* home = universe.System(universe.Home());
    REQUIRE(home != nullptr);
    CHECK(state.body == home->hub);
    CHECK(state.region == -1);
    CHECK(state.credits > 0);
    CHECK((state.Known(home->id.Packed(), home->hub) & CampaignState::kKnownVisited) != 0);
}

TEST_CASE("A campaign written out reads back the same, keeping what it does not know", "[campaign]")
{
    Universe universe;
    universe.Reset(42, &ShippedData());
    CampaignState state = CampaignState::Begin("Round trip", 42, universe);
    state.credits = 12345;
    state.components["field_assembly"] = 2;
    state.upgrades["travel"] = 3;
    state.colors.accent = {0.1f, 0.2f, 0.3f};
    state.travel.underway = true;
    state.travel.target = 4;
    state.travel.position = {1.5f, 0.0f, -2.0f};
    state.Learn(state.system, 3, CampaignState::kKnownScanned);
    state.FindRegion(state.system, 3, 2);
    state.regionChanges[CampaignState::RegionKey(state.system, 3, 2)] = {{"doors", {1, 4}}};
    CampaignState::LogEntry& entry = state.AddLog("planet", CampaignState::BodyKey(state.system, 3), "Scanned", "Seen from orbit.");
    entry.important = true;
    state.story["met_somebody"] = true;
    state.cargo["salvage_plate"] = 5;
    state.extra["fromTheFuture"] = {{"thing", 7}};

    const nlohmann::json json = state.ToJson();
    CampaignState back;
    REQUIRE(CampaignState::FromJson(json, back));
    CHECK(back.name == "Round trip");
    CHECK(back.universeSeed == 42);
    CHECK(back.credits == 12345);
    CHECK(back.Components("field_assembly") == 2);
    CHECK(back.Upgrade("travel") == 3);
    CHECK(back.colors.accent.z == 0.3f);
    CHECK(back.travel.underway);
    CHECK(back.travel.target == 4);
    CHECK(back.Known(state.system, 3) == state.Known(state.system, 3));
    CHECK((back.Known(state.system, 3) & CampaignState::kKnownScanned) != 0);
    CHECK(back.RegionFound(state.system, 3, 2, false));
    CHECK_FALSE(back.RegionFound(state.system, 3, 1, false));
    CHECK(back.regionChanges.size() == 1);
    REQUIRE(back.log.size() == 1);
    CHECK(back.log.front().important);
    CHECK(back.log.front().title == "Scanned");
    CHECK(back.story["met_somebody"] == true);
    CHECK(back.cargo["salvage_plate"] == 5);
    CHECK(back.extra["fromTheFuture"]["thing"] == 7);
    // And out again, the same.
    CHECK(back.ToJson() == json);
}

TEST_CASE("The same log entry is not added twice", "[campaign]")
{
    CampaignState state;
    const uint32_t first = state.AddLog("planet", "0:1", "Found", "").id;
    const uint32_t again = state.AddLog("planet", "0:1", "Found", "").id;
    CHECK(first == again);
    CHECK(state.log.size() == 1);
    CHECK(state.AddLog("planet", "0:2", "Found", "").id != first);
}

TEST_CASE("The store saves a campaign, lists it, loads the newest, and survives a damaged file", "[campaign]")
{
    CampaignStore store(ScratchFolder("campaign_store"));
    Universe universe;
    universe.Reset(5, &ShippedData());
    CampaignState state = CampaignState::Begin("Stored", 5, universe);
    const std::string folder = store.NewFolder(state.name);
    CHECK(folder == "stored");

    state.credits = 100;
    store.Save(folder, state, false);
    CHECK(store.Finish().empty());
    state.credits = 200;
    store.Save(folder, state, true);
    CHECK(store.Finish().empty());

    const std::vector<CampaignSlot> slots = store.List();
    REQUIRE(slots.size() == 1);
    CHECK(slots.front().name == "Stored");
    CHECK(slots.front().folder == folder);

    CampaignState loaded;
    REQUIRE(store.Load(folder, loaded));
    // Saved within the same second, either may be "newest"; both are good.
    CHECK((loaded.credits == 100 || loaded.credits == 200));

    // The hand-saved file ruined: the autosave is still there.
    {
        std::ofstream ruin(store.FileFor(folder, false), std::ios::trunc);
        ruin << "{ not json";
    }
    REQUIRE(store.Load(folder, loaded));
    CHECK(loaded.credits == 200);

    // Both ruined: the backups (the hand save's, from before it was replaced, is absent; the autosave has none either
    // since it was written once) -- so nothing loads, and it says so.
    {
        std::ofstream ruin(store.FileFor(folder, true), std::ios::trunc);
        ruin << "";
    }
    std::string error;
    CHECK_FALSE(store.Load(folder, loaded, &error));
    CHECK_FALSE(error.empty());

    CHECK(store.NewFolder("Stored") == "stored_2");
    CHECK(store.Delete(folder));
    CHECK(store.List().empty());
    CHECK_FALSE(store.Delete("../somewhere"));
}

TEST_CASE("Saving over a save keeps the one before as a backup", "[campaign]")
{
    const std::filesystem::path folder = ScratchFolder("safe_write");
    const std::filesystem::path file = folder / "thing.json";
    CHECK(WriteFileSafely(file, "first").empty());
    CHECK(WriteFileSafely(file, "second").empty());
    std::ifstream now(file);
    std::string text;
    now >> text;
    CHECK(text == "second");
    std::ifstream before(file.string() + ".bak");
    before >> text;
    CHECK(text == "first");
}

#include "Game/Campaign/SystemMap.h"
#include "Game/Campaign/Travel.h"

TEST_CASE("The ship flies to a moving planet and arrives at it, sooner with a better drive", "[campaign][travel]")
{
    Universe universe;
    universe.Reset(321, &ShippedData());
    const StarSystem& system = *universe.System(universe.Home());
    for (const int tier : {0, 2})
    {
        CampaignState campaign = CampaignState::Begin("Flight", 321, universe);
        // To the planet furthest from where it starts, of the first four.
        int target = -1;
        float farthest = 0.0f;
        for (const Body& body : system.bodies)
        {
            if (body.kind == BodyKind::Planet && body.index != campaign.body && body.index < 8)
            {
                const float distance = glm::length(system.Position(body.index, 0.0) - system.Position(campaign.body, 0.0));
                if (distance > farthest)
                {
                    farthest = distance;
                    target = body.index;
                }
            }
        }
        REQUIRE(target >= 0);
        REQUIRE(Travel::SetCourse(campaign, system, target));
        CHECK(campaign.travel.underway);
        CHECK(campaign.body == -1);
        const float expected = Travel::Seconds(farthest, tier);
        float flown = 0.0f;
        bool arrived = false;
        while (flown < expected * 4.0f && !arrived)
        {
            campaign.clock += 0.05;
            flown += 0.05f;
            arrived = Travel::Step(campaign, system, 0.05f, tier);
        }
        INFO("tier " << tier << ": expected about " << expected << " s, flew " << flown << " s");
        REQUIRE(arrived);
        CHECK(campaign.body == target);
        CHECK_FALSE(campaign.travel.underway);
        // Not instant, and not wildly longer than the estimate though the planet moved.
        CHECK(flown > expected * 0.5f);
        CHECK(flown < expected * 2.0f);
    }
    CHECK(Travel::Seconds(1.0f, 3) < Travel::Seconds(1.0f, 0) * 0.5f);
    CHECK(Travel::Seconds(4.0f, 0) < Travel::Seconds(1.0f, 0) * 2.5f);
}

TEST_CASE("A new destination part way is simply steered for, and no destination is coming to rest", "[campaign][travel]")
{
    Universe universe;
    universe.Reset(321, &ShippedData());
    const StarSystem& system = *universe.System(universe.Home());
    CampaignState campaign = CampaignState::Begin("Redirect", 321, universe);
    const int first = campaign.body == 0 ? 1 : 0;
    const int second = campaign.body == 2 ? 3 : 2;
    REQUIRE(Travel::SetCourse(campaign, system, first));
    for (int i = 0; i < 400; ++i)
    {
        campaign.clock += 0.05;
        Travel::Step(campaign, system, 0.05f, 0);
    }
    REQUIRE(campaign.travel.underway);
    REQUIRE(Travel::SetCourse(campaign, system, second));
    bool arrived = false;
    for (int i = 0; i < 20000 && !arrived; ++i)
    {
        campaign.clock += 0.05;
        arrived = Travel::Step(campaign, system, 0.05f, 0);
    }
    CHECK(arrived);
    CHECK(campaign.body == second);

    // Off again, then stopped: it comes to rest between the planets.
    REQUIRE(Travel::SetCourse(campaign, system, first));
    for (int i = 0; i < 200; ++i)
    {
        campaign.clock += 0.05;
        Travel::Step(campaign, system, 0.05f, 0);
    }
    REQUIRE(Travel::SetCourse(campaign, system, -1));
    bool stopped = false;
    for (int i = 0; i < 20000 && !stopped; ++i)
    {
        campaign.clock += 0.05;
        stopped = Travel::Step(campaign, system, 0.05f, 0);
    }
    CHECK(stopped);
    CHECK(campaign.body == -1);
    CHECK_FALSE(campaign.travel.underway);
    // Nowhere to go from where it is already.
    CHECK_FALSE(Travel::SetCourse(campaign, system, -1));
}

TEST_CASE("The map draws planets outwards in order, moons by their planets, and picks what is under the pointer", "[campaign][map]")
{
    const StarSystem system = Universe::Generate(55, SystemId{}, ShippedData());
    const auto drawn = SystemMapView::Layout(system, 0.0);
    REQUIRE(drawn.size() == system.bodies.size());
    float last = 0.0f;
    for (const Body& body : system.bodies)
    {
        const SystemMapView::Drawn& at = drawn[body.index];
        if (body.kind == BodyKind::Planet)
        {
            const float out = glm::length(at.at);
            CHECK(out > last);
            last = out;
        }
        else
        {
            const SystemMapView::Drawn& planet = drawn[static_cast<size_t>(body.parent)];
            // Clear of its planet, and nearer it than any other planet is.
            CHECK(glm::length(at.at - planet.at) > planet.radius + at.radius);
            CHECK(glm::length(at.at - planet.at) < 4.0f);
        }
    }
    // Looking straight at a planet from close by, the middle of the picture is that planet.
    SystemMapView view;
    const SystemMapView::Drawn& planet = drawn[0];
    view.Focus(planet.at, 4.0f);
    for (int i = 0; i < 200; ++i)
    {
        view.Update(0.05f);
    }
    CHECK(view.Pick({0.5f, 0.5f}, drawn, 16.0f / 9.0f, false) == planet.index);
    CHECK(view.Pick({0.02f, 0.02f}, drawn, 16.0f / 9.0f, false) != planet.index);
}
