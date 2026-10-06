#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Cinematic/Cinematic.h"
#include "Game/World/OutpostStyle.h"
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
// Landed at a hub, where the ground is under the ship (its legs reach down to it).
inline constexpr float kFieldGround = -2.6f;
inline constexpr float kReach = 600.0f;
// Its one deck, the top of its floor; the shuttle bay is twice as tall as the rest.
inline constexpr float kDeck = 0.0f;
// Where the shuttle rests in its bay, on the bay doors, in the ship's own frame.
inline constexpr glm::vec3 kShuttleHome{0.0f, 0.0f, 11.5f};
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

// How the ship's outside looks: its colours, and what its upgrades have put on it. A different look is a new outside.
struct ShipHullLook
{
    glm::vec3 primary{0.78f, 0.78f, 0.76f};
    glm::vec3 secondary{0.16f, 0.17f, 0.18f};
    glm::vec3 accent{0.92f, 0.52f, 0.14f};
    int drive = 0;   // engines: more of them and bigger with each tier
    int sensors = 0; // a mast; then a dish; then arrays
    bool operator==(const ShipHullLook& other) const = default;
};

// Which outpost the ship stands at -- Kestrel Station, or another world's planned from its seed (Outpost) -- and the colours of
// that world's ground and rock.
struct FieldLook
{
    bool kestrel = true;
    uint32_t seed = 0;
    glm::vec3 ground{0.4f};
    glm::vec3 rock{0.3f};
    OutpostStyle style;
    bool operator==(const FieldLook& other) const = default;
};

// The crew's own ship: small, cramped, working, and where everybody is between expeditions.
//
// One deck, laid out along its spine in the ship's own frame with its bow towards -z, a section after another:
//   cockpit       the helm under the windows, two seats, windows ahead and to either side
//   ops room      the navigation table (the console the system map opens at), the two screens on its forward wall,
//                 a galley counter and a bench
//   crew section  a corridor between the bunks to port and the gear room to starboard (the loadout locker, lockers,
//                 ammunition)
//   shuttle bay   twice as tall, the shuttle parked on the bay doors in its floor
//   engine room   aft, the reactor and the machinery
// The hull is built round all of it from its look (ShipHullModel), so a cinematic outside sees the ship everybody is
// standing in: the shuttle drops out through its belly, and the cockpit's windows are windows in it. Upgrades are to
// add sections along the spine; its outside is made again whenever its look changes.
class ShipMap
{
public:
    // Before the site is built: a site that is rebuilt takes away every lamp added after its own.
    void Build(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, LevelLights* lights);
    // Its outside made again for a look (colours, engines, sensors); nothing when it already looks so.
    void SetLook(Scene& scene, MeshLibrary& meshes, const ShipHullLook& look);
    const ShipHullLook& Look() const { return m_look; }
    // The outside for a look, in the ship's frame. The stage's has no rooms inside it, so it is filled: a dark core in every
    // section, the boarding door's hatch shut, and lit glass in the cockpit's windows -- or every gap in it was a hole to the sky.
    static ModelAsset HullModel(const ShipHullLook& look, bool stage = false);
    // The boarding door's hatch, in the ops room's port wall.
    static ModelAsset AirlockModel();
    // Asked open or shut; it slides there (UpdateAirlock), solid only when shut.
    void SetAirlockOpen(Scene& scene, MeshLibrary& meshes, bool open);
    void UpdateAirlock(Scene& scene, MeshLibrary& meshes, float dt);
    bool AirlockOpen() const { return m_airlockOpen; }
    // Where the door's controls are: inside on the wall aft of it, and outside on the hull beside it.
    CinePose AirlockControl(bool inside) const;
    // The landing gear, wanted down or up, and the boarding stair, out or in: round the rooms (standing at an outpost) and on the
    // stage (as a cinematic has it). They fold and slide there (UpdateFittings) -- the first time at once.
    void SetGear(bool rooms, bool stage);
    void SetStair(bool rooms, bool stage);
    // The stage's straight to how they are wanted: a cinematic starting finds its ship as it begins, not unfolding into it.
    void SnapStage();
    // Each leg folding down from its hinge under the belly to stand on, or up against it; the stair swinging down from the
    // boarding door to the ground, or up level and in under the floor. Solid only standing (the stair, all the way out).
    void UpdateFittings(Scene& scene, MeshLibrary& meshes, float dt);
    // A leg (0 to 3: forward to port and starboard, aft to port and starboard), the stair's landing at the door, and its ramp,
    // each about its own hinge, in the ship's frame: the leg hanging down, the ramp level.
    static ModelAsset GearLegModel();
    static ModelAsset StairLandingModel();
    static ModelAsset StairRampModel();
    static glm::vec3 GearHinge(int leg);
    // Whether the ship is standing at an outpost: the outpost round it (Kestrel Station, or another world's), solid to walk on
    // and into, its lamps lit; built again only for another outpost or another world's colours.
    void SetField(Scene& scene, MeshLibrary& meshes, bool shown, const FieldLook& look);
    // The same outpost on the stage, for the cinematics of taking off and setting down (the ship's legs not on it: there the
    // ship is flying).
    void SetStageField(Scene& scene, MeshLibrary& meshes, bool shown, const FieldLook& look);
    bool FieldShown() const { return m_fieldShown; }
    bool Built() const { return m_built; }

