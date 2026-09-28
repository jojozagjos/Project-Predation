#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Cinematic/Cinematic.h"
#include "Game/World/Vehicles.h"
#include "Game/World/WorldObjects.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pred
{

class MeshLibrary;
class LevelLights;
class ItemDatabase;

namespace ShipSpec
{
// Where the ship is in the world: well away from the testing area and the sites, in a space of its own. Wherever the
// eye is within kReach of it, the sky is space.
inline constexpr glm::vec3 kOrigin{-1500.0f, 0.0f, 0.0f};
inline constexpr float kReach = 600.0f;
// Its two decks, the top of each floor; the hangar is as tall as both.
inline constexpr float kLowerDeck = 0.0f;
inline constexpr float kUpperDeck = 3.6f;
// Where the shuttle rests in the hangar, on the bay doors, in the ship's own frame.
inline constexpr glm::vec3 kShuttleHome{0.0f, 0.0f, 20.0f};
} // namespace ShipSpec

// The team's carrier between deployments: where everybody is before and after a mission.
//
// Two decks and a hangar as tall as both, laid out in the ship's own frame with its bow towards -z:
//   lower deck   the hangar aft, with the shuttle parked on the bay doors in its floor; forward of it a corridor with
//                the gear room and the crew quarters to port, the stairs and the mess to starboard
//   upper deck   a gallery looking down into the hangar, the landing at the top of the stairs, the briefing room with
//                the deployment console and its screen, and forward of that the cockpit, its windows on space
// The hull is built round all of it, so a cinematic outside sees the ship everybody is standing in: the shuttle drops
// out through its belly, and the cockpit's windows are windows in it. Built once; nothing in it is used up.
class ShipMap
{
public:
    // Before the site is built: a site that is rebuilt takes away every lamp added after its own.
    void Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights);
    // The kit on the gear room's bench: every item there is, as the testing area has them.
    void LayOutKit(const ItemDatabase& items);
    bool Built() const { return m_built; }

    // Whether a point is near enough the ship that the sky round it is space.
    bool Contains(const glm::vec3& point) const;
    bool InHangar(const glm::vec3& point) const;
    // Where each player stands arriving aboard -- the briefing room, looking at its screen -- and which way that is.
    glm::vec3 Spawn(uint8_t player) const;
    float SpawnYaw() const;
    // Where the deployment console stands, facing where people stand at it.
    CinePose BriefingConsole() const;
    // What WorldObjects puts aboard: the lockers and ammunition in the gear room, and the kit on its bench.
    const WorldObjects::Placements& Placements() const { return m_placements; }

    VehicleProp& Shuttle() { return m_shuttle; }
    const VehicleProp& Shuttle() const { return m_shuttle; }
    VehicleProp& BayDoors() { return m_bayDoors; }
    const VehicleProp& BayDoors() const { return m_bayDoors; }

    // The engines' glow: 0 cold, 1 at full burn.
    void SetEngines(Scene& scene, float burn);
    float Engines() const { return m_burn; }

    // What a cinematic measures from aboard: the ship itself, the hangar, the cockpit, the briefing room.
    void Anchors(std::map<std::string, CinePose>& anchors) const;

    static glm::vec3 ToWorld(const glm::vec3& local) { return ShipSpec::kOrigin + local; }

private:
    bool m_built = false;
    WorldObjects::Placements m_placements;
    VehicleProp m_shuttle;
    VehicleProp m_bayDoors;
    std::vector<Entity> m_engineGlow;
    std::vector<glm::vec3> m_engineGlowColour;
    std::vector<Entity> m_entities;
    std::vector<BodyHandle> m_bodies;
    float m_burn = 0.0f;
};

} // namespace pred
