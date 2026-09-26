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