    // Whether a point is near enough the ship that the sky round it is space.
    bool Contains(const glm::vec3& point) const;
    bool InHangar(const glm::vec3& point) const;
    // Whether a point is inside the ship's rooms (its own deck, not the station round it), and whether it is in the boarding
    // door's way -- the doorway, or just either side of it, where a hatch shutting would catch somebody.
    static bool Aboard(const glm::vec3& point);
    static bool InDoorway(const glm::vec3& point);
    // Where each player stands arriving aboard -- the briefing room, looking at its screen -- and which way that is.
    glm::vec3 Spawn(uint8_t player) const;
    float SpawnYaw() const;
    // The middle of the loadout locker's screen, in the gear room, turned to face the room.
    CinePose LoadoutLocker() const;
    // Where the navigation console stands (the deployment console, without a campaign), facing where people stand at it.
    CinePose BriefingConsole() const;
    // The middle of the helm's top, in the cockpit, between the seats.
    CinePose Helm() const;
    // Where each of the two screens hangs, facing the room, and how wide it is.
    CinePose BriefingScreen(int index, float& width) const;
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
    // The gear and the stair round the rooms ([0]) and on the stage ([1]): their pieces, how far down or out each is (0 to 1),
    // and how it is wanted; whether the stair round the rooms is solid now.
    VehicleProp m_legs[2][4];
    VehicleProp m_stairLanding[2];
    VehicleProp m_stairRamp[2];
    float m_gearDown[2] = {0.0f, 0.0f};
    bool m_gearWanted[2] = {false, false};
    float m_stairOut[2] = {0.0f, 0.0f};
    bool m_stairWanted[2] = {false, false};
    bool m_stairSolid = false;
    bool m_fittingsSet = false;
    void ShowFittings(Scene& scene, int which);
    static void FarVisible(Scene& scene, VehicleProp& prop);
    PhysicsWorld* m_physics = nullptr;
    LevelLights* m_lights = nullptr;
    uint32_t m_group = 0;
    uint32_t m_fieldGroup = 0;
    VehicleProp m_airlock;
    bool m_airlockOpen = false;
    bool m_airlockSolid = false;
    float m_airlockSlide = 0.0f; // 0 shut, 1 open
    VehicleProp m_fieldDressing;
    VehicleProp m_stageFieldDressing;
    VehicleProp m_field;
    bool m_fieldShown = false;
    VehicleProp m_stageField;
    bool m_stageFieldShown = false;
    FieldLook m_fieldLook;
    FieldLook m_stageFieldLook;
    // The lamps kept ready for whichever outpost it is, round the rooms and round the stage (made with the ship, aimed as each
    // outpost has them: AimFieldLamps).
    std::vector<int> m_fieldLamps;
    std::vector<int> m_stageFieldLamps;
    ShipHullLook m_look;
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
