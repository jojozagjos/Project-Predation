#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Cinematic/Cinematic.h"
#include "Game/World/Vehicles.h"
#include "Game/World/WorldObjects.h"

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pred
{

class MeshLibrary;
class LevelLights;

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
// Under way to a site the ship flies bow first at the planet: dead ahead, a touch low, in the cockpit's windows.
// Not quite a unit vector; normalise it.
inline constexpr glm::vec3 kTravelHeading{0.0f, -0.08f, -1.0f};
// The shuttle's cabin, from its lamp: all its lamp lights, so none of it comes through the roof.
inline constexpr glm::vec3 kShuttleCabinMin{-1.8f, -2.45f, -3.8f};
inline constexpr glm::vec3 kShuttleCabinMax{1.8f, 0.05f, 4.0f};
// That box round a shuttle's lamp where it now is, turned as the shuttle is.
inline void ShuttleCabinBounds(const CinePose& lamp, glm::vec3& min, glm::vec3& max)
{
    min = glm::vec3(1e9f);
    max = glm::vec3(-1e9f);
    for (int corner = 0; corner < 8; ++corner)
    {
        const glm::vec3 local{(corner & 1) ? kShuttleCabinMax.x : kShuttleCabinMin.x, (corner & 2) ? kShuttleCabinMax.y : kShuttleCabinMin.y,
                              (corner & 4) ? kShuttleCabinMax.z : kShuttleCabinMin.z};
        const glm::vec3 at = lamp.position + lamp.rotation * local;
        min = glm::min(min, at);
        max = glm::max(max, at);
    }
}
// Where cinematics fly the ship: its outside again, in open space well away from where everybody is standing in it, so
// it can go off into the distance and come back without leaving anybody's rooms behind in space.
inline constexpr glm::vec3 kStage{-1500.0f, 300.0f, -2600.0f};
inline constexpr float kStageReach = 900.0f;
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
    bool Built() const { return m_built; }

    // Whether a point is near enough the ship that the sky round it is space.
    bool Contains(const glm::vec3& point) const;
    bool InHangar(const glm::vec3& point) const;
    // Where each player stands arriving aboard -- the briefing room, looking at its screen -- and which way that is.
    glm::vec3 Spawn(uint8_t player) const;
    float SpawnYaw() const;
    // The middle of the loadout locker's screen, in the gear room, turned to face the room.
    CinePose LoadoutLocker() const;
    // Where the deployment console stands, facing where people stand at it.
    CinePose BriefingConsole() const;
    // What WorldObjects puts aboard: the lockers and ammunition in the gear room.
    const WorldObjects::Placements& Placements() const { return m_placements; }

    VehicleProp& Shuttle() { return m_shuttle; }
    // The lamp in its cabin: an index into the level's lights, or -1.
    int ShuttleLamp() const { return m_shuttleLamp; }
    const VehicleProp& Shuttle() const { return m_shuttle; }
    VehicleProp& BayDoors() { return m_bayDoors; }
    const VehicleProp& BayDoors() const { return m_bayDoors; }
    // The outside of it, round the rooms; and the same outside on the stage, for cinematics to fly.
    VehicleProp& Hull() { return m_hull; }
    VehicleProp& StageHull() { return m_stageHull; }

    // Only the ship that belongs in the picture taken from `eye`: the one round the rooms, or the one on the stage. Each is
    // in the distance behind the other, so a shot of one showed the other in its background.
    void ShowFor(Scene& scene, const glm::vec3& eye);

    // Under way: specks of dust going past the windows at `speed` metres a second (0, none), which is how it is seen that
    // the ship is moving at all -- the stars are too far to. They stream straight down its length, bow to stern.
    void UpdateDust(Scene& scene, MeshLibrary& meshes, float speed, float dt);

    // The engines' glow: 0 cold, 1 at full burn.
    void SetEngines(Scene& scene, float burn);
    float Engines() const { return m_burn; }

    // What a cinematic measures from aboard: the ship itself, the hangar, the cockpit, the briefing room.
    void Anchors(std::map<std::string, CinePose>& anchors) const;

    static glm::vec3 ToWorld(const glm::vec3& local) { return ShipSpec::kOrigin + local; }
    // The rooms of a deck (0 lower, 1 upper), as rectangles in the ship's frame (x0, z0, x1, z1): its map. The hangar is
    // on both, being as tall as both.
    static std::vector<glm::vec4> DeckPlan(int deck);

private:
    bool m_built = false;
    WorldObjects::Placements m_placements;
    VehicleProp m_shuttle;
    int m_shuttleLamp = -1;
    VehicleProp m_bayDoors;
    VehicleProp m_hull;
    VehicleProp m_stageHull;
    std::vector<Entity> m_entities;
    std::vector<BodyHandle> m_bodies;
    float m_burn = 0.0f;
    // Which ship is being shown: the one round the rooms (false), or the one on the stage.
    bool m_showingStage = true;
    bool m_showSet = false;
    bool m_hullsFar = false;
    struct Speck
    {
        Entity entity;
        glm::vec3 at{0.0f}; // in the ship's frame
    };
    std::vector<Speck> m_dust;
    bool m_dustShown = false;
    uint32_t m_dustSeed = 12345u;
};

} // namespace pred
