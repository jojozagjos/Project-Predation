#include "Engine/Navigation/NavMesh.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/Creature.h"
#include "Game/World/FacilityLayout.h"
#include "Game/World/FacilityMap.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <sstream>
#include <string>

using namespace pred;

// Creatures in a built facility: up and down its stairs.
namespace
{

struct Built
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;
    NavMesh nav;
    FacilityMap map;

    explicit Built(uint16_t seed)
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        map.Build(seed, scene, meshes, physics, nullptr);
        REQUIRE(nav.Build(physics.StaticTriangles(), NavSettings{}));
    }
};

// Outside a stairwell's way in, on its floor, or its way out, on the floor above: the middle of the cell just
// past the opening.
glm::vec3 OutsideWell(const FacilityLayout& layout, const FacilityLayout::Stairwell& well, bool high)
{
    const int floor = well.floor + (high ? 1 : 0);
    const auto inWell = [&](glm::ivec2 c) { return c.x >= well.min.x && c.x <= well.max.x && c.y >= well.min.y && c.y <= well.max.y; };
    for (const FacilityLayout::Door& door : layout.doors)
    {
        if (door.floor != floor || door.room >= 0)
        {
            continue;
        }
        const glm::ivec2 next = door.cell + (door.side == 0 ? glm::ivec2(1, 0) : glm::ivec2(0, 1));
        if (inWell(door.cell) != inWell(next))
        {
            const glm::ivec2 outside = inWell(door.cell) ? next : door.cell;
            return FacilityMap::ToWorld(floor, glm::vec2(outside) + glm::vec2(0.5f));
        }
    }
    return glm::vec3(0.0f, -100.0f, 0.0f);
}

CreatureTraits Hunter(uint32_t seed)
{
    CreatureTraits traits = CreatureTraits::FromSeed(seed);
    traits.temperament = Temperament::Predator;
    return traits;
}

} // namespace

TEST_CASE("A creature goes up and down a facility's stairs after a noise without getting stuck", "[creature][facility][stairs]")
{
    for (const uint16_t seed : {1, 7, 12})
    {
        Built built(seed);
        const FacilityLayout& layout = built.map.Layout();
        for (size_t w = 0; w < layout.stairwells.size(); ++w)
        {
            for (const bool up : {true, false})
            {
                for (const uint32_t creatureSeed : {5u, 8883u, 53535u})
                {
                    const FacilityLayout::Stairwell& well = layout.stairwells[w];
                    glm::vec3 from;
                    glm::vec3 to;
                    if (!built.nav.NearestPoint(OutsideWell(layout, well, !up), 1.5f, from) || !built.nav.NearestPoint(OutsideWell(layout, well, up), 1.5f, to))
                    {
                        continue;
                    }
                    INFO("seed " << seed << " well " << w << (up ? " up" : " down") << " creature " << creatureSeed);
                    CreatureTraits traits = Hunter(creatureSeed);
                    // One of them goes about overhead where it can: up the stairwell walls, over the landings.
                    if (creatureSeed == 53535u)
                    {
                        traits.quirks |= 1u << static_cast<unsigned>(Quirk::CeilingDweller);
                    }
                    Creature creature(built.scene, built.meshes, built.physics, &built.nav, traits, from);
                    Noise noise;
                    noise.kind = NoiseKind::Gunshot;
                    noise.position = to;
                    noise.reach = 70.0f;
                    constexpr float dt = 1.0f / 60.0f;
                    float time = 0.0f;
                    bool arrived = false;
                    std::ostringstream trace;
                    for (int tick = 0; tick < 60 * 25 && !arrived; ++tick)
                    {
                        time += dt;
                        CreatureSenses senses;
                        senses.mayBuildNest = false;
                        if (tick == 1)
                        {
                            senses.noises = {noise};
                        }
                        creature.Update(senses, time, dt);
                        const glm::vec3 at = creature.Position();
                        arrived = glm::length(glm::vec2(at.x - to.x, at.z - to.z)) < 3.0f && std::abs(at.y - to.y) < 1.0f;
                        if (tick % 30 == 0)
                        {
                            trace << "\n  " << time << ": " << at.x << "," << at.y << "," << at.z << " " << creature.Brain().CurrentGoal();
                        }
                    }
                    std::vector<glm::vec3> corners;
                    bool reached = false;
                    built.nav.FindPath(from, to, corners, &reached, nullptr, 0);
                    std::ostringstream path;
                    for (const glm::vec3& c : corners)
                    {
                        path << " (" << c.x << "," << c.y << "," << c.z << ")";
                    }
                    INFO("well " << well.min.x << "," << well.min.y << " to " << well.max.x << "," << well.max.y << " alongX " << well.alongX << " rising " << well.rising
                                 << " path reached " << reached << path.str());
                    INFO("from " << from.x << "," << from.y << "," << from.z << " to " << to.x << "," << to.y << "," << to.z << trace.str());
                    CHECK(arrived);
                }
            }
        }
    }
}

TEST_CASE("A creature goes after somebody it can see up or down a facility's stairs, and gets to them", "[creature][facility][stairs]")
{
    for (const uint16_t seed : {1, 7})
    {
        Built built(seed);
        const FacilityLayout& layout = built.map.Layout();
        for (size_t w = 0; w < layout.stairwells.size(); ++w)
        {
            for (const bool up : {true, false})
            {
                for (const uint32_t creatureSeed : {5u, 53535u})
                {
                    const FacilityLayout::Stairwell& well = layout.stairwells[w];
                    glm::vec3 from;
                    glm::vec3 to;
                    if (!built.nav.NearestPoint(OutsideWell(layout, well, !up), 1.5f, from) || !built.nav.NearestPoint(OutsideWell(layout, well, up), 1.5f, to))
                    {
                        continue;
                    }
                    INFO("seed " << seed << " well " << w << (up ? " up" : " down") << " creature " << creatureSeed);
                    CreatureTraits traits = Hunter(creatureSeed);
                    traits.fear = 0.0f;
                    traits.aggression = 1.0f;
                    if (creatureSeed == 53535u)
                    {
                        traits.quirks |= 1u << static_cast<unsigned>(Quirk::CeilingDweller);
                    }
                    Creature creature(built.scene, built.meshes, built.physics, &built.nav, traits, from);
                    SensedPlayer player;
                    player.id = 1;
                    player.name = "Waiting";
                    player.feet = to;
                    constexpr float dt = 1.0f / 60.0f;
                    float time = 0.0f;
                    float closest = 1.0e9f;
                    std::ostringstream trace;
                    for (int tick = 0; tick < 60 * 25 && closest > 2.2f; ++tick)
                    {
                        time += dt;
                        CreatureSenses senses;
                        senses.mayBuildNest = false;
                        senses.players = {player};
                        // Heard first, so it knows where to look.
                        if (tick == 1)
                        {
                            Noise noise;
                            noise.kind = NoiseKind::Gunshot;
                            noise.position = to;
                            noise.reach = 70.0f;
                            noise.player = player.id;
                            senses.noises = {noise};
                        }
                        creature.Update(senses, time, dt);
                        const glm::vec3 at = creature.Position();
                        if (std::abs(at.y - to.y) < 1.2f)
                        {
                            closest = std::min(closest, glm::length(glm::vec2(at.x - to.x, at.z - to.z)));
                        }
                        if (tick % 30 == 0)
                        {
                            trace << "\n  " << time << ": " << at.x << "," << at.y << "," << at.z << " " << creature.Brain().CurrentGoal();
                        }
                    }
                    INFO("from " << from.x << "," << from.y << "," << from.z << " to " << to.x << "," << to.y << "," << to.z << trace.str());
                    CHECK(closest <= 2.2f);
                }
            }
        }
    }
}

namespace
{

// The facility's doors, as the game would move them: shut to begin with, swung open when a creature
// opens one, and broken open by blows when locked. What a creature senses of them each tick.
struct DoorSim
{
    struct State
    {
        WorldObjects::PlacedDoor placed;
        float angle = 0.0f;
        float target = 0.0f;
        int blows = 0;
    };
    std::vector<State> doors;

    explicit DoorSim(const WorldObjects::Placements& placements)
    {
        for (const WorldObjects::PlacedDoor& placed : placements.doors)
        {
            doors.push_back({placed, placed.closedYaw, placed.closedYaw, 0});
        }
    }

    static glm::vec3 Along(float yaw) { return {std::cos(yaw), 0.0f, -std::sin(yaw)}; }

    std::vector<DoorSense> Senses() const
    {
        std::vector<DoorSense> out;
        for (size_t i = 0; i < doors.size(); ++i)
        {
            const State& door = doors[i];
            DoorSense sense;
            sense.index = static_cast<int>(i);
            sense.a = door.placed.hinge;
            sense.b = door.placed.hinge + Along(door.placed.closedYaw) * door.placed.width;
            sense.tip = door.placed.hinge + Along(door.angle) * door.placed.width;
            sense.shut = std::abs(door.angle - door.placed.closedYaw) < 0.35f;
            sense.locked = door.placed.locked && door.blows < 3;
            out.push_back(sense);
        }
        return out;
    }

    void Apply(const CreatureIntent& intent, float dt)
    {
        const auto valid = [&](int i) { return i >= 0 && static_cast<size_t>(i) < doors.size(); };
        if (valid(intent.openDoor) && !(doors[static_cast<size_t>(intent.openDoor)].placed.locked &&
                                        doors[static_cast<size_t>(intent.openDoor)].blows < 3))
        {
            doors[static_cast<size_t>(intent.openDoor)].target = doors[static_cast<size_t>(intent.openDoor)].placed.openYaw;
        }
        if (valid(intent.bashDoor))
        {
            State& door = doors[static_cast<size_t>(intent.bashDoor)];
            if (++door.blows >= 3)
            {
                door.target = door.placed.openYaw;
            }
        }
        if (valid(intent.closeDoor))
        {
            doors[static_cast<size_t>(intent.closeDoor)].target = doors[static_cast<size_t>(intent.closeDoor)].placed.closedYaw;
        }
        for (State& door : doors)
        {
            door.angle += std::clamp(door.target - door.angle, -4.5f * dt, 4.5f * dt);
        }
    }
};

} // namespace

TEST_CASE("A creature gets through a facility's shut doors to a noise in another room", "[creature][facility][door]")
{
    for (const uint16_t seed : {1, 7})
    {
        Built built(seed);
        const FacilityLayout& layout = built.map.Layout();
        int tried = 0;
        for (size_t r = 0; r < layout.rooms.size() && tried < 8; ++r)
        {
            const FacilityLayout::Room& room = layout.rooms[r];
            glm::vec3 to;
            if (!built.nav.NearestPoint(FacilityMap::ToWorld(room.floor, glm::vec2(room.min + room.max + glm::ivec2(1)) * 0.5f), 1.5f, to))
            {
                continue;
            }
            // From the way in, to a room on the same floor a walk away.
            glm::vec3 from;
            REQUIRE(built.nav.NearestPoint(built.map.Spawn(), 1.5f, from));
            if (std::abs(from.y - to.y) > 1.0f || glm::distance(from, to) < 8.0f || glm::distance(from, to) > 30.0f)
            {
                continue;
            }
            ++tried;
            for (const uint32_t creatureSeed : {5u, 53535u})
            {
                INFO("seed " << seed << " room " << r << " creature " << creatureSeed);
                DoorSim doors(built.map.Placements());
                Creature creature(built.scene, built.meshes, built.physics, &built.nav, Hunter(creatureSeed), from);
                Noise noise;
                noise.kind = NoiseKind::Gunshot;
                noise.position = to;
                noise.reach = 70.0f;
                constexpr float dt = 1.0f / 60.0f;
                float time = 0.0f;
                bool arrived = false;
                std::ostringstream trace;
                for (int tick = 0; tick < 60 * 60 && !arrived; ++tick)
                {
                    time += dt;
                    CreatureSenses senses;
                    senses.mayBuildNest = false;
                    senses.doors = doors.Senses();
                    if (tick == 1)
                    {
                        senses.noises = {noise};
                    }
                    creature.Update(senses, time, dt);
                    doors.Apply(creature.Brain().Intent(), dt);
                    const glm::vec3 at = creature.Position();
                    // Near: a sound is placed by ear, not to the step.
                    arrived = glm::length(glm::vec2(at.x - to.x, at.z - to.z)) < 4.5f && std::abs(at.y - to.y) < 1.0f;
                    if (tick % 60 == 0)
                    {
                        trace << "\n  " << time << ": " << at.x << "," << at.y << "," << at.z << " " << creature.Brain().CurrentGoal();
                    }
                }
                INFO("from " << from.x << "," << from.z << " to " << to.x << "," << to.z << trace.str() << "\n mind:" << [&] {
                    std::string mind;
                    for (const CreatureBrain::TimelineEntry& entry : creature.Brain().Timeline())
                    {
                        mind += "\n  " + std::to_string(entry.time).substr(0, 5) + "  " + entry.what;
                    }
                    return mind;
                }());
                CHECK(arrived);
            }
        }
        CHECK(tried > 0);
    }
}
